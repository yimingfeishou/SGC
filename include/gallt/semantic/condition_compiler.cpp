#include "condition_compiler.hpp"
#include <cctype>
#include <cstdlib>

namespace gallt {
namespace {

    std::string_view strip_integer_suffix(std::string_view text) {
        if (text.size() >= 2) {
            char a = static_cast<char>(std::tolower(static_cast<unsigned char>(text[text.size() - 2])));
            char b = static_cast<char>(std::tolower(static_cast<unsigned char>(text[text.size() - 1])));
            if (a == 'l' && b == 'u') {
                return text.substr(0, text.size() - 2);
            }
        }
        if (!text.empty()) {
            char c = static_cast<char>(std::tolower(static_cast<unsigned char>(text.back())));
            if (c == 'l' || c == 'u') {
                return text.substr(0, text.size() - 1);
            }
        }
        return text;
    }

    std::optional<long long> parse_integer_text(std::string_view text) {
        text = strip_integer_suffix(text);
        if (text.empty()) {
            return std::nullopt;
        }
        std::string buffer(text);
        const char* begin = buffer.c_str();
        char* end = nullptr;
        int base = 10;
        if (buffer.size() > 2 && buffer[0] == '0' &&
            (buffer[1] == 'x' || buffer[1] == 'X')) {
            base = 16;
            begin += 2;
        }
        else if (buffer.size() > 2 && buffer[0] == '0' &&
            (buffer[1] == 'b' || buffer[1] == 'B')) {
            base = 2;
            begin += 2;
        }
        long long value = std::strtoll(begin, &end, base);
        if (end == begin || (end != nullptr && *end != '\0')) {
            return std::nullopt;
        }
        return value;
    }

    std::string unquote_literal(std::string_view lexeme) {
        if (lexeme.size() >= 2 && lexeme.front() == '"' && lexeme.back() == '"') {
            return std::string(lexeme.substr(1, lexeme.size() - 2));
        }
        return std::string(lexeme);
    }

    int hex_digit_value(char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    }

    bool is_octal_digit(char c) {
        return c >= '0' && c <= '7';
    }

    std::optional<long long> decode_char_literal(std::string_view lexeme) {
        if (lexeme.size() < 2 || lexeme.front() != '\'' || lexeme.back() != '\'') {
            return std::nullopt;
        }
        std::string_view inner = lexeme.substr(1, lexeme.size() - 2);
        if (inner.empty()) return std::nullopt;
        if (inner.front() != '\\') {
            if (inner.size() != 1) return std::nullopt;
            return static_cast<long long>(
                static_cast<unsigned char>(inner.front()));
        }
        std::string_view body = inner.substr(1);
        if (body.empty()) return std::nullopt;
        switch (body.front()) {
        case 'n': return 10;
        case 't': return 9;
        case 'r': return 13;
        case 'b': return 8;
        case 'f': return 12;
        case 'v': return 11;
        case '\\': return 92;
        case '"': return 34;
        case '\'': return 39;
        case 'x': {
            long long value = 0;
            int digits = 0;
            for (std::size_t i = 1; i < body.size() && digits < 2; ++i) {
                int d = hex_digit_value(body[i]);
                if (d < 0) break;
                value = value * 16 + d;
                ++digits;
            }
            if (digits == 0) return std::nullopt;
            return value;
        }
        default:
            break;
        }
        if (is_octal_digit(body.front())) {
            long long value = 0;
            int digits = 0;
            for (std::size_t i = 0; i < body.size() && digits < 3; ++i) {
                if (!is_octal_digit(body[i])) break;
                value = value * 8 + (body[i] - '0');
                ++digits;
            }
            if (digits == 0) return std::nullopt;
            return value;
        }
        return std::nullopt;
    }

}

    ConditionCompiler::ConditionCompiler(DiagnosticEngine& diag) : diag_(diag) {
    }

    bool ConditionCompiler::run(AST::Program* program) {
        if (program == nullptr) {
            return true;
        }
        process_top_level_list(program->top_levels);
        process_statement_list(program->global_initializers);
        return !had_error_ && !diag_.has_errors();
    }

    void ConditionCompiler::process_top_level_list(
        std::vector<std::unique_ptr<AST::TopLevel>>& nodes) {
        std::vector<std::unique_ptr<AST::TopLevel>> kept;
        kept.reserve(nodes.size());
        for (std::unique_ptr<AST::TopLevel>& node : nodes) {
            if (node == nullptr) {
                continue;
            }
            if (auto* cond = dynamic_cast<AST::CondDefinition*>(node.get())) {
                handle_condition_definition(cond);
                continue;
            }
            if (auto* uncond = dynamic_cast<AST::UncondDefinition*>(node.get())) {
                handle_condition_removal(uncond);
                continue;
            }
            if (dynamic_cast<AST::ConditionalBlock*>(node.get()) != nullptr) {
                AST::Statement* as_statement = dynamic_cast<AST::Statement*>(node.get());
                if (as_statement == nullptr) {
                    continue;
                }
                node.release();
                std::unique_ptr<AST::Statement> statement(as_statement);
                process_top_level_statement(statement, kept);
                continue;
            }
            if (auto* top_level_block = dynamic_cast<AST::TopLevelBlock*>(node.get())) {
                std::vector<std::unique_ptr<AST::TopLevel>> items =
                    std::move(top_level_block->items);
                node.reset();
                process_top_level_list(items);
                for (std::unique_ptr<AST::TopLevel>& item : items) {
                    if (item != nullptr) {
                        kept.push_back(std::move(item));
                    }
                }
                continue;
            }
            process_top_level_node(node.get());
            kept.push_back(std::move(node));
        }
        nodes = std::move(kept);
    }

    void ConditionCompiler::process_top_level_statement(
        std::unique_ptr<AST::Statement>& stmt,
        std::vector<std::unique_ptr<AST::TopLevel>>& kept) {
        if (stmt == nullptr) {
            return;
        }
        if (auto* cond = dynamic_cast<AST::CondDefinition*>(stmt.get())) {
            handle_condition_definition(cond);
            stmt.reset();
            return;
        }
        if (auto* uncond = dynamic_cast<AST::UncondDefinition*>(stmt.get())) {
            handle_condition_removal(uncond);
            stmt.reset();
            return;
        }
        if (auto* conditional = dynamic_cast<AST::ConditionalBlock*>(stmt.get())) {
            Evaluation result;
            if (!evaluate_condition(conditional->condition.get(), result)) {
                stmt.reset();
                return;
            }
            std::unique_ptr<AST::Statement>& chosen =
                result.value != 0 ? conditional->then_block : conditional->else_block;
            if (chosen == nullptr) {
                stmt.reset();
                return;
            }
            stmt = std::move(chosen);
            process_top_level_statement(stmt, kept);
            return;
        }
        if (auto* block = dynamic_cast<AST::Block*>(stmt.get())) {
            std::vector<std::unique_ptr<AST::Statement>> inner = std::move(block->statements);
            stmt.reset();
            for (std::unique_ptr<AST::Statement>& child : inner) {
                process_top_level_statement(child, kept);
            }
            return;
        }
        if (auto* top_level_block = dynamic_cast<AST::TopLevelBlock*>(stmt.get())) {
            std::vector<std::unique_ptr<AST::TopLevel>> items =
                std::move(top_level_block->items);
            stmt.reset();
            process_top_level_list(items);
            for (std::unique_ptr<AST::TopLevel>& item : items) {
                if (item != nullptr) {
                    kept.push_back(std::move(item));
                }
            }
            return;
        }
        AST::TopLevel* top = dynamic_cast<AST::TopLevel*>(stmt.get());
        if (top == nullptr) {
            stmt.reset();
            return;
        }
        process_top_level_node(top);
        stmt.release();
        kept.emplace_back(top);
    }

    void ConditionCompiler::process_top_level_node(AST::TopLevel* node) {
        if (node == nullptr) {
            return;
        }
        if (auto* ns = dynamic_cast<AST::NamespaceDefinition*>(node)) {
            process_top_level_list(ns->members);
        }
        else if (auto* addition = dynamic_cast<AST::AdditionNamespaceStatement*>(node)) {
            process_top_level_list(addition->members);
        }
        else if (auto* generic = dynamic_cast<AST::GenericDefinition*>(node)) {
            process_top_level_list(generic->members);
        }
        else if (auto* func = dynamic_cast<AST::FunctionDefinition*>(node)) {
            for (std::unique_ptr<AST::Expression>& def : func->param_defaults) {
                transform_expression(def.get());
            }
            if (func->body != nullptr) {
                process_statement(func->body);
            }
        }
        else if (auto* strct = dynamic_cast<AST::StructDefinition*>(node)) {
            for (std::unique_ptr<AST::SpecialMemberFunction>& member : strct->special_members) {
                if (member != nullptr && member->body != nullptr) {
                    process_statement(member->body);
                }
            }
        }
        else if (auto* decl = dynamic_cast<AST::VariableDeclaration*>(node)) {
            transform_initializer(decl->initializer.get());
            transform_expression(decl->array_size_expr.get());
        }
    }

    void ConditionCompiler::process_statement(std::unique_ptr<AST::Statement>& stmt) {
        if (stmt == nullptr) {
            return;
        }
        if (auto* cond = dynamic_cast<AST::CondDefinition*>(stmt.get())) {
            handle_condition_definition(cond);
            stmt.reset();
            return;
        }
        if (auto* uncond = dynamic_cast<AST::UncondDefinition*>(stmt.get())) {
            handle_condition_removal(uncond);
            stmt.reset();
            return;
        }
        if (auto* block = dynamic_cast<AST::ConditionalBlock*>(stmt.get())) {
            Evaluation result;
            if (evaluate_condition(block->condition.get(), result)) {
                if (result.value != 0) {
                    if (block->then_block != nullptr) {
                        process_statement(block->then_block);
                        stmt = std::move(block->then_block);
                    }
                    else {
                        stmt.reset();
                    }
                }
                else if (block->else_block != nullptr) {
                    process_statement(block->else_block);
                    stmt = std::move(block->else_block);
                }
                else {
                    stmt.reset();
                }
            }
            return;
        }
        if (auto* block = dynamic_cast<AST::Block*>(stmt.get())) {
            process_block(block);
            return;
        }
        if (auto* if_stmt = dynamic_cast<AST::IfStatement*>(stmt.get())) {
            resolve_condition_references(if_stmt->condition.get());
            if (if_stmt->then_block != nullptr) {
                process_statement(if_stmt->then_block);
            }
            if (if_stmt->else_block != nullptr) {
                process_statement(if_stmt->else_block);
            }
            return;
        }
        if (auto* for_stmt = dynamic_cast<AST::ForStatement*>(stmt.get())) {
            if (for_stmt->init != nullptr) {
                process_statement(for_stmt->init);
            }
            resolve_condition_references(for_stmt->condition.get());
            resolve_condition_references(for_stmt->step.get());
            if (for_stmt->body != nullptr) {
                process_statement(for_stmt->body);
            }
            return;
        }
        if (auto* while_stmt = dynamic_cast<AST::WhileStatement*>(stmt.get())) {
            resolve_condition_references(while_stmt->condition.get());
            if (while_stmt->body != nullptr) {
                process_statement(while_stmt->body);
            }
            return;
        }
        if (auto* decl = dynamic_cast<AST::VariableDeclaration*>(stmt.get())) {
            transform_initializer(decl->initializer.get());
            transform_expression(decl->array_size_expr.get());
            return;
        }
        if (auto* ret = dynamic_cast<AST::ReturnStatement*>(stmt.get())) {
            transform_expression(ret->value.get());
            return;
        }
        if (auto* expr_stmt = dynamic_cast<AST::ExpressionStatement*>(stmt.get())) {
            transform_expression(expr_stmt->expr.get());
            return;
        }
        if (auto* destruct = dynamic_cast<AST::DestructStatement*>(stmt.get())) {
            transform_expression(destruct->target.get());
            return;
        }
        if (auto* emit = dynamic_cast<AST::EmitStatement*>(stmt.get())) {
            for (std::unique_ptr<AST::Expression>& piece : emit->pieces) {
                transform_expression(piece.get());
            }
            return;
        }
        if (auto* generic = dynamic_cast<AST::GenericDefinition*>(stmt.get())) {
            process_top_level_list(generic->members);
            return;
        }
        if (auto* strct = dynamic_cast<AST::StructDefinition*>(stmt.get())) {
            for (std::unique_ptr<AST::SpecialMemberFunction>& member : strct->special_members) {
                if (member != nullptr && member->body != nullptr) {
                    process_statement(member->body);
                }
            }
            return;
        }
    }

    void ConditionCompiler::process_block(AST::Block* block) {
        process_statement_list(block->statements);
    }

    void ConditionCompiler::process_statement_list(
        std::vector<std::unique_ptr<AST::Statement>>& stmts) {
        std::vector<std::unique_ptr<AST::Statement>> kept;
        for (std::unique_ptr<AST::Statement>& stmt : stmts) {
            process_statement(stmt);
            if (stmt != nullptr) {
                kept.push_back(std::move(stmt));
            }
        }
        stmts = std::move(kept);
    }

    void ConditionCompiler::handle_condition_definition(AST::CondDefinition* node) {
        long long value = 0;
        if (node->value != nullptr) {
            const std::size_t errors_before = diag_.error_count();
            Evaluation evaluated;
            if (evaluate_condition(node->value.get(), evaluated)) {
                value = evaluated.value;
            }
            else {
                had_error_ = true;
                if (diag_.error_count() == errors_before) {
                    report(node->location, ErrorCode::CompileTimeConditionNotBoolean,
                        std::string(
                            "@cond value must be a compile-time constant expression"));
                }
            }
        }
        ConditionValue& entry = conditions_[node->name];
        entry.defined = true;
        entry.value = value;
    }

    void ConditionCompiler::handle_condition_removal(AST::UncondDefinition* node) {
        conditions_.erase(node->name);
    }

    bool ConditionCompiler::evaluate_condition(AST::Expression* expr, Evaluation& out) {
        if (expr == nullptr) {
            return false;
        }
        if (auto* prim = dynamic_cast<AST::PrimaryExpression*>(expr)) {
            switch (prim->kind) {
            case AST::PrimaryExpression::Kind::Literal:
                if (auto parsed = literal_value(expr)) {
                    out.defined = true;
                    out.value = *parsed;
                    return true;
                }
                report(expr->location, ErrorCode::ExpressionSyntaxError,
                    "condition expression requires an integer or boolean literal");
                return false;
            case AST::PrimaryExpression::Kind::Parens:
                return evaluate_condition(prim->paren_expr.get(), out);
            case AST::PrimaryExpression::Kind::Identifier:
                return evaluateNamedCondition(prim->identifier, expr->location, out);
            default:
                break;
            }
            report(expr->location, ErrorCode::ExpressionSyntaxError,
                "unsupported construct in condition expression");
            return false;
        }
        if (auto* postfix = dynamic_cast<AST::PostfixExpression*>(expr)) {
            if (postfix->op == AST::PostfixExpression::Operator::FunctionCall) {
                auto* callee = dynamic_cast<AST::PrimaryExpression*>(postfix->base.get());
                if (callee != nullptr &&
                    callee->kind == AST::PrimaryExpression::Kind::Identifier) {
                    if (callee->identifier == "is_defined") {
                        if (postfix->arguments.size() != 1) {
                            report(expr->location, ErrorCode::ExpressionSyntaxError,
                                "is_defined requires exactly one string argument");
                            return false;
                        }
                        auto* arg = dynamic_cast<AST::PrimaryExpression*>(
                            postfix->arguments[0].get());
                        if (arg == nullptr ||
                            arg->kind != AST::PrimaryExpression::Kind::Literal ||
                            arg->literal_token.type != TokenType::StringLiteral) {
                            report(expr->location, ErrorCode::ExpressionSyntaxError,
                                "is_defined requires a string literal argument");
                            return false;
                        }
                        std::string name = unquote_literal(arg->literal_token.lexeme);
                        out.defined = true;
                        out.value = conditions_.count(name) != 0 ? 1 : 0;
                        return true;
                    }
                    return evaluateNamedCondition(callee->identifier, expr->location, out);
                }
            }
            report(expr->location, ErrorCode::ExpressionSyntaxError,
                "unsupported construct in condition expression");
            return false;
        }
        if (auto* unary = dynamic_cast<AST::UnaryExpression*>(expr)) {
            Evaluation inner;
            if (!evaluate_condition(unary->operand.get(), inner)) {
                return false;
            }
            switch (unary->op) {
            case AST::UnaryExpression::Operator::LogicalNot:
                out.defined = true;
                out.value = inner.value == 0 ? 1 : 0;
                return true;
            case AST::UnaryExpression::Operator::UnaryMinus:
                out.defined = true;
                out.value = -inner.value;
                return true;
            case AST::UnaryExpression::Operator::UnaryPlus:
                out = inner;
                return true;
            default:
                break;
            }
            report(expr->location, ErrorCode::ExpressionSyntaxError,
                "unsupported unary operator in condition expression");
            return false;
        }
        if (auto* cmp = dynamic_cast<AST::ComparisonExpression*>(expr)) {
            Evaluation left;
            Evaluation right;
            if (!evaluate_condition(cmp->left.get(), left) ||
                !evaluate_condition(cmp->right.get(), right)) {
                return false;
            }
            sanitize_condition_references(cmp->left.get());
            sanitize_condition_references(cmp->right.get());
            bool result = false;
            switch (cmp->op) {
            case AST::ComparisonExpression::Operator::Equal:
                result = left.value == right.value;
                break;
            case AST::ComparisonExpression::Operator::NotEqual:
                result = left.value != right.value;
                break;
            case AST::ComparisonExpression::Operator::Greater:
                result = left.value > right.value;
                break;
            case AST::ComparisonExpression::Operator::Less:
                result = left.value < right.value;
                break;
            case AST::ComparisonExpression::Operator::GreaterEqual:
                result = left.value >= right.value;
                break;
            case AST::ComparisonExpression::Operator::LessEqual:
                result = left.value <= right.value;
                break;
            }
            out.defined = true;
            out.value = result ? 1 : 0;
            return true;
        }
        if (auto* land = dynamic_cast<AST::LogicalAndExpression*>(expr)) {
            Evaluation left;
            if (!evaluate_condition(land->left.get(), left)) {
                return false;
            }
            sanitize_condition_references(land->left.get());
            if (left.value == 0) {
                out.defined = true;
                out.value = 0;
                return true;
            }
            Evaluation right;
            if (!evaluate_condition(land->right.get(), right)) {
                return false;
            }
            sanitize_condition_references(land->right.get());
            out.defined = true;
            out.value = right.value != 0 ? 1 : 0;
            return true;
        }
        if (auto* lor = dynamic_cast<AST::LogicalOrExpression*>(expr)) {
            Evaluation left;
            if (!evaluate_condition(lor->left.get(), left)) {
                return false;
            }
            sanitize_condition_references(lor->left.get());
            if (left.value != 0) {
                out.defined = true;
                out.value = 1;
                return true;
            }
            Evaluation right;
            if (!evaluate_condition(lor->right.get(), right)) {
                return false;
            }
            sanitize_condition_references(lor->right.get());
            out.defined = true;
            out.value = right.value != 0 ? 1 : 0;
            return true;
        }
        if (auto* add = dynamic_cast<AST::AdditiveExpression*>(expr)) {
            Evaluation left;
            Evaluation right;
            if (!evaluate_condition(add->left.get(), left) ||
                !evaluate_condition(add->right.get(), right)) {
                return false;
            }
            out.defined = true;
            out.value = add->op == AST::AdditiveExpression::Operator::Plus
                ? left.value + right.value : left.value - right.value;
            return true;
        }
        if (auto* mul = dynamic_cast<AST::MultiplicativeExpression*>(expr)) {
            Evaluation left;
            Evaluation right;
            if (!evaluate_condition(mul->left.get(), left) ||
                !evaluate_condition(mul->right.get(), right)) {
                return false;
            }
            out.defined = true;
            switch (mul->op) {
            case AST::MultiplicativeExpression::Operator::Multiply:
                out.value = left.value * right.value;
                break;
            case AST::MultiplicativeExpression::Operator::Divide:
                if (right.value == 0) {
                    report(expr->location, ErrorCode::ExpressionSyntaxError,
                        "division by zero in condition expression");
                    return false;
                }
                out.value = left.value / right.value;
                break;
            case AST::MultiplicativeExpression::Operator::Remainder:
                if (right.value == 0) {
                    report(expr->location, ErrorCode::ExpressionSyntaxError,
                        "division by zero in condition expression");
                    return false;
                }
                out.value = left.value % right.value;
                break;
            }
            return true;
        }
        report(expr->location, ErrorCode::ExpressionSyntaxError,
            "unsupported construct in condition expression");
        return false;
    }

    bool ConditionCompiler::evaluateNamedCondition(const std::string& name,
        SourceLocation loc, Evaluation& out) {
        if (name == "true") {
            out.defined = true;
            out.value = 1;
            return true;
        }
        if (name == "false") {
            out.defined = true;
            out.value = 0;
            return true;
        }
        auto it = conditions_.find(name);
        if (it == conditions_.end()) {
            report(loc, ErrorCode::UndefinedIdentifier,
                "condition '" + name + "' is not defined");
            return false;
        }
        out.defined = true;
        out.value = it->second.value;
        return true;
    }

    std::optional<long long> ConditionCompiler::literal_value(
        const AST::Expression* expr) const {
        auto* prim = dynamic_cast<const AST::PrimaryExpression*>(expr);
        if (prim == nullptr || prim->kind != AST::PrimaryExpression::Kind::Literal) {
            return std::nullopt;
        }
        switch (prim->literal_token.type) {
        case TokenType::IntegerLiteral:
            return parse_integer_text(prim->literal_token.lexeme);
        case TokenType::CharLiteral:
            return decode_char_literal(prim->literal_token.lexeme);
        case TokenType::BoolLiteral:
            return prim->literal_token.lexeme == "true" ? 1 : 0;
        default:
            return std::nullopt;
        }
    }

    bool ConditionCompiler::is_boolean_expression(const AST::Expression* expr) const {
        if (expr == nullptr) {
            return false;
        }
        if (dynamic_cast<const AST::ComparisonExpression*>(expr) != nullptr ||
            dynamic_cast<const AST::LogicalAndExpression*>(expr) != nullptr ||
            dynamic_cast<const AST::LogicalOrExpression*>(expr) != nullptr) {
            return true;
        }
        if (auto* unary = dynamic_cast<const AST::UnaryExpression*>(expr)) {
            return unary->op == AST::UnaryExpression::Operator::LogicalNot;
        }
        return false;
    }

    bool ConditionCompiler::is_condition_name(const AST::Expression* expr) const {
        auto* prim = dynamic_cast<const AST::PrimaryExpression*>(expr);
        if (prim == nullptr || prim->kind != AST::PrimaryExpression::Kind::Identifier) {
            return false;
        }
        return conditions_.count(prim->identifier) != 0 ||
            prim->identifier == "true" || prim->identifier == "false";
    }

    void ConditionCompiler::sanitize_condition_references(AST::Expression* expr) {
        if (expr == nullptr) {
            return;
        }
        if (auto* prim = dynamic_cast<AST::PrimaryExpression*>(expr)) {
            if (prim->kind == AST::PrimaryExpression::Kind::Identifier) {
                auto it = conditions_.find(prim->identifier);
                if (it != conditions_.end()) {
                    std::unique_ptr<AST::Expression> replacement =
                        make_integer_literal(expr->location, it->second.value);
                    if (auto* repl =
                        dynamic_cast<AST::PrimaryExpression*>(replacement.get())) {
                        prim->kind = AST::PrimaryExpression::Kind::Literal;
                        prim->literal_token = repl->literal_token;
                    }
                    return;
                }
            }
            sanitize_condition_references(prim->paren_expr.get());
            for (std::unique_ptr<AST::Expression>& arg : prim->construct_args) {
                sanitize_condition_references(arg.get());
            }
            return;
        }
        if (auto* postfix = dynamic_cast<AST::PostfixExpression*>(expr)) {
            sanitize_condition_references(postfix->base.get());
            sanitize_condition_references(postfix->subscript_expr.get());
            for (std::unique_ptr<AST::Expression>& arg : postfix->arguments) {
                sanitize_condition_references(arg.get());
            }
            return;
        }
        if (auto* assign = dynamic_cast<AST::AssignmentExpression*>(expr)) {
            sanitize_condition_references(assign->left.get());
            sanitize_condition_references(assign->right.get());
            return;
        }
        if (auto* lor = dynamic_cast<AST::LogicalOrExpression*>(expr)) {
            sanitize_condition_references(lor->left.get());
            sanitize_condition_references(lor->right.get());
            return;
        }
        if (auto* land = dynamic_cast<AST::LogicalAndExpression*>(expr)) {
            sanitize_condition_references(land->left.get());
            sanitize_condition_references(land->right.get());
            return;
        }
        if (auto* cmp = dynamic_cast<AST::ComparisonExpression*>(expr)) {
            sanitize_condition_references(cmp->left.get());
            sanitize_condition_references(cmp->right.get());
            return;
        }
        if (auto* add = dynamic_cast<AST::AdditiveExpression*>(expr)) {
            sanitize_condition_references(add->left.get());
            sanitize_condition_references(add->right.get());
            return;
        }
        if (auto* mul = dynamic_cast<AST::MultiplicativeExpression*>(expr)) {
            sanitize_condition_references(mul->left.get());
            sanitize_condition_references(mul->right.get());
            return;
        }
        if (auto* power = dynamic_cast<AST::PowerExpression*>(expr)) {
            sanitize_condition_references(power->left.get());
            sanitize_condition_references(power->right.get());
            return;
        }
        if (auto* unary = dynamic_cast<AST::UnaryExpression*>(expr)) {
            sanitize_condition_references(unary->operand.get());
            return;
        }
        if (auto* prop = dynamic_cast<AST::CompileTimePropertyExpression*>(expr)) {
            sanitize_condition_references(prop->receiver.get());
            for (std::unique_ptr<AST::Expression>& arg : prop->arguments) {
                sanitize_condition_references(arg.get());
            }
            return;
        }
    }

    void ConditionCompiler::resolve_condition_references(AST::Expression* expr) {
        if (expr == nullptr) {
            return;
        }
        if (auto* prim = dynamic_cast<AST::PrimaryExpression*>(expr)) {
            if (prim->kind != AST::PrimaryExpression::Kind::Identifier) {
                return;
            }
        }
        if (!is_condition_name(expr)) {
            return;
        }
        auto* prim = dynamic_cast<AST::PrimaryExpression*>(expr);
        report(expr->location, ErrorCode::ConditionUsedAsValue,
            std::string(prim->identifier));
    }

    void ConditionCompiler::transform_expression(AST::Expression* expr) {
        if (expr == nullptr) {
            return;
        }
        if (auto* postfix = dynamic_cast<AST::PostfixExpression*>(expr)) {
            transform_expression(postfix->base.get());
            transform_expression(postfix->subscript_expr.get());
            for (std::unique_ptr<AST::Expression>& arg : postfix->arguments) {
                transform_expression(arg.get());
            }
            if (postfix->op == AST::PostfixExpression::Operator::FunctionCall) {
                auto* callee = dynamic_cast<AST::PrimaryExpression*>(postfix->base.get());
                if (callee != nullptr &&
                    callee->kind == AST::PrimaryExpression::Kind::Identifier &&
                    callee->identifier == "is_defined") {
                    if (postfix->arguments.size() != 1) {
                        report(expr->location, ErrorCode::ExpressionSyntaxError,
                            "is_defined requires exactly one string argument");
                        return;
                    }
                    auto* arg = dynamic_cast<AST::PrimaryExpression*>(
                        postfix->arguments[0].get());
                    if (arg == nullptr ||
                        arg->kind != AST::PrimaryExpression::Kind::Literal ||
                        arg->literal_token.type != TokenType::StringLiteral) {
                        report(expr->location, ErrorCode::ConditionUsedAsValue,
                            std::string("<non-literal>"));
                        return;
                    }
                    std::string name = unquote_literal(arg->literal_token.lexeme);
                    bool defined = conditions_.count(name) != 0;
                    callee->kind = AST::PrimaryExpression::Kind::Literal;
                    callee->literal_token = make_bool_token(expr->location, defined);
                    postfix->subscript_expr = nullptr;
                    postfix->arguments.clear();
                    return;
                }
            }
            resolve_condition_references(expr);
            return;
        }
        if (auto* assign = dynamic_cast<AST::AssignmentExpression*>(expr)) {
            transform_expression(assign->left.get());
            transform_expression(assign->right.get());
            resolve_condition_references(expr);
            return;
        }
        if (auto* prim = dynamic_cast<AST::PrimaryExpression*>(expr)) {
            if (prim->kind == AST::PrimaryExpression::Kind::Identifier) {
                resolve_condition_references(expr);
                return;
            }
            transform_expression(prim->paren_expr.get());
            transform_expression(prim->heap_size.get());
            transform_expression(prim->placement_target.get());
            for (std::unique_ptr<AST::Expression>& arg : prim->construct_args) {
                transform_expression(arg.get());
            }
            return;
        }
        resolve_condition_references(expr);
        if (auto* lor = dynamic_cast<AST::LogicalOrExpression*>(expr)) {
            transform_expression(lor->left.get());
            transform_expression(lor->right.get());
            return;
        }
        if (auto* land = dynamic_cast<AST::LogicalAndExpression*>(expr)) {
            transform_expression(land->left.get());
            transform_expression(land->right.get());
            return;
        }
        if (auto* cmp = dynamic_cast<AST::ComparisonExpression*>(expr)) {
            transform_expression(cmp->left.get());
            transform_expression(cmp->right.get());
            return;
        }
        if (auto* add = dynamic_cast<AST::AdditiveExpression*>(expr)) {
            transform_expression(add->left.get());
            transform_expression(add->right.get());
            return;
        }
        if (auto* mul = dynamic_cast<AST::MultiplicativeExpression*>(expr)) {
            transform_expression(mul->left.get());
            transform_expression(mul->right.get());
            return;
        }
        if (auto* power = dynamic_cast<AST::PowerExpression*>(expr)) {
            transform_expression(power->left.get());
            transform_expression(power->right.get());
            return;
        }
        if (auto* unary = dynamic_cast<AST::UnaryExpression*>(expr)) {
            transform_expression(unary->operand.get());
            return;
        }
        if (auto* prop = dynamic_cast<AST::CompileTimePropertyExpression*>(expr)) {
            transform_expression(prop->receiver.get());
            for (std::unique_ptr<AST::Expression>& arg : prop->arguments) {
                transform_expression(arg.get());
            }
            return;
        }
    }

    void ConditionCompiler::transform_initializer(AST::Initializer* init) {
        if (init == nullptr) {
            return;
        }
        if (auto* expr_init = dynamic_cast<AST::ExpressionInitializer*>(init)) {
            transform_expression(expr_init->expr.get());
            return;
        }
        if (auto* array_init = dynamic_cast<AST::ArrayInitializer*>(init)) {
            for (std::unique_ptr<AST::Initializer>& element : array_init->elements) {
                transform_initializer(element.get());
            }
        }
    }

    std::unique_ptr<AST::Expression> ConditionCompiler::make_integer_literal(
        SourceLocation loc, long long value) {
        lexeme_pool_.push_back(std::to_string(value));
        Token token(TokenType::IntegerLiteral, loc,
            std::string_view(lexeme_pool_.back()));
        return std::make_unique<AST::PrimaryExpression>(loc, token);
    }

    Token ConditionCompiler::make_bool_token(SourceLocation loc, bool value) {
        lexeme_pool_.push_back(value ? "true" : "false");
        return Token(TokenType::BoolLiteral, loc,
            std::string_view(lexeme_pool_.back()));
    }

    void ConditionCompiler::report(SourceLocation loc, ErrorCode code,
        const std::vector<std::string>& values) {
        diag_.report_error_template(loc, code, values);
        had_error_ = true;
    }

    void ConditionCompiler::report(SourceLocation loc, ErrorCode code,
        const std::string& message) {
        diag_.report_error(loc, code, message);
        had_error_ = true;
    }

}
