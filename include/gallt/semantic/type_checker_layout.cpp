#include "../semantic/type_checker.hpp"
#include "type_checker_detail.hpp"
#include "../parser/ast.hpp"
#include "../semantic/constant_folding.hpp"
#include <algorithm>
#include <cctype>
#include <charconv>
#include <system_error>
#include <unordered_set>
#include <functional>

using namespace gallt::AST;

namespace gallt {
    using namespace type_checker_detail;

    void TypeChecker::collect_local_structs(AST::Statement* stmt) {
        if (stmt == nullptr) { return; }
        if (auto* def = dynamic_cast<AST::StructDefinition*>(stmt)) {
            if (struct_defs_.find(def->name) == struct_defs_.end() &&
                predeclared_structs_.find(def->name) == predeclared_structs_.end()) {
                predeclared_structs_[def->name] = def;
            }
        }

        if (auto* block = dynamic_cast<AST::Block*>(stmt)) {
            for (auto& inner : block->statements) {
                collect_local_structs(inner.get());
            }

            return;
        }

        if (auto* ifs = dynamic_cast<AST::IfStatement*>(stmt)) {
            collect_local_structs(ifs->then_block.get());
            if (ifs->else_block) { collect_local_structs(ifs->else_block.get()); }
            return;
        }

        if (auto* for_stmt = dynamic_cast<AST::ForStatement*>(stmt)) {
            if (for_stmt->init) { collect_local_structs(for_stmt->init.get()); }
            if (for_stmt->body) { collect_local_structs(for_stmt->body.get()); }
            return;
        }

        if (auto* while_stmt = dynamic_cast<AST::WhileStatement*>(stmt)) {
            if (while_stmt->body) { collect_local_structs(while_stmt->body.get()); }
            return;
        }
    }

    bool TypeChecker::type_contains_struct_by_value(const AST::Type& type,
        const std::string& target, std::vector<std::string>& visited) {
        if (type.kind == TypeKind::Array) {
            return type.element_type != nullptr &&
                type_contains_struct_by_value(*type.element_type, target, visited);
        }

        if (type.kind != TypeKind::Struct) {
            return false;
        }

        if (type.struct_name == target) {
            return true;
        }

        if (std::find(visited.begin(), visited.end(), type.struct_name) != visited.end()) {
            return false;
        }

        visited.push_back(type.struct_name);
        AST::StructDefinition* def = get_struct_definition(type.struct_name);
        if (def == nullptr) { return false; }

        for (const auto& member : def->members) {
            if (type_contains_struct_by_value(member.type, target, visited)) {
                return true;
            }
        }
        return false;
    }

    std::size_t TypeChecker::type_size_of(const AST::Type& type) {
        auto round_up = [](std::size_t value, std::size_t alignment) -> std::size_t {
            if (alignment <= 1) { return value; }
            return (value + alignment - 1) / alignment * alignment;
        };
        switch (type.kind) {
        case TypeKind::Int:
        case TypeKind::Uint:
        case TypeKind::Float:
            return 4;
        case TypeKind::Lint:
        case TypeKind::Luint:
        case TypeKind::Double:
            return 8;
        case TypeKind::Char:
        case TypeKind::Uchar:
        case TypeKind::Bool:
            return 1;
        case TypeKind::String:
            return 32;
        case TypeKind::File:
            return 8;
        case TypeKind::Pointer:
        case TypeKind::Function:
            return 8;
        case TypeKind::Void:
            return 0;
        case TypeKind::Array:
            return type.array_size.value_or(0) *
                (type.element_type ? type_size_of(*type.element_type) : 0u);
        case TypeKind::Struct: {
            if (layout_depth_ > 64) { return 0; }
            struct DepthGuard {
                int& depth;
                explicit DepthGuard(int& d) : depth(d) { ++depth; }

                ~DepthGuard() { --depth; }
            } guard(layout_depth_);
            AST::StructDefinition* def = get_struct_definition(type.struct_name);
            if (def == nullptr) { return 0; }

            std::size_t offset = 0;
            std::size_t alignment = 1;

            for (const auto& member : def->members) {
                const std::size_t member_align = type_align_of(member.type);
                if (member_align > alignment) { alignment = member_align; }
                offset = round_up(offset, member_align);
                offset += type_size_of(member.type);
            }

            return round_up(offset, alignment);
        }
        }
        return 0;
    }

    std::size_t TypeChecker::type_align_of(const AST::Type& type) {
        switch (type.kind) {
        case TypeKind::Int:
        case TypeKind::Uint:
        case TypeKind::Float:
            return 4;
        case TypeKind::Lint:
        case TypeKind::Luint:
        case TypeKind::Double:
            return 8;
        case TypeKind::Char:
        case TypeKind::Uchar:
        case TypeKind::Bool:
            return 1;
        case TypeKind::String:
            return 8;
        case TypeKind::File:
            return 8;
        case TypeKind::Pointer:
        case TypeKind::Function:
            return 8;
        case TypeKind::Void:
            return 1;
        case TypeKind::Array:
            return type.element_type ? type_align_of(*type.element_type) : 1u;
        case TypeKind::Struct: {
            if (layout_depth_ > 64) { return 1; }
            struct DepthGuard {
                int& depth;
                explicit DepthGuard(int& d) : depth(d) { ++depth; }

                ~DepthGuard() { --depth; }
            } guard(layout_depth_);
            std::size_t alignment = 1;
            AST::StructDefinition* def = get_struct_definition(type.struct_name);
            if (def == nullptr) { return alignment; }

            for (const auto& member : def->members) {
                const std::size_t member_align = type_align_of(member.type);
                if (member_align > alignment) { alignment = member_align; }
            }

            return alignment;
        }
        }
        return 1;
    }

    bool TypeChecker::type_layout_of_name(const std::string& name, std::size_t& size,
        std::size_t& align) {
        AST::Type named;
        if (name == "int" || name == "uint" || name == "float") {
            named = AST::Type::make_int();
            if (name == "uint") {
                named = AST::Type::make_uint();
            } else if (name == "float") {
                named = AST::Type::make_float();
            }
        } else if (name == "lint") {
            named = AST::Type::make_lint();
        } else if (name == "luint") {
            named = AST::Type::make_luint();
        } else if (name == "double") {
            named = AST::Type::make_double();
        } else if (name == "char") {
            named = AST::Type::make_char();
        } else if (name == "uchar") {
            named = AST::Type::make_uchar();
        } else if (name == "bool") {
            named = AST::Type::make_bool();
        } else if (name == "string") {
            named = AST::Type::make_string();
        } else if (name == "file") {
            named = AST::Type::make_file();
        } else if (name == "void") {
            named = AST::Type::make_void();
        } else if (struct_defs_.find(name) != struct_defs_.end()) {
            named = AST::Type::make_struct(name);
        } else {
            const Symbol* symbol = sym_table_.lookup(name);
            if (symbol == nullptr || symbol->kind == SymbolKind::Function) {
                return false;
            }
            named = symbol->type;
        }
        size = type_size_of(named);
        align = type_align_of(named);
        return size > 0;
    }

    bool TypeChecker::is_complete_type(const AST::Type& type) {
        if (type.kind == TypeKind::Void) { return true; }
        if (type.kind == TypeKind::Int || type.kind == TypeKind::Lint ||
            type.kind == TypeKind::Uint || type.kind == TypeKind::Luint ||
            type.kind == TypeKind::Float ||
            type.kind == TypeKind::Double || type.kind == TypeKind::Char ||
            type.kind == TypeKind::Uchar ||
            type.kind == TypeKind::Bool || type.kind == TypeKind::String ||
            type.kind == TypeKind::File) {
            return true;
        }
        if (type.kind == TypeKind::Pointer) {
            return true;
        }
        if (type.kind == TypeKind::Array) {
            if (!type.array_size.has_value() || type.array_size.value() == 0) { return false; }
            if (type.element_type) { return is_complete_type(*type.element_type); }
            return false;
        }
        if (type.kind == TypeKind::Struct) {
            auto it = struct_defs_.find(type.struct_name);
            if (it != struct_defs_.end()) { return true; }
            return predeclared_structs_.find(type.struct_name) !=
                predeclared_structs_.end();
        }
        if (type.kind == TypeKind::Function) {
            if (type.return_type && !is_complete_type(*type.return_type) && type.return_type->kind != TypeKind::Void) {
                return false;
            }
            for (const auto& p : type.parameter_types) {
                if (!is_complete_type(p) && p.kind != TypeKind::Void) {
                    return false;
                }
            }
            return true;
        }
        return false;
    }

    std::optional<size_t> TypeChecker::evaluate_const_expression(AST::Expression* expr) {
        if (auto* prim = dynamic_cast<AST::PrimaryExpression*>(expr)) {
            if (prim->kind == AST::PrimaryExpression::Kind::Literal &&
                prim->literal_token.type == TokenType::IntegerLiteral) {
                std::string_view lexeme = prim->literal_token.lexeme;
                size_t value = 0;
                auto [ptr, ec] = std::from_chars(lexeme.data(), lexeme.data() + lexeme.size(), value);
                if (ec == std::errc()) { return value; }
            }
        }
        return std::nullopt;
    }

    bool TypeChecker::is_constant_integer_expression(AST::Expression* expr, size_t* out_value) {
        if (auto val = evaluate_const_expression(expr)) {
            if (out_value) { *out_value = *val; }
            return true;
        }
        return false;
    }

    std::optional<long long> TypeChecker::evaluate_const_integer_expression(
        AST::Expression* expr) {
        if (expr == nullptr) { return std::nullopt; }
        ConstantEvaluationContext ctx;
        ctx.type_layout = [this](const std::string& type_name, std::size_t& size,
            std::size_t& align) {
            return type_layout_of_name(type_name, size, align);
        };
        ctx.lookup_constant = [this](const std::string& name, long long& int_value,
            double& float_value, bool& is_float) {
            for (auto it = const_values_.rbegin(); it != const_values_.rend(); ++it) {
                auto found = it->find(name);
                if (found != it->end()) {
                    int_value = found->second.int_value;
                    float_value = found->second.float_value;
                    is_float = found->second.is_float;
                    return true;
                }
            }
            return false;
        };
        long long int_value = 0;
        double float_value = 0.0;
        bool is_float = false;
        if (!evaluate_constant_expression(expr, int_value, float_value, is_float, ctx)) {
            return std::nullopt;
        }
        if (is_float) {
            const auto truncated = static_cast<long long>(float_value);
            if (static_cast<double>(truncated) != float_value) { return std::nullopt; }
            int_value = truncated;
        }
        if (int_value < 0) { return std::nullopt; }
        return int_value;
    }

    std::optional<long long> TypeChecker::evaluate_signed_const_integer_expression(
        AST::Expression* expr) {
        if (expr == nullptr) { return std::nullopt; }
        ConstantEvaluationContext ctx;
        ctx.type_layout = [this](const std::string& type_name, std::size_t& size,
            std::size_t& align) {
            return type_layout_of_name(type_name, size, align);
        };
        ctx.lookup_constant = [this](const std::string& name, long long& int_value,
            double& float_value, bool& is_float) {
            for (auto it = const_values_.rbegin(); it != const_values_.rend(); ++it) {
                auto found = it->find(name);
                if (found != it->end()) {
                    int_value = found->second.int_value;
                    float_value = found->second.float_value;
                    is_float = found->second.is_float;
                    return true;
                }
            }
            return false;
        };
        long long int_value = 0;
        double float_value = 0.0;
        bool is_float = false;
        if (!evaluate_constant_expression(expr, int_value, float_value, is_float, ctx)) {
            return std::nullopt;
        }
        if (is_float) {
            const auto truncated = static_cast<long long>(float_value);
            if (static_cast<double>(truncated) != float_value) { return std::nullopt; }
            int_value = truncated;
        }
        return int_value;
    }

    void TypeChecker::record_const_value(const std::string& name, AST::Expression* initializer,
        const AST::Type& type) {
        if (!type.is_integer() && !type.is_floating()) { return; }
        ConstantEvaluationContext ctx;
        ctx.type_layout = [this](const std::string& type_name, std::size_t& size,
            std::size_t& align) {
            return type_layout_of_name(type_name, size, align);
        };
        ctx.lookup_constant = [this](const std::string& lookup, long long& int_value,
            double& float_value, bool& is_float) {
            for (auto it = const_values_.rbegin(); it != const_values_.rend(); ++it) {
                auto found = it->find(lookup);
                if (found != it->end()) {
                    int_value = found->second.int_value;
                    float_value = found->second.float_value;
                    is_float = found->second.is_float;
                    return true;
                }
            }
            return false;
        };
        long long int_value = 0;
        double float_value = 0.0;
        bool is_float = false;
        if (!evaluate_constant_expression(initializer, int_value, float_value, is_float, ctx)) {
            return;
        }
        ConstValue value;
        if (type.is_floating()) {
            value.is_float = true;
            value.float_value = is_float ? float_value : static_cast<double>(int_value);
            value.int_value = static_cast<long long>(value.float_value);
        } else {
            value.is_float = false;
            value.int_value = is_float ? static_cast<long long>(float_value) : int_value;
            value.float_value = static_cast<double>(value.int_value);
        }
        if (const_values_.empty()) {
            const_values_.emplace_back();
        }
        const_values_.back()[name] = value;
    }

    std::string TypeChecker::expression_display_name(const AST::Expression* expr) {
        using namespace AST;
        if (expr == nullptr) { return "<expression>"; }
        if (auto* prim = dynamic_cast<const PrimaryExpression*>(expr)) {
            switch (prim->kind) {
            case PrimaryExpression::Kind::Identifier:
                return prim->identifier;
            case PrimaryExpression::Kind::Parens:
                return expression_display_name(prim->paren_expr.get());
            default:
                return "<expression>";
            }
        }
        if (auto* post = dynamic_cast<const PostfixExpression*>(expr)) {
            switch (post->op) {
            case PostfixExpression::Operator::Subscript:
            case PostfixExpression::Operator::Increment:
            case PostfixExpression::Operator::Decrement:
                return expression_display_name(post->base.get());
            case PostfixExpression::Operator::Dot:
            case PostfixExpression::Operator::Arrow:
                if (!post->member_name.empty()) { return post->member_name; }
                return expression_display_name(post->base.get());
            default:
                return "<expression>";
            }
        }
        if (auto* un = dynamic_cast<const UnaryExpression*>(expr)) {
            if (un->op == UnaryExpression::Operator::Dereference ||
                un->op == UnaryExpression::Operator::Increment ||
                un->op == UnaryExpression::Operator::Decrement) {
                return expression_display_name(un->operand.get());
            }
        }
        return "<expression>";
    }

    bool TypeChecker::is_address_of_const_identifier(const AST::Expression* expr) {
        auto* unary = dynamic_cast<const AST::UnaryExpression*>(expr);
        if (unary == nullptr ||
            unary->op != AST::UnaryExpression::Operator::AddressOf) {
            return false;
        }
        auto* target = dynamic_cast<const AST::PrimaryExpression*>(unary->operand.get());
        if (target == nullptr ||
            target->kind != AST::PrimaryExpression::Kind::Identifier) {
            return false;
        }
        const Symbol* symbol = sym_table_.lookup(target->identifier);
        return symbol != nullptr && symbol->kind != SymbolKind::Function &&
            symbol->type.is_const;
    }

    bool TypeChecker::is_compile_time_constant_expression(AST::Expression* expr) {
        using namespace AST;
        if (expr == nullptr) { return false; }

        if (auto* prim = dynamic_cast<PrimaryExpression*>(expr)) {
            switch (prim->kind) {
            case PrimaryExpression::Kind::Literal:
                return true;
            case PrimaryExpression::Kind::Parens:
                return is_compile_time_constant_expression(prim->paren_expr.get());
            case PrimaryExpression::Kind::Identifier: {
                Symbol* sym = lookup_symbol(prim->identifier, false);
                return sym != nullptr && sym->type.is_const;
            }
            case PrimaryExpression::Kind::Null:
                return true;
            default:
                return false;
            }
        }

        if (auto* un = dynamic_cast<UnaryExpression*>(expr)) {
            switch (un->op) {
            case UnaryExpression::Operator::UnaryPlus:
            case UnaryExpression::Operator::UnaryMinus:
            case UnaryExpression::Operator::LogicalNot:
            case UnaryExpression::Operator::BitwiseNot:
                return is_compile_time_constant_expression(un->operand.get());
            case UnaryExpression::Operator::AddressOf: {
                auto* prim = dynamic_cast<PrimaryExpression*>(un->operand.get());
                return prim != nullptr &&
                    (prim->kind == PrimaryExpression::Kind::Identifier ||
                        prim->kind == PrimaryExpression::Kind::Null);
            }
            default:
                return false;
            }
        }
        if (auto* e = dynamic_cast<AdditiveExpression*>(expr)) {
            return is_compile_time_constant_expression(e->left.get()) &&
                is_compile_time_constant_expression(e->right.get());
        }

        if (auto* e = dynamic_cast<MultiplicativeExpression*>(expr)) {
            return is_compile_time_constant_expression(e->left.get()) &&
                is_compile_time_constant_expression(e->right.get());
        }

        if (auto* e = dynamic_cast<PowerExpression*>(expr)) {
            return is_compile_time_constant_expression(e->left.get()) &&
                is_compile_time_constant_expression(e->right.get());
        }

        if (auto* e = dynamic_cast<ComparisonExpression*>(expr)) {
            return is_compile_time_constant_expression(e->left.get()) &&
                is_compile_time_constant_expression(e->right.get());
        }

        if (auto* e = dynamic_cast<BitwiseExpression*>(expr)) {
            return is_compile_time_constant_expression(e->left.get()) &&
                is_compile_time_constant_expression(e->right.get());
        }

        if (auto* e = dynamic_cast<ShiftExpression*>(expr)) {
            return is_compile_time_constant_expression(e->left.get()) &&
                is_compile_time_constant_expression(e->right.get());
        }

        if (auto* e = dynamic_cast<ConditionalExpression*>(expr)) {
            std::optional<long long> condition =
                evaluate_signed_const_integer_expression(e->condition.get());
            if (!condition.has_value()) { return false; }
            return is_compile_time_constant_expression(
                *condition != 0 ? e->then_expr.get() : e->else_expr.get());
        }

        if (auto* e = dynamic_cast<LogicalAndExpression*>(expr)) {
            return is_compile_time_constant_expression(e->left.get()) &&
                is_compile_time_constant_expression(e->right.get());
        }

        if (auto* e = dynamic_cast<LogicalOrExpression*>(expr)) {
            return is_compile_time_constant_expression(e->left.get()) &&
                is_compile_time_constant_expression(e->right.get());
        }

        if (auto* post = dynamic_cast<PostfixExpression*>(expr)) {
            switch (post->op) {
            case PostfixExpression::Operator::Cast:
                return is_compile_time_constant_expression(post->base.get()) &&
                    (post->cast_type.is_arithmetic() ||
                        post->cast_type.kind == TypeKind::String);
            case PostfixExpression::Operator::FunctionCall: {
                auto* callee = dynamic_cast<PrimaryExpression*>(post->base.get());
                if (callee == nullptr ||
                    callee->kind != PrimaryExpression::Kind::Identifier) {
                    return false;
                }
                if (callee->identifier != "size" && callee->identifier != "align") {
                    return false;
                }
                if (post->arguments.size() != 1) {
                    return false;
                }

                if (auto* prim_arg = dynamic_cast<PrimaryExpression*>(
                    post->arguments[0].get())) {
                    if (prim_arg->kind == PrimaryExpression::Kind::Identifier) {
                        std::size_t size = 0;
                        std::size_t align = 0;

                        if (type_layout_of_name(prim_arg->identifier, size, align)) {
                            return true;
                        }
                    }
                }
                return is_compile_time_constant_expression(post->arguments[0].get());
            }
            default:
                return false;
            }
        }

        return false;
    }

    void TypeChecker::check_const_declaration(AST::VariableDeclaration* decl) {
        const std::string& name = decl->name;
        if (decl->initializer == nullptr) {
            diag_.report_error_template(decl->location,
                ErrorCode::ConstCannotStoreVariable, { name });
            return;
        }

        if (auto* expr_init = dynamic_cast<AST::ExpressionInitializer*>(decl->initializer.get())) {
            record_const_value(name, expr_init->expr.get(), decl->type);
        }

        std::function<void(AST::Initializer*)> check_initializer =
            [&](AST::Initializer* init) {
                if (init == nullptr) { return; }
                if (auto* expr_init = dynamic_cast<AST::ExpressionInitializer*>(init)) {
                    if (expression_mentions_variadic_pack(expr_init->expr.get())) {
                        diag_.report_error_template(init->location,
                        ErrorCode::ParameterPackInConstantExpression, { name });
                        return;
                    }

                    if (!is_compile_time_constant_expression(expr_init->expr.get())) {
                        diag_.report_error_template(init->location,
                            ErrorCode::ConstCannotStoreVariable, { name });
                    }

                    return;
                }

                if (auto* arr_init = dynamic_cast<AST::ArrayInitializer*>(init)) {
                    for (auto& element : arr_init->elements) {
                        check_initializer(element.get());
                    }
                }
            };

        check_initializer(decl->initializer.get());
    }

    bool TypeChecker::type_layout(const AST::Type& type, std::size_t& size,
        std::size_t& align) const {
        auto round_up = [](std::size_t value, std::size_t alignment) {
            if (alignment <= 1) { return value; }
            return (value + alignment - 1) / alignment * alignment;
        };
        switch (type.kind) {
        case TypeKind::Int: case TypeKind::Uint:
        case TypeKind::Float: size = 4; align = 4; return true;
        case TypeKind::Lint: case TypeKind::Luint:
        case TypeKind::Double: size = 8; align = 8; return true;
        case TypeKind::Char: case TypeKind::Uchar:
        case TypeKind::Bool: size = 1; align = 1; return true;
        case TypeKind::String: size = 32; align = 8; return true;
        case TypeKind::File: size = 8; align = 8; return true;
        case TypeKind::Pointer:
        case TypeKind::Function: size = 8; align = 8; return true;
        case TypeKind::Array: {
            std::size_t element_size = 0;
            std::size_t element_align = 1;

            if (!type.element_type || !type_layout(*type.element_type, element_size, element_align)) {
                return false;
            }

            size = element_size * type.array_size.value_or(0);
            align = element_align;
            return true;
        }
        case TypeKind::Struct: {
            auto def = struct_defs_.find(type.struct_name);
            if (def == struct_defs_.end() || def->second == nullptr) { return false; }

            std::size_t offset = 0;
            std::size_t max_align = 1;

            for (const auto& member : def->second->members) {
                std::size_t member_size = 0;
                std::size_t member_align = 1;
                if (!type_layout(member.type, member_size, member_align)) { return false; }
                max_align = std::max(max_align, member_align);
                offset = round_up(offset, member_align);
                offset += member_size;
            }

            size = round_up(offset, max_align);
            align = max_align;
            return true;
        }
        default:
            return false;
        }
    }

    bool TypeChecker::type_is_copyable(const AST::Type& type) const {
        switch (type.kind) {
        case TypeKind::Array:
            return type.element_type ? type_is_copyable(*type.element_type) : true;
        case TypeKind::Struct: {
            auto def = struct_defs_.find(type.struct_name);
            if (def == struct_defs_.end() || def->second == nullptr) { return true; }
            if (def->second->no_copy) { return false; }

            for (const auto& member : def->second->members) {
                if (!type_is_copyable(member.type)) { return false; }
            }

            return true;
        }
        default:
            return true;
        }
    }

    bool TypeChecker::type_is_movable(const AST::Type& type) const {
        switch (type.kind) {
        case TypeKind::Array:
            return type.element_type ? type_is_movable(*type.element_type) : true;
        case TypeKind::Struct: {
            auto def = struct_defs_.find(type.struct_name);
            if (def == struct_defs_.end() || def->second == nullptr) { return true; }
            if (def->second->no_move) { return false; }

            for (const auto& member : def->second->members) {
                if (!type_is_movable(member.type)) { return false; }
            }

            return true;
        }
        default:
            return true;
        }
    }

    bool TypeChecker::is_expression_parameter_temp(const AST::Initializer* init) {
        if (init == nullptr || !init->is_expression()) {
            return false;
        }
        const auto* expr_init = static_cast<const AST::ExpressionInitializer*>(init);
        const auto* prim = dynamic_cast<const AST::PrimaryExpression*>(
            expr_init->expr.get());
        return prim != nullptr && prim->kind == AST::PrimaryExpression::Kind::Identifier &&
            prim->identifier.rfind("__glt_expr", 0) == 0;
    }

    bool TypeChecker::is_move_expression(const AST::Expression* expr) {
        auto* cm = dynamic_cast<const AST::PrimaryExpression*>(expr);
        return cm != nullptr && cm->kind == AST::PrimaryExpression::Kind::CopyMove &&
            cm->copy_move_kind == AST::PrimaryExpression::CopyMoveKind::Move;
    }

}
