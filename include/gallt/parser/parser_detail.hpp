#ifndef GALLT_PARSER_PARSER_DETAIL_HPP
#define GALLT_PARSER_PARSER_DETAIL_HPP

#include "../parser/parser.hpp"
#include "../semantic/constant_folding.hpp"
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <string>
#include <system_error>

namespace gallt {
    using namespace AST;

    namespace parser_detail {

        inline bool is_builtin_type_keyword(TokenType t) noexcept {
            switch (t) {
            case TokenType::Keyword_Int:
            case TokenType::Keyword_Lint:
            case TokenType::Keyword_Uint:
            case TokenType::Keyword_Luint:
            case TokenType::Keyword_Float:
            case TokenType::Keyword_Double:
            case TokenType::Keyword_Char:
            case TokenType::Keyword_Uchar:
            case TokenType::Keyword_Bool:
            case TokenType::Keyword_String:
            case TokenType::Keyword_File:
                return true;
            default:
                return false;
            }
        }

        inline bool is_builtin_or_void_type_keyword(TokenType t) noexcept {
            return is_builtin_type_keyword(t) || t == TokenType::Keyword_Void;
        }

        inline bool is_type_start_keyword(TokenType t) noexcept {
            return is_builtin_or_void_type_keyword(t) || t == TokenType::Keyword_Const;
        }

        inline std::string unquote_string(std::string_view lexeme) {
            if (lexeme.size() >= 2 && lexeme.front() == '"' && lexeme.back() == '"') {
                lexeme.remove_prefix(1);
                lexeme.remove_suffix(1);
            }

            return std::string(lexeme);
        }

        inline std::string compile_time_expr_text(const gallt::AST::Expression* expr) {
            using namespace gallt::AST;
            if (expr == nullptr) { return "<empty>"; }

            if (auto* prim = dynamic_cast<const PrimaryExpression*>(expr)) {
                switch (prim->kind) {
                case PrimaryExpression::Kind::Literal:
                    return std::string(prim->literal_token.lexeme);
                case PrimaryExpression::Kind::Identifier:
                    return prim->identifier;
                case PrimaryExpression::Kind::Parens:
                    return "(" + compile_time_expr_text(prim->paren_expr.get()) + ")";
                default:
                    return "<expression>";
                }
            }

            if (auto* un = dynamic_cast<const UnaryExpression*>(expr)) {
                const char* op = "?";

                switch (un->op) {
                case UnaryExpression::Operator::UnaryMinus: op = "-"; break;
                case UnaryExpression::Operator::UnaryPlus: op = "+"; break;
                case UnaryExpression::Operator::LogicalNot: op = "!"; break;
                case UnaryExpression::Operator::BitwiseNot: op = "~"; break;
                case UnaryExpression::Operator::AddressOf: op = "&"; break;
                case UnaryExpression::Operator::Dereference: op = "*"; break;
                default: break;
                }

                return std::string(op) + compile_time_expr_text(un->operand.get());
            }

            if (auto* bin = dynamic_cast<const AdditiveExpression*>(expr)) {
                const char* op = bin->op == AdditiveExpression::Operator::Plus ? "+" : "-";
                return compile_time_expr_text(bin->left.get()) + " " + op + " " +
                    compile_time_expr_text(bin->right.get());
            }

            if (auto* bin = dynamic_cast<const MultiplicativeExpression*>(expr)) {
                const char* op = "/";

                switch (bin->op) {
                case MultiplicativeExpression::Operator::Multiply: op = "*"; break;
                case MultiplicativeExpression::Operator::Divide: op = "/"; break;
                case MultiplicativeExpression::Operator::Remainder: op = "%"; break;
                }

                return compile_time_expr_text(bin->left.get()) + " " + op + " " +
                    compile_time_expr_text(bin->right.get());
            }

            if (auto* bin = dynamic_cast<const ShiftExpression*>(expr)) {
                const char* op = bin->op == ShiftExpression::Operator::Left ? "<<" : ">>";
                return compile_time_expr_text(bin->left.get()) + " " + op + " " +
                    compile_time_expr_text(bin->right.get());
            }

            if (auto* bin = dynamic_cast<const BitwiseExpression*>(expr)) {
                const char* op = "&";

                switch (bin->op) {
                case BitwiseExpression::Operator::And: op = "&"; break;
                case BitwiseExpression::Operator::Xor: op = "^"; break;
                case BitwiseExpression::Operator::Or: op = "|"; break;
                }

                return compile_time_expr_text(bin->left.get()) + " " + op + " " +
                    compile_time_expr_text(bin->right.get());
            }

            if (auto* cmp = dynamic_cast<const ComparisonExpression*>(expr)) {
                const char* op = "==";

                switch (cmp->op) {
                case ComparisonExpression::Operator::Greater: op = ">"; break;
                case ComparisonExpression::Operator::Less: op = "<"; break;
                case ComparisonExpression::Operator::Equal: op = "=="; break;
                case ComparisonExpression::Operator::NotEqual: op = "!="; break;
                case ComparisonExpression::Operator::GreaterEqual: op = ">="; break;
                case ComparisonExpression::Operator::LessEqual: op = "<="; break;
                }

                return compile_time_expr_text(cmp->left.get()) + " " + op + " " +
                    compile_time_expr_text(cmp->right.get());
            }

            if (auto* cond = dynamic_cast<const ConditionalExpression*>(expr)) {
                return compile_time_expr_text(cond->condition.get()) + " ? " +
                    compile_time_expr_text(cond->then_expr.get()) + " : " +
                    compile_time_expr_text(cond->else_expr.get());
            }

            if (auto* call = dynamic_cast<const PostfixExpression*>(expr)) {
                if (call->op == PostfixExpression::Operator::FunctionCall) {
                    return compile_time_expr_text(call->base.get()) + "(...)";
                }

                if (call->op == PostfixExpression::Operator::Cast) {
                    return call->cast_type.to_string() + "(" +
                        compile_time_expr_text(call->base.get()) + ")";
                }
            }

            return "<expression>";
        }

        inline bool fold_constant_expression(const gallt::AST::Expression* expr,
            long long& int_out, double& float_out, bool& is_float_out,
            const std::function<bool(const std::string&, std::size_t&, std::size_t&)>*
                resolve_type_size = nullptr);

        struct ConstValue {
            bool valid = false;
            bool is_float = false;
            long long int_value = 0;
            double float_value = 0.0;
            std::string text;
        };

        inline bool decode_char_literal(std::string_view lexeme, long long& out) {
            if (lexeme.size() < 3 || lexeme.front() != '\'' || lexeme.back() != '\'') {
                return false;
            }

            std::string_view inner = lexeme.substr(1, lexeme.size() - 2);
            if (inner.empty()) { return false; }

            if (inner[0] != '\\') {
                out = static_cast<unsigned char>(inner[0]);
                return true;
            }

            switch (inner.size() > 1 ? inner[1] : '\0') {
            case 'n': out = '\n'; return true;
            case 't': out = '\t'; return true;
            case 'r': out = '\r'; return true;
            case 'b': out = '\b'; return true;
            case 'f': out = '\f'; return true;
            case 'v': out = '\v'; return true;
            case '0': out = '\0'; return true;
            case '\\': out = '\\'; return true;
            case '"': out = '"'; return true;
            case '\'': out = '\''; return true;
            default: return false;
            }
        }

        inline bool fold_constant_expression(const Expression* expr, long long& int_out,
            double& float_out, bool& is_float_out,
            const std::function<bool(const std::string&, std::size_t&, std::size_t&)>*
                resolve_type_size) {
            gallt::ConstantEvaluationContext context;

            if (resolve_type_size != nullptr) {
                context.type_layout = *resolve_type_size;
            }

            return gallt::evaluate_constant_expression(expr, int_out, float_out,
                is_float_out, context);
        }

    }
}

#endif
