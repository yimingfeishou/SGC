#include "generic_expander.hpp"
#include "generic_expander_detail.hpp"
#include "constant_folding.hpp"
#include "../lexer/lexer.hpp"
#include "../parser/parser.hpp"
#include <algorithm>
#include <cstdlib>
#include <cstdio>
#include <functional>
#include <map>
#include <set>

using namespace gallt::AST;

namespace gallt {
    using namespace generic_expander_detail;

    bool GenericExpander::statement_is_allowed_in_expression_body(const AST::Statement* stmt,
        SourceLocation& bad_loc, ErrorCode& code, std::string& detail) {
        if (stmt == nullptr) {
            return true;
        }
        if (dynamic_cast<const StructDefinition*>(stmt) != nullptr) {
            bad_loc = stmt->location;
            code = ErrorCode::ExprParameterBlockDisallowedDeclaration;
            detail = "struct";
            return false;
        }
        if (dynamic_cast<const GenericDefinition*>(stmt) != nullptr) {
            bad_loc = stmt->location;
            code = ErrorCode::ExprParameterBlockDisallowedDeclaration;
            detail = "generic";
            return false;
        }
        if (dynamic_cast<const NamespaceDefinition*>(stmt) != nullptr ||
            dynamic_cast<const AdditionNamespaceStatement*>(stmt) != nullptr) {
            bad_loc = stmt->location;
            code = ErrorCode::ExprParameterBlockDisallowedDeclaration;
            detail = "namespace";
            return false;
        }
        if (dynamic_cast<const EmitStatement*>(stmt) != nullptr) {
            bad_loc = stmt->location;
            code = ErrorCode::ExprParameterBlockDisallowedConstruct;
            detail = "emit";
            return false;
        }
        if (dynamic_cast<const GuideStatement*>(stmt) != nullptr) {
            bad_loc = stmt->location;
            code = ErrorCode::ExprParameterBlockDisallowedConstruct;
            detail = "guide";
            return false;
        }
        if (dynamic_cast<const ClibStatement*>(stmt) != nullptr) {
            bad_loc = stmt->location;
            code = ErrorCode::ExprParameterBlockDisallowedConstruct;
            detail = "clib";
            return false;
        }
        if (dynamic_cast<const ExternDeclaration*>(stmt) != nullptr) {
            bad_loc = stmt->location;
            code = ErrorCode::ExprParameterBlockDisallowedConstruct;
            detail = "extern";
            return false;
        }
        if (dynamic_cast<const CondDefinition*>(stmt) != nullptr ||
            dynamic_cast<const UncondDefinition*>(stmt) != nullptr ||
            dynamic_cast<const ConditionalBlock*>(stmt) != nullptr ||
            dynamic_cast<const TopLevelBlock*>(stmt) != nullptr) {
            bad_loc = stmt->location;
            code = ErrorCode::ExprParameterBlockDisallowedConstruct;
            detail = "condition";
            return false;
        }
        if (auto* block = dynamic_cast<const Block*>(stmt)) {
            for (const auto& inner : block->statements) {
                if (!statement_is_allowed_in_expression_body(inner.get(), bad_loc, code,
                    detail)) {
                    return false;
                }
            }
            return true;
        }
        if (auto* if_stmt = dynamic_cast<const IfStatement*>(stmt)) {
            if (!statement_is_allowed_in_expression_body(if_stmt->then_block.get(), bad_loc,
                code, detail)) {
                return false;
            }
            return statement_is_allowed_in_expression_body(if_stmt->else_block.get(),
                bad_loc, code, detail);
        }
        if (auto* for_stmt = dynamic_cast<const ForStatement*>(stmt)) {
            return statement_is_allowed_in_expression_body(for_stmt->body.get(), bad_loc,
                code, detail);
        }
        if (auto* while_stmt = dynamic_cast<const WhileStatement*>(stmt)) {
            return statement_is_allowed_in_expression_body(while_stmt->body.get(), bad_loc,
                code, detail);
        }
        return true;
    }

    void GenericExpander::collect_declared_names(const AST::Statement* stmt,
        std::unordered_set<std::string>& out) const {
        if (stmt == nullptr) {
            return;
        }
        if (auto* decl = dynamic_cast<const VariableDeclaration*>(stmt)) {
            out.insert(decl->name);
            return;
        }
        if (auto* block = dynamic_cast<const Block*>(stmt)) {
            for (const auto& inner : block->statements) {
                collect_declared_names(inner.get(), out);
            }
            return;
        }
        if (auto* if_stmt = dynamic_cast<const IfStatement*>(stmt)) {
            collect_declared_names(if_stmt->then_block.get(), out);
            collect_declared_names(if_stmt->else_block.get(), out);
            return;
        }
        if (auto* for_stmt = dynamic_cast<const ForStatement*>(stmt)) {
            collect_declared_names(for_stmt->init.get(), out);
            collect_declared_names(for_stmt->body.get(), out);
            return;
        }
        if (auto* while_stmt = dynamic_cast<const WhileStatement*>(stmt)) {
            collect_declared_names(while_stmt->body.get(), out);
            return;
        }
    }

    void GenericExpander::collect_free_identifiers(const Expression* expr,
        const std::unordered_set<std::string>& bound,
        std::unordered_set<std::string>& out) const {
        if (expr == nullptr) {
            return;
        }
        if (auto* prim = dynamic_cast<const PrimaryExpression*>(expr)) {
            if (prim->kind == PrimaryExpression::Kind::Identifier) {
                if (bound.count(prim->identifier) == 0) {
                    out.insert(prim->identifier);
                }
                return;
            }
            collect_free_identifiers(prim->paren_expr.get(), bound, out);
            collect_free_identifiers(prim->heap_size.get(), bound, out);
            collect_free_identifiers(prim->placement_target.get(), bound, out);
            for (const auto& arg : prim->construct_args) {
                collect_free_identifiers(arg.get(), bound, out);
            }
            return;
        }
        if (auto* post = dynamic_cast<const PostfixExpression*>(expr)) {
            collect_free_identifiers(post->base.get(), bound, out);
            collect_free_identifiers(post->subscript_expr.get(), bound, out);
            for (const auto& arg : post->arguments) {
                collect_free_identifiers(arg.get(), bound, out);
            }
            return;
        }
        if (auto* e = dynamic_cast<const AssignmentExpression*>(expr)) {
            collect_free_identifiers(e->left.get(), bound, out);
            collect_free_identifiers(e->right.get(), bound, out);
            return;
        }
        if (auto* e = dynamic_cast<const LogicalOrExpression*>(expr)) {
            collect_free_identifiers(e->left.get(), bound, out);
            collect_free_identifiers(e->right.get(), bound, out);
            return;
        }
        if (auto* e = dynamic_cast<const LogicalAndExpression*>(expr)) {
            collect_free_identifiers(e->left.get(), bound, out);
            collect_free_identifiers(e->right.get(), bound, out);
            return;
        }
        if (auto* e = dynamic_cast<const ComparisonExpression*>(expr)) {
            collect_free_identifiers(e->left.get(), bound, out);
            collect_free_identifiers(e->right.get(), bound, out);
            return;
        }
        if (auto* e = dynamic_cast<const AdditiveExpression*>(expr)) {
            collect_free_identifiers(e->left.get(), bound, out);
            collect_free_identifiers(e->right.get(), bound, out);
            return;
        }
        if (auto* e = dynamic_cast<const MultiplicativeExpression*>(expr)) {
            collect_free_identifiers(e->left.get(), bound, out);
            collect_free_identifiers(e->right.get(), bound, out);
            return;
        }
        if (auto* e = dynamic_cast<const PowerExpression*>(expr)) {
            collect_free_identifiers(e->left.get(), bound, out);
            collect_free_identifiers(e->right.get(), bound, out);
            return;
        }
        if (auto* e = dynamic_cast<const BitwiseExpression*>(expr)) {
            collect_free_identifiers(e->left.get(), bound, out);
            collect_free_identifiers(e->right.get(), bound, out);
            return;
        }
        if (auto* e = dynamic_cast<const ShiftExpression*>(expr)) {
            collect_free_identifiers(e->left.get(), bound, out);
            collect_free_identifiers(e->right.get(), bound, out);
            return;
        }
        if (auto* e = dynamic_cast<const ConditionalExpression*>(expr)) {
            collect_free_identifiers(e->condition.get(), bound, out);
            collect_free_identifiers(e->then_expr.get(), bound, out);
            collect_free_identifiers(e->else_expr.get(), bound, out);
            return;
        }
        if (auto* e = dynamic_cast<const UnaryExpression*>(expr)) {
            collect_free_identifiers(e->operand.get(), bound, out);
            return;
        }
    }

    void GenericExpander::collect_free_identifiers_in_statement(const Statement* stmt,
        std::unordered_set<std::string>& out) const {
        if (stmt == nullptr) {
            return;
        }
        std::unordered_set<std::string> bound;
        if (auto* decl = dynamic_cast<const VariableDeclaration*>(stmt)) {
            bound.insert(decl->name);
            if (decl->initializer != nullptr && decl->initializer->is_expression()) {
                collect_free_identifiers(
                    static_cast<const ExpressionInitializer*>(decl->initializer.get())
                        ->expr.get(),
                    bound, out);
            }
            collect_free_identifiers(decl->array_size_expr.get(), bound, out);
            return;
        }
        if (auto* ret = dynamic_cast<const ReturnStatement*>(stmt)) {
            collect_free_identifiers(ret->value.get(), bound, out);
            return;
        }
        if (auto* expr_stmt = dynamic_cast<const ExpressionStatement*>(stmt)) {
            collect_free_identifiers(expr_stmt->expr.get(), bound, out);
            return;
        }
        if (auto* block = dynamic_cast<const Block*>(stmt)) {
            for (const auto& inner : block->statements) {
                collect_free_identifiers_in_statement(inner.get(), out);
            }
            return;
        }
        if (auto* if_stmt = dynamic_cast<const IfStatement*>(stmt)) {
            collect_free_identifiers(if_stmt->condition.get(), bound, out);
            collect_free_identifiers_in_statement(if_stmt->then_block.get(), out);
            collect_free_identifiers_in_statement(if_stmt->else_block.get(), out);
            return;
        }
        if (auto* for_stmt = dynamic_cast<const ForStatement*>(stmt)) {
            collect_free_identifiers(for_stmt->condition.get(), bound, out);
            collect_free_identifiers(for_stmt->step.get(), bound, out);
            collect_free_identifiers_in_statement(for_stmt->init.get(), out);
            collect_free_identifiers_in_statement(for_stmt->body.get(), out);
            return;
        }
        if (auto* while_stmt = dynamic_cast<const WhileStatement*>(stmt)) {
            collect_free_identifiers(while_stmt->condition.get(), bound, out);
            collect_free_identifiers_in_statement(while_stmt->body.get(), out);
            return;
        }
    }

    bool GenericExpander::validate_expression_body(const GenericParameter& param,
        const Substitution::ExpressionBinding& binding, SourceLocation loc) {
        if (binding.body == nullptr || binding.body->statements.empty()) {
            report(loc, ErrorCode::ExprParameterBlockMissingReturn,
                std::vector<std::string>{ param.name });
            return false;
        }
        SourceLocation bad_loc;
        ErrorCode bad_code = ErrorCode::ExprParameterDisallowedSyntax;
        std::string detail;
        for (const auto& stmt : binding.body->statements) {
            if (!statement_is_allowed_in_expression_body(stmt.get(), bad_loc, bad_code,
                detail)) {
                report(bad_loc, bad_code, std::vector<std::string>{ detail });
                return false;
            }
        }
        const Statement* last = binding.body->statements.back().get();
        while (auto* block = dynamic_cast<const Block*>(last)) {
            if (block->statements.empty()) {
                break;
            }
            last = block->statements.back().get();
        }
        const bool void_return = binding.return_type.kind == TypeKind::Void;
        if (!void_return && dynamic_cast<const ReturnStatement*>(last) == nullptr) {
            report(loc, ErrorCode::ExprParameterBlockMissingReturn,
                std::vector<std::string>{ param.name });
            return false;
        }
        if (void_return && dynamic_cast<const ReturnStatement*>(last) == nullptr &&
            dynamic_cast<const ExpressionStatement*>(last) == nullptr) {
            report(loc, ErrorCode::ExprParameterBlockMissingReturn,
                std::vector<std::string>{ param.name });
            return false;
        }
        std::unordered_set<std::string> declared;
        for (const auto& stmt : binding.body->statements) {
            collect_declared_names(stmt.get(), declared);
        }
        for (const std::string& local : declared) {
            for (const std::string& pname : binding.parameter_names) {
                if (!pname.empty() && pname == local) {
                    report(loc, ErrorCode::ExprParameterBlockLocalConflictsParameter,
                        std::vector<std::string>{ local, pname });
                    return false;
                }
            }
        }
        for (const auto& stmt : binding.body->statements) {
            if (auto* decl = dynamic_cast<const VariableDeclaration*>(stmt.get())) {
                if (decl->type.kind == TypeKind::Void) {
                    report(decl->location, ErrorCode::ExprParameterDisallowedSyntax,
                        std::vector<std::string>{});
                    return false;
                }
            }
        }
        return true;
    }

    bool GenericExpander::decode_string_literal(std::string_view lexeme,
        std::string& out) const {
        std::string_view content = lexeme;
        if (content.size() >= 2 && content.front() == '"' && content.back() == '"') {
            content = content.substr(1, content.size() - 2);
        }
        out.clear();
        for (std::size_t i = 0; i < content.size(); ++i) {
            char c = content[i];
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (++i >= content.size()) break;
            char e = content[i];
            switch (e) {
            case 'n': out.push_back('\n'); break;
            case 't': out.push_back('\t'); break;
            case 'r': out.push_back('\r'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'v': out.push_back('\v'); break;
            case '0': out.push_back('\0'); break;
            case '\\': out.push_back('\\'); break;
            case '"': out.push_back('"'); break;
            case '\'': out.push_back('\''); break;
            case 'x': {
                int value = 0;
                int digits = 0;
                while (i + 1 < content.size() && digits < 2) {
                    char h = content[i + 1];
                    int d = (h >= '0' && h <= '9') ? (h - '0')
                        : (h >= 'a' && h <= 'f') ? (h - 'a' + 10)
                        : (h >= 'A' && h <= 'F') ? (h - 'A' + 10) : -1;
                    if (d < 0) break;
                    ++i;
                    value = value * 16 + d;
                    ++digits;
                }
                out.push_back(static_cast<char>(value));
                break;
            }
            default:
                if (e >= '0' && e <= '7') {
                    int value = e - '0';
                    int digits = 1;
                    while (i + 1 < content.size() && digits < 3 &&
                        content[i + 1] >= '0' && content[i + 1] <= '7') {
                        ++i;
                        value = value * 8 + (content[i] - '0');
                        ++digits;
                    }
                    out.push_back(static_cast<char>(value));
                }
                else {
                    out.push_back(e);
                }
                break;
            }
        }
        return true;
    }

    std::string GenericExpander::expression_text(const Expression* expr) const {
        if (expr == nullptr) return "<empty>";
        if (auto* e = dynamic_cast<const PrimaryExpression*>(expr)) {
            switch (e->kind) {
            case PrimaryExpression::Kind::Literal:
                return std::string(e->literal_token.lexeme);
            case PrimaryExpression::Kind::Identifier:
                return e->identifier;
            default:
                return "<expression>";
            }
        }
        if (auto* e = dynamic_cast<const PostfixExpression*>(expr)) {
            if (e->op == PostfixExpression::Operator::Dot) {
                return expression_text(e->base.get()) + "." + e->member_name;
            }
            if (e->op == PostfixExpression::Operator::FunctionCall) {
                return expression_text(e->base.get()) + "(...)";
            }
            return "<expression>";
        }
        if (auto* e = dynamic_cast<const CompileTimePropertyExpression*>(expr)) {
            return expression_text(e->receiver.get()) + "." + e->property + "<...>";
        }
        if (auto* e = dynamic_cast<const AdditiveExpression*>(expr)) {
            return expression_text(e->left.get()) + " + " + expression_text(e->right.get());
        }
        if (auto* e = dynamic_cast<const ComparisonExpression*>(expr)) {
            const char* op = "==";
            switch (e->op) {
            case ComparisonExpression::Operator::Greater: op = ">"; break;
            case ComparisonExpression::Operator::Less: op = "<"; break;
            case ComparisonExpression::Operator::Equal: op = "=="; break;
            case ComparisonExpression::Operator::NotEqual: op = "!="; break;
            case ComparisonExpression::Operator::GreaterEqual: op = ">="; break;
            case ComparisonExpression::Operator::LessEqual: op = "<="; break;
            }
            return expression_text(e->left.get()) + " " + op + " " +
                expression_text(e->right.get());
        }
        if (auto* e = dynamic_cast<const LogicalAndExpression*>(expr)) {
            return expression_text(e->left.get()) + " && " +
                expression_text(e->right.get());
        }
        if (auto* e = dynamic_cast<const LogicalOrExpression*>(expr)) {
            return expression_text(e->left.get()) + " || " +
                expression_text(e->right.get());
        }
        if (auto* e = dynamic_cast<const BitwiseExpression*>(expr)) {
            const char* op = "&";
            switch (e->op) {
            case BitwiseExpression::Operator::And: op = "&"; break;
            case BitwiseExpression::Operator::Xor: op = "^"; break;
            case BitwiseExpression::Operator::Or: op = "|"; break;
            }
            return expression_text(e->left.get()) + " " + op + " " +
                expression_text(e->right.get());
        }
        if (auto* e = dynamic_cast<const ShiftExpression*>(expr)) {
            const char* op = e->op == ShiftExpression::Operator::Left ? "<<" : ">>";
            return expression_text(e->left.get()) + " " + op + " " +
                expression_text(e->right.get());
        }
        if (auto* e = dynamic_cast<const ConditionalExpression*>(expr)) {
            return expression_text(e->condition.get()) + " ? " +
                expression_text(e->then_expr.get()) + " : " +
                expression_text(e->else_expr.get());
        }
        return "<expression>";
    }

    std::unique_ptr<Expression> GenericExpander::rewrite_property_expression(
        const PostfixExpression* expr, const Substitution& sub) {
        if (expr == nullptr || expr->op != PostfixExpression::Operator::Dot) return nullptr;
        auto* prim = dynamic_cast<const PrimaryExpression*>(expr->base.get());
        if (prim == nullptr || prim->kind != PrimaryExpression::Kind::Identifier) {
            return nullptr;
        }
        const std::string& parameter = prim->identifier;
        bool is_type_param = (sub.types.find(parameter) != sub.types.end());
        bool is_constant_param =
            (sub.constant_types.find(parameter) != sub.constant_types.end());
        if (!is_type_param && !is_constant_param) return nullptr;

        if (expr->member_name == "size" || expr->member_name == "align") {
            long long value = 0;
            if (!eval_size_align_property(parameter, expr->member_name, sub, value)) {
                report(expr->location, ErrorCode::CompileTimePropertyNotApplicable,
                    std::vector<std::string>{ expr->member_name, parameter });
                value = 0;
            }
            return make_constant_literal(expr->location,
                ConstantValue{ value, 0.0, false, false, std::string() });
        }
        if (expr->member_name == "typename") {
            if (!is_type_param) {
                report(expr->location, ErrorCode::CompileTimePropertyNotApplicable, std::vector<std::string>{ expr->member_name, parameter });
                return make_constant_literal(expr->location,
                    ConstantValue{ 0LL, 0.0, false, false, std::string() });
            }
            ConstantValue value;
            value.is_string = true;
            value.string_value = sub.types.at(parameter).to_string();
            return make_constant_literal(expr->location, value);
        }
        if (expr->member_name.rfind("is_", 0) == 0) {
            bool value = false;
            if (eval_bool_property(expr->base.get(), expr->member_name, {}, sub, value)) {
                return make_constant_literal(expr->location,
                    ConstantValue{ value ? 1LL : 0LL, 0.0, false, false, std::string() });
            }
            report(expr->location, ErrorCode::CompileTimePropertyNotApplicable, std::vector<std::string>{ expr->member_name, parameter });
            return make_constant_literal(expr->location,
                ConstantValue{ 0LL, 0.0, false, false, std::string() });
        }
        return nullptr;
    }

    bool GenericExpander::param_type_of(const std::string& name,
        const Substitution& sub, AST::Type& out) const {
        auto type_param = sub.types.find(name);
        if (type_param != sub.types.end()) {
            out = type_param->second;
            return true;
        }
        auto constant_param = sub.constant_types.find(name);
        if (constant_param != sub.constant_types.end()) {
            out = constant_param->second;
            return true;
        }
        return false;
    }

    bool GenericExpander::base_type_of(const std::string& name, const Substitution& sub,
        AST::Type& out) const {
        if (param_type_of(name, sub, out)) return true;
        if (name == "int") { out = AST::Type::make_int(); return true; }
        if (name == "lint") { out = AST::Type::make_lint(); return true; }
        if (name == "uint") { out = AST::Type::make_uint(); return true; }
        if (name == "luint") { out = AST::Type::make_luint(); return true; }
        if (name == "float") { out = AST::Type::make_float(); return true; }
        if (name == "double") { out = AST::Type::make_double(); return true; }
        if (name == "char") { out = AST::Type::make_char(); return true; }
        if (name == "uchar") { out = AST::Type::make_uchar(); return true; }
        if (name == "bool") { out = AST::Type::make_bool(); return true; }
        if (name == "string") { out = AST::Type::make_string(); return true; }
        if (name == "file") { out = AST::Type::make_file(); return true; }
        if (name == "void") { out = AST::Type::make_void(); return true; }
        if (struct_defs_.count(name) != 0 || instantiated_structs_.count(name) != 0) {
            out = AST::Type::make_struct(name);
            return true;
        }
        return false;
    }

    bool GenericExpander::property_argument_type(const std::string& text,
        const Substitution& sub, AST::Type& out) const {
        std::string base = text;
        int pointers = 0;
        bool array = false;
        for (;;) {
            if (base.size() >= 2 && base.compare(base.size() - 2, 2, "**") == 0) {
                pointers += 2;
                base.resize(base.size() - 2);
                continue;
            }
            if (!base.empty() && base.back() == '*') {
                ++pointers;
                base.pop_back();
                continue;
            }
            if (base.size() >= 2 && base.compare(base.size() - 2, 2, "[]") == 0) {
                array = true;
                base.resize(base.size() - 2);
                continue;
            }
            break;
        }
        AST::Type type;
        if (!base_type_of(base, sub, type)) return false;
        if (array) {
            type = AST::Type::make_array(std::make_shared<AST::Type>(type), std::nullopt);
        }
        for (int i = 0; i < pointers; ++i) {
            type = AST::Type::make_pointer(std::make_shared<AST::Type>(type));
        }
        out = type;
        return true;
    }

    bool GenericExpander::property_convertible(const AST::Type& from,
        const AST::Type& to) const {
        if (from == to) return true;
        auto arithmetic_rank = [](const AST::Type& t) -> int {
            switch (t.kind) {
            case TypeKind::Bool: return 0;
            case TypeKind::Char: return 0;
            case TypeKind::Uchar: return 1;
            case TypeKind::Int: return 2;
            case TypeKind::Uint: return 3;
            case TypeKind::Lint: return 4;
            case TypeKind::Luint: return 5;
            case TypeKind::Float: return 6;
            case TypeKind::Double: return 7;
            default: return -1;
            }
        };
        if (arithmetic_rank(from) >= 0 && arithmetic_rank(to) >= 0) return true;
        if (to.kind == TypeKind::Bool && from.is_integer()) return true;
        if (from.kind == TypeKind::Pointer && to.kind == TypeKind::Pointer) {
            if (from.pointee_type && from.pointee_type->kind == TypeKind::Void) {
                return true;
            }
            if (to.pointee_type && to.pointee_type->kind == TypeKind::Void) {
                return true;
            }
            return from == to;
        }
        if (from.kind == TypeKind::Array && to.kind == TypeKind::Pointer) {
            if (!from.element_type || !to.pointee_type) return false;
            if (to.pointee_type->kind == TypeKind::Void) return true;
            return *from.element_type == *to.pointee_type;
        }
        return false;
    }

    bool GenericExpander::eval_size_align_property(const std::string& parameter,
        const std::string& property, const Substitution& sub, long long& out) {
        bool want_align = (property == "align");
        AST::Type type;
        if (!param_type_of(parameter, sub, type)) return false;
        std::size_t size = 0;
        std::size_t align = 0;
        if (!layout_of_composite_type(type, size, align, sub) &&
            !resolve_type_layout(type.to_string(), size, align, sub) &&
            !resolve_type_layout(type.struct_name, size, align, sub)) {
            return false;
        }
        out = static_cast<long long>(want_align ? align : size);
        return true;
    }

    bool GenericExpander::layout_of_composite_type(const AST::Type& type,
        std::size_t& size, std::size_t& align, const Substitution& sub) const {
        switch (type.kind) {
        case TypeKind::Int:
        case TypeKind::Uint:
        case TypeKind::Float:
            size = 4; align = 4; return true;
        case TypeKind::Lint:
        case TypeKind::Luint:
        case TypeKind::Double:
            size = 8; align = 8; return true;
        case TypeKind::Char:
        case TypeKind::Uchar:
        case TypeKind::Bool:
            size = 1; align = 1; return true;
        case TypeKind::String:
            size = 32; align = 8; return true;
        case TypeKind::File:
            size = 8; align = 8; return true;
        case TypeKind::Pointer:
        case TypeKind::Function:
            size = 8; align = 8; return true;
        case TypeKind::Void:
            size = 0; align = 1; return true;
        case TypeKind::Array:
            if (!type.element_type) return false;
            if (!layout_of_composite_type(*type.element_type, size, align, sub)) {
                return false;
            }
            size *= type.array_size.value_or(0);
            return true;
        case TypeKind::Struct:
            return resolve_type_layout(type.struct_name, size, align, sub);
        }
        return false;
    }

    bool GenericExpander::eval_bool_property(const Expression* receiver,
        const std::string& property, const std::vector<const Expression*>& args,
        const Substitution& sub, bool& out) {
        auto* prim = dynamic_cast<const PrimaryExpression*>(receiver);
        if (prim == nullptr || prim->kind != PrimaryExpression::Kind::Identifier) {
            return false;
        }
        const std::string& parameter = prim->identifier;
        AST::Type type;
        bool is_type_param = (sub.types.find(parameter) != sub.types.end());
        bool is_constant_param = (sub.constant_types.find(parameter) != sub.constant_types.end());
        if (!is_type_param && !is_constant_param) {
            return false;
        }
        if (!param_type_of(parameter, sub, type)) return false;

        if (property == "is_same" || property == "is_convertible" ||
            property == "has_member") {
            if (!is_type_param) {
                report(receiver->location, ErrorCode::CompileTimePropertyNotApplicable, std::vector<std::string>{ property, parameter });
                return false;
            }
            if (args.empty()) {
                report(receiver->location, ErrorCode::CompileTimePropertyArgCountMismatch, std::vector<std::string>{ property, "1", "0" });
                return false;
            }
            if (property == "has_member") {
                const StructDefinition* def = nullptr;
                if (type.kind == TypeKind::Struct) {
                    auto found = struct_defs_.find(type.struct_name);
                    if (found != struct_defs_.end()) def = found->second;
                }
                for (std::size_t i = 0; i < args.size(); ++i) {
                    auto* arg_prim = dynamic_cast<const PrimaryExpression*>(args[i]);
                    if (arg_prim == nullptr ||
                        arg_prim->kind != PrimaryExpression::Kind::Literal ||
                        arg_prim->literal_token.type != TokenType::StringLiteral) {
                        report(receiver->location,
                            ErrorCode::CompileTimePropertyArgNotString, std::vector<std::string>{ property, std::to_string(i + 1) });
                        return false;
                    }
                    std::string member_name;
                    decode_string_literal(arg_prim->literal_token.lexeme, member_name);
                    if (def == nullptr) { out = false; return true; }
                    bool has = false;
                    for (const auto& member : def->members) {
                        if (member.name == member_name) { has = true; break; }
                    }
                    if (!has) { out = false; return true; }
                }
                out = true;
                return true;
            }
            for (const Expression* arg : args) {
                std::string arg_text = expression_text(arg);
                AST::Type arg_type;
                if (!property_argument_type(arg_text, sub, arg_type)) {
                    return false;
                }
                if (property == "is_same") {
                    if (!(type == arg_type)) { out = false; return true; }
                }
                else {
                    if (!property_convertible(type, arg_type)) { out = false; return true; }
                }
            }
            out = true;
            return true;
        }

        auto matches = [&](TypeKind kind) { out = (type.kind == kind); return true; };
        const bool type_property = property == "is_integer" || property == "is_float" ||
            property == "is_double" || property == "is_char" || property == "is_bool" ||
            property == "is_struct" || property == "is_string" ||
            property == "is_pointer" || property == "is_array";
        if (type_property && !is_type_param) {
            report(receiver->location, ErrorCode::CompileTimePropertyNotApplicable,
                std::vector<std::string>{ property, parameter });
            return false;
        }
        if (property == "is_integer") {
            out = type.kind == TypeKind::Int || type.kind == TypeKind::Lint ||
                type.kind == TypeKind::Uint || type.kind == TypeKind::Luint ||
                type.kind == TypeKind::Char || type.kind == TypeKind::Uchar ||
                type.kind == TypeKind::Bool;
            return true;
        }
        if (property == "is_float") return matches(TypeKind::Float);
        if (property == "is_double") return matches(TypeKind::Double);
        if (property == "is_char") return matches(TypeKind::Char);
        if (property == "is_bool") return matches(TypeKind::Bool);
        if (property == "is_struct") return matches(TypeKind::Struct);
        if (property == "is_string") return matches(TypeKind::String);
        if (property == "is_pointer") return matches(TypeKind::Pointer);
        if (property == "is_array") return matches(TypeKind::Array);
        return false;
    }

    bool GenericExpander::eval_compile_time_condition(const Expression* expr,
        const Substitution& sub, bool& out) {
        if (expr == nullptr) return false;
        if (auto* e = dynamic_cast<const UnaryExpression*>(expr)) {
            if (e->op == UnaryExpression::Operator::LogicalNot) {
                bool inner = false;
                if (!eval_compile_time_condition(e->operand.get(), sub, inner)) return false;
                out = !inner;
                return true;
            }
            return false;
        }
        if (auto* e = dynamic_cast<const LogicalAndExpression*>(expr)) {
            bool left = false;
            bool right = false;
            if (!eval_compile_time_condition(e->left.get(), sub, left)) return false;
            if (!eval_compile_time_condition(e->right.get(), sub, right)) return false;
            out = left && right;
            return true;
        }
        if (auto* e = dynamic_cast<const LogicalOrExpression*>(expr)) {
            bool left = false;
            bool right = false;
            if (!eval_compile_time_condition(e->left.get(), sub, left)) return false;
            if (!eval_compile_time_condition(e->right.get(), sub, right)) return false;
            out = left || right;
            return true;
        }
        if (auto* e = dynamic_cast<const ConditionalExpression*>(expr)) {
            bool condition = false;
            if (!eval_compile_time_condition(e->condition.get(), sub, condition)) {
                return false;
            }
            return eval_compile_time_condition(
                condition ? e->then_expr.get() : e->else_expr.get(), sub, out);
        }

        auto property_side = [&](const Expression* side, bool& value,
            bool& is_property) -> bool {
            is_property = false;
            if (auto* e = dynamic_cast<const PostfixExpression*>(side)) {
                if (e->op == PostfixExpression::Operator::Dot) {
                    is_property = true;
                    return eval_bool_property(e->base.get(), e->member_name, {}, sub, value);
                }
            }
            if (auto* e = dynamic_cast<const CompileTimePropertyExpression*>(side)) {
                is_property = true;
                std::vector<const Expression*> args;
                for (const auto& arg : e->arguments) args.push_back(arg.get());
                return eval_bool_property(e->receiver.get(), e->property, args, sub, value);
            }
            return false;
        };
        auto literal_side = [&](const Expression* side, bool& value) -> bool {
            auto* prim = dynamic_cast<const PrimaryExpression*>(side);
            if (prim == nullptr || prim->kind != PrimaryExpression::Kind::Literal) return false;
            if (prim->literal_token.type != TokenType::BoolLiteral) return false;
            value = (prim->literal_token.lexeme == "true");
            return true;
        };

        if (auto* e = dynamic_cast<const ComparisonExpression*>(expr)) {
            auto string_operand = [&](const Expression* side, std::string& value) -> bool {
                auto* prim = dynamic_cast<const PrimaryExpression*>(side);
                if (prim == nullptr) return false;
                if (prim->kind == PrimaryExpression::Kind::Literal &&
                    prim->literal_token.type == TokenType::StringLiteral) {
                    return decode_string_literal(prim->literal_token.lexeme, value);
                }
                if (prim->kind == PrimaryExpression::Kind::Identifier) {
                    auto found = sub.constants.find(prim->identifier);
                    if (found != sub.constants.end() && found->second.is_string) {
                        value = found->second.string_value;
                        return true;
                    }
                }
                return false;
            };
            std::string left_text;
            std::string right_text;
            auto typename_operand = [&](const Expression* side, std::string& value) -> bool {
                const Expression* receiver = nullptr;
                std::string property;
                if (auto* post = dynamic_cast<const PostfixExpression*>(side)) {
                    if (post->op == PostfixExpression::Operator::Dot) {
                        receiver = post->base.get();
                        property = post->member_name;
                    }
                }
                else if (auto* prop = dynamic_cast<const CompileTimePropertyExpression*>(side)) {
                    receiver = prop->receiver.get();
                    property = prop->property;
                }
                if (receiver == nullptr || property != "typename") return false;
                auto* prim = dynamic_cast<const PrimaryExpression*>(receiver);
                if (prim == nullptr || prim->kind != PrimaryExpression::Kind::Identifier) {
                    return false;
                }
                if (sub.types.find(prim->identifier) == sub.types.end()) return false;
                value = sub.types.at(prim->identifier).to_string();
                return true;
            };
            if (typename_operand(e->left.get(), left_text)) {
                if (!string_operand(e->right.get(), right_text)) return false;
                switch (e->op) {
                case ComparisonExpression::Operator::Equal:
                    out = (left_text == right_text);
                    return true;
                case ComparisonExpression::Operator::NotEqual:
                    out = (left_text != right_text);
                    return true;
                default:
                    return false;
                }
            }
            if (typename_operand(e->right.get(), right_text)) {
                if (!string_operand(e->left.get(), left_text)) return false;
                switch (e->op) {
                case ComparisonExpression::Operator::Equal:
                    out = (left_text == right_text);
                    return true;
                case ComparisonExpression::Operator::NotEqual:
                    out = (left_text != right_text);
                    return true;
                default:
                    return false;
                }
            }
            if (string_operand(e->left.get(), left_text) &&
                string_operand(e->right.get(), right_text)) {
                switch (e->op) {
                case ComparisonExpression::Operator::Equal:
                    out = (left_text == right_text);
                    return true;
                case ComparisonExpression::Operator::NotEqual:
                    out = (left_text != right_text);
                    return true;
                default:
                    return false;
                }
            }
            bool left_prop = false;
            bool left_value = false;
            bool left_is_property = false;
            bool right_value = false;
            bool right_is_property = false;
            bool left_ok = property_side(e->left.get(), left_value, left_is_property);
            bool right_ok = property_side(e->right.get(), right_value, right_is_property);
            if (!left_ok) {
                left_ok = literal_side(e->left.get(), left_value);
                left_is_property = false;
            }
            if (!right_ok) {
                right_ok = literal_side(e->right.get(), right_value);
                right_is_property = false;
            }
            if (!left_ok || !right_ok) return false;
            (void)left_prop;
            (void)left_is_property;
            (void)right_is_property;
            switch (e->op) {
            case ComparisonExpression::Operator::Equal:
                out = (left_value == right_value);
                return true;
            case ComparisonExpression::Operator::NotEqual:
                out = (left_value != right_value);
                return true;
            default:
                return false;
            }
        }
        bool value = false;
        bool is_property = false;
        if (property_side(expr, value, is_property) && is_property) {
            out = value;
            return true;
        }
        if (literal_side(expr, value)) {
            out = value;
            return true;
        }
        return false;
    }

    bool GenericExpander::eval_emit_piece(const Expression* expr, const Substitution& sub,
        std::string& out) {
        if (expr == nullptr) return false;
        if (auto* e = dynamic_cast<const AdditiveExpression*>(expr)) {
            if (e->op != AdditiveExpression::Operator::Plus) return false;
            std::string left;
            std::string right;
            bool left_ok = eval_emit_piece(e->left.get(), sub, left);
            bool right_ok = eval_emit_piece(e->right.get(), sub, right);
            if (left_ok && right_ok) {
                out += left;
                out += right;
                return true;
            }
            ConstantValue value;
            if (evaluate_with_substitution(expr, sub, value)) {
                if (value.is_float) {
                    char buffer[64];
                    std::snprintf(buffer, sizeof(buffer), "%g", value.float_value);
                    out += buffer;
                }
                else {
                    out += std::to_string(value.int_value);
                }
                return true;
            }
            return false;
        }
        if (auto* e = dynamic_cast<const PrimaryExpression*>(expr)) {
            if (e->kind == PrimaryExpression::Kind::Literal) {
                switch (e->literal_token.type) {
                case TokenType::StringLiteral: {
                    std::string decoded;
                    decode_string_literal(e->literal_token.lexeme, decoded);
                    out += decoded;
                    return true;
                }
                case TokenType::IntegerLiteral:
                case TokenType::FloatLiteral:
                case TokenType::CharLiteral:
                case TokenType::BoolLiteral:
                    out += std::string(e->literal_token.lexeme);
                    return true;
                default:
                    return false;
                }
            }
            if (e->kind == PrimaryExpression::Kind::Identifier) {
                auto constant = sub.constants.find(e->identifier);
                if (constant != sub.constants.end()) {
                    if (constant->second.is_string) {
                        out += constant->second.string_value;
                    }
                    else if (constant->second.is_float) {
                        char buffer[64];
                        std::snprintf(buffer, sizeof(buffer), "%g",
                            constant->second.float_value);
                        out += buffer;
                    }
                    else {
                        out += std::to_string(constant->second.int_value);
                    }
                    return true;
                }
                return false;
            }
            return false;
        }
        if (auto* e = dynamic_cast<const PostfixExpression*>(expr)) {
            if (e->op != PostfixExpression::Operator::Dot) return false;
            auto* prim = dynamic_cast<const PrimaryExpression*>(e->base.get());
            if (prim == nullptr || prim->kind != PrimaryExpression::Kind::Identifier) {
                return false;
            }
            const std::string& parameter = prim->identifier;
            bool is_type_param = (sub.types.find(parameter) != sub.types.end());
            bool is_constant_param =
                (sub.constant_types.find(parameter) != sub.constant_types.end());
            if (!is_type_param && !is_constant_param) return false;
            if (e->member_name == "typename") {
                if (!is_type_param) {
                    report(e->location, ErrorCode::CompileTimePropertyNotApplicable, std::vector<std::string>{ e->member_name, parameter });
                    return false;
                }
                out += sub.types.at(parameter).to_string();
                return true;
            }
            if (e->member_name == "size" || e->member_name == "align") {
                long long value = 0;
                if (!eval_size_align_property(parameter, e->member_name, sub, value)) {
                    report(e->location, ErrorCode::CompileTimePropertyNotApplicable, std::vector<std::string>{ e->member_name, parameter });
                    return false;
                }
                out += std::to_string(value);
                return true;
            }
            return false;
        }
        return false;
    }

    bool GenericExpander::flush_emit_string(const std::string& text, SourceLocation loc,
        std::vector<AST::TopLevel*>& member_ptrs,
        std::vector<std::unique_ptr<AST::TopLevel>>& owner) {
        if (text.empty()) return true;
        emit_source_pool_.push_back(text);
        std::string& buffer = emit_source_pool_.back();
        std::string name = "<emit>";
        std::string_view source_view(buffer);
        std::string_view name_view(name);
        DiagnosticEngine emit_diag;
        Lexer lexer(source_view, name_view, emit_diag);
        Parser parser(lexer, emit_diag);
        auto program = parser.parse();
        if (program == nullptr || emit_diag.has_errors()) {
            report(loc, ErrorCode::EmitBlockNotExpandable, std::vector<std::string>());
            return false;
        }
        for (auto& top : program->top_levels) {
            if (dynamic_cast<GenericDefinition*>(top.get()) != nullptr) {
                report(loc, ErrorCode::EmitBlockNotExpandable, std::vector<std::string>());
                return false;
            }
            member_ptrs.push_back(top.get());
            owner.push_back(std::move(top));
        }
        return true;
    }

    void GenericExpander::expand_expression(std::unique_ptr<Expression>& expr_holder) {
        Expression* expr = expr_holder.get();
        if (expr == nullptr) return;
        if (auto* e = dynamic_cast<AssignmentExpression*>(expr)) {
            expand_expression(e->left);
            expand_expression(e->right);
        }
        else if (auto* e = dynamic_cast<LogicalOrExpression*>(expr)) {
            expand_expression(e->left);
            expand_expression(e->right);
        }
        else if (auto* e = dynamic_cast<LogicalAndExpression*>(expr)) {
            expand_expression(e->left);
            expand_expression(e->right);
        }
        else if (auto* e = dynamic_cast<ComparisonExpression*>(expr)) {
            expand_expression(e->left);
            expand_expression(e->right);
        }
        else if (auto* e = dynamic_cast<AdditiveExpression*>(expr)) {
            expand_expression(e->left);
            expand_expression(e->right);
        }
        else if (auto* e = dynamic_cast<MultiplicativeExpression*>(expr)) {
            expand_expression(e->left);
            expand_expression(e->right);
        }
        else if (auto* e = dynamic_cast<PowerExpression*>(expr)) {
            expand_expression(e->left);
            expand_expression(e->right);
        }
        else if (auto* e = dynamic_cast<BitwiseExpression*>(expr)) {
            expand_expression(e->left);
            expand_expression(e->right);
        }
        else if (auto* e = dynamic_cast<ShiftExpression*>(expr)) {
            expand_expression(e->left);
            expand_expression(e->right);
        }
        else if (auto* e = dynamic_cast<ConditionalExpression*>(expr)) {
            expand_expression(e->condition);
            expand_expression(e->then_expr);
            expand_expression(e->else_expr);
        }
        else if (auto* e = dynamic_cast<UnaryExpression*>(expr)) {
            expand_expression(e->operand);
        }
        else if (auto* e = dynamic_cast<PostfixExpression*>(expr)) {
            expand_expression(e->base);
            expand_expression(e->subscript_expr);
            for (auto& arg : e->arguments) expand_expression(arg);
            resolve_type(e->cast_type);
        }
        else if (auto* e = dynamic_cast<PrimaryExpression*>(expr)) {
            switch (e->kind) {
            case PrimaryExpression::Kind::Parens:
                expand_expression(e->paren_expr);
                break;
            case PrimaryExpression::Kind::Heap:
                resolve_type(e->heap_type);
                expand_expression(e->heap_size);
                break;
            case PrimaryExpression::Kind::QualifiedName: {
                auto mangled = ensure_instantiation(*e->generic_ref, false);
                std::string name = mangled.has_value() ? *mangled : e->generic_ref->to_string();
                expr_holder = std::make_unique<PrimaryExpression>(e->location, name);
                break;
            }
            case PrimaryExpression::Kind::Construct:
            case PrimaryExpression::Kind::PlacementConstruct: {
                resolve_type(e->construct_type);
                for (auto& arg : e->construct_args) expand_expression(arg);
                expand_expression(e->placement_target);
                break;
            }
            case PrimaryExpression::Kind::CopyMove:
                expand_expression(e->paren_expr);
                break;
            case PrimaryExpression::Kind::Identifier: {
                std::string name = e->identifier;
                bool resolved_short_name = false;
                for (std::size_t depth = short_scopes_.size(); depth > 0; --depth) {
                    const auto& shorts = short_scopes_[depth - 1];
                    auto found = shorts.find(name);
                    if (found != shorts.end()) {
                        expr_holder = std::make_unique<PrimaryExpression>(e->location,
                            found->second.target);
                        resolved_short_name = true;
                        break;
                    }
                    const auto& declared = declared_scopes_[depth - 1];
                    if (declared.count(name) != 0) {
                        break;
                    }
                }
                if (!resolved_short_name) {
                    for (std::size_t depth = instance_scopes_.size(); depth > 0; --depth) {
                        for (const std::string& instance : instance_scopes_[depth - 1]) {
                            auto missing = missing_members_.find(instance);
                            if (missing != missing_members_.end() &&
                                missing->second.count(name) != 0) {
                                report(e->location, ErrorCode::GenericSpecializationMissingMember, std::vector<std::string>{ instance, name });
                                return;
                            }
                        }
                    }
                }
                break;
            }
            default:
                break;
            }
        }
    }

}
