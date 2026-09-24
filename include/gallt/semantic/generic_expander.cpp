#include "generic_expander.hpp"
#include "generic_expander_detail.hpp"
#include "constant_folding.hpp"
#include "../lexer/lexer.hpp"
#include "../parser/parser.hpp"
#include <algorithm>
#include <cstdlib>
#include <cstdio>
#include <functional>
#include <map>
#include <set>

using namespace gallt::AST;

namespace gallt {
    using namespace generic_expander_detail;

    GenericExpander::GenericExpander(DiagnosticEngine& diag) : diag_(diag) {}

    std::string GenericExpander::signature_text(const std::vector<AST::Type>& types) {
        std::string out;

        for (std::size_t i = 0; i < types.size(); ++i) {
            if (i != 0) { out += ", "; }
            out += types[i].to_string();
        }

        return out;
    }

    void GenericExpander::report(SourceLocation loc, ErrorCode code,
        const std::vector<std::string>& values) {
        diag_.report_error_template(loc, code, values);
        had_error_ = true;
    }

    void GenericExpander::report(SourceLocation loc, ErrorCode code, const std::string& message) {
        diag_.report_error(loc, code, message);
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

            if (it->second.target != binding.target) {
                report(loc, ErrorCode::GenericShortNameAmbiguous, std::vector<std::string>{ name });
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
                std::set<std::string> function_signatures;

                for (auto& member : def->members) {
                    std::string member_name;

                    if (auto* sd = dynamic_cast<StructDefinition*>(member.get())) {
                        member_name = sd->name;
                        if (!struct_member_names.insert(member_name).second ||
                            function_member_names.count(member_name) != 0) {
                            report(def->location, ErrorCode::RedefinedFunction,
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
                        }
                    }

                    if (member_name.empty()) { continue; }

                    for (const GenericParameter& p : def->parameters) {
                        if (p.name == member_name) {
                            if (p.is_expr) {
                                report(def->location,
                                    ErrorCode::ExprParameterNameConflict,
                                    std::vector<std::string>{ p.name });
                            } else {
                                report(def->location, ErrorCode::GenericMemberNameConflictsParameter, std::vector<std::string>{ member_name, p.name });
                            }
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
                func_defs_[fd->name] = fd;
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

    std::optional<std::string> GenericExpander::ensure_instantiation(const GenericRef& ref,
        bool whole_block) {
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

            if (param.is_expr) {
                if (!arg.is_expr) {
                    report(ref.location, ErrorCode::ExprParameterSignatureMismatch,
                        std::vector<std::string>{ param.name, "expr", arg.normalize() });
                    return std::nullopt;
                }

                if (!arg.expr_name.empty() && arg.expr_name != param.name) {
                    report(ref.location, ErrorCode::ExprParameterUndefined,
                        std::vector<std::string>{ arg.expr_name });
                    return std::nullopt;
                }

                std::vector<AST::Type> declared_parameter_types;

                for (const AST::Type& declared : param.expr_param_types) {
                    AST::Type resolved = declared;
                    AST::Type substituted = AST::Type::make_void();
                    if (substitute_type(declared, primary_sub, substituted)) {
                        resolved = substituted;
                    }
                    declared_parameter_types.push_back(resolved);
                }

                AST::Type declared_return_type = param.expr_return_type;
                {
                    AST::Type substituted = AST::Type::make_void();
                    if (substitute_type(param.expr_return_type, primary_sub, substituted)) {
                        declared_return_type = substituted;
                    }
                }

                Substitution::ExpressionBinding binding;
                binding.name = param.name;
                binding.body = arg.expr_body;
                binding.normalized = arg.text;

                if (arg.expr_shorthand) {
                    bool all_named = !declared_parameter_types.empty() &&
                        param.expr_param_names.size() == declared_parameter_types.size();
                    for (const std::string& pname : param.expr_param_names) {
                        if (pname.empty()) { all_named = false; }
                    }

                    if (declared_parameter_types.empty()) {
                        all_named = true;
                    }

                    if (!all_named) {
                        report(ref.location, ErrorCode::ExprParameterShorthandRequiresParameterNames,
                            std::vector<std::string>{ param.name });
                        return std::nullopt;
                    }

                    binding.parameter_types = declared_parameter_types;
                    binding.parameter_names = param.expr_param_names;
                    binding.return_type = declared_return_type;
                } else {
                    if (arg.expr_param_types.size() != declared_parameter_types.size()) {
                        report(ref.location, ErrorCode::ExprParameterSignatureMismatch,
                            std::vector<std::string>{ param.name,
                                signature_text(declared_parameter_types),
                                signature_text(arg.expr_param_types) });
                        return std::nullopt;
                    }

                    for (std::size_t k = 0; k < arg.expr_param_types.size(); ++k) {
                        if (!(arg.expr_param_types[k] == declared_parameter_types[k])) {
                            report(ref.location, ErrorCode::ExprParameterSignatureMismatch,
                                std::vector<std::string>{ param.name,
                                    signature_text(declared_parameter_types),
                                    signature_text(arg.expr_param_types) });
                            return std::nullopt;
                        }
                    }

                    binding.parameter_types = declared_parameter_types;
                    binding.parameter_names = arg.expr_param_names;

                    if (binding.parameter_names.size() != binding.parameter_types.size()) {
                        binding.parameter_names = param.expr_param_names;
                    }

                    binding.return_type = declared_return_type;

                    if (!(arg.expr_return_type == declared_return_type)) {
                        report(ref.location, ErrorCode::ExprParameterReturnTypeMismatch,
                            std::vector<std::string>{ param.name,
                                declared_return_type.to_string(),
                                arg.expr_return_type.to_string() });
                        return std::nullopt;
                    }
                }

                if (!validate_expression_body(param, binding, ref.location)) {
                    return std::nullopt;
                }

                if (primary_sub.types.count(param.name) != 0 ||
                    primary_sub.constants.count(param.name) != 0 ||
                    primary_sub.expressions.count(param.name) != 0) {
                    report(ref.location, ErrorCode::ExprParameterNameConflict,
                        std::vector<std::string>{ param.name });
                    return std::nullopt;
                }

                {
                    bool conflicts = false;

                    for (std::size_t k = 0; k < entry.primary->parameters.size(); ++k) {
                        const GenericParameter& other = entry.primary->parameters[k];
                        if (k != i && other.name == param.name) {
                            conflicts = true;
                            break;
                        }
                    }

                    if (!conflicts) {
                        for (auto& member : entry.primary->members) {
                            std::string member_name;

                            if (auto* sd = dynamic_cast<StructDefinition*>(member.get())) {
                                member_name = sd->name;
                            } else if (auto* fd = dynamic_cast<FunctionDefinition*>(member.get())) {
                                member_name = fd->name;
                            }
                            if (!member_name.empty() && member_name == param.name) {
                                conflicts = true;
                                break;
                            }
                        }
                    }

                    if (conflicts) {
                        report(ref.location, ErrorCode::ExprParameterNameConflict,
                            std::vector<std::string>{ param.name });
                        return std::nullopt;
                    }
                }

                primary_sub.expressions[param.name] = std::move(binding);
                continue;
            }

            if (param.is_type) {
                if (!arg.is_type) {
                    report(ref.location, ErrorCode::GenericNonTypeArgTypeMismatch, std::vector<std::string>{ param.name, arg.normalize() });
                    return std::nullopt;
                }
                if (!constraint_satisfied(param.constraint, arg.type)) {
                    Type resolved_arg = arg.type;
                    if (resolved_arg.generic_ref) { resolve_type(resolved_arg); }
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
                    if (resolved.generic_ref) { resolve_type(resolved); }
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
                for (const auto& binding_entry : primary_sub.expressions) {
                    if (sub.expressions.count(binding_entry.first) == 0) {
                        sub.expressions[binding_entry.first] = binding_entry.second;
                    }
                }
            }
        }

        for (const auto& binding_entry : sub.expressions) {
            for (auto& member : block->members) {
                std::string member_name;
                if (auto* sd = dynamic_cast<StructDefinition*>(member.get())) {
                    member_name = sd->name;
                } else if (auto* fd = dynamic_cast<FunctionDefinition*>(member.get())) {
                    member_name = fd->name;
                }
                if (!member_name.empty() && member_name == binding_entry.first) {
                    report(ref.location, ErrorCode::ExprParameterNameConflict,
                        std::vector<std::string>{ binding_entry.first });
                    return std::nullopt;
                }
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
            bool has_expression_argument = false;

            for (const GenericArgument& arg : ref.arguments) {
                if (arg.is_expr) {
                    has_expression_argument = true;
                    break;
                }
            }

            if (has_expression_argument) {
                report(ref.location, ErrorCode::ExprParameterNormalizationConflict,
                    std::vector<std::string>{});
            } else {
                report(ref.location, ErrorCode::GenericNormalizationConflict,
                    std::vector<std::string>{});
            }

            return std::nullopt;
        }

        sub.instance = key_ref;
        std::vector<TopLevel*> block_member_ptrs;
        std::vector<std::unique_ptr<TopLevel>> parsed_emit_members;

        for (auto& member : block->members) {
            block_member_ptrs.push_back(member.get());
        }

        if (!block->compile_time_items.empty()) {
            std::string emit_text;
            SourceLocation emit_loc = ref.location;

            if (!process_compile_time_items(block->compile_time_items, sub, emit_text,
                emit_loc, block_member_ptrs, parsed_emit_members)) {
                return std::nullopt;
            }

            if (!flush_emit_string(emit_text, emit_loc, block_member_ptrs,
                parsed_emit_members)) {
                return std::nullopt;
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
                }
            }

            for (auto& member : entry.primary->members) {
                std::string member_name;

                if (auto* sd = dynamic_cast<StructDefinition*>(member.get())) {
                    member_name = sd->name;
                } else if (auto* fd = dynamic_cast<FunctionDefinition*>(member.get())) {
                    member_name = fd->name;
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

        std::unordered_map<std::string, std::string> member_types;
        std::unordered_map<std::string, std::string> member_functions;

        for (TopLevel* member : block_member_ptrs) {
            if (auto* sd = dynamic_cast<StructDefinition*>(member)) {
                std::string mangled = instance_key + "$" + sd->name;
                member_types[sd->name] = mangled;
                sub.types[sd->name] = Type::make_struct(mangled);
                sub.members[sd->name] = mangled;
            } else if (auto* fd = dynamic_cast<FunctionDefinition*>(member)) {
                member_functions[fd->name] = instance_key + "$" + fd->name;
                sub.members[fd->name] = instance_key + "$" + fd->name;
            }
        }

        auto materialize = [&](const std::string& member_name, bool register_short) -> std::string {
            std::string first_target;

            for (TopLevel* member : block_member_ptrs) {
                if (auto* sd = dynamic_cast<StructDefinition*>(member)) {
                    if (!member_name.empty() && sd->name != member_name) { continue; }

                    std::string mangled = instance_key + "$" + sd->name;

                    if (instantiated_members_.insert(mangled).second) {
                        auto clone = std::make_unique<StructDefinition>(sd->location, mangled,
                            std::vector<StructDefinition::Member>{});
                        clone->no_copy = sd->no_copy;
                        clone->no_move = sd->no_move;

                        for (const auto& m : sd->members) {
                            clone->members.push_back(clone_member(m, sub));
                        }

                        for (const auto& special : sd->special_members) {
                            clone->special_members.push_back(clone_special_member(special.get(),
                                sub));
                        }

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
                    std::string mangled = instance_key + "$" + fd->name;
                    std::string member_key = mangled + "(";

                    for (const Type& p : fd->parameters) {
                        member_key += p.to_string();
                        member_key += ",";
                    }
                    member_key += ")";

                    if (instantiated_members_.insert(member_key).second) {
                        auto clone = clone_function(fd, sub, mangled);
                        func_defs_[mangled] = clone.get();
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

    bool GenericExpander::process_compile_time_items(
        const std::vector<std::unique_ptr<AST::Statement>>& items,
        const Substitution& sub, std::string& text, SourceLocation& text_loc,
        std::vector<AST::TopLevel*>& member_ptrs,
        std::vector<std::unique_ptr<AST::TopLevel>>& owner) {
        for (const auto& item : items) {
            if (auto* emit = dynamic_cast<const EmitStatement*>(item.get())) {
                if (emit->is_block) {
                    if (!flush_emit_string(text, text_loc, member_ptrs, owner)) { return false; }
                    text.clear();

                    for (const auto& node : emit->block_items) {
                        if (auto* fd = dynamic_cast<FunctionDefinition*>(node.get())) {
                            member_ptrs.push_back(fd);
                        } else if (auto* sd = dynamic_cast<StructDefinition*>(node.get())) {
                            member_ptrs.push_back(sd);
                        } else {
                            report(node->location, ErrorCode::EmitBlockNotExpandable, std::vector<std::string>());
                            return false;
                        }
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
                continue;
            }

            if (auto* ifs = dynamic_cast<const IfStatement*>(item.get())) {
                bool condition = false;

                if (!eval_compile_time_condition(ifs->condition.get(), sub, condition)) {
                    report(ifs->location, ErrorCode::CompileTimeConditionNotBoolean, std::vector<std::string>{ expression_text(ifs->condition.get()) });
                    return false;
                }

                const Statement* branch = condition ? ifs->then_block.get()
                                                    : ifs->else_block.get();

                if (branch == nullptr) { continue; }

                if (auto* block = dynamic_cast<const Block*>(branch)) {
                    if (!process_compile_time_items(block->statements, sub, text, text_loc,
                        member_ptrs, owner)) {
                        return false;
                    }
                } else {
                    report(branch->location, ErrorCode::EmitBlockNotExpandable, std::vector<std::string>());
                    return false;
                }
                continue;
            }

            report(item->location, ErrorCode::EmitBlockNotExpandable, std::vector<std::string>());
            return false;
        }

        return true;
    }

    void GenericExpander::expand_initializer(Initializer* init) {
        if (init == nullptr) { return; }
        if (auto* e = dynamic_cast<ExpressionInitializer*>(init)) {
            expand_expression(e->expr);
        } else if (auto* a = dynamic_cast<ArrayInitializer*>(init)) {
            for (auto& element : a->elements) { expand_initializer(element.get()); }
        }
    }

    void GenericExpander::expand_statement(Statement* stmt) {
        if (stmt == nullptr) { return; }

        if (auto* block = dynamic_cast<Block*>(stmt)) {
            push_scope();

            for (auto& child : block->statements) {
                if (auto* vd = dynamic_cast<VariableDeclaration*>(child.get())) {
                    declare_name(vd->name);
                } else if (auto* sd = dynamic_cast<StructDefinition*>(child.get())) {
                    declare_name(sd->name);
                } else if (auto* def = dynamic_cast<FunctionDefinition*>(child.get())) {
                    declare_name(def->name);
                }
            }

            for (auto& child : block->statements) { expand_statement(child.get()); }
            std::vector<std::unique_ptr<Statement>> kept;
            kept.reserve(block->statements.size());

            for (auto& child : block->statements) {
                if (dynamic_cast<InstantiationStatement*>(child.get()) != nullptr) { continue; }
                kept.push_back(std::move(child));
            }

            block->statements = std::move(kept);
            pop_scope();
            return;
        }

        if (auto* vd = dynamic_cast<VariableDeclaration*>(stmt)) {
            resolve_type(vd->type);
            expand_initializer(vd->initializer.get());
            return;
        }

        if (auto* sd = dynamic_cast<StructDefinition*>(stmt)) {
            struct_defs_[sd->name] = sd;

            for (auto& member : sd->members) {
                resolve_type(member.type);

                if (member.array_size_expr) {
                    expand_expression(member.array_size_expr);
                }

                if (member.function_pointer_type.has_value()) {
                    resolve_type(*member.function_pointer_type);
                }

                expand_initializer(member.initializer.get());
            }

            for (auto& smf : sd->special_members) {
                for (Type& p : smf->parameters) { resolve_type(p); }
                resolve_type(smf->parameter_type);
                expand_statement(smf->body.get());
            }
            return;
        }

        if (auto* is = dynamic_cast<IfStatement*>(stmt)) {
            expand_expression(is->condition);
            expand_statement(is->then_block.get());
            expand_statement(is->else_block.get());
            return;
        }

        if (auto* fs = dynamic_cast<ForStatement*>(stmt)) {
            push_scope();

            if (fs->init) {
                if (auto* vd = dynamic_cast<VariableDeclaration*>(fs->init.get())) { declare_name(vd->name); }
                expand_statement(fs->init.get());
            }

            expand_expression(fs->condition);
            expand_expression(fs->step);
            expand_statement(fs->body.get());
            pop_scope();
            return;
        }

        if (auto* ws = dynamic_cast<WhileStatement*>(stmt)) {
            expand_expression(ws->condition);
            expand_statement(ws->body.get());
            return;
        }

        if (auto* rs = dynamic_cast<ReturnStatement*>(stmt)) {
            expand_expression(rs->value);
            return;
        }

        if (auto* es = dynamic_cast<ExpressionStatement*>(stmt)) {
            expand_expression(es->expr);
            return;
        }

        if (auto* ds = dynamic_cast<DestructStatement*>(stmt)) {
            expand_expression(ds->target);
            return;
        }

        if (auto* inst = dynamic_cast<InstantiationStatement*>(stmt)) {
            ensure_instantiation(inst->reference, inst->reference.member.empty());
            return;
        }
    }

    void GenericExpander::expand_top_level(TopLevel* node) {
        if (dynamic_cast<GenericDefinition*>(node) != nullptr) {
            return;
        }

        if (auto* inst = dynamic_cast<InstantiationStatement*>(node)) {
            ensure_instantiation(inst->reference, inst->reference.member.empty());
            return;
        }

        if (auto* sd = dynamic_cast<StructDefinition*>(node)) {
            struct_defs_[sd->name] = sd;

            for (auto& member : sd->members) {
                resolve_type(member.type);
                if (member.array_size_expr) { expand_expression(member.array_size_expr); }

                if (member.function_pointer_type.has_value()) {
                    resolve_type(*member.function_pointer_type);
                }

                expand_initializer(member.initializer.get());
            }

            for (auto& smf : sd->special_members) {
                for (Type& p : smf->parameters) { resolve_type(p); }
                resolve_type(smf->parameter_type);
                for (auto& d : smf->parameter_defaults) {
                    expand_expression(d);
                }
                push_scope();
                if (!smf->parameter_name.empty()) { declare_name(smf->parameter_name); }
                for (const std::string& pname : smf->parameter_names) { declare_name(pname); }

                expand_statement(smf->body.get());
                pop_scope();
            }
            return;
        }

        if (auto* fd = dynamic_cast<FunctionDefinition*>(node)) {
            func_defs_[fd->name] = fd;
            resolve_type(fd->return_type);
            for (Type& p : fd->parameters) { resolve_type(p); }
            for (auto& d : fd->param_defaults) {
                expand_expression(d);
            }
            push_scope();

            for (const std::string& pname : fd->param_names) {
                if (!pname.empty()) { declare_name(pname); }
            }

            expand_statement(fd->body.get());
            pop_scope();
            return;
        }

        if (auto* vd = dynamic_cast<VariableDeclaration*>(node)) {
            resolve_type(vd->type);
            if (vd->array_size_expr) { expand_expression(vd->array_size_expr); }
            expand_initializer(vd->initializer.get());
            return;
        }
        if (auto* ed = dynamic_cast<ExternDeclaration*>(node)) {
            resolve_type(ed->return_type);
            for (Type& p : ed->parameters) { resolve_type(p); }
            return;
        }
    }

    bool GenericExpander::expand(AST::Program* program) {
        if (program == nullptr) { return false; }
        program_ = program;
        had_error_ = false;
        push_scope();
        collect_declarations();
        collect_generics();
        std::vector<std::unique_ptr<TopLevel>> output;
        output.reserve(program_->top_levels.size());
        output_ = &output;
        for (auto& top : program_->top_levels) {
            if (dynamic_cast<GenericDefinition*>(top.get()) != nullptr) {
                continue;
            }
            if (auto* inst = dynamic_cast<InstantiationStatement*>(top.get())) {
                ensure_instantiation(inst->reference, inst->reference.member.empty());
                continue;
            }
            expand_top_level(top.get());
            output.push_back(std::move(top));
        }
        output_ = nullptr;
        pop_scope();
        program_->top_levels = std::move(output);
        return !had_error_;
    }

}
