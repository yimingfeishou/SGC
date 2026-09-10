// parser/parser.cpp
// 语法分析器实现 —— 递归下降解析 Gallt 源代码
// Parser implementation — recursive-descent parsing of Gallt source code

#include "../parser/parser.hpp"
#include <cctype>
#include <charconv>
#include <string>
#include <system_error>

// 为了简化限定名，引入 AST 命名空间
using namespace gallt::AST;

namespace gallt {

    namespace {
        // 去掉字符串字面量外层引号；返回内部文本
        // Remove the surrounding quotes from a string literal lexeme
        std::string unquote_string(std::string_view lexeme) {
            if (lexeme.size() >= 2 && lexeme.front() == '"' && lexeme.back() == '"') {
                lexeme.remove_prefix(1);
                lexeme.remove_suffix(1);
            }
            return std::string(lexeme);
        }
    }

    // ============================================================================
    // 构造函数 (Constructor)
    // ============================================================================

    Parser::Parser(Lexer& lexer, DiagnosticEngine& diag)
        : lexer_(lexer), diag_(diag) {
        advance();
    }

    // ============================================================================
    // 底层词法辅助 (Lexer Helpers)
    // ============================================================================

    void Parser::advance() {
        // 递归下降解析器只消费一个 token；peek_token 作为可选单步预读存在
        // The recursive-descent parser consumes one token per advance
        current_ = lexer_.next_token();
        has_peek_ = false;
        // 表达式复杂度统计：超过上限时报告 ER 0020，避免病态输入导致栈溢出
        // Expression complexity accounting: report ER 0020 beyond the limit
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
        if (!has_peek_) {
            peek_ = lexer_.next_token();
            has_peek_ = true;
        }
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
        // 复杂度超限后正在丢弃当前构造，抑制随之而来的级联诊断
        // While a too-complex construct is being discarded, suppress cascading errors
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

    void Parser::skip_newlines() {
        // 空行对应多个 Newline token，语法上无意义，直接跳过
        // Blank lines produce multiple Newline tokens; they carry no syntax meaning
        while (current_.type == TokenType::Newline) {
            advance();
        }
    }

    void Parser::synchronize() {
        // 错误恢复必须至少前进一个 token：否则遇到无法构成语句的关键字
        // （else/from/guide/clib/extern 等）时，恢复循环会原地空转，
        // 造成编译挂死并不断累积诊断信息。
        // Error recovery must always make progress: otherwise a keyword that cannot
        // start a statement would make the recovery loops spin forever while
        // diagnostics keep accumulating.
        bool advanced = false;
        while (current_.type != TokenType::EndOfFile) {
            if (current_.type == TokenType::Semicolon ||
                current_.type == TokenType::Newline ||
                current_.type == TokenType::RightBrace) {
                advance();
                return;
            }
            if (current_.is_keyword()) {
                // 遇到关键字说明可能已同步到新语句/顶层声明，恢复普通解析
                // A keyword is likely a new statement/top-level; resume normal parsing
                in_error_recovery_ = false;
                if (!advanced) {
                    // 当前 token 本身就未能被消费，强制跳过它以保证前进
                    // Force progress when nothing has been consumed yet
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

    // ============================================================================
    // 主解析入口 (Main Parse Entry)
    // ============================================================================

    std::unique_ptr<Program> Parser::parse() {
        SourceLocation start_loc = current_location();
        std::vector<std::unique_ptr<TopLevel>> top_levels;

        while (current_.type != TokenType::EndOfFile) {
            // 顶层声明之间允许任意空行
            // Blank lines between top-level declarations are allowed
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

    // ============================================================================
    // 解析顶层节点 (Top-Level Parsing)
    // ============================================================================

    std::unique_ptr<TopLevel> Parser::parse_top_level() {
        // 新的顶层声明：清除上一条的复杂度超限抑制状态
        // New top-level declaration: clear the complexity-limit suppression state
        complexity_limit_hit_ = false;
        switch (current_.type) {
        case TokenType::Keyword_Guide:
            return parse_guide_statement();
        case TokenType::Keyword_Clib:
            return parse_clib_statement();
        case TokenType::Keyword_Extern:
            return parse_extern_declaration();
        case TokenType::Keyword_Struct:
            return parse_struct_definition();
        case TokenType::Keyword_Else:
            // ER 0026: else 语句缺少匹配的 if
            // ER 0026: 'else' without a matching 'if'
            report_error(ErrorCode::ElseWithoutIf, "else statement without matching if");
            advance();
            return nullptr;
        default: {
            TokenType tt = current_.type;
            if (tt == TokenType::Keyword_Int || tt == TokenType::Keyword_Float ||
                tt == TokenType::Keyword_Double || tt == TokenType::Keyword_Char ||
                tt == TokenType::Keyword_Bool || tt == TokenType::Keyword_String ||
                tt == TokenType::Keyword_File || tt == TokenType::Keyword_Void ||
                tt == TokenType::Identifier) {
                return parse_function_definition();
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
        return std::make_unique<ExternDeclaration>(
            loc, std::move(ret_type), func_name, lib_name,
            std::move(param_types), std::move(param_names));
    }

    std::unique_ptr<TopLevel> Parser::parse_function_definition() {
        SourceLocation loc = current_location();
        Type ret_type = parse_type(true);

        if (current_.type != TokenType::Identifier) {
            report_error(ErrorCode::ExpressionSyntaxError,
                "expected function name");
            return nullptr;
        }
        std::string func_name(current_.lexeme);
        advance();

        if (current_.type != TokenType::LeftParen) {
            // 没有 '('，按全局变量声明处理
            // Without '(', parse the remainder as a global variable declaration
            std::optional<size_t> array_size = std::nullopt;
            Type var_type = finish_declarator_type(std::move(ret_type), true, &array_size);
            if (var_type.kind == TypeKind::Void ||
                (var_type.kind == TypeKind::Array && var_type.element_type &&
                    var_type.element_type->kind == TypeKind::Void)) {
                report_error_at(loc, ErrorCode::ExpressionSyntaxError,
                    "global variable cannot have void type");
                return nullptr;
            }
            std::unique_ptr<Initializer> init = nullptr;
            if (match(TokenType::Assign)) {
                init = parse_initializer();
                if (init == nullptr) {
                    report_error(ErrorCode::ExpressionSyntaxError,
                        "invalid global variable initializer");
                }
            }
            expect_stmt_end("global variable declaration");
            return std::make_unique<VariableDeclaration>(
                loc, std::move(var_type), func_name, array_size, std::nullopt, std::move(init));
        }
        advance(); // 消费函数定义左括号 / consume the function opening parenthesis

        auto [param_types, param_names] = parse_parameter_list();
        if (!expect(TokenType::RightParen, "expected ')' after parameter list")) {
            return nullptr;
        }

        auto body = parse_block();
        if (body == nullptr) {
            report_error(ErrorCode::ExpressionSyntaxError, "expected function body");
            return nullptr;
        }

        return std::make_unique<FunctionDefinition>(
            loc, std::move(ret_type), func_name,
            std::move(param_types), std::move(param_names),
            std::move(body));
    }

    std::unique_ptr<StructDefinition> Parser::parse_struct_definition() {
        SourceLocation loc = current_location();
        expect(TokenType::Keyword_Struct, "expected 'struct'");

        if (current_.type != TokenType::Identifier) {
            report_error(ErrorCode::ExpressionSyntaxError,
                "expected struct name");
            return nullptr;
        }
        std::string struct_name(current_.lexeme);
        advance();

        if (!expect(TokenType::LeftBrace, "expected '{' after struct name")) {
            return nullptr;
        }

        std::vector<StructDefinition::Member> members;
        skip_newlines();

        while (current_.type != TokenType::RightBrace && current_.type != TokenType::EndOfFile) {
            skip_newlines();
            if (current_.type == TokenType::RightBrace) {
                break;
            }
            // 允许 void 以支持返回 void 的函数指针成员；最终类型在声明符后验证
            // Allow void here to support function pointers returning void
            Type member_type = parse_type(true);

            if (current_.type != TokenType::Identifier) {
                report_error(ErrorCode::ExpressionSyntaxError,
                    "expected member name");
                break;
            }
            std::string member_name(current_.lexeme);
            advance();

            std::optional<size_t> array_size = std::nullopt;
            member_type = finish_declarator_type(std::move(member_type), true, &array_size);
            if (member_type.kind == TypeKind::Void ||
                (member_type.kind == TypeKind::Array &&
                    member_type.element_type && member_type.element_type->kind == TypeKind::Void)) {
                report_error_at(loc, ErrorCode::ExpressionSyntaxError,
                    "struct member cannot have void type");
                break;
            }
            std::optional<Type> function_pointer_type = std::nullopt;
            if (member_type.kind == TypeKind::Function) {
                function_pointer_type = member_type;
            }

            std::unique_ptr<Initializer> init = nullptr;
            if (match(TokenType::Assign)) {
                init = parse_initializer();
                if (init == nullptr) {
                    report_error(ErrorCode::ExpressionSyntaxError, "invalid initializer");
                }
            }

            if (!expect_stmt_end("struct member")) {
                break;
            }

            members.emplace_back(loc, std::move(member_type), member_name,
                array_size, std::move(function_pointer_type), std::move(init));
        }

        if (!expect(TokenType::RightBrace, "expected '}' to close struct definition")) {
            return nullptr;
        }

        return std::make_unique<StructDefinition>(loc, struct_name, std::move(members));
    }

    // ============================================================================
    // 解析语句 (Statement Parsing)
    // ============================================================================

    std::unique_ptr<Statement> Parser::parse_statement() {
        // 新语句：清除上一条的复杂度超限抑制状态
        // New statement: clear the complexity-limit suppression state
        complexity_limit_hit_ = false;
        // 语句之间的空行不产生语法结构，统一跳过
        // Blank lines between statements do not form syntax, so skip them
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
            // ER 0026: else 语句缺少匹配的 if
            // ER 0026: 'else' without a matching 'if'
            report_error(ErrorCode::ElseWithoutIf, "else statement without matching if");
            advance();
            return nullptr;
        default: {
            TokenType tt = current_.type;
            if (tt == TokenType::Keyword_Int || tt == TokenType::Keyword_Float ||
                tt == TokenType::Keyword_Double || tt == TokenType::Keyword_Char ||
                tt == TokenType::Keyword_Bool || tt == TokenType::Keyword_String ||
                tt == TokenType::Keyword_File ||
                (tt == TokenType::Identifier &&
                    (lexer_.peek_token().type == TokenType::Identifier ||
                        lexer_.peek_token().type == TokenType::Star))) {
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
        if (tt == TokenType::Keyword_Int || tt == TokenType::Keyword_Float ||
            tt == TokenType::Keyword_Double || tt == TokenType::Keyword_Char ||
            tt == TokenType::Keyword_Bool || tt == TokenType::Keyword_String ||
            tt == TokenType::Keyword_File ||
            (tt == TokenType::Identifier &&
                (lexer_.peek_token().type == TokenType::Identifier ||
                    lexer_.peek_token().type == TokenType::Star))) {
            auto var_decl = parse_variable_declaration();
            if (var_decl != nullptr) {
                return var_decl;
            }
        }
        auto expr = parse_expression();
        if (expr != nullptr) {
            return std::make_unique<ExpressionStatement>(current_location(), std::move(expr));
        }
        return nullptr;
    }

    std::unique_ptr<VariableDeclaration> Parser::parse_variable_declaration() {
        SourceLocation loc = current_location();
        // 允许 void 以支持返回 void 的函数指针变量；最终类型在声明符后验证
        // Allow void for function pointers returning void; validate after declarator
        Type var_type = parse_type(true);

        if (current_.type != TokenType::Identifier) {
            report_error(ErrorCode::ExpressionSyntaxError, "expected variable name");
            return nullptr;
        }
        std::string var_name(current_.lexeme);
        advance();

        std::optional<size_t> array_size = std::nullopt;
        var_type = finish_declarator_type(std::move(var_type), true, &array_size);
        if (var_type.kind == TypeKind::Void ||
            (var_type.kind == TypeKind::Array &&
                var_type.element_type && var_type.element_type->kind == TypeKind::Void)) {
            report_error_at(loc, ErrorCode::ExpressionSyntaxError,
                "variable cannot have void type");
            return nullptr;
        }
        std::optional<Type> function_pointer_type = std::nullopt;
        if (var_type.kind == TypeKind::Function) {
            function_pointer_type = var_type;
        }

        std::unique_ptr<Initializer> init = nullptr;
        if (match(TokenType::Assign)) {
            init = parse_initializer();
            if (init == nullptr) {
                report_error(ErrorCode::ExpressionSyntaxError, "invalid initializer");
            }
        }

        expect_stmt_end("variable declaration");

        return std::make_unique<VariableDeclaration>(
            loc, std::move(var_type), var_name,
            array_size, std::move(function_pointer_type), std::move(init));
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
                // 已经报错
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
            // 已报错
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

        // 块嵌套深度保护：避免超深嵌套块让解析/语义/代码生成的递归耗尽栈
        // Block nesting guard: deep nesting would exhaust the stack in later passes
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

    // ============================================================================
    // 解析类型 (Type Parsing)
    // ============================================================================

    Type Parser::parse_type(bool allow_void, bool allow_function_suffix) {
        Type base_type = parse_type_specifier(allow_void);
        if (base_type.kind == TypeKind::Void && !allow_void) {
            report_error(ErrorCode::ExpressionSyntaxError, "void type not allowed here");
        }

        while (true) {
            if (allow_function_suffix && current_.type == TokenType::LeftParen) {
                // 函数指针后缀
                advance();
                std::vector<Type> param_types;
                std::vector<std::string> param_names;
                if (current_.type != TokenType::RightParen) {
                    auto [types, names] = parse_parameter_list();
                    param_types = std::move(types);
                    param_names = std::move(names);
                }
                if (!expect(TokenType::RightParen, "expected ')' after parameter list in function pointer")) {
                    break;
                }
                if (!expect(TokenType::Star, "expected '*' after function pointer parameter list")) {
                    break;
                }
                base_type = Type::make_function(
                    std::make_shared<Type>(base_type),
                    param_types
                );
            }
            else if (current_.type == TokenType::Star) {
                advance();
                base_type = Type::make_pointer(std::make_shared<Type>(base_type));
            }
            else if (current_.type == TokenType::Power) {
                // 声明中连续两个 '*' 会被词法器识别为 '**'，这里拆成两级指针
                // Two adjacent '*' in a declaration lex as '**'; split into two pointer levels
                advance();
                base_type = Type::make_pointer(std::make_shared<Type>(
                    Type::make_pointer(std::make_shared<Type>(base_type))));
            }
            else {
                break;
            }
        }
        return base_type;
    }

    Type Parser::finish_declarator_type(Type base, bool allow_empty_array,
        std::optional<size_t>* out_array_size) {
        // 声明符后缀可能为数组或函数指针；语法示例见 Gallt 0.2.txt §7、§13
        // Declarator suffixes cover arrays and function pointers (§7, §13)
        if (current_.type == TokenType::LeftBracket) {
            SourceLocation loc = current_.location;
            advance();
            std::optional<size_t> size;
            if (current_.type == TokenType::IntegerLiteral) {
                size = try_parse_integer_literal();
                if (!size.has_value()) {
                    report_error_at(loc, ErrorCode::ArraySizeNotConstant,
                        "array size must be a constant integer expression");
                }
                advance();
            }
            else if (current_.type == TokenType::RightBracket) {
                // 空下标；由类型检查器按初始化列表推断长度
                // Empty bracket; the type checker infers the length from the initializer
                if (!allow_empty_array) {
                    report_error_at(loc, ErrorCode::ArraySizeNotConstant,
                        "array size must be specified here");
                }
            }
            else {
                report_error_at(loc, ErrorCode::ArraySizeNotConstant,
                    "array size must be a constant integer expression");
            }
            if (!expect(TokenType::RightBracket, "expected ']' after array size")) {
                // 保持当前 base 并继续恢复
                // Keep current base and continue recovery
            }
            if (out_array_size != nullptr) {
                *out_array_size = size;
            }
            base = Type::make_array(std::make_shared<Type>(std::move(base)), size);
        }

        // 函数指针成员/变量在声明符名称之后书写后缀，例如 int op(int, int)*
        // Function-pointer declarators place the suffix after the name
        if (current_.type == TokenType::LeftParen && base.kind != TypeKind::Function) {
            auto suffix = parse_function_pointer_suffix(base);
            if (suffix.has_value()) {
                base = std::move(suffix.value());
            }
        }
        return base;
    }

    std::optional<Type> Parser::parse_function_pointer_suffix(Type base_type) {
        // 当前 token 必须是 '('；解析签名并消费末尾 '*'
        // The current token must be '('; parse the signature and consume trailing '*'
        SourceLocation loc = current_.location;
        if (current_.type != TokenType::LeftParen) {
            return std::nullopt;
        }
        advance();
        std::vector<Type> params;
        if (current_.type != TokenType::RightParen) {
            auto [types, names] = parse_parameter_list();
            params = std::move(types);
        }
        if (!expect(TokenType::RightParen, "expected ')' after function pointer parameter list")) {
            return std::nullopt;
        }
        if (current_.type != TokenType::Star) {
            report_error_at(loc, ErrorCode::ExpressionSyntaxError,
                "expected '*' after function pointer parameter list");
            return std::nullopt;
        }
        advance();
        // 构成完整函数类型，例如 int(int, int)*
        // Build the complete function type, e.g. int(int, int)*
        return Type::make_function(std::make_shared<Type>(std::move(base_type)),
            std::move(params));
    }

    Type Parser::parse_type_specifier(bool allow_void) {
        switch (current_.type) {
        case TokenType::Keyword_Int:
            advance();
            return Type::make_int();
        case TokenType::Keyword_Float:
            advance();
            return Type::make_float();
        case TokenType::Keyword_Double:
            advance();
            return Type::make_double();
        case TokenType::Keyword_Char:
            advance();
            return Type::make_char();
        case TokenType::Keyword_Bool:
            advance();
            return Type::make_bool();
        case TokenType::Keyword_String:
            advance();
            return Type::make_string();
        case TokenType::Keyword_File:
            // Gallt 0.2.txt §2：file 为不透明对象类型（需 file* 才能操作）
            // Gallt 0.2.txt §2: file is an opaque object type (file* is used for handles)
            advance();
            return Type::make_file();
        case TokenType::Keyword_Void:
            if (!allow_void) {
                report_error(ErrorCode::ExpressionSyntaxError, "void type not allowed here");
            }
            advance();
            return Type::make_void();
        case TokenType::Identifier: {
            std::string name(current_.lexeme);
            advance();
            return Type::make_struct(name);
        }
        default:
            report_error(ErrorCode::ExpressionSyntaxError, "expected type specifier");
            return Type::make_void();
        }
    }

    // ============================================================================
    // 解析形参列表 (Parameter List)
    // ============================================================================

    std::pair<std::vector<Type>, std::vector<std::string>> Parser::parse_parameter_list() {
        std::vector<Type> param_types;
        std::vector<std::string> param_names;

        if (current_.type == TokenType::RightParen) {
            return { std::move(param_types), std::move(param_names) };
        }

        do {
            Type param_type = parse_type(true);

            std::string param_name;
            if (current_.type == TokenType::Identifier) {
                param_name = current_.lexeme;
                advance();
            }

            // 数组参数会退化为指向首元素的指针；函数指针参数可在名称后带签名后缀
            // Array parameters decay to element pointers; function pointers may carry a signature suffix
            if (current_.type == TokenType::LeftBracket) {
                // 消费一个空的或定长的数组维度
                // Consume either an empty or fixed array dimension
                SourceLocation bracket_loc = current_.location;
                advance();
                std::optional<size_t> size;
                if (current_.type == TokenType::IntegerLiteral) {
                    size = try_parse_integer_literal();
                    if (!size.has_value()) {
                        report_error_at(bracket_loc, ErrorCode::ArraySizeNotConstant,
                            "array size must be a constant integer expression");
                    }
                    advance();
                }
                else if (current_.type != TokenType::RightBracket) {
                    report_error_at(bracket_loc, ErrorCode::ArraySizeNotConstant,
                        "array size must be a constant integer expression");
                }
                if (!expect(TokenType::RightBracket, "expected ']' after parameter array dimension")) {
                    break;
                }
                // 在 C 语义中，数组形参的数组维度被忽略并退化为指针
                // In C semantics, array dimensions in parameters are ignored and decay to pointers
                (void)size;
                param_type = Type::make_pointer(std::make_shared<Type>(std::move(param_type)));
            }
            else if (current_.type == TokenType::LeftParen && param_type.kind != TypeKind::Function) {
                auto suffix = parse_function_pointer_suffix(param_type);
                if (suffix.has_value()) {
                    param_type = std::move(suffix.value());
                }
            }
            if (param_type.kind == TypeKind::Void ||
                (param_type.kind == TypeKind::Array &&
                    param_type.element_type && param_type.element_type->kind == TypeKind::Void)) {
                report_error(ErrorCode::VoidParameter, "parameter cannot have void type");
            }

            param_types.push_back(std::move(param_type));
            param_names.push_back(std::move(param_name));

            if (current_.type != TokenType::Comma) {
                break;
            }
            advance();
        } while (true);

        return { std::move(param_types), std::move(param_names) };
    }

    // ============================================================================
    // 解析初始化器 (Initializer)
    // ============================================================================

    std::unique_ptr<Initializer> Parser::parse_initializer() {
        // 初始化列表嵌套深度保护（与块、表达式同一类问题）
        // Initializer nesting guard (same class of problem as blocks/expressions)
        struct InitializerGuard {
            int& depth;
            explicit InitializerGuard(int& d) : depth(d) { ++depth; }
            ~InitializerGuard() { --depth; }
        } guard(initializer_depth_);
        if (initializer_depth_ > kMaxInitializerNesting) {
            report_error(ErrorCode::ExpressionSyntaxError,
                "initializer nesting is too deep (more than " +
                std::to_string(kMaxInitializerNesting) + " levels)");
            complexity_limit_hit_ = true;
            return nullptr;
        }

        if (current_.type == TokenType::LeftBrace) {
            SourceLocation loc = current_location();
            advance();
            std::vector<std::unique_ptr<Initializer>> elements;

            if (current_.type != TokenType::RightBrace) {
                do {
                    auto init = parse_initializer();
                    if (init == nullptr) {
                        report_error(ErrorCode::ExpressionSyntaxError, "invalid initializer element");
                        break;
                    }
                    elements.push_back(std::move(init));
                    if (current_.type != TokenType::Comma) {
                        break;
                    }
                    advance();
                } while (true);
            }

            if (!expect(TokenType::RightBrace, "expected '}' to close initializer")) {
                return nullptr;
            }
            return std::make_unique<ArrayInitializer>(loc, std::move(elements));
        }
        else {
            SourceLocation loc = current_location();
            auto expr = parse_expression();
            if (expr == nullptr) {
                report_error(ErrorCode::ExpressionSyntaxError, "expected expression in initializer");
                return nullptr;
            }
            return std::make_unique<ExpressionInitializer>(loc, std::move(expr));
        }
    }

    // ============================================================================
    // 表达式解析 (Expression Parsing)
    // ============================================================================

    std::unique_ptr<Expression> Parser::parse_expression() {
        // 表达式嵌套深度与长度保护（超限报 ER 0020，而不是让递归撑爆栈）
        // Depth/length guards for expressions: report ER 0020 instead of overflowing
        struct NestingGuard {
            int& depth;
            explicit NestingGuard(int& d) : depth(d) { ++depth; }
            ~NestingGuard() { --depth; }
        } guard(expression_depth_);

        if (expression_depth_ == 1) {
            // 新的顶层表达式：重置 token 统计
            // New top-level expression: reset the token counter
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
            // 复杂度超限：丢弃已经构造的部分 AST，避免后续递归处理
            // Complexity limit hit: drop the partially built AST
            return nullptr;
        }
        return expr;
    }

    std::unique_ptr<Expression> Parser::parse_assignment_expression() {
        auto left = parse_logical_or_expression();
        if (left == nullptr) return nullptr;

        if (current_.type == TokenType::Assign ||
            current_.type == TokenType::PlusAssign ||
            current_.type == TokenType::MinusAssign) {
            SourceLocation loc = current_.location;
            AssignmentExpression::Operator op;
            switch (current_.type) {
            case TokenType::Assign: op = AssignmentExpression::Operator::Assign; break;
            case TokenType::PlusAssign: op = AssignmentExpression::Operator::PlusAssign; break;
            case TokenType::MinusAssign: op = AssignmentExpression::Operator::MinusAssign; break;
            default: op = AssignmentExpression::Operator::Assign; break;
            }
            advance();
            auto right = parse_assignment_expression();
            if (right == nullptr) {
                report_error(ErrorCode::ExpressionSyntaxError, "expected right-hand side of assignment");
                return nullptr;
            }
            return std::make_unique<AssignmentExpression>(loc, std::move(left), op, std::move(right));
        }
        return left;
    }

    std::unique_ptr<Expression> Parser::parse_logical_or_expression() {
        auto left = parse_logical_and_expression();
        if (left == nullptr) return nullptr;

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
        auto left = parse_comparison_expression();
        if (left == nullptr) return nullptr;

        while (current_.type == TokenType::LogicalAnd) {
            SourceLocation loc = current_.location;
            advance();
            auto right = parse_comparison_expression();
            if (right == nullptr) {
                report_error(ErrorCode::ExpressionSyntaxError, "expected right operand of '&&'");
                return nullptr;
            }
            left = std::make_unique<LogicalAndExpression>(loc, std::move(left), std::move(right));
        }
        return left;
    }

    std::unique_ptr<Expression> Parser::parse_comparison_expression() {
        auto left = parse_additive_expression();
        if (left == nullptr) return nullptr;

        while (current_.type == TokenType::Greater ||
            current_.type == TokenType::Less ||
            current_.type == TokenType::Equal ||
            current_.type == TokenType::NotEqual ||
            current_.type == TokenType::GreaterEqual ||
            current_.type == TokenType::LessEqual) {
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
            auto right = parse_additive_expression();
            if (right == nullptr) {
                report_error(ErrorCode::ExpressionSyntaxError, "expected right operand of comparison");
                return nullptr;
            }
            left = std::make_unique<ComparisonExpression>(loc, std::move(left), op, std::move(right));
        }
        return left;
    }

    std::unique_ptr<Expression> Parser::parse_additive_expression() {
        auto left = parse_multiplicative_expression();
        if (left == nullptr) return nullptr;

        while (current_.type == TokenType::Plus || current_.type == TokenType::Minus) {
            SourceLocation loc = current_.location;
            AdditiveExpression::Operator op;
            if (current_.type == TokenType::Plus) op = AdditiveExpression::Operator::Plus;
            else op = AdditiveExpression::Operator::Minus;
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
        if (left == nullptr) return nullptr;

        // Gallt 0.2.txt §3：% 与 *、/ 同级（左结合）
        // Gallt 0.2.txt §3: '%' shares the precedence of '*' and '/'
        while (current_.type == TokenType::Star || current_.type == TokenType::Slash ||
            current_.type == TokenType::Percent) {
            SourceLocation loc = current_.location;
            MultiplicativeExpression::Operator op;
            if (current_.type == TokenType::Star) op = MultiplicativeExpression::Operator::Multiply;
            else if (current_.type == TokenType::Slash) op = MultiplicativeExpression::Operator::Divide;
            else op = MultiplicativeExpression::Operator::Remainder;
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
        if (left == nullptr) return nullptr;

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
            case TokenType::Star: op = UnaryExpression::Operator::Dereference; break;
            // 前缀位置的一元正负号（'-x'、'+x'、'-5'、'-3.14f'）
            // Unary sign operators in prefix position
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
            // 一元位置的 '**' 表示两次解引用（词法器将 '*' '*' 合并为 Power）
            // A leading '**' means double dereference (the lexer merges '*''*' into Power)
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
        if (primary == nullptr) return nullptr;
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
            }
            else if (current_.type == TokenType::LeftParen) {
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
            }
            else if (current_.type == TokenType::Increment) {
                advance();
                base = std::make_unique<PostfixExpression>(
                    loc, std::move(base), PostfixExpression::Operator::Increment);
            }
            else if (current_.type == TokenType::Decrement) {
                advance();
                base = std::make_unique<PostfixExpression>(
                    loc, std::move(base), PostfixExpression::Operator::Decrement);
            }
            else if (current_.type == TokenType::Dot) {
                advance();
                if (current_.type != TokenType::Identifier) {
                    report_error(ErrorCode::ExpressionSyntaxError,
                        "expected member name after '.'");
                    return nullptr;
                }
                std::string member(current_.lexeme);
                advance();
                base = std::make_unique<PostfixExpression>(
                    loc, std::move(base), PostfixExpression::Operator::Dot,
                    nullptr, std::vector<std::unique_ptr<Expression>>{},
                    Type(), member);
            }
            else if (current_.type == TokenType::Arrow) {
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
            }
            else {
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
        case TokenType::Keyword_Input:
        case TokenType::Keyword_Output:
        case TokenType::Keyword_Free:
        case TokenType::Keyword_Size:
        case TokenType::Keyword_Align: {
            // 内置函数在表达式语法中可像标识符一样被调用
            // Built-in functions behave like callable identifiers in expressions
            std::string id(current_.lexeme);
            advance();
            return std::make_unique<PrimaryExpression>(loc, id);
        }
        case TokenType::Keyword_Int:
        case TokenType::Keyword_Float:
        case TokenType::Keyword_Double:
        case TokenType::Keyword_Char:
        case TokenType::Keyword_Bool:
        case TokenType::Keyword_String: {
            // size/align 的参数可以是类型名（int、double、struct 等）
            // size/align may take a type name such as int or double as their argument
            TokenType after = lexer_.peek_token().type;
            if (after == TokenType::RightParen || after == TokenType::Comma) {
                std::string type_name(current_.lexeme);
                advance();
                return std::make_unique<PrimaryExpression>(loc, type_name);
            }
            // 前缀类型转换：int(i)、double(x) 等（Gallt 0.2.txt §7）
            // Prefix type casts such as int(i) and double(x) (§7)
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
        case TokenType::Keyword_File: {
            // file 是不透明对象类型（Gallt 0.2.txt §2/§17）：
            // 可作为 size/align 的类型名参数，但不允许作为类型转换目标
            // file is opaque: allowed as a size/align type-name argument, not as a cast target
            TokenType after = lexer_.peek_token().type;
            if (after == TokenType::RightParen || after == TokenType::Comma) {
                std::string type_name(current_.lexeme);
                advance();
                return std::make_unique<PrimaryExpression>(loc, type_name);
            }
            report_error_at(loc, ErrorCode::InvalidTypeCast,
                "cannot cast to 'file' type");
            advance();
            return nullptr;
        }
        case TokenType::Identifier: {
            std::string id(current_.lexeme);
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
            return std::make_unique<PrimaryExpression>(loc, true);
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

    std::unique_ptr<Statement> Parser::parse_for_init() {
        return parse_declaration_or_statement();
    }

    std::unique_ptr<Expression> Parser::parse_optional_expression() {
        if (current_.type == TokenType::Semicolon || current_.type == TokenType::RightParen) {
            return nullptr;
        }
        return parse_expression();
    }

    // ============================================================================
    // 辅助函数 (Helper Functions)
    // ============================================================================

    bool Parser::is_stmt_end() const {
        return current_.type == TokenType::Semicolon ||
            current_.type == TokenType::Newline ||
            current_.type == TokenType::EndOfFile;
    }

    bool Parser::expect_stmt_end(const std::string& context) {
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
            // 块/结构体的最后一个语句允许由 '}' 直接结束（文档示例为一行函数体）
            // A trailing '}' terminates the final statement of a block (doc examples)
            return true;
        }
        // 既不是分号也不是换行，按语法错误报告
        // Neither semicolon nor newline; report a syntax error
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

    bool Parser::is_valid_identifier(const std::string& name) const {
        if (name.empty()) return false;
        if (!std::isalpha(name[0]) && name[0] != '_') return false;
        for (char c : name) {
            if (!std::isalnum(c) && c != '_') return false;
        }
        return true;
    }

} // namespace gallt
