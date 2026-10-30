#include "generic_expander.hpp"
#include "constant_folding.hpp"
#include <algorithm>
#include <cstdlib>
#include <cstdio>
#include <string>
#include <vector>

using namespace gallt::AST;

namespace gallt {

    namespace {

        ConstexprValue constant_substitution_value(bool is_string,
            const std::string& string_value, bool is_float, double float_value,
            long long int_value, const AST::Type* declared_type) {
            if (is_string) {
                ConstexprValue out = ConstexprValue::make_string(string_value);
                if (declared_type != nullptr) { out.type = *declared_type; }
                return out;
            }

            if (is_float) {
                const AST::Type type = declared_type != nullptr &&
                    declared_type->is_floating()
                    ? *declared_type : AST::Type::make_double();
                return ConstexprValue::make_floating(float_value, type);
            }

            const AST::Type type = declared_type != nullptr &&
                declared_type->is_integer()
                ? *declared_type : AST::Type::make_int();
            return ConstexprValue::make_integer(int_value, type);
        }

        int constexpr_arithmetic_position(const AST::Type& type) {
            switch (type.kind) {
            case AST::TypeKind::Char:
            case AST::TypeKind::Bool:
                return 0;
            case AST::TypeKind::Uchar:
                return 1;
            case AST::TypeKind::Int:
                return 2;
            case AST::TypeKind::Uint:
                return 3;
            case AST::TypeKind::Lint:
                return 4;
            case AST::TypeKind::Luint:
                return 5;
            case AST::TypeKind::Float:
                return 6;
            case AST::TypeKind::Double:
                return 7;
            default:
                return -1;
            }
        }

        int constexpr_conversion_rank(const AST::Type& from,
            const AST::Type& to) {
            AST::Type source = from;
            AST::Type target = to;
            source.is_const = false;
            target.is_const = false;

            if (source == target) { return 0; }

            const int source_position =
                constexpr_arithmetic_position(source);
            const int target_position =
                constexpr_arithmetic_position(target);

            if (target.kind == AST::TypeKind::String &&
                source_position >= 0) {
                return 200;
            }

            if (source_position < 0 || target_position < 0) {
                return -1;
            }

            const bool small_integer =
                source.kind == AST::TypeKind::Bool ||
                source.kind == AST::TypeKind::Char ||
                source.kind == AST::TypeKind::Uchar;
            const bool integral_promotion = small_integer &&
                (target.kind == AST::TypeKind::Int ||
                    target.kind == AST::TypeKind::Uint);
            const bool floating_promotion =
                source.kind == AST::TypeKind::Float &&
                target.kind == AST::TypeKind::Double;
            int distance = std::abs(target_position - source_position);
            if (distance == 0) { distance = 1; }
            return (integral_promotion || floating_promotion ? 100 : 200) +
                distance;
        }

    }

    const AST::FunctionDefinition* GenericExpander::find_constexpr_function(
        const std::string& name,
        const std::vector<ConstexprValue>& arguments) const {
        auto found = func_defs_.find(name);
        if (found == func_defs_.end()) {
            return nullptr;
        }

        struct Candidate {
            const AST::FunctionDefinition* function = nullptr;
            std::vector<int> ranks;
        };
        std::vector<Candidate> viable;

        for (const AST::FunctionDefinition* function : found->second) {
            if (function == nullptr || !function->is_constexpr_function ||
                function->is_variadic) {
                continue;
            }

            const std::size_t parameter_count = function->parameters.size();
            std::size_t required = parameter_count;
            for (std::size_t i = function->param_defaults.size();
                i > 0; --i) {
                if (function->param_defaults[i - 1] != nullptr) {
                    required = i - 1;
                } else {
                    break;
                }
            }

            if (arguments.size() < required ||
                arguments.size() > parameter_count) {
                continue;
            }

            Candidate candidate;
            candidate.function = function;
            bool viable_candidate = true;

            for (std::size_t i = 0; i < arguments.size(); ++i) {
                const int rank = constexpr_conversion_rank(
                    arguments[i].type, function->parameters[i]);
                if (rank < 0) {
                    viable_candidate = false;
                    break;
                }
                candidate.ranks.push_back(rank);
            }

            if (viable_candidate) {
                viable.push_back(std::move(candidate));
            }
        }

        int best = -1;
        bool ambiguous = false;

        for (std::size_t i = 0; i < viable.size(); ++i) {
            bool is_best = true;

            for (std::size_t j = 0; j < viable.size(); ++j) {
                if (i == j) { continue; }

                bool better = false;
                bool worse = false;
                const std::vector<int>& left = viable[i].ranks;
                const std::vector<int>& right = viable[j].ranks;
                const std::size_t count = std::min(left.size(), right.size());

                for (std::size_t k = 0; k < count; ++k) {
                    if (left[k] < right[k]) { better = true; }
                    if (left[k] > right[k]) { worse = true; }
                }

                if (!better || worse) {
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

        if (ambiguous || best < 0) { return nullptr; }
        return viable[static_cast<std::size_t>(best)].function;
    }

    ConstexprInterpreter GenericExpander::make_constexpr_interpreter(
        const Substitution& sub) const {
        ConstexprInterpreter::Host host;

        host.lookup_constant = [this, &sub](const std::string& name,
            ConstexprValue& out) {
            auto found = sub.constants.find(name);
            if (found == sub.constants.end()) {
                auto member = constant_member_values_.find(name);
                if (member == constant_member_values_.end()) { return false; }
                out = member->second;
                return true;
            }

            const AST::Type* declared = nullptr;
            auto type = sub.constant_types.find(name);
            if (type != sub.constant_types.end()) { declared = &type->second; }
            const ConstantValue& value = found->second;
            out = constant_substitution_value(value.is_string,
                value.string_value, value.is_float, value.float_value,
                value.int_value, declared);
            return true;
        };

        host.resolve_function = [this](const std::string& name,
            const std::vector<ConstexprValue>& arguments) {
            return find_constexpr_function(name, arguments);
        };

        host.layout_of_type = [this, &sub](const AST::Type& type,
            std::size_t& size, std::size_t& align) {
            return layout_of_composite_type(type, size, align, sub);
        };

        host.layout_of_name = [this, &sub](const std::string& name,
            std::size_t& size, std::size_t& align) {
            return resolve_type_layout(name, size, align, sub);
        };

        host.lookup_struct = [this](const std::string& name)
            -> const AST::StructDefinition* {
            auto found = struct_defs_.find(name);
            return found == struct_defs_.end() ? nullptr : found->second;
        };

        ConstexprInterpreter interpreter(std::move(host));
        interpreter.set_max_steps(constexpr_max_steps_);
        interpreter.set_max_integer_exponent(
            constexpr_max_integer_exponent_);
        return interpreter;
    }

    bool GenericExpander::constexpr_string_value(const AST::Expression* expr,
        const Substitution& sub, std::string& out) {
        if (expr == nullptr) { return false; }

        if (auto* primary = dynamic_cast<const PrimaryExpression*>(expr)) {
            if (primary->kind == PrimaryExpression::Kind::Literal &&
                primary->literal_token.type == TokenType::StringLiteral) {
                return decode_string_literal(primary->literal_token.lexeme, out);
            }

            if (primary->kind == PrimaryExpression::Kind::Identifier) {
                auto constant = sub.constants.find(primary->identifier);
                if (constant != sub.constants.end() && constant->second.is_string) {
                    out = constant->second.string_value;
                    return true;
                }
            }
        }

        if (auto* postfix = dynamic_cast<const PostfixExpression*>(expr)) {
            if (postfix->op == PostfixExpression::Operator::Dot &&
                postfix->member_name == "typename") {
                if (auto* base = dynamic_cast<const PrimaryExpression*>(
                    postfix->base.get())) {
                    if (base->kind == PrimaryExpression::Kind::Identifier) {
                        auto bound = sub.types.find(base->identifier);
                        if (bound != sub.types.end()) {
                            out = bound->second.to_source_string();
                            return true;
                        }
                        auto constant_type =
                            sub.constant_types.find(base->identifier);
                        if (constant_type != sub.constant_types.end() &&
                            constant_type->second.kind == TypeKind::String) {
                            auto constant =
                                sub.constants.find(base->identifier);
                            if (constant != sub.constants.end() &&
                                constant->second.is_string) {
                                out = constant->second.string_value;
                                return true;
                            }
                        }
                    }
                } else {
                    AST::Type receiver_type;
                    std::string receiver_name;
                    if (property_receiver_type(postfix->base.get(), sub,
                        receiver_type, receiver_name)) {
                        out = receiver_type.to_source_string();
                        return true;
                    }
                }
            }
        }

        if (auto* additive = dynamic_cast<const AdditiveExpression*>(expr)) {
            if (additive->op != AdditiveExpression::Operator::Plus) {
                return false;
            }

            std::string left;
            std::string right;
            if (!constexpr_string_value(additive->left.get(), sub, left) ||
                !constexpr_string_value(additive->right.get(), sub, right)) {
                return false;
            }

            out = left + right;
            return true;
        }

        if (auto* primary = dynamic_cast<const PrimaryExpression*>(expr)) {
            const bool generic_value =
                primary->kind == PrimaryExpression::Kind::QualifiedName ||
                (primary->kind == PrimaryExpression::Kind::Identifier &&
                    sub.members.count(primary->identifier) != 0);
            if (generic_value) {
                ConstexprValue interpreted;
                if (evaluate_constexpr_with_substitution(expr, sub, interpreted) &&
                    interpreted.type.kind == TypeKind::String) {
                    out = interpreted.string_value;
                    return true;
                }
            }
        }

        return false;
    }

    bool GenericExpander::constexpr_numeric_value(const AST::Expression* expr,
        const Substitution& sub, ConstantValue& out) {
        if (!evaluate_constant_with_members(expr, sub, out)) { return false; }
        if (out.is_string) { return false; }
        return true;
    }

    bool GenericExpander::constexpr_argument_value(const AST::Expression* expr,
        const Substitution& sub, const AST::Type& parameter_type,
        ConstexprValue& out) {
        AST::Type target = parameter_type;
        target.is_const = false;

        ConstexprValue raw;

        if (target.kind == TypeKind::String) {
            std::string text;
            if (!constexpr_string_value(expr, sub, text)) {
                std::string piece;
                if (!eval_emit_piece(expr, sub, piece)) { return false; }
                text = std::move(piece);
            }
            raw = ConstexprValue::make_string(std::move(text));
        } else if (target.kind == TypeKind::Array ||
            target.kind == TypeKind::Struct) {
            return false;
        } else {
            ConstantValue numeric;
            if (!constexpr_numeric_value(expr, sub, numeric)) { return false; }

            const AST::Type* declared = nullptr;
            if (auto* primary = dynamic_cast<const PrimaryExpression*>(expr)) {
                if (primary->kind == PrimaryExpression::Kind::Identifier) {
                    auto found = sub.constant_types.find(primary->identifier);
                    if (found != sub.constant_types.end()) {
                        declared = &found->second;
                    }
                }
            }

            raw = constant_substitution_value(numeric.is_string,
                numeric.string_value, numeric.is_float, numeric.float_value,
                numeric.int_value, declared);
        }

        return ConstexprInterpreter::coerce(raw, target, out);
    }

    bool GenericExpander::evaluate_constexpr_call(
        const AST::PostfixExpression* call, const Substitution& sub,
        ConstexprValue& out) {
        if (call == nullptr ||
            call->op != PostfixExpression::Operator::FunctionCall) {
            return false;
        }

        std::string name;
        if (auto* primary = dynamic_cast<const PrimaryExpression*>(call->base.get())) {
            if (primary->kind == PrimaryExpression::Kind::Identifier) {
                name = primary->identifier;
            }
        }

        if (name.empty()) { return false; }

        std::vector<ConstexprValue> arguments;
        arguments.reserve(call->arguments.size());

        for (const auto& argument : call->arguments) {
            ConstexprValue value;
            std::string text;
            if (constexpr_string_value(argument.get(), sub, text)) {
                value = ConstexprValue::make_string(std::move(text));
            } else {
                ConstantValue numeric;
                if (!constexpr_numeric_value(argument.get(), sub, numeric)) {
                    return false;
                }
                if (numeric.is_string) { return false; }

                AST::Type type = numeric.is_float
                    ? AST::Type::make_double() : AST::Type::make_int();

                if (auto* primary = dynamic_cast<const PrimaryExpression*>(
                    argument.get())) {
                    if (primary->kind == PrimaryExpression::Kind::Literal) {
                        switch (primary->literal_token.type) {
                        case TokenType::IntegerLiteral:
                            type = AST::Type::integer_literal_type(
                                primary->literal_token.lexeme);
                            break;
                        case TokenType::FloatLiteral:
                            type = AST::Type::float_literal_type(
                                primary->literal_token.lexeme);
                            break;
                        case TokenType::CharLiteral:
                            type = AST::Type::make_char();
                            break;
                        case TokenType::BoolLiteral:
                            type = AST::Type::make_bool();
                            break;
                        default:
                            break;
                        }
                    } else if (primary->kind ==
                        PrimaryExpression::Kind::Identifier) {
                        auto declared =
                            sub.constant_types.find(primary->identifier);
                        if (declared != sub.constant_types.end()) {
                            type = declared->second;
                        }
                    }
                }

                value = numeric.is_float
                    ? ConstexprValue::make_floating(
                        numeric.float_value, type)
                    : ConstexprValue::make_integer(
                        numeric.int_value, type);
            }
            arguments.push_back(std::move(value));
        }

        const AST::FunctionDefinition* function =
            find_constexpr_function(name, arguments);
        if (function == nullptr) { return false; }

        for (std::size_t i = 0; i < arguments.size(); ++i) {
            ConstexprValue converted;
            if (!constexpr_argument_value(call->arguments[i].get(), sub,
                function->parameters[i], converted)) {
                return false;
            }
            arguments[i] = std::move(converted);
        }

        for (std::size_t i = arguments.size();
            i < function->parameters.size(); ++i) {
            if (i >= function->param_defaults.size() ||
                function->param_defaults[i] == nullptr) {
                return false;
            }
            ConstexprValue value;
            if (!constexpr_argument_value(
                function->param_defaults[i].get(), sub,
                function->parameters[i], value)) {
                return false;
            }
            arguments.push_back(std::move(value));
        }

        ConstexprInterpreter interpreter = make_constexpr_interpreter(sub);
        return interpreter.call(function, arguments, out);
    }

    bool GenericExpander::evaluate_constexpr_with_substitution(
        const AST::Expression* expr, const Substitution& sub,
        ConstexprValue& out) {
        if (expr == nullptr) { return false; }
        std::unique_ptr<Expression> rewritten = clone_expression(expr, sub);
        drain_deferred_instantiations(true);
        if (rewritten == nullptr) { return false; }
        ConstexprInterpreter interpreter = make_constexpr_interpreter(sub);
        return interpreter.evaluate(rewritten.get(), out);
    }

    bool GenericExpander::evaluate_constant_with_members(
        const AST::Expression* expr, const Substitution& sub, ConstantValue& out) {
        if (expr == nullptr) { return false; }
        std::unique_ptr<Expression> rewritten = clone_expression(expr, sub);
        drain_deferred_instantiations(true);
        if (rewritten == nullptr) { return false; }

        ConstantEvaluationContext context;
        context.lookup_constant = [&](const std::string& name,
            long long& int_value, double& float_value, bool& is_float) {
            auto found = sub.constants.find(name);
            if (found != sub.constants.end()) {
                int_value = found->second.int_value;
                float_value = found->second.float_value;
                is_float = found->second.is_float;
                return true;
            }

            auto member = constant_member_values_.find(name);
            if (member == constant_member_values_.end() ||
                !member->second.type.is_arithmetic()) {
                return false;
            }

            is_float = member->second.type.is_floating();
            float_value = member->second.as_floating();
            int_value = member->second.as_integer();
            return true;
        };
        context.type_layout = [&](const std::string& name, std::size_t& size,
            std::size_t& align) {
            return resolve_type_layout(name, size, align, sub);
        };

        long long int_value = 0;
        double float_value = 0.0;
        bool is_float = false;
        if (!evaluate_constant_expression(rewritten.get(), int_value,
            float_value, is_float, context)) {
            return false;
        }

        out.int_value = int_value;
        out.float_value = float_value;
        out.is_float = is_float;
        return true;
    }

    bool GenericExpander::eval_constexpr_call(const AST::PostfixExpression* call,
        const Substitution& sub, std::string& out) {
        ConstexprValue value;
        if (!evaluate_constexpr_call(call, sub, value)) { return false; }

        switch (value.type.kind) {
        case TypeKind::String:
            out += value.string_value;
            return true;
        case TypeKind::Bool:
            out += value.int_value != 0 ? "true" : "false";
            return true;
        case TypeKind::Char:
        case TypeKind::Uchar:
            out.push_back(static_cast<char>(
                static_cast<unsigned char>(value.int_value)));
            return true;
        case TypeKind::Float:
        case TypeKind::Double: {
            char buffer[64];
            std::snprintf(buffer, sizeof(buffer), "%g", value.float_value);
            out += buffer;
            return true;
        }
        case TypeKind::Int:
        case TypeKind::Lint:
        case TypeKind::Uint:
        case TypeKind::Luint:
            out += std::to_string(value.int_value);
            return true;
        default:
            return false;
        }
    }

}
