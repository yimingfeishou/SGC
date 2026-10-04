#include "../semantic/type_checker.hpp"
#include "../parser/ast_visitor.hpp"
#include "type_checker_detail.hpp"
#include "../semantic/constant_folding.hpp"
#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

using namespace gallt::AST;

namespace gallt {
    using namespace type_checker_detail;

    namespace {

        std::string escape_string_literal(const std::string& text) {
            std::string out;
            out.reserve(text.size() + 2);
            out.push_back('"');

            for (unsigned char byte : text) {
                switch (byte) {
                case '\\': out += "\\\\"; break;
                case '"': out += "\\\""; break;
                case '\n': out += "\\n"; break;
                case '\t': out += "\\t"; break;
                case '\r': out += "\\r"; break;
                default:
                    if (byte < 0x20 || byte == 0x7F) {
                        char buffer[8];
                        std::snprintf(buffer, sizeof(buffer), "\\x%02x", byte);
                        out += buffer;
                    } else {
                        out.push_back(static_cast<char>(byte));
                    }
                    break;
                }
            }

            out.push_back('"');
            return out;
        }

        std::string escape_char_literal(unsigned char byte) {
            switch (byte) {
            case '\\': return "'\\\\'";
            case '\'': return "'\\''";
            case '\n': return "'\\n'";
            case '\t': return "'\\t'";
            case '\r': return "'\\r'";
            default: break;
            }

            if (byte < 0x20 || byte == 0x7F) {
                char buffer[8];
                std::snprintf(buffer, sizeof(buffer), "'\\x%02x'", byte);
                return std::string(buffer);
            }

            return std::string("'") + static_cast<char>(byte) + "'";
        }

        std::string format_float_literal(double value, const AST::Type& type) {
            std::ostringstream format;
            format.precision(17);
            format << value;
            std::string text = format.str();

            if (text.find('.') == std::string::npos &&
                text.find('e') == std::string::npos &&
                text.find('E') == std::string::npos &&
                type.kind != TypeKind::Double) {
                text += ".0";
            }

            return text;
        }

    }

    void TypeChecker::configure_constexpr_host() {
        ConstexprInterpreter::Host host;

        host.lookup_constant = [this](const std::string& name, ConstexprValue& out) {
            for (auto scope = constexpr_values_.rbegin();
                scope != constexpr_values_.rend(); ++scope) {
                auto found = scope->find(name);
                if (found != scope->end()) {
                    out = found->second;
                    return true;
                }
            }

            return false;
        };

        host.resolve_function = [this](const std::string& name,
            const std::vector<ConstexprValue>& arguments) {
            return resolve_constexpr_function(name, arguments);
        };

        host.layout_of_type = [this](const AST::Type& type, std::size_t& size,
            std::size_t& align) {
            return type_layout(type, size, align);
        };

        host.layout_of_name = [this](const std::string& name, std::size_t& size,
            std::size_t& align) {
            return type_layout_of_name(name, size, align);
        };

        host.lookup_struct = [this](const std::string& name)
            -> const AST::StructDefinition* {
            auto found = struct_defs_.find(name);
            return found == struct_defs_.end() ? nullptr : found->second;
        };

        constexpr_interpreter_.set_host(std::move(host));
    }

    bool TypeChecker::constexpr_type_is_compilable(const AST::Type& type,
        std::string& offender_type, std::string& offender_member) {
        switch (type.kind) {
        case TypeKind::Int:
        case TypeKind::Lint:
        case TypeKind::Uint:
        case TypeKind::Luint:
        case TypeKind::Float:
        case TypeKind::Double:
        case TypeKind::Char:
        case TypeKind::Uchar:
        case TypeKind::Bool:
        case TypeKind::String:
        case TypeKind::Void:
            return true;
        case TypeKind::Array:
            if (!type.array_size.has_value() || type.element_type == nullptr) {
                offender_type = type.to_string();
                return false;
            }
            return constexpr_type_is_compilable(*type.element_type, offender_type,
                offender_member);
        case TypeKind::Struct: {
            AST::StructDefinition* definition = get_struct_definition(
                type.struct_name);
            if (definition == nullptr) {
                offender_type = type.to_string();
                return false;
            }

            for (const StructDefinition::Member& member : definition->members) {
                std::string nested_type;
                std::string nested_member;
                if (constexpr_type_is_compilable(member.type, nested_type,
                    nested_member)) {
                    continue;
                }

                offender_type = nested_type;
                offender_member = nested_member.empty() ? member.name
                                                        : nested_member;
                return false;
            }

            return true;
        }
        default:
            offender_type = type.to_string();
            return false;
        }
    }

    void TypeChecker::validate_constexpr_types(AST::FunctionDefinition* node) {
        auto report_type = [&](const AST::Type& type, SourceLocation loc) {
            if (type.kind == TypeKind::Void) { return; }
            std::string offender_type;
            std::string offender_member;
            if (constexpr_type_is_compilable(type, offender_type, offender_member)) {
                return;
            }

            if (!offender_member.empty()) {
                report_error_template(loc,
                    ErrorCode::ConstexprStructMemberNotCompilable,
                    { offender_member });
            } else {
                report_error_template(loc, ErrorCode::ConstexprNonCompilableType,
                    { offender_type });
            }
        };

        report_type(node->return_type, node->location);

        for (const AST::Type& parameter : node->parameters) {
            report_type(parameter, node->location);
        }
    }

    void TypeChecker::report_constexpr_operation(const AST::Node* node,
        const std::string& operation, bool side_effect) {
        const std::string owner = constexpr_owner_ != nullptr
            ? constexpr_owner_->name : std::string();
        const SourceLocation loc = node != nullptr ? node->location
                                                   : SourceLocation();

        if (side_effect) {
            report_error_template(loc, ErrorCode::ConstexprSideEffectStatement,
                { owner });
        } else {
            report_error_template(loc, ErrorCode::ConstexprDisallowedOperation,
                { owner, operation });
        }
    }

    void TypeChecker::validate_constexpr_statement(const AST::Statement* stmt) {
        if (stmt == nullptr) { return; }

        if (auto* block = dynamic_cast<const Block*>(stmt)) {
            for (const auto& child : block->statements) {
                validate_constexpr_statement(child.get());
            }
            return;
        }

        if (auto* declaration = dynamic_cast<const VariableDeclaration*>(stmt)) {
            std::string offender_type;
            std::string offender_member;

            if (!constexpr_type_is_compilable(declaration->type, offender_type,
                offender_member)) {
                if (!offender_member.empty()) {
                    report_error_template(declaration->location,
                        ErrorCode::ConstexprStructMemberNotCompilable,
                        { offender_member });
                } else {
                    report_error_template(declaration->location,
                        ErrorCode::ConstexprNonCompilableType, { offender_type });
                }
            }

            if (declaration->array_size_expr != nullptr) {
                validate_constexpr_expression(declaration->array_size_expr.get());
            }

            if (declaration->initializer != nullptr) {
                if (auto* expression = dynamic_cast<const ExpressionInitializer*>(
                    declaration->initializer.get())) {
                    validate_constexpr_expression(expression->expr.get());
                } else if (auto* array = dynamic_cast<const ArrayInitializer*>(
                    declaration->initializer.get())) {
                    for (const auto& element : array->elements) {
                        if (auto* expression = dynamic_cast<const ExpressionInitializer*>(
                            element.get())) {
                            validate_constexpr_expression(expression->expr.get());
                        } else if (auto* nested = dynamic_cast<const ArrayInitializer*>(
                            element.get())) {
                            for (const auto& inner : nested->elements) {
                                if (auto* inner_expression =
                                    dynamic_cast<const ExpressionInitializer*>(
                                        inner.get())) {
                                    validate_constexpr_expression(
                                        inner_expression->expr.get());
                                }
                            }
                        }
                    }
                }
            }
            return;
        }

        if (auto* statement = dynamic_cast<const IfStatement*>(stmt)) {
            validate_constexpr_expression(statement->condition.get());
            validate_constexpr_statement(statement->then_block.get());
            validate_constexpr_statement(statement->else_block.get());
            return;
        }

        if (auto* statement = dynamic_cast<const ForStatement*>(stmt)) {
            validate_constexpr_statement(statement->init.get());
            validate_constexpr_expression(statement->condition.get());
            validate_constexpr_expression(statement->step.get());
            validate_constexpr_statement(statement->body.get());
            return;
        }

        if (auto* statement = dynamic_cast<const WhileStatement*>(stmt)) {
            validate_constexpr_expression(statement->condition.get());
            validate_constexpr_statement(statement->body.get());
            return;
        }

        if (auto* statement = dynamic_cast<const ReturnStatement*>(stmt)) {
            validate_constexpr_expression(statement->value.get());
            return;
        }

        if (auto* statement = dynamic_cast<const ExpressionStatement*>(stmt)) {
            validate_constexpr_expression(statement->expr.get());
            return;
        }

        if (dynamic_cast<const BreakStatement*>(stmt) != nullptr ||
            dynamic_cast<const EmptyStatement*>(stmt) != nullptr) {
            return;
        }

        if (dynamic_cast<const DestructStatement*>(stmt) != nullptr) {
            report_constexpr_operation(stmt, "destruct", true);
            return;
        }

        if (dynamic_cast<const EmitStatement*>(stmt) != nullptr) {
            report_constexpr_operation(stmt, "emit", true);
            return;
        }

        report_constexpr_operation(stmt, "statement", false);
    }

    void TypeChecker::validate_constexpr_expression(const AST::Expression* expr) {
        if (expr == nullptr) { return; }

        if (auto* primary = dynamic_cast<const PrimaryExpression*>(expr)) {
            switch (primary->kind) {
            case PrimaryExpression::Kind::Literal:
            case PrimaryExpression::Kind::Identifier:
                return;
            case PrimaryExpression::Kind::Parens:
                validate_constexpr_expression(primary->paren_expr.get());
                return;
            case PrimaryExpression::Kind::Heap:
                report_constexpr_operation(primary, "heap", true);
                return;
            case PrimaryExpression::Kind::Construct:
            case PrimaryExpression::Kind::PlacementConstruct:
                report_constexpr_operation(primary, "construct", true);
                return;
            case PrimaryExpression::Kind::CopyMove:
                report_constexpr_operation(primary,
                    primary->copy_move_kind == PrimaryExpression::CopyMoveKind::DeepCopy
                        ? "deep copy" : "copy or move", true);
                return;
            default:
                report_constexpr_operation(primary, "expression", false);
                return;
            }
        }

        if (auto* assignment = dynamic_cast<const AssignmentExpression*>(expr)) {
            validate_constexpr_expression(assignment->left.get());
            validate_constexpr_expression(assignment->right.get());
            return;
        }

        if (auto* expression = dynamic_cast<const LogicalOrExpression*>(expr)) {
            validate_constexpr_expression(expression->left.get());
            validate_constexpr_expression(expression->right.get());
            return;
        }

        if (auto* expression = dynamic_cast<const LogicalAndExpression*>(expr)) {
            validate_constexpr_expression(expression->left.get());
            validate_constexpr_expression(expression->right.get());
            return;
        }

        if (auto* expression = dynamic_cast<const ComparisonExpression*>(expr)) {
            validate_constexpr_expression(expression->left.get());
            validate_constexpr_expression(expression->right.get());
            return;
        }

        if (auto* expression = dynamic_cast<const AdditiveExpression*>(expr)) {
            validate_constexpr_expression(expression->left.get());
            validate_constexpr_expression(expression->right.get());
            return;
        }

        if (auto* expression = dynamic_cast<const MultiplicativeExpression*>(expr)) {
            validate_constexpr_expression(expression->left.get());
            validate_constexpr_expression(expression->right.get());
            return;
        }

        if (auto* expression = dynamic_cast<const PowerExpression*>(expr)) {
            validate_constexpr_expression(expression->left.get());
            validate_constexpr_expression(expression->right.get());
            return;
        }

        if (auto* expression = dynamic_cast<const BitwiseExpression*>(expr)) {
            validate_constexpr_expression(expression->left.get());
            validate_constexpr_expression(expression->right.get());
            return;
        }

        if (auto* expression = dynamic_cast<const ShiftExpression*>(expr)) {
            validate_constexpr_expression(expression->left.get());
            validate_constexpr_expression(expression->right.get());
            return;
        }

        if (auto* expression = dynamic_cast<const ConditionalExpression*>(expr)) {
            validate_constexpr_expression(expression->condition.get());
            validate_constexpr_expression(expression->then_expr.get());
            validate_constexpr_expression(expression->else_expr.get());
            return;
        }

        if (auto* expression = dynamic_cast<const UnaryExpression*>(expr)) {
            switch (expression->op) {
            case UnaryExpression::Operator::Increment:
            case UnaryExpression::Operator::Decrement:
            case UnaryExpression::Operator::LogicalNot:
            case UnaryExpression::Operator::UnaryPlus:
            case UnaryExpression::Operator::UnaryMinus:
            case UnaryExpression::Operator::BitwiseNot:
                validate_constexpr_expression(expression->operand.get());
                return;
            case UnaryExpression::Operator::AddressOf:
                report_constexpr_operation(expression, "address-of", false);
                return;
            case UnaryExpression::Operator::Dereference:
                report_constexpr_operation(expression, "dereference", false);
                return;
            }
            report_constexpr_operation(expression, "unary operator", false);
            return;
        }

        if (auto* postfix = dynamic_cast<const PostfixExpression*>(expr)) {
            switch (postfix->op) {
            case PostfixExpression::Operator::Subscript:
                validate_constexpr_expression(postfix->base.get());
                validate_constexpr_expression(postfix->subscript_expr.get());
                return;
            case PostfixExpression::Operator::Dot:
                validate_constexpr_expression(postfix->base.get());
                return;
            case PostfixExpression::Operator::FunctionCall:
                for (const auto& argument : postfix->arguments) {
                    validate_constexpr_expression(argument.get());
                }
                validate_constexpr_call_expression(postfix);
                return;
            case PostfixExpression::Operator::Increment:
            case PostfixExpression::Operator::Decrement:
                validate_constexpr_expression(postfix->base.get());
                return;
            case PostfixExpression::Operator::Cast:
                if (!postfix->cast_type.is_arithmetic() &&
                    postfix->cast_type.kind != TypeKind::String) {
                    report_constexpr_operation(postfix, "cast", false);
                    return;
                }
                validate_constexpr_expression(postfix->base.get());
                return;
            case PostfixExpression::Operator::Arrow:
                report_constexpr_operation(postfix, "arrow", false);
                return;
            case PostfixExpression::Operator::PackExpand:
                report_constexpr_operation(postfix, "pack expansion", false);
                return;
            }
            report_constexpr_operation(postfix, "postfix operator", false);
            return;
        }

        if (dynamic_cast<const CompileTimePropertyExpression*>(expr) != nullptr) {
            report_constexpr_operation(expr, "compile-time property", false);
            return;
        }

        report_constexpr_operation(expr, "expression", false);
    }

    void TypeChecker::validate_constexpr_call_expression(
        const AST::PostfixExpression* call) {
        std::string name;
        if (auto* primary = dynamic_cast<const PrimaryExpression*>(call->base.get())) {
            if (primary->kind == PrimaryExpression::Kind::Identifier) {
                name = primary->identifier;
            }
        }

        if (name.empty()) {
            report_constexpr_operation(call, "function call", false);
            return;
        }

        if (is_constexpr_call_expression(call)) { return; }

        if (name == "size" || name == "align") { return; }

        if (is_string_builtin_name(name)) {
            static const char* const kSideEffectBuiltins[] = {
                "strmove", "strsetchar", "strread", "strwrite",
                "strwritefile",
            };

            for (const char* builtin : kSideEffectBuiltins) {
                if (name == builtin) {
                    report_constexpr_operation(call, name, true);
                    return;
                }
            }

            return;
        }

        if (is_file_builtin_name(name) || name == "output" || name == "input" ||
            name == "free" || name == "heap") {
            report_constexpr_operation(call, name, true);
            return;
        }

        report_constexpr_operation(call, name, false);
    }

    bool TypeChecker::evaluate_constexpr_expression(const AST::Expression* expr,
        ConstexprValue& out) {
        const std::size_t errors_before = diag_.error_count();
        constexpr_interpreter_.reset_failure();

        if (constexpr_interpreter_.evaluate(expr, out)) { return true; }

        if (constexpr_interpreter_.failure() == ConstexprFailure::LimitExceeded &&
            diag_.error_count() == errors_before) {
            report_error_template(expr != nullptr ? expr->location
                                                  : SourceLocation(),
                ErrorCode::ExprParameterRecursionLimitExceeded, {});
        }

        return false;
    }

    bool TypeChecker::evaluate_constexpr_function_call(
        const AST::FunctionDefinition* function,
        const std::vector<ConstexprValue>& arguments, ConstexprValue& out) {
        const std::size_t errors_before = diag_.error_count();
        constexpr_interpreter_.reset_failure();

        if (constexpr_interpreter_.call(function, arguments, out)) { return true; }

        if (constexpr_interpreter_.failure() == ConstexprFailure::LimitExceeded &&
            diag_.error_count() == errors_before) {
            report_error_template(function != nullptr ? function->location
                                                      : SourceLocation(),
                ErrorCode::ExprParameterRecursionLimitExceeded, {});
        }

        return false;
    }

    bool TypeChecker::evaluate_constexpr_initializer(
        const AST::Initializer* init, const AST::Type& type,
        ConstexprValue& out) {
        const std::size_t errors_before = diag_.error_count();
        constexpr_interpreter_.reset_failure();

        if (constexpr_interpreter_.evaluate_initializer(init, type, out)) {
            return true;
        }

        if (constexpr_interpreter_.failure() == ConstexprFailure::LimitExceeded &&
            diag_.error_count() == errors_before) {
            report_error_template(init != nullptr ? init->location
                                                  : SourceLocation(),
                ErrorCode::ExprParameterRecursionLimitExceeded, {});
        }

        return false;
    }

    const AST::FunctionDefinition* TypeChecker::resolve_constexpr_function(
        const std::string& name, const std::vector<ConstexprValue>& arguments) {
        std::vector<Symbol>* set = sym_table_.lookup_overloads(name);

        if (set == nullptr) {
            Symbol* symbol = sym_table_.lookup(name);
            if (symbol == nullptr || symbol->kind != SymbolKind::Function) {
                return nullptr;
            }
            if (!is_declared_function(symbol->function_node) ||
                !symbol->function_node->is_constexpr_function) {
                return nullptr;
            }
            if (symbol->function_node->parameters.size() != arguments.size()) {
                return nullptr;
            }
            return symbol->function_node;
        }

        const AST::FunctionDefinition* best = nullptr;
        int best_rank = 0;

        for (Symbol& symbol : *set) {
            const AST::FunctionDefinition* function = symbol.function_node;
            if (!is_declared_function(function) ||
                !function->is_constexpr_function) {
                continue;
            }
            if (symbol.is_variadic ||
                function->parameters.size() != arguments.size()) {
                continue;
            }

            int total = 0;
            bool viable = true;

            for (std::size_t i = 0; i < arguments.size(); ++i) {
                const int rank = conversion_rank(arguments[i].type,
                    function->parameters[i]);
                if (rank < 0) {
                    viable = false;
                    break;
                }
                total += rank;
            }

            if (!viable) { continue; }

            if (best == nullptr || total < best_rank) {
                best = function;
                best_rank = total;
            }
        }

        return best;
    }

    void TypeChecker::record_constexpr_declaration(AST::VariableDeclaration* decl) {
        if (decl == nullptr || !decl->type.is_const || decl->initializer == nullptr) {
            return;
        }

        std::string offender_type;
        std::string offender_member;
        if (!constexpr_type_is_compilable(decl->type, offender_type,
            offender_member)) {
            return;
        }

        ConstexprValue value;

        if (auto* expression = dynamic_cast<ExpressionInitializer*>(
            decl->initializer.get())) {
            if (auto* call = dynamic_cast<PostfixExpression*>(expression->expr.get())) {
                auto found = constexpr_calls_.find(call);
                if (found != constexpr_calls_.end()) {
                    value = found->second;
                } else if (is_constexpr_call_expression(call)) {
                    return;
                } else if (!evaluate_constexpr_expression(expression->expr.get(),
                    value)) {
                    return;
                }
            } else if (!evaluate_constexpr_expression(expression->expr.get(),
                value)) {
                return;
            }
        } else if (!evaluate_constexpr_initializer(decl->initializer.get(),
            decl->type, value)) {
            return;
        }

        ConstexprValue converted;
        if (!ConstexprInterpreter::coerce(value, decl->type, converted)) {
            return;
        }

        if (constexpr_values_.empty()) { constexpr_values_.emplace_back(); }
        constexpr_values_.back()[decl->name] = converted;

        if (converted.type.is_arithmetic()) {
            ConstValue numeric;
            numeric.is_float = converted.type.is_floating();
            numeric.float_value = converted.as_floating();
            numeric.int_value = converted.as_integer();

            if (const_values_.empty()) { const_values_.emplace_back(); }
            const_values_.back()[decl->name] = numeric;
        }
    }

    void TypeChecker::check_constexpr_call(AST::PostfixExpression* call,
        const Symbol& callee) {
        const AST::FunctionDefinition* function = callee.function_node;
        if (!is_declared_function(function) ||
            !function->is_constexpr_function) {
            return;
        }
        if (constexpr_owner_ != nullptr) { return; }

        std::vector<AST::Expression*> arguments;
        arguments.reserve(call->arguments.size() + call->appended_defaults.size());

        for (auto& argument : call->arguments) {
            arguments.push_back(argument.get());
        }

        for (AST::Expression* appended : call->appended_defaults) {
            arguments.push_back(appended);
        }

        for (std::size_t i = 0; i < arguments.size(); ++i) {
            if (is_compile_time_constant_expression(arguments[i])) { continue; }
            report_error_template(arguments[i]->location,
                ErrorCode::ConstexprArgumentNotConstant,
                { function->name, std::to_string(i + 1) });
        }

        std::vector<ConstexprValue> values;
        values.reserve(arguments.size());

        for (AST::Expression* argument : arguments) {
            ConstexprValue value;
            if (!evaluate_constexpr_expression(argument, value)) { return; }
            values.push_back(std::move(value));
        }

        ConstexprValue result;
        if (!evaluate_constexpr_function_call(function, values, result)) {
            return;
        }

        constexpr_calls_[call] = std::move(result);
    }

    bool TypeChecker::is_constexpr_call_expression(
        const AST::PostfixExpression* call) {
        if (call == nullptr ||
            call->op != PostfixExpression::Operator::FunctionCall) {
            return false;
        }

        if (constexpr_calls_.find(call) != constexpr_calls_.end()) { return true; }

        std::string name;
        if (auto* primary = dynamic_cast<const PrimaryExpression*>(call->base.get())) {
            if (primary->kind == PrimaryExpression::Kind::Identifier) {
                name = primary->identifier;
            }
        }

        if (name.empty()) { return false; }

        if (std::vector<Symbol>* set = sym_table_.lookup_overloads(name)) {
            for (const Symbol& symbol : *set) {
                if (is_declared_function(symbol.function_node) &&
                    symbol.function_node->is_constexpr_function) {
                    return true;
                }
            }
            return false;
        }

        Symbol* symbol = sym_table_.lookup(name);
        return symbol != nullptr && symbol->kind == SymbolKind::Function &&
            is_declared_function(symbol->function_node) &&
            symbol->function_node->is_constexpr_function;
    }

    std::unique_ptr<AST::Expression> TypeChecker::make_constexpr_literal(
        SourceLocation loc, const ConstexprValue& value) {
        auto pool = [this](std::string text) -> std::string_view {
            constexpr_lexemes_.push_back(std::move(text));
            return constexpr_lexemes_.back();
        };

        std::unique_ptr<PrimaryExpression> literal;

        switch (value.type.kind) {
        case TypeKind::Bool:
            literal = std::make_unique<PrimaryExpression>(loc, Token(
                TokenType::BoolLiteral, loc,
                pool(value.int_value != 0 ? "true" : "false")));
            break;
        case TypeKind::Char:
        case TypeKind::Uchar:
            literal = std::make_unique<PrimaryExpression>(loc, Token(
                TokenType::CharLiteral, loc,
                pool(escape_char_literal(static_cast<unsigned char>(
                    value.int_value)))));
            break;
        case TypeKind::Int:
        case TypeKind::Lint:
        case TypeKind::Uint:
        case TypeKind::Luint: {
            std::string text = std::to_string(value.int_value);
            literal = std::make_unique<PrimaryExpression>(loc, Token(
                TokenType::IntegerLiteral, loc, pool(std::move(text))));
            break;
        }
        case TypeKind::Float:
        case TypeKind::Double: {
            std::string text = format_float_literal(
                value.type.kind == TypeKind::Float
                    ? static_cast<double>(static_cast<float>(value.float_value))
                    : value.float_value,
                value.type);
            literal = std::make_unique<PrimaryExpression>(loc, Token(
                TokenType::FloatLiteral, loc, pool(std::move(text))));
            break;
        }
        case TypeKind::String: {
            std::string text = escape_string_literal(value.string_value);
            literal = std::make_unique<PrimaryExpression>(loc, Token(
                TokenType::StringLiteral, loc, pool(std::move(text))));
            break;
        }
        default:
            return nullptr;
        }

        expression_types_[literal.get()] = value.type;
        return literal;
    }

    std::unique_ptr<AST::Initializer> TypeChecker::make_constexpr_initializer(
        SourceLocation loc, const AST::Type& type, const ConstexprValue& value) {
        std::vector<std::unique_ptr<Initializer>> elements;

        if (type.kind == TypeKind::Struct || type.kind == TypeKind::Array) {
            for (const ConstexprValue& element : value.elements) {
                std::unique_ptr<Initializer> nested =
                    make_constexpr_initializer(loc, element.type, element);

                if (nested == nullptr) { return nullptr; }
                elements.push_back(std::move(nested));
            }
        } else {
            std::unique_ptr<Expression> literal = make_constexpr_literal(loc, value);
            if (literal == nullptr) { return nullptr; }
            return std::make_unique<ExpressionInitializer>(loc, std::move(literal));
        }

        return std::make_unique<ArrayInitializer>(loc, std::move(elements));
    }

    void TypeChecker::fold_initializer(std::unique_ptr<AST::Initializer>& holder) {
        if (holder == nullptr) { return; }

        if (auto* expression = dynamic_cast<ExpressionInitializer*>(holder.get())) {
            if (auto* call = dynamic_cast<PostfixExpression*>(expression->expr.get())) {
                auto found = constexpr_calls_.find(call);
                if (found != constexpr_calls_.end() &&
                    (found->second.type.kind == TypeKind::Struct ||
                        found->second.type.kind == TypeKind::Array)) {
                    auto replacement = make_constexpr_initializer(call->location,
                        found->second.type, found->second);
                    if (replacement != nullptr) {
                        holder = std::move(replacement);
                        return;
                    }
                }
            }

            fold_expression(expression->expr);
            return;
        }

        if (auto* array = dynamic_cast<ArrayInitializer*>(holder.get())) {
            for (auto& element : array->elements) {
                fold_initializer(element);
            }
        }
    }

    void TypeChecker::fold_expression(std::unique_ptr<AST::Expression>& holder) {
        if (holder == nullptr) { return; }

        fold_expression_tree(holder.get());

        auto* call = dynamic_cast<PostfixExpression*>(holder.get());
        if (call == nullptr ||
            call->op != PostfixExpression::Operator::FunctionCall) {
            return;
        }

        auto found = constexpr_calls_.find(call);
        if (found == constexpr_calls_.end()) { return; }
        if (found->second.type.kind == TypeKind::Struct ||
            found->second.type.kind == TypeKind::Array) {
            return;
        }

        auto literal = make_constexpr_literal(call->location, found->second);
        if (literal != nullptr) { holder = std::move(literal); }
    }
    class TypeChecker::FoldRewriter : public AST::AstRewriter {
    public:
        explicit FoldRewriter(TypeChecker& owner) : owner_(owner) {
        }

    protected:
        bool EnterPrimaryExpression(PrimaryExpression* node) override {
            switch (node->kind) {
            case PrimaryExpression::Kind::Parens:
                owner_.fold_expression(node->paren_expr);
                return false;
            case PrimaryExpression::Kind::Heap:
                owner_.fold_expression(node->heap_size);
                return false;
            case PrimaryExpression::Kind::Construct:
            case PrimaryExpression::Kind::PlacementConstruct:
                for (std::unique_ptr<Expression>& argument : node->construct_args) {
                    owner_.fold_expression(argument);
                }

                owner_.fold_expression(node->placement_target);
                return false;
            case PrimaryExpression::Kind::CopyMove:
                owner_.fold_expression(node->paren_expr);
                return false;
            default:
                return false;
            }
        }

        bool EnterCompileTimePropertyExpression(
            CompileTimePropertyExpression* node) override {
            owner_.fold_expression(node->receiver);

            for (std::unique_ptr<Expression>& argument : node->arguments) {
                owner_.fold_expression(argument);
            }

            return false;
        }

        bool EnterAssignmentExpression(AssignmentExpression* node) override {
            owner_.fold_expression(node->left);
            owner_.fold_expression(node->right);
            return false;
        }

        bool EnterLogicalOrExpression(LogicalOrExpression* node) override {
            owner_.fold_expression(node->left);
            owner_.fold_expression(node->right);
            return false;
        }

        bool EnterLogicalAndExpression(LogicalAndExpression* node) override {
            owner_.fold_expression(node->left);
            owner_.fold_expression(node->right);
            return false;
        }

        bool EnterComparisonExpression(ComparisonExpression* node) override {
            owner_.fold_expression(node->left);
            owner_.fold_expression(node->right);
            return false;
        }

        bool EnterAdditiveExpression(AdditiveExpression* node) override {
            owner_.fold_expression(node->left);
            owner_.fold_expression(node->right);
            return false;
        }

        bool EnterMultiplicativeExpression(MultiplicativeExpression* node) override {
            owner_.fold_expression(node->left);
            owner_.fold_expression(node->right);
            return false;
        }

        bool EnterPowerExpression(PowerExpression* node) override {
            owner_.fold_expression(node->left);
            owner_.fold_expression(node->right);
            return false;
        }

        bool EnterBitwiseExpression(BitwiseExpression* node) override {
            owner_.fold_expression(node->left);
            owner_.fold_expression(node->right);
            return false;
        }

        bool EnterShiftExpression(ShiftExpression* node) override {
            owner_.fold_expression(node->left);
            owner_.fold_expression(node->right);
            return false;
        }

        bool EnterConditionalExpression(ConditionalExpression* node) override {
            owner_.fold_expression(node->condition);
            owner_.fold_expression(node->then_expr);
            owner_.fold_expression(node->else_expr);
            return false;
        }

        bool EnterUnaryExpression(UnaryExpression* node) override {
            owner_.fold_expression(node->operand);
            return false;
        }

        bool EnterPostfixExpression(PostfixExpression* node) override {
            owner_.fold_expression(node->base);
            owner_.fold_expression(node->subscript_expr);

            for (std::unique_ptr<Expression>& argument : node->arguments) {
                owner_.fold_expression(argument);
            }

            return false;
        }

        bool EnterBlock(Block* node) override {
            for (std::unique_ptr<Statement>& child : node->statements) {
                owner_.fold_statement(child.get());
            }

            return false;
        }

        bool EnterVariableDeclaration(VariableDeclaration* node) override {
            owner_.fold_expression(node->array_size_expr);
            owner_.fold_initializer(node->initializer);
            return false;
        }

        bool EnterStructDefinition(StructDefinition* node) override {
            for (StructDefinition::Member& member : node->members) {
                owner_.fold_expression(member.array_size_expr);
                owner_.fold_initializer(member.initializer);
            }

            for (std::unique_ptr<SpecialMemberFunction>& special :
                node->special_members) {
                if (special == nullptr) { continue; }

                for (std::unique_ptr<Expression>& value : special->parameter_defaults) {
                    owner_.fold_expression(value);
                }

                owner_.fold_statement(special->body.get());
            }

            return false;
        }

        bool EnterFunctionDefinition(FunctionDefinition* node) override {
            if (node->is_constexpr_function) { return false; }

            for (std::unique_ptr<Expression>& value : node->param_defaults) {
                owner_.fold_expression(value);
            }

            owner_.fold_statement(node->body.get());
            return false;
        }

        bool EnterIfStatement(IfStatement* node) override {
            owner_.fold_expression(node->condition);
            owner_.fold_statement(node->then_block.get());
            owner_.fold_statement(node->else_block.get());
            return false;
        }

        bool EnterForStatement(ForStatement* node) override {
            owner_.fold_statement(node->init.get());
            owner_.fold_expression(node->condition);
            owner_.fold_expression(node->step);
            owner_.fold_statement(node->body.get());
            return false;
        }

        bool EnterWhileStatement(WhileStatement* node) override {
            owner_.fold_expression(node->condition);
            owner_.fold_statement(node->body.get());
            return false;
        }

        bool EnterReturnStatement(ReturnStatement* node) override {
            owner_.fold_expression(node->value);
            return false;
        }

        bool EnterExpressionStatement(ExpressionStatement* node) override {
            owner_.fold_expression(node->expr);
            return false;
        }

        bool EnterDestructStatement(DestructStatement* node) override {
            owner_.fold_expression(node->target);
            return false;
        }

        bool EnterEmitStatement(EmitStatement* node) override {
            for (std::unique_ptr<Expression>& piece : node->pieces) {
                owner_.fold_expression(piece);
            }

            return false;
        }

        bool EnterExternDeclaration(ExternDeclaration*) override { return false; }

        bool EnterGenericDefinition(GenericDefinition*) override { return false; }

        bool EnterInstantiationStatement(InstantiationStatement*) override {
            return false;
        }

        bool EnterNamespaceDefinition(NamespaceDefinition*) override {
            return false;
        }

        bool EnterAccessNamespaceStatement(AccessNamespaceStatement*) override {
            return false;
        }

        bool EnterAdditionNamespaceStatement(AdditionNamespaceStatement*) override {
            return false;
        }

        bool EnterCondDefinition(CondDefinition*) override { return false; }

        bool EnterUncondDefinition(UncondDefinition*) override { return false; }

        bool EnterConditionalBlock(ConditionalBlock*) override { return false; }

        bool EnterTopLevelBlock(TopLevelBlock*) override { return false; }

    private:
        TypeChecker& owner_;
    };

    void TypeChecker::fold_expression_tree(AST::Expression* expr) {
        if (expr == nullptr) { return; }

        std::unique_ptr<Expression> holder(expr);
        FoldRewriter rewriter(*this);
        rewriter.rewrite_expression(holder);
        holder.release();
    }

    void TypeChecker::fold_statement(AST::Statement* stmt) {
        if (stmt == nullptr) { return; }

        std::unique_ptr<Statement> holder(stmt);
        FoldRewriter rewriter(*this);
        rewriter.rewrite_statement(holder);
        holder.release();
    }

    void TypeChecker::fold_top_level(AST::TopLevel* node) {
        if (node == nullptr) { return; }

        std::unique_ptr<TopLevel> holder(node);
        FoldRewriter rewriter(*this);
        rewriter.rewrite_top_level(holder);
        holder.release();
    }

    void TypeChecker::apply_constexpr_folding(AST::Program& program) {
        if (constexpr_calls_.empty()) { return; }

        for (auto& top : program.top_levels) {
            fold_top_level(top.get());
        }

        for (auto& stmt : program.global_initializers) {
            fold_statement(stmt.get());
        }
    }

}
