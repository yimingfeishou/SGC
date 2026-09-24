#include "codegen.hpp"
#include "codegen_detail.hpp"
#include "../semantic/constant_folding.hpp"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <sstream>
#include <stdexcept>
#include <system_error>

using namespace gallt::AST;

namespace gallt {
    using namespace codegen_detail;

    bool CodeGenerator::fold_constant_declaration(AST::VariableDeclaration* decl,
        LocalInfo& info) {
        if (decl == nullptr || decl->initializer == nullptr) {
            return false;
        }

        auto* expr_init = dynamic_cast<AST::ExpressionInitializer*>(decl->initializer.get());
        if (expr_init == nullptr || expr_init->expr == nullptr) {
            return false;
        }

        AST::Expression* expr = expr_init->expr.get();
        info = LocalInfo{};
        info.type = decl->type;
        info.is_constant = true;
        if (decl->type.kind == TypeKind::String || decl->type.kind == TypeKind::File ||
            decl->type.kind == TypeKind::Pointer || decl->type.kind == TypeKind::Function) {
            info.constant_expr = expr;
            return true;
        }

        if (!decl->type.is_scalar()) {
            if (fold_constant_aggregate(decl, info)) {
                return true;
            }
            info.is_constant = false;
            return false;
        }

        ConstantEvaluationContext ctx;
        ctx.type_layout = [this](const std::string& name, std::size_t& size,
            std::size_t& align) {
            AST::Type type;
            if (!builtin_type_by_name(name, type)) { return false; }
            size = type_size(type);
            align = type_align(type);
            return true;
        };
        ctx.lookup_constant = [this](const std::string& name, long long& int_value,
            double& float_value, bool& is_float) {
            auto found = constant_values_.find(name);
            if (found == constant_values_.end()) { return false; }
            int_value = found->second.int_value;
            float_value = found->second.float_value;
            is_float = found->second.is_float;
            return true;
        };
        long long int_value = 0;
        double float_value = 0.0;
        bool is_float = false;
        if (!evaluate_constant_expression(expr, int_value, float_value, is_float, ctx)) {
            info.is_constant = false;
            return false;
        }

        ConstantNumeric value;
        if (decl->type.is_floating()) {
            double result = is_float ? float_value : static_cast<double>(int_value);

            if (decl->type.kind == TypeKind::Float) {
                result = static_cast<double>(static_cast<float>(result));
            }
            value.is_float = true;
            value.float_value = result;
            value.int_value = static_cast<long long>(result);
            std::ostringstream format;
            format.precision(17);
            format << result;
            info.constant_text = llvm_float_constant_text(format.str());
        } else {
            long long result = is_float ? static_cast<long long>(float_value) : int_value;
            switch (decl->type.kind) {
            case TypeKind::Char:
            case TypeKind::Uchar:
                result = static_cast<long long>(static_cast<unsigned char>(result));
                break;
            case TypeKind::Bool:
                result = (result != 0) ? 1 : 0;
                break;
            case TypeKind::Int:
            case TypeKind::Uint:
                result = static_cast<long long>(static_cast<int>(
                    static_cast<unsigned int>(result)));
                break;
            case TypeKind::Luint:
                result = static_cast<long long>(
                    static_cast<unsigned long long>(result));
                break;
            default:
                break;
            }
            value.is_float = false;
            value.int_value = result;
            value.float_value = static_cast<double>(result);
            info.constant_text = std::to_string(result);
        }

        constant_values_[decl->name] = value;
        return true;
    }

    bool CodeGenerator::build_constant_scalar(const AST::Expression* expr,
        const AST::Type& type, std::string& out) {
        if (expr == nullptr) { return false; }

        if (type.kind == TypeKind::String) {
            auto* prim = dynamic_cast<const AST::PrimaryExpression*>(expr);
            if (prim == nullptr ||
                prim->kind != AST::PrimaryExpression::Kind::Literal ||
                prim->literal_token.type != TokenType::StringLiteral) {
                return false;
            }

            std::string bytes = decode_escaped_bytes(prim->literal_token.lexeme, false);
            if (bytes.size() > 15) { return false; }
            std::string buffer(bytes);
            buffer.resize(16, '\0');
            out = "%struct.gallt.string { i64 " + std::to_string(bytes.size()) +
                ", i64 16, [16 x i8] " + llvm_escape_bytes(buffer) + " }";
            return true;
        }

        if (type.kind == TypeKind::Pointer || type.kind == TypeKind::File ||
            type.kind == TypeKind::Function) {
            auto* prim = dynamic_cast<const AST::PrimaryExpression*>(expr);
            if (prim != nullptr && prim->kind == AST::PrimaryExpression::Kind::Null) {
                out = llvm_type(type) + " null";
                return true;
            }

            if (type.kind == TypeKind::Pointer) {
                out = "ptr null";
                return false;
            }
            return false;
        }

        ConstantEvaluationContext ctx;
        ctx.lookup_constant = [this](const std::string& name, long long& int_value,
            double& float_value, bool& is_float) {
            auto found = constant_values_.find(name);
            if (found == constant_values_.end()) { return false; }
            int_value = found->second.int_value;
            float_value = found->second.float_value;
            is_float = found->second.is_float;
            return true;
        };
        long long int_value = 0;
        double float_value = 0.0;
        bool is_float = false;
        if (!evaluate_constant_expression(expr, int_value, float_value, is_float, ctx)) {
            return false;
        }

        if (type.is_floating()) {
            double result = is_float ? float_value : static_cast<double>(int_value);

            if (type.kind == TypeKind::Float) {
                result = static_cast<double>(static_cast<float>(result));
            }
            std::ostringstream format;
            format.precision(17);
            format << result;
            out = llvm_type(type) + " " + llvm_float_constant_text(format.str());
            return true;
        }

        long long result = is_float ? static_cast<long long>(float_value) : int_value;
        switch (type.kind) {
        case TypeKind::Char:
        case TypeKind::Uchar:
            result = static_cast<long long>(static_cast<unsigned char>(result));
            break;
        case TypeKind::Bool:
            result = (result != 0) ? 1 : 0;
            break;
        case TypeKind::Int:
        case TypeKind::Uint:
            result = static_cast<long long>(static_cast<int>(
                static_cast<unsigned int>(result)));
            break;
        case TypeKind::Luint:
            result = static_cast<long long>(static_cast<unsigned long long>(result));
            break;
        default:
            break;
        }

        out = llvm_type(type) + " " + std::to_string(result);
        return true;
    }

    bool CodeGenerator::build_constant_initializer(const AST::Initializer* init,
        const AST::Type& type, std::string& out) {
        if (init == nullptr) { return false; }

        if (auto* expr_init = dynamic_cast<const AST::ExpressionInitializer*>(init)) {
            return build_constant_scalar(expr_init->expr.get(), type, out);
        }

        auto* arr_init = dynamic_cast<const AST::ArrayInitializer*>(init);
        if (arr_init == nullptr) { return false; }

        if (type.kind == TypeKind::Array) {
            if (!type.element_type) { return false; }
            const std::size_t count =
                type.array_size.value_or(arr_init->elements.size());
            std::string text = "[";

            for (std::size_t i = 0; i < count; ++i) {
                if (i != 0) { text += ", "; }

                if (i < arr_init->elements.size()) {
                    std::string element;

                    if (!build_constant_initializer(arr_init->elements[i].get(),
                        *type.element_type, element)) {
                        return false;
                    }

                    text += element;
                } else {
                    text += "zeroinitializer";
                }
            }

            text += "]";
            out = text;
            return true;
        }

        if (type.kind == TypeKind::Struct) {
            auto it = struct_by_name_.find(type.struct_name);
            if (it == struct_by_name_.end() || it->second == nullptr) { return false; }
            AST::StructDefinition* def = it->second;
            if (arr_init->elements.size() > def->members.size()) { return false; }
            std::string text = "{";

            for (std::size_t i = 0; i < def->members.size(); ++i) {
                if (i != 0) { text += ", "; }
                const AST::StructDefinition::Member& member = def->members[i];
                const AST::Initializer* element = i < arr_init->elements.size()
                    ? arr_init->elements[i].get() : member.initializer.get();
                if (element == nullptr) {
                    text += "zeroinitializer";
                    continue;
                }

                std::string value;
                if (!build_constant_initializer(element, member.type, value)) {
                    return false;
                }

                text += value;
            }

            text += "}";
            out = text;
            return true;
        }
        return false;
    }

    bool CodeGenerator::fold_constant_aggregate(AST::VariableDeclaration* decl,
        LocalInfo& info) {
        if (decl == nullptr || decl->initializer == nullptr) { return false; }
        if (decl->type.kind != TypeKind::Array && decl->type.kind != TypeKind::Struct) {
            return false;
        }

        std::string text;
        if (!build_constant_initializer(decl->initializer.get(), decl->type, text)) {
            return false;
        }

        std::string name = "@__sgc_const$" + decl->name + "$" +
            std::to_string(constant_aggregate_counter_++);
        constant_aggregate_globals_.push_back(name +
            " = private unnamed_addr constant " + llvm_type(decl->type) + " " + text);
        info = LocalInfo{};
        info.type = decl->type;
        info.is_constant = true;
        info.address = name;
        return true;
    }

    void CodeGenerator::emit_constant_aggregate_globals() {
        if (constant_aggregate_globals_.empty()) { return; }
        current_label_.clear();

        for (const std::string& line : constant_aggregate_globals_) {
            emit_line(line);
        }

        constant_aggregate_globals_.clear();
    }

    void CodeGenerator::emit_variable_declaration(AST::VariableDeclaration* decl) {
        if (decl->type.is_const) {
            LocalInfo constant;
            if (fold_constant_declaration(decl, constant)) {
                if (scopes_.empty()) { push_scope(); }
                scopes_.back()[decl->name] = std::move(constant);
                return;
            }
        }

        std::string type_text = llvm_type(decl->type);
        std::string address = emit_alloca(type_text, ("alloca_" + decl->name).c_str());
        LocalInfo info;
        info.type = decl->type;
        info.address = address;
        if (scopes_.empty()) { push_scope(); }
        scopes_.back()[decl->name] = std::move(info);
        emit_debug_local_variable(decl->name, decl->type, address, decl->location);

        if (type_contains_string(decl->type)) {
            register_string_cleanup(address, decl->type);
            std::string n = std::to_string(type_size(decl->type));
            emit_line("call void @llvm.memset.p0.i64(ptr " + address +
                ", i8 0, i64 " + n + ", i1 false)");
        } else if (decl->type.kind == TypeKind::Struct) {
            auto it = struct_by_name_.find(decl->type.struct_name);
            if (it != struct_by_name_.end() && it->second != nullptr &&
                it->second->needs_destruction) {
                register_string_cleanup(address, decl->type);
            }
        }

        if (!decl->initializer) {
            if (!decl->constructed_by_lowering && decl->type.kind == TypeKind::Struct) {
                AST::ArrayInitializer empty_init(decl->location,
                    std::vector<std::unique_ptr<AST::Initializer>>{});
                emit_struct_brace_initialization(address, decl->type, &empty_init);
            }

            return;
        }

        if (auto* expr_init = dynamic_cast<AST::ExpressionInitializer*>(decl->initializer.get())) {
            if (auto* cm = dynamic_cast<AST::PrimaryExpression*>(expr_init->expr.get())) {
                if (cm->kind == AST::PrimaryExpression::Kind::CopyMove) {
                    AST::Type source_type;
                    std::string source_address = operand_address(cm->paren_expr.get(),
                        &source_type);

                    if (!source_address.empty()) {
                        switch (cm->copy_move_kind) {
                        case AST::PrimaryExpression::CopyMoveKind::Copy:
                            emit_memberwise_copy(decl->type, address, source_address, false);
                            break;
                        case AST::PrimaryExpression::CopyMoveKind::Move:
                            emit_memberwise_move(decl->type, address, source_address, false);
                            break;
                        case AST::PrimaryExpression::CopyMoveKind::DeepCopy:
                            emit_deep_copy(decl->type, address, source_address);
                            break;
                        case AST::PrimaryExpression::CopyMoveKind::ShallowCopy:
                            emit_shallow_copy(decl->type, address, source_address);
                            break;
                        }

                        return;
                    }
                }
            }

            if (returns_via_sret(decl->type) && is_struct_returning_call(expr_init->expr.get())) {
                pending_sret_destination_ = address;
                gen_expr(expr_init->expr.get());
                pending_sret_destination_.clear();
                return;
            }

            ExprValue value = gen_expr(expr_init->expr.get());
            emit_aggregate_assign(address, decl->type, value);
            return;
        }

        auto* arr_init = dynamic_cast<AST::ArrayInitializer*>(decl->initializer.get());
        if (arr_init && decl->type.kind == TypeKind::Array) {
            emit_array_brace_initialization(address, decl->type, arr_init);
            return;
        }

        if (arr_init && decl->type.kind == TypeKind::Struct) {
            emit_struct_brace_initialization(address, decl->type, arr_init);
            return;
        }
    }

    std::size_t CodeGenerator::type_size(const AST::Type& type) const {
        auto round_up = [](std::size_t v, std::size_t a) -> std::size_t {
            if (a <= 1) { return v; }
            return (v + a - 1) / a * a;
        };
        switch (type.kind) {
        case TypeKind::Int:
        case TypeKind::Uint:
        case TypeKind::Float: return 4;
        case TypeKind::Lint:
        case TypeKind::Luint:
        case TypeKind::Double: return 8;
        case TypeKind::Char:
        case TypeKind::Uchar:
        case TypeKind::Bool: return 1;
        case TypeKind::String: return 32;
        case TypeKind::File: return 8;
        case TypeKind::Pointer:
        case TypeKind::Function: return 8;
        case TypeKind::Void: return 0;
        case TypeKind::Array:
            return type.array_size.value_or(0) *
                (type.element_type ? type_size(*type.element_type) : 0u);
        case TypeKind::Struct: {
            auto it = struct_by_name_.find(type.struct_name);
            if (it == struct_by_name_.end()) { return 0; }
            std::size_t offset = 0;
            std::size_t align = 1;

            for (const auto& m : it->second->members) {
                std::size_t a = type_align(m.type);
                align = std::max(align, a);
                offset = round_up(offset, a);
                offset += type_size(m.type);
            }

            return round_up(offset, align);
        }
        }
        return 0;
    }

    std::size_t CodeGenerator::type_align(const AST::Type& type) const {
        switch (type.kind) {
        case TypeKind::Int:
        case TypeKind::Uint:
        case TypeKind::Float: return 4;
        case TypeKind::Lint:
        case TypeKind::Luint:
        case TypeKind::Double: return 8;
        case TypeKind::Char:
        case TypeKind::Uchar:
        case TypeKind::Bool: return 1;
        case TypeKind::String: return 8;
        case TypeKind::File: return 8;
        case TypeKind::Pointer:
        case TypeKind::Function: return 8;
        case TypeKind::Void: return 1;
        case TypeKind::Array:
            return type.element_type ? type_align(*type.element_type) : 1u;
        case TypeKind::Struct: {
            auto it = struct_by_name_.find(type.struct_name);
            if (it == struct_by_name_.end()) { return 1; }
            std::size_t align = 1;

            for (const auto& m : it->second->members) {
                align = std::max(align, type_align(m.type));
            }

            return align;
        }
        }
        return 1;
    }

}
