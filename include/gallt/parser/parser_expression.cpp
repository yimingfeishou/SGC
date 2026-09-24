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

    std::string Parser::expression_argument_text(std::size_t from, std::size_t to) const {
        std::string out;
        if (from >= tokens_.size()) {
            return out;
        }

        std::size_t end = to < tokens_.size() ? to : tokens_.size();

        for (std::size_t i = from; i < end; ++i) {
            if (tokens_[i].type == TokenType::Newline) {
                out += ' ';
                continue;
            }

            out += std::string(tokens_[i].lexeme);
        }

        return out;
    }

    std::unique_ptr<Expression> Parser::parse_condition_expression() {
        if (current_.type != TokenType::LeftParen) {
            report_error(ErrorCode::ExpressionSyntaxError,
                "expected '(' after '@if'");
            return nullptr;
        }
        advance();
        skip_newlines();
        auto expr = parse_expression();

        if (expr == nullptr) {
            report_error(ErrorCode::ExpressionSyntaxError,
                "expected condition expression after '('");
            return nullptr;
        }

        skip_newlines();

        if (!expect(TokenType::RightParen, "expected ')' after condition expression")) {
            return nullptr;
        }
        return expr;
    }

    std::unique_ptr<Expression> Parser::parse_property_argument() {
        SourceLocation loc = current_location();
        if (current_.type == TokenType::StringLiteral) {
            Token lit = current_;
            advance();
            return std::make_unique<PrimaryExpression>(loc, lit);
        }

        if (current_.type == TokenType::IntegerLiteral ||
            current_.type == TokenType::FloatLiteral ||
            current_.type == TokenType::CharLiteral ||
            current_.type == TokenType::BoolLiteral) {
            Token lit = current_;
            advance();
            return std::make_unique<PrimaryExpression>(loc, lit);
        }

        auto is_type_keyword = [](TokenType t) {
            return is_builtin_type_keyword(t);
        };

        if (current_.type == TokenType::Identifier || is_type_keyword(current_.type)) {
            std::string text(current_.lexeme);
            advance();

            while (current_.type == TokenType::ColonColon &&
                lookahead_type(1) == TokenType::Identifier) {
                advance();
                text += "::";
                text += std::string(current_.lexeme);
                advance();
            }

            for (;;) {
                if (current_.type == TokenType::Star) {
                    text += "*";
                    advance();
                    continue;
                }

                if (current_.type == TokenType::Power) {
                    text += "**";
                    advance();
                    continue;
                }

                if (current_.type == TokenType::LeftBracket &&
                    lookahead_type(1) == TokenType::RightBracket) {
                    text += "[]";
                    advance();
                    advance();
                    continue;
                }

                break;
            }

            return std::make_unique<PrimaryExpression>(loc, text);
        }

        report_error(ErrorCode::ExpressionSyntaxError,
            "expected type name or string literal in compile-time property");
        return nullptr;
    }

    std::unique_ptr<Expression> Parser::parse_expression() {
        struct NestingGuard {
            int& depth;
            explicit NestingGuard(int& d) : depth(d) { ++depth; }

            ~NestingGuard() { --depth; }
        } guard(expression_depth_);

        if (expression_depth_ == 1) {
            expression_tokens_ = 0;
        }

        if (expression_depth_ > kMaxExpressionNesting) {
            if (!complexity_limit_hit_) {
                report_error(ErrorCode::ExpressionSyntaxError,
                    "expression nesting is too deep (more than " +
                    std::to_string(kMaxExpressionNesting) + " levels)");
                complexity_limit_hit_ = true;
            }

            return nullptr;
        }

        if (complexity_limit_hit_) {
            return nullptr;
        }

        auto expr = parse_assignment_expression();

        if (complexity_limit_hit_) {
            return nullptr;
        }

        return expr;
    }

    std::unique_ptr<Expression> Parser::parse_assignment_expression() {
        auto left = parse_conditional_expression();
        if (left == nullptr) { return nullptr; }

        if (current_.type == TokenType::Assign ||
            current_.type == TokenType::PlusAssign ||
            current_.type == TokenType::MinusAssign ||
            current_.type == TokenType::AndAssign ||
            current_.type == TokenType::OrAssign ||
            current_.type == TokenType::XorAssign ||
            compound_shift_assignment()) {
            SourceLocation loc = current_.location;
            AssignmentExpression::Operator op;
            if (compound_shift_assignment()) {
                const bool is_left_shift = current_.type == TokenType::Less;
                const std::size_t consumed =
                    lookahead_type(1) == TokenType::LessEqual ||
                    lookahead_type(1) == TokenType::GreaterEqual ? 2u : 3u;

                for (std::size_t i = 0; i < consumed; ++i) {
                    advance();
                }

                op = is_left_shift
                    ? AssignmentExpression::Operator::ShiftLeftAssign
                    : AssignmentExpression::Operator::ShiftRightAssign;
            } else {
                switch (current_.type) {
                case TokenType::Assign: op = AssignmentExpression::Operator::Assign; break;
                case TokenType::PlusAssign: op = AssignmentExpression::Operator::PlusAssign; break;
                case TokenType::MinusAssign: op = AssignmentExpression::Operator::MinusAssign; break;
                case TokenType::AndAssign: op = AssignmentExpression::Operator::AndAssign; break;
                case TokenType::OrAssign: op = AssignmentExpression::Operator::OrAssign; break;
                case TokenType::XorAssign: op = AssignmentExpression::Operator::XorAssign; break;
                default: op = AssignmentExpression::Operator::Assign; break;
                }
                advance();
            }

            auto right = parse_assignment_expression();

            if (right == nullptr) {
                report_error(ErrorCode::ExpressionSyntaxError, "expected right-hand side of assignment");
                return nullptr;
            }

            return std::make_unique<AssignmentExpression>(loc, std::move(left), op, std::move(right));
        }

        return left;
    }

    bool Parser::compound_shift_assignment() const {
        if (current_.type != TokenType::Less && current_.type != TokenType::Greater) {
            return false;
        }

        if (current_.type == TokenType::Less) {
            if (lookahead_type(1) == TokenType::LessEqual) {
                return true;
            }
            return lookahead_type(1) == TokenType::Less &&
                lookahead_type(2) == TokenType::Assign;
        }

        if (lookahead_type(1) == TokenType::GreaterEqual) {
            return true;
        }

        return lookahead_type(1) == TokenType::Greater &&
            lookahead_type(2) == TokenType::Assign;
    }

    bool Parser::shift_followed_by_assign() const {
        if (current_.type != TokenType::Less && current_.type != TokenType::Greater) {
            return false;
        }

        if (lookahead_type(1) != current_.type) {
            return false;
        }

        TokenType third = lookahead_type(2);
        return third == TokenType::Assign || third == TokenType::LessEqual ||
            third == TokenType::GreaterEqual;
    }

    bool Parser::at_shift_or_assign_head() const {
        if (current_.type == TokenType::Less) {
            return lookahead_type(1) == TokenType::Less ||
                lookahead_type(1) == TokenType::LessEqual;
        }

        if (current_.type == TokenType::Greater) {
            return lookahead_type(1) == TokenType::Greater ||
                lookahead_type(1) == TokenType::GreaterEqual;
        }

        return false;
    }

    std::unique_ptr<Expression> Parser::parse_conditional_expression() {
        auto condition = parse_logical_or_expression();
        if (condition == nullptr) { return nullptr; }

        if (current_.type != TokenType::Question) {
            return condition;
        }

        SourceLocation loc = current_.location;
        advance();
        auto then_expr = parse_expression();

        if (then_expr == nullptr) {
            report_error(ErrorCode::ExpressionSyntaxError,
                "expected expression after '?'");
            return nullptr;
        }

        if (!expect(TokenType::Colon, "expected ':' in conditional expression")) {
            return nullptr;
        }

        auto else_expr = parse_conditional_expression();

        if (else_expr == nullptr) {
            report_error(ErrorCode::ExpressionSyntaxError,
                "expected expression after ':' in conditional expression");
            return nullptr;
        }

        return std::make_unique<ConditionalExpression>(loc, std::move(condition),
            std::move(then_expr), std::move(else_expr));
    }

    std::unique_ptr<Expression> Parser::parse_logical_or_expression() {
        auto left = parse_logical_and_expression();
        if (left == nullptr) { return nullptr; }

        while (current_.type == TokenType::LogicalOr) {
            SourceLocation loc = current_.location;
            advance();
            auto right = parse_logical_and_expression();

            if (right == nullptr) {
                report_error(ErrorCode::ExpressionSyntaxError, "expected right operand of '||'");
                return nullptr;
            }

            left = std::make_unique<LogicalOrExpression>(loc, std::move(left), std::move(right));
        }

        return left;
    }

    std::unique_ptr<Expression> Parser::parse_logical_and_expression() {
        auto left = parse_bitwise_expression();
        if (left == nullptr) { return nullptr; }

        while (current_.type == TokenType::LogicalAnd) {
            SourceLocation loc = current_.location;
            advance();
            auto right = parse_bitwise_expression();

            if (right == nullptr) {
                report_error(ErrorCode::ExpressionSyntaxError, "expected right operand of '&&'");
                return nullptr;
            }

            left = std::make_unique<LogicalAndExpression>(loc, std::move(left), std::move(right));
        }

        return left;
    }

    std::unique_ptr<Expression> Parser::parse_bitwise_expression() {
        auto left = parse_comparison_expression();
        if (left == nullptr) { return nullptr; }

        while (current_.type == TokenType::AddressOf ||
            current_.type == TokenType::Caret ||
            current_.type == TokenType::Pipe) {
            SourceLocation loc = current_.location;
            BitwiseExpression::Operator op = BitwiseExpression::Operator::And;
            if (current_.type == TokenType::Caret) {
                op = BitwiseExpression::Operator::Xor;
            } else if (current_.type == TokenType::Pipe) {
                op = BitwiseExpression::Operator::Or;
            }
            advance();
            auto right = parse_comparison_expression();

            if (right == nullptr) {
                report_error(ErrorCode::ExpressionSyntaxError,
                    "expected right operand of the bitwise operator");
                return nullptr;
            }

            left = std::make_unique<BitwiseExpression>(loc, std::move(left), op,
                std::move(right));
        }

        return left;
    }

    std::unique_ptr<Expression> Parser::parse_comparison_expression() {
        auto left = parse_shift_expression();
        if (left == nullptr) { return nullptr; }

        while (current_.type == TokenType::Greater ||
            current_.type == TokenType::Less ||
            current_.type == TokenType::Equal ||
            current_.type == TokenType::NotEqual ||
            current_.type == TokenType::GreaterEqual ||
            current_.type == TokenType::LessEqual) {
            if (in_expr_argument_ && current_.type == TokenType::Greater) {
                break;
            }
            if (at_shift_or_assign_head()) {
                break;
            }
            SourceLocation loc = current_.location;
            ComparisonExpression::Operator op;
            switch (current_.type) {
            case TokenType::Greater: op = ComparisonExpression::Operator::Greater; break;
            case TokenType::Less: op = ComparisonExpression::Operator::Less; break;
            case TokenType::Equal: op = ComparisonExpression::Operator::Equal; break;
            case TokenType::NotEqual: op = ComparisonExpression::Operator::NotEqual; break;
            case TokenType::GreaterEqual: op = ComparisonExpression::Operator::GreaterEqual; break;
            case TokenType::LessEqual: op = ComparisonExpression::Operator::LessEqual; break;
            default: op = ComparisonExpression::Operator::Equal; break;
            }
            advance();
            auto right = parse_shift_expression();

            if (right == nullptr) {
                report_error(ErrorCode::ExpressionSyntaxError, "expected right operand of comparison");
                return nullptr;
            }

            left = std::make_unique<ComparisonExpression>(loc, std::move(left), op, std::move(right));
        }

        return left;
    }

    std::unique_ptr<Expression> Parser::parse_shift_expression() {
        auto left = parse_additive_expression();
        if (left == nullptr) { return nullptr; }

        while ((current_.type == TokenType::Less || current_.type == TokenType::Greater) &&
            lookahead_type(1) == current_.type &&
            !shift_followed_by_assign()) {
            if (in_expr_argument_ && current_.type == TokenType::Greater) {
                break;
            }
            SourceLocation loc = current_.location;
            const bool is_left_shift = current_.type == TokenType::Less;
            advance();
            advance();
            auto right = parse_additive_expression();

            if (right == nullptr) {
                report_error(ErrorCode::ExpressionSyntaxError,
                    is_left_shift ? "expected right operand of '<<'"
                                  : "expected right operand of '>>'");
                return nullptr;
            }

            left = std::make_unique<ShiftExpression>(loc, std::move(left),
                is_left_shift ? ShiftExpression::Operator::Left
                              : ShiftExpression::Operator::Right,
                std::move(right));
        }

        return left;
    }

    std::unique_ptr<Expression> Parser::parse_additive_expression() {
        auto left = parse_multiplicative_expression();
        if (left == nullptr) { return nullptr; }

        while (current_.type == TokenType::Plus || current_.type == TokenType::Minus) {
            SourceLocation loc = current_.location;
            AdditiveExpression::Operator op;
            if (current_.type == TokenType::Plus) {
                op = AdditiveExpression::Operator::Plus;
            } else {
                op = AdditiveExpression::Operator::Minus;
            }
            advance();
            auto right = parse_multiplicative_expression();

            if (right == nullptr) {
                report_error(ErrorCode::ExpressionSyntaxError, "expected right operand of additive operator");
                return nullptr;
            }

            left = std::make_unique<AdditiveExpression>(loc, std::move(left), op, std::move(right));
        }

        return left;
    }

    std::unique_ptr<Expression> Parser::parse_multiplicative_expression() {
        auto left = parse_power_expression();
        if (left == nullptr) { return nullptr; }

        while (current_.type == TokenType::Star || current_.type == TokenType::Slash ||
            current_.type == TokenType::Percent) {
            SourceLocation loc = current_.location;
            MultiplicativeExpression::Operator op;
            if (current_.type == TokenType::Star) {
                op = MultiplicativeExpression::Operator::Multiply;
            } else if (current_.type == TokenType::Slash) {
                op = MultiplicativeExpression::Operator::Divide;
            } else {
                op = MultiplicativeExpression::Operator::Remainder;
            }
            advance();
            auto right = parse_power_expression();

            if (right == nullptr) {
                report_error(ErrorCode::ExpressionSyntaxError, "expected right operand of multiplicative operator");
                return nullptr;
            }

            left = std::make_unique<MultiplicativeExpression>(loc, std::move(left), op, std::move(right));
        }

        return left;
    }

    std::unique_ptr<Expression> Parser::parse_power_expression() {
        auto left = parse_unary_expression();
        if (left == nullptr) { return nullptr; }

        if (current_.type == TokenType::Power) {
            SourceLocation loc = current_.location;
            advance();
            auto right = parse_power_expression();
            if (right == nullptr) {
                report_error(ErrorCode::ExpressionSyntaxError, "expected right operand of '**'");
                return nullptr;
            }
            left = std::make_unique<PowerExpression>(loc, std::move(left), std::move(right));
        }

        return left;
    }

    std::unique_ptr<Expression> Parser::parse_unary_expression() {
        if (current_.type == TokenType::Increment ||
            current_.type == TokenType::Decrement ||
            current_.type == TokenType::LogicalNot ||
            current_.type == TokenType::AddressOf ||
            current_.type == TokenType::Tilde ||
            current_.type == TokenType::Star ||
            current_.type == TokenType::Plus ||
            current_.type == TokenType::Minus) {
            SourceLocation loc = current_.location;
            UnaryExpression::Operator op;
            switch (current_.type) {
            case TokenType::Increment: op = UnaryExpression::Operator::Increment; break;
            case TokenType::Decrement: op = UnaryExpression::Operator::Decrement; break;
            case TokenType::LogicalNot: op = UnaryExpression::Operator::LogicalNot; break;
            case TokenType::AddressOf: op = UnaryExpression::Operator::AddressOf; break;
            case TokenType::Tilde: op = UnaryExpression::Operator::BitwiseNot; break;
            case TokenType::Star: op = UnaryExpression::Operator::Dereference; break;
            case TokenType::Plus: op = UnaryExpression::Operator::UnaryPlus; break;
            case TokenType::Minus: op = UnaryExpression::Operator::UnaryMinus; break;
            default: op = UnaryExpression::Operator::LogicalNot; break;
            }
            advance();
            auto operand = parse_unary_expression();
            if (operand == nullptr) {
                report_error(ErrorCode::ExpressionSyntaxError, "expected operand for unary operator");
                return nullptr;
            }
            return std::make_unique<UnaryExpression>(loc, op, std::move(operand));
        }
        if (current_.type == TokenType::Power) {
            SourceLocation loc = current_.location;
            advance();
            auto operand = parse_unary_expression();
            if (operand == nullptr) {
                report_error(ErrorCode::ExpressionSyntaxError,
                    "expected operand for dereference operator");
                return nullptr;
            }
            auto inner = std::make_unique<UnaryExpression>(
                loc, UnaryExpression::Operator::Dereference, std::move(operand));
            return std::make_unique<UnaryExpression>(
                loc, UnaryExpression::Operator::Dereference, std::move(inner));
        }
        return parse_postfix_expression();
    }

    std::unique_ptr<Expression> Parser::parse_postfix_expression() {
        auto primary = parse_primary_expression();
        if (primary == nullptr) { return nullptr; }
        return parse_postfix_operator(std::move(primary));
    }

    std::unique_ptr<Expression> Parser::parse_postfix_operator(std::unique_ptr<Expression> base) {
        while (true) {
            SourceLocation loc = current_.location;
            if (current_.type == TokenType::LeftBracket) {
                advance();
                auto index = parse_expression();

                if (index == nullptr) {
                    report_error(ErrorCode::ExpressionSyntaxError, "expected index expression");
                    return nullptr;
                }

                if (!expect(TokenType::RightBracket, "expected ']' after index")) {
                    return nullptr;
                }
                base = std::make_unique<PostfixExpression>(
                    loc, std::move(base), PostfixExpression::Operator::Subscript,
                    std::move(index));
            } else if (current_.type == TokenType::LeftParen) {
                advance();
                std::vector<std::unique_ptr<Expression>> args;

                if (current_.type != TokenType::RightParen) {
                    args = parse_argument_list();

                    if (args.empty() && current_.type != TokenType::RightParen) {
                        report_error(ErrorCode::ExpressionSyntaxError, "invalid argument list");
                    }
                }

                if (!expect(TokenType::RightParen, "expected ')' after function arguments")) {
                    return nullptr;
                }
                base = std::make_unique<PostfixExpression>(
                    loc, std::move(base), PostfixExpression::Operator::FunctionCall,
                    nullptr, std::move(args));
            } else if (current_.type == TokenType::Increment) {
                advance();
                base = std::make_unique<PostfixExpression>(
                    loc, std::move(base), PostfixExpression::Operator::Increment);
            } else if (current_.type == TokenType::Decrement) {
                advance();
                base = std::make_unique<PostfixExpression>(
                    loc, std::move(base), PostfixExpression::Operator::Decrement);
            } else if (current_.type == TokenType::Dot) {
                advance();
                if (current_.type != TokenType::Identifier) {
                    report_error(ErrorCode::ExpressionSyntaxError,
                        "expected member name after '.'");
                    return nullptr;
                }

                std::string member(current_.lexeme);
                advance();

                if ((member == "is_same" || member == "is_convertible" ||
                    member == "has_member") && current_.type == TokenType::Less) {
                    advance();
                    std::vector<std::unique_ptr<Expression>> args;

                    if (current_.type != TokenType::Greater) {
                        do {
                            auto arg = parse_property_argument();
                            if (arg == nullptr) { return nullptr; }
                            args.push_back(std::move(arg));

                            if (current_.type == TokenType::Comma) {
                                advance();
                                continue;
                            }

                            break;
                        } while (true);
                    }

                    if (!expect(TokenType::Greater,
                        "expected '>' to close compile-time property argument list")) {
                        return nullptr;
                    }
                    base = std::make_unique<CompileTimePropertyExpression>(loc,
                        std::move(base), member, std::move(args));
                    continue;
                }
                base = std::make_unique<PostfixExpression>(
                    loc, std::move(base), PostfixExpression::Operator::Dot,
                    nullptr, std::vector<std::unique_ptr<Expression>>{},
                    Type(), member);
            } else if (current_.type == TokenType::Arrow) {
                advance();
                if (current_.type != TokenType::Identifier) {
                    report_error(ErrorCode::ExpressionSyntaxError,
                        "expected member name after '->'");
                    return nullptr;
                }

                std::string member(current_.lexeme);
                advance();
                base = std::make_unique<PostfixExpression>(
                    loc, std::move(base), PostfixExpression::Operator::Arrow,
                    nullptr, std::vector<std::unique_ptr<Expression>>{},
                    Type(), member);
            } else if (current_.type == TokenType::ColonColon) {
                report_error_template(ErrorCode::ScopeOperatorOperandInvalid,
                    { std::string("expression") });
                advance();
                if (current_.type == TokenType::Identifier) {
                    advance();
                }
                break;
            } else if (current_.type == TokenType::Ellipsis) {
                advance();
                base = std::make_unique<PostfixExpression>(
                    loc, std::move(base), PostfixExpression::Operator::PackExpand);
            } else {
                break;
            }
        }
        return base;
    }

    std::unique_ptr<Expression> Parser::parse_primary_expression() {
        SourceLocation loc = current_.location;
        switch (current_.type) {
        case TokenType::IntegerLiteral:
        case TokenType::FloatLiteral:
        case TokenType::CharLiteral:
        case TokenType::StringLiteral:
        case TokenType::BoolLiteral: {
            Token lit = current_;
            advance();
            return std::make_unique<PrimaryExpression>(loc, lit);
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
            TokenType after = lookahead_type(1);
            if (after == TokenType::RightParen || after == TokenType::Comma) {
                std::string type_name(current_.lexeme);
                advance();
                return std::make_unique<PrimaryExpression>(loc, type_name);
            }
            const bool function_suffix = cast_type_has_function_pointer_suffix();
            Type cast_type = parse_type(false, false);
            if (function_suffix) {
                auto suffix = parse_function_pointer_suffix(cast_type);
                if (suffix.has_value()) {
                    cast_type = std::move(suffix.value());
                }
            }
            if (current_.type != TokenType::LeftParen) {
                report_error_at(loc, ErrorCode::ExpressionSyntaxError,
                    "expected '(' after type name in cast");
                return nullptr;
            }
            advance();
            auto operand = parse_expression();
            if (operand == nullptr) {
                report_error_at(loc, ErrorCode::ExpressionSyntaxError,
                    "expected expression in cast");
                return nullptr;
            }
            if (!expect(TokenType::RightParen, "expected ')' after cast expression")) {
                return nullptr;
            }
            return std::make_unique<PostfixExpression>(
                loc, std::move(operand), PostfixExpression::Operator::Cast,
                nullptr, std::vector<std::unique_ptr<Expression>>{},
                std::move(cast_type));
        }

        case TokenType::Keyword_Void: {
            TokenType after = lookahead_type(1);
            if (after == TokenType::RightParen || after == TokenType::Comma) {
                std::string type_name(current_.lexeme);
                advance();
                return std::make_unique<PrimaryExpression>(loc, type_name);
            }
            const bool function_suffix = cast_type_has_function_pointer_suffix();
            Type cast_type = parse_type(true, false);
            if (function_suffix) {
                auto suffix = parse_function_pointer_suffix(cast_type);
                if (suffix.has_value()) {
                    cast_type = std::move(suffix.value());
                }
            }
            if (current_.type != TokenType::LeftParen) {
                report_error_at(loc, ErrorCode::ExpressionSyntaxError,
                    "expected '(' after type name in cast");
                return nullptr;
            }
            advance();
            auto operand = parse_expression();
            if (operand == nullptr) {
                report_error_at(loc, ErrorCode::ExpressionSyntaxError,
                    "expected expression in cast");
                return nullptr;
            }
            if (!expect(TokenType::RightParen, "expected ')' after cast expression")) {
                return nullptr;
            }
            return std::make_unique<PostfixExpression>(
                loc, std::move(operand), PostfixExpression::Operator::Cast,
                nullptr, std::vector<std::unique_ptr<Expression>>{},
                std::move(cast_type));
        }

        case TokenType::Keyword_File: {
            TokenType after = lookahead_type(1);
            if (after == TokenType::RightParen || after == TokenType::Comma) {
                std::string type_name(current_.lexeme);
                advance();
                return std::make_unique<PrimaryExpression>(loc, type_name);
            }
            Type cast_type = parse_type(false, false);
            if (current_.type != TokenType::LeftParen) {
                report_error_at(loc, ErrorCode::ExpressionSyntaxError,
                    "expected '(' after type name in cast");
                return nullptr;
            }
            advance();
            auto operand = parse_expression();
            if (operand == nullptr) {
                report_error_at(loc, ErrorCode::ExpressionSyntaxError,
                    "expected expression in cast");
                return nullptr;
            }
            if (!expect(TokenType::RightParen, "expected ')' after cast expression")) {
                return nullptr;
            }
            return std::make_unique<PostfixExpression>(
                loc, std::move(operand), PostfixExpression::Operator::Cast,
                nullptr, std::vector<std::unique_ptr<Expression>>{},
                std::move(cast_type));
        }

        case TokenType::Identifier: {
            std::string id(current_.lexeme);
            if (looks_like_generic_instantiation()) {
                GenericRef ref = parse_generic_reference(id);
                return std::make_unique<PrimaryExpression>(loc, std::move(ref));
            }
            if (lookahead_type(1) == TokenType::ColonColon) {
                std::vector<std::string> path;
                path.push_back(id);
                advance();
                while (current_.type == TokenType::ColonColon) {
                    advance();
                    if (current_.type != TokenType::Identifier) {
                        report_error(ErrorCode::ExpressionSyntaxError,
                            "expected identifier after '::'");
                        return nullptr;
                    }
                    path.push_back(std::string(current_.lexeme));
                    advance();
                }
                return std::make_unique<PrimaryExpression>(loc, std::move(path));
            }
            if (declared_type_names_.find(id) != declared_type_names_.end() &&
                declared_value_names_.find(id) == declared_value_names_.end() &&
                looks_like_pointer_type_cast()) {
                Type cast_type = parse_type(false, false);
                if (current_.type != TokenType::LeftParen) {
                    report_error_at(loc, ErrorCode::ExpressionSyntaxError,
                        "expected '(' after type name in cast");
                    return nullptr;
                }
                advance();
                auto operand = parse_expression();
                if (operand == nullptr) {
                    report_error_at(loc, ErrorCode::ExpressionSyntaxError,
                        "expected expression in cast");
                    return nullptr;
                }
                if (!expect(TokenType::RightParen, "expected ')' after cast expression")) {
                    return nullptr;
                }
                return std::make_unique<PostfixExpression>(
                    loc, std::move(operand), PostfixExpression::Operator::Cast,
                    nullptr, std::vector<std::unique_ptr<Expression>>{},
                    std::move(cast_type));
            }
            if (lookahead_type(1) == TokenType::LeftParen &&
                (id == "copy" || id == "move" || id == "deep_copy" || id == "shallow_copy")) {
                PrimaryExpression::CopyMoveKind cm = PrimaryExpression::CopyMoveKind::Copy;
                if (id == "move") {
                    cm = PrimaryExpression::CopyMoveKind::Move;
                } else if (id == "deep_copy") {
                    cm = PrimaryExpression::CopyMoveKind::DeepCopy;
                } else if (id == "shallow_copy") {
                    cm = PrimaryExpression::CopyMoveKind::ShallowCopy;
                }
                advance();
                advance();
                auto operand = parse_expression();
                if (operand == nullptr) {
                    report_error(ErrorCode::ExpressionSyntaxError,
                        "expected operand for " + id);
                    return nullptr;
                }
                if (!expect(TokenType::RightParen, "expected ')' after " + id + " operand")) {
                    return nullptr;
                }
                return std::make_unique<PrimaryExpression>(loc, cm, std::move(operand));
            }
            bool construct_type_start = false;
            switch (lookahead_type(1)) {
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
                construct_type_start = true;
                break;
            default:
                construct_type_start = false;
                break;
            }
            if (id == "construct" && construct_type_start) {
                advance();
                Type construct_type = parse_type(true, false);
                if (!expect(TokenType::LeftParen, "expected '(' after construct type")) {
                    return nullptr;
                }
                std::vector<std::unique_ptr<Expression>> args;
                if (current_.type != TokenType::RightParen) {
                    args = parse_argument_list();
                }
                if (!expect(TokenType::RightParen, "expected ')' after construct arguments")) {
                    return nullptr;
                }
                std::unique_ptr<Expression> placement = nullptr;
            if (current_.type == TokenType::Identifier && current_.lexeme == "at") {
                    advance();
                    placement = parse_expression();

                    if (placement == nullptr) {
                        report_error(ErrorCode::ExpressionSyntaxError,
                            "expected pointer expression after 'at'");
                        return nullptr;
                    }
                }

                return std::make_unique<PrimaryExpression>(
                    loc, std::move(construct_type), std::move(args), std::move(placement));
            }
            advance();
            return std::make_unique<PrimaryExpression>(loc, id);
        }

        case TokenType::LeftParen: {
            advance();
            auto expr = parse_expression();
            if (expr == nullptr) {
                report_error(ErrorCode::ExpressionSyntaxError, "expected expression inside parentheses");
                return nullptr;
            }
            if (!expect(TokenType::RightParen, "expected ')' after parenthesized expression")) {
                return nullptr;
            }
            return std::make_unique<PrimaryExpression>(loc, std::move(expr));
        }

        case TokenType::Keyword_Null: {
            advance();
            return PrimaryExpression::make_null(loc);
        }

        case TokenType::Keyword_Heap: {
            advance();
            if (!expect(TokenType::LeftParen, "expected '(' after 'heap'")) {
                return nullptr;
            }
            Type heap_type = parse_type(false);
            if (heap_type.kind == TypeKind::Void) {
                report_error(ErrorCode::HeapFirstArgNotType, "heap() first argument must be a type");
            }
            std::unique_ptr<Expression> size_expr = nullptr;
            if (current_.type == TokenType::Comma) {
                advance();
                size_expr = parse_expression();
                if (size_expr == nullptr) {
                    report_error(ErrorCode::ExpressionSyntaxError, "expected size expression in heap()");
                }
            }
            if (!expect(TokenType::RightParen, "expected ')' after heap() arguments")) {
                return nullptr;
            }
            return std::make_unique<PrimaryExpression>(loc, std::move(heap_type), std::move(size_expr));
        }

        default: {
            report_error(ErrorCode::ExpressionSyntaxError, "expected primary expression");
            return nullptr;
        }
        }
    }

    std::vector<std::unique_ptr<Expression>> Parser::parse_argument_list() {
        std::vector<std::unique_ptr<Expression>> args;

        do {
            auto expr = parse_expression();

            if (expr == nullptr) {
                report_error(ErrorCode::ExpressionSyntaxError, "expected expression in argument list");
                break;
            }

            args.push_back(std::move(expr));

            if (current_.type != TokenType::Comma) {
                break;
            }

            advance();
        } while (true);

        return args;
    }

    std::unique_ptr<Expression> Parser::parse_optional_expression() {
        if (current_.type == TokenType::Semicolon || current_.type == TokenType::RightParen) {
            return nullptr;
        }
        return parse_expression();
    }

}
