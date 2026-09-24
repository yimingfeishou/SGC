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

    std::unique_ptr<TopLevel> Parser::parse_function_definition(
        bool exported, SourceLocation export_location) {
        SourceLocation loc = current_location();
        Type ret_type = parse_type(true);

        std::string func_name;
        if (!check_identifier_name(func_name, "function name")) {
            return nullptr;
        }

        if (current_.type != TokenType::LeftParen) {
            if (exported) {
                report_error_template_at(export_location,
                    ErrorCode::ExportRequiresFunctionDefinition,
                    { std::string("variable declaration") });
            }
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
        bool variadic = false;
        auto [param_types, param_names] = parse_parameter_list(&param_defaults,
            &variadic);

        if (!expect(TokenType::RightParen, "expected ')' after parameter list")) {
            return nullptr;
        }

        if (current_.type == TokenType::Star || current_.type == TokenType::Power) {
            if (exported) {
                report_error_template_at(export_location,
                    ErrorCode::ExportRequiresFunctionDefinition,
                    { std::string("function pointer declaration") });
            }

            const bool double_pointer = current_.type == TokenType::Power;
            advance();
            std::shared_ptr<Type> element;
            if (variadic && !param_types.empty()) {
                element = std::make_shared<Type>(param_types.back());
                param_types.pop_back();
            }

            Type declared = Type::make_function(
                std::make_shared<Type>(std::move(ret_type)), std::move(param_types),
                variadic, std::move(element));
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
        func->is_export = exported;
        func->is_variadic = variadic;
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
        } else if (current_.type == TokenType::Identifier) {
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
            } else if (lookahead_type(1) == TokenType::Star ||
                lookahead_type(1) == TokenType::Power) {
                offset = 2;
            } else {
                offset = 1;
            }
        } else if (is_type_start_keyword(current_.type)) {
            if (lookahead_type(1) == TokenType::Star ||
                lookahead_type(1) == TokenType::Power) {
                offset = 2;
            } else {
                offset = 1;
            }
        } else {
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

    bool Parser::at_operator_symbol(TokenType type) const {
        switch (type) {
        case TokenType::Plus:
        case TokenType::Minus:
        case TokenType::Star:
        case TokenType::Slash:
        case TokenType::Percent:
        case TokenType::Power:
        case TokenType::Pipe:
        case TokenType::Caret:
        case TokenType::Tilde:
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
        case TokenType::AndAssign:
        case TokenType::OrAssign:
        case TokenType::XorAssign:
        case TokenType::Question:
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
            const bool shift_head = op_text == "<" || op_text == ">";
            const TokenType repeat_type = current_.type;
            advance();

            if (shift_head && current_.type == repeat_type) {
                op_text += op_text;
                advance();
                if (current_.type == TokenType::Assign) {
                    op_text += "=";
                    advance();
                }
            } else if (shift_head && current_.type == TokenType::LessEqual &&
                repeat_type == TokenType::Less) {
                op_text = "<<=";
                advance();
            } else if (shift_head && current_.type == TokenType::GreaterEqual &&
                repeat_type == TokenType::Greater) {
                op_text = ">>=";
                advance();
            }

            if (op_text == "?") {
                if (current_.type == TokenType::Colon) {
                    advance();
                }
                op_text = "?:";
            }

            if (op_text == "[") {
                if (!expect(TokenType::RightBracket, "expected ']' in operator[]")) {
                    return nullptr;
                }
                op_text = "[]";
            }
        } else {
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
        bool variadic = false;
        auto [param_types, param_names] = parse_parameter_list(&param_defaults,
            &variadic);

        if (!expect(TokenType::RightParen, "expected ')' after parameter list")) {
            return nullptr;
        }

        if (variadic) {
            report_error_template_at(loc,
                ErrorCode::OperatorOverloadVariadicNotAllowed,
                { op_text.empty() ? conversion_target.to_string() : op_text });
        }

        auto body = parse_block();
        if (body == nullptr) {
            report_error(ErrorCode::ExpressionSyntaxError, "expected operator body");
            return nullptr;
        }

        std::string raw_name;
        if (conversion) {
            raw_name = std::string("conv_") + conversion_target.to_string();
        } else if (op_text == "[]") {
            raw_name = "index";
        } else if (op_text == "->") {
            raw_name = "arrow";
        } else if (op_text == "++") {
            raw_name = param_types.size() == 2 ? "post_inc" : "pre_inc";
        } else if (op_text == "--") {
            raw_name = param_types.size() == 2 ? "post_dec" : "pre_dec";
        } else {
            raw_name = op_text;
        }

        std::string sanitized;
        sanitized.reserve(raw_name.size());

        for (char c : raw_name) {
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '_') {
                sanitized.push_back(c);
            } else {
                sanitized.push_back('_');
                static const char kHexDigits[] = "0123456789abcdef";
                const unsigned char code = static_cast<unsigned char>(c);
                sanitized.push_back(kHexDigits[(code >> 4) & 0xF]);
                sanitized.push_back(kHexDigits[code & 0xF]);
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
            if (attr == "nocopy") {
                no_copy = true;
            } else if (attr == "nomove") {
                no_move = true;
            }
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

            if (current_.type == TokenType::Keyword_Export) {
                report_error_template(ErrorCode::ExportNotAllowedInContext,
                    { std::string("a struct definition") });
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
                skip_newlines();
                continue;
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
                    if (token == TokenType::EndOfFile) { break; }
                    if (token == TokenType::LeftParen) {
                        paren_depth++;
                    } else if (token == TokenType::RightParen) {
                        paren_depth--;
                        if (paren_depth == 0) { break; }
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
                        } else if (current_.type == TokenType::RightBrace) {
                            if (brace_depth == 0) { break; }
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
                bool variadic = false;

                if (current_.type != TokenType::RightParen) {
                    auto [types, names] = parse_parameter_list(nullptr, &variadic);
                    param_types = std::move(types);
                    param_names = std::move(names);
                }

                if (!expect(TokenType::RightParen, "expected ')' after parameter list in function pointer")) {
                    break;
                }

                if (!expect(TokenType::Star, "expected '*' after function pointer parameter list")) {
                    break;
                }

                std::shared_ptr<Type> element;
                if (variadic && !param_types.empty()) {
                    element = std::make_shared<Type>(param_types.back());
                    param_types.pop_back();
                }

                base_type = Type::make_function(
                    std::make_shared<Type>(base_type),
                    param_types, variadic, std::move(element));
            } else if (current_.type == TokenType::Star) {
                advance();
                base_type = Type::make_pointer(std::make_shared<Type>(base_type));
            } else if (current_.type == TokenType::Power) {
                advance();
                base_type = Type::make_pointer(std::make_shared<Type>(
                    Type::make_pointer(std::make_shared<Type>(base_type))));
            } else if (current_.type == TokenType::Keyword_Const) {
                advance();
                base_type.is_const = true;
            } else {
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
                } else if (current_.type == TokenType::RightBracket) {
                    if (!is_first_dimension || !allow_empty_array) {
                        report_error_at(loc, ErrorCode::ArraySizeNotConstant,
                            "array size must be specified here");
                    }
                } else {
                    auto expr = parse_compile_time_expression();

                    if (expr != nullptr) {
                        long long iv = 0;
                        double dv = 0.0;
                        bool is_flt = false;

                        if (fold_constant_expression(expr.get(), iv, dv, is_flt) && !is_flt && iv >= 0) {
                            size = static_cast<size_t>(iv);
                        } else if (is_first_dimension && out_size_expr != nullptr) {
                            *out_size_expr = std::move(expr);
                        } else {
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
        bool variadic = false;

        if (current_.type != TokenType::RightParen) {
            auto [types, names] = parse_parameter_list(nullptr, &variadic);
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
        std::shared_ptr<Type> element;
        if (variadic && !params.empty()) {
            element = std::make_shared<Type>(params.back());
            params.pop_back();
        }

        return Type::make_function(std::make_shared<Type>(std::move(base_type)),
            std::move(params), variadic, std::move(element));
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
        std::vector<std::unique_ptr<Expression>>* defaults, bool* out_variadic,
        bool* out_c_variadic) {
        std::vector<Type> param_types;
        std::vector<std::string> param_names;
        bool variadic = false;
        bool c_variadic = false;
        SourceLocation variadic_loc;

        if (current_.type == TokenType::RightParen) {
            if (out_variadic != nullptr) { *out_variadic = false; }
            if (out_c_variadic != nullptr) { *out_c_variadic = false; }
            return { std::move(param_types), std::move(param_names) };
        }

        do {
            if (current_.type == TokenType::Ellipsis) {
                if (out_c_variadic == nullptr) {
                    report_error(ErrorCode::ExpressionSyntaxError,
                        "an unnamed '...' parameter is only allowed in an extern declaration");
                } else {
                    c_variadic = true;
                }
                advance();
                break;
            }

            Type param_type = parse_type(true);

            bool parameter_is_variadic = false;
            if (current_.type == TokenType::Ellipsis) {
                parameter_is_variadic = true;
                variadic_loc = current_.location;
                advance();
            }

            std::string param_name;
            if (current_.type == TokenType::Identifier) {
                param_name = current_.lexeme;
                advance();
                declared_value_names_.insert(param_name);
            }

            if (parameter_is_variadic) {
                if (variadic) {
                    report_error(ErrorCode::VariadicParameterNotLast,
                        "a variadic parameter must be the last parameter in the parameter list");
                }
                variadic = true;
                if (param_type.kind == TypeKind::Void) {
                    report_error_at(variadic_loc, ErrorCode::VariadicElementTypeIsVoid,
                        "the element type of a variadic parameter cannot be void");
                }
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
                    } else if (current_.type != TokenType::RightBracket) {
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
            } else if (current_.type == TokenType::LeftParen && param_type.kind != TypeKind::Function) {
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
                if (parameter_is_variadic) {
                    report_error_at(variadic_loc,
                        ErrorCode::VariadicParameterDefaultArgument,
                        "a variadic parameter may not declare a default argument");
                }
                default_value = parse_expression();
                if (default_value == nullptr) {
                    report_error(ErrorCode::ExpressionSyntaxError,
                        "expected expression for default argument");
                }
                if (defaults == nullptr) {
                    report_error(ErrorCode::ExpressionSyntaxError,
                        "default arguments are not allowed in this declaration");
                } else if (param_name.empty()) {
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
            if (variadic) {
                report_error(ErrorCode::VariadicParameterNotLast,
                    "a variadic parameter must be the last parameter in the parameter list");
            }
        } while (true);

        if (out_variadic != nullptr) { *out_variadic = variadic; }
        if (out_c_variadic != nullptr) { *out_c_variadic = c_variadic; }
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
        } else {
            SourceLocation loc = current_location();
            auto expr = parse_expression();
            if (expr == nullptr) {
                report_error(ErrorCode::ExpressionSyntaxError, "expected expression in initializer");
                return nullptr;
            }
            return std::make_unique<ExpressionInitializer>(loc, std::move(expr));
        }
    }

    bool Parser::is_valid_identifier(const std::string& name) const {
        if (name.empty()) { return false; }
        if (!std::isalpha(name[0]) && name[0] != '_') { return false; }
        for (char c : name) {
            if (!std::isalnum(c) && c != '_') { return false; }
        }
        return true;
    }

    bool Parser::at_struct_attribute() const {
        if (current_.type != TokenType::LeftBracket) { return false; }
        Token name = lookahead(1);
        if (name.type != TokenType::Identifier) { return false; }
        if (name.lexeme != "nocopy" && name.lexeme != "nomove") { return false; }
        return lookahead_type(2) == TokenType::RightBracket;
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
                if (i == std::string::npos) { return false; }
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
        if (current_.type != TokenType::Identifier) { return false; }
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

        if (!matched) { return false; }
        return lookahead_type(1) == TokenType::LeftParen;
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

        if (lookahead_type(index) != TokenType::LeftParen) { return false; }
        const Token first = lookahead(index + 1);
        const bool parameter_start = is_builtin_or_void_type_keyword(first.type) ||
            first.type == TokenType::RightParen ||
            (first.type == TokenType::Identifier &&
                declared_type_names_.find(std::string(first.lexeme)) !=
                declared_type_names_.end());
        if (!parameter_start) { return false; }

        int depth = 0;
        std::size_t cursor = index;

        while (true) {
            const TokenType type = lookahead_type(cursor);
            if (type == TokenType::EndOfFile) { return false; }
            if (type == TokenType::LeftParen) {
                ++depth;
            } else if (type == TokenType::RightParen) {
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

    std::unique_ptr<SpecialMemberFunction> Parser::parse_special_member_function() {
        SourceLocation loc = current_location();
        std::string_view lexeme = current_.lexeme;
        SpecialMemberFunction::Kind kind = SpecialMemberFunction::Kind::Constructor;
        if (lexeme == "constructor") {
            kind = SpecialMemberFunction::Kind::Constructor;
        } else if (lexeme == "destructor") {
            kind = SpecialMemberFunction::Kind::Destructor;
        } else if (lexeme == "copy_constructor") {
            kind = SpecialMemberFunction::Kind::CopyConstructor;
        } else if (lexeme == "move_constructor") {
            kind = SpecialMemberFunction::Kind::MoveConstructor;
        } else if (lexeme == "copy_assignment") {
            kind = SpecialMemberFunction::Kind::CopyAssignment;
        } else if (lexeme == "move_assignment") {
            kind = SpecialMemberFunction::Kind::MoveAssignment;
        } else {
            report_error(ErrorCode::ExpressionSyntaxError, "unknown special member function");
            return nullptr;
        }

        advance();

        auto member = std::make_unique<SpecialMemberFunction>(loc, kind);
        if (!expect(TokenType::LeftParen, "expected '(' in special member function")) {
            return nullptr;
        }

        if (kind == SpecialMemberFunction::Kind::Constructor) {
            bool variadic = false;
            auto [types, names] = parse_parameter_list(&member->parameter_defaults,
                &variadic);
            member->parameters = std::move(types);
            member->parameter_names = std::move(names);

            if (variadic) {
                report_error_template_at(loc,
                    ErrorCode::SpecialMemberVariadicNotAllowed,
                    { std::string(member->kind_name()) });
            }
        } else if (kind == SpecialMemberFunction::Kind::Destructor) {
            if (current_.type != TokenType::RightParen) {
                report_error(ErrorCode::DestructorWithParameters,
                    "destructor must not have parameters");
            }
        } else {
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
