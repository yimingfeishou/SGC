// parser/parser.cpp
// 语法分析器实现 —— 递归下降解析 Gallt 源代码
// Parser implementation — recursive-descent parsing of Gallt source code

#include "../parser/parser.hpp"
#include "../semantic/constant_folding.hpp"
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <functional>
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

        // 编译期表达式的近似源码文本（用于 ER 0068 / ER 0069 的 '[expr]' 占位符）
        // Approximate source text of a compile-time expression (the '[expr]'
        // placeholder of ER 0068 / ER 0069)
        std::string compile_time_expr_text(const gallt::AST::Expression* expr) {
            using namespace gallt::AST;
            if (expr == nullptr) return "<empty>";
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
                default: break;
                }
                return std::string(op) + compile_time_expr_text(un->operand.get());
            }
            if (auto* bin = dynamic_cast<const AdditiveExpression*>(expr)) {
                return compile_time_expr_text(bin->left.get()) + " + " +
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

        // 编译期常量折叠（定义见文件末尾的泛型解析小节）；
        // resolve_type_size 用于 size/align 内建在编译期求值（可为空）
        // Compile-time constant folding; resolve_type_size evaluates size/align (may be null)
        bool fold_constant_expression(const gallt::AST::Expression* expr,
            long long& int_out, double& float_out, bool& is_float_out,
            const std::function<bool(const std::string&, std::size_t&, std::size_t&)>*
                resolve_type_size = nullptr);
    }

    // ============================================================================
    // 构造函数 (Constructor)
    // ============================================================================

    Parser::Parser(Lexer& lexer, DiagnosticEngine& diag)
        : lexer_(lexer), diag_(diag) {
        // 预取首个 Token：整条 Token 流按需缓冲，以支持前瞻与回溯
        // Prime the first token; the stream is buffered on demand for lookahead
        ensure_tokens(0);
        token_index_ = 0;
        current_ = tokens_[0];
    }

    // ============================================================================
    // 底层词法辅助 (Lexer Helpers)
    // ============================================================================

    void Parser::ensure_tokens(std::size_t n) const {
        // 按需从词法器拉取 token；到达 EOF 后不再拉取
        // Pull tokens from the lexer on demand; stop once EOF has been produced
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
        // 递归下降解析器只消费一个 token；peek_token 作为可选单步预读存在
        // The recursive-descent parser consumes one token per advance
        ensure_tokens(token_index_ + 1);
        if (token_index_ + 1 < tokens_.size()) {
            ++token_index_;
        }
        current_ = tokens_[token_index_];
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
        // 缓冲式前瞻：直接查缓冲区，不再向前消费词法器
        // Buffered lookahead: read the buffer instead of consuming the lexer
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
        case TokenType::Keyword_Generics:
            // Gallt 0.3.txt §19：主泛型或特化/偏特化定义
            // Gallt 0.3.txt §19: primary generic or specialization definition
            return parse_generic_definition();
        // ---- Gallt 0.4.txt §21：命名空间 ----
        // ---- Gallt 0.4.txt §21: namespaces ----
        case TokenType::Keyword_Namespace:
            return parse_namespace_definition();
        case TokenType::Keyword_Access:
            return parse_access_namespace();
        case TokenType::Keyword_Addition:
            return parse_addition_namespace();
        case TokenType::Keyword_Emit:
            // ER 0106：emit 语句只能出现在泛型块内
            // ER 0106: emit statements may only appear inside a generic block
            report_error_template(ErrorCode::EmitOutsideGenericBlock, {});
            while (current_.type != TokenType::Newline &&
                current_.type != TokenType::Semicolon &&
                current_.type != TokenType::EndOfFile) {
                advance();
            }
            advance();
            return nullptr;
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
            // [nocopy] / [nomove] 修饰的结构体定义
            // Struct definitions carrying [nocopy] / [nomove] attributes
            if (at_struct_attribute()) {
                return parse_struct_definition();
            }
            if (tt == TokenType::Keyword_Int || tt == TokenType::Keyword_Float ||
                tt == TokenType::Keyword_Double || tt == TokenType::Keyword_Char ||
                tt == TokenType::Keyword_Bool || tt == TokenType::Keyword_String ||
                tt == TokenType::Keyword_File || tt == TokenType::Keyword_Void ||
                tt == TokenType::Identifier) {
                if (tt == TokenType::Identifier && looks_like_generic_instantiation()) {
                    // Box<int> / Box<int>.Example 作为实例化语句或泛型类型声明
                    // Instantiation statement or a declaration typed by a generic member
                    SourceLocation ref_loc = current_location();
                    GenericRef ref = parse_generic_reference(current_.lexeme);
                    bool declaration_follows = (current_.type == TokenType::Identifier ||
                        current_.type == TokenType::Star || current_.type == TokenType::LeftBracket);
                    if (!declaration_follows) {
                        // Gallt 0.4.txt §19：`Box<int>` 与 `Box<int>::Member` 都是实例化语句
                        // （`::` 形式同时把该成员的短名引入当前作用域）
                        // Gallt 0.4.txt §19: both `Box<int>` and `Box<int>::Member` are
                        // instantiation statements (`::` also introduces the short name)
                        expect_stmt_end("generic instantiation");
                        return std::make_unique<InstantiationStatement>(ref_loc, std::move(ref));
                    }
                    if (ref.member.empty()) {
                        // 整体实例化后声明：Box<int> x
                        Type t = Type::make_struct(ref.to_string());
                        t.generic_ref = std::make_shared<GenericRef>(ref);
                        return parse_variable_declaration_with_type(std::move(t));
                    }
                    Type t = Type::make_struct(ref.to_string());
                    t.generic_ref = std::make_shared<GenericRef>(ref);
                    return parse_variable_declaration_with_type(std::move(t));
                }
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
        auto decl = std::make_unique<ExternDeclaration>(
            loc, std::move(ret_type), func_name, lib_name,
            std::move(param_types), std::move(param_names));
        // C 导出符号名与源级名称一致（重载命名修饰只影响源级名称）
        // The exported C symbol matches the source-level name (mangling only affects the
        // source-level name)
        decl->c_symbol_name = func_name;
        return decl;
    }

    std::unique_ptr<TopLevel> Parser::parse_function_definition() {
        SourceLocation loc = current_location();
        Type ret_type = parse_type(true);

        std::string func_name;
        if (!check_identifier_name(func_name, "function name")) {
            return nullptr;
        }

        if (current_.type != TokenType::LeftParen) {
            // 没有 '('，按全局变量声明处理
            // Without '(', parse the remainder as a global variable declaration
            std::optional<size_t> array_size = std::nullopt;
            std::unique_ptr<Expression> array_size_expr;
            Type var_type = finish_declarator_type(std::move(ret_type), true, &array_size,
                &array_size_expr);
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
            auto decl = std::make_unique<VariableDeclaration>(
                loc, std::move(var_type), func_name, array_size, std::nullopt, std::move(init));
            decl->array_size_expr = std::move(array_size_expr);
            return decl;
        }
        advance(); // 消费函数定义左括号 / consume the function opening parenthesis

        std::vector<std::unique_ptr<Expression>> param_defaults;
        auto [param_types, param_names] = parse_parameter_list(&param_defaults);
        if (!expect(TokenType::RightParen, "expected ')' after parameter list")) {
            return nullptr;
        }

        auto body = parse_block();
        if (body == nullptr) {
            report_error(ErrorCode::ExpressionSyntaxError, "expected function body");
            return nullptr;
        }

        auto func = std::make_unique<FunctionDefinition>(
            loc, std::move(ret_type), func_name,
            std::move(param_types), std::move(param_names),
            std::move(body));
        func->param_defaults = std::move(param_defaults);
        return func;
    }

    std::unique_ptr<StructDefinition> Parser::parse_struct_definition() {
        SourceLocation loc = current_location();
        bool no_copy = false;
        bool no_move = false;
        // [nocopy] / [nomove] 标记（Gallt 0.3.txt §20）
        // [nocopy] / [nomove] attributes (Gallt 0.3.txt §20)
        while (at_struct_attribute()) {
            advance(); // '['
            std::string attr(current_.lexeme);
            if (attr == "nocopy") no_copy = true;
            else if (attr == "nomove") no_move = true;
            advance(); // 属性名
            expect(TokenType::RightBracket, "expected ']' after struct attribute");
            skip_newlines();
        }
        expect(TokenType::Keyword_Struct, "expected 'struct'");

        std::string struct_name;
        if (!check_identifier_name(struct_name, "struct name")) {
            return nullptr;
        }

        if (!expect(TokenType::LeftBrace, "expected '{' after struct name")) {
            return nullptr;
        }

        std::vector<StructDefinition::Member> members;
        std::vector<std::unique_ptr<SpecialMemberFunction>> special_members;
        skip_newlines();

        while (current_.type != TokenType::RightBrace && current_.type != TokenType::EndOfFile) {
            skip_newlines();
            if (current_.type == TokenType::RightBrace) {
                break;
            }
            // 特殊成员函数定义（Gallt 0.3.txt §20）
            // Special member function definitions (Gallt 0.3.txt §20)
            if (at_special_member_keyword()) {
                auto smf = parse_special_member_function();
                if (smf != nullptr) {
                    special_members.push_back(std::move(smf));
                    skip_newlines();
                    continue;
                }
                break;
            }
            // 构造函数不允许声明返回值类型：`int constructor(...)` 这类写法会被
            // 当作普通成员，这里给出更精确的诊断
            // A constructor may not declare a return type; diagnose it precisely
            bool type_start_token = (current_.type == TokenType::Identifier ||
                current_.type == TokenType::Keyword_Int ||
                current_.type == TokenType::Keyword_Float ||
                current_.type == TokenType::Keyword_Double ||
                current_.type == TokenType::Keyword_Char ||
                current_.type == TokenType::Keyword_Bool ||
                current_.type == TokenType::Keyword_String ||
                current_.type == TokenType::Keyword_File ||
                current_.type == TokenType::Keyword_Void);
            if (type_start_token &&
                lookahead_type(1) == TokenType::Identifier &&
                lookahead_type(2) == TokenType::LeftParen) {
                Token next = lookahead(1);
                if (next.lexeme == "constructor" || next.lexeme == "destructor" ||
                    next.lexeme == "copy_constructor" || next.lexeme == "move_constructor" ||
                    next.lexeme == "copy_assignment" || next.lexeme == "move_assignment") {
                    report_error(ErrorCode::ConstructorWithReturnType,
                        "special member functions must not declare a return type");
                    advance(); // 类型
                    auto smf = parse_special_member_function();
                    if (smf != nullptr) {
                        special_members.push_back(std::move(smf));
                        skip_newlines();
                        continue;
                    }
                    break;
                }
            }
            // 允许 void 以支持返回 void 的函数指针成员；最终类型在声明符后验证
            // Allow void here to support function pointers returning void
            Type member_type = parse_type(true);

            std::string member_name;
            if (!check_identifier_name(member_name, "member name")) {
                break;
            }

            std::optional<size_t> array_size = std::nullopt;
            std::unique_ptr<Expression> member_size_expr;
            member_type = finish_declarator_type(std::move(member_type), true, &array_size,
                &member_size_expr);
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
            members.back().array_size_expr = std::move(member_size_expr);
        }

        if (!expect(TokenType::RightBrace, "expected '}' to close struct definition")) {
            return nullptr;
        }

        auto def = std::make_unique<StructDefinition>(loc, struct_name, std::move(members));
        def->no_copy = no_copy;
        def->no_move = no_move;
        def->special_members = std::move(special_members);
        return def;
    }

    // ============================================================================
    // Gallt 0.4.txt §21：命名空间解析
    // Gallt 0.4.txt §21: namespace parsing
    // ============================================================================

    bool Parser::check_identifier_name(std::string& out, const char* context) {
        if (current_.type == TokenType::Identifier) {
            out = std::string(current_.lexeme);
            advance();
            return true;
        }
        if (current_.is_keyword()) {
            // ER 0099：定义了编译器关键字 '[identifier]'
            // ER 0099: a compiler keyword was defined as an identifier
            report_error_template(ErrorCode::KeywordAsIdentifier,
                { std::string(current_.lexeme) });
            out = std::string(current_.lexeme);
            advance();
            return false;
        }
        report_error(ErrorCode::ExpressionSyntaxError,
            std::string("expected ") + context);
        return false;
    }

    std::vector<std::unique_ptr<TopLevel>> Parser::parse_namespace_members() {
        // 命名空间体的成员：结构体 / 函数 / 全局变量 / 泛型 / 嵌套命名空间 /
        // access namespace / addition namespace / 实例化语句
        // Namespace body members: structs, functions, globals, generics, nested
        // namespaces, access/addition namespace and instantiation statements
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
            case TokenType::Keyword_Emit:
                // emit 只能出现在泛型块内（ER 0106）
                // emit may only appear inside a generic block (ER 0106)
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
                    looks_like_generic_instantiation() && !at_declaration_start()) {
                    // 命名空间内的实例化语句：Box<int> / Box<int>::Member
                    // Instantiation statement inside a namespace
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
                    current_.type == TokenType::Keyword_Int ||
                    current_.type == TokenType::Keyword_Float ||
                    current_.type == TokenType::Keyword_Double ||
                    current_.type == TokenType::Keyword_Char ||
                    current_.type == TokenType::Keyword_Bool ||
                    current_.type == TokenType::Keyword_String ||
                    current_.type == TokenType::Keyword_File ||
                    current_.type == TokenType::Keyword_Void) {
                    // 函数定义或命名空间级变量声明
                    // Function definition or namespace-scope variable declaration
                    member = parse_function_definition();
                }
            }

            if (member != nullptr) {
                members.push_back(std::move(member));
                continue;
            }

            // 其余内容是可执行语句：命名空间成员位于全局作用域，不允许可执行语句
            // Anything else is an executable statement: namespace members live at
            // global scope, where executable statements are rejected (ER 0028)
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
        // `access namespace ns::member` 只引入单个对象（Gallt 0.4.txt §21）
        // `access namespace ns::member` introduces a single object (Gallt 0.4.txt §21)
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

    // ============================================================================
    // Gallt 0.4.txt §19：编译期代码生成（Emit）解析
    // Gallt 0.4.txt §19: compile-time code generation (Emit) parsing
    // ============================================================================

    std::unique_ptr<EmitStatement> Parser::parse_emit_statement(bool inside_generic) {
        SourceLocation loc = current_location();
        expect(TokenType::Keyword_Emit, "expected 'emit'");
        if (!inside_generic) {
            // ER 0106：emit 语句只能出现在泛型块内
            // ER 0106: emit statements may only appear inside a generic block
            report_error_template_at(loc, ErrorCode::EmitOutsideGenericBlock, {});
        }

        if (current_.type == TokenType::LeftBrace) {
            // Emit Block：AST 插入
            // Emit Block: AST insertion
            auto stmt = std::make_unique<EmitStatement>(loc);
            advance();                       // '{'
            skip_newlines();
            while (current_.type != TokenType::RightBrace &&
                current_.type != TokenType::EndOfFile) {
                skip_newlines();
                if (current_.type == TokenType::RightBrace) break;
                std::unique_ptr<Node> item;
                if (current_.type == TokenType::Keyword_Struct || at_struct_attribute()) {
                    item = parse_struct_definition();
                }
                else if (emit_item_starts_with_function_definition()) {
                    item = parse_function_definition();
                }
                else {
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
            expect(TokenType::RightBrace, "expected '}' to close emit block");
            expect_stmt_end("emit block");
            return stmt;
        }

        // Emit String：编译期字符串片段序列（逗号分隔，按顺序拼接）
        // Emit String: a sequence of compile-time string pieces, concatenated in order
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

    std::unique_ptr<Statement> Parser::parse_generic_compile_time_item() {
        // 泛型块顶部的编译期项：emit（字符串/块）与编译期 if（Gallt 0.4.txt §19）
        // Compile-time items at the top of a generic block: emit and compile-time if
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

    bool Parser::emit_item_starts_with_function_definition() const {
        // 从当前位置扫描 `类型 名称 (`：类型可含泛型实参列表、`::` 限定、
        // 指针与数组后缀（Gallt 0.4.txt §19 Emit Block）
        // Scan `type name (` from the current position; the type may carry a generic
        // argument list, `::` qualification, and pointer/array suffixes
        auto is_type_keyword = [](TokenType t) {
            return t == TokenType::Keyword_Int || t == TokenType::Keyword_Float ||
                t == TokenType::Keyword_Double || t == TokenType::Keyword_Char ||
                t == TokenType::Keyword_Bool || t == TokenType::Keyword_String ||
                t == TokenType::Keyword_File || t == TokenType::Keyword_Void;
        };

        std::size_t i = 0;
        TokenType tt = lookahead_type(i);
        if (tt != TokenType::Identifier && !is_type_keyword(tt)) return false;

        // 类型说明符（可嵌套泛型实参）
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
        }
        else {
            ++i;
            // `A::B::T`
            while (lookahead_type(i) == TokenType::ColonColon &&
                lookahead_type(i + 1) == TokenType::Identifier) {
                i += 2;
            }
        }

        // 指针 / 数组后缀
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
                    if (inner == TokenType::LeftBracket) ++depth;
                    else if (inner == TokenType::RightBracket) --depth;
                    ++i;
                }
                continue;
            }
            break;
        }

        // 名称 + '('
        if (lookahead_type(i) != TokenType::Identifier) return false;
        return lookahead_type(i + 1) == TokenType::LeftParen;
    }

    std::unique_ptr<Expression> Parser::parse_property_argument() {
        // 编译期属性实参：字符串字面量（has_member）或类型名/参数标识符
        // Compile-time property argument: a string literal (has_member) or a type name
        SourceLocation loc = current_location();
        if (current_.type == TokenType::StringLiteral) {
            Token lit = current_;
            advance();
            return std::make_unique<PrimaryExpression>(loc, lit);
        }
        // 其它字面量也接受：由语义层报告 ER 0111（必须为字符串字面量）
        // Other literals are accepted so the semantic layer can report ER 0111
        if (current_.type == TokenType::IntegerLiteral ||
            current_.type == TokenType::FloatLiteral ||
            current_.type == TokenType::CharLiteral ||
            current_.type == TokenType::BoolLiteral) {
            Token lit = current_;
            advance();
            return std::make_unique<PrimaryExpression>(loc, lit);
        }
        auto is_type_keyword = [](TokenType t) {
            return t == TokenType::Keyword_Int || t == TokenType::Keyword_Float ||
                t == TokenType::Keyword_Double || t == TokenType::Keyword_Char ||
                t == TokenType::Keyword_Bool || t == TokenType::Keyword_String ||
                t == TokenType::Keyword_File;
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
            // `T*`、`T**`、`T[]` 等模式后缀
            // Pattern suffixes such as `T*`, `T**` and `T[]`
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
            if (at_struct_attribute()) {
                return parse_struct_definition();
            }
            // ---- Gallt 0.4.txt §19/§21：编译期代码生成与命名空间 ----
            // ---- Gallt 0.4.txt §19/§21: compile-time code generation, namespaces ----
            if (tt == TokenType::Keyword_Emit) {
                // 泛型块顶部的编译期项由 generic_ct_depth_ 标记（Gallt 0.4.txt §19）
                // Compile-time items at the top of a generic block are marked by
                // generic_ct_depth_ (Gallt 0.4.txt §19)
                return parse_emit_statement(generic_ct_depth_ > 0);
            }
            if (tt == TokenType::Keyword_Access) {
                return parse_access_namespace();
            }
            if (tt == TokenType::Keyword_Namespace || tt == TokenType::Keyword_Addition) {
                // 命名空间定义与追加命名空间只允许出现在全局或命名空间作用域
                // （命名空间体成员位于全局作用域，与 C++20 的规则一致）
                // Namespace and addition-namespace definitions are only allowed at
                // global or namespace scope (same rule as C++20)
                report_error(ErrorCode::ExpressionSyntaxError,
                    "namespace definitions are only allowed at global or namespace scope");
                advance();
                return nullptr;
            }
            // destruct [表达式]（Gallt 0.3.txt §20）
            // destruct [expression] (Gallt 0.3.txt §20)
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
           // 泛型实例化语句：Box<int> 或 Box<int>.Example
           // Generic instantiation statement: Box<int> or Box<int>.Example
           if (tt == TokenType::Identifier && looks_like_generic_instantiation() &&
               !at_declaration_start()) {
                // 仅有实例化引用后紧跟语句结束符时才是实例化语句；
                // 否则按限定名表达式（例如 Box<int>.Returner(e)）解析
                // Only a bare instantiation followed by a statement terminator is an
                // instantiation statement; otherwise it is a qualified expression
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
            if (tt == TokenType::Keyword_Int || tt == TokenType::Keyword_Float ||
                tt == TokenType::Keyword_Double || tt == TokenType::Keyword_Char ||
                tt == TokenType::Keyword_Bool || tt == TokenType::Keyword_String ||
                tt == TokenType::Keyword_File || at_declaration_start()) {
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
        if (tt == TokenType::Keyword_Int || tt == TokenType::Keyword_Float ||
            tt == TokenType::Keyword_Double || tt == TokenType::Keyword_Char ||
            tt == TokenType::Keyword_Bool || tt == TokenType::Keyword_String ||
            tt == TokenType::Keyword_File || at_declaration_start()) {
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

    std::unique_ptr<VariableDeclaration> Parser::parse_variable_declaration() {
        SourceLocation loc = current_location();
        // 允许 void 以支持返回 void 的函数指针变量；最终类型在声明符后验证
        // Allow void for function pointers returning void; validate after declarator
        Type var_type = parse_type(true);

        std::string var_name;
        if (!check_identifier_name(var_name, "variable name")) {
            return nullptr;
        }

        std::optional<size_t> array_size = std::nullopt;
        std::unique_ptr<Expression> array_size_expr;
        var_type = finish_declarator_type(std::move(var_type), true, &array_size,
            &array_size_expr);
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

        auto decl = std::make_unique<VariableDeclaration>(
            loc, std::move(var_type), var_name,
            array_size, std::move(function_pointer_type), std::move(init));
        decl->array_size_expr = std::move(array_size_expr);
        return decl;
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
        std::optional<size_t>* out_array_size,
        std::unique_ptr<Expression>* out_size_expr) {
        // 声明符后缀可能为（多维）数组或函数指针；语法示例见 Gallt 0.3.txt §7、§13
        // 多维数组：连续的下标维度，第一个为最外层维度（int m[2][3] 等价 C 的
        // [2 x [3 x int]]）。允许省略的只有第一维（由初始化列表推断）。
        // Declarator suffixes cover (multi-dimensional) arrays and function pointers
        // (§7, §13): successive dimensions, the first being outermost. Only the first
        // dimension may be omitted (inferred from the initializer).
        if (current_.type == TokenType::LeftBracket) {
            std::vector<std::optional<size_t>> dimensions;
            std::vector<std::unique_ptr<Expression>> dimension_exprs;
            while (current_.type == TokenType::LeftBracket) {
                SourceLocation loc = current_.location;
                advance();
                const bool is_first_dimension = dimensions.empty();
                std::optional<size_t> size;
                std::unique_ptr<Expression> size_expr;
                if (current_.type == TokenType::IntegerLiteral) {
                    size = try_parse_integer_literal();
                    if (!size.has_value()) {
                        report_error_at(loc, ErrorCode::ArraySizeNotConstant,
                            "array size must be a constant integer expression");
                    }
                    advance();
                }
                else if (current_.type == TokenType::RightBracket) {
                    // 空下标；只有第一维可由初始化列表推断长度
                    // Empty bracket; only the first dimension may be inferred
                    if (!is_first_dimension || !allow_empty_array) {
                        report_error_at(loc, ErrorCode::ArraySizeNotConstant,
                            "array size must be specified here");
                    }
                }
                else {
                    // 非常量字面量：解析编译期常量表达式（可能引用泛型编译期常量参数）
                    // Non-literal: parse a compile-time constant expression (may reference a
                    // generic compile-time constant parameter)
                    auto expr = parse_compile_time_expression();
                    if (expr != nullptr) {
                        long long iv = 0;
                        double dv = 0.0;
                        bool is_flt = false;
                        if (fold_constant_expression(expr.get(), iv, dv, is_flt) && !is_flt && iv >= 0) {
                            size = static_cast<size_t>(iv);
                        }
                        else if (is_first_dimension && out_size_expr != nullptr) {
                            // 交由泛型展开阶段或语义阶段解析（当前仅第一维支持）
                            // Let the generic-expansion / semantic phases resolve it
                            // (currently only the outermost dimension)
                            *out_size_expr = std::move(expr);
                        }
                        else {
                            report_error_at(loc, ErrorCode::ArraySizeNotConstant,
                                "array size must be a constant integer expression");
                        }
                    }
                }
                if (!expect(TokenType::RightBracket, "expected ']' after array size")) {
                    // 保持当前 base 并继续恢复
                    // Keep current base and continue recovery
                    break;
                }
                dimensions.push_back(size);
                dimension_exprs.push_back(std::move(size_expr));
            }
            if (!dimensions.empty()) {
                if (out_array_size != nullptr) {
                    // 声明上的 array_size 始终表示最外层维度（其余维度在类型中）
                    // The declarator's array_size is always the outermost dimension
                    *out_array_size = dimensions.front();
                }
                // 从最内层向外构建，使第一维成为最外层
                // Build from the innermost dimension outward so the first one is outermost
                for (std::size_t i = dimensions.size(); i > 0; --i) {
                    base = Type::make_array(std::make_shared<Type>(std::move(base)),
                        dimensions[i - 1]);
                }
            }
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
            // 泛型实例成员类型：Box<int>.Example、ns::Box<int>.Example
            // （Gallt 0.4.txt §19/§21，泛型名可带命名空间路径）
            // Instantiated generic member type; the generic name may carry a namespace
            // path (Gallt 0.4.txt §19/§21)
            if (looks_like_generic_instantiation()) {
                GenericRef ref = parse_generic_reference(name);
                Type t = Type::make_struct(ref.to_string());
                t.generic_ref = std::make_shared<GenericRef>(std::move(ref));
                return t;
            }
            // 命名空间限定类型名：mynamespace::Inner（Gallt 0.4.txt §21）
            // Namespace-qualified type name: mynamespace::Inner (Gallt 0.4.txt §21)
            if (lookahead_type(1) == TokenType::ColonColon) {
                std::string path = name;
                advance();                       // 标识符
                while (current_.type == TokenType::ColonColon) {
                    advance();                   // '::'
                    if (current_.type != TokenType::Identifier) {
                        report_error(ErrorCode::ExpressionSyntaxError,
                            "expected identifier after '::' in type name");
                        return Type::make_void();
                    }
                    path += "::";
                    path += std::string(current_.lexeme);
                    advance();
                }
                return Type::make_struct(path);
            }
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

    std::pair<std::vector<Type>, std::vector<std::string>> Parser::parse_parameter_list(
        std::vector<std::unique_ptr<Expression>>* defaults) {
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
                // 消费（可能多维的）数组维度：第 0 维按 C 语义忽略，
                // 其余维度构成元素类型，整个形参退化为指向该元素类型的指针。
                // 例如 int m[2][3] 形参 → int(*)[3]。
                // Consume the (possibly multi-dimensional) array suffix: dimension 0 is
                // ignored per C semantics, the remaining ones form the element type, and the
                // parameter decays to a pointer to that element type (int m[2][3] -> int(*)[3])
                std::vector<std::optional<size_t>> dimensions;
                while (current_.type == TokenType::LeftBracket) {
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
                    dimensions.push_back(size);
                }
                for (std::size_t i = dimensions.size(); i > 1; --i) {
                    param_type = Type::make_array(std::make_shared<Type>(std::move(param_type)),
                        dimensions[i - 1]);
                }
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

            // 默认参数（Gallt 0.3.txt §8）
            // Default arguments (Gallt 0.3.txt §8)
            std::unique_ptr<Expression> default_value = nullptr;
            if (current_.type == TokenType::Assign) {
                advance();
                default_value = parse_expression();
                if (default_value == nullptr) {
                    report_error(ErrorCode::ExpressionSyntaxError,
                        "expected expression for default argument");
                }
                if (defaults == nullptr) {
                    report_error(ErrorCode::ExpressionSyntaxError,
                        "default arguments are not allowed in this declaration");
                }
                else if (param_name.empty()) {
                    report_error(ErrorCode::ExpressionSyntaxError,
                        "parameter with a default argument must have a name");
                }
            }
            param_types.push_back(std::move(param_type));
            param_names.push_back(std::move(param_name));
            if (defaults != nullptr) {
                defaults->push_back(std::move(default_value));
            }

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
                // 编译期属性：`[参数].is_same<...>`、`.is_convertible<...>`、
                // `.has_member<"...">`（Gallt 0.4.txt §19）
                // Compile-time properties with an argument list (Gallt 0.4.txt §19)
                if ((member == "is_same" || member == "is_convertible" ||
                    member == "has_member") && current_.type == TokenType::Less) {
                    advance();               // '<'
                    std::vector<std::unique_ptr<Expression>> args;
                    if (current_.type != TokenType::Greater) {
                        do {
                            auto arg = parse_property_argument();
                            if (arg == nullptr) return nullptr;
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
            else if (current_.type == TokenType::ColonColon) {
                // `::` 的左操作数必须是命名空间或泛型实例（Gallt 0.4.txt §21 / ER 0105）。
                // 命名空间限定名与泛型按成员实例化在基本表达式阶段已整体解析，因此
                // 走到这里说明左操作数是一个普通表达式。
                // The left operand of '::' must be a namespace or generic instance
                // (Gallt 0.4.txt §21 / ER 0105). Qualified names and generic member
                // instantiations are consumed by the primary-expression parser, so
                // reaching this point means the left operand is an ordinary expression.
                report_error_template(ErrorCode::ScopeOperatorOperandInvalid,
                    { std::string("expression") });
                advance();                       // '::'
                if (current_.type == TokenType::Identifier) {
                    advance();                   // 成员名
                }
                break;
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
            TokenType after = lookahead_type(1);
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
            TokenType after = lookahead_type(1);
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
            // 泛型实例成员限定名：Box<int>.Example / Box<int>::Returner
            // 也支持命名空间限定的泛型名：ns::Box<int>.Example（Gallt 0.4.txt §21）
            // Qualified instantiated generic member name; namespace-qualified generic
            // names such as ns::Box<int>.Example are supported too (Gallt 0.4.txt §21)
            if (looks_like_generic_instantiation()) {
                GenericRef ref = parse_generic_reference(id);
                return std::make_unique<PrimaryExpression>(loc, std::move(ref));
            }
            // 命名空间限定名 A::B::member（Gallt 0.4.txt §21）
            // Namespace-qualified name A::B::member (Gallt 0.4.txt §21)
            if (lookahead_type(1) == TokenType::ColonColon) {
                std::vector<std::string> path;
                path.push_back(id);
                advance();                       // 标识符
                while (current_.type == TokenType::ColonColon) {
                    advance();                   // '::'
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
            // copy / move / deep_copy / shallow_copy（Gallt 0.3.txt §20）
            if (lookahead_type(1) == TokenType::LeftParen &&
                (id == "copy" || id == "move" || id == "deep_copy" || id == "shallow_copy")) {
                PrimaryExpression::CopyMoveKind cm = PrimaryExpression::CopyMoveKind::Copy;
                if (id == "move") cm = PrimaryExpression::CopyMoveKind::Move;
                else if (id == "deep_copy") cm = PrimaryExpression::CopyMoveKind::DeepCopy;
                else if (id == "shallow_copy") cm = PrimaryExpression::CopyMoveKind::ShallowCopy;
                advance(); // 名称
                advance(); // '('
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
            // construct [类型]([实参]) [at [指针]]（Gallt 0.3.txt §20）
            bool construct_type_start = false;
            switch (lookahead_type(1)) {
            case TokenType::Identifier:
            case TokenType::Keyword_Int:
            case TokenType::Keyword_Float:
            case TokenType::Keyword_Double:
            case TokenType::Keyword_Char:
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
                advance(); // 'construct'
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

    // ============================================================================
    // Gallt 0.3：编译期泛型解析 (Gallt 0.3.txt §19)
    // Gallt 0.3: compile-time generics parsing (Gallt 0.3.txt §19)
    // ============================================================================

    namespace {
        // 编译期常量表达式的折叠（仅字面量与纯算术）
        // Fold a compile-time constant expression (literals and pure arithmetic only)
        struct ConstValue {
            bool valid = false;
            bool is_float = false;
            long long int_value = 0;
            double float_value = 0.0;
            std::string text;   // 非常量字面量（标识符引用）时保存规范化文本
        };

        // 解析字符字面量的取值，支持简单转义
        // Decode a character literal, supporting simple escape sequences
        bool decode_char_literal(std::string_view lexeme, long long& out) {
            if (lexeme.size() < 3 || lexeme.front() != '\'' || lexeme.back() != '\'') {
                return false;
            }
            std::string_view inner = lexeme.substr(1, lexeme.size() - 2);
            if (inner.empty()) return false;
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

        // 编译期常量折叠：字面量、一元号、算术与幂运算、类型转换
        // 编译期常量折叠：统一的求值器见 semantic/constant_folding.{hpp,cpp}
        // Compile-time constant folding: the shared evaluator lives in constant_folding.*
        bool fold_constant_expression(const Expression* expr, long long& int_out,
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

    bool Parser::at_struct_attribute() const {
        if (current_.type != TokenType::LeftBracket) return false;
        Token name = lookahead(1);
        if (name.type != TokenType::Identifier) return false;
        if (name.lexeme != "nocopy" && name.lexeme != "nomove") return false;
        return lookahead_type(2) == TokenType::RightBracket;
    }

    bool Parser::generic_param_list_is_primary_shaped() const {
        // 当前 token 必须是 '<'；判定 `<...>` 是参数列表（主泛型）还是模式列表（特化）
        // The current token must be '<'; decide parameter list (primary) versus pattern list
        if (current_.type != TokenType::Less) return true;
        std::size_t i = 1;
        if (lookahead_type(i) == TokenType::Greater) {
            return true;   // 空列表视为参数列表
        }
        for (;;) {
            TokenType first = lookahead_type(i);
            TokenType second = lookahead_type(i + 1);
            bool first_ident = first == TokenType::Identifier;
            bool first_type_keyword = (first == TokenType::Keyword_Int ||
                first == TokenType::Keyword_Float || first == TokenType::Keyword_Double ||
                first == TokenType::Keyword_Char || first == TokenType::Keyword_Bool ||
                first == TokenType::Keyword_String || first == TokenType::Keyword_File ||
                first == TokenType::Keyword_Void);
           if (first_ident) {
               if (second == TokenType::Identifier) {
                   i += 2;                                  // `T N` 常量参数
               }
                else if (second == TokenType::Colon) {
                    i += 2;                                  // `T: constraint`
                    int depth = 0;
                    for (;;) {
                        TokenType t = lookahead_type(i);
                        if (t == TokenType::EndOfFile) return false;
                        if (t == TokenType::Less) { ++depth; ++i; continue; }
                        if (t == TokenType::Greater) {
                            if (depth == 0) break;
                            --depth; ++i; continue;
                        }
                        if (t == TokenType::Comma && depth == 0) break;
                        ++i;
                    }
                }
               else {
                    // 裸类型参数 `T`：其后必须紧跟 ',' 或 '>'，否则属于复合模式
                    // A bare type parameter must be followed by ',' or '>' — otherwise this
                    // is a composite pattern such as `T*`
                    TokenType after_param = lookahead_type(i + 1);
                    if (after_param != TokenType::Comma && after_param != TokenType::Greater) {
                        return false;
                    }
                    i += 1;
               }
            }
            else if (first_type_keyword && second == TokenType::Identifier) {
                i += 2;                                      // `int N` 常量参数
            }
            else {
                return false;                                // 具体类型/复合模式 → 特化形态
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

    bool Parser::at_declaration_start() const {
        TokenType tt = current_.type;
        if (tt == TokenType::Keyword_Int || tt == TokenType::Keyword_Float ||
            tt == TokenType::Keyword_Double || tt == TokenType::Keyword_Char ||
            tt == TokenType::Keyword_Bool || tt == TokenType::Keyword_String ||
            tt == TokenType::Keyword_File) {
            return true;
        }
       if (tt == TokenType::Identifier) {
            // construct / destruct / copy / move 等为内置操作，不能当作声明起始
            // construct / destruct / copy / move are builtin operations, not declarations
            static const std::unordered_set<std::string> kBuiltins = {
                "construct", "destruct", "copy", "move", "deep_copy", "shallow_copy",
            };
            if (kBuiltins.count(std::string(current_.lexeme)) != 0) {
                return false;
            }
           if (lookahead_type(1) == TokenType::Identifier ||
               lookahead_type(1) == TokenType::Star) {
                return true;
            }
            if (looks_like_generic_instantiation()) {
                // 需要 `Name<...>.member 变量名` 形态（泛型名可带命名空间路径）
                // Probe the token after the matching '>' (the generic name may carry a
                // namespace path)
                std::size_t i = scan_generic_instantiation_end();
                if (i == std::string::npos) return false;
                TokenType after = lookahead_type(i);
                if (after == TokenType::Dot || after == TokenType::ColonColon) {
                    // 跳过 `.member` / `::member`，例如 Box<int>.Example e
                    // Skip `.member` / `::member`, e.g. Box<int>.Example e
                    i += 2;
                    after = lookahead_type(i);
                }
                return after == TokenType::Identifier || after == TokenType::Star ||
                    after == TokenType::LeftBracket;
            }
            // 命名空间限定名 `A::B::T name`（Gallt 0.4.txt §21）：链式 `Ident::Ident`
            // 之后紧跟标识符、`*` 或 `[` 视为声明开始
            // Namespace-qualified declaration `A::B::T name`: a chain of `Ident::Ident`
            // followed by an identifier, `*` or `[` starts a declaration
            if (lookahead_type(1) == TokenType::ColonColon) {
                std::size_t i = 1;
                while (lookahead_type(i) == TokenType::ColonColon &&
                    lookahead_type(i + 1) == TokenType::Identifier) {
                    i += 2;
                }
                TokenType after = lookahead_type(i);
                return after == TokenType::Identifier || after == TokenType::Star ||
                    after == TokenType::LeftBracket;
            }
        }
        return false;
    }

    bool Parser::at_special_member_keyword() const {
        if (current_.type != TokenType::Identifier) return false;
        std::string_view lexeme = current_.lexeme;
        static const char* kKeywords[] = {
            "constructor", "destructor", "copy_constructor",
            "move_constructor", "copy_assignment", "move_assignment",
        };
        bool matched = false;
        for (const char* kw : kKeywords) {
            if (lexeme == kw) {
                matched = true;
                break;
            }
        }
        if (!matched) return false;
        return lookahead_type(1) == TokenType::LeftParen;
    }

    std::size_t Parser::scan_generic_instantiation_end() const {
        // Gallt 0.4.txt §19/§21：泛型名可以带命名空间路径前缀
        // `Ident (:: Ident)* '<' ... '>'`
        // Gallt 0.4.txt §19/§21: a generic name may carry a namespace path prefix
        if (current_.type != TokenType::Identifier) return std::string::npos;
        std::size_t i = 1;
        while (lookahead_type(i) == TokenType::ColonColon &&
            lookahead_type(i + 1) == TokenType::Identifier) {
            i += 2;
        }
        if (lookahead_type(i) != TokenType::Less) return std::string::npos;

        // 扫描到匹配的 '>'；仅接受可能出现在类型/常量实参中的 token
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
            case TokenType::Keyword_Float:
            case TokenType::Keyword_Double:
            case TokenType::Keyword_Char:
            case TokenType::Keyword_Bool:
            case TokenType::Keyword_String:
            case TokenType::Keyword_File:
            case TokenType::Keyword_Void:
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
            case TokenType::Dot:
            case TokenType::ColonColon:
                break;
            default:
                // 其它 token（换行、分号、大括号、赋值、&&、|| 等）说明不是泛型实例化
                // Anything else means this is not a generic instantiation
                return std::string::npos;
            }
        }
    }

    bool Parser::looks_like_generic_instantiation() const {
        std::size_t after = scan_generic_instantiation_end();
        if (after == std::string::npos) return false;
        // '>' 之后的 token 必须能接续一个泛型实例化
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
        // 第 19 章编译期常量表达式：
        //   字面量 / 已实例化的编译期常量参数 / 括号 / 算术 + - * / % ** /
        //   比较 > < == / 逻辑 && || ! / 类型转换 [类型]([表达式]) / size、align
        // 其中单独的 '>' 与泛型实参列表的结束符冲突，采用“其后能否继续构成表达式”
        // 的前瞻判定来消解。
        // §19 compile-time constant expression. A lone '>' is disambiguated against the
        // closing '>' of an argument list by looking at whether an expression can follow.
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
                advance();
                return std::make_unique<PrimaryExpression>(loc, id);
            }
            // 类型转换 [类型]([编译期常量表达式])（第 19 章 / 第 7 章）
            // Type conversion [type]([compile-time constant expression])
            case TokenType::Keyword_Int:
            case TokenType::Keyword_Float:
            case TokenType::Keyword_Double:
            case TokenType::Keyword_Char:
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
                if (operand == nullptr) return nullptr;
                if (!expect(TokenType::RightParen, "expected ')' after compile-time cast")) {
                    return nullptr;
                }
                return std::make_unique<PostfixExpression>(loc, std::move(operand),
                    PostfixExpression::Operator::Cast,
                    nullptr, std::vector<std::unique_ptr<Expression>>{},
                    std::move(cast_type));
            }
            case TokenType::Keyword_Size:
            case TokenType::Keyword_Align: {
                std::string id(current_.lexeme);
                advance();
                if (!expect(TokenType::LeftParen, "expected '(' after size/align")) {
                    return nullptr;
                }
                // size/align 的参数可以是类型名（第 14 章），也可以是表达式
                // The argument of size/align may be a type name (§14) or an expression
                std::unique_ptr<Expression> arg;
                TokenType tt = current_.type;
                bool type_name_argument = (tt == TokenType::Keyword_Int ||
                    tt == TokenType::Keyword_Float || tt == TokenType::Keyword_Double ||
                    tt == TokenType::Keyword_Char || tt == TokenType::Keyword_Bool ||
                    tt == TokenType::Keyword_String || tt == TokenType::Keyword_File ||
                    (tt == TokenType::Identifier &&
                        lookahead_type(1) == TokenType::RightParen));
                if (type_name_argument) {
                    std::string type_text(current_.lexeme);
                    advance();
                    arg = std::make_unique<PrimaryExpression>(loc, type_text);
                }
                else {
                    arg = parse_compile_time_expression();
                }
                if (arg == nullptr) return nullptr;
                if (!expect(TokenType::RightParen, "expected ')' after size/align argument")) {
                    return nullptr;
                }
                auto call = std::make_unique<PostfixExpression>(
                    loc, std::make_unique<PrimaryExpression>(loc, id),
                    PostfixExpression::Operator::FunctionCall);
                call->arguments.push_back(std::move(arg));
                return call;
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
                current_.type == TokenType::LogicalNot) {
                UnaryExpression::Operator op = UnaryExpression::Operator::UnaryPlus;
                if (current_.type == TokenType::Minus) op = UnaryExpression::Operator::UnaryMinus;
                else if (current_.type == TokenType::LogicalNot) op = UnaryExpression::Operator::LogicalNot;
                advance();
                auto operand = parse_unary_ct();
                if (!operand) return nullptr;
                return std::make_unique<UnaryExpression>(loc, op, std::move(operand));
            }
            return parse_primary_ct();
        };
        std::function<std::unique_ptr<Expression>()> parse_mul =
            [&]() -> std::unique_ptr<Expression> {
            auto left = parse_unary_ct();
            if (!left) return nullptr;
            while (current_.type == TokenType::Star || current_.type == TokenType::Slash ||
                current_.type == TokenType::Percent) {
                SourceLocation loc = current_location();
                MultiplicativeExpression::Operator op = MultiplicativeExpression::Operator::Multiply;
                if (current_.type == TokenType::Slash) op = MultiplicativeExpression::Operator::Divide;
                else if (current_.type == TokenType::Percent) op = MultiplicativeExpression::Operator::Remainder;
                advance();
                auto right = parse_unary_ct();
                if (!right) return nullptr;
                left = std::make_unique<MultiplicativeExpression>(loc, std::move(left), op, std::move(right));
            }
            return left;
        };
        // 幂运算（右结合）
        std::function<std::unique_ptr<Expression>()> parse_power_ct =
            [&]() -> std::unique_ptr<Expression> {
            auto base = parse_mul();
            if (!base) return nullptr;
            if (current_.type == TokenType::Power) {
                SourceLocation loc = current_location();
                advance();
                auto right = parse_power_ct();
                if (!right) return nullptr;
                return std::make_unique<PowerExpression>(loc, std::move(base), std::move(right));
            }
            return base;
        };
        std::function<std::unique_ptr<Expression>()> parse_add_ct =
            [&]() -> std::unique_ptr<Expression> {
            auto value = parse_power_ct();
            if (!value) return nullptr;
            while (current_.type == TokenType::Plus || current_.type == TokenType::Minus) {
                SourceLocation loc = current_location();
                AdditiveExpression::Operator op = current_.type == TokenType::Plus
                    ? AdditiveExpression::Operator::Plus
                    : AdditiveExpression::Operator::Minus;
                advance();
                auto right = parse_power_ct();
                if (!right) return nullptr;
                value = std::make_unique<AdditiveExpression>(loc, std::move(value), op,
                    std::move(right));
            }
            return value;
        };
        // 比较：'<' '>=' '<=' '==' '!='，以及需要前瞻消解的 '>'
        // Comparisons; a lone '>' needs the lookahead disambiguation described above
        auto greater_is_comparison = [&]() -> bool {
            if (current_.type != TokenType::Greater) return false;
            switch (lookahead_type(1)) {
            case TokenType::IntegerLiteral:
            case TokenType::FloatLiteral:
            case TokenType::CharLiteral:
            case TokenType::StringLiteral:
            case TokenType::BoolLiteral:
            case TokenType::Identifier:
            case TokenType::LeftParen:
            case TokenType::Keyword_Int:
            case TokenType::Keyword_Float:
            case TokenType::Keyword_Double:
            case TokenType::Keyword_Char:
            case TokenType::Keyword_Bool:
            case TokenType::Keyword_String:
            case TokenType::Keyword_Size:
            case TokenType::Keyword_Align:
                return true;
            default:
                return false;   // 视为实参列表的结束符
            }
        };
        std::function<std::unique_ptr<Expression>()> parse_comparison_ct =
            [&]() -> std::unique_ptr<Expression> {
            auto value = parse_add_ct();
            if (!value) return nullptr;
            for (;;) {
                ComparisonExpression::Operator op;
                switch (current_.type) {
                case TokenType::Less: op = ComparisonExpression::Operator::Less; break;
                case TokenType::LessEqual: op = ComparisonExpression::Operator::LessEqual; break;
                case TokenType::Equal: op = ComparisonExpression::Operator::Equal; break;
                case TokenType::NotEqual: op = ComparisonExpression::Operator::NotEqual; break;
                case TokenType::GreaterEqual:
                    op = ComparisonExpression::Operator::GreaterEqual; break;
                case TokenType::Greater:
                    if (!greater_is_comparison()) return value;
                    op = ComparisonExpression::Operator::Greater;
                    break;
                default:
                    return value;
                }
                SourceLocation loc = current_location();
                advance();
                auto right = parse_add_ct();
                if (!right) return nullptr;
                value = std::make_unique<ComparisonExpression>(loc, std::move(value), op,
                    std::move(right));
            }
        };
        std::function<std::unique_ptr<Expression>()> parse_logical_and_ct =
            [&]() -> std::unique_ptr<Expression> {
            auto value = parse_comparison_ct();
            if (!value) return nullptr;
            while (current_.type == TokenType::LogicalAnd) {
                SourceLocation loc = current_location();
                advance();
                auto right = parse_comparison_ct();
                if (!right) return nullptr;
                value = std::make_unique<LogicalAndExpression>(loc, std::move(value),
                    std::move(right));
            }
            return value;
        };
        auto value = parse_logical_and_ct();
        if (!value) return nullptr;
        while (current_.type == TokenType::LogicalOr) {
            SourceLocation loc = current_location();
            advance();
            auto right = parse_logical_and_ct();
            if (!right) return nullptr;
            value = std::make_unique<LogicalOrExpression>(loc, std::move(value),
                std::move(right));
        }
        return value;
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
            bool type_start = (tt == TokenType::Keyword_Int || tt == TokenType::Keyword_Float ||
                tt == TokenType::Keyword_Double || tt == TokenType::Keyword_Char ||
                tt == TokenType::Keyword_Bool || tt == TokenType::Keyword_String ||
                tt == TokenType::Keyword_File || tt == TokenType::Keyword_Void ||
                tt == TokenType::Identifier);
            if (type_start) {
               // 先按类型尝试：`int`、`T*`、`Box<int>.Example`
               std::size_t m = mark();
               Type t = parse_type(true, false);
                // 数组类型实参：`int[]`、`int[3]`（第 19 章 array 约束需要数组类型实参）
                // Array type arguments such as int[] / int[3] (needed by the array constraint)
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
                }
                else {
                    reset_to(m);
                }
            }
            if (!parsed_type) {
                // 编译期常量实参
                auto expr = parse_compile_time_expression();
                arg.is_type = false;
                if (expr) {
                    // 保留表达式：泛型展开阶段按已绑定常量参数重新求值（§19）
                    // Keep the expression so expansion can re-evaluate it with the bound
                    // constant parameters (§19)
                    arg.expression = std::shared_ptr<Expression>(expr.release());
                    // 字符串字面量等非数值常量：记录其类型，供 ER 0070 使用
                    // Non-numeric constants (e.g. string literals) record their type for ER 0070
                    if (auto* prim = dynamic_cast<PrimaryExpression*>(arg.expression.get())) {
                        if (prim->kind == PrimaryExpression::Kind::Literal &&
                            prim->literal_token.type == TokenType::StringLiteral) {
                            arg.constant_actual_type = Type::make_string();
                            // 字符串常量实参进入规范化与命名修饰（Gallt 0.4.txt §19）
                            // String constant arguments take part in normalization and
                            // mangling (Gallt 0.4.txt §19)
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
                        }
                        else {
                            arg.int_value = iv;
                        }
                        arg.text = arg.normalize();
                    }
                    else if (arg.is_string_constant) {
                        // 字符串编译期常量实参（Gallt 0.4.txt §19）
                        // String compile-time constant argument (Gallt 0.4.txt §19)
                        arg.text = arg.normalize();
                    }
                    else if (auto* prim = dynamic_cast<PrimaryExpression*>(arg.expression.get())) {
                        if (prim->kind == PrimaryExpression::Kind::Identifier) {
                            // 引用绑定的编译期常量参数：规范化文本保留标识符
                            // Reference to a bound constant parameter: keep its name
                            arg.text = prim->identifier;
                        }
                        else {
                            arg.text = compile_time_expr_text(arg.expression.get());
                        }
                    }
                   else {
                       arg.text = compile_time_expr_text(arg.expression.get());
                   }
                    // 非常量且无法作为单个标识符引用 → ER 0069
                    // Not a constant and not a plain identifier reference -> ER 0069
                    if (current_.type != TokenType::Comma && current_.type != TokenType::Greater) {
                        report_error_template(ErrorCode::GenericNonTypeArgNotConstant,
                            { arg.text });
                    }
               }
           }
            args.push_back(std::move(arg));
            if (current_.type == TokenType::Comma) {
                advance();
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
        advance(); // 消费泛型名
        // Gallt 0.4.txt §19/§21：命名空间限定的泛型实例化
        // `ns1::ns2::Box<...>`：`::` 之前的标识符构成命名空间路径
        // Gallt 0.4.txt §19/§21: namespace-qualified generic instantiation; the
        // identifiers before `::` form the namespace path
        while (current_.type == TokenType::ColonColon &&
            lookahead_type(1) == TokenType::Identifier) {
            advance();                                  // '::'
            ref.namespace_path.push_back(ref.generic_name);
            ref.generic_name = std::string(current_.lexeme);
            advance();                                  // 标识符
            if (current_.type == TokenType::Less) break;
        }
        ref.arguments = parse_generic_arguments();
        // Gallt 0.4.txt §19：
        //   `[泛型名]<[实参列表]>::[成员标识符]` 按成员实例化并引入短名
        //   `[泛型名]<[实参列表]>.[成员标识符]`  限定名，只直接解析
        // Gallt 0.4.txt §19: `::` is per-member instantiation (introduces a short
        // name); `.` is a qualified name that always resolves directly
        if (current_.type == TokenType::Dot || current_.type == TokenType::ColonColon) {
            ref.member_scope_access = (current_.type == TokenType::ColonColon);
            std::string_view separator =
                ref.member_scope_access ? std::string_view("::") : std::string_view(".");
            advance();
            if (current_.type == TokenType::Identifier) {
                ref.member = std::string(current_.lexeme);
                advance();
            }
            else {
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
        // 具体类型或具体类型联合
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
        // 形态判定必须在消费 '<' 之前进行
        // The shape check must run before '<' is consumed
        bool primary_shaped = generic_param_list_is_primary_shaped();
        if (!expect(TokenType::Less, "expected '<' after generic name")) {
            return nullptr;
        }

        // 形态判定：参数列表（主泛型）还是模式列表（特化）
        // Shape check: parameter list (primary) versus pattern list (specialization)
        bool is_specialization = seen_generics_.count(name) != 0 || !primary_shaped;
        auto def = std::make_unique<GenericDefinition>(loc, name);
        def->is_specialization = is_specialization;
        def->primary_shaped = primary_shaped;
        seen_generics_.insert(name);

        // 解析参数列表（主泛型）或模式列表（特化）
        if (current_.type != TokenType::Greater) {
            do {
                if (!is_specialization) {
                    GenericParameter param;
                    param.location = current_location();
                    // 形式一：[identifier] [':' constraint]  → 类型参数
                    // 形式二：[type|identifier] identifier → 编译期常量参数
                    if (current_.type == TokenType::Identifier) {
                        std::string first(current_.lexeme);
                        if (lookahead_type(1) == TokenType::Identifier) {
                            // `T N`：常量参数，其类型由类型参数 T 决定
                            param.is_type = false;
                            param.constant_type_is_parameter = true;
                            param.constant_type_parameter = first;
                            param.name = std::string(lookahead(1).lexeme);
                            advance();
                            advance();
                            def->parameters.push_back(std::move(param));
                        }
                        else {
                            param.is_type = true;
                            param.name = first;
                            advance();
                            if (current_.type == TokenType::Colon) {
                                advance();
                                param.constraint = parse_generic_constraint();
                            }
                            def->parameters.push_back(std::move(param));
                        }
                    }
                    else if (current_.type == TokenType::Keyword_Int ||
                        current_.type == TokenType::Keyword_Float ||
                        current_.type == TokenType::Keyword_Double ||
                        current_.type == TokenType::Keyword_Char ||
                        current_.type == TokenType::Keyword_Bool ||
                        current_.type == TokenType::Keyword_String) {
                        // `int N`：显式类型的常量参数
                        param.is_type = false;
                        param.constant_type = parse_type_specifier(true);
                        if (current_.type != TokenType::Identifier) {
                            report_error(ErrorCode::ExpressionSyntaxError,
                                "expected constant parameter name");
                            return nullptr;
                        }
                        param.name = std::string(current_.lexeme);
                        advance();
                        def->parameters.push_back(std::move(param));
                    }
                    else {
                        report_error(ErrorCode::ExpressionSyntaxError,
                            "invalid generic parameter");
                        return nullptr;
                    }
                }
                else {
                    // 特化模式：类型模式或编译期常量模式
                    GenericPatternArg pattern;
                    bool type_like = (current_.type == TokenType::Keyword_Int ||
                        current_.type == TokenType::Keyword_Float ||
                        current_.type == TokenType::Keyword_Double ||
                        current_.type == TokenType::Keyword_Char ||
                        current_.type == TokenType::Keyword_Bool ||
                        current_.type == TokenType::Keyword_String ||
                        current_.type == TokenType::Keyword_File ||
                        current_.type == TokenType::Identifier);
                    if (type_like) {
                        Type t = parse_type(true, false);
                        // 复合模式：T*、T[]、T**、T*[]、T[]* 等，'*' 与 '[]' 可任意组合
                        // Composite patterns: '*' and '[]' suffixes in any order
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
                                // 词法器把 '**' 合并为 Power：两级指针
                                // The lexer merges '**' into Power: two pointer levels
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
                    }
                    else {
                        auto expr = parse_compile_time_expression();
                        long long iv = 0;
                        double dv = 0.0;
                        bool is_flt = false;
                        // Gallt 0.4.txt §19（修正后）：编译期常量表达式允许字符串字面量，
                        // 因此字符串常量模式同样合法（按字符串内容参与匹配与命名修饰）
                        // Gallt 0.4.txt §19 (corrected): string literals are allowed in
                        // compile-time constant expressions, so string constant patterns
                        // are valid too (matched/mangled by their contents)
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
                            // ER 0068：常量模式无法在编译期求值（变量、函数调用等）
                            // ER 0068: the constant pattern is not evaluable at compile time
                            report_error_template(ErrorCode::GenericConstantPatternNotConstant,
                                { compile_time_expr_text(expr.get()) });
                            return nullptr;
                        }
                        pattern.is_constant = true;
                        if (string_pattern) {
                            pattern.string_constant = true;
                            pattern.string_value = string_value;
                            pattern.text = "\"" + string_value + "\"";
                        }
                        else if (is_flt) {
                            pattern.float_constant = true;
                            pattern.float_value = dv;
                            pattern.text = std::to_string(iv);
                        }
                        else {
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
            if (current_.type == TokenType::RightBrace) break;
           // Gallt 0.4.txt §19：泛型块顶部的编译期代码生成项（emit / 编译期 if）
           // Gallt 0.4.txt §19: compile-time code-generation items (emit / compile-time if)
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
                // 泛型块内只允许结构体与函数定义（成员定义）
                // Only struct and function definitions are allowed inside a generic block
                if (dynamic_cast<StructDefinition*>(member.get()) == nullptr &&
                    dynamic_cast<FunctionDefinition*>(member.get()) == nullptr) {
                    report_error_at(member->location, ErrorCode::GenericStatementNotAllowed,
                        "executable statements are not allowed directly inside a generic block");
                    continue;
                }
               def->members.push_back(std::move(member));
            }
            else {
                // ER 0060：泛型块顶部、所有成员定义之外不允许可执行语句
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
        if (current_.type == TokenType::Keyword_Struct) {
            return parse_struct_definition();
        }
        // [nocopy] / [nomove] 修饰的泛型成员结构体（第 19 章 + 第 20 章）
        // Generic member structs carrying [nocopy] / [nomove] attributes
        if (at_struct_attribute()) {
            return parse_struct_definition();
        }
        TokenType tt = current_.type;
        bool type_start = (tt == TokenType::Keyword_Int || tt == TokenType::Keyword_Float ||
            tt == TokenType::Keyword_Double || tt == TokenType::Keyword_Char ||
            tt == TokenType::Keyword_Bool || tt == TokenType::Keyword_String ||
            tt == TokenType::Keyword_File || tt == TokenType::Keyword_Void ||
            tt == TokenType::Identifier);
        if (type_start) {
            return parse_function_definition();
        }
        return nullptr;
    }

    std::unique_ptr<SpecialMemberFunction> Parser::parse_special_member_function() {
        SourceLocation loc = current_location();
        std::string_view lexeme = current_.lexeme;
        SpecialMemberFunction::Kind kind = SpecialMemberFunction::Kind::Constructor;
        if (lexeme == "constructor") kind = SpecialMemberFunction::Kind::Constructor;
        else if (lexeme == "destructor") kind = SpecialMemberFunction::Kind::Destructor;
        else if (lexeme == "copy_constructor") kind = SpecialMemberFunction::Kind::CopyConstructor;
        else if (lexeme == "move_constructor") kind = SpecialMemberFunction::Kind::MoveConstructor;
        else if (lexeme == "copy_assignment") kind = SpecialMemberFunction::Kind::CopyAssignment;
        else if (lexeme == "move_assignment") kind = SpecialMemberFunction::Kind::MoveAssignment;
        else {
            report_error(ErrorCode::ExpressionSyntaxError, "unknown special member function");
            return nullptr;
        }
        advance(); // 消费关键字

        auto member = std::make_unique<SpecialMemberFunction>(loc, kind);
        if (!expect(TokenType::LeftParen, "expected '(' in special member function")) {
            return nullptr;
        }
        if (kind == SpecialMemberFunction::Kind::Constructor) {
            auto [types, names] = parse_parameter_list(&member->parameter_defaults);
            member->parameters = std::move(types);
            member->parameter_names = std::move(names);
        }
        else if (kind == SpecialMemberFunction::Kind::Destructor) {
            // 析构函数无参数（Gallt 0.3.txt §20）
            // A destructor takes no parameters (Gallt 0.3.txt §20)
            if (current_.type != TokenType::RightParen) {
                report_error(ErrorCode::DestructorWithParameters,
                    "destructor must not have parameters");
            }
        }
        else {
            // copy/move 构造/赋值：([类型]* [参数名])
            Type pointer_type = parse_type(true, false);
            if (current_.type == TokenType::Star) {
                advance();
                pointer_type = Type::make_pointer(std::make_shared<Type>(std::move(pointer_type)));
            }
            member->parameter_type = std::move(pointer_type);
            if (current_.type == TokenType::Identifier) {
                member->parameter_name = std::string(current_.lexeme);
                advance();
            }
        }
        if (!expect(TokenType::RightParen, "expected ')' in special member function")) {
            return nullptr;
        }
        member->body = parse_block();
        return member;
    }

    std::unique_ptr<VariableDeclaration> Parser::parse_variable_declaration_with_type(
        Type base_type, bool allow_empty_array) {
        SourceLocation loc = current_location();
        std::string var_name;
        if (!check_identifier_name(var_name, "variable name")) {
            return nullptr;
        }

        std::optional<size_t> array_size = std::nullopt;
        std::unique_ptr<Expression> array_size_expr;
        Type var_type = finish_declarator_type(std::move(base_type), allow_empty_array,
            &array_size, &array_size_expr);
        if (var_type.kind == TypeKind::Void) {
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
        auto decl = std::make_unique<VariableDeclaration>(
            loc, std::move(var_type), var_name, array_size,
            std::move(function_pointer_type), std::move(init));
        decl->array_size_expr = std::move(array_size_expr);
        return decl;
    }

} // namespace gallt
