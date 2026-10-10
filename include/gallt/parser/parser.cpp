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
        if (try_recover_expected(type, err_msg.c_str())) {
            if (current_.type == type) { advance(); }
            return true;
        }
        return false;
    }

    void Parser::report_error(ErrorCode code, const std::string& msg) {
        if (complexity_limit_hit_) {
            ++suppressed_error_count_;
            return;
        }

        if (!in_error_recovery_) { reset_recovery_node(); }
        diag_.report_error(current_.location, code, msg);
        has_error_ = true;
        in_error_recovery_ = true;
    }

    void Parser::report_error_at(SourceLocation loc, ErrorCode code, const std::string& msg) {
        if (complexity_limit_hit_) {
            ++suppressed_error_count_;
            return;
        }

        if (!in_error_recovery_) { reset_recovery_node(); }
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
        if (complexity_limit_hit_) {
            ++suppressed_error_count_;
            return;
        }

        if (!in_error_recovery_) { reset_recovery_node(); }
        diag_.report_error_template(loc, code, values);
        has_error_ = true;
        in_error_recovery_ = true;
    }

    void Parser::finalize_complexity_limit() {
        if (!complexity_limit_hit_) {
            return;
        }

        complexity_limit_hit_ = false;

        if (suppressed_error_count_ == 0) {
            return;
        }

        std::string summary = "expression is too complex; " +
            std::to_string(suppressed_error_count_) +
            " further diagnostic(s) inside it were suppressed to avoid cascade errors";
        suppressed_error_count_ = 0;

        diag_.report_error(current_.location, ErrorCode::ExpressionSyntaxError, summary);
        has_error_ = true;
    }

    void Parser::skip_newlines() {
        while (current_.type == TokenType::Newline) {
            advance();
        }
    }

    bool Parser::synchronize() {
        bool advanced = false;
        bool stopped_at_right_brace = false;

        while (current_.type != TokenType::EndOfFile) {
            if (current_.type == TokenType::RightBrace) {
                stopped_at_right_brace = true;
                break;
            }

            if (current_.type == TokenType::Semicolon ||
                current_.type == TokenType::Newline) {
                advance();
                break;
            }

            if (current_.is_keyword()) {
                if (!advanced) {
                    advance();
                }

                break;
            }

            advance();
            advanced = true;
        }

        if (!stopped_at_right_brace) {
            in_error_recovery_ = false;
        }

        reset_recovery_node();
        return stopped_at_right_brace;
    }

    SourceLocation Parser::current_location() const {
        return current_.location;
    }

    std::string_view Parser::intern_recovery_lexeme(const std::string& text) {
        recovery_lexemes_.push_back(text);
        return recovery_lexemes_.back();
    }

    bool Parser::recovery_window_ok(std::size_t offset) const {
        return offset <= kRecoveryWindow;
    }

    bool Parser::recovery_is_punctuation(TokenType type) {
        const Token probe(type, SourceLocation{}, std::string_view());
        return probe.is_operator() || probe.is_delimiter();
    }

    bool Parser::recovery_starts_statement(TokenType type) {
        switch (type) {
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
        case TokenType::Keyword_Struct:
        case TokenType::Keyword_Namespace:
        case TokenType::Keyword_Addition:
        case TokenType::Keyword_Access:
        case TokenType::Keyword_Emit:
        case TokenType::Keyword_If:
        case TokenType::Keyword_For:
        case TokenType::Keyword_While:
        case TokenType::Keyword_Switch:
        case TokenType::Keyword_Break:
        case TokenType::Keyword_Continue:
        case TokenType::Keyword_Fallthrough:
        case TokenType::Keyword_Return:
            return true;
        default:
            return false;
        }
    }

    bool Parser::recovery_starts_expression(TokenType type) {
        switch (type) {
        case TokenType::Identifier:
        case TokenType::IntegerLiteral:
        case TokenType::FloatLiteral:
        case TokenType::CharLiteral:
        case TokenType::StringLiteral:
        case TokenType::BoolLiteral:
        case TokenType::Keyword_Null:
        case TokenType::Keyword_Heap:
        case TokenType::Keyword_Cast:
        case TokenType::Keyword_Const:
        case TokenType::Keyword_Generics:
        case TokenType::LeftParen:
        case TokenType::LeftBracket:
        case TokenType::LeftBrace:
        case TokenType::Plus:
        case TokenType::Minus:
        case TokenType::Star:
        case TokenType::Tilde:
        case TokenType::LogicalNot:
        case TokenType::AddressOf:
        case TokenType::Increment:
        case TokenType::Decrement:
            return true;
        default:
            return false;
        }
    }

    bool Parser::recovery_is_noise(TokenType type) {
        return type == TokenType::Semicolon || type == TokenType::Comma;
    }

    bool Parser::recovery_continues_parsing(bool closes_block) const {
        if (!recovery_window_ok(3)) { return false; }

        bool reached_end = false;

        for (std::size_t k = 1; k <= 3; ++k) {
            const Token token = lookahead(k);
            if (token.type == TokenType::Unknown) { return false; }
            if (token.type == TokenType::EndOfFile) {
                if (!closes_block) { return false; }
                reached_end = true;
                break;
            }
            if (reached_end) { return false; }
        }

        return true;
    }

    void Parser::record_recovery(RecoveryKind kind, TokenType expected,
        TokenType found, std::string expected_text, std::string found_text,
        SourceLocation loc) {
        RecoveryRecord record;
        record.kind = kind;
        record.expected = expected;
        record.found = found;
        record.expected_text = std::move(expected_text);
        record.found_text = std::move(found_text);
        record.location = loc;
        recovery_records_.push_back(record);
    }

    bool Parser::apply_recovery_insert(TokenType type, const char* context) {
        (void)context;
        if (!recovery_is_punctuation(type)) { return false; }
        const SourceLocation loc = current_.location;
        const std::string text(token_type_to_string(type));
        const Token inserted(type, loc, intern_recovery_lexeme(text));

        tokens_.insert(tokens_.begin() + static_cast<std::ptrdiff_t>(token_index_),
            inserted);
        current_ = tokens_[token_index_];
        has_peek_ = false;
        in_error_recovery_ = false;
        record_recovery(RecoveryKind::Insert, type, type, text, std::string(), loc);
        return true;
    }

    bool Parser::apply_recovery_delete(const char* context) {
        (void)context;
        if (token_index_ >= tokens_.size() || tokens_.size() <= 1) { return false; }
        const Token removed = tokens_[token_index_];
        if (!recovery_is_punctuation(removed.type)) { return false; }
        const std::string text(removed.lexeme);

        tokens_.erase(tokens_.begin() + static_cast<std::ptrdiff_t>(token_index_));
        reset_to(token_index_);
        in_error_recovery_ = false;
        record_recovery(RecoveryKind::Delete, TokenType::Unknown, removed.type,
            std::string(), text, removed.location);
        return true;
    }

    bool Parser::try_recover_expected(TokenType expected, const char* context) {
        if (recovery_blocked_ || complexity_limit_hit_) { return false; }
        if (expected == TokenType::Unknown || expected == TokenType::Newline ||
            expected == TokenType::EndOfFile) {
            recovery_blocked_ = true;
            return false;
        }
        if (!recovery_window_ok(0) || !recovery_window_ok(3)) {
            recovery_blocked_ = true;
            return false;
        }

        const bool closes_block = expected == TokenType::RightBrace ||
            expected == TokenType::RightParen || expected == TokenType::RightBracket;

        if (recovery_attempts_ < kMaxRecoveryAttempts) {
            ++recovery_attempts_;
            const bool follower_is_safe = recovery_starts_statement(current_.type) ||
                current_.type == TokenType::Newline ||
                current_.type == TokenType::Semicolon ||
                current_.type == TokenType::RightBrace ||
                current_.type == TokenType::EndOfFile;
            const bool insert_allowed = recovery_is_punctuation(expected) &&
                ((expected == TokenType::Semicolon &&
                    recovery_starts_statement(current_.type)) ||
                    (closes_block && follower_is_safe));

            if (insert_allowed && recovery_continues_parsing(closes_block)) {
                return apply_recovery_insert(expected, context);
            }
        }

        if (recovery_attempts_ < kMaxRecoveryAttempts) {
            ++recovery_attempts_;
            const Token next = lookahead(1);
            const bool delete_allowed = recovery_is_punctuation(current_.type) &&
                (next.type == expected || recovery_is_noise(current_.type));

            if (delete_allowed && recovery_continues_parsing(closes_block) &&
                apply_recovery_delete(context)) {
                return true;
            }
        }

        recovery_blocked_ = true;
        return false;
    }

    bool Parser::try_recover_statement_end(const char* context) {
        if (recovery_blocked_ || complexity_limit_hit_) { return false; }
        if (recovery_attempts_ >= kMaxRecoveryAttempts) {
            recovery_blocked_ = true;
            return false;
        }
        if (!recovery_window_ok(0) || !recovery_window_ok(3)) {
            recovery_blocked_ = true;
            return false;
        }
        if (current_.type == TokenType::RightParen ||
            current_.type == TokenType::RightBracket) {
            ++recovery_attempts_;
            return apply_recovery_delete(context);
        }
        if (!recovery_starts_statement(current_.type)) {
            recovery_blocked_ = true;
            return false;
        }

        ++recovery_attempts_;

        if (!recovery_continues_parsing(false)) {
            recovery_blocked_ = true;
            return false;
        }

        return apply_recovery_insert(TokenType::Semicolon, context);
    }

    bool Parser::try_recover_primary_expression(const char* context) {
        if (recovery_blocked_ || complexity_limit_hit_) { return false; }
        if (current_.type == TokenType::Unknown ||
            current_.type == TokenType::EndOfFile ||
            current_.type == TokenType::Newline) {
            return false;
        }
        if (recovery_starts_expression(current_.type)) { return false; }
        if (!recovery_starts_expression(lookahead(1).type)) { return false; }
        if (!recovery_is_punctuation(current_.type)) { return false; }
        if (!recovery_window_ok(0) || !recovery_window_ok(3)) {
            recovery_blocked_ = true;
            return false;
        }
        if (recovery_attempts_ >= kMaxRecoveryAttempts) {
            recovery_blocked_ = true;
            return false;
        }
        ++recovery_attempts_;

        if (!recovery_continues_parsing(false)) {
            recovery_blocked_ = true;
            return false;
        }

        report_error(ErrorCode::ExpressionSyntaxError, context);
        return apply_recovery_delete(context);
    }

    void Parser::reset_recovery_node() {
        recovery_attempts_ = 0;
        recovery_blocked_ = false;
    }

    std::unique_ptr<Program> Parser::parse() {
        SourceLocation start_loc = current_location();
        std::vector<std::unique_ptr<TopLevel>> top_levels;

        while (current_.type != TokenType::EndOfFile) {
            skip_newlines();

            if (current_.type == TokenType::EndOfFile) {
                break;
            }

            if (current_.type == TokenType::RightBrace) {
                report_error(ErrorCode::ExpressionSyntaxError,
                    "unexpected '}' at top level");
                in_error_recovery_ = false;
                advance();
                continue;
            }

            if (in_error_recovery_) {
                synchronize();
                continue;
            }

            auto tl = parse_top_level();

            if (tl != nullptr) {
                top_levels.push_back(std::move(tl));
            } else {
                if (!in_error_recovery_) {
                    report_error(ErrorCode::ExpressionSyntaxError, "failed to parse top-level declaration");
                }
                synchronize();
            }
        }

        return std::make_unique<Program>(start_loc, std::move(top_levels));
    }

    std::unique_ptr<TopLevel> Parser::parse_top_level() {
        finalize_complexity_limit();

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
        case TokenType::Keyword_Switch:
        case TokenType::Keyword_Case:
        case TokenType::Keyword_Break:
        case TokenType::Keyword_Continue:
        case TokenType::Keyword_Fallthrough: {
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
            if (tt == TokenType::Keyword_Const) {
                if (at_const_else_clause()) {
                    report_error(ErrorCode::ElseWithoutIf,
                        "const else statement without matching const if");
                    advance();
                    advance();
                    return nullptr;
                }

                if (at_const_if_statement()) {
                    auto statement = parse_if_statement(true);
                    (void)statement;
                    return nullptr;
                }
            }

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

        bool c_variadic = false;
        bool variadic = false;
        auto [param_types, param_names] = parse_parameter_list(nullptr, &variadic,
            &c_variadic);

        if (variadic) {
            report_error(ErrorCode::ExpressionSyntaxError,
                "an extern declaration may only use an unnamed '...' for variadic parameters");
        }

        if (!expect(TokenType::RightParen, "expected ')' after parameter list")) {
            return nullptr;
        }

        expect_stmt_end("extern declaration");
        auto decl = std::make_unique<ExternDeclaration>(
            loc, std::move(ret_type), func_name, lib_name,
            std::move(param_types), std::move(param_names));
        decl->c_symbol_name = func_name;
        decl->c_variadic = c_variadic;
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
            if (current_.type == TokenType::RightBrace) { break; }

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
                } else if ((current_.type == TokenType::Identifier ||
                    is_type_start_keyword(current_.type)) &&
                    looks_like_operator_definition()) {
                    member = parse_operator_definition();
                } else if (current_.type == TokenType::Identifier &&
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
                    } else {
                        Type t = Type::make_struct(ref.to_string());
                        t.generic_ref = std::make_shared<GenericRef>(std::move(ref));
                        member = parse_variable_declaration_with_type(std::move(t));
                    }
                } else if (at_declaration_start() ||
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
        finalize_complexity_limit();
        skip_newlines();

        if (in_error_recovery_) {
            if (synchronize()) { return nullptr; }
        }

        switch (current_.type) {
        case TokenType::Keyword_If:
            return parse_if_statement();
        case TokenType::Keyword_For:
            return parse_for_statement();
        case TokenType::Keyword_While:
            return parse_while_statement();
        case TokenType::Keyword_Switch:
            return parse_switch_statement();
        case TokenType::Keyword_Break:
            return parse_break_statement();
        case TokenType::Keyword_Continue:
            return parse_continue_statement();
        case TokenType::Keyword_Fallthrough:
            return parse_fallthrough_statement();
        case TokenType::Keyword_Case:
            report_error_template(ErrorCode::ExpressionSyntaxError,
                { std::string("case") });
            {
                int nested_braces = 0;

                while (current_.type != TokenType::EndOfFile) {
                    if (current_.type == TokenType::LeftBrace) {
                        ++nested_braces;
                    } else if (current_.type == TokenType::RightBrace) {
                        if (nested_braces == 0) { break; }
                        --nested_braces;
                    } else if (nested_braces == 0 &&
                        (current_.type == TokenType::Newline ||
                            current_.type == TokenType::Semicolon)) {
                        break;
                    }
                    advance();
                }

                if (current_.type != TokenType::RightBrace) {
                    advance();
                }
            }
            return nullptr;
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
                    } else if (current_.type == TokenType::RightBrace) {
                        if (nested_braces == 0) { break; }
                        --nested_braces;
                    } else if (nested_braces == 0 &&
                        (current_.type == TokenType::Newline ||
                            current_.type == TokenType::Semicolon)) {
                        break;
                    }
                    advance();
                }

                return nullptr;
            }

            if (tt == TokenType::Keyword_Const) {
                if (at_const_else_clause()) {
                    report_error(ErrorCode::ElseWithoutIf,
                        "const else statement without matching const if");
                    advance();
                    advance();
                    return nullptr;
                }

                if (at_const_if_statement()) {
                    return parse_if_statement(true);
                }
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

    std::unique_ptr<IfStatement> Parser::parse_if_statement(bool compile_time) {
        SourceLocation loc = current_location();

        if (compile_time) {
            expect(TokenType::Keyword_Const, "expected 'const'");
        }

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

        if (current_.type != TokenType::LeftBrace) {
            report_error(ErrorCode::MissingBraces, "if statement must be followed by a block");
            return nullptr;
        }

        auto then_block = parse_block();

        if (then_block == nullptr) {
            return nullptr;
        }

        std::unique_ptr<Statement> else_block = nullptr;
        skip_newlines();

        bool const_else = false;

        if (current_.type == TokenType::Keyword_Const &&
            lookahead_type(1) == TokenType::Keyword_Else) {
            const_else = true;
            advance();
        }

        if (match(TokenType::Keyword_Else)) {
            skip_newlines();

            if (current_.type != TokenType::LeftBrace) {
                report_error(ErrorCode::MissingBraces, "else statement must be followed by a block");
                return nullptr;
            }

            else_block = parse_block();

            if (else_block == nullptr) {
                return nullptr;
            }

            if (const_else && !compile_time) {
                report_error_at(loc, ErrorCode::ElseWithoutIf,
                    "const else statement without matching const if");
            }
        }

        auto statement = std::make_unique<IfStatement>(
            loc, std::move(cond), std::move(then_block), std::move(else_block));
        statement->is_compile_time = compile_time;

        if (compile_time && !const_if_allowed()) {
            report_error_template_at(loc,
                ErrorCode::CompileTimeConditionOutsideGenericBlock, {});
        }

        return statement;
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
        } else {
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

        if (current_.type != TokenType::LeftBrace) {
            report_error(ErrorCode::MissingBraces, "for loop body must be a block");
            return nullptr;
        }

        auto body = parse_block();

        if (body == nullptr) {
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

        if (current_.type != TokenType::LeftBrace) {
            report_error(ErrorCode::MissingBraces, "while loop body must be a block");
            return nullptr;
        }

        auto body = parse_block();

        if (body == nullptr) {
            return nullptr;
        }

        loop_depth_++;
        loop_depth_--;

        return std::make_unique<WhileStatement>(loc, std::move(cond), std::move(body));
    }

    std::unique_ptr<SwitchCaseStatement> Parser::parse_switch_statement() {
        SourceLocation loc = current_location();
        expect(TokenType::Keyword_Switch, "expected 'switch'");

        if (!expect(TokenType::LeftParen, "expected '(' after 'switch'")) {
            return nullptr;
        }
        auto cond = parse_expression();

        if (cond == nullptr) {
            report_error(ErrorCode::ExpressionSyntaxError,
                "expected switch condition expression");
            return nullptr;
        }

        if (!expect(TokenType::RightParen, "expected ')' after switch condition")) {
            return nullptr;
        }

        if (current_.type != TokenType::LeftBrace) {
            report_error(ErrorCode::MissingBraces,
                "switch statement must be followed by a block");
            return nullptr;
        }

        advance();
        auto node = std::make_unique<SwitchCaseStatement>(loc, std::move(cond));
        SwitchCaseStatement::Clause* pending_default = nullptr;

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

            if (current_.type == TokenType::Keyword_Case) {
                SourceLocation clause_loc = current_location();
                advance();

                if (!expect(TokenType::LeftParen, "expected '(' after 'case'")) {
                    return nullptr;
                }

                auto case_cond = parse_expression();

                if (case_cond == nullptr) {
                    report_error(ErrorCode::ExpressionSyntaxError,
                        "expected case condition expression");
                    return nullptr;
                }

                if (!expect(TokenType::RightParen, "expected ')' after case condition")) {
                    return nullptr;
                }

                if (current_.type != TokenType::LeftBrace) {
                    report_error(ErrorCode::MissingBraces,
                        "case branch must be followed by a block");
                    return nullptr;
                }

                auto body = parse_block();

                if (body == nullptr) {
                    return nullptr;
                }

                SwitchCaseStatement::Clause clause;
                clause.is_default = false;
                clause.condition = std::move(case_cond);
                clause.statements = std::move(body->statements);
                clause.location = clause_loc;
                node->clauses.push_back(std::move(clause));
                pending_default = nullptr;
                continue;
            }

            auto stmt = parse_statement();

            if (stmt == nullptr) {
                if (!in_error_recovery_) {
                    report_error(ErrorCode::ExpressionSyntaxError,
                        "failed to parse statement in switch-case block");
                }
                synchronize();
                continue;
            }

            if (pending_default == nullptr) {
                SwitchCaseStatement::Clause clause;
                clause.is_default = true;
                clause.location = stmt->location;
                node->clauses.push_back(std::move(clause));
                pending_default = &node->clauses.back();
            }

            pending_default->statements.push_back(std::move(stmt));
        }

        if (!expect(TokenType::RightBrace, "expected '}' to close switch-case")) {
            return nullptr;
        }

        skip_newlines();
        return node;
    }

        std::unique_ptr<BreakStatement> Parser::parse_break_statement() {
        SourceLocation loc = current_location();
        expect(TokenType::Keyword_Break, "expected 'break'");
        expect_stmt_end("break statement");
        return std::make_unique<BreakStatement>(loc);
    }

    std::unique_ptr<ContinueStatement> Parser::parse_continue_statement() {
        SourceLocation loc = current_location();
        expect(TokenType::Keyword_Continue, "expected 'continue'");
        expect_stmt_end("continue statement");
        return std::make_unique<ContinueStatement>(loc);
    }

    std::unique_ptr<FallthroughStatement> Parser::parse_fallthrough_statement() {
        SourceLocation loc = current_location();
        expect(TokenType::Keyword_Fallthrough, "expected 'fallthrough'");
        expect_stmt_end("fallthrough statement");
        return std::make_unique<FallthroughStatement>(loc);
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
            } else {
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
        if (try_recover_statement_end(context.c_str())) { return true; }
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
        } else if (lexeme.size() > 1 && lexeme[0] == '0') {
            base = 8;
            lexeme = lexeme.substr(1);
        }

        while (lexeme.size() > 1) {
            const char back = lexeme.back();

            if (back == 'l' || back == 'L' || back == 'u' || back == 'U') {
                lexeme.remove_suffix(1);
            } else {
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
