#include "generic_expander.hpp"
#include "../parser/ast_visitor.hpp"
#include "diagnosed_registry.hpp"
#include "generic_expander_detail.hpp"
#include "constant_folding.hpp"
#include "../lexer/lexer.hpp"
#include "../pal/platform.hpp"
#include "../parser/parser.hpp"
#include <iostream>
#include <algorithm>
#include <cstdlib>
#include <cstdio>
#include <functional>
#include <map>
#include <set>

using namespace gallt::AST;

namespace gallt {
    using namespace generic_expander_detail;

    class EmitGeneratedValidator : public AST::AstVisitor {
    public:
        void set_root(const Node* node) { root_ = node; }
        const Node* violation() const { return violation_; }
        ErrorCode error_code() const { return error_code_; }

    protected:
        bool EnterGuideStatement(const GuideStatement* node) override {
            return reject(node, ErrorCode::EmitBlockNotExpandable);
        }

        bool EnterClibStatement(const ClibStatement* node) override {
            return reject(node, ErrorCode::EmitBlockNotExpandable);
        }

        bool EnterGenericDefinition(const GenericDefinition* node) override {
            return reject(node, ErrorCode::EmitBlockNotExpandable);
        }

        bool EnterInstantiationStatement(const InstantiationStatement* node) override {
            return reject(node, ErrorCode::EmitBlockNotExpandable);
        }

        bool EnterCondDefinition(const CondDefinition* node) override {
            return reject(node, ErrorCode::CompileTimeConditionOutsideGenericBlock);
        }

        bool EnterUncondDefinition(const UncondDefinition* node) override {
            return reject(node, ErrorCode::CompileTimeConditionOutsideGenericBlock);
        }

        bool EnterConditionalBlock(const ConditionalBlock* node) override {
            return reject(node, ErrorCode::CompileTimeConditionOutsideGenericBlock);
        }

        bool EnterIfStatement(const IfStatement* node) override {
            if (node->is_compile_time) {
                return reject(node, ErrorCode::CompileTimeConditionOutsideGenericBlock);
            }

            return true;
        }

        bool EnterFunctionDefinition(const FunctionDefinition* node) override {
            if (node->name == "main") {
                return reject(node, ErrorCode::EmitBlockNotExpandable);
            }

            return true;
        }

        bool EnterPrimaryExpression(const PrimaryExpression* node) override {
            if (node->generic_ref != nullptr) {
                return reject(node, ErrorCode::EmitBlockNotExpandable);
            }

            return true;
        }

        bool EnterType(const Type& type) override {
            if (type.generic_ref != nullptr) {
                return reject(root_, ErrorCode::EmitBlockNotExpandable);
            }

            return true;
        }

    private:
        bool reject(const Node* node, ErrorCode code) {
            if (violation_ == nullptr) {
                violation_ = node != nullptr ? node : root_;
                error_code_ = code;
            }

            stop();
            return false;
        }

        const Node* violation_ = nullptr;
        const Node* root_ = nullptr;
        ErrorCode error_code_ = ErrorCode::EmitBlockNotExpandable;
    };

    GenericExpander::GenericExpander(DiagnosticEngine& diag, NamespaceLowering& namespaces,
        std::optional<std::uint64_t> instantiation_depth,
        std::optional<std::uint64_t> constexpr_max_steps,
        std::optional<std::uint64_t> constexpr_max_integer_exponent)
        : diag_(diag)
        , namespaces_(namespaces) {
        if (instantiation_depth.has_value() && *instantiation_depth > 0) {
            instantiation_depth_limit_ =
                static_cast<std::size_t>(*instantiation_depth);
        }

        if (constexpr_max_steps.has_value() && *constexpr_max_steps > 0) {
            constexpr_max_steps_ =
                static_cast<std::size_t>(*constexpr_max_steps);
        }

        if (constexpr_max_integer_exponent.has_value() &&
            *constexpr_max_integer_exponent > 0) {
            constexpr_max_integer_exponent_ =
                *constexpr_max_integer_exponent;
        }
    }

    GenericExpander::InstantiationDepthGuard::InstantiationDepthGuard(
        GenericExpander& pass, SourceLocation loc)
        : pass_(pass), ok_(pass.enter_instantiation(loc)) {}

    GenericExpander::InstantiationDepthGuard::~InstantiationDepthGuard() {
        pass_.leave_instantiation();
    }

    bool GenericExpander::enter_instantiation(SourceLocation loc) {
        ++instantiation_depth_;

        if (stack_probe_triggered_) { return false; }

        if (instantiation_depth_ > instantiation_depth_limit_) {
            if (!instantiation_depth_reported_) {
                instantiation_depth_reported_ = true;
                report(loc, ErrorCode::InstantiationDepthTooDeep,
                    std::vector<std::string>{
                        std::to_string(instantiation_depth_limit_),
                        std::to_string(instantiation_depth_) });
            }

            return false;
        }

        if (!pal::stack_headroom_available(instantiation_stack_reserve())) {
            stack_probe_triggered_ = true;
            std::cerr
                << "sgc: generics instantiation depth is too deep, total recursion depth '"
                << instantiation_depth_ << "'\n";
            return false;
        }

        return true;
    }

    std::size_t GenericExpander::instantiation_stack_reserve() const {
        const std::size_t total = pal::stack_total_bytes();

        if (total == pal::kStackHeadroomUnknown) {
            return kInstantiationStackDefaultReserve;
        }

        const std::size_t scaled = total / kInstantiationStackReserveDivisor;

        return scaled < kInstantiationStackMinimumReserve
            ? kInstantiationStackMinimumReserve : scaled;
    }

    void GenericExpander::leave_instantiation() {
        if (instantiation_depth_ > 0) { --instantiation_depth_; }
    }

    GenericExpander::PoisonGuard::PoisonGuard(GenericExpander& pass,
        const AST::Node* node)
        : pass_(pass), node_(node), errors_(pass.diag_.error_count()) {}

    GenericExpander::PoisonGuard::~PoisonGuard() {
        if (node_ != nullptr && pass_.diag_.error_count() != errors_) {
            pass_.poison_node(node_);
        }
    }

    void GenericExpander::report(SourceLocation loc, ErrorCode code,
        const std::vector<std::string>& values) {
        if (!diagnosed_registry::reported_by_earlier_stage(diag_, loc,
            SemanticStage::Generics)) {
            diag_.report_error_template(loc, code, values);
        }

        diagnosed_registry::record(diag_, loc, SemanticStage::Generics);
        had_error_ = true;
    }

    void GenericExpander::report(SourceLocation loc, ErrorCode code, const std::string& message) {
        if (!diagnosed_registry::reported_by_earlier_stage(diag_, loc,
            SemanticStage::Generics)) {
            diag_.report_error(loc, code, message);
        }

        diagnosed_registry::record(diag_, loc, SemanticStage::Generics);
        had_error_ = true;
    }

    void GenericExpander::push_scope() {
        declared_scopes_.emplace_back();
        short_scopes_.emplace_back();
        instance_scopes_.emplace_back();
    }

    void GenericExpander::pop_scope() {
        declared_scopes_.pop_back();
        short_scopes_.pop_back();
        instance_scopes_.pop_back();
    }

    void GenericExpander::declare_name(const std::string& name) {
        if (!declared_scopes_.empty()) {
            declared_scopes_.back().insert(name);
        }
    }

    bool GenericExpander::name_declared_in_current_scope(const std::string& name) const {
        if (declared_scopes_.empty()) { return false; }
        return declared_scopes_.back().count(name) != 0;
    }

    void GenericExpander::register_short_name(const std::string& name,
        const ShortBinding& binding, const std::string& owning_generic, SourceLocation loc) {
        if (short_scopes_.empty()) { return; }
        auto& shorts = short_scopes_.back();
        auto it = shorts.find(name);

        if (it != shorts.end()) {
            if (it->second.instance == binding.instance) {
                return;
            }

            if (it->second.target != binding.target ||
                it->second.is_type != binding.is_type) {
                it->second.ambiguous = true;
            }
            return;
        }

        if (name == owning_generic) {
            shorts[name] = binding;
            return;
        }

        if (name_declared_in_current_scope(name)) {
            report(loc, ErrorCode::GenericShortNameConflict, std::vector<std::string>{ name });
            return;
        }
        shorts[name] = binding;
        declare_name(name);
    }

    void GenericExpander::add_top_level(std::unique_ptr<TopLevel> node) {
        if (output_ != nullptr) {
            output_->push_back(std::move(node));
        } else {
            program_->top_levels.push_back(std::move(node));
        }
    }

    void GenericExpander::collect_generics() {
        std::map<std::string, std::vector<GenericDefinition*>> by_name;

        for (auto& top : program_->top_levels) {
            if (auto* g = dynamic_cast<GenericDefinition*>(top.get())) {
                by_name[g->name].push_back(g);
            }
        }

        for (auto& pair : by_name) {
            const std::string& name = pair.first;
            std::vector<GenericDefinition*>& defs = pair.second;
            GenericEntry entry;
            std::vector<GenericDefinition*> before_primary;
            bool has_primary_shaped = false;

            for (GenericDefinition* def : defs) {
                if (def->primary_shaped) { has_primary_shaped = true; }
            }

            for (GenericDefinition* def : defs) {
                std::set<std::string> seen_params;

                for (const GenericParameter& p : def->parameters) {
                    if (!seen_params.insert(p.name).second) {
                        report(def->location, ErrorCode::GenericParameterRedefined, std::vector<std::string>{ p.name });
                    }
                }

                for (const GenericParameter& p : def->parameters) {
                    if (p.is_type && p.constraint.kind == GenericConstraint::Kind::Types) {

                        for (const Type& t : p.constraint.types) {
                            if (t.kind != TypeKind::Struct) { continue; }
                            if (t.generic_ref) { continue; }
                            if (!is_builtin_type_name(t.struct_name) &&
                                struct_defs_.find(t.struct_name) == struct_defs_.end()) {
                                report(def->location, ErrorCode::GenericConstraintInvalid, std::vector<std::string>{ p.constraint.to_string() });
                            }
                        }
                    }
                }

                if (!def->primary_shaped) {
                    std::map<std::string, std::string> structure;
                    std::function<std::string(const Type&)> structure_of =
                        [&](const Type& t) -> std::string {
                        switch (t.kind) {
                        case TypeKind::Struct:
                            if (!is_builtin_type_name(t.struct_name) &&
                                struct_defs_.find(t.struct_name) == struct_defs_.end()) {
                                return "T";
                            }
                            return "S:" + t.struct_name;
                        case TypeKind::Pointer:
                            return "P(" + (t.pointee_type ? structure_of(*t.pointee_type)
                                                          : std::string("?")) + ")";
                        case TypeKind::Array:
                            return "A(" + (t.element_type ? structure_of(*t.element_type)
                                                          : std::string("?")) + ")";
                        default:
                            return t.to_string();
                        }
                    };
                    std::function<void(const Type&)> scan = [&](const Type& t) {
                        switch (t.kind) {
                        case TypeKind::Struct:
                            if (!is_builtin_type_name(t.struct_name) &&
                                struct_defs_.find(t.struct_name) == struct_defs_.end()) {
                                auto found = structure.find(t.struct_name);
                                if (found == structure.end()) {
                                    structure[t.struct_name] = "T";
                                }
                            }
                            return;
                        case TypeKind::Pointer:
                            if (t.pointee_type) { scan(*t.pointee_type); }
                            return;
                        case TypeKind::Array:
                            if (t.element_type) { scan(*t.element_type); }
                            return;
                        default:
                            return;
                        }
                    };

                    for (const GenericPatternArg& pattern : def->patterns) {
                        if (!pattern.is_constant) { scan(pattern.type); }
                    }
                    std::map<std::string, std::string> full_structure;
                    std::function<void(const Type&)> verify = [&](const Type& t) {
                        if (t.kind == TypeKind::Struct &&
                            !is_builtin_type_name(t.struct_name) &&
                            struct_defs_.find(t.struct_name) == struct_defs_.end()) {
                            std::string shape = structure_of(t);
                            auto found = full_structure.find(t.struct_name);
                            if (found == full_structure.end()) {
                                full_structure[t.struct_name] = shape;
                            } else if (found->second != shape) {
                                report(def->location,
                                    ErrorCode::GenericFreeIdentifierConflict, std::vector<std::string>{ t.struct_name });
                            }
                            return;
                        }
                        if (t.kind == TypeKind::Pointer && t.pointee_type) {
                            if (t.pointee_type->kind == TypeKind::Struct &&
                                !is_builtin_type_name(t.pointee_type->struct_name) &&
                                struct_defs_.find(t.pointee_type->struct_name) == struct_defs_.end()) {
                                const std::string& n = t.pointee_type->struct_name;
                                auto found = full_structure.find(n);
                                if (found == full_structure.end()) {
                                    full_structure[n] = "P(T)";
                                } else if (found->second != "P(T)") {
                                    report(def->location,
                                        ErrorCode::GenericFreeIdentifierConflict, std::vector<std::string>{ n });
                                }
                                return;
                            }
                            verify(*t.pointee_type);
                            return;
                        }
                        if (t.kind == TypeKind::Array && t.element_type) {
                            if (t.element_type->kind == TypeKind::Struct &&
                                !is_builtin_type_name(t.element_type->struct_name) &&
                                struct_defs_.find(t.element_type->struct_name) == struct_defs_.end()) {
                                const std::string& n = t.element_type->struct_name;
                                auto found = full_structure.find(n);
                                if (found == full_structure.end()) {
                                    full_structure[n] = "A(T)";
                                } else if (found->second != "A(T)") {
                                    report(def->location,
                                        ErrorCode::GenericFreeIdentifierConflict, std::vector<std::string>{ n });
                                }
                                return;
                            }
                            verify(*t.element_type);
                            return;
                        }
                    };

                    for (const GenericPatternArg& pattern : def->patterns) {
                        if (!pattern.is_constant) { verify(pattern.type); }
                    }
                }

                std::set<std::string> struct_member_names;
                std::set<std::string> function_member_names;
                std::set<std::string> value_member_names;
                std::set<std::string> function_signatures;

                for (auto& member : def->members) {
                    std::string member_name;

                    if (auto* sd = dynamic_cast<StructDefinition*>(member.get())) {
                        member_name = sd->name;
                        if (!struct_member_names.insert(member_name).second ||
                            function_member_names.count(member_name) != 0) {
                            report(def->location, ErrorCode::RedefinedFunction,
                                std::vector<std::string>{ member_name });
                        } else if (value_member_names.count(member_name) != 0) {
                            report(def->location, ErrorCode::RedefinedIdentifier,
                                std::vector<std::string>{ member_name });
                        }
                    } else if (auto* fd = dynamic_cast<FunctionDefinition*>(member.get())) {
                        member_name = fd->name;
                        std::string signature = member_name + "(";
                        for (const Type& p : fd->parameters) {
                            signature += p.to_string();
                            signature += ",";
                        }
                        signature += ")";
                        function_member_names.insert(member_name);
                        if (struct_member_names.count(member_name) != 0 ||
                            !function_signatures.insert(signature).second) {
                            report(def->location, ErrorCode::RedefinedFunction,
                                std::vector<std::string>{ member_name });
                        } else if (value_member_names.count(member_name) != 0) {
                            report(def->location, ErrorCode::RedefinedIdentifier,
                                std::vector<std::string>{ member_name });
                        }
                    } else if (auto* vd = dynamic_cast<VariableDeclaration*>(member.get())) {
                        member_name = vd->name;
                        if (struct_member_names.count(member_name) != 0 ||
                            function_member_names.count(member_name) != 0 ||
                            !value_member_names.insert(member_name).second) {
                            report(def->location, ErrorCode::RedefinedIdentifier,
                                std::vector<std::string>{ member_name });
                        }
                    }

                    if (member_name.empty()) { continue; }

                    for (const GenericParameter& p : def->parameters) {
                        if (p.name == member_name) {
                            report(def->location, ErrorCode::GenericMemberNameConflictsParameter, std::vector<std::string>{ member_name, p.name });
                        }
                    }
                }

                if (def->primary_shaped && entry.primary == nullptr) {
                    entry.primary = def;
                } else if (def->primary_shaped) {
                    bool same_shape = false;
                    if (entry.primary != nullptr) {
                        std::vector<GenericPatternArg> primary_patterns =
                            entry.primary->patterns.empty()
                            ? parameters_as_free_patterns(entry.primary)
                            : entry.primary->patterns;
                        std::vector<GenericPatternArg> def_patterns = def->patterns;
                        if (def_patterns.empty() && !def->parameters.empty()) {
                            def_patterns = parameters_as_free_patterns(def);
                        }
                        if (!primary_patterns.empty() &&
                            primary_patterns.size() == def_patterns.size()) {
                            std::function<std::string(const GenericPatternArg&)>
                                raw_signature = [&](const GenericPatternArg& pattern) -> std::string {
                                const Type& t = pattern.type;
                                if (pattern.is_constant) {
                                    return "C=" + pattern.text;
                                }
                                if (t.kind == TypeKind::Struct) {
                                    return t.struct_name;
                                }
                                if (t.kind == TypeKind::Pointer) {
                                    return (t.pointee_type ? raw_signature(GenericPatternArg{ false, false, *t.pointee_type })
                                                           : std::string("?")) + "*";
                                }
                                if (t.kind == TypeKind::Array) {
                                    return (t.element_type ? raw_signature(GenericPatternArg{ false, false, *t.element_type })
                                                           : std::string("?")) + "[]";
                                }
                                return t.to_string();
                            };
                            same_shape = true;
                            for (std::size_t i = 0; i < def_patterns.size(); ++i) {
                                if (raw_signature(primary_patterns[i]) !=
                                    raw_signature(def_patterns[i])) {
                                    same_shape = false;
                                    break;
                                }
                            }
                        }
                    }
                    if (same_shape) {
                        report(def->location, ErrorCode::GenericPrimaryRedefined,
                            std::vector<std::string>{ name });
                        continue;
                    }
                    def->is_specialization = true;
                    if (def->patterns.empty()) {
                        def->patterns = parameters_as_free_patterns(def);
                    }
                    if (duplicate_pattern_signature(entry, def)) {
                        report(def->location, ErrorCode::GenericPrimaryRedefined, std::vector<std::string>{ name });
                    } else {
                        entry.specializations.push_back(def);
                    }
                } else if (entry.primary == nullptr) {
                    before_primary.push_back(def);
                } else {
                    if (duplicate_pattern_signature(entry, def)) {
                        report(def->location, ErrorCode::GenericPrimaryRedefined, std::vector<std::string>{ name });
                    } else {
                        entry.specializations.push_back(def);
                    }
                }
            }
            if (entry.primary == nullptr) {
                if (!before_primary.empty()) {
                    if (has_primary_shaped) {
                        for (GenericDefinition* def : before_primary) {
                            report(def->location, ErrorCode::GenericSpecializationBeforePrimary, std::vector<std::string>{ def->name, name });
                        }
                    } else {
                        report(before_primary.front()->location,
                            ErrorCode::GenericSpecializationWithoutPrimary, std::vector<std::string>{ name });
                    }
                }
            } else {
                for (GenericDefinition* def : before_primary) {
                    report(def->location, ErrorCode::GenericSpecializationBeforePrimary, std::vector<std::string>{ def->name, name });
                }
            }
            generics_[name] = std::move(entry);
        }
    }

    void GenericExpander::collect_declarations() {
        std::function<void(const std::vector<std::unique_ptr<TopLevel>>&)> scan_top;
        for (auto& top : program_->top_levels) {
            if (auto* sd = dynamic_cast<StructDefinition*>(top.get())) {
                struct_defs_[sd->name] = sd;
                declare_name(sd->name);
            } else if (auto* fd = dynamic_cast<FunctionDefinition*>(top.get())) {
                register_function_definition(fd);
                declare_name(fd->name);
            } else if (auto* ed = dynamic_cast<ExternDeclaration*>(top.get())) {
                extern_defs_[ed->name] = ed;
                declare_name(ed->name);
            } else if (auto* vd = dynamic_cast<VariableDeclaration*>(top.get())) {
                declare_name(vd->name);
            } else if (auto* g = dynamic_cast<GenericDefinition*>(top.get())) {
                declare_name(g->name);
            }
        }
        (void)scan_top;
    }

    void GenericExpander::register_function_definition(
        FunctionDefinition* function) {
        if (function == nullptr) { return; }

        std::vector<FunctionDefinition*>& candidates =
            func_defs_[function->name];
        if (std::find(candidates.begin(), candidates.end(), function) ==
            candidates.end()) {
            candidates.push_back(function);
        }
    }

    std::optional<std::string> GenericExpander::ensure_instantiation(const GenericRef& ref,
        bool whole_block) {
        InstantiationDepthGuard depth_guard(*this, ref.location);

        if (!depth_guard.ok()) { return std::nullopt; }

        auto entry_it = generics_.find(ref.generic_name);
        if (entry_it == generics_.end()) {
            report(ref.location, ErrorCode::GenericUndefined, std::vector<std::string>{ ref.generic_name });
            return std::nullopt;
        }

        GenericEntry& entry = entry_it->second;
        if (entry.primary == nullptr) {
            report(ref.location, ErrorCode::GenericNoMatch, std::vector<std::string>{ ref.generic_name, arguments_text(ref) });
            return std::nullopt;
        }

        const std::size_t parameter_count = entry.primary->parameters.size();
        const bool trailer_is_pack = parameter_count > 0 &&
            entry.primary->parameters.back().is_pack;
        std::size_t expected = trailer_is_pack ? parameter_count - 1
                                               : parameter_count;
        if ((!trailer_is_pack && ref.arguments.size() != expected) ||
            (trailer_is_pack && ref.arguments.size() < expected)) {
            report(ref.location, ErrorCode::GenericArgCountMismatch, std::vector<std::string>{ ref.generic_name, arguments_text(ref),
                  std::to_string(expected), std::to_string(ref.arguments.size()) });
            return std::nullopt;
        }

        Substitution primary_sub;

        for (std::size_t i = 0; i < expected; ++i) {
            const GenericParameter& param = entry.primary->parameters[i];
            const GenericArgument& arg = ref.arguments[i];

            if (param.is_type) {
                if (!arg.is_type) {
                    report(ref.location, ErrorCode::GenericNonTypeArgTypeMismatch, std::vector<std::string>{ param.name, arg.normalize() });
                    return std::nullopt;
                }
                if (!constraint_satisfied(param.constraint, arg.type)) {
                    Type resolved_arg = arg.type;
                    if (resolved_arg.generic_ref) {
                        resolve_type(resolved_arg, ref.location);
                    }
                    if (constraint_satisfied(param.constraint, resolved_arg)) {
                        primary_sub.types[param.name] = resolved_arg;
                        continue;
                    }
                    report(ref.location, ErrorCode::GenericConstraintViolated, std::vector<std::string>{ ref.generic_name, arguments_text(ref),
                          std::to_string(i + 1), param.constraint.to_string(),
                          arg.type.to_string() });
                    return std::nullopt;
                }
                primary_sub.types[param.name] = arg.type;
            } else {
                if (arg.is_type) {
                    if (arg.expression != nullptr) {
                        ConstantValue value;
                        if (resolve_constant_argument(arg, nullptr, value)) {
                            Type target = param.constant_type;
                            if (param.constant_type_is_parameter) {
                                auto found = primary_sub.types.find(
                                    param.constant_type_parameter);
                                if (found != primary_sub.types.end()) {
                                    target = found->second;
                                }
                            }
                            if (target.kind != TypeKind::Void) {
                                const bool integral =
                                    target.kind == TypeKind::Int ||
                                    target.kind == TypeKind::Char ||
                                    target.kind == TypeKind::Bool;
                                if (integral && value.is_float) {
                                    value.is_float = false;
                                    value.int_value =
                                        static_cast<long long>(
                                            value.float_value);
                                } else if (target.kind == TypeKind::Float ||
                                    target.kind == TypeKind::Double) {
                                    if (!value.is_float) {
                                        value.float_value =
                                            static_cast<double>(
                                                value.int_value);
                                        value.is_float = true;
                                    }
                                }
                            }
                            primary_sub.constants[param.name] = value;
                            AST::Type declared = param.constant_type;
                            if (param.constant_type_is_parameter) {
                                auto found = primary_sub.types.find(
                                    param.constant_type_parameter);
                                if (found != primary_sub.types.end()) {
                                    declared = found->second;
                                }
                            }
                            primary_sub.constant_types[param.name] = declared;
                            continue;
                        }
                    }

                    const std::string type_text = arg.type.to_string();
                    bool parameter_name = false;
                    if (entry.primary != nullptr) {
                        for (const GenericParameter& candidate : entry.primary->parameters) {
                            if (candidate.name == type_text) {
                                parameter_name = true;
                                break;
                            }
                        }
                    }
                    const bool known_type = is_builtin_type_name(type_text) ||
                        struct_defs_.find(type_text) != struct_defs_.end() ||
                        parameter_name;
                    if (!known_type) {
                        report(ref.location, ErrorCode::GenericNonTypeArgNotConstant,
                            std::vector<std::string>{ arg.text.empty()
                                ? arg.normalize() : arg.text });
                    } else {
                        report(ref.location, ErrorCode::GenericNonTypeArgTypeMismatch,
                            std::vector<std::string>{ param.constant_type_is_parameter ? param.constant_type_parameter
                                                       : param.constant_type.to_string(),
                              arg.type.to_string() });
                    }
                    return std::nullopt;
                }
                if (arg.constant_actual_type.kind != TypeKind::Void) {
                    Type target = param.constant_type;
                    if (param.constant_type_is_parameter) {
                        auto found = primary_sub.types.find(param.constant_type_parameter);
                        if (found != primary_sub.types.end()) { target = found->second; }
                    }
                    bool string_argument =
                        (arg.constant_actual_type.kind == TypeKind::String);
                    if (target.kind != TypeKind::Void &&
                        !(string_argument && target.kind == TypeKind::String)) {
                        report(ref.location, ErrorCode::GenericNonTypeArgTypeMismatch, std::vector<std::string>{ target.to_string(), arg.constant_actual_type.to_string() });
                        return std::nullopt;
                    }
                }
                ConstantValue value;
                if (!resolve_constant_argument(arg, nullptr, value)) {
                    report(ref.location, ErrorCode::GenericNonTypeArgNotConstant,
                      std::vector<std::string>{ arg.text.empty() ? arg.normalize()
                                                                 : arg.text });
                    return std::nullopt;
                }
                Type target = param.constant_type;
                if (param.constant_type_is_parameter) {
                    auto found = primary_sub.types.find(param.constant_type_parameter);
                    if (found != primary_sub.types.end()) { target = found->second; }
                }
                if (target.kind != TypeKind::Void) {
                    bool integral = (target.kind == TypeKind::Int || target.kind == TypeKind::Char ||
                        target.kind == TypeKind::Bool);
                    if (integral && value.is_float) {
                        value.is_float = false;
                        value.int_value = static_cast<long long>(value.float_value);
                    } else if (integral) {
                        long long limit = (target.kind == TypeKind::Char) ? 255
                            : (target.kind == TypeKind::Bool ? 1
                                : (long long)0x7fffffffLL);
                        if (value.int_value > limit || value.int_value < -limit - 1) {
                            report(ref.location, ErrorCode::GenericNonTypeArgOutOfRange, std::vector<std::string>{ target.to_string() });
                            return std::nullopt;
                        }
                    } else if (target.kind == TypeKind::Float || target.kind == TypeKind::Double) {
                        if (!value.is_float) {
                            value.float_value = static_cast<double>(value.int_value);
                            value.is_float = true;
                        }
                    }
                }
                primary_sub.constants[param.name] = value;
                AST::Type declared = param.constant_type;
                if (param.constant_type_is_parameter) {
                    auto found = primary_sub.types.find(param.constant_type_parameter);
                    if (found != primary_sub.types.end()) { declared = found->second; }
                }
                primary_sub.constant_types[param.name] = declared;
            }
        }

        if (trailer_is_pack) {
            const GenericParameter& pack_param = entry.primary->parameters.back();
            if (pack_param.is_type) {
                std::vector<AST::Type> elements;
                for (std::size_t i = expected; i < ref.arguments.size(); ++i) {
                    const GenericArgument& arg = ref.arguments[i];
                    if (!arg.is_type) {
                        report(ref.location, ErrorCode::GenericNonTypeArgTypeMismatch,
                            std::vector<std::string>{ pack_param.name,
                                arg.normalize() });
                        return std::nullopt;
                    }
                    AST::Type resolved = arg.type;
                    if (resolved.generic_ref) {
                        resolve_type(resolved, ref.location);
                    }
                    if (!constraint_satisfied(pack_param.constraint, resolved)) {
                        report(ref.location, ErrorCode::GenericConstraintViolated,
                            std::vector<std::string>{ ref.generic_name,
                                arguments_text(ref), std::to_string(i + 1),
                                pack_param.constraint.to_string(),
                                arg.type.to_string() });
                        return std::nullopt;
                    }
                    elements.push_back(std::move(resolved));
                }
                primary_sub.type_packs[pack_param.name] = std::move(elements);
            } else {
                AST::Type element = pack_param.constant_type;
                if (pack_param.constant_type_is_parameter) {
                    auto found =
                        primary_sub.types.find(pack_param.constant_type_parameter);
                    if (found != primary_sub.types.end()) { element = found->second; }
                }
                std::vector<ConstantValue> values;
                for (std::size_t i = expected; i < ref.arguments.size(); ++i) {
                    const GenericArgument& arg = ref.arguments[i];
                    if (arg.is_type) {
                        report(ref.location, ErrorCode::GenericNonTypeArgTypeMismatch,
                            std::vector<std::string>{ element.to_string(),
                                arg.type.to_string() });
                        return std::nullopt;
                    }
                    ConstantValue value;
                    if (!resolve_constant_argument(arg, nullptr, value)) {
                        report(ref.location, ErrorCode::GenericNonTypeArgNotConstant,
                            std::vector<std::string>{ arg.text.empty()
                                ? arg.normalize() : arg.text });
                        return std::nullopt;
                    }
                    if (element.kind != TypeKind::Void) {
                        const bool integral = element.kind == TypeKind::Int ||
                            element.kind == TypeKind::Char ||
                            element.kind == TypeKind::Bool ||
                            element.kind == TypeKind::Uint ||
                            element.kind == TypeKind::Lint ||
                            element.kind == TypeKind::Luint;
                        if (integral && value.is_float) {
                            value.is_float = false;
                            value.int_value =
                                static_cast<long long>(value.float_value);
                        } else if (integral && element.kind == TypeKind::Char) {
                            if (value.int_value < -128 || value.int_value > 255) {
                                report(ref.location,
                                    ErrorCode::GenericNonTypeArgOutOfRange,
                                    std::vector<std::string>{ element.to_string() });
                                return std::nullopt;
                            }
                        } else if (element.kind == TypeKind::Bool) {
                            if (value.int_value < 0 || value.int_value > 1) {
                                report(ref.location,
                                    ErrorCode::GenericNonTypeArgOutOfRange,
                                    std::vector<std::string>{ element.to_string() });
                                return std::nullopt;
                            }
                        } else if (element.kind == TypeKind::Float ||
                            element.kind == TypeKind::Double) {
                            if (!value.is_float) {
                                value.float_value =
                                    static_cast<double>(value.int_value);
                                value.is_float = true;
                            }
                        }
                    }
                    values.push_back(std::move(value));
                }
                primary_sub.const_pack_element[pack_param.name] = element;
                primary_sub.const_packs[pack_param.name] = std::move(values);
            }
        }

        std::vector<GenericDefinition*> candidates;
        std::vector<Substitution> candidate_subs;
        for (GenericDefinition* spec : entry.specializations) {
            MatchResult mr = match_specialization(spec, ref);
            if (!mr.matched) { continue; }
            candidates.push_back(spec);
            candidate_subs.push_back(mr.substitution);
        }
        GenericDefinition* block = entry.primary;
        Substitution sub = primary_sub;
        if (!candidates.empty()) {
            int best = -1;
            bool ambiguous = false;
            for (std::size_t i = 0; i < candidates.size(); ++i) {
                bool is_best = true;
                for (std::size_t j = 0; j < candidates.size(); ++j) {
                    if (i == j) { continue; }
                    if (!more_specialized(candidates[i], candidates[j])) {
                        is_best = false;
                        break;
                    }
                }
                if (is_best) {
                    if (best != -1) {
                        ambiguous = true;
                    } else {
                        best = static_cast<int>(i);
                    }
                }
            }
            if (ambiguous || best < 0) {
                report(ref.location, ErrorCode::GenericAmbiguousSpecialization, std::vector<std::string>{ ref.generic_name, arguments_text(ref) });
                return std::nullopt;
            }
            if (best >= 0) {
                block = candidates[static_cast<std::size_t>(best)];
                sub = candidate_subs[static_cast<std::size_t>(best)];
            }
        }

        GenericRef key_ref = ref;
        key_ref.member.clear();
        std::string instance_key = key_ref.mangle();
        auto kind_it = instantiated_kind_signatures_.find(instance_key);
        std::string kind_signature = arguments_kind_signature(ref);
        if (kind_it == instantiated_kind_signatures_.end()) {
            instantiated_kind_signatures_[instance_key] = kind_signature;
        } else if (kind_it->second != kind_signature) {
            report(ref.location, ErrorCode::GenericNormalizationConflict,
                std::vector<std::string>{});

            return std::nullopt;
        }

        sub.instance = key_ref;
        instance_substitutions_[instance_key] = sub;
        std::vector<TopLevel*> block_member_ptrs;
        std::vector<std::unique_ptr<TopLevel>>& parsed_emit_members =
            instance_emit_members_[instance_key];

        for (auto& member : block->members) {
            block_member_ptrs.push_back(member.get());
        }

        for (TopLevel* member : instance_emit_member_ptrs_[instance_key]) {
            if (std::find(block_member_ptrs.begin(), block_member_ptrs.end(),
                member) == block_member_ptrs.end()) {
                block_member_ptrs.push_back(member);
            }
        }

        for (const auto& member : parsed_emit_members) {
            block_member_ptrs.push_back(member.get());
        }

        if (!block->compile_time_items.empty() &&
            processed_compile_time_instances_.insert(instance_key).second) {
            std::string emit_text;
            SourceLocation emit_loc = ref.location;

            if (!process_compile_time_items(block->compile_time_items, sub, emit_text,
                emit_loc, block_member_ptrs, parsed_emit_members)) {
                return std::nullopt;
            }

            if (!flush_emit_string(emit_text, emit_loc, sub, block_member_ptrs,
                parsed_emit_members)) {
                return std::nullopt;
            }
        }

        for (TopLevel* member : block_member_ptrs) {
            bool from_block = false;
            for (const auto& source_member : block->members) {
                if (source_member.get() == member) {
                    from_block = true;
                    break;
                }
            }
            if (from_block) { continue; }

            std::vector<TopLevel*>& cached =
                instance_emit_member_ptrs_[instance_key];
            if (std::find(cached.begin(), cached.end(), member) ==
                cached.end()) {
                cached.push_back(member);
            }
        }

        if (!instance_scopes_.empty()) {
            instance_scopes_.back().insert(instance_key);
        }

        if (block != entry.primary) {
            std::unordered_set<std::string> block_members;

            for (auto& member : block->members) {
                if (auto* sd = dynamic_cast<StructDefinition*>(member.get())) {
                    block_members.insert(sd->name);
                } else if (auto* fd = dynamic_cast<FunctionDefinition*>(member.get())) {
                    block_members.insert(fd->name);
                } else if (auto* vd = dynamic_cast<VariableDeclaration*>(member.get())) {
                    block_members.insert(vd->name);
                }
            }

            for (auto& member : entry.primary->members) {
                std::string member_name;

                if (auto* sd = dynamic_cast<StructDefinition*>(member.get())) {
                    member_name = sd->name;
                } else if (auto* fd = dynamic_cast<FunctionDefinition*>(member.get())) {
                    member_name = fd->name;
                } else if (auto* vd = dynamic_cast<VariableDeclaration*>(member.get())) {
                    member_name = vd->name;
                }

                if (member_name.empty() || block_members.count(member_name) != 0) { continue; }
                missing_members_[instance_key].insert(member_name);
            }
        }

        {
            std::vector<std::string>& members = instance_struct_members_[instance_key];
            members.clear();

            for (TopLevel* member : block_member_ptrs) {
                if (auto* sd = dynamic_cast<StructDefinition*>(member)) {
                    members.push_back(sd->name);
                }
            }
        }

        {
            std::vector<std::string>& members = instance_value_members_[instance_key];
            members.clear();

            for (TopLevel* member : block_member_ptrs) {
                if (auto* vd = dynamic_cast<VariableDeclaration*>(member)) {
                    members.push_back(vd->name);
                }
            }

            instance_member_counts_[instance_key] = block_member_ptrs.size();
        }

        std::unordered_map<std::string, std::string> member_types;
        std::unordered_map<std::string, std::string> member_functions;
        auto instantiated_name = [&](const std::string& name) {
            if (name.find('$') != std::string::npos) { return name; }
            return instance_key + "$" + name;
        };

        for (TopLevel* member : block_member_ptrs) {
            if (auto* sd = dynamic_cast<StructDefinition*>(member)) {
                std::string mangled = instantiated_name(sd->name);
                member_types[sd->name] = mangled;
                sub.types[sd->name] = Type::make_struct(mangled);
                sub.members[sd->name] = mangled;
            } else if (auto* fd = dynamic_cast<FunctionDefinition*>(member)) {
                member_functions[fd->name] = instantiated_name(fd->name);
                sub.members[fd->name] = instantiated_name(fd->name);
            } else if (auto* vd = dynamic_cast<VariableDeclaration*>(member)) {
                sub.members[vd->name] = instantiated_name(vd->name);
            }
        }

        auto materialize = [&](const std::string& member_name, bool register_short) -> std::string {
            std::string first_target;

            for (TopLevel* member : block_member_ptrs) {
                if (auto* sd = dynamic_cast<StructDefinition*>(member)) {
                    if (!member_name.empty() && sd->name != member_name) { continue; }

                    std::string mangled = instantiated_name(sd->name);

                    if (instantiated_members_.insert(mangled).second) {
                        auto clone = std::make_unique<StructDefinition>(sd->location, mangled,
                            std::vector<StructDefinition::Member>{});
                        clone->no_copy = sd->no_copy;
                        clone->no_move = sd->no_move;

                        instantiation_parent_targets_.push_back(mangled);

                        for (const auto& m : sd->members) {
                            clone->members.push_back(clone_member(m, sub));
                        }

                        for (const auto& special : sd->special_members) {
                            clone->special_members.push_back(clone_special_member(special.get(),
                                sub));
                        }

                        instantiation_parent_targets_.pop_back();
                        drain_deferred_instantiations();

                        StructDefinition* raw = clone.get();
                        struct_defs_[mangled] = raw;
                        instantiated_structs_[mangled] = raw;
                        add_top_level(std::move(clone));
                    }

                    if (register_short) {
                        ShortBinding binding;
                        binding.target = mangled;
                        binding.is_type = true;
                        binding.instance = instance_key;
                        register_short_name(sd->name, binding, ref.generic_name, sd->location);
                    }

                    if (!member_name.empty() && first_target.empty()) {
                        first_target = mangled;
                    }
                } else if (auto* fd = dynamic_cast<FunctionDefinition*>(member)) {
                    if (!member_name.empty() && fd->name != member_name) { continue; }
                    std::string mangled = instantiated_name(fd->name);
                    std::string member_key = mangled + "(";

                    for (const Type& p : fd->parameters) {
                        member_key += p.to_string();
                        member_key += ",";
                    }
                    member_key += ")";

                    if (instantiated_members_.insert(member_key).second) {
                        instantiation_parent_targets_.push_back(mangled);
                        auto clone = clone_function(fd, sub, mangled);
                        instantiation_parent_targets_.pop_back();
                        drain_deferred_instantiations();
                        register_function_definition(clone.get());
                        add_top_level(std::move(clone));
                    }

                    if (register_short) {
                        ShortBinding binding;
                        binding.target = mangled;
                        binding.is_type = false;
                        binding.instance = instance_key;

                        if (!fd->is_operator) {
                            register_short_name(fd->name, binding, ref.generic_name,
                                fd->location);
                        }
                    }

                    if (!member_name.empty() && first_target.empty()) {
                        first_target = mangled;
                    }
                } else if (auto* vd = dynamic_cast<VariableDeclaration*>(member)) {
                    if (!member_name.empty() && vd->name != member_name) { continue; }
                    std::string mangled = instantiated_name(vd->name);

                    if (instantiated_members_.insert(mangled).second) {
                        instantiation_parent_targets_.push_back(mangled);
                        std::unique_ptr<Statement> cloned_statement =
                            clone_statement(vd, sub);
                        instantiation_parent_targets_.pop_back();
                        drain_deferred_instantiations(true);
                        auto* cloned_variable = dynamic_cast<VariableDeclaration*>(
                            cloned_statement.get());
                        auto* cloned_top_level = dynamic_cast<TopLevel*>(
                            cloned_statement.get());

                        if (cloned_variable == nullptr ||
                            cloned_top_level == nullptr) {
                            continue;
                        }

                        cloned_variable->name = mangled;
                        cloned_variable->type.is_const = vd->type.is_const;

                        const bool is_const = cloned_variable->type.is_const ||
                            (cloned_variable->type.kind == TypeKind::Array &&
                                cloned_variable->type.element_type &&
                                cloned_variable->type.element_type->is_const);
                        if (is_const && cloned_variable->initializer) {
                            ConstexprValue value;
                            ConstexprInterpreter interpreter =
                                make_constexpr_interpreter(sub);
                            if (interpreter.evaluate_initializer(
                                cloned_variable->initializer.get(),
                                cloned_variable->type, value)) {
                                constant_member_values_[mangled] = std::move(value);
                            }
                        }

                        instance_variables_[mangled] = cloned_variable;
                        cloned_statement.release();
                        add_top_level(std::unique_ptr<TopLevel>(cloned_top_level));
                    }

                    if (register_short) {
                        ShortBinding binding;
                        binding.target = mangled;
                        binding.is_type = false;
                        binding.instance = instance_key;
                        register_short_name(vd->name, binding, ref.generic_name,
                            vd->location);
                    }

                    if (!member_name.empty() && first_target.empty()) {
                        first_target = mangled;
                    }
                }
            }
            return first_target;
        };

        if (whole_block) {
            if (instantiated_blocks_.insert(instance_key).second) {
                materialize(std::string(), true);
            } else {
                materialize(std::string(), false);
            }

            return instance_key;
        }

        if (ref.member.empty()) {
            return instance_key;
        }

        std::string target = materialize(ref.member, ref.member_scope_access);

        if (target.empty()) {
            bool in_primary = false;

            if (entry.primary != block) {
                for (auto& member : entry.primary->members) {
                    std::string member_name;

                    if (auto* sd = dynamic_cast<StructDefinition*>(member.get())) {
                        member_name = sd->name;
                    } else if (auto* fd = dynamic_cast<FunctionDefinition*>(member.get())) {
                        member_name = fd->name;
                    } else if (auto* vd = dynamic_cast<VariableDeclaration*>(member.get())) {
                        member_name = vd->name;
                    }

                    if (member_name == ref.member) { in_primary = true; break; }
                }
            }

            if (in_primary) {
                report(ref.location, ErrorCode::GenericSpecializationMissingMember, std::vector<std::string>{ instance_key, ref.member });
            } else {
                report(ref.location, ErrorCode::GenericMemberNotInInstantiation, std::vector<std::string>{ ref.member, instance_key });
            }

            return std::nullopt;
        }

        return target;
    }

    bool GenericExpander::defer_instantiation_dependency(
        const GenericRef& ref) {
        if (instantiation_depth_ == 0 || ref.member.empty()) {
            return false;
        }

        const std::size_t depth = instantiation_depth_ + 1;

        if (depth > instantiation_depth_limit_) {
            if (!instantiation_depth_reported_) {
                instantiation_depth_reported_ = true;
                report(ref.location, ErrorCode::InstantiationDepthTooDeep,
                    std::vector<std::string>{
                        std::to_string(instantiation_depth_limit_),
                        std::to_string(depth) });
            }

            return true;
        }

        const std::string key = ref.mangle();

        if (queued_deferred_instantiations_.insert(key).second) {
            deferred_instantiations_.push_back(
                DeferredInstantiation{
                    ref,
                    instantiation_parent_targets_.empty()
                        ? std::string()
                        : instantiation_parent_targets_.back(),
                    depth });
        }

        return true;
    }

    void GenericExpander::drain_deferred_instantiations(bool force) {
        if (draining_deferred_instantiations_ && !force) { return; }

        const bool was_draining = draining_deferred_instantiations_;
        draining_deferred_instantiations_ = true;
        const std::size_t base_frame_count =
            deferred_instantiation_frames_.size();

        if (!deferred_instantiations_.empty()) {
            deferred_instantiation_frames_.push_back(
                DeferredInstantiationFrame{
                    std::move(deferred_instantiations_), 0 });
            deferred_instantiations_.clear();
        }

        while (deferred_instantiation_frames_.size() >
            base_frame_count) {
            DeferredInstantiationFrame& frame =
                deferred_instantiation_frames_.back();

            if (frame.next >= frame.tasks.size()) {
                deferred_instantiation_frames_.pop_back();
                continue;
            }

            DeferredInstantiation pending =
                std::move(frame.tasks[frame.next++]);
            queued_deferred_instantiations_.erase(
                pending.reference.mangle());

            const std::size_t previous_depth = instantiation_depth_;
            instantiation_depth_ = pending.depth - 1;
            const std::size_t output_begin =
                output_ != nullptr ? output_->size() : 0;
            ensure_instantiation(pending.reference, false);
            instantiation_depth_ = previous_depth;

            if (output_ != nullptr && output_begin < output_->size() &&
                !pending.parent_target.empty()) {
                std::size_t parent_index = output_->size();

                for (std::size_t i = output_begin; i > 0; --i) {
                    TopLevel* node = (*output_)[i - 1].get();
                    std::string name;

                    if (auto* sd = dynamic_cast<StructDefinition*>(node)) {
                        name = sd->name;
                    } else if (auto* fd = dynamic_cast<FunctionDefinition*>(node)) {
                        name = fd->name;
                    } else if (auto* vd = dynamic_cast<VariableDeclaration*>(node)) {
                        name = vd->name;
                    }

                    if (name == pending.parent_target) {
                        parent_index = i - 1;
                        break;
                    }
                }

                if (parent_index < output_begin) {
                    std::rotate(output_->begin() + parent_index,
                        output_->begin() + output_begin, output_->end());
                }
            }

            if (!deferred_instantiations_.empty()) {
                deferred_instantiation_frames_.push_back(
                    DeferredInstantiationFrame{
                        std::move(deferred_instantiations_), 0 });
                deferred_instantiations_.clear();
            }

            if (stack_probe_triggered_) {
                deferred_instantiation_frames_.resize(base_frame_count);
                deferred_instantiations_.clear();
                break;
            }
        }

        draining_deferred_instantiations_ = was_draining;
    }

    bool GenericExpander::process_compile_time_items(
        const std::vector<std::unique_ptr<AST::Statement>>& items,
        const Substitution& sub, std::string& text, SourceLocation& text_loc,
        std::vector<AST::TopLevel*>& member_ptrs,
        std::vector<std::unique_ptr<AST::TopLevel>>& owner,
        bool generated) {
        std::function<bool(Node*, bool)> process_item;
        process_item = [&](Node* node, bool generated_item) -> bool {
            if (node == nullptr) { return true; }

            if (generated_item && !validate_emit_generated_node(node)) {
                return false;
            }

            if (auto* emit = dynamic_cast<const EmitStatement*>(node)) {
                if (emit->is_block) {
                    if (!flush_emit_string(text, text_loc, sub, member_ptrs, owner)) {
                        return false;
                    }
                    text.clear();

                    std::vector<std::unique_ptr<TopLevel>> lowered;
                    std::vector<Node*> deferred;

                    auto flush_batch = [&]() -> bool {
                        if (!adopt_emit_top_levels(
                                lowered, sub, member_ptrs, owner)) {
                            return false;
                        }

                        for (Node* deferred_item : deferred) {
                            if (!process_item(deferred_item, true)) {
                                return false;
                            }
                        }

                        deferred.clear();
                        return true;
                    };

                    for (const auto& block_item : emit->block_items) {
                        if (!validate_emit_generated_node(block_item.get())) {
                            return false;
                        }

                        if (auto* namespace_def =
                                dynamic_cast<NamespaceDefinition*>(block_item.get())) {
                            auto lowered_namespace =
                                std::make_unique<NamespaceDefinition>(
                                    namespace_def->location, namespace_def->name);
                            lowered_namespace->members =
                                std::move(namespace_def->members);
                            lowered.push_back(std::move(lowered_namespace));
                            continue;
                        }

                        if (auto* addition =
                                dynamic_cast<AdditionNamespaceStatement*>(
                                    block_item.get())) {
                            auto lowered_addition =
                                std::make_unique<AdditionNamespaceStatement>(
                                    addition->location, addition->path);
                            lowered_addition->members =
                                std::move(addition->members);
                            lowered.push_back(std::move(lowered_addition));
                            continue;
                        }

                        if (auto* access =
                                dynamic_cast<AccessNamespaceStatement*>(
                                    block_item.get())) {
                            lowered.push_back(
                                std::make_unique<AccessNamespaceStatement>(
                                    access->location, access->path));
                            continue;
                        }

                        if (auto* external =
                                dynamic_cast<ExternDeclaration*>(block_item.get())) {
                            auto generated = std::make_unique<ExternDeclaration>(
                                external->location, external->return_type,
                                external->name, external->library,
                                external->parameters, external->param_names);
                            generated->c_symbol_name = external->c_symbol_name;
                            generated->c_variadic = external->c_variadic;
                            lowered.push_back(std::move(generated));
                            continue;
                        }

                        if (dynamic_cast<TopLevelStatement*>(block_item.get()) !=
                                nullptr ||
                            (dynamic_cast<Statement*>(block_item.get()) != nullptr &&
                                dynamic_cast<TopLevel*>(block_item.get()) == nullptr)) {
                            if (!flush_batch()) {
                                return false;
                            }
                            if (!process_item(block_item.get(), true)) {
                                return false;
                            }
                            continue;
                        }

                        deferred.push_back(block_item.get());
                    }

                    if (!flush_batch()) {
                        return false;
                    }
                } else {
                    if (text.empty()) { text_loc = emit->location; }

                    for (const auto& piece : emit->pieces) {
                        std::string piece_text;

                        if (!eval_emit_piece(piece.get(), sub, piece_text)) {
                            report(piece->location, ErrorCode::EmitStringNotConstant, std::vector<std::string>{ expression_text(piece.get()) });
                            return false;
                        }

                        text += piece_text;
                    }
                }
                return true;
            }

            if (auto* ifs = dynamic_cast<const IfStatement*>(node)) {
                if (!ifs->is_compile_time) {
                    report(ifs->location, ErrorCode::GenericStatementNotAllowed,
                        std::vector<std::string>());
                    return false;
                }

                bool condition = false;

                const std::size_t condition_errors_before = diag_.error_count();

                if (!eval_compile_time_condition(ifs->condition.get(), sub, condition)) {
                    if (diag_.error_count() == condition_errors_before) {
                        report(ifs->location, ErrorCode::CompileTimeConditionNotBoolean, std::vector<std::string>{ expression_text(ifs->condition.get()) });
                    }
                    return false;
                }

                const Statement* branch = condition ? ifs->then_block.get()
                                                    : ifs->else_block.get();

                if (branch == nullptr) { return true; }

                if (auto* block = dynamic_cast<const Block*>(branch)) {
                    for (const auto& child : block->statements) {
                        if (!process_item(child.get(), generated_item)) { return false; }
                    }
                } else {
                    report(branch->location, ErrorCode::EmitBlockNotExpandable, std::vector<std::string>());
                    return false;
                }
                return true;
            }

            if (auto* function = dynamic_cast<const FunctionDefinition*>(node)) {
                FunctionDefinition* generated_function =
                    const_cast<FunctionDefinition*>(function);
                if (generated_item &&
                    !namespaces_.rewrite_new_generated_top_level(generated_function)) {
                    return false;
                }
                member_ptrs.push_back(generated_function);
                return true;
            }

            if (auto* struct_def = dynamic_cast<const StructDefinition*>(node)) {
                StructDefinition* generated_struct =
                    const_cast<StructDefinition*>(struct_def);
                if (generated_item &&
                    !namespaces_.rewrite_new_generated_top_level(generated_struct)) {
                    return false;
                }
                member_ptrs.push_back(generated_struct);
                return true;
            }

            if (auto* variable = dynamic_cast<const VariableDeclaration*>(node)) {
                VariableDeclaration* generated_variable =
                    const_cast<VariableDeclaration*>(variable);
                if (generated_item &&
                    !namespaces_.rewrite_new_generated_top_level(generated_variable)) {
                    return false;
                }
                member_ptrs.push_back(generated_variable);
                return true;
            }

            if (auto* statement = dynamic_cast<const TopLevelStatement*>(node)) {
                return process_item(statement->statement.get(), generated_item);
            }

            if (auto* member_block = dynamic_cast<const TopLevelBlock*>(node)) {
                for (const auto& member : member_block->items) {
                    if (!process_item(member.get(), generated_item)) {
                        return false;
                    }
                }
                return true;
            }

            if (auto* namespace_def = dynamic_cast<NamespaceDefinition*>(node)) {
                auto lowered_namespace = std::make_unique<NamespaceDefinition>(
                    namespace_def->location, namespace_def->name);
                lowered_namespace->members = std::move(namespace_def->members);
                std::vector<std::unique_ptr<TopLevel>> lowered;
                lowered.push_back(std::move(lowered_namespace));

                return adopt_emit_top_levels(lowered, sub, member_ptrs, owner);
            }

            if (auto* addition = dynamic_cast<AdditionNamespaceStatement*>(node)) {
                auto lowered_addition = std::make_unique<AdditionNamespaceStatement>(
                    addition->location, addition->path);
                lowered_addition->members = std::move(addition->members);
                std::vector<std::unique_ptr<TopLevel>> lowered;
                lowered.push_back(std::move(lowered_addition));

                return adopt_emit_top_levels(lowered, sub, member_ptrs, owner);
            }

            if (auto* access = dynamic_cast<AccessNamespaceStatement*>(node)) {
                std::vector<std::unique_ptr<TopLevel>> lowered;
                lowered.push_back(std::make_unique<AccessNamespaceStatement>(
                    access->location, access->path));

                return namespaces_.lower_new_top_levels(lowered);
            }

            if (auto* external = dynamic_cast<ExternDeclaration*>(node)) {
                if (!generated_item) {
                    report(external->location, ErrorCode::EmitBlockNotExpandable,
                        std::vector<std::string>());
                    return false;
                }

                return materialize_emit_extern(external, sub);
            }

            report(node->location, ErrorCode::EmitBlockNotExpandable,
                std::vector<std::string>());
            return false;
        };

        for (const auto& item : items) {
            if (!process_item(item.get(), generated)) { return false; }
        }

        return true;
    }

    bool GenericExpander::materialize_emit_extern(
        const ExternDeclaration* external, const Substitution& sub) {
        if (external == nullptr) { return true; }

        auto generated = std::make_unique<ExternDeclaration>(
            external->location, Type::make_void(), external->name,
            external->library, std::vector<Type>{}, external->param_names);

        if (!substitute_type(external->return_type, sub, generated->return_type)) {
            return false;
        }

        for (const Type& parameter : external->parameters) {
            Type substituted = Type::make_void();
            if (!substitute_type(parameter, sub, substituted)) {
                return false;
            }
            generated->parameters.push_back(std::move(substituted));
        }

        generated->c_symbol_name = external->c_symbol_name;
        generated->c_variadic = external->c_variadic;
        add_top_level(std::move(generated));
        return true;
    }

    bool GenericExpander::validate_emit_generated_node(const Node* node) {
        if (node == nullptr) { return true; }

        EmitGeneratedValidator validator;
        validator.set_root(node);

        if (const TopLevel* top_level = dynamic_cast<const TopLevel*>(node)) {
            if (const TopLevelStatement* wrapper =
                    dynamic_cast<const TopLevelStatement*>(top_level)) {
                validator.visit_statement(wrapper->statement.get());
            } else {
                validator.visit_top_level(top_level);
            }
        } else if (const Statement* statement = dynamic_cast<const Statement*>(node)) {
            validator.visit_statement(statement);
        }

        if (validator.violation() == nullptr) { return true; }

        report(validator.violation()->location, validator.error_code(),
            std::vector<std::string>());
        return false;
    }

    class GenericExpander::ExpansionRewriter : public AST::AstRewriter {
    public:
        explicit ExpansionRewriter(GenericExpander& owner) : owner_(owner) {
        }

    protected:
        bool EnterTopLevel(std::unique_ptr<TopLevel>&) override {
            ++top_level_entry_;
            return true;
        }

        void LeaveTopLevel(std::unique_ptr<TopLevel>&) override {
            --top_level_entry_;
        }

        bool EnterExpressionInitializer(ExpressionInitializer* node) override {
            owner_.expand_expression(node->expr);
            return false;
        }

        bool EnterArrayInitializer(ArrayInitializer* node) override {
            for (std::unique_ptr<Initializer>& element : node->elements) {
                owner_.expand_initializer(element.get());
            }

            return false;
        }

        bool EnterBlock(Block* node) override {
            owner_.push_scope();

            for (std::unique_ptr<Statement>& child : node->statements) {
                auto* variable = dynamic_cast<VariableDeclaration*>(child.get());

                if (variable == nullptr || !variable->type.generic_ref ||
                    !variable->type.generic_ref->member.empty()) {
                    continue;
                }

                GenericRef reference = *variable->type.generic_ref;
                auto instance = owner_.ensure_instantiation(reference, true);

                if (!instance.has_value()) { continue; }

                auto member_count = owner_.instance_member_counts_.find(*instance);
                auto value_members = owner_.instance_value_members_.find(*instance);
                auto struct_members = owner_.instance_struct_members_.find(*instance);

                if (member_count == owner_.instance_member_counts_.end() ||
                    member_count->second != 1 ||
                    value_members == owner_.instance_value_members_.end() ||
                    value_members->second.size() != 1 ||
                    (struct_members != owner_.instance_struct_members_.end() &&
                        !struct_members->second.empty())) {
                    continue;
                }

                GenericRef member_reference = reference;
                member_reference.member = value_members->second.front();
                member_reference.member_scope_access = true;

                std::string target_name = *instance + "$" +
                    value_members->second.front();
                auto target = owner_.instance_variables_.find(target_name);
                const bool target_is_const =
                    target != owner_.instance_variables_.end() &&
                    target->second != nullptr &&
                    (target->second->type.is_const ||
                        (target->second->type.kind == TypeKind::Array &&
                            target->second->type.element_type &&
                            target->second->type.element_type->is_const));

                auto apply_target_initializer = [&]() {
                    if (target == owner_.instance_variables_.end() ||
                        target->second == nullptr ||
                        variable->initializer == nullptr) {
                        return;
                    }

                    target->second->initializer =
                        std::move(variable->initializer);

                    auto substitution =
                        owner_.instance_substitutions_.find(*instance);
                    if (target_is_const && substitution !=
                        owner_.instance_substitutions_.end()) {
                        ConstexprValue value;
                        ConstexprInterpreter interpreter =
                            owner_.make_constexpr_interpreter(
                                substitution->second);
                        if (interpreter.evaluate_initializer(
                            target->second->initializer.get(),
                            target->second->type, value)) {
                            owner_.constant_member_values_[target_name] =
                                std::move(value);
                        } else {
                            owner_.constant_member_values_.erase(target_name);
                        }
                    }
                };

                auto* expression_initializer = dynamic_cast<ExpressionInitializer*>(
                    variable->initializer.get());

                if (expression_initializer != nullptr) {
                    if (target_is_const) {
                        apply_target_initializer();
                        child = std::make_unique<InstantiationStatement>(
                            variable->location, std::move(reference));
                        continue;
                    }

                    auto assignment = std::make_unique<AssignmentExpression>(
                        variable->location,
                        std::make_unique<PrimaryExpression>(
                            variable->location, std::move(member_reference)),
                        AssignmentExpression::Operator::Assign,
                        std::move(expression_initializer->expr));
                    auto statement = std::make_unique<ExpressionStatement>(
                        variable->location, std::move(assignment));
                    std::vector<std::unique_ptr<Statement>> replacement;
                    replacement.push_back(std::move(statement));
                    child = std::make_unique<Block>(variable->location,
                        std::move(replacement));
                    continue;
                }

                apply_target_initializer();

                child = std::make_unique<InstantiationStatement>(
                    variable->location, std::move(reference));
            }

            for (std::unique_ptr<Statement>& child : node->statements) {
                if (auto* vd = dynamic_cast<VariableDeclaration*>(child.get())) {
                    if (owner_.short_scopes_.back().count(vd->name) != 0) {
                        owner_.report(vd->location,
                            ErrorCode::GenericShortNameConflict,
                            std::vector<std::string>{ vd->name });
                    }
                    owner_.declare_name(vd->name);
                } else if (auto* sd = dynamic_cast<StructDefinition*>(child.get())) {
                    if (owner_.short_scopes_.back().count(sd->name) != 0) {
                        owner_.report(sd->location,
                            ErrorCode::GenericShortNameConflict,
                            std::vector<std::string>{ sd->name });
                    }
                    owner_.declare_name(sd->name);
                } else if (auto* def = dynamic_cast<FunctionDefinition*>(child.get())) {
                    if (owner_.short_scopes_.back().count(def->name) != 0) {
                        owner_.report(def->location,
                            ErrorCode::GenericShortNameConflict,
                            std::vector<std::string>{ def->name });
                    }
                    owner_.declare_name(def->name);
                }
            }

            for (std::unique_ptr<Statement>& child : node->statements) {
                owner_.expand_statement(child.get());
            }

            std::vector<std::unique_ptr<Statement>> kept;
            kept.reserve(node->statements.size());

            for (std::unique_ptr<Statement>& child : node->statements) {
                if (dynamic_cast<InstantiationStatement*>(child.get()) != nullptr) {
                    continue;
                }

                kept.push_back(std::move(child));
            }

            node->statements = std::move(kept);
            owner_.pop_scope();
            return false;
        }

        bool EnterVariableDeclaration(VariableDeclaration* node) override {
            owner_.resolve_type(node->type, node->location);

            if (top_level_entry_ > 0 && node->array_size_expr) {
                owner_.expand_expression(node->array_size_expr);
            }

            owner_.expand_initializer(node->initializer.get());
            return false;
        }

        bool EnterStructDefinition(StructDefinition* node) override {
            owner_.struct_defs_[node->name] = node;

            for (StructDefinition::Member& member : node->members) {
                owner_.resolve_type(member.type, node->location);

                if (member.array_size_expr) {
                    owner_.expand_expression(member.array_size_expr);
                }

                if (member.function_pointer_type.has_value()) {
                    owner_.resolve_type(*member.function_pointer_type, node->location);
                }

                owner_.expand_initializer(member.initializer.get());
            }

            for (std::unique_ptr<SpecialMemberFunction>& smf : node->special_members) {
                for (Type& parameter : smf->parameters) {
                    owner_.resolve_type(parameter, smf->location);
                }

                owner_.resolve_type(smf->parameter_type, smf->location);

                if (top_level_entry_ > 0) {
                    for (std::unique_ptr<Expression>& value : smf->parameter_defaults) {
                        owner_.expand_expression(value);
                    }

                    owner_.push_scope();

                    if (!smf->parameter_name.empty()) {
                        owner_.declare_name(smf->parameter_name);
                    }

                    for (const std::string& name : smf->parameter_names) {
                        owner_.declare_name(name);
                    }
                }

                owner_.expand_statement(smf->body.get());

                if (top_level_entry_ > 0) {
                    owner_.pop_scope();
                }
            }

            return false;
        }

        bool EnterFunctionDefinition(FunctionDefinition* node) override {
            if (top_level_entry_ == 0) {
                return false;
            }

            owner_.register_function_definition(node);
            owner_.resolve_type(node->return_type, node->location);

            for (Type& parameter : node->parameters) {
                owner_.resolve_type(parameter, node->location);
            }

            for (std::unique_ptr<Expression>& value : node->param_defaults) {
                owner_.expand_expression(value);
            }

            owner_.push_scope();

            for (const std::string& name : node->param_names) {
                if (!name.empty()) {
                    owner_.declare_name(name);
                }
            }

            owner_.expand_statement(node->body.get());
            owner_.pop_scope();
            return false;
        }

        bool EnterIfStatement(IfStatement* node) override {
            owner_.expand_expression(node->condition);
            owner_.expand_statement(node->then_block.get());
            owner_.expand_statement(node->else_block.get());
            return false;
        }

        bool EnterForStatement(ForStatement* node) override {
            owner_.push_scope();

            if (node->init) {
                if (auto* vd = dynamic_cast<VariableDeclaration*>(node->init.get())) {
                    owner_.declare_name(vd->name);
                }

                owner_.expand_statement(node->init.get());
            }

            owner_.expand_expression(node->condition);
            owner_.expand_expression(node->step);
            owner_.expand_statement(node->body.get());
            owner_.pop_scope();
            return false;
        }

        bool EnterWhileStatement(WhileStatement* node) override {
            owner_.expand_expression(node->condition);
            owner_.expand_statement(node->body.get());
            return false;
        }

        bool EnterSwitchCaseStatement(SwitchCaseStatement* node) override {
            owner_.expand_expression(node->condition);

            for (SwitchCaseStatement::Clause& clause : node->clauses) {
                owner_.expand_expression(clause.condition);
                owner_.push_scope();

                for (std::unique_ptr<Statement>& child : clause.statements) {
                    if (auto* vd = dynamic_cast<VariableDeclaration*>(child.get())) {
                        owner_.declare_name(vd->name);
                    } else if (auto* sd = dynamic_cast<StructDefinition*>(child.get())) {
                        owner_.declare_name(sd->name);
                    } else if (auto* def = dynamic_cast<FunctionDefinition*>(child.get())) {
                        owner_.declare_name(def->name);
                    }
                }

                for (std::unique_ptr<Statement>& child : clause.statements) {
                    owner_.expand_statement(child.get());
                }

                std::vector<std::unique_ptr<Statement>> kept;
                kept.reserve(clause.statements.size());

                for (std::unique_ptr<Statement>& child : clause.statements) {
                    if (dynamic_cast<InstantiationStatement*>(child.get()) != nullptr) {
                        continue;
                    }

                    kept.push_back(std::move(child));
                }

                clause.statements = std::move(kept);
                owner_.pop_scope();
            }

            return false;
        }

        bool EnterReturnStatement(ReturnStatement* node) override {
            owner_.expand_expression(node->value);
            return false;
        }

        bool EnterExpressionStatement(ExpressionStatement* node) override {
            owner_.expand_expression(node->expr);
            return false;
        }

        bool EnterDestructStatement(DestructStatement* node) override {
            owner_.expand_expression(node->target);
            return false;
        }

        bool EnterInstantiationStatement(InstantiationStatement* node) override {
            owner_.ensure_instantiation(node->reference,
                node->reference.member.empty());
            return false;
        }

        bool EnterExternDeclaration(ExternDeclaration* node) override {
            owner_.resolve_type(node->return_type, node->location);

            for (Type& parameter : node->parameters) {
                owner_.resolve_type(parameter, node->location);
            }

            return false;
        }

        bool EnterGenericDefinition(GenericDefinition*) override { return false; }

        bool EnterNamespaceDefinition(NamespaceDefinition*) override {
            return false;
        }

        bool EnterAdditionNamespaceStatement(AdditionNamespaceStatement*) override {
            return false;
        }

        bool EnterCondDefinition(CondDefinition*) override { return false; }

        bool EnterConditionalBlock(ConditionalBlock*) override { return false; }

        bool EnterTopLevelBlock(TopLevelBlock*) override { return false; }

        bool EnterEmitStatement(EmitStatement*) override { return false; }

    private:
        GenericExpander& owner_;
        int top_level_entry_ = 0;
    };

    void GenericExpander::expand_initializer(Initializer* init) {
        if (init == nullptr) { return; }
        if (poisoned_node(init)) { return; }
        PoisonGuard guard(*this, init);

        std::unique_ptr<Initializer> holder(init);
        ExpansionRewriter rewriter(*this);
        rewriter.rewrite_initializer(holder);
        holder.release();
    }

    void GenericExpander::expand_statement(Statement* stmt) {
        if (stmt == nullptr) { return; }
        if (poisoned_node(stmt)) { return; }
        PoisonGuard guard(*this, stmt);

        std::unique_ptr<Statement> holder(stmt);
        ExpansionRewriter rewriter(*this);
        rewriter.rewrite_statement(holder);
        holder.release();
    }

    void GenericExpander::expand_top_level(TopLevel* node) {
        if (node == nullptr) { return; }
        if (poisoned_node(node)) { return; }
        PoisonGuard guard(*this, node);

        std::unique_ptr<TopLevel> holder(node);
        ExpansionRewriter rewriter(*this);
        rewriter.rewrite_top_level(holder);
        holder.release();
    }

    bool GenericExpander::expand(AST::Program* program) {
        if (program == nullptr) { return false; }
        program_ = program;
        had_error_ = false;
        poisoned_.clear();
        deferred_instantiations_.clear();
        deferred_instantiation_frames_.clear();
        queued_deferred_instantiations_.clear();
        instantiation_parent_targets_.clear();
        draining_deferred_instantiations_ = false;
        push_scope();
        collect_declarations();
        collect_generics();
        Substitution empty_substitution;
        for (const auto& top : program_->top_levels) {
            auto* variable = dynamic_cast<VariableDeclaration*>(top.get());
            if (variable == nullptr || variable->initializer == nullptr) {
                continue;
            }

            const bool is_const = variable->type.is_const ||
                (variable->type.kind == TypeKind::Array &&
                    variable->type.element_type &&
                    variable->type.element_type->is_const);
            if (!is_const) { continue; }

            ConstexprInterpreter interpreter =
                make_constexpr_interpreter(empty_substitution);
            ConstexprValue value;
            if (interpreter.evaluate_initializer(variable->initializer.get(),
                variable->type, value)) {
                constant_member_values_[variable->name] = std::move(value);
            }
        }
        std::vector<std::unique_ptr<TopLevel>> output;
        output.reserve(program_->top_levels.size());
        output_ = &output;
        for (auto& top : program_->top_levels) {
            if (dynamic_cast<GenericDefinition*>(top.get()) != nullptr) {
                continue;
            }
            if (auto* inst = dynamic_cast<InstantiationStatement*>(top.get())) {
                ensure_instantiation(inst->reference, inst->reference.member.empty());
                drain_deferred_instantiations();
                continue;
            }
            expand_top_level(top.get());
            drain_deferred_instantiations();
            output.push_back(std::move(top));
        }
        output_ = nullptr;
        pop_scope();
        program_->top_levels = std::move(output);
        return !had_error_;
    }

}
