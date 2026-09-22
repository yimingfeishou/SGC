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

    std::unique_ptr<Expression> GenericExpander::expand_expression_call(
        const FunctionDefinition* owner, const Substitution& sub,
        const std::string& name,
        std::vector<std::unique_ptr<Expression>>& arguments,
        SourceLocation loc, bool& ok) {
        (void)owner;
        ok = false;
        auto found = sub.expressions.find(name);
        if (found == sub.expressions.end()) {
            return nullptr;
        }
        const Substitution::ExpressionBinding& binding = found->second;
        if (binding.body == nullptr) {
            return nullptr;
        }
        if (arguments.size() != binding.parameter_types.size()) {
            report(loc, ErrorCode::ExprParameterCallArgCountMismatch,
                std::vector<std::string>{ name,
                    std::to_string(binding.parameter_types.size()),
                    std::to_string(arguments.size()) });
            return nullptr;
        }
        if (expression_expansion_depth_ >= kMaxExpressionExpansionDepth) {
            report(loc, ErrorCode::ExprParameterRecursionLimitExceeded,
                std::vector<std::string>{ name });
            return nullptr;
        }
        if (pending_hoisted_ == nullptr) {
            report(loc, ErrorCode::ExprParameterDisallowedSyntax,
                std::vector<std::string>{ name });
            return nullptr;
        }
        Substitution body_sub = sub;
        body_sub.renames.clear();
        body_sub.expr_arguments.clear();
        body_sub.expr_argument_types.clear();
        for (std::size_t i = 0; i < arguments.size(); ++i) {
            expr_argument_storage_.push_back(std::move(arguments[i]));
        }
        std::size_t first_new = expr_argument_storage_.size() - arguments.size();
        for (std::size_t i = 0; i < binding.parameter_types.size(); ++i) {
            if (i >= binding.parameter_names.size() ||
                binding.parameter_names[i].empty()) {
                continue;
            }
            body_sub.expr_arguments[binding.parameter_names[i]] =
                expr_argument_storage_[first_new + i].get();
            body_sub.expr_argument_types[binding.parameter_names[i]] =
                binding.parameter_types[i];
            expression_parameter_index_[binding.parameter_names[i]] = i;
        }
        std::unordered_set<std::string> locals;
        for (const auto& stmt : binding.body->statements) {
            collect_declared_names(stmt.get(), locals);
        }
        const std::string suffix = "$expr" + std::to_string(++expr_temp_counter_);
        for (const std::string& local : locals) {
            body_sub.renames[local] = local + suffix;
        }
        const bool void_result = binding.return_type.kind == TypeKind::Void;
        std::string temp_name;
        if (!void_result) {
            temp_name = "__glt_expr" + std::to_string(expr_temp_counter_);
        }
        std::vector<std::unique_ptr<Statement>> emitted;
        const bool outer_in_expression_body = in_expression_body_;
        in_expression_body_ = true;
        ++expression_expansion_depth_;
        bool body_ok = emit_body_into_temp(binding.body->statements, body_sub,
            temp_name, binding.return_type, emitted);
        --expression_expansion_depth_;
        in_expression_body_ = outer_in_expression_body;
        if (!body_ok) {
            report(binding.body->location, ErrorCode::ExprParameterDisallowedSyntax,
                std::vector<std::string>{ name });
            return nullptr;
        }
        ExpressionCallSite call_site;
        call_site.name = name;
        call_site.declared_return_type = binding.return_type;
        call_site.declared_parameter_types = binding.parameter_types;
        call_site.location = loc;
        call_site.depth = expression_expansion_depth_;
        if (void_result) {
            for (auto& stmt : emitted) {
                pending_hoisted_->push_back(std::move(stmt));
            }
            ok = true;
            lexeme_pool_.push_back("0");
            Token placeholder(TokenType::IntegerLiteral, loc,
                std::string_view(lexeme_pool_.back()));
            auto node = std::make_unique<PrimaryExpression>(loc, placeholder);
            expression_call_sites_[node.get()] = call_site;
            return node;
        }
        auto declaration = std::make_unique<VariableDeclaration>(loc,
            binding.return_type, temp_name, std::nullopt, std::nullopt, nullptr);
        pending_hoisted_->push_back(std::move(declaration));
        for (auto& stmt : emitted) {
            pending_hoisted_->push_back(std::move(stmt));
        }
        ok = true;
        auto node = std::make_unique<PrimaryExpression>(loc, temp_name);
        expression_call_sites_[node.get()] = call_site;
        return node;
    }

    bool GenericExpander::substitute_type(const Type& in, const Substitution& sub, Type& out) {
        switch (in.kind) {
        case TypeKind::Array: {
            Type elem = Type::make_void();
            bool ok = in.element_type ? substitute_type(*in.element_type, sub, elem) : false;
            out = Type::make_array(std::make_shared<Type>(std::move(elem)), in.array_size);
            return ok;
        }
        case TypeKind::Pointer: {
            Type pointee = Type::make_void();
            bool ok = in.pointee_type ? substitute_type(*in.pointee_type, sub, pointee) : false;
            out = Type::make_pointer(std::make_shared<Type>(std::move(pointee)));
            return ok;
        }
        case TypeKind::Function: {
            Type ret = Type::make_void();
            bool ok = in.return_type ? substitute_type(*in.return_type, sub, ret) : true;
            std::vector<Type> params;
            for (const Type& p : in.parameter_types) {
                Type substituted = Type::make_void();
                if (!substitute_type(p, sub, substituted)) ok = false;
                params.push_back(std::move(substituted));
            }
            out = Type::make_function(std::make_shared<Type>(std::move(ret)), params);
            return ok;
        }
        case TypeKind::Struct: {
            if (in.generic_ref) {
                GenericRef ref = *in.generic_ref;
                for (GenericArgument& arg : ref.arguments) {
                    if (arg.is_type) {
                        Type substituted = Type::make_void();
                        if (!substitute_type(arg.type, sub, substituted)) return false;
                        arg.type = std::move(substituted);
                        arg.text = arg.type.to_string();
                    }
                }
                auto mangled = ensure_instantiation(ref, false);
                if (!mangled.has_value()) return false;
                out = Type::make_struct(*mangled);
                return true;
            }
            auto found = sub.types.find(in.struct_name);
            if (found != sub.types.end()) {
                out = found->second;
                return true;
            }
            out = in;
            return true;
        }
        default:
            out = in;
            return true;
        }
    }

    void GenericExpander::resolve_type(Type& type) {
        switch (type.kind) {
        case TypeKind::Array:
            if (type.element_type) resolve_type(*type.element_type);
            break;
        case TypeKind::Pointer:
            if (type.pointee_type) resolve_type(*type.pointee_type);
            break;
        case TypeKind::Function:
            if (type.return_type) resolve_type(*type.return_type);
            for (Type& p : type.parameter_types) resolve_type(p);
            break;
        case TypeKind::Struct: {
            if (type.generic_ref) {
                GenericRef ref = *type.generic_ref;
                if (ref.member.empty()) {
                    auto instance_key = ensure_instantiation(ref, true);
                    if (!instance_key.has_value()) {
                        type.generic_ref.reset();
                        break;
                    }
                    auto members = instance_struct_members_.find(*instance_key);
                    if (members != instance_struct_members_.end() && members->second.size() == 1) {
                        ref.member = members->second.front();
                    }
                    else {
                        report(type.generic_ref->location, ErrorCode::GenericMemberNotInInstantiation, std::vector<std::string>{ ref.generic_name, ref.generic_name });
                        type.generic_ref.reset();
                        break;
                    }
                }
                auto mangled = ensure_instantiation(ref, false);
                if (mangled.has_value()) {
                    type.struct_name = *mangled;
                }
                type.generic_ref.reset();
                break;
            }
            for (auto it = short_scopes_.rbegin(); it != short_scopes_.rend(); ++it) {
                auto found = it->find(type.struct_name);
                if (found != it->end()) {
                    if (found->second.is_type) {
                        type.struct_name = found->second.target;
                    }
                    break;
                }
            }
            break;
        }
        default:
            break;
        }
    }

    bool GenericExpander::resolve_constant_argument(const GenericArgument& arg,
        const Substitution* sub, ConstantValue& out) {
        if (arg.is_type) return false;
        if (arg.constant_actual_type.kind == TypeKind::String) {
            if (arg.is_string_constant) {
                out.is_string = true;
                out.is_float = false;
                out.string_value = arg.string_constant;
                return true;
            }
            if (arg.expression != nullptr) {
                if (auto* prim = dynamic_cast<const PrimaryExpression*>(arg.expression.get())) {
                    if (prim->kind == PrimaryExpression::Kind::Literal &&
                        prim->literal_token.type == TokenType::StringLiteral) {
                        out.is_string = true;
                        out.is_float = false;
                        if (!decode_string_literal(prim->literal_token.lexeme,
                            out.string_value)) {
                            return false;
                        }
                        return true;
                    }
                }
            }
            return false;
        }
        if (arg.expression != nullptr) {
            Substitution empty;
            const Substitution& env = sub != nullptr ? *sub : empty;
            if (evaluate_with_substitution(arg.expression.get(), env, out)) {
                return true;
            }
            return false;
        }
        if (arg.float_constant) {
            out.is_float = true;
            out.float_value = arg.float_value;
            return true;
        }
        if (!arg.text.empty() && sub != nullptr) {
            auto found = sub->constants.find(arg.text);
            if (found != sub->constants.end()) {
                out = found->second;
                return true;
            }
        }
        out.is_float = false;
        out.int_value = arg.int_value;
        return true;
    }

    bool GenericExpander::evaluate_with_substitution(const AST::Expression* expr,
        const Substitution& sub, ConstantValue& out) {
        ConstantEvaluationContext context;
        context.lookup_constant = [&](const std::string& name, long long& int_value,
            double& float_value, bool& is_float) {
            auto found = sub.constants.find(name);
            if (found == sub.constants.end()) return false;
            int_value = found->second.int_value;
            float_value = found->second.float_value;
            is_float = found->second.is_float;
            return true;
        };
        context.type_layout = [&](const std::string& name, std::size_t& size,
            std::size_t& align) { return resolve_type_layout(name, size, align, sub); };
        long long int_value = 0;
        double float_value = 0.0;
        bool is_float = false;
        if (!evaluate_constant_expression(expr, int_value, float_value, is_float, context)) {
            return false;
        }
        out.int_value = int_value;
        out.float_value = float_value;
        out.is_float = is_float;
        return true;
    }

    bool GenericExpander::resolve_type_layout(const std::string& type_name, std::size_t& size,
        std::size_t& align, const Substitution& sub) const {
        if (type_name == "int" || type_name == "uint" || type_name == "float") {
            size = 4; align = 4; return true;
        }
        if (type_name == "lint" || type_name == "luint" || type_name == "double") {
            size = 8; align = 8; return true;
        }
        if (type_name == "char" || type_name == "uchar" || type_name == "bool") {
            size = 1; align = 1; return true;
        }
        if (type_name == "string") { size = 32; align = 8; return true; }
        if (type_name == "file") { size = 8; align = 8; return true; }
        auto bound = sub.types.find(type_name);
        if (bound != sub.types.end()) {
            return resolve_type_layout(bound->second.struct_name, size, align, sub) ||
                resolve_type_layout(bound->second.to_string(), size, align, sub);
        }
        auto it = struct_defs_.find(type_name);
        if (it == struct_defs_.end() || it->second == nullptr) return false;
        auto round_up = [](std::size_t value, std::size_t alignment) {
            if (alignment <= 1) return value;
            return (value + alignment - 1) / alignment * alignment;
        };
        std::function<bool(const AST::Type&, std::size_t&, std::size_t&)> type_layout =
            [&](const AST::Type& type, std::size_t& out_size, std::size_t& out_align) -> bool {
            switch (type.kind) {
            case TypeKind::Int: case TypeKind::Float: out_size = 4; out_align = 4; return true;
            case TypeKind::Double: out_size = 8; out_align = 8; return true;
            case TypeKind::Char: case TypeKind::Bool: out_size = 1; out_align = 1; return true;
            case TypeKind::String: out_size = 32; out_align = 8; return true;
            case TypeKind::File: out_size = 8; out_align = 8; return true;
            case TypeKind::Pointer:
            case TypeKind::Function: out_size = 8; out_align = 8; return true;
            case TypeKind::Array: {
                std::size_t element_size = 0;
                std::size_t element_align = 1;
                if (!type.element_type ||
                    !type_layout(*type.element_type, element_size, element_align)) {
                    return false;
                }
                out_size = element_size * type.array_size.value_or(0);
                out_align = element_align;
                return true;
            }
            case TypeKind::Struct: {
                auto def = struct_defs_.find(type.struct_name);
                if (def == struct_defs_.end()) return false;
                std::size_t offset = 0;
                std::size_t max_align = 1;
                for (const auto& member : def->second->members) {
                    std::size_t member_size = 0;
                    std::size_t member_align = 1;
                    if (!type_layout(member.type, member_size, member_align)) return false;
                    max_align = std::max(max_align, member_align);
                    offset = round_up(offset, member_align);
                    offset += member_size;
                }
                out_size = round_up(offset, max_align);
                out_align = max_align;
                return true;
            }
            default:
                return false;
            }
        };
        return type_layout(AST::Type::make_struct(type_name), size, align);
    }

    std::unique_ptr<Expression> GenericExpander::make_typed_constant_literal(SourceLocation loc,
        const ConstantValue& value, const AST::Type& type) {
        if (value.is_float || type.kind == TypeKind::Float || type.kind == TypeKind::Double) {
            return make_constant_literal(loc, value);
        }
        return make_constant_literal(loc, value);
    }

    std::vector<GenericPatternArg> GenericExpander::parameters_as_free_patterns(
        GenericDefinition* def) const {
        std::vector<GenericPatternArg> patterns;
        for (const GenericParameter& param : def->parameters) {
            GenericPatternArg pattern;
            if (param.is_type) {
                pattern.is_constant = false;
                pattern.type = Type::make_struct(param.name);
                pattern.text = param.name;
            }
            else {
                pattern.is_constant = true;
                pattern.free_constant = true;
                pattern.text = param.name;
            }
            patterns.push_back(std::move(pattern));
        }
        return patterns;
    }

    std::string GenericExpander::pattern_signature(const GenericPatternArg& pattern) const {
        std::function<std::string(const Type&)> type_sig = [&](const Type& t) -> std::string {
            switch (t.kind) {
            case TypeKind::Struct:
                return is_free_identifier(t.struct_name, struct_defs_)
                    ? std::string("F") : ("S:" + t.struct_name);
            case TypeKind::Pointer:
                return "P(" + (t.pointee_type ? type_sig(*t.pointee_type) : std::string("?")) + ")";
            case TypeKind::Array:
                return "A(" + (t.element_type ? type_sig(*t.element_type) : std::string("?")) +
                    (t.array_size.has_value() ? "," + std::to_string(*t.array_size) : "") + ")";
            default:
                return t.to_string();
            }
        };
        if (pattern.is_constant) {
            if (pattern.free_constant) return "FC";
            if (pattern.string_constant) return "CS:" + pattern.string_value;
            if (pattern.float_constant) {
                char buffer[64];
                std::snprintf(buffer, sizeof(buffer), "%g", pattern.float_value);
                return std::string("C:") + buffer;
            }
            return "C:" + std::to_string(pattern.int_value);
        }
        return type_sig(pattern.type);
    }

    bool GenericExpander::duplicate_pattern_signature(const GenericEntry& entry,
        GenericDefinition* def) const {
        auto same_signature = [&](GenericDefinition* other) {
            if (other == def) return false;
            if (other->patterns.size() != def->patterns.size()) return false;
            for (std::size_t i = 0; i < def->patterns.size(); ++i) {
                if (pattern_signature(other->patterns[i]) != pattern_signature(def->patterns[i])) {
                    return false;
                }
            }
            return true;
        };
        if (entry.primary != nullptr && same_signature(entry.primary)) return true;
        for (GenericDefinition* spec : entry.specializations) {
            if (same_signature(spec)) return true;
        }
        return false;
    }

    GenericExpander::PatternOrder GenericExpander::compare_pattern_type(const Type& a,
        const Type& b) const {
        bool a_free = (a.kind == TypeKind::Struct &&
            is_free_identifier(a.struct_name, struct_defs_));
        bool b_free = (b.kind == TypeKind::Struct &&
            is_free_identifier(b.struct_name, struct_defs_));
        if (a_free && b_free) return PatternOrder::Equal;
        if (a_free) return PatternOrder::Worse;
        if (b_free) return PatternOrder::Better;
        bool a_composite = (a.kind == TypeKind::Pointer || a.kind == TypeKind::Array);
        bool b_composite = (b.kind == TypeKind::Pointer || b.kind == TypeKind::Array);
        if (!a_composite && !b_composite) {
            return PatternOrder::Equal;
        }
        if (a_composite && !b_composite) return PatternOrder::Worse;
        if (!a_composite && b_composite) return PatternOrder::Better;
        if (a.kind != b.kind) return PatternOrder::Incomparable;
        if (a.kind == TypeKind::Pointer) {
            if (!a.pointee_type || !b.pointee_type) return PatternOrder::Incomparable;
            return compare_pattern_type(*a.pointee_type, *b.pointee_type);
        }
        if (!a.element_type || !b.element_type) return PatternOrder::Incomparable;
        if (a.array_size.has_value() != b.array_size.has_value()) {
            return PatternOrder::Incomparable;
        }
        if (a.array_size.has_value() && *a.array_size != *b.array_size) {
            return PatternOrder::Incomparable;
        }
        return compare_pattern_type(*a.element_type, *b.element_type);
    }

    GenericExpander::PatternOrder GenericExpander::compare_pattern(
        const GenericPatternArg& a, const GenericPatternArg& b) const {
        if (a.is_constant && b.is_constant) {
            if (a.free_constant && b.free_constant) return PatternOrder::Equal;
            if (a.free_constant) return PatternOrder::Worse;
            if (b.free_constant) return PatternOrder::Better;
            if (a.string_constant || b.string_constant) {
                if (a.string_constant != b.string_constant) {
                    return PatternOrder::Incomparable;
                }
                return (a.string_value == b.string_value)
                    ? PatternOrder::Equal : PatternOrder::Incomparable;
            }
            if (a.float_constant != b.float_constant) return PatternOrder::Incomparable;
            bool same = a.float_constant ? (a.float_value == b.float_value)
                                         : (a.int_value == b.int_value);
            return same ? PatternOrder::Equal : PatternOrder::Equal;
        }
        if (a.is_constant || b.is_constant) return PatternOrder::Incomparable;
        return compare_pattern_type(a.type, b.type);
    }

    bool GenericExpander::at_least_as_specialized(GenericDefinition* a,
        GenericDefinition* b) const {
        if (a->patterns.size() != b->patterns.size()) return false;
        for (std::size_t i = 0; i < a->patterns.size(); ++i) {
            if (compare_pattern(a->patterns[i], b->patterns[i]) == PatternOrder::Worse) {
                return false;
            }
        }
        return true;
    }

    bool GenericExpander::more_specialized(GenericDefinition* a,
        GenericDefinition* b) const {
        return at_least_as_specialized(a, b) && !at_least_as_specialized(b, a);
    }

    GenericExpander::MatchResult GenericExpander::match_specialization(
        GenericDefinition* def, const GenericRef& ref) {
        MatchResult result;
        if (def->patterns.size() != ref.arguments.size()) return result;

        std::function<bool(const Type&, const Type&)> match;
        match = [&](const Type& pattern, const Type& actual) -> bool {
            switch (pattern.kind) {
            case TypeKind::Struct:
                if (is_free_identifier(pattern.struct_name, struct_defs_)) {
                    auto found = result.substitution.types.find(pattern.struct_name);
                    if (found == result.substitution.types.end()) {
                        result.substitution.types[pattern.struct_name] = actual;
                    }
                    else if (!(found->second == actual)) {
                        report(def->location, ErrorCode::GenericPatternDeductionMismatch, std::vector<std::string>{ pattern.struct_name });
                        return false;
                    }
                    return true;
                }
                return pattern.struct_name == actual.struct_name && actual.kind == TypeKind::Struct;
            case TypeKind::Pointer:
                if (actual.kind != TypeKind::Pointer || !actual.pointee_type || !pattern.pointee_type) {
                    return false;
                }
                return match(*pattern.pointee_type, *actual.pointee_type);
            case TypeKind::Array:
                if (actual.kind != TypeKind::Array || !actual.element_type || !pattern.element_type) {
                    return false;
                }
                if (pattern.array_size.has_value() && actual.array_size.has_value() &&
                    pattern.array_size.value() != actual.array_size.value()) {
                    return false;
                }
                return match(*pattern.element_type, *actual.element_type);
            default:
                return pattern == actual;
            }
        };

        for (std::size_t i = 0; i < def->patterns.size(); ++i) {
            const GenericPatternArg& pattern = def->patterns[i];
            const GenericArgument& arg = ref.arguments[i];
            if (pattern.is_constant) {
                if (arg.is_type) return MatchResult{};
                if (pattern.free_constant) continue;
                ConstantValue value;
                if (!resolve_constant_argument(arg, nullptr, value)) return MatchResult{};
                if (pattern.string_constant) {
                    if (!value.is_string) return MatchResult{};
                    if (pattern.string_value != value.string_value) return MatchResult{};
                    continue;
                }
                if (value.is_string) return MatchResult{};
                if (pattern.float_constant != value.is_float) return MatchResult{};
                if (pattern.float_constant) {
                    if (pattern.float_value != value.float_value) return MatchResult{};
                }
                else if (pattern.int_value != value.int_value) {
                    return MatchResult{};
                }
            }
            else {
                if (!arg.is_type) return MatchResult{};
                if (!match(pattern.type, arg.type)) return MatchResult{};
            }
        }
        result.matched = true;
        return result;
    }

    bool GenericExpander::constraint_satisfied(const GenericConstraint& constraint,
        const Type& arg) {
        switch (constraint.kind) {
        case GenericConstraint::Kind::Any:
            return true;
        case GenericConstraint::Kind::Struct:
            return arg.kind == TypeKind::Struct;
        case GenericConstraint::Kind::Pointer:
            return arg.kind == TypeKind::Pointer;
        case GenericConstraint::Kind::Array:
            return arg.kind == TypeKind::Array;
        case GenericConstraint::Kind::File:
            return arg.kind == TypeKind::File;
        case GenericConstraint::Kind::Types:
            for (const Type& allowed : constraint.types) {
                if (allowed.kind == TypeKind::Struct && arg.kind == TypeKind::Struct) {
                    Type resolved = allowed;
                    if (resolved.generic_ref) {
                        resolve_type(resolved);
                    }
                    if (resolved.struct_name == arg.struct_name) return true;
                    continue;
                }
                if (allowed == arg) return true;
            }
            return false;
        }
        return false;
    }

}
