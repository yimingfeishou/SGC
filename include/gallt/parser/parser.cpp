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

    Parser::Parser(Lexer& lexer, DiagnosticEngine& diag)
        : lexer_(lexer), diag_(diag) {
        ensure_tokens(0);
        token_index_ = 0;
        current_ = tokens_[0];
    }

    void Parser::seed_generic_names(const std::unordered_set<std::string>& names) {
        seen_generics_.insert(names.begin(), names.end());
    }


    void Parser::ensure_tokens(std::size_t n) const {
        while (tokens_.size() <= n && !lexer_exhausted_) {
            Token t = lexer_.next_token();
            if (t.type == TokenType::EndOfFile) {
                lexer_exhausted_ = true;
            }
            tokens_.push_back(std::move(t));
        }
    }

    Token Parser::lookahead(std::size_t n) const {
        ensure_tokens(token_index_ + n);
        std::size_t idx = token_index_ + n;
        if (idx >= tokens_.size()) {
            idx = tokens_.empty() ? 0 : tokens_.size() - 1;
        }
        return tokens_[idx];
    }

    TokenType Parser::lookahead_type(std::size_t n) const {
        return lookahead(n).type;
    }

    void Parser::reset_to(std::size_t m) {
        token_index_ = m;
        ensure_tokens(m);
        if (token_index_ < tokens_.size()) {
            current_ = tokens_[token_index_];
        }
        has_peek_ = false;
    }

    void Parser::advance() {
        ensure_tokens(token_index_ + 1);
        if (token_index_ + 1 < tokens_.size()) {
            ++token_index_;
        }
        current_ = tokens_[token_index_];
        has_peek_ = false;
        if (expression_depth_ > 0 && !complexity_limit_hit_) {
            ++expression_tokens_;
            if (expression_tokens_ > kMaxExpressionTokens) {
                report_error(ErrorCode::ExpressionSyntaxError,
                    "expression is too complex (more than " +
                    std::to_string(kMaxExpressionTokens) + " tokens)");
                complexity_limit_hit_ = true;
            }
        }
    }

    void Parser::peek_token() {
        peek_ = lookahead(1);
        has_peek_ = true;
    }

    bool Parser::match(TokenType type) {
        if (current_.type == type) {
            advance();
            return true;
        }
        return false;
    }

    bool Parser::expect(TokenType type, const std::string& err_msg) {
        if (current_.type == type) {
            advance();
            return true;
        }
        report_error(ErrorCode::ExpressionSyntaxError, err_msg);
        return false;
    }

    void Parser::report_error(ErrorCode code, const std::string& msg) {
        if (complexity_limit_hit_) return;
        diag_.report_error(current_.location, code, msg);
        has_error_ = true;
        in_error_recovery_ = true;
    }

    void Parser::report_error_at(SourceLocation loc, ErrorCode code, const std::string& msg) {
        if (complexity_limit_hit_) return;
        diag_.report_error(loc, code, msg);
        has_error_ = true;
        in_error_recovery_ = true;
    }

    void Parser::report_error_template(ErrorCode code,
        const std::vector<std::string>& values) {
        report_error_template_at(current_.location, code, values);
    }

    void Parser::report_error_template_at(SourceLocation loc, ErrorCode code,
        const std::vector<std::string>& values) {
        if (complexity_limit_hit_) return;
        diag_.report_error_template(loc, code, values);
        has_error_ = true;
        in_error_recovery_ = true;
    }

    void Parser::skip_newlines() {
        while (current_.type == TokenType::Newline) {
            advance();
        }
    }

    void Parser::synchronize() {
        bool advanced = false;
        while (current_.type != TokenType::EndOfFile) {
            if (current_.type == TokenType::Semicolon ||
                current_.type == TokenType::Newline ||
                current_.type == TokenType::RightBrace) {
                advance();
                return;
            }
            if (current_.is_keyword()) {
                in_error_recovery_ = false;
                if (!advanced) {
                    advance();
                }
                return;
            }
            advance();
            advanced = true;
        }
        in_error_recovery_ = false;
    }

    SourceLocation Parser::current_location() const {
        return current_.location;
    }

    std::unique_ptr<Program> Parser::parse() {
        SourceLocation start_loc = current_location();
        std::vector<std::unique_ptr<TopLevel>> top_levels;

        while (current_.type != TokenType::EndOfFile) {
            skip_newlines();
            if (current_.type == TokenType::EndOfFile) {
                break;
            }
            if (in_error_recovery_) {
                synchronize();
                continue;
            }

            auto tl = parse_top_level();
            if (tl != nullptr) {
                top_levels.push_back(std::move(tl));
            }
            else {
                if (!in_error_recovery_) {
                    report_error(ErrorCode::ExpressionSyntaxError, "failed to parse top-level declaration");
                }
                synchronize();
            }
        }

        return std::make_unique<Program>(start_loc, std::move(top_levels));
    }

    std::unique_ptr<TopLevel> Parser::parse_top_level() {
        complexity_limit_hit_ = false;
        switch (current_.type) {
        case TokenType::Keyword_Guide:
            return parse_guide_statement();
        case TokenType::Keyword_Clib:
            return parse_clib_statement();
        case TokenType::Keyword_Extern:
            return parse_extern_declaration();
        case TokenType::Keyword_Generics:
            return parse_generic_definition();
        case TokenType::Keyword_Namespace:
            return parse_namespace_definition();
        case TokenType::Keyword_Access:
            return parse_access_namespace();
        case TokenType::Keyword_Addition:
            return parse_addition_namespace();
        case TokenType::At:
            return parse_condition_statement();
        case TokenType::Keyword_Emit:
            report_error_template(ErrorCode::EmitOutsideGenericBlock, {});
            while (current_.type != TokenType::Newline &&
                current_.type != TokenType::Semicolon &&
                current_.type != TokenType::EndOfFile) {
                advance();
            }
            advance();
            return nullptr;
        case TokenType::Keyword_Export:
            return parse_export_definition();
        case TokenType::Keyword_Struct:
            return parse_struct_definition();
        case TokenType::Keyword_Else:
            report_error(ErrorCode::ElseWithoutIf, "else statement without matching if");
            advance();
            return nullptr;
        case TokenType::Keyword_Return:
            report_error_template(ErrorCode::ReturnOutsideFunction, {});
            while (current_.type != TokenType::Newline &&
                current_.type != TokenType::Semicolon &&
                current_.type != TokenType::EndOfFile) {
                advance();
            }
            advance();
            return nullptr;
        case TokenType::Keyword_If:
        case TokenType::Keyword_For:
        case TokenType::Keyword_While:
        case TokenType::Keyword_Break: {
            report_error_template(ErrorCode::StatementInGlobalScope, {});
            while (current_.type != TokenType::Newline &&
                current_.type != TokenType::EndOfFile) {
                advance();
            }
            skip_newlines();
            return nullptr;
        }
        default: {
            TokenType tt = current_.type;
            if (at_struct_attribute()) {
                return parse_struct_definition();
            }
            if (is_type_start_keyword(tt) || tt == TokenType::Identifier) {
                if (looks_like_operator_definition()) {
                    return parse_operator_definition();
                }
                if (tt == TokenType::Identifier && looks_like_generic_instantiation()) {
                    SourceLocation ref_loc = current_location();
                    GenericRef ref = parse_generic_reference(current_.lexeme);
                    bool declaration_follows = (current_.type == TokenType::Identifier ||
                        current_.type == TokenType::Star || current_.type == TokenType::LeftBracket);
                    if (!declaration_follows) {
                        expect_stmt_end("generic instantiation");
                        return std::make_unique<InstantiationStatement>(ref_loc, std::move(ref));
                    }
                    if (ref.member.empty()) {
                        Type t = Type::make_struct(ref.to_string());
                        t.generic_ref = std::make_shared<GenericRef>(ref);
                        return parse_variable_declaration_with_type(std::move(t));
                    }
                    Type t = Type::make_struct(ref.to_string());
                    t.generic_ref = std::make_shared<GenericRef>(ref);
                    return parse_variable_declaration_with_type(std::move(t));
                }
                if (tt == TokenType::Identifier) {
                    const TokenType next = lookahead_type(1);
                    const bool declaration_follows =
                        next == TokenType::Identifier ||
                        declaration_name_after_pointer_suffix(1);
                    if (!declaration_follows) {
                        report_error_template(ErrorCode::StatementInGlobalScope, {});
                        while (current_.type != TokenType::Newline &&
                            current_.type != TokenType::Semicolon &&
                            current_.type != TokenType::EndOfFile) {
                            advance();
                        }
                        skip_newlines();
                        return nullptr;
                    }
                }
                return parse_function_definition();
            }
            switch (tt) {
            case TokenType::IntegerLiteral:
            case TokenType::FloatLiteral:
            case TokenType::CharLiteral:
            case TokenType::StringLiteral:
            case TokenType::BoolLiteral:
            case TokenType::LeftParen:
            case TokenType::LogicalNot:
            case TokenType::Keyword_Null:
            case TokenType::Keyword_Heap:
                report_error_template(ErrorCode::StatementInGlobalScope, {});
                while (current_.type != TokenType::Newline &&
                    current_.type != TokenType::EndOfFile) {
                    advance();
                }
                skip_newlines();
                return nullptr;
            default:
                break;
            }
            report_error(ErrorCode::ExpressionSyntaxError, "unexpected token at top level");
            return nullptr;
        }
        }
    }

    std::unique_ptr<GuideStatement> Parser::parse_guide_statement() {
        SourceLocation loc = current_location();
        expect(TokenType::Keyword_Guide, "expected 'guide'");
        if (current_.type != TokenType::StringLiteral) {
            report_error(ErrorCode::ExpressionSyntaxError, "expected string literal after 'guide'");
            return nullptr;
        }
        std::string path = unquote_string(current_.lexeme);
        advance();
        expect_stmt_end("guide statement");
        return std::make_unique<GuideStatement>(loc, path);
    }

    std::unique_ptr<ClibStatement> Parser::parse_clib_statement() {
        SourceLocation loc = current_location();
        expect(TokenType::Keyword_Clib, "expected 'clib'");
        if (!expect(TokenType::LeftParen, "expected '(' after 'clib'")) {
            return nullptr;
        }
        if (current_.type != TokenType::StringLiteral) {
            report_error(ErrorCode::ExpressionSyntaxError, "expected string literal inside clib()");
            return nullptr;
        }
        std::string lib = unquote_string(current_.lexeme);
        advance();
        if (!expect(TokenType::RightParen, "expected ')' after clib string literal")) {
            return nullptr;
        }
        expect_stmt_end("clib statement");
        return std::make_unique<ClibStatement>(loc, lib);
    }

    std::unique_ptr<ExternDeclaration> Parser::parse_extern_declaration() {
        SourceLocation loc = current_location();
        expect(TokenType::Keyword_Extern, "expected 'extern'");

        Type ret_type = parse_type(true);

        if (current_.type != TokenType::Identifier) {
            report_error(ErrorCode::ExpressionSyntaxError,
                "expected function name after return type in extern declaration");
            return nullptr;
        }
        std::string func_name(current_.lexeme);
        advance();

        if (!expect(TokenType::Keyword_From, "expected 'from' in extern declaration")) {
            return nullptr;
        }

        if (current_.type != TokenType::Identifier) {
            report_error(ErrorCode::ExpressionSyntaxError,
                "expected library name after 'from'");
            return nullptr;
        }
        std::string lib_name(current_.lexeme);
        advance();

        if (!expect(TokenType::LeftParen, "expected '(' for parameter list")) {
            return nullptr;
        }
        auto [param_types, param_names] = parse_parameter_list();
        if (!expect(TokenType::RightParen, "expected ')' after parameter list")) {
            return nullptr;
        }

        expect_stmt_end("extern declaration");
        auto decl = std::make_unique<ExternDeclaration>(
            loc, std::move(ret_type), func_name, lib_name,
            std::move(param_types), std::move(param_names));
        decl->c_symbol_name = func_name;
        return decl;
    }

    const char* Parser::export_kind_for_token(TokenType type) const {
        switch (type) {
        case TokenType::Keyword_Struct: return "struct definition";
        case TokenType::Keyword_Namespace: return "namespace definition";
        case TokenType::Keyword_Addition: return "addition namespace statement";
        case TokenType::Keyword_Access: return "access namespace statement";
        case TokenType::Keyword_Generics: return "generics definition";
        case TokenType::Keyword_Extern: return "extern declaration";
        case TokenType::Keyword_Guide: return "guide import";
        case TokenType::Keyword_Clib: return "clib import";
        case TokenType::Keyword_Emit: return "emit statement";
        case TokenType::At: return "conditional directive";
        case TokenType::Keyword_Return:
        case TokenType::Keyword_If:
        case TokenType::Keyword_For:
        case TokenType::Keyword_While:
        case TokenType::Keyword_Break:
            return "statement";
        default:
            return "statement";
        }
    }

    std::unique_ptr<TopLevel> Parser::parse_export_member(const char* context) {
        report_error_template(ErrorCode::ExportNotAllowedInContext,
            { std::string(context) });
        advance();
        if (current_.type == TokenType::Keyword_Export) {
            advance();
        }
        return parse_top_level();
    }

    std::unique_ptr<TopLevel> Parser::parse_export_definition() {
        SourceLocation export_loc = current_location();
        advance();
        if (current_.type == TokenType::Keyword_Export) {
            report_error_template_at(export_loc, ErrorCode::ExportNotAllowedInContext,
                { std::string("another export declaration") });
            advance();
        }
        const TokenType kind_token = current_.type;
        switch (kind_token) {
        case TokenType::Keyword_Struct:
        case TokenType::Keyword_Namespace:
        case TokenType::Keyword_Addition:
        case TokenType::Keyword_Access:
        case TokenType::Keyword_Generics:
        case TokenType::Keyword_Extern:
        case TokenType::Keyword_Guide:
        case TokenType::Keyword_Clib:
        case TokenType::Keyword_Emit:
        case TokenType::At:
            report_error_template_at(export_loc, ErrorCode::ExportRequiresFunctionDefinition,
                { std::string(export_kind_for_token(kind_token)) });
            return parse_top_level();
        default:
            break;
        }
        if (is_type_start_keyword(kind_token) || kind_token == TokenType::Identifier) {
            return parse_function_definition(true, export_loc);
        }
        report_error_template_at(export_loc, ErrorCode::ExportRequiresFunctionDefinition,
            { std::string(export_kind_for_token(kind_token)) });
        while (current_.type != TokenType::Newline &&
            current_.type != TokenType::Semicolon &&
            current_.type != TokenType::EndOfFile) {
            advance();
        }
        return nullptr;
    }

    std::vector<std::unique_ptr<TopLevel>> Parser::parse_namespace_members() {
        std::vector<std::unique_ptr<TopLevel>> members;
        skip_newlines();
        while (current_.type != TokenType::RightBrace &&
            current_.type != TokenType::EndOfFile) {
            skip_newlines();
            if (current_.type == TokenType::RightBrace) break;
            if (in_error_recovery_) {
                synchronize();
                continue;
            }

            std::unique_ptr<TopLevel> member;
            switch (current_.type) {
            case TokenType::Keyword_Export:
                member = parse_export_member("a namespace definition");
                break;
            case TokenType::Keyword_Namespace:
                member = parse_namespace_definition();
                break;
            case TokenType::Keyword_Access:
                member = parse_access_namespace();
                break;
            case TokenType::Keyword_Addition:
                member = parse_addition_namespace();
                break;
            case TokenType::Keyword_Struct:
                member = parse_struct_definition();
                break;
            case TokenType::Keyword_Generics:
                member = parse_generic_definition();
                break;
            case TokenType::Keyword_Extern:
                member = parse_extern_declaration();
                break;
            case TokenType::At:
                member = parse_condition_statement();
                break;
            case TokenType::Keyword_Emit:
                report_error_template(ErrorCode::EmitOutsideGenericBlock, {});
                advance();
                while (current_.type != TokenType::Newline &&
                    current_.type != TokenType::Semicolon &&
                    current_.type != TokenType::RightBrace &&
                    current_.type != TokenType::EndOfFile) {
                    advance();
                }
                continue;
            case TokenType::Keyword_Guide:
            case TokenType::Keyword_Clib:
                report_error(ErrorCode::ExpressionSyntaxError,
                    "guide/clib statements are not allowed inside a namespace");
                advance();
                continue;
            default:
                break;
            }

            if (member == nullptr && !in_error_recovery_) {
                if (at_struct_attribute()) {
                    member = parse_struct_definition();
                }
                else if (current_.type == TokenType::Identifier &&
                    at_operator_definition()) {
                    member = parse_operator_definition();
                }
                else if (current_.type == TokenType::Identifier &&
                    looks_like_generic_instantiation() && !at_declaration_start()) {
                    SourceLocation iloc = current_location();
                    GenericRef ref = parse_generic_reference(current_.lexeme);
                    bool ends_statement = (current_.type == TokenType::Semicolon ||
                        current_.type == TokenType::Newline ||
                        current_.type == TokenType::EndOfFile ||
                        current_.type == TokenType::RightBrace);
                    if (ends_statement) {
                        expect_stmt_end("generic instantiation");
                        member = std::make_unique<InstantiationStatement>(iloc,
                            std::move(ref));
                    }
                    else {
                        Type t = Type::make_struct(ref.to_string());
                        t.generic_ref = std::make_shared<GenericRef>(std::move(ref));
                        member = parse_variable_declaration_with_type(std::move(t));
                    }
                }
                else if (at_declaration_start() ||
                    is_type_start_keyword(current_.type)) {
                    member = parse_function_definition();
                }
            }

            if (member != nullptr) {
                members.push_back(std::move(member));
                continue;
            }

            if (!in_error_recovery_) {
                report_error_template(ErrorCode::StatementInGlobalScope, {});
                auto discard = parse_statement();
                (void)discard;
            }
            synchronize();
        }
        return members;
    }

    std::unique_ptr<NamespaceDefinition> Parser::parse_namespace_definition() {
        SourceLocation loc = current_location();
        expect(TokenType::Keyword_Namespace, "expected 'namespace'");
        std::string name;
        if (!check_identifier_name(name, "namespace name")) {
            return nullptr;
        }
        if (!expect(TokenType::LeftBrace, "expected '{' after namespace name")) {
            return nullptr;
        }
        auto def = std::make_unique<NamespaceDefinition>(loc, name);
        def->members = parse_namespace_members();
        if (!expect(TokenType::RightBrace, "expected '}' to close namespace definition")) {
            return nullptr;
        }
        expect_stmt_end("namespace definition");
        return def;
    }

    std::unique_ptr<AccessNamespaceStatement> Parser::parse_access_namespace() {
        SourceLocation loc = current_location();
        expect(TokenType::Keyword_Access, "expected 'access'");
        expect(TokenType::Keyword_Namespace, "expected 'namespace' after 'access'");
        std::vector<std::string> path;
        std::string first;
        if (!check_identifier_name(first, "namespace name after 'access namespace'")) {
            return nullptr;
        }
        path.push_back(first);
        while (current_.type == TokenType::ColonColon) {
            advance();
            std::string part;
            if (!check_identifier_name(part, "identifier after '::'")) {
                return nullptr;
            }
            path.push_back(part);
        }
        expect_stmt_end("access namespace statement");
        return std::make_unique<AccessNamespaceStatement>(loc, std::move(path));
    }

    std::unique_ptr<AdditionNamespaceStatement> Parser::parse_addition_namespace() {
        SourceLocation loc = current_location();
        expect(TokenType::Keyword_Addition, "expected 'addition'");
        expect(TokenType::Keyword_Namespace, "expected 'namespace' after 'addition'");
        std::string name;
        if (!check_identifier_name(name, "namespace name after 'addition namespace'")) {
            return nullptr;
        }
        if (!expect(TokenType::LeftBrace, "expected '{' after namespace name")) {
            return nullptr;
        }
        auto def = std::make_unique<AdditionNamespaceStatement>(loc, name);
        def->members = parse_namespace_members();
        if (!expect(TokenType::RightBrace, "expected '}' to close addition namespace")) {
            return nullptr;
        }
        expect_stmt_end("addition namespace statement");
        return def;
    }

    std::unique_ptr<Statement> Parser::parse_statement() {
        complexity_limit_hit_ = false;
        skip_newlines();
        if (in_error_recovery_) {
            synchronize();
            if (in_error_recovery_) return nullptr;
        }

        switch (current_.type) {
        case TokenType::Keyword_If:
            return parse_if_statement();
        case TokenType::Keyword_For:
            return parse_for_statement();
        case TokenType::Keyword_While:
            return parse_while_statement();
        case TokenType::Keyword_Break:
            return parse_break_statement();
        case TokenType::Keyword_Return:
            return parse_return_statement();
        case TokenType::LeftBrace:
            return parse_block();
        case TokenType::Semicolon:
            return parse_empty_statement();
        case TokenType::Keyword_Struct:
            return parse_struct_definition();
        case TokenType::Keyword_Else:
            report_error(ErrorCode::ElseWithoutIf, "else statement without matching if");
            advance();
            return nullptr;
        default: {
            TokenType tt = current_.type;
            if (tt == TokenType::At) {
                return parse_condition_node(false);
            }
            if (tt == TokenType::Keyword_Export) {
                report_error_template(ErrorCode::ExportNotAllowedInContext,
                    { std::string("a local scope") });
                advance();
                int nested_braces = 0;
                while (current_.type != TokenType::EndOfFile) {
                    if (current_.type == TokenType::LeftBrace) {
                        ++nested_braces;
                    }
                    else if (current_.type == TokenType::RightBrace) {
                        if (nested_braces == 0) break;
                        --nested_braces;
                    }
                    else if (nested_braces == 0 &&
                        (current_.type == TokenType::Newline ||
                            current_.type == TokenType::Semicolon)) {
                        break;
                    }
                    advance();
                }
                return nullptr;
            }
            if (at_struct_attribute()) {
                return parse_struct_definition();
            }
            if ((tt == TokenType::Identifier && at_operator_definition()) ||
                (is_type_start_keyword(tt) && lookahead(1).lexeme == "operator" &&
                    lookahead_type(1) == TokenType::Identifier)) {
                report_error_template(ErrorCode::OperatorOverloadInsideStruct,
                    { std::string(lookahead(1).lexeme) });
                if (tt != TokenType::Identifier) {
                    advance();
                }
                auto skipped = parse_operator_definition();
                (void)skipped;
                return nullptr;
            }
            if (tt == TokenType::Keyword_Emit) {
                return parse_emit_statement(generic_ct_depth_ > 0);
            }
            if (tt == TokenType::Keyword_Access) {
                return parse_access_namespace();
            }
            if (tt == TokenType::Keyword_Namespace || tt == TokenType::Keyword_Addition) {
                report_error(ErrorCode::ExpressionSyntaxError,
                    "namespace definitions are only allowed at global or namespace scope");
                advance();
                return nullptr;
            }
            if (tt == TokenType::Identifier && current_.lexeme == "destruct" &&
                lookahead_type(1) != TokenType::LeftParen) {
                SourceLocation dloc = current_location();
                advance();
                auto target = parse_expression();
                if (target == nullptr) {
                    report_error(ErrorCode::ExpressionSyntaxError,
                        "expected expression after 'destruct'");
                    return nullptr;
                }
                expect_stmt_end("destruct statement");
                return std::make_unique<DestructStatement>(dloc, std::move(target));
            }
           if (tt == TokenType::Identifier && looks_like_generic_instantiation() &&
               !at_declaration_start()) {
                std::size_t start = mark();
                SourceLocation iloc = current_location();
                GenericRef ref = parse_generic_reference(current_.lexeme);
                bool ends_statement = (current_.type == TokenType::Semicolon ||
                    current_.type == TokenType::Newline ||
                    current_.type == TokenType::EndOfFile ||
                    current_.type == TokenType::RightBrace);
                if (ends_statement) {
                    expect_stmt_end("generic instantiation");
                    return std::make_unique<InstantiationStatement>(iloc, std::move(ref));
                }
                reset_to(start);
           }
            if (is_type_start_keyword(tt) || at_declaration_start()) {
                auto var_decl = parse_variable_declaration();
                if (var_decl != nullptr) {
                    return var_decl;
                }
            }
            return parse_expression_statement();
        }
        }
    }

    std::unique_ptr<Statement> Parser::parse_declaration_or_statement() {
        TokenType tt = current_.type;
        if (at_struct_attribute()) {
            return parse_struct_definition();
        }
        if (tt == TokenType::Keyword_Generics) {
            report_error(ErrorCode::GenericStatementNotAllowed,
                "generic definitions are only allowed at the top level");
            advance();
            return nullptr;
        }
        if (is_type_start_keyword(tt) || at_declaration_start()) {
            auto var_decl = parse_variable_declaration();
            if (var_decl != nullptr) {
                return var_decl;
            }
        }
        if (tt == TokenType::Identifier && looks_like_generic_instantiation()) {
            SourceLocation iloc = current_location();
            GenericRef ref = parse_generic_reference(current_.lexeme);
            if (ref.member.empty() && current_.type != TokenType::Identifier) {
                expect_stmt_end("generic instantiation");
                return std::make_unique<InstantiationStatement>(iloc, std::move(ref));
            }
            Type t = Type::make_struct(ref.to_string());
            t.generic_ref = std::make_shared<GenericRef>(ref);
            return parse_variable_declaration_with_type(std::move(t));
        }
        auto expr = parse_expression();
        if (expr != nullptr) {
            return std::make_unique<ExpressionStatement>(current_location(), std::move(expr));
        }
        return nullptr;
    }


    std::unique_ptr<IfStatement> Parser::parse_if_statement() {
        SourceLocation loc = current_location();
        expect(TokenType::Keyword_If, "expected 'if'");

        if (!expect(TokenType::LeftParen, "expected '(' after 'if'")) {
            return nullptr;
        }
        auto cond = parse_expression();
        if (cond == nullptr) {
            report_error(ErrorCode::ExpressionSyntaxError, "expected condition expression");
            return nullptr;
        }
        if (!expect(TokenType::RightParen, "expected ')' after condition")) {
            return nullptr;
        }

        auto then_block = parse_block();
        if (then_block == nullptr) {
            report_error(ErrorCode::MissingBraces, "if statement must be followed by a block");
            return nullptr;
        }

        std::unique_ptr<Statement> else_block = nullptr;
        skip_newlines();
        if (match(TokenType::Keyword_Else)) {
            skip_newlines();
            else_block = parse_block();
            if (else_block == nullptr) {
                report_error(ErrorCode::MissingBraces, "else statement must be followed by a block");
                return nullptr;
            }
        }

        return std::make_unique<IfStatement>(
            loc, std::move(cond), std::move(then_block), std::move(else_block));
    }

    std::unique_ptr<ForStatement> Parser::parse_for_statement() {
        SourceLocation loc = current_location();
        expect(TokenType::Keyword_For, "expected 'for'");

        if (!expect(TokenType::LeftParen, "expected '(' after 'for'")) {
            return nullptr;
        }

        std::unique_ptr<Statement> init_stmt = nullptr;
        if (current_.type != TokenType::Semicolon) {
            init_stmt = parse_declaration_or_statement();
            if (init_stmt == nullptr) {
                report_error(ErrorCode::ExpressionSyntaxError, "invalid for initializer");
            }
        }
        if (init_stmt == nullptr) {
            if (!expect(TokenType::Semicolon, "expected ';' after for initializer")) {
            }
        }
        else {
            if (current_.type == TokenType::Semicolon) {
                advance();
            }
        }

        std::unique_ptr<Expression> cond_expr = nullptr;
        if (current_.type != TokenType::Semicolon) {
            cond_expr = parse_expression();
            if (cond_expr == nullptr) {
                report_error(ErrorCode::ExpressionSyntaxError, "invalid for condition");
            }
        }
        if (!expect(TokenType::Semicolon, "expected ';' after for condition")) {
        }

        std::unique_ptr<Expression> step_expr = nullptr;
        if (current_.type != TokenType::RightParen) {
            step_expr = parse_expression();
            if (step_expr == nullptr) {
                report_error(ErrorCode::ExpressionSyntaxError, "invalid for step expression");
            }
        }
        if (!expect(TokenType::RightParen, "expected ')' after for step")) {
            return nullptr;
        }

        auto body = parse_block();
        if (body == nullptr) {
            report_error(ErrorCode::MissingBraces, "for loop body must be a block");
            return nullptr;
        }

        loop_depth_++;
        loop_depth_--;

        return std::make_unique<ForStatement>(
            loc, std::move(init_stmt), std::move(cond_expr), std::move(step_expr), std::move(body));
    }

    std::unique_ptr<WhileStatement> Parser::parse_while_statement() {
        SourceLocation loc = current_location();
        expect(TokenType::Keyword_While, "expected 'while'");

        if (!expect(TokenType::LeftParen, "expected '(' after 'while'")) {
            return nullptr;
        }
        auto cond = parse_expression();
        if (cond == nullptr) {
            report_error(ErrorCode::ExpressionSyntaxError, "expected condition expression");
            return nullptr;
        }
        if (!expect(TokenType::RightParen, "expected ')' after condition")) {
            return nullptr;
        }

        auto body = parse_block();
        if (body == nullptr) {
            report_error(ErrorCode::MissingBraces, "while loop body must be a block");
            return nullptr;
        }

        loop_depth_++;
        loop_depth_--;

        return std::make_unique<WhileStatement>(loc, std::move(cond), std::move(body));
    }

    std::unique_ptr<BreakStatement> Parser::parse_break_statement() {
        SourceLocation loc = current_location();
        expect(TokenType::Keyword_Break, "expected 'break'");
        expect_stmt_end("break statement");
        return std::make_unique<BreakStatement>(loc);
    }

    std::unique_ptr<ReturnStatement> Parser::parse_return_statement() {
        SourceLocation loc = current_location();
        expect(TokenType::Keyword_Return, "expected 'return'");

        std::unique_ptr<Expression> value = nullptr;
        if (current_.type != TokenType::Semicolon && current_.type != TokenType::Newline &&
            current_.type != TokenType::EndOfFile) {
            value = parse_expression();
            if (value == nullptr) {
                report_error(ErrorCode::ExpressionSyntaxError, "invalid return value expression");
            }
        }

        expect_stmt_end("return statement");
        return std::make_unique<ReturnStatement>(loc, std::move(value));
    }

    std::unique_ptr<Block> Parser::parse_block() {
        SourceLocation loc = current_location();
        if (!expect(TokenType::LeftBrace, "expected '{' to start block")) {
            return nullptr;
        }

        struct BlockGuard {
            int& depth;
            explicit BlockGuard(int& d) : depth(d) { ++depth; }
            ~BlockGuard() { --depth; }
        } guard(block_depth_);
        if (block_depth_ > kMaxBlockNesting) {
            report_error(ErrorCode::ExpressionSyntaxError,
                "block nesting is too deep (more than " +
                std::to_string(kMaxBlockNesting) + " levels)");
            complexity_limit_hit_ = true;
            return nullptr;
        }

        std::vector<std::unique_ptr<Statement>> stmts;
        while (current_.type != TokenType::RightBrace && current_.type != TokenType::EndOfFile) {
            skip_newlines();
            if (current_.type == TokenType::RightBrace || current_.type == TokenType::EndOfFile) {
                break;
            }
            if (in_error_recovery_) {
                synchronize();
                continue;
            }
            auto stmt = parse_statement();
            if (stmt != nullptr) {
                stmts.push_back(std::move(stmt));
            }
            else {
                if (!in_error_recovery_) {
                    report_error(ErrorCode::ExpressionSyntaxError, "failed to parse statement in block");
                }
                synchronize();
            }
        }

        if (!expect(TokenType::RightBrace, "expected '}' to close block")) {
            return nullptr;
        }

        return std::make_unique<Block>(loc, std::move(stmts));
    }

    std::unique_ptr<ExpressionStatement> Parser::parse_expression_statement() {
        SourceLocation loc = current_location();
        auto expr = parse_expression();
        if (expr == nullptr) {
            report_error(ErrorCode::ExpressionSyntaxError, "expected expression");
            return nullptr;
        }
        expect_stmt_end("expression statement");
        return std::make_unique<ExpressionStatement>(loc, std::move(expr));
    }

    std::unique_ptr<EmptyStatement> Parser::parse_empty_statement() {
        SourceLocation loc = current_location();
        expect(TokenType::Semicolon, "expected ';'");
        return std::make_unique<EmptyStatement>(loc);
    }

    std::unique_ptr<Statement> Parser::parse_for_init() {
        return parse_declaration_or_statement();
    }


    bool Parser::is_stmt_end() const {
        if (in_expr_argument_ &&
            (current_.type == TokenType::Greater || current_.type == TokenType::Comma)) {
            return true;
        }
        return current_.type == TokenType::Semicolon ||
            current_.type == TokenType::Newline ||
            current_.type == TokenType::EndOfFile;
    }

    bool Parser::expect_stmt_end(const std::string& context) {
        if (in_expr_argument_ &&
            (current_.type == TokenType::Greater || current_.type == TokenType::Comma)) {
            return true;
        }
        if (current_.type == TokenType::Semicolon) {
            advance();
            return true;
        }
        if (current_.type == TokenType::Newline) {
            advance();
            return true;
        }
        if (current_.type == TokenType::EndOfFile) {
            return true;
        }
        if (current_.type == TokenType::RightBrace) {
            return true;
        }
        report_error(ErrorCode::ExpressionSyntaxError,
            context + " must end with ';' or newline");
        return false;
    }

    std::optional<size_t> Parser::try_parse_integer_literal() {
        if (current_.type != TokenType::IntegerLiteral) {
            return std::nullopt;
        }
        std::string_view lexeme = current_.lexeme;
        int base = 10;
        if (lexeme.size() > 2 && lexeme[0] == '0' && (lexeme[1] == 'x' || lexeme[1] == 'X')) {
            base = 16;
            lexeme = lexeme.substr(2);
        }
        else if (lexeme.size() > 1 && lexeme[0] == '0') {
            base = 8;
            lexeme = lexeme.substr(1);
        }
        while (lexeme.size() > 1) {
            const char back = lexeme.back();
            if (back == 'l' || back == 'L' || back == 'u' || back == 'U') {
                lexeme.remove_suffix(1);
            }
            else {
                break;
            }
        }
        size_t value = 0;
        auto [ptr, ec] = std::from_chars(lexeme.data(), lexeme.data() + lexeme.size(), value, base);
        if (ec != std::errc()) {
            return std::nullopt;
        }
        return value;
    }

    std::optional<size_t> Parser::evaluate_constant_expression(Expression* expr) {
        if (auto* primary = dynamic_cast<PrimaryExpression*>(expr)) {
            if (primary->kind == PrimaryExpression::Kind::Literal &&
                primary->literal_token.type == TokenType::IntegerLiteral) {
                return try_parse_integer_literal();
            }
        }
        return std::nullopt;
    }

}
