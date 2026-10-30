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

    bool Parser::at_const_if_statement() const {
        return current_.type == TokenType::Keyword_Const &&
            lookahead_type(1) == TokenType::Keyword_If;
    }

    bool Parser::at_const_else_clause() const {
        return current_.type == TokenType::Keyword_Const &&
            lookahead_type(1) == TokenType::Keyword_Else;
    }

    bool Parser::const_if_allowed() const {
        return generic_block_depth_ > 0 && emit_content_depth_ == 0;
    }

    std::unique_ptr<EmitStatement> Parser::parse_emit_statement(bool inside_generic) {
        SourceLocation loc = current_location();
        expect(TokenType::Keyword_Emit, "expected 'emit'");
        if (!inside_generic) {
            report_error_template_at(loc, ErrorCode::EmitOutsideGenericBlock, {});
        }

        if (current_.type == TokenType::LeftBrace) {
            auto stmt = std::make_unique<EmitStatement>(loc);
            advance();
            skip_newlines();
            ++emit_content_depth_;
            const bool generic_member_context =
                (generic_block_depth_ > 0 || generated_emit_context_) &&
                function_body_depth_ == 0;
            while (current_.type != TokenType::RightBrace &&
                current_.type != TokenType::EndOfFile) {
                skip_newlines();
                if (current_.type == TokenType::RightBrace) { break; }
                std::unique_ptr<Node> item;
                if (generic_member_context) {
                    if (current_.type == TokenType::Keyword_Emit ||
                        at_const_if_statement()) {
                        std::unique_ptr<Statement> compile_time_item =
                            parse_generic_compile_time_item();
                        item = std::move(compile_time_item);
                    } else {
                        const TokenType item_type = current_.type;
                        const bool top_level_item =
                            item_type == TokenType::Keyword_Guide ||
                            item_type == TokenType::Keyword_Clib ||
                            item_type == TokenType::Keyword_Extern ||
                            item_type == TokenType::Keyword_Generics ||
                            item_type == TokenType::Keyword_Namespace ||
                            item_type == TokenType::Keyword_Access ||
                            item_type == TokenType::Keyword_Addition ||
                            item_type == TokenType::At ||
                            item_type == TokenType::Keyword_Export ||
                            item_type == TokenType::Keyword_Struct ||
                            at_struct_attribute() ||
                            ((is_type_start_keyword(item_type) ||
                                item_type == TokenType::Identifier) &&
                                (at_declaration_start() ||
                                    looks_like_operator_definition() ||
                                    (item_type == TokenType::Identifier &&
                                        looks_like_generic_instantiation())));
                        if (top_level_item) {
                            item = parse_top_level();
                        } else {
                            item = parse_statement();
                        }
                    }
                } else {
                    item = parse_statement();
                }
                if (item != nullptr) {
                    stmt->block_items.push_back(std::move(item));
                    continue;
                }
                if (!in_error_recovery_) {
                    report_error(ErrorCode::EmitBlockNotExpandable,
                        "emit block item is not a valid declaration or statement");
                }
                synchronize();
            }
            --emit_content_depth_;
            expect(TokenType::RightBrace, "expected '}' to close emit block");
            expect_stmt_end("emit block");
            return stmt;
        }

        std::vector<std::unique_ptr<Expression>> pieces;
        auto first = parse_expression();
        if (first == nullptr) {
            report_error(ErrorCode::ExpressionSyntaxError,
                "expected string expression after 'emit'");
            return nullptr;
        }
        pieces.push_back(std::move(first));
        while (current_.type == TokenType::Comma) {
            advance();
            skip_newlines();
            auto piece = parse_expression();
            if (piece == nullptr) {
                report_error(ErrorCode::ExpressionSyntaxError,
                    "expected string expression after ','");
                return nullptr;
            }
            pieces.push_back(std::move(piece));
        }
        expect_stmt_end("emit statement");
        return std::make_unique<EmitStatement>(loc, std::move(pieces));
    }

        std::unique_ptr<TopLevel> Parser::parse_condition_statement() {
        std::unique_ptr<Statement> node = parse_condition_node(true);
        if (node == nullptr) {
            return nullptr;
        }
        Statement* raw = node.release();
        TopLevel* top = dynamic_cast<TopLevel*>(raw);
        if (top == nullptr) {
            delete raw;
            return nullptr;
        }
        return std::unique_ptr<TopLevel>(top);
    }

    std::unique_ptr<Statement> Parser::parse_condition_node(bool top_level) {
        SourceLocation loc = current_location();
        if (!expect(TokenType::At, "expected '@'")) {
            return nullptr;
        }
        if (current_.type != TokenType::Identifier &&
            current_.type != TokenType::Keyword_If) {
            report_error(ErrorCode::ExpressionSyntaxError,
                "expected 'cond', 'uncond' or 'if' after '@'");
            synchronize();
            return nullptr;
        }
        std::string directive(current_.lexeme);
        if (directive == "else" || directive == "endif") {
            report_error(ErrorCode::MissingBraces,
                "conditional compilation directive without matching @if");
            advance();
            expect_stmt_end("conditional compilation directive");
            return nullptr;
        }
        if (directive == "cond") {
            advance();
            if (current_.type != TokenType::Identifier) {
                report_error(ErrorCode::ExpressionSyntaxError,
                    "expected condition identifier after '@cond'");
                synchronize();
                return nullptr;
            }
            std::string name(current_.lexeme);
            advance();
            std::unique_ptr<Expression> value = nullptr;
            if (current_.type == TokenType::Assign) {
                advance();
                skip_newlines();
                value = parse_expression();
                if (value == nullptr) {
                    report_error(ErrorCode::ExpressionSyntaxError,
                        "expected expression after '@cond' '='");
                    synchronize();
                    return nullptr;
                }
            }
            expect_stmt_end("condition definition");
            return std::make_unique<CondDefinition>(loc, name, std::move(value));
        }
        if (directive == "uncond") {
            advance();
            if (current_.type != TokenType::Identifier) {
                report_error(ErrorCode::ExpressionSyntaxError,
                    "expected condition identifier after '@uncond'");
                synchronize();
                return nullptr;
            }
            std::string name(current_.lexeme);
            advance();
            expect_stmt_end("condition removal");
            return std::make_unique<UncondDefinition>(loc, name);
        }
        if (directive == "if") {
            advance();
            std::unique_ptr<ConditionalBlock> block = parse_conditional_block(top_level);
            return std::unique_ptr<Statement>(block.release());
        }
        report_error(ErrorCode::ExpressionSyntaxError,
            "expected 'cond', 'uncond' or 'if' after '@'");
        synchronize();
        return nullptr;
    }

    std::unique_ptr<ConditionalBlock> Parser::parse_conditional_block(
        bool top_level, bool consume_endif) {
        SourceLocation loc = current_location();
        auto condition = parse_condition_expression();
        if (condition == nullptr) {
            synchronize();
            return nullptr;
        }
        skip_newlines();

        const bool braced_form = current_.type == TokenType::LeftBrace;
        std::unique_ptr<Statement> then_block = braced_form
            ? (top_level
                ? std::unique_ptr<Statement>(parse_top_level_block())
                : std::unique_ptr<Statement>(parse_block()))
            : parse_conditional_block_form_branch(top_level);

        if (then_block == nullptr) {
            return nullptr;
        }

        skip_newlines();
        std::unique_ptr<Statement> else_block = nullptr;
        if (current_.type == TokenType::At && lookahead(1).lexeme == "else") {
            advance();
            advance();
            if (current_.type == TokenType::Keyword_If) {
                advance();
                std::unique_ptr<AST::ConditionalBlock> nested =
                    parse_conditional_block(top_level, false);
                if (nested == nullptr) {
                    return nullptr;
                }
                else_block.reset(nested.release());
            } else {
                skip_newlines();
                else_block = braced_form
                    ? (top_level
                        ? std::unique_ptr<Statement>(parse_top_level_block())
                        : std::unique_ptr<Statement>(parse_block()))
                    : parse_conditional_block_form_branch(top_level);
                if (else_block == nullptr) {
                    return nullptr;
                }
            }
            skip_newlines();
        }

        if (!braced_form && consume_endif) {
            if (current_.type != TokenType::At ||
                lookahead(1).lexeme != "endif") {
                report_error(ErrorCode::MissingBraces,
                    "@if block form must be closed by @endif");
                synchronize();
                return nullptr;
            }
            advance();
            advance();
            expect_stmt_end("conditional compilation end");
            skip_newlines();
        }

        return std::make_unique<ConditionalBlock>(loc, std::move(condition),
            std::move(then_block), std::move(else_block));
    }

    std::unique_ptr<Statement> Parser::parse_conditional_block_form_branch(
        bool top_level) {
        SourceLocation loc = current_location();
        std::unique_ptr<Statement> branch;

        if (top_level) {
            auto block = std::make_unique<TopLevelBlock>(loc);
            while (current_.type != TokenType::EndOfFile &&
                !(current_.type == TokenType::At &&
                    (lookahead(1).lexeme == "else" ||
                        lookahead(1).lexeme == "endif"))) {
                skip_newlines();
                if (current_.type == TokenType::EndOfFile ||
                    (current_.type == TokenType::At &&
                        (lookahead(1).lexeme == "else" ||
                            lookahead(1).lexeme == "endif"))) {
                    break;
                }
                std::unique_ptr<TopLevel> item = parse_top_level();
                if (item == nullptr) {
                    synchronize();
                    continue;
                }
                block->items.push_back(std::move(item));
            }
            branch = std::move(block);
        } else {
            auto block = std::make_unique<Block>(loc,
                std::vector<std::unique_ptr<Statement>>{});
            while (current_.type != TokenType::EndOfFile &&
                !(current_.type == TokenType::At &&
                    (lookahead(1).lexeme == "else" ||
                        lookahead(1).lexeme == "endif"))) {
                skip_newlines();
                if (current_.type == TokenType::EndOfFile ||
                    (current_.type == TokenType::At &&
                        (lookahead(1).lexeme == "else" ||
                            lookahead(1).lexeme == "endif"))) {
                    break;
                }
                std::unique_ptr<Statement> item = parse_statement();
                if (item == nullptr) {
                    synchronize();
                    continue;
                }
                block->statements.push_back(std::move(item));
            }
            branch = std::move(block);
        }

        return branch;
    }

    std::unique_ptr<Statement> Parser::parse_conditional_branch_top_level() {
        if (current_.type == TokenType::At && lookahead(1).lexeme == "else") {
            return nullptr;
        }
        std::unique_ptr<TopLevel> item = parse_top_level();
        if (item == nullptr) {
            return nullptr;
        }
        if (auto* statement = dynamic_cast<Statement*>(item.get())) {
            item.release();
            return std::unique_ptr<Statement>(statement);
        }
        auto block = std::make_unique<TopLevelBlock>(item->location);
        block->items.push_back(std::move(item));
        return block;
    }

    std::unique_ptr<Statement> Parser::parse_top_level_block() {
        SourceLocation loc = current_location();
        if (!expect(TokenType::LeftBrace, "expected '{' to start block")) {
            return nullptr;
        }
        auto block = std::make_unique<TopLevelBlock>(loc);
        while (current_.type != TokenType::RightBrace &&
            current_.type != TokenType::EndOfFile) {
            skip_newlines();
            if (current_.type == TokenType::RightBrace ||
                current_.type == TokenType::EndOfFile) {
                break;
            }
            if (in_error_recovery_) {
                synchronize();
                continue;
            }
            auto item = parse_top_level();
            if (item != nullptr) {
                block->items.push_back(std::move(item));
            } else if (!in_error_recovery_) {
                report_error(ErrorCode::ExpressionSyntaxError,
                    "failed to parse declaration in block");
                synchronize();
            }
        }
        if (!expect(TokenType::RightBrace, "expected '}' to close block")) {
            return nullptr;
        }
        skip_newlines();
        return block;
    }

    bool Parser::emit_item_starts_with_function_definition() const {
        auto is_type_keyword = [](TokenType t) {
            return is_type_start_keyword(t);
        };

        std::size_t i = 0;
        TokenType tt = lookahead_type(i);
        if (tt != TokenType::Identifier && !is_type_keyword(tt)) { return false; }

        if (lookahead_type(i + 1) == TokenType::Less) {
            int depth = 0;
            std::size_t j = i + 1;
            for (;; ++j) {
                TokenType t = lookahead_type(j);
                if (t == TokenType::Less) { ++depth; continue; }
                if (t == TokenType::Greater) {
                    --depth;
                    if (depth == 0) { ++j; break; }
                    continue;
                }
                if (t == TokenType::EndOfFile || t == TokenType::Newline ||
                    t == TokenType::Semicolon) {
                    return false;
                }
            }
            i = j;
        } else {
            ++i;
            while (lookahead_type(i) == TokenType::ColonColon &&
                lookahead_type(i + 1) == TokenType::Identifier) {
                i += 2;
            }
        }

        for (;;) {
            TokenType t = lookahead_type(i);
            if (t == TokenType::Star) { ++i; continue; }
            if (t == TokenType::LeftBracket) {
                ++i;
                int depth = 1;
                while (depth > 0) {
                    TokenType inner = lookahead_type(i);
                    if (inner == TokenType::EndOfFile || inner == TokenType::Newline ||
                        inner == TokenType::Semicolon) {
                        return false;
                    }
                    if (inner == TokenType::LeftBracket) {
                        ++depth;
                    } else if (inner == TokenType::RightBracket) {
                        --depth;
                    }
                    ++i;
                }
                continue;
            }
            break;
        }

        if (lookahead_type(i) != TokenType::Identifier) { return false; }
        return lookahead_type(i + 1) == TokenType::LeftParen;
    }

}
