#include "constant_folding.hpp"
#include <charconv>
#include <cmath>
#include <cstdlib>

using namespace gallt::AST;

namespace gallt {

    namespace {
        bool decode_char_literal(std::string_view lexeme, long long& out) {
            if (lexeme.size() < 3 || lexeme.front() != '\'' || lexeme.back() != '\'') {
                return false;
            }

            std::string_view inner = lexeme.substr(1, lexeme.size() - 2);
            if (inner.empty()) { return false; }

            if (inner[0] != '\\') {
                if (inner.size() != 1) { return false; }
                out = static_cast<unsigned char>(inner[0]);
                return true;
            }

            std::string_view body = inner.substr(1);
            if (body.empty()) { return false; }

            switch (body[0]) {
            case 'n': out = '\n'; return true;
            case 't': out = '\t'; return true;
            case 'r': out = '\r'; return true;
            case 'b': out = '\b'; return true;
            case 'f': out = '\f'; return true;
            case 'v': out = '\v'; return true;
            case '\\': out = '\\'; return true;
            case '"': out = '"'; return true;
            case '\'': out = '\''; return true;
            case 'x': {
                long long value = 0;
                int digits = 0;

                for (std::size_t i = 1; i < body.size() && digits < 2; ++i) {
                    const char c = body[i];
                    int digit = -1;
                    if (c >= '0' && c <= '9') {
                        digit = c - '0';
                    } else if (c >= 'a' && c <= 'f') {
                        digit = c - 'a' + 10;
                    } else if (c >= 'A' && c <= 'F') {
                        digit = c - 'A' + 10;
                    }
                    if (digit < 0) { break; }
                    value = value * 16 + digit;
                    ++digits;
                }

                if (digits == 0) { return false; }
                out = value;
                return true;
            }
            default: break;
            }

            if (body[0] >= '0' && body[0] <= '7') {
                long long value = 0;
                int digits = 0;

                for (std::size_t i = 0; i < body.size() && digits < 3; ++i) {
                    if (body[i] < '0' || body[i] > '7') { break; }
                    value = value * 8 + (body[i] - '0');
                    ++digits;
                }

                if (digits == 0) { return false; }
                out = value;
                return true;
            }
            return false;
        }

        bool builtin_layout(const std::string& name, std::size_t& size, std::size_t& align) {
            if (name == "int" || name == "uint" || name == "float") {
                size = 4; align = 4; return true;
            }
            if (name == "lint" || name == "luint" || name == "double") {
                size = 8; align = 8; return true;
            }
            if (name == "char" || name == "uchar" || name == "bool") {
                size = 1; align = 1; return true;
            }
            if (name == "string") { size = 32; align = 8; return true; }
            if (name == "file") { size = 8; align = 8; return true; }
            if (name == "void") { size = 0; align = 1; return true; }
            return false;
        }

        struct Value {
            bool valid = false;
            bool is_float = false;
            long long int_value = 0;
            double float_value = 0.0;
        };

        double as_double(const Value& v) {
            return v.is_float ? v.float_value : static_cast<double>(v.int_value);
        }

        Value make_int(long long value) {
            Value v;
            v.valid = true;
            v.is_float = false;
            v.int_value = value;
            return v;
        }

        Value make_float(double value) {
            Value v;
            v.valid = true;
            v.is_float = true;
            v.float_value = value;
            return v;
        }

        Value convert_to(const Value& v, const Type& target) {
            if (!v.valid) { return v; }

            switch (target.kind) {
            case TypeKind::Int:
                return make_int(v.is_float ? static_cast<long long>(v.float_value) : v.int_value);
            case TypeKind::Lint:
                return make_int(v.is_float ? static_cast<long long>(v.float_value) : v.int_value);
            case TypeKind::Uint:
                return make_int(static_cast<unsigned int>(v.is_float
                    ? static_cast<long long>(v.float_value) : v.int_value));
            case TypeKind::Luint:
                return make_int(static_cast<long long>(static_cast<unsigned long long>(
                    v.is_float ? static_cast<long long>(v.float_value) : v.int_value)));
            case TypeKind::Char:
                return make_int(static_cast<unsigned char>(v.is_float
                    ? static_cast<long long>(v.float_value) : v.int_value));
            case TypeKind::Uchar:
                return make_int(static_cast<unsigned char>(v.is_float
                    ? static_cast<long long>(v.float_value) : v.int_value));
            case TypeKind::Bool:
                return make_int((v.is_float ? (v.float_value != 0.0) : (v.int_value != 0)) ? 1 : 0);
            case TypeKind::Float:
                return make_float(static_cast<float>(as_double(v)));
            case TypeKind::Double:
                return make_float(as_double(v));
            default:
                return Value{};
            }
        }

        Value evaluate(const Expression* expr, const ConstantEvaluationContext& ctx) {
            if (expr == nullptr) { return Value{}; }

            if (auto* prim = dynamic_cast<const PrimaryExpression*>(expr)) {
                switch (prim->kind) {
                case PrimaryExpression::Kind::Parens:
                    return evaluate(prim->paren_expr.get(), ctx);
                case PrimaryExpression::Kind::Literal: {
                    const Token& tok = prim->literal_token;
                    if (tok.type == TokenType::IntegerLiteral) {
                        std::string_view lexeme = Type::strip_integer_suffix(tok.lexeme);
                        int base = 10;
                        if (lexeme.size() > 2 && lexeme[0] == '0' &&
                            (lexeme[1] == 'x' || lexeme[1] == 'X')) {
                            base = 16;
                            lexeme = lexeme.substr(2);
                        } else if (lexeme.size() > 1 && lexeme[0] == '0') {
                            base = 8;
                            lexeme = lexeme.substr(1);
                        }

                        long long value = 0;
                        auto [ptr, ec] = std::from_chars(lexeme.data(),
                            lexeme.data() + lexeme.size(), value, base);
                        if (ec != std::errc()) { return Value{}; }
                        return make_int(value);
                    }

                    if (tok.type == TokenType::FloatLiteral) {
                        std::string text(tok.lexeme);
                        if (!text.empty() && (text.back() == 'f' || text.back() == 'F')) {
                            text.pop_back();
                        }
                        char* end = nullptr;
                        double value = std::strtod(text.c_str(), &end);
                        if (end == nullptr || *end != '\0') { return Value{}; }
                        return make_float(value);
                    }

                    if (tok.type == TokenType::BoolLiteral) {
                        return make_int(tok.lexeme == "true" ? 1 : 0);
                    }

                    if (tok.type == TokenType::CharLiteral) {
                        long long value = 0;
                        if (!decode_char_literal(tok.lexeme, value)) { return Value{}; }
                        return make_int(value);
                    }

                    return Value{};
                }
                case PrimaryExpression::Kind::Identifier: {
                    if (ctx.lookup_constant) {
                        long long int_value = 0;
                        double float_value = 0.0;
                        bool is_float = false;
                        if (ctx.lookup_constant(prim->identifier, int_value, float_value, is_float)) {
                            return is_float ? make_float(float_value) : make_int(int_value);
                        }
                    }
                    return Value{};
                }
                default:
                    return Value{};
                }
            }

            if (auto* un = dynamic_cast<const UnaryExpression*>(expr)) {
                Value operand = evaluate(un->operand.get(), ctx);
                if (!operand.valid) { return Value{}; }

                switch (un->op) {
                case UnaryExpression::Operator::UnaryPlus:
                    return operand;
                case UnaryExpression::Operator::UnaryMinus:
                    return operand.is_float ? make_float(-operand.float_value)
                                            : make_int(-operand.int_value);
                case UnaryExpression::Operator::LogicalNot:
                    return make_int((operand.is_float ? (operand.float_value == 0.0)
                                                      : (operand.int_value == 0)) ? 1 : 0);
                case UnaryExpression::Operator::BitwiseNot:
                    if (operand.is_float) { return Value{}; }
                    return make_int(~operand.int_value);
                default:
                    return Value{};
                }
            }

            if (auto* cast = dynamic_cast<const PostfixExpression*>(expr)) {
                if (cast->op == PostfixExpression::Operator::Cast) {
                    return convert_to(evaluate(cast->base.get(), ctx), cast->cast_type);
                }

                if (cast->op == PostfixExpression::Operator::FunctionCall) {
                    auto* callee = dynamic_cast<const PrimaryExpression*>(cast->base.get());
                    if (callee != nullptr &&
                        callee->kind == PrimaryExpression::Kind::Identifier &&
                        cast->arguments.size() == 1) {
                        const bool is_size = callee->identifier == "size";
                        const bool is_align = callee->identifier == "align";

                        if ((is_size || is_align) && ctx.type_layout) {
                            const Expression* arg = cast->arguments[0].get();
                            std::string type_name;

                            if (auto* prim_arg = dynamic_cast<const PrimaryExpression*>(arg)) {
                                if (prim_arg->kind == PrimaryExpression::Kind::Identifier) {
                                    type_name = prim_arg->identifier;
                                }
                            }

                            if (!type_name.empty()) {
                                std::size_t size = 0;
                                std::size_t align = 1;

                                if (ctx.type_layout(type_name, size, align)) {
                                    return make_int(static_cast<long long>(
                                        is_size ? size : align));
                                }
                            }
                        }
                    }
                    return Value{};
                }

                return Value{};
            }

            if (auto* add = dynamic_cast<const AdditiveExpression*>(expr)) {
                Value l = evaluate(add->left.get(), ctx);
                Value r = evaluate(add->right.get(), ctx);
                if (!l.valid || !r.valid) { return Value{}; }
                bool use_float = l.is_float || r.is_float;
                if (add->op == AdditiveExpression::Operator::Plus) {
                    return use_float ? make_float(as_double(l) + as_double(r))
                                     : make_int(l.int_value + r.int_value);
                }
                return use_float ? make_float(as_double(l) - as_double(r))
                                 : make_int(l.int_value - r.int_value);
            }

            if (auto* mul = dynamic_cast<const MultiplicativeExpression*>(expr)) {
                Value l = evaluate(mul->left.get(), ctx);
                Value r = evaluate(mul->right.get(), ctx);
                if (!l.valid || !r.valid) { return Value{}; }
                bool use_float = l.is_float || r.is_float;

                switch (mul->op) {
                case MultiplicativeExpression::Operator::Multiply:
                    return use_float ? make_float(as_double(l) * as_double(r))
                                     : make_int(l.int_value * r.int_value);
                case MultiplicativeExpression::Operator::Divide:
                    if (use_float) {
                        if (as_double(r) == 0.0) { return Value{}; }
                        return make_float(as_double(l) / as_double(r));
                    }
                    if (r.int_value == 0) { return Value{}; }
                    return make_int(l.int_value / r.int_value);
                case MultiplicativeExpression::Operator::Remainder:
                    if (use_float || r.int_value == 0) { return Value{}; }
                    return make_int(l.int_value % r.int_value);
                }
                return Value{};
            }

            if (auto* power = dynamic_cast<const PowerExpression*>(expr)) {
                Value l = evaluate(power->left.get(), ctx);
                Value r = evaluate(power->right.get(), ctx);
                if (!l.valid || !r.valid) { return Value{}; }
                if (!l.is_float && !r.is_float) {
                    if (r.int_value < 0) { return Value{}; }
                    long long acc = 1;
                    for (long long i = 0; i < r.int_value; ++i) { acc *= l.int_value; }
                    return make_int(acc);
                }
                return make_float(std::pow(as_double(l), as_double(r)));
            }

            if (auto* cmp = dynamic_cast<const ComparisonExpression*>(expr)) {
                Value l = evaluate(cmp->left.get(), ctx);
                Value r = evaluate(cmp->right.get(), ctx);
                if (!l.valid || !r.valid) { return Value{}; }
                bool use_float = l.is_float || r.is_float;

                auto less = [&]() { return use_float ? as_double(l) < as_double(r)
                                                     : l.int_value < r.int_value; };
                auto equal = [&]() { return use_float ? as_double(l) == as_double(r)
                                                      : l.int_value == r.int_value; };

                switch (cmp->op) {
                case ComparisonExpression::Operator::Less: return make_int(less() ? 1 : 0);
                case ComparisonExpression::Operator::Greater: return make_int(less() ? 0 : (equal() ? 0 : 1));
                case ComparisonExpression::Operator::LessEqual:
                    return make_int((less() || equal()) ? 1 : 0);
                case ComparisonExpression::Operator::GreaterEqual:
                    return make_int((less() ? 0 : 1));
                case ComparisonExpression::Operator::Equal: return make_int(equal() ? 1 : 0);
                case ComparisonExpression::Operator::NotEqual: return make_int(equal() ? 0 : 1);
                }
                return Value{};
            }

            if (auto* bit = dynamic_cast<const BitwiseExpression*>(expr)) {
                Value l = evaluate(bit->left.get(), ctx);
                Value r = evaluate(bit->right.get(), ctx);
                if (!l.valid || !r.valid) { return Value{}; }
                if (l.is_float || r.is_float) { return Value{}; }

                switch (bit->op) {
                case BitwiseExpression::Operator::And:
                    return make_int(l.int_value & r.int_value);
                case BitwiseExpression::Operator::Xor:
                    return make_int(l.int_value ^ r.int_value);
                case BitwiseExpression::Operator::Or:
                    return make_int(l.int_value | r.int_value);
                }
                return Value{};
            }

            if (auto* shift = dynamic_cast<const ShiftExpression*>(expr)) {
                Value l = evaluate(shift->left.get(), ctx);
                Value r = evaluate(shift->right.get(), ctx);
                if (!l.valid || !r.valid) { return Value{}; }
                if (l.is_float || r.is_float) { return Value{}; }
                if (r.int_value < 0 || r.int_value >= 64) { return Value{}; }
                if (shift->op == ShiftExpression::Operator::Left) {
                    return make_int(l.int_value << r.int_value);
                }
                return make_int(l.int_value >> r.int_value);
            }

            if (auto* cond = dynamic_cast<const ConditionalExpression*>(expr)) {
                Value condition = evaluate(cond->condition.get(), ctx);
                if (!condition.valid) { return Value{}; }
                const bool taken = condition.is_float
                    ? (condition.float_value != 0.0)
                    : (condition.int_value != 0);
                return evaluate(taken ? cond->then_expr.get() : cond->else_expr.get(), ctx);
            }

            if (auto* land = dynamic_cast<const LogicalAndExpression*>(expr)) {
                Value l = evaluate(land->left.get(), ctx);
                Value r = evaluate(land->right.get(), ctx);
                if (!l.valid || !r.valid) { return Value{}; }
                bool lv = l.is_float ? (l.float_value != 0.0) : (l.int_value != 0);
                bool rv = r.is_float ? (r.float_value != 0.0) : (r.int_value != 0);
                return make_int((lv && rv) ? 1 : 0);
            }

            if (auto* lor = dynamic_cast<const LogicalOrExpression*>(expr)) {
                Value l = evaluate(lor->left.get(), ctx);
                Value r = evaluate(lor->right.get(), ctx);
                if (!l.valid || !r.valid) { return Value{}; }
                bool lv = l.is_float ? (l.float_value != 0.0) : (l.int_value != 0);
                bool rv = r.is_float ? (r.float_value != 0.0) : (r.int_value != 0);
                return make_int((lv || rv) ? 1 : 0);
            }

            return Value{};
        }
    }

    bool evaluate_constant_expression(const AST::Expression* expr, long long& int_out,
        double& float_out, bool& is_float_out, const ConstantEvaluationContext& context) {
        ConstantEvaluationContext ctx = context;

        if (!ctx.type_layout) {
            ctx.type_layout = [](const std::string& name, std::size_t& size,
                std::size_t& align) { return builtin_layout(name, size, align); };
        }

        Value value = evaluate(expr, ctx);
        if (!value.valid) { return false; }
        is_float_out = value.is_float;
        int_out = value.int_value;
        float_out = value.float_value;
        return true;
    }

}
