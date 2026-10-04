#include "generic_expander.hpp"
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

    }

    const AST::FunctionDefinition* GenericExpander::find_constexpr_function(
        const std::string& name, std::size_t arity) const {
        auto found = func_defs_.find(name);
        if (found == func_defs_.end() || found->second == nullptr) {
            return nullptr;
        }

        const AST::FunctionDefinition* function = found->second;
        if (!function->is_constexpr_function) { return nullptr; }
        if (function->parameters.size() != arity) { return nullptr; }
        return function;
    }

    ConstexprInterpreter GenericExpander::make_constexpr_interpreter(
        const Substitution& sub) const {
        ConstexprInterpreter::Host host;

        host.lookup_constant = [&sub](const std::string& name,
            ConstexprValue& out) {
            auto found = sub.constants.find(name);
            if (found == sub.constants.end()) { return false; }

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
            return find_constexpr_function(name, arguments.size());
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

        return ConstexprInterpreter(std::move(host));
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

        return false;
    }

    bool GenericExpander::constexpr_numeric_value(const AST::Expression* expr,
        const Substitution& sub, ConstantValue& out) {
        if (!evaluate_with_substitution(expr, sub, out)) { return false; }
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

        const AST::FunctionDefinition* function = find_constexpr_function(name,
            call->arguments.size());
        if (function == nullptr) { return false; }

        std::vector<ConstexprValue> arguments;
        arguments.reserve(call->arguments.size());

        for (std::size_t i = 0; i < call->arguments.size(); ++i) {
            ConstexprValue value;
            if (!constexpr_argument_value(call->arguments[i].get(), sub,
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
        ConstexprInterpreter interpreter = make_constexpr_interpreter(sub);
        return interpreter.evaluate(expr, out);
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
