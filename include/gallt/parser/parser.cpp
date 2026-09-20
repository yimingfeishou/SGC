#include "../parser/parser.hpp"
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

    namespace {

        bool is_builtin_type_keyword(TokenType t) noexcept {
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

        bool is_builtin_or_void_type_keyword(TokenType t) noexcept {
            return is_builtin_type_keyword(t) || t == TokenType::Keyword_Void;
        }

        bool is_type_start_keyword(TokenType t) noexcept {
            return is_builtin_or_void_type_keyword(t) || t == TokenType::Keyword_Const;
        }

        std::string unquote_string(std::string_view lexeme) {
            if (lexeme.size() >= 2 && lexeme.front() == '"' && lexeme.back() == '"') {
                lexeme.remove_prefix(1);
                lexeme.remove_suffix(1);
            }
            return std::string(lexeme);
        }

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

        bool fold_constant_expression(const gallt::AST::Expression* expr,
            long long& int_out, double& float_out, bool& is_float_out,
            const std::function<bool(const std::string&, std::size_t&, std::size_t&)>*
                resolve_type_size = nullptr);
    }

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

    std::unique_ptr<TopLevel> Parser::parse_function_definition() {
        SourceLocation loc = current_location();
        Type ret_type = parse_type(true);

        std::string func_name;
        if (!check_identifier_name(func_name, "function name")) {
            return nullptr;
        }

        if (current_.type != TokenType::LeftParen) {
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
            declared_value_names_.insert(func_name);
            auto decl = std::make_unique<VariableDeclaration>(
                loc, std::move(var_type), func_name, array_size, std::nullopt, std::move(init));
            decl->array_size_expr = std::move(array_size_expr);
            return decl;
        }
        advance(); 

        std::vector<std::unique_ptr<Expression>> param_defaults;
        auto [param_types, param_names] = parse_parameter_list(&param_defaults);
        if (!expect(TokenType::RightParen, "expected ')' after parameter list")) {
            return nullptr;
        }

        if (current_.type == TokenType::Star || current_.type == TokenType::Power) {
            const bool double_pointer = current_.type == TokenType::Power;
            advance();
            Type declared = Type::make_function(
                std::make_shared<Type>(std::move(ret_type)), std::move(param_types));
            if (double_pointer) {
                declared = Type::make_pointer(std::make_shared<Type>(std::move(declared)));
            }
            std::optional<Type> function_pointer_type = std::nullopt;
            if (declared.kind == TypeKind::Function) {
                function_pointer_type = declared;
            }
            std::unique_ptr<Initializer> init = nullptr;
            if (match(TokenType::Assign)) {
                init = parse_initializer();
                if (init == nullptr) {
                    report_error(ErrorCode::ExpressionSyntaxError,
                        "invalid global function pointer initializer");
                }
            }
            expect_stmt_end("global function pointer declaration");
            return std::make_unique<VariableDeclaration>(
                loc, std::move(declared), func_name, std::nullopt,
                std::move(function_pointer_type), std::move(init));
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

    bool Parser::at_operator_definition() const {
        if (current_.type != TokenType::Identifier || current_.lexeme != "operator") {
            return false;
        }
        if (at_operator_symbol(lookahead_type(1))) {
            return true;
        }
        return (lookahead_type(1) == TokenType::Identifier ||
            is_type_start_keyword(lookahead_type(1))) &&
            lookahead_type(2) == TokenType::LeftParen;
    }

    bool Parser::looks_like_operator_definition() const {
        std::size_t offset = 0;
        if (current_.type == TokenType::Identifier && current_.lexeme == "operator") {
            offset = 0;
        }
        else if (current_.type == TokenType::Identifier) {
            std::size_t after = scan_expr_argument_instantiation_end();
            if (after == std::string::npos) {
                after = scan_generic_instantiation_end();
            }
            if (after != std::string::npos) {
                offset = after;
                while (lookahead_type(offset) == TokenType::Star ||
                    lookahead_type(offset) == TokenType::Power) {
                    ++offset;
                }
            }
            else if (lookahead_type(1) == TokenType::Star ||
                lookahead_type(1) == TokenType::Power) {
                offset = 2;
            }
            else {
                offset = 1;
            }
        }
        else if (is_type_start_keyword(current_.type)) {
            if (lookahead_type(1) == TokenType::Star ||
                lookahead_type(1) == TokenType::Power) {
                offset = 2;
            }
            else {
                offset = 1;
            }
        }
        else {
            return false;
        }
        if (lookahead_type(offset) != TokenType::Identifier ||
            lookahead(offset).lexeme != "operator") {
            return false;
        }
        if (at_operator_symbol(lookahead_type(offset + 1)) ||
            lookahead_type(offset + 1) == TokenType::RightBracket) {
            return true;
        }
        return (lookahead_type(offset + 1) == TokenType::Identifier ||
            is_type_start_keyword(lookahead_type(offset + 1))) &&
            lookahead_type(offset + 2) == TokenType::LeftParen;
    }

    bool Parser::at_operator_parameter_list() const {
        return at_operator_definition();
    }

    std::string Parser::operator_token_text() const {
        return std::string(current_.lexeme);
    }

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

    bool Parser::at_operator_symbol(TokenType type) const {
        switch (type) {
        case TokenType::Plus:
        case TokenType::Minus:
        case TokenType::Star:
        case TokenType::Slash:
        case TokenType::Percent:
        case TokenType::Power:
        case TokenType::Equal:
        case TokenType::NotEqual:
        case TokenType::Greater:
        case TokenType::Less:
        case TokenType::GreaterEqual:
        case TokenType::LessEqual:
        case TokenType::LogicalAnd:
        case TokenType::LogicalOr:
        case TokenType::LogicalNot:
        case TokenType::AddressOf:
        case TokenType::Increment:
        case TokenType::Decrement:
        case TokenType::PlusAssign:
        case TokenType::MinusAssign:
        case TokenType::LeftBracket:
        case TokenType::Arrow:
        case TokenType::Dot:
        case TokenType::Assign:
            return true;
        default:
            return false;
        }
    }

    std::unique_ptr<TopLevel> Parser::parse_operator_definition() {
        SourceLocation loc = current_location();
        Type return_type = Type::make_void();
        if (current_.type != TokenType::Identifier || current_.lexeme != "operator") {
            return_type = parse_type(true, false);
            if (current_.type != TokenType::Identifier ||
                current_.lexeme != "operator") {
                report_error_at(loc, ErrorCode::ExpressionSyntaxError,
                    "expected 'operator' in operator overload declaration");
                return nullptr;
            }
        }
        advance();
        std::string op_text;
        bool conversion = false;
        if (at_operator_symbol(current_.type)) {
            op_text = std::string(current_.lexeme);
            advance();
            if (op_text == "[") {
                if (!expect(TokenType::RightBracket, "expected ']' in operator[]")) {
                    return nullptr;
                }
                op_text = "[]";
            }
        }
        else {
            conversion = true;
        }
        Type conversion_target;
        if (conversion) {
            conversion_target = parse_type(true, false);
            if (conversion_target.kind == TypeKind::Void) {
                report_error_at(loc, ErrorCode::ConversionOperatorTargetInvalid,
                    "invalid conversion operator target type");
                return nullptr;
            }
            return_type = conversion_target;
        }
        if (!expect(TokenType::LeftParen, "expected '(' after operator name")) {
            return nullptr;
        }
        std::vector<std::unique_ptr<Expression>> param_defaults;
        auto [param_types, param_names] = parse_parameter_list(&param_defaults);
        if (!expect(TokenType::RightParen, "expected ')' after parameter list")) {
            return nullptr;
        }
        auto body = parse_block();
        if (body == nullptr) {
            report_error(ErrorCode::ExpressionSyntaxError, "expected operator body");
            return nullptr;
        }
        std::string raw_name;
        if (conversion) {
            raw_name = std::string("conv_") + conversion_target.to_string();
        }
        else if (op_text == "[]") {
            raw_name = "index";
        }
        else if (op_text == "->") {
            raw_name = "arrow";
        }
        else if (op_text == "++") {
            raw_name = param_types.size() == 2 ? "post_inc" : "pre_inc";
        }
        else if (op_text == "--") {
            raw_name = param_types.size() == 2 ? "post_dec" : "pre_dec";
        }
        else {
            raw_name = op_text;
        }
        std::string sanitized;
        sanitized.reserve(raw_name.size());
        for (char c : raw_name) {
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '_') {
                sanitized.push_back(c);
            }
            else {
                sanitized.push_back('_');
            }
        }
        if (sanitized.empty()) {
            sanitized = "op";
        }
        std::string name = std::string("__glt_op_") + sanitized;
        auto func = std::make_unique<FunctionDefinition>(
            loc, std::move(return_type), name,
            std::move(param_types), std::move(param_names),
            std::move(body));
        func->is_operator = true;
        func->overloaded_operator = op_text;
        func->is_conversion_operator = conversion;
        func->conversion_target_type = conversion_target;
        func->param_defaults = std::move(param_defaults);
        return func;
    }

    std::unique_ptr<StructDefinition> Parser::parse_struct_definition() {
        SourceLocation loc = current_location();
        bool no_copy = false;
        bool no_move = false;
        while (at_struct_attribute()) {
            advance(); 
            std::string attr(current_.lexeme);
            if (attr == "nocopy") no_copy = true;
            else if (attr == "nomove") no_move = true;
            advance(); 
            expect(TokenType::RightBracket, "expected ']' after struct attribute");
            skip_newlines();
        }
        expect(TokenType::Keyword_Struct, "expected 'struct'");

        std::string struct_name;
        if (!check_identifier_name(struct_name, "struct name")) {
            return nullptr;
        }
        declared_type_names_.insert(struct_name);

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
            if (at_special_member_keyword()) {
                auto smf = parse_special_member_function();
                if (smf != nullptr) {
                    special_members.push_back(std::move(smf));
                    skip_newlines();
                    continue;
                }
                break;
            }
            if (at_operator_definition()) {
                SourceLocation op_loc = current_location();
                report_error_template_at(op_loc, ErrorCode::OperatorOverloadInsideStruct,
                    { std::string(lookahead(1).lexeme) });
                auto skipped = parse_operator_definition();
                (void)skipped;
                skip_newlines();
                continue;
            }
            if (looks_like_operator_definition()) {
                SourceLocation op_loc = current_location();
                std::string op_text = lookahead(1).lexeme == "operator"
                    ? std::string(lookahead(2).lexeme)
                    : std::string(lookahead(1).lexeme);
                report_error_template_at(op_loc, ErrorCode::OperatorOverloadInsideStruct,
                    { op_text });
                auto skipped = parse_operator_definition();
                (void)skipped;
                skip_newlines();
                continue;
            }
            bool type_start_token = (current_.type == TokenType::Identifier ||
                is_type_start_keyword(current_.type));
            if (type_start_token &&
                lookahead_type(1) == TokenType::Identifier &&
                lookahead_type(2) == TokenType::LeftParen) {
                Token next = lookahead(1);
                if (next.lexeme == "constructor" || next.lexeme == "destructor" ||
                    next.lexeme == "copy_constructor" || next.lexeme == "move_constructor" ||
                    next.lexeme == "copy_assignment" || next.lexeme == "move_assignment") {
                    report_error(ErrorCode::ConstructorWithReturnType,
                        "special member functions must not declare a return type");
                    advance(); 
                    auto smf = parse_special_member_function();
                    if (smf != nullptr) {
                        special_members.push_back(std::move(smf));
                        skip_newlines();
                        continue;
                    }
                    break;
                }
                std::size_t scan = 2;
                int paren_depth = 0;
                while (true) {
                    TokenType token = lookahead_type(scan);
                    if (token == TokenType::EndOfFile) break;
                    if (token == TokenType::LeftParen) {
                        paren_depth++;
                    }
                    else if (token == TokenType::RightParen) {
                        paren_depth--;
                        if (paren_depth == 0) break;
                    }
                    scan++;
                }
                TokenType after_params = lookahead_type(scan + 1);
                if (after_params != TokenType::Star &&
                    after_params != TokenType::Power) {
                    report_error_template(ErrorCode::StructMethodNotAllowed,
                        { std::string(next.lexeme) });
                    int brace_depth = 0;
                    while (current_.type != TokenType::EndOfFile) {
                        if (current_.type == TokenType::LeftBrace) {
                            brace_depth++;
                        }
                        else if (current_.type == TokenType::RightBrace) {
                            if (brace_depth == 0) break;
                            brace_depth--;
                        }
                        advance();
                    }
                    skip_newlines();
                    continue;
                }
            }
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

    bool Parser::check_identifier_name(std::string& out, const char* context) {
        if (current_.type == TokenType::Identifier) {
            out = std::string(current_.lexeme);
            advance();
            return true;
        }
        if (current_.is_keyword()) {
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

    std::unique_ptr<ConditionalBlock> Parser::parse_conditional_block(bool top_level) {
        SourceLocation loc = current_location();
        auto condition = parse_condition_expression();
        if (condition == nullptr) {
            synchronize();
            return nullptr;
        }
        skip_newlines();
        std::unique_ptr<Statement> then_block;
        if (current_.type == TokenType::LeftBrace) {
            if (top_level) {
                then_block = parse_top_level_block();
            }
            else {
                std::unique_ptr<AST::Block> block = parse_block();
                then_block.reset(block.release());
            }
        }
        else {
            if (top_level) {
                then_block = parse_conditional_branch_top_level();
            }
            else {
                std::unique_ptr<AST::Statement> nested = parse_statement();
                then_block = std::move(nested);
            }
        }
        skip_newlines();
        std::unique_ptr<Statement> else_block = nullptr;
        if (current_.type == TokenType::At && lookahead(1).lexeme == "else") {
            advance();
            advance();
            skip_newlines();
            if (current_.type == TokenType::At && lookahead(1).lexeme == "if") {
                advance();
                std::unique_ptr<AST::ConditionalBlock> nested =
                    parse_conditional_block(top_level);
                else_block.reset(nested.release());
            }
            else if (current_.type == TokenType::LeftBrace) {
                if (top_level) {
                    else_block = parse_top_level_block();
                }
                else {
                    std::unique_ptr<AST::Block> block = parse_block();
                    else_block.reset(block.release());
                }
            }
            else if (top_level) {
                else_block = parse_conditional_branch_top_level();
            }
            else {
                std::unique_ptr<AST::Statement> nested = parse_statement();
                else_block = std::move(nested);
            }
            skip_newlines();
        }
        return std::make_unique<ConditionalBlock>(loc, std::move(condition),
            std::move(then_block), std::move(else_block));
    }

    std::unique_ptr<Statement> Parser::parse_conditional_branch_top_level() {
        if (current_.type == TokenType::At && lookahead(1).lexeme == "else") {
            return nullptr;
        }
        if (current_.type == TokenType::Keyword_Guide ||
            current_.type == TokenType::Keyword_Clib) {
            report_error(ErrorCode::ExpressionSyntaxError,
                "guide and clib are not allowed inside a conditional block");
            synchronize();
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
            if (current_.type == TokenType::Keyword_Guide ||
                current_.type == TokenType::Keyword_Clib) {
                report_error(ErrorCode::ExpressionSyntaxError,
                    "guide and clib are not allowed inside a conditional block");
                synchronize();
                continue;
            }
            auto item = parse_top_level();
            if (item != nullptr) {
                block->items.push_back(std::move(item));
            }
            else if (!in_error_recovery_) {
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

    bool Parser::emit_item_starts_with_function_definition() const {
        auto is_type_keyword = [](TokenType t) {
            return is_type_start_keyword(t);
        };

        std::size_t i = 0;
        TokenType tt = lookahead_type(i);
        if (tt != TokenType::Identifier && !is_type_keyword(tt)) return false;

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
                    if (inner == TokenType::LeftBracket) ++depth;
                    else if (inner == TokenType::RightBracket) --depth;
                    ++i;
                }
                continue;
            }
            break;
        }

        if (lookahead_type(i) != TokenType::Identifier) return false;
        return lookahead_type(i + 1) == TokenType::LeftParen;
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

    std::unique_ptr<VariableDeclaration> Parser::parse_variable_declaration() {
        SourceLocation loc = current_location();
        Type var_type = parse_type(true);

        std::string var_name;
        if (!check_identifier_name(var_name, "variable name")) {
            return nullptr;
        }
        declared_value_names_.insert(var_name);

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

    Type Parser::parse_type(bool allow_void, bool allow_function_suffix) {
        Type base_type = parse_type_specifier(allow_void);
        if (base_type.kind == TypeKind::Void && !allow_void) {
            report_error(ErrorCode::ExpressionSyntaxError, "void type not allowed here");
        }

        while (true) {
            if (allow_function_suffix && current_.type == TokenType::LeftParen) {
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
                advance();
                base_type = Type::make_pointer(std::make_shared<Type>(
                    Type::make_pointer(std::make_shared<Type>(base_type))));
            }
            else if (current_.type == TokenType::Keyword_Const) {
                advance();
                base_type.is_const = true;
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
                    if (!is_first_dimension || !allow_empty_array) {
                        report_error_at(loc, ErrorCode::ArraySizeNotConstant,
                            "array size must be specified here");
                    }
                }
                else {
                    auto expr = parse_compile_time_expression();
                    if (expr != nullptr) {
                        long long iv = 0;
                        double dv = 0.0;
                        bool is_flt = false;
                        if (fold_constant_expression(expr.get(), iv, dv, is_flt) && !is_flt && iv >= 0) {
                            size = static_cast<size_t>(iv);
                        }
                        else if (is_first_dimension && out_size_expr != nullptr) {
                            *out_size_expr = std::move(expr);
                        }
                        else {
                            report_error_at(loc, ErrorCode::ArraySizeNotConstant,
                                "array size must be a constant integer expression");
                        }
                    }
                }
                if (!expect(TokenType::RightBracket, "expected ']' after array size")) {
                    break;
                }
                dimensions.push_back(size);
                dimension_exprs.push_back(std::move(size_expr));
            }
            if (!dimensions.empty()) {
                if (out_array_size != nullptr) {
                    *out_array_size = dimensions.front();
                }
                for (std::size_t i = dimensions.size(); i > 0; --i) {
                    base = Type::make_array(std::make_shared<Type>(std::move(base)),
                        dimensions[i - 1]);
                }
            }
        }

        if (current_.type == TokenType::LeftParen && base.kind != TypeKind::Function) {
            auto suffix = parse_function_pointer_suffix(base);
            if (suffix.has_value()) {
                base = std::move(suffix.value());
            }
        }
        return base;
    }

    std::optional<Type> Parser::parse_function_pointer_suffix(Type base_type) {
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
        return Type::make_function(std::make_shared<Type>(std::move(base_type)),
            std::move(params));
    }

    Type Parser::parse_type_specifier(bool allow_void) {
        if (current_.type == TokenType::Keyword_Const) {
            advance();
            Type qualified = parse_type_specifier(allow_void);
            qualified.is_const = true;
            return qualified;
        }
        switch (current_.type) {
        case TokenType::Keyword_Int:
            advance();
            return Type::make_int();
        case TokenType::Keyword_Lint:
            advance();
            return Type::make_lint();
        case TokenType::Keyword_Uint:
            advance();
            return Type::make_uint();
        case TokenType::Keyword_Luint:
            advance();
            return Type::make_luint();
        case TokenType::Keyword_Float:
            advance();
            return Type::make_float();
        case TokenType::Keyword_Double:
            advance();
            return Type::make_double();
        case TokenType::Keyword_Char:
            advance();
            return Type::make_char();
        case TokenType::Keyword_Uchar:
            advance();
            return Type::make_uchar();
        case TokenType::Keyword_Bool:
            advance();
            return Type::make_bool();
        case TokenType::Keyword_String:
            advance();
            return Type::make_string();
        case TokenType::Keyword_File:
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
            if (looks_like_generic_instantiation()) {
                GenericRef ref = parse_generic_reference(name);
                Type t = Type::make_struct(ref.to_string());
                t.generic_ref = std::make_shared<GenericRef>(std::move(ref));
                return t;
            }
            if (lookahead_type(1) == TokenType::ColonColon) {
                std::string path = name;
                advance();                       
                while (current_.type == TokenType::ColonColon) {
                    advance();                   
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
                declared_value_names_.insert(param_name);
            }

            if (current_.type == TokenType::LeftBracket) {
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

    std::unique_ptr<Initializer> Parser::parse_initializer() {
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
            if (in_expr_argument_ && current_.type == TokenType::Greater) {
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
                if ((member == "is_same" || member == "is_convertible" ||
                    member == "has_member") && current_.type == TokenType::Less) {
                    advance();               
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
                report_error_template(ErrorCode::ScopeOperatorOperandInvalid,
                    { std::string("expression") });
                advance();                       
                if (current_.type == TokenType::Identifier) {
                    advance();                   
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
                if (id == "move") cm = PrimaryExpression::CopyMoveKind::Move;
                else if (id == "deep_copy") cm = PrimaryExpression::CopyMoveKind::DeepCopy;
                else if (id == "shallow_copy") cm = PrimaryExpression::CopyMoveKind::ShallowCopy;
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

    std::unique_ptr<Statement> Parser::parse_for_init() {
        return parse_declaration_or_statement();
    }

    std::unique_ptr<Expression> Parser::parse_optional_expression() {
        if (current_.type == TokenType::Semicolon || current_.type == TokenType::RightParen) {
            return nullptr;
        }
        return parse_expression();
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

    bool Parser::is_valid_identifier(const std::string& name) const {
        if (name.empty()) return false;
        if (!std::isalpha(name[0]) && name[0] != '_') return false;
        for (char c : name) {
            if (!std::isalnum(c) && c != '_') return false;
        }
        return true;
    }

    namespace {
        struct ConstValue {
            bool valid = false;
            bool is_float = false;
            long long int_value = 0;
            double float_value = 0.0;
            std::string text;   
        };

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
        if (current_.type != TokenType::Less) return true;
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
                           if (t == TokenType::EndOfFile) return false;
                           if (t == TokenType::LeftParen) { ++depth; ++i; continue; }
                           if (t == TokenType::RightParen) {
                               --depth;
                               ++i;
                               if (depth == 0) break;
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
                   }
                   else {
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
               }
               else if (second == TokenType::Identifier) {
                   i += 2;                                  
               }
                else if (second == TokenType::Colon) {
                    i += 2;                                  
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
                    TokenType after_param = lookahead_type(i + 1);
                    if (after_param != TokenType::Comma && after_param != TokenType::Greater) {
                        return false;
                    }
                    i += 1;
               }
            }
            else if (first_type_keyword && second == TokenType::Identifier) {
                i += 2;                                      
            }
            else {
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

    bool Parser::at_declaration_start() const {
        TokenType tt = current_.type;
        if (is_type_start_keyword(tt)) {
            return true;
        }
       if (tt == TokenType::Identifier) {
            static const std::unordered_set<std::string> kBuiltins = {
                "construct", "destruct", "copy", "move", "deep_copy", "shallow_copy",
            };
            const std::string name(current_.lexeme);
            if (kBuiltins.count(name) != 0) {
                return false;
            }
            if (declared_value_names_.find(name) != declared_value_names_.end()) {
                return false;
            }
           if (lookahead_type(1) == TokenType::Identifier ||
               declaration_name_after_star_suffix(1)) {
                return true;
            }
            if (declared_type_names_.find(name) != declared_type_names_.end() &&
                declaration_name_after_pointer_suffix(1)) {
                return true;
            }
            if (looks_like_generic_instantiation()) {
                std::size_t i = scan_generic_instantiation_end();
                if (i == std::string::npos) return false;
                TokenType after = lookahead_type(i);
                if (after == TokenType::Dot || after == TokenType::ColonColon) {
                    i += 2;
                    after = lookahead_type(i);
                }
                return after == TokenType::Identifier || after == TokenType::Star ||
                    after == TokenType::LeftBracket ||
                    declaration_name_after_pointer_suffix(i);
            }
            if (lookahead_type(1) == TokenType::ColonColon) {
                std::size_t i = 1;
                while (lookahead_type(i) == TokenType::ColonColon &&
                    lookahead_type(i + 1) == TokenType::Identifier) {
                    i += 2;
                }
                TokenType after = lookahead_type(i);
                return after == TokenType::Identifier || after == TokenType::Star ||
                    after == TokenType::LeftBracket ||
                    declaration_name_after_pointer_suffix(i);
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
        if (current_.type != TokenType::Identifier) return std::string::npos;
        std::size_t i = 1;
        while (lookahead_type(i) == TokenType::ColonColon &&
            lookahead_type(i + 1) == TokenType::Identifier) {
            i += 2;
        }
        if (lookahead_type(i) != TokenType::Less) return std::string::npos;

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
            case TokenType::Dot:
            case TokenType::ColonColon:
                break;
            default:
                return std::string::npos;
            }
        }
    }

    std::size_t Parser::scan_expr_argument_instantiation_end() const {
        if (current_.type != TokenType::Identifier) return std::string::npos;
        std::size_t i = 1;
        while (lookahead_type(i) == TokenType::ColonColon &&
            lookahead_type(i + 1) == TokenType::Identifier) {
            i += 2;
        }
        if (lookahead_type(i) != TokenType::Less) return std::string::npos;

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
                if (paren == 0) return std::string::npos;
                --paren;
                break;
            case TokenType::LeftBrace:
                ++brace;
                break;
            case TokenType::RightBrace:
                if (brace == 0) return std::string::npos;
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
        if (after == std::string::npos) return false;
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

    bool Parser::looks_like_pointer_type_cast() const {
        std::size_t index = 1;
        bool saw_pointer = false;
        while (true) {
            const TokenType type = lookahead_type(index);
            if (type == TokenType::Star || type == TokenType::Power) {
                saw_pointer = true;
                ++index;
                continue;
            }
            if (type == TokenType::Keyword_Const) {
                ++index;
                continue;
            }
            break;
        }
        return saw_pointer && lookahead_type(index) == TokenType::LeftParen;
    }

    bool Parser::declaration_name_after_pointer_suffix(std::size_t index) const {
        bool saw_pointer = false;
        while (true) {
            const TokenType type = lookahead_type(index);
            if (type == TokenType::Star || type == TokenType::Power) {
                saw_pointer = true;
                ++index;
                continue;
            }
            if (type == TokenType::Keyword_Const) {
                ++index;
                continue;
            }
            break;
        }
        return saw_pointer && lookahead_type(index) == TokenType::Identifier;
    }

    bool Parser::declaration_name_after_star_suffix(std::size_t index) const {
        bool saw_star = false;
        while (true) {
            const TokenType type = lookahead_type(index);
            if (type == TokenType::Star) {
                saw_star = true;
                ++index;
                continue;
            }
            if (type == TokenType::Keyword_Const) {
                ++index;
                continue;
            }
            break;
        }
        return saw_star && lookahead_type(index) == TokenType::Identifier;
    }

    bool Parser::cast_type_has_function_pointer_suffix() const {
        std::size_t index = 1;
        while (true) {
            const TokenType type = lookahead_type(index);
            if (type == TokenType::Star || type == TokenType::Power) {
                ++index;
                continue;
            }
            if (type == TokenType::Keyword_Const) {
                ++index;
                continue;
            }
            break;
        }
        if (lookahead_type(index) != TokenType::LeftParen) return false;
        const Token first = lookahead(index + 1);
        const bool parameter_start = is_builtin_or_void_type_keyword(first.type) ||
            first.type == TokenType::RightParen ||
            (first.type == TokenType::Identifier &&
                declared_type_names_.find(std::string(first.lexeme)) !=
                declared_type_names_.end());
        if (!parameter_start) return false;
        int depth = 0;
        std::size_t cursor = index;
        while (true) {
            const TokenType type = lookahead_type(cursor);
            if (type == TokenType::EndOfFile) return false;
            if (type == TokenType::LeftParen) {
                ++depth;
            }
            else if (type == TokenType::RightParen) {
                --depth;
                if (depth == 0) {
                    break;
                }
            }
            ++cursor;
        }
        return lookahead_type(cursor + 1) == TokenType::Star &&
            lookahead_type(cursor + 2) == TokenType::LeftParen;
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
                    }
                    else {
                        arg = parse_compile_time_expression();
                    }
                    if (arg == nullptr) return nullptr;
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
                if (operand == nullptr) return nullptr;
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
                    auto [types, names] = parse_parameter_list(&defaults);
                    arg.expr_param_types = std::move(types);
                    arg.expr_param_names = std::move(names);
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
                }
                else {
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
                }
                else if (current_.type == TokenType::LeftBrace) {
                    block_form = true;
                    advance();
                }
                else {
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
                        if (current_.type == TokenType::RightBrace) break;
                        auto stmt = parse_statement();
                        if (stmt != nullptr) {
                            body->statements.push_back(std::move(stmt));
                        }
                        else {
                            synchronize();
                        }
                    }
                    if (!expect(TokenType::RightBrace,
                        "expected '}' to close expression argument block")) {
                        return args;
                    }
                }
                else {
                    while (true) {
                        skip_newlines();
                        auto stmt = parse_statement();
                        if (stmt != nullptr) {
                            body->statements.push_back(std::move(stmt));
                        }
                        else {
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
                }
                else {
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
                        }
                        else {
                            arg.int_value = iv;
                        }
                        arg.text = arg.normalize();
                    }
                    else if (arg.is_string_constant) {
                        arg.text = arg.normalize();
                    }
                    else if (auto* prim = dynamic_cast<PrimaryExpression*>(arg.expression.get())) {
                        if (prim->kind == PrimaryExpression::Kind::Identifier) {
                            arg.text = prim->identifier;
                        }
                        else {
                            arg.text = compile_time_expr_text(arg.expression.get());
                        }
                    }
                   else {
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
            if (current_.type == TokenType::Less) break;
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
                            auto [types, names] = parse_parameter_list(&defaults);
                            param.expr_param_types = std::move(types);
                            param.expr_param_names = std::move(names);
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
                    }
                    else if (current_.type == TokenType::Identifier) {
                        std::string first(current_.lexeme);
                        if (lookahead_type(1) == TokenType::Identifier) {
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
                    else if (is_builtin_type_keyword(current_.type)) {
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
                    }
                    else {
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
            }
            else {
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
        advance(); 

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
            if (current_.type != TokenType::RightParen) {
                report_error(ErrorCode::DestructorWithParameters,
                    "destructor must not have parameters");
            }
        }
        else {
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

} 
