#include "../parser/parser.hpp"
#include "parser_detail.hpp"
#include "../semantic/constant_folding.hpp"
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <string>
#include <system_error>

using namespace gallt::AST;

namespace gallt {
    using namespace parser_detail;

    std::unique_ptr<Statement> Parser::parse_generic_compile_time_item() {
        if (current_.type == TokenType::Keyword_Emit) {
            ++generic_ct_depth_;
            auto stmt = parse_emit_statement(true);
            --generic_ct_depth_;
            return stmt;
        }

        if (current_.type == TokenType::Keyword_If) {
            ++generic_ct_depth_;
            auto stmt = parse_if_statement();
            --generic_ct_depth_;
            return stmt;
        }

        return nullptr;
    }

    bool Parser::generic_param_list_is_primary_shaped() const {
        if (current_.type != TokenType::Less) { return true; }
        std::size_t i = 1;
        if (lookahead_type(i) == TokenType::Greater) {
            return true;
        }

        for (;;) {
            TokenType first = lookahead_type(i);
            TokenType second = lookahead_type(i + 1);
            bool first_ident = first == TokenType::Identifier;
            bool first_type_keyword = is_type_start_keyword(first);

            if (first_ident) {
                if (lookahead(i).lexeme == "expr" &&
                   (second == TokenType::Identifier ||
                       second == TokenType::LeftParen)) {
                    i += 1;
                    if (lookahead_type(i) == TokenType::Identifier) {
                        ++i;
                    }

                    if (lookahead_type(i) == TokenType::LeftParen) {
                        int depth = 0;

                        for (;;) {
                            TokenType t = lookahead_type(i);
                            if (t == TokenType::EndOfFile) { return false; }
                            if (t == TokenType::LeftParen) { ++depth; ++i; continue; }
                            if (t == TokenType::RightParen) {
                                --depth;
                                ++i;
                                if (depth == 0) { break; }
                                continue;
                            }
                            ++i;
                        }
                    }

                    if (lookahead_type(i) != TokenType::Arrow) {
                        return false;
                    }
                    ++i;

                    if (is_type_start_keyword(lookahead_type(i)) ||
                       lookahead_type(i) == TokenType::Identifier) {
                        ++i;
                        while (lookahead_type(i) == TokenType::ColonColon &&
                           lookahead_type(i + 1) == TokenType::Identifier) {
                            i += 2;
                        }
                    } else {
                        return false;
                    }

                    while (true) {
                        TokenType t = lookahead_type(i);
                        if (t == TokenType::Star || t == TokenType::Power ||
                           t == TokenType::Keyword_Const) {
                            ++i;
                            continue;
                        }
                        break;
                    }
                } else if (second == TokenType::Identifier) {
                    i += 2;
                } else if (second == TokenType::Ellipsis) {
                    i += 2;

                    if (lookahead_type(i) == TokenType::Identifier) {
                        ++i;
                    } else if (lookahead_type(i) == TokenType::Colon) {
                        i += 1;
                        int depth = 0;

                        for (;;) {
                            TokenType t = lookahead_type(i);
                            if (t == TokenType::EndOfFile) { return false; }
                            if (t == TokenType::Less) { ++depth; ++i; continue; }
                            if (t == TokenType::Greater) {
                                if (depth == 0) { break; }
                                --depth; ++i; continue;
                            }
                            if (t == TokenType::Comma && depth == 0) { break; }
                            ++i;
                        }
                    }
                } else if (second == TokenType::Colon) {
                    i += 2;
                    int depth = 0;

                    for (;;) {
                        TokenType t = lookahead_type(i);
                        if (t == TokenType::EndOfFile) { return false; }
                        if (t == TokenType::Less) { ++depth; ++i; continue; }
                        if (t == TokenType::Greater) {
                            if (depth == 0) { break; }
                            --depth; ++i; continue;
                        }
                        if (t == TokenType::Comma && depth == 0) { break; }
                        ++i;
                    }
                } else {
                    TokenType after_param = lookahead_type(i + 1);

                    if (after_param != TokenType::Comma && after_param != TokenType::Greater) {
                        return false;
                    }

                    i += 1;
                }
            } else if (first_type_keyword && second == TokenType::Identifier) {
                i += 2;
            } else if (first_type_keyword && second == TokenType::Ellipsis) {
                i += 2;
                if (lookahead_type(i) != TokenType::Identifier) { return false; }
                ++i;
            } else {
                return false;
            }
            TokenType separator = lookahead_type(i);
            if (separator == TokenType::Comma) {
                ++i;
                continue;
            }

            if (separator == TokenType::Greater) {
                return true;
            }

            return false;
        }
    }

    std::size_t Parser::scan_generic_instantiation_end() const {
        if (current_.type != TokenType::Identifier) { return std::string::npos; }
        std::size_t i = 1;
        while (lookahead_type(i) == TokenType::ColonColon &&
            lookahead_type(i + 1) == TokenType::Identifier) {
            i += 2;
        }
        if (lookahead_type(i) != TokenType::Less) { return std::string::npos; }

        int depth = 0;
        for (;; ++i) {
            TokenType t = lookahead_type(i);
            switch (t) {
            case TokenType::Less:
                ++depth;
                break;
            case TokenType::Greater:
                --depth;
                if (depth == 0) {
                    return i + 1;
                }
                break;
            case TokenType::Identifier:
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
            case TokenType::Keyword_Void:
            case TokenType::Keyword_Const:
            case TokenType::IntegerLiteral:
            case TokenType::FloatLiteral:
            case TokenType::CharLiteral:
            case TokenType::BoolLiteral:
            case TokenType::StringLiteral:
            case TokenType::Star:
            case TokenType::Comma:
            case TokenType::LeftBracket:
            case TokenType::RightBracket:
            case TokenType::LeftParen:
            case TokenType::RightParen:
            case TokenType::Plus:
            case TokenType::Minus:
            case TokenType::Slash:
            case TokenType::Percent:
            case TokenType::Power:
            case TokenType::Pipe:
            case TokenType::Caret:
            case TokenType::Tilde:
            case TokenType::Question:
            case TokenType::Colon:
            case TokenType::Dot:
            case TokenType::ColonColon:
            case TokenType::Ellipsis:
                break;

            default:
                return std::string::npos;
            }
        }
    }

    std::size_t Parser::scan_expr_argument_instantiation_end() const {
        if (current_.type != TokenType::Identifier) { return std::string::npos; }
        std::size_t i = 1;
        while (lookahead_type(i) == TokenType::ColonColon &&
            lookahead_type(i + 1) == TokenType::Identifier) {
            i += 2;
        }
        if (lookahead_type(i) != TokenType::Less) { return std::string::npos; }

        int angle = 0;
        int paren = 0;
        int brace = 0;
        bool saw_expr_argument = false;
        for (;; ++i) {
            TokenType t = lookahead_type(i);
            if (t == TokenType::EndOfFile) {
                return std::string::npos;
            }
            if (t == TokenType::Identifier && lookahead(i).lexeme == "expr" &&
                (lookahead_type(i + 1) == TokenType::Identifier ||
                    lookahead_type(i + 1) == TokenType::LeftBrace ||
                    lookahead_type(i + 1) == TokenType::LeftParen)) {
                saw_expr_argument = true;
            }
            switch (t) {
            case TokenType::Less:
                ++angle;
                break;
            case TokenType::Greater:
                if (paren == 0 && brace == 0) {
                    --angle;
                    if (angle == 0) {
                        return saw_expr_argument ? i + 1 : std::string::npos;
                    }
                }
                break;
            case TokenType::LeftParen:
                ++paren;
                break;
            case TokenType::RightParen:
                if (paren == 0) { return std::string::npos; }
                --paren;
                break;
            case TokenType::LeftBrace:
                ++brace;
                break;
            case TokenType::RightBrace:
                if (brace == 0) { return std::string::npos; }
                --brace;
                break;
            case TokenType::Unknown:
                return std::string::npos;
            default:
                break;
            }
        }
    }

    bool Parser::looks_like_generic_instantiation() const {
        std::size_t after = scan_expr_argument_instantiation_end();
        if (after == std::string::npos) {
            after = scan_generic_instantiation_end();
        }
        if (after == std::string::npos) { return false; }
        if (lookahead_type(after) == TokenType::Power) {
            return declaration_name_after_pointer_suffix(after);
        }
        switch (lookahead_type(after)) {
        case TokenType::Dot:
        case TokenType::ColonColon:
        case TokenType::Identifier:
        case TokenType::Star:
        case TokenType::Semicolon:
        case TokenType::Newline:
        case TokenType::RightBrace:
        case TokenType::EndOfFile:
        case TokenType::Comma:
        case TokenType::RightParen:
        case TokenType::LeftBracket:
        case TokenType::LeftBrace:
            return true;
        default:
            return false;
        }
    }

    std::unique_ptr<Expression> Parser::parse_compile_time_expression() {
        auto parse_primary_ct = [&]() -> std::unique_ptr<Expression> {
            SourceLocation loc = current_location();
            switch (current_.type) {
            case TokenType::IntegerLiteral:
            case TokenType::FloatLiteral:
            case TokenType::CharLiteral:
            case TokenType::BoolLiteral:
            case TokenType::StringLiteral: {
                Token lit = current_;
                advance();
                return std::make_unique<PrimaryExpression>(loc, lit);
            }

            case TokenType::LeftParen: {
                advance();
                auto inner = parse_compile_time_expression();
                if (!expect(TokenType::RightParen, "expected ')' in compile-time expression")) {
                    return nullptr;
                }

                return std::make_unique<PrimaryExpression>(loc, std::move(inner));
            }

            case TokenType::Identifier: {
                std::string id(current_.lexeme);

                if ((id == "size" || id == "align") &&
                    lookahead_type(1) == TokenType::LeftParen) {
                    advance();
                    advance();
                    std::unique_ptr<Expression> arg;
                    TokenType arg_start = current_.type;
                    bool type_name_argument = (is_builtin_type_keyword(arg_start) ||
                        (arg_start == TokenType::Identifier &&
                            lookahead_type(1) == TokenType::RightParen));

                    if (type_name_argument) {
                        std::string type_text(current_.lexeme);
                        advance();
                        arg = std::make_unique<PrimaryExpression>(loc, type_text);
                    } else {
                        arg = parse_compile_time_expression();
                    }

                    if (arg == nullptr) { return nullptr; }

                    if (!expect(TokenType::RightParen,
                        "expected ')' after size/align argument")) {
                        return nullptr;
                    }
                    auto call = std::make_unique<PostfixExpression>(
                        loc, std::make_unique<PrimaryExpression>(loc, id),
                        PostfixExpression::Operator::FunctionCall);
                    call->arguments.push_back(std::move(arg));
                    return call;
                }

                advance();
                return std::make_unique<PrimaryExpression>(loc, id);
            }
            case TokenType::Keyword_Int:
            case TokenType::Keyword_Lint:
            case TokenType::Keyword_Uint:
            case TokenType::Keyword_Luint:
            case TokenType::Keyword_Float:
            case TokenType::Keyword_Double:
            case TokenType::Keyword_Char:
            case TokenType::Keyword_Uchar:
            case TokenType::Keyword_Bool:
            case TokenType::Keyword_String: {
                Type cast_type = parse_type_specifier(false);

                if (current_.type != TokenType::LeftParen) {
                    report_error(ErrorCode::ExpressionSyntaxError,
                        "expected '(' after type name in compile-time cast");
                    return nullptr;
                }

                advance();
                auto operand = parse_compile_time_expression();
                if (operand == nullptr) { return nullptr; }

                if (!expect(TokenType::RightParen, "expected ')' after compile-time cast")) {
                    return nullptr;
                }

                return std::make_unique<PostfixExpression>(loc, std::move(operand),
                    PostfixExpression::Operator::Cast,
                    nullptr, std::vector<std::unique_ptr<Expression>>{},
                    std::move(cast_type));
            }
            default:
                report_error(ErrorCode::GenericNonTypeArgNotConstant,
                    "expected compile-time constant expression");
                return nullptr;
            }
        };
        std::function<std::unique_ptr<Expression>()> parse_unary_ct =
            [&]() -> std::unique_ptr<Expression> {
            SourceLocation loc = current_location();
            if (current_.type == TokenType::Minus || current_.type == TokenType::Plus ||
                current_.type == TokenType::LogicalNot ||
                current_.type == TokenType::Tilde) {
                UnaryExpression::Operator op = UnaryExpression::Operator::UnaryPlus;
                if (current_.type == TokenType::Minus) {
                    op = UnaryExpression::Operator::UnaryMinus;
                } else if (current_.type == TokenType::LogicalNot) {
                    op = UnaryExpression::Operator::LogicalNot;
                } else if (current_.type == TokenType::Tilde) {
                    op = UnaryExpression::Operator::BitwiseNot;
                }
                advance();
                auto operand = parse_unary_ct();
                if (!operand) { return nullptr; }

                return std::make_unique<UnaryExpression>(loc, op, std::move(operand));
            }

            return parse_primary_ct();
        };
        std::function<std::unique_ptr<Expression>()> parse_mul =
            [&]() -> std::unique_ptr<Expression> {
            auto left = parse_unary_ct();
            if (!left) { return nullptr; }

            while (current_.type == TokenType::Star || current_.type == TokenType::Slash ||
                current_.type == TokenType::Percent) {
                SourceLocation loc = current_location();
                MultiplicativeExpression::Operator op = MultiplicativeExpression::Operator::Multiply;
                if (current_.type == TokenType::Slash) {
                    op = MultiplicativeExpression::Operator::Divide;
                } else if (current_.type == TokenType::Percent) {
                    op = MultiplicativeExpression::Operator::Remainder;
                }
                advance();
                auto right = parse_unary_ct();
                if (!right) { return nullptr; }
                left = std::make_unique<MultiplicativeExpression>(loc, std::move(left), op, std::move(right));
            }

            return left;
        };
        std::function<std::unique_ptr<Expression>()> parse_power_ct =
            [&]() -> std::unique_ptr<Expression> {
            auto base = parse_mul();
            if (!base) { return nullptr; }

            if (current_.type == TokenType::Power) {
                SourceLocation loc = current_location();
                advance();
                auto right = parse_power_ct();
                if (!right) { return nullptr; }
                return std::make_unique<PowerExpression>(loc, std::move(base), std::move(right));
            }

            return base;
        };
        std::function<std::unique_ptr<Expression>()> parse_add_ct =
            [&]() -> std::unique_ptr<Expression> {
            auto value = parse_power_ct();
            if (!value) { return nullptr; }

            while (current_.type == TokenType::Plus || current_.type == TokenType::Minus) {
                SourceLocation loc = current_location();
                AdditiveExpression::Operator op = current_.type == TokenType::Plus
                    ? AdditiveExpression::Operator::Plus
                    : AdditiveExpression::Operator::Minus;
                advance();
                auto right = parse_power_ct();
                if (!right) { return nullptr; }
                value = std::make_unique<AdditiveExpression>(loc, std::move(value), op,
                    std::move(right));
            }

            return value;
        };
        auto greater_is_comparison = [&]() -> bool {
            if (current_.type != TokenType::Greater) { return false; }
            switch (lookahead_type(1)) {
            case TokenType::IntegerLiteral:
            case TokenType::FloatLiteral:
            case TokenType::CharLiteral:
            case TokenType::StringLiteral:
            case TokenType::BoolLiteral:
            case TokenType::Identifier:
            case TokenType::LeftParen:
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
                return true;
            default:
                return false;
            }
        };
        auto constant_operand_start = [&](TokenType t) -> bool {
            switch (t) {
            case TokenType::IntegerLiteral:
            case TokenType::FloatLiteral:
            case TokenType::CharLiteral:
            case TokenType::StringLiteral:
            case TokenType::BoolLiteral:
            case TokenType::Identifier:
            case TokenType::LeftParen:
            case TokenType::Minus:
            case TokenType::Plus:
            case TokenType::LogicalNot:
            case TokenType::Tilde:
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
                return true;
            default:
                return false;
            }
        };

        auto shift_operator_here = [&]() -> bool {
            TokenType head = current_.type;
            if (head != TokenType::Less && head != TokenType::Greater) { return false; }
            if (lookahead_type(1) != head) { return false; }
            TokenType third = lookahead_type(2);
            if (third == TokenType::Assign || third == TokenType::LessEqual ||
                third == TokenType::GreaterEqual) {
                return false;
            }

            return constant_operand_start(lookahead_type(2));
        };
        std::function<std::unique_ptr<Expression>()> parse_shift_ct =
            [&]() -> std::unique_ptr<Expression> {
            auto value = parse_add_ct();
            if (!value) { return nullptr; }

            while (shift_operator_here()) {
                SourceLocation loc = current_location();
                const bool is_left_shift = current_.type == TokenType::Less;
                advance();
                advance();
                auto right = parse_add_ct();
                if (!right) { return nullptr; }
                value = std::make_unique<ShiftExpression>(loc, std::move(value),
                    is_left_shift ? ShiftExpression::Operator::Left
                                  : ShiftExpression::Operator::Right,
                    std::move(right));
            }

            return value;
        };
        std::function<std::unique_ptr<Expression>()> parse_comparison_ct =
            [&]() -> std::unique_ptr<Expression> {
            auto value = parse_shift_ct();
            if (!value) { return nullptr; }

            for (;;) {
                ComparisonExpression::Operator op;
                if (current_.type == TokenType::Less &&
                    (lookahead_type(1) == TokenType::Less ||
                        lookahead_type(1) == TokenType::LessEqual)) {
                    return value;
                }
                if (current_.type == TokenType::Greater &&
                    (lookahead_type(1) == TokenType::Greater ||
                        lookahead_type(1) == TokenType::GreaterEqual)) {
                    return value;
                }
                switch (current_.type) {
                case TokenType::Less: op = ComparisonExpression::Operator::Less; break;
                case TokenType::LessEqual: op = ComparisonExpression::Operator::LessEqual; break;
                case TokenType::Equal: op = ComparisonExpression::Operator::Equal; break;
                case TokenType::NotEqual: op = ComparisonExpression::Operator::NotEqual; break;
                case TokenType::GreaterEqual:
                    op = ComparisonExpression::Operator::GreaterEqual; break;
                case TokenType::Greater:
                    if (!greater_is_comparison()) { return value; }
                    op = ComparisonExpression::Operator::Greater;
                    break;
                default:
                    return value;
                }
                SourceLocation loc = current_location();
                advance();
                auto right = parse_shift_ct();
                if (!right) { return nullptr; }
                value = std::make_unique<ComparisonExpression>(loc, std::move(value), op,
                    std::move(right));
            }
        };
        std::function<std::unique_ptr<Expression>()> parse_bitwise_ct =
            [&]() -> std::unique_ptr<Expression> {
            auto value = parse_comparison_ct();
            if (!value) { return nullptr; }

            while (current_.type == TokenType::AddressOf ||
                current_.type == TokenType::Caret ||
                current_.type == TokenType::Pipe) {
                SourceLocation loc = current_location();
                BitwiseExpression::Operator op = BitwiseExpression::Operator::And;
                if (current_.type == TokenType::Caret) {
                    op = BitwiseExpression::Operator::Xor;
                } else if (current_.type == TokenType::Pipe) {
                    op = BitwiseExpression::Operator::Or;
                }
                advance();
                auto right = parse_comparison_ct();
                if (!right) { return nullptr; }
                value = std::make_unique<BitwiseExpression>(loc, std::move(value), op,
                    std::move(right));
            }

            return value;
        };
        std::function<std::unique_ptr<Expression>()> parse_logical_and_ct =
            [&]() -> std::unique_ptr<Expression> {
            auto value = parse_bitwise_ct();
            if (!value) { return nullptr; }

            while (current_.type == TokenType::LogicalAnd) {
                SourceLocation loc = current_location();
                advance();
                auto right = parse_bitwise_ct();
                if (!right) { return nullptr; }
                value = std::make_unique<LogicalAndExpression>(loc, std::move(value),
                    std::move(right));
            }

            return value;
        };
        auto value = parse_logical_and_ct();
        if (!value) { return nullptr; }

        while (current_.type == TokenType::LogicalOr) {
            SourceLocation loc = current_location();
            advance();
            auto right = parse_logical_and_ct();
            if (!right) { return nullptr; }
            value = std::make_unique<LogicalOrExpression>(loc, std::move(value),
                std::move(right));
        }

        if (current_.type != TokenType::Question) {
            return value;
        }

        SourceLocation cond_loc = current_location();
        advance();
        auto then_expr = parse_compile_time_expression();
        if (then_expr == nullptr) { return nullptr; }

        if (!expect(TokenType::Colon,
            "expected ':' in compile-time conditional expression")) {
            return nullptr;
        }

        auto else_expr = parse_compile_time_expression();
        if (else_expr == nullptr) { return nullptr; }
        return std::make_unique<ConditionalExpression>(cond_loc, std::move(value),
            std::move(then_expr), std::move(else_expr));
    }

    std::vector<GenericArgument> Parser::parse_generic_arguments() {
        std::vector<GenericArgument> args;
        if (!expect(TokenType::Less, "expected '<' to start generic argument list")) {
            return args;
        }
        if (current_.type == TokenType::Greater) {
            advance();
            return args;
        }
        do {
            GenericArgument arg;
            bool parsed_type = false;
            TokenType tt = current_.type;
            if (tt == TokenType::Identifier && current_.lexeme == "expr" &&
                (lookahead_type(1) == TokenType::Identifier ||
                    lookahead_type(1) == TokenType::LeftBrace)) {
                advance();
                arg.is_type = false;
                arg.is_expr = true;
                arg.constant_actual_type = Type::make_void();
                if (current_.type == TokenType::Identifier) {
                    arg.expr_name = std::string(current_.lexeme);
                    advance();
                }

                if (current_.type == TokenType::LeftParen) {
                    advance();
                    std::vector<std::unique_ptr<Expression>> defaults;
                    bool expr_variadic = false;
                    auto [types, names] = parse_parameter_list(&defaults,
                        &expr_variadic);
                    arg.expr_param_types = std::move(types);
                    arg.expr_param_names = std::move(names);

                    if (expr_variadic) {
                        report_error_template(ErrorCode::ExpressionParameterVariadicNotAllowed,
                            { arg.expr_name });
                    }

                    for (const std::unique_ptr<Expression>& default_value : defaults) {
                        if (default_value != nullptr) {
                            report_error_template(
                                ErrorCode::ExprParameterDisallowedSyntax,
                                { arg.expr_name });
                            break;
                        }
                    }

                    if (!expect(TokenType::RightParen,
                        "expected ')' after expression parameter list")) {
                        return args;
                    }
                } else {
                    arg.expr_shorthand = true;
                }

                if (current_.type != TokenType::Arrow) {
                    report_error(ErrorCode::ExpressionSyntaxError,
                        "expected '->' and a return type in expression argument");
                    return args;
                }
                advance();
                arg.expr_return_type = parse_type(true, false);
                std::size_t body_start = mark();
                bool block_form = false;

                if (current_.type == TokenType::Assign) {
                    advance();
                } else if (current_.type == TokenType::LeftBrace) {
                    block_form = true;
                    advance();
                } else {
                    report_error(ErrorCode::ExpressionSyntaxError,
                        "expected '=' or '{' in expression argument");
                    return args;
                }

                auto body = std::make_shared<ExpressionParameterBody>();
                body->location = current_location();
                const bool outer_expr_argument = in_expr_argument_;
                in_expr_argument_ = !block_form;

                if (block_form) {
                    while (current_.type != TokenType::RightBrace &&
                        current_.type != TokenType::EndOfFile) {
                        skip_newlines();
                        if (current_.type == TokenType::RightBrace) { break; }

                        auto stmt = parse_statement();

                        if (stmt != nullptr) {
                            body->statements.push_back(std::move(stmt));
                        } else {
                            synchronize();
                        }
                    }
                    if (!expect(TokenType::RightBrace,
                        "expected '}' to close expression argument block")) {
                        return args;
                    }
                } else {
                    while (true) {
                        skip_newlines();
                        auto stmt = parse_statement();

                        if (stmt != nullptr) {
                            body->statements.push_back(std::move(stmt));
                        } else {
                            break;
                        }
                        skip_newlines();

                        if (current_.type == TokenType::Comma ||
                            current_.type == TokenType::Greater ||
                            current_.type == TokenType::EndOfFile) {
                            break;
                        }

                        if (current_.type == TokenType::Semicolon) {
                            advance();
                            skip_newlines();

                            if (current_.type == TokenType::Comma ||
                                current_.type == TokenType::Greater) {
                                break;
                            }

                            if (current_.type == TokenType::RightBrace ||
                                current_.type == TokenType::EndOfFile) {
                                break;
                            }
                        }
                    }

                    while (current_.type == TokenType::Semicolon) {
                        advance();
                        skip_newlines();
                    }
                }

                in_expr_argument_ = outer_expr_argument;
                skip_newlines();

                if (!block_form && !body->statements.empty()) {
                    Statement* last = body->statements.back().get();
                    if (auto* expr_stmt = dynamic_cast<ExpressionStatement*>(last)) {
                        auto ret = std::make_unique<ReturnStatement>(
                            expr_stmt->location, std::move(expr_stmt->expr));
                        body->statements.pop_back();
                        body->statements.push_back(std::move(ret));
                    }
                }

                arg.expr_body = body;
                arg.text = expression_argument_text(body_start, mark());
                args.push_back(std::move(arg));

                if (current_.type == TokenType::Comma) {
                    advance();
                    skip_newlines();
                    continue;
                }

                break;
            }
            bool type_start = (is_builtin_or_void_type_keyword(tt) ||
                tt == TokenType::Identifier);

            if (tt == TokenType::Identifier &&
                lookahead_type(1) == TokenType::Ellipsis) {
                arg.is_type = false;
                arg.is_pack_expansion = true;
                arg.pack_name = std::string(current_.lexeme);
                arg.text = arg.pack_name + "...";
                advance();
                advance();
                args.push_back(std::move(arg));

                if (current_.type == TokenType::Comma) {
                    advance();
                    skip_newlines();
                    continue;
                }

                break;
            }

            if (type_start) {
                std::size_t m = mark();
                Type t = parse_type(true, false);

                while (current_.type == TokenType::LeftBracket) {
                    advance();
                    std::optional<std::size_t> size = std::nullopt;

                    if (current_.type == TokenType::IntegerLiteral) {
                        size = try_parse_integer_literal();
                        advance();
                    }

                    if (!expect(TokenType::RightBracket, "expected ']' in array type argument")) {
                        break;
                    }

                    t = Type::make_array(std::make_shared<Type>(std::move(t)), size);
                }

                if ((current_.type == TokenType::Comma || current_.type == TokenType::Greater) &&
                    !t.to_string().empty()) {
                    arg.is_type = true;
                    arg.type = std::move(t);
                    arg.text = arg.type.to_string();
                    parsed_type = true;
                } else {
                    reset_to(m);
                }
            }

            if (!parsed_type) {
                auto expr = parse_compile_time_expression();
                arg.is_type = false;

                if (expr) {
                    arg.expression = std::shared_ptr<Expression>(expr.release());
                    if (auto* prim = dynamic_cast<PrimaryExpression*>(arg.expression.get())) {
                        if (prim->kind == PrimaryExpression::Kind::Literal &&
                            prim->literal_token.type == TokenType::StringLiteral) {
                            arg.constant_actual_type = Type::make_string();
                            arg.is_string_constant = true;
                            arg.string_constant = unquote_string(prim->literal_token.lexeme);
                            arg.text = arg.normalize();
                        }
                    }

                    long long iv = 0;
                    double dv = 0.0;
                    bool is_flt = false;
                    bool ok = fold_constant_expression(arg.expression.get(), iv, dv, is_flt);

                    if (ok) {
                        if (is_flt) {
                            arg.float_constant = true;
                            arg.float_value = dv;
                        } else {
                            arg.int_value = iv;
                        }
                        arg.text = arg.normalize();
                    } else if (arg.is_string_constant) {
                        arg.text = arg.normalize();
                    } else if (auto* prim = dynamic_cast<PrimaryExpression*>(arg.expression.get())) {
                        if (prim->kind == PrimaryExpression::Kind::Identifier) {
                            arg.text = prim->identifier;
                        } else {
                            arg.text = compile_time_expr_text(arg.expression.get());
                        }
                    } else {
                        arg.text = compile_time_expr_text(arg.expression.get());
                    }

                    if (current_.type != TokenType::Comma && current_.type != TokenType::Greater) {
                        report_error_template(ErrorCode::GenericNonTypeArgNotConstant,
                            { arg.text });
                    }
                }
            }

            args.push_back(std::move(arg));

            if (current_.type == TokenType::Comma) {
                advance();
                skip_newlines();
                continue;
            }

            break;
        } while (true);
        expect(TokenType::Greater, "expected '>' to close generic argument list");
        return args;
    }

    GenericRef Parser::parse_generic_reference(std::string_view name) {
        GenericRef ref;
        ref.location = current_location();
        ref.generic_name = std::string(name);
        advance();

        while (current_.type == TokenType::ColonColon &&
            lookahead_type(1) == TokenType::Identifier) {
            advance();
            ref.namespace_path.push_back(ref.generic_name);
            ref.generic_name = std::string(current_.lexeme);
            advance();

            if (current_.type == TokenType::Less) { break; }
        }

        ref.arguments = parse_generic_arguments();

        if (current_.type == TokenType::Dot || current_.type == TokenType::ColonColon) {
            ref.member_scope_access = (current_.type == TokenType::ColonColon);
            std::string_view separator =
                ref.member_scope_access ? std::string_view("::") : std::string_view(".");
            advance();

            if (current_.type == TokenType::Identifier) {
                ref.member = std::string(current_.lexeme);
                advance();
            } else {
                report_error(ErrorCode::ExpressionSyntaxError,
                    "expected member identifier after '" + std::string(separator) +
                    "' in generic reference");
            }
        }

        return ref;
    }

    GenericConstraint Parser::parse_generic_constraint() {
        GenericConstraint constraint;
        if (current_.type == TokenType::Identifier && current_.lexeme == "any") {
            constraint.kind = GenericConstraint::Kind::Any;
            constraint.text = "any";
            advance();
            return constraint;
        }

        if (current_.type == TokenType::Keyword_Struct) {
            constraint.kind = GenericConstraint::Kind::Struct;
            constraint.text = "struct";
            advance();
            return constraint;
        }

        if (current_.type == TokenType::Identifier && current_.lexeme == "pointer") {
            constraint.kind = GenericConstraint::Kind::Pointer;
            constraint.text = "pointer";
            advance();
            return constraint;
        }

        if (current_.type == TokenType::Identifier && current_.lexeme == "array") {
            constraint.kind = GenericConstraint::Kind::Array;
            constraint.text = "array";
            advance();
            return constraint;
        }

        if (current_.type == TokenType::Keyword_File) {
            constraint.kind = GenericConstraint::Kind::File;
            constraint.text = "file";
            advance();
            return constraint;
        }

        constraint.kind = GenericConstraint::Kind::Types;
        do {
            Type t = parse_type_specifier(false);
            constraint.text += t.to_string();
            constraint.types.push_back(std::move(t));

            if (current_.type == TokenType::Pipe) {
                constraint.text += " | ";
                advance();
                continue;
            }

            break;
        } while (true);

        return constraint;
    }

    std::unique_ptr<TopLevel> Parser::parse_generic_definition() {
        SourceLocation loc = current_location();
        expect(TokenType::Keyword_Generics, "expected 'generics'");
        std::string name;
        if (!check_identifier_name(name, "generic name after 'generics'")) {
            return nullptr;
        }
        bool primary_shaped = generic_param_list_is_primary_shaped();
        if (!expect(TokenType::Less, "expected '<' after generic name")) {
            return nullptr;
        }

        bool is_specialization = seen_generics_.count(name) != 0 || !primary_shaped;
        auto def = std::make_unique<GenericDefinition>(loc, name);
        def->is_specialization = is_specialization;
        def->primary_shaped = primary_shaped;
        seen_generics_.insert(name);

        if (current_.type != TokenType::Greater) {
            do {
                if (!is_specialization) {
                    GenericParameter param;
                    param.location = current_location();
                    if (current_.type == TokenType::Identifier &&
                        current_.lexeme == "expr" &&
                        lookahead_type(1) == TokenType::Identifier) {
                        advance();
                        param.is_type = false;
                        param.is_expr = true;
                        param.name = std::string(current_.lexeme);
                        advance();
                        if (current_.type == TokenType::LeftParen) {
                            advance();
                            std::vector<std::unique_ptr<Expression>> defaults;
                            bool expr_variadic = false;
                            auto [types, names] = parse_parameter_list(&defaults,
                                &expr_variadic);
                            param.expr_param_types = std::move(types);
                            param.expr_param_names = std::move(names);
                            if (expr_variadic) {
                                report_error_template(
                                    ErrorCode::ExpressionParameterVariadicNotAllowed,
                                    { param.name });
                            }
                            for (const std::unique_ptr<Expression>& default_value : defaults) {
                                if (default_value != nullptr) {
                                    report_error_template(
                                        ErrorCode::ExprParameterDisallowedSyntax,
                                        { param.name });
                                    break;
                                }
                            }
                            if (!expect(TokenType::RightParen,
                                "expected ')' after expression parameter list")) {
                                return nullptr;
                            }
                        }
                        if (current_.type != TokenType::Arrow) {
                            report_error(ErrorCode::ExpressionSyntaxError,
                                "expected '->' and a return type in expression parameter declaration");
                            return nullptr;
                        }

                        advance();
                        param.expr_return_type = parse_type(true, false);
                        def->parameters.push_back(std::move(param));
                    } else if (current_.type == TokenType::Identifier) {
                        std::string first(current_.lexeme);

                        if (lookahead_type(1) == TokenType::Ellipsis) {
                            if (lookahead_type(2) == TokenType::Identifier) {
                                param.is_type = false;
                                param.is_pack = true;
                                param.constant_type_is_parameter = true;
                                param.constant_type_parameter = first;
                                param.name = std::string(lookahead(2).lexeme);
                                advance();
                                advance();
                                advance();
                            } else {
                                param.is_type = true;
                                param.is_pack = true;
                                param.name = first;
                                advance();
                                advance();

                                if (current_.type == TokenType::Colon) {
                                    advance();
                                    param.constraint = parse_generic_constraint();
                                }
                            }

                            def->parameters.push_back(std::move(param));
                        } else if (lookahead_type(1) == TokenType::Identifier) {
                            param.is_type = false;
                            param.constant_type_is_parameter = true;
                            param.constant_type_parameter = first;
                            param.name = std::string(lookahead(1).lexeme);
                            advance();
                            advance();
                            def->parameters.push_back(std::move(param));
                        } else {
                            param.is_type = true;
                            param.name = first;
                            advance();

                            if (current_.type == TokenType::Colon) {
                                advance();
                                param.constraint = parse_generic_constraint();
                            }

                            def->parameters.push_back(std::move(param));
                        }
                    } else if (is_builtin_type_keyword(current_.type)) {
                        param.is_type = false;
                        param.constant_type = parse_type_specifier(true);

                        if (current_.type == TokenType::Ellipsis) {
                            param.is_pack = true;
                            advance();
                        }

                        if (current_.type != TokenType::Identifier) {
                            report_error(ErrorCode::ExpressionSyntaxError,
                                "expected constant parameter name");
                            return nullptr;
                        }

                        param.name = std::string(current_.lexeme);
                        advance();
                        def->parameters.push_back(std::move(param));
                    } else {
                        report_error(ErrorCode::ExpressionSyntaxError,
                            "invalid generic parameter");
                        return nullptr;
                    }
                } else {
                    GenericPatternArg pattern;

                    if (current_.type == TokenType::Identifier &&
                        current_.lexeme == "expr" &&
                        (lookahead_type(1) == TokenType::Identifier ||
                            lookahead_type(1) == TokenType::LeftParen)) {
                        report_error_template(ErrorCode::ExprParameterInSpecializationPattern,
                            { std::string(lookahead(1).lexeme) });
                        return nullptr;
                    }

                    bool type_like = (is_builtin_type_keyword(current_.type) ||
                        current_.type == TokenType::Identifier);

                    if (type_like) {
                        Type t = parse_type(true, false);

                        for (;;) {
                            if (current_.type == TokenType::LeftBracket) {
                                advance();
                                if (!expect(TokenType::RightBracket, "expected ']' in pattern")) {
                                    return nullptr;
                                }
                                t = Type::make_array(std::make_shared<Type>(std::move(t)),
                                    std::nullopt);
                                continue;
                            }
                            if (current_.type == TokenType::Star) {
                                advance();
                                t = Type::make_pointer(std::make_shared<Type>(std::move(t)));
                                continue;
                            }

                            if (current_.type == TokenType::Power) {
                                advance();
                                t = Type::make_pointer(std::make_shared<Type>(
                                    Type::make_pointer(std::make_shared<Type>(std::move(t)))));
                                continue;
                            }

                            break;
                        }

                        pattern.is_constant = false;
                        pattern.text = t.to_string();
                        pattern.type = std::move(t);
                    } else {
                        auto expr = parse_compile_time_expression();
                        long long iv = 0;
                        double dv = 0.0;
                        bool is_flt = false;
                        bool string_pattern = false;
                        std::string string_value;

                        if (expr != nullptr) {
                            if (auto* prim = dynamic_cast<PrimaryExpression*>(expr.get())) {
                                if (prim->kind == PrimaryExpression::Kind::Literal &&
                                    prim->literal_token.type == TokenType::StringLiteral) {
                                    string_pattern = true;
                                    string_value = unquote_string(prim->literal_token.lexeme);
                                }
                            }
                        }

                        if (!string_pattern &&
                            (!expr || !fold_constant_expression(expr.get(), iv, dv, is_flt))) {
                            report_error_template(ErrorCode::GenericConstantPatternNotConstant,
                                { compile_time_expr_text(expr.get()) });
                            return nullptr;
                        }

                        pattern.is_constant = true;

                        if (string_pattern) {
                            pattern.string_constant = true;
                            pattern.string_value = string_value;
                            pattern.text = "\"" + string_value + "\"";
                        } else if (is_flt) {
                            pattern.float_constant = true;
                            pattern.float_value = dv;
                            pattern.text = std::to_string(iv);
                        } else {
                            pattern.int_value = iv;
                            pattern.text = std::to_string(iv);
                        }
                    }

                    def->patterns.push_back(std::move(pattern));
                }

                if (current_.type == TokenType::Comma) {
                    advance();
                    continue;
                }
                break;
            } while (true);
        }

        if (!expect(TokenType::Greater, "expected '>' to close generic parameter list")) {
            return nullptr;
        }

        if (!expect(TokenType::LeftBrace, "expected '{' to open generic block")) {
            return nullptr;
        }

        skip_newlines();

        while (current_.type != TokenType::RightBrace && current_.type != TokenType::EndOfFile) {
            skip_newlines();

            if (current_.type == TokenType::RightBrace) { break; }

            if (current_.type == TokenType::Keyword_Emit ||
               current_.type == TokenType::Keyword_If) {
                auto item = parse_generic_compile_time_item();

                if (item != nullptr) {
                    def->compile_time_items.push_back(std::move(item));
                    continue;
                }
            }

            auto member = parse_generic_member();

            if (member != nullptr) {
                if (dynamic_cast<StructDefinition*>(member.get()) == nullptr &&
                    dynamic_cast<FunctionDefinition*>(member.get()) == nullptr) {
                    report_error_at(member->location, ErrorCode::GenericStatementNotAllowed,
                        "executable statements are not allowed directly inside a generic block");
                    continue;
                }

                def->members.push_back(std::move(member));
            } else {
                report_error(ErrorCode::GenericStatementNotAllowed,
                    "executable statements are not allowed directly inside a generic block");
                synchronize();
            }
        }

        expect(TokenType::RightBrace, "expected '}' to close generic block");
        expect_stmt_end("generic definition");
        return def;
    }

    std::unique_ptr<TopLevel> Parser::parse_generic_member() {
        if (current_.type == TokenType::Keyword_Export) {
            return parse_export_member("a generics block");
        }
        if (current_.type == TokenType::Keyword_Struct) {
            return parse_struct_definition();
        }
        if (at_struct_attribute()) {
            return parse_struct_definition();
        }
        TokenType tt = current_.type;
        bool type_start = (is_type_start_keyword(tt) ||
            tt == TokenType::Identifier);
        if (type_start) {
            if (looks_like_operator_definition()) {
                return parse_operator_definition();
            }
            return parse_function_definition();
        }
        return nullptr;
    }

}
