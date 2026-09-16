#include "codegen.hpp"
#include <algorithm>

using namespace gallt::AST;

namespace gallt {

    bool CodeGenerator::type_is_copyable(const AST::Type& type) const {
        switch (type.kind) {
        case TypeKind::Array:
            return type.element_type ? type_is_copyable(*type.element_type) : true;
        case TypeKind::Struct: {
            auto it = struct_by_name_.find(type.struct_name);
            if (it == struct_by_name_.end() || it->second == nullptr) return true;
            if (it->second->no_copy) return false;
            for (const auto& member : it->second->members) {
                if (!type_is_copyable(member.type)) return false;
            }
            return true;
        }
        default:
            return true;
        }
    }

    bool CodeGenerator::type_is_movable(const AST::Type& type) const {
        switch (type.kind) {
        case TypeKind::Array:
            return type.element_type ? type_is_movable(*type.element_type) : true;
        case TypeKind::Struct: {
            auto it = struct_by_name_.find(type.struct_name);
            if (it == struct_by_name_.end() || it->second == nullptr) return true;
            if (it->second->no_move) return false;
            for (const auto& member : it->second->members) {
                if (!type_is_movable(member.type)) return false;
            }
            return true;
        }
        default:
            return true;
        }
    }

    bool CodeGenerator::deep_copyable_pointee(const AST::Type& pointer_type) const {
        if (pointer_type.kind != TypeKind::Pointer || !pointer_type.pointee_type) return false;
        const AST::Type& pointee = *pointer_type.pointee_type;
        if (pointee.kind == TypeKind::Void || pointee.kind == TypeKind::Function) return false;
        return type_size(pointee) > 0;
    }


    void CodeGenerator::emit_shallow_copy(const AST::Type& type, const std::string& dst,
        const std::string& src) {
        std::size_t size = type_size(type);
        if (size == 0) return;
        emit_line("call void @llvm.memcpy.p0.p0.i64(ptr " + dst + ", ptr " + src +
            ", i64 " + std::to_string(size) + ", i1 false)");
    }


    void CodeGenerator::emit_memberwise_copy(const AST::Type& type, const std::string& dst,
        const std::string& src, bool is_assignment) {
        if (type.kind == TypeKind::String) {
            emit_line("call void @gallt_string_assign(ptr " + dst + ", ptr " + src + ")");
            return;
        }
        if (type.kind == TypeKind::Array && type.element_type) {
            std::size_t count = type.array_size.value_or(0);
            std::string ir = llvm_type(type);
            for (std::size_t i = 0; i < count; ++i) {
                std::string element_dst = new_temp("cp_dst");
                std::string element_src = new_temp("cp_src");
                emit_line(element_dst + " = getelementptr " + ir + ", ptr " + dst +
                    ", i64 0, i64 " + std::to_string(i));
                emit_line(element_src + " = getelementptr " + ir + ", ptr " + src +
                    ", i64 0, i64 " + std::to_string(i));
                emit_memberwise_copy(*type.element_type, element_dst, element_src,
                    is_assignment);
            }
            return;
        }
        if (type.kind == TypeKind::Struct) {
            auto it = struct_by_name_.find(type.struct_name);
            if (it != struct_by_name_.end() && it->second != nullptr) {
                const AST::StructDefinition* def = it->second;
                const std::string& user_fn = is_assignment ? def->copy_assignment_name
                                                           : def->copy_constructor_name;
                if (!user_fn.empty()) {
                    std::string callee = function_reference(user_fn);
                    if (!callee.empty()) {
                        emit_line("call void " + callee + "(ptr " + dst + ", ptr " + src + ")");
                        return;
                    }
                }
                std::string ir = llvm_type(type);
                for (std::size_t i = 0; i < def->members.size(); ++i) {
                    std::string field_dst = new_temp("cp_field_dst");
                    std::string field_src = new_temp("cp_field_src");
                    emit_line(field_dst + " = getelementptr " + ir + ", ptr " + dst +
                        ", i32 0, i32 " + std::to_string(i));
                    emit_line(field_src + " = getelementptr " + ir + ", ptr " + src +
                        ", i32 0, i32 " + std::to_string(i));
                    emit_memberwise_copy(def->members[i].type, field_dst, field_src,
                        is_assignment);
                }
                return;
            }
        }
        std::string type_text = llvm_type(type);
        std::string value = new_temp("cp_load");
        emit_line(value + " = load " + type_text + ", ptr " + src);
        emit_line("store " + type_text + " " + value + ", ptr " + dst);
    }

    void CodeGenerator::emit_memberwise_move(const AST::Type& type, const std::string& dst,
        const std::string& src, bool is_assignment) {
        if (type.kind == TypeKind::String) {
            emit_line("call void @gallt_string_destroy(ptr " + dst + ")");
            std::string value = new_temp("mv_string");
            emit_line(value + " = load %struct.gallt.string, ptr " + src);
            emit_line("store %struct.gallt.string " + value + ", ptr " + dst);
            emit_line("call void @llvm.memset.p0.i64(ptr " + src +
                ", i8 0, i64 32, i1 false)");
            return;
        }
        if (type.kind == TypeKind::Array && type.element_type) {
            std::size_t count = type.array_size.value_or(0);
            std::string ir = llvm_type(type);
            for (std::size_t i = 0; i < count; ++i) {
                std::string element_dst = new_temp("mv_dst");
                std::string element_src = new_temp("mv_src");
                emit_line(element_dst + " = getelementptr " + ir + ", ptr " + dst +
                    ", i64 0, i64 " + std::to_string(i));
                emit_line(element_src + " = getelementptr " + ir + ", ptr " + src +
                    ", i64 0, i64 " + std::to_string(i));
                emit_memberwise_move(*type.element_type, element_dst, element_src,
                    is_assignment);
            }
            return;
        }
        if (type.kind == TypeKind::Struct) {
            auto it = struct_by_name_.find(type.struct_name);
            if (it != struct_by_name_.end() && it->second != nullptr) {
                const AST::StructDefinition* def = it->second;
                const std::string& user_fn = is_assignment ? def->move_assignment_name
                                                           : def->move_constructor_name;
                if (!user_fn.empty()) {
                    std::string callee = function_reference(user_fn);
                    if (!callee.empty()) {
                        emit_line("call void " + callee + "(ptr " + dst + ", ptr " + src + ")");
                        return;
                    }
                }
                std::string ir = llvm_type(type);
                for (std::size_t i = 0; i < def->members.size(); ++i) {
                    std::string field_dst = new_temp("mv_field_dst");
                    std::string field_src = new_temp("mv_field_src");
                    emit_line(field_dst + " = getelementptr " + ir + ", ptr " + dst +
                        ", i32 0, i32 " + std::to_string(i));
                    emit_line(field_src + " = getelementptr " + ir + ", ptr " + src +
                        ", i32 0, i32 " + std::to_string(i));
                    emit_memberwise_move(def->members[i].type, field_dst, field_src,
                        is_assignment);
                }
                return;
            }
        }
        if (type.kind == TypeKind::Pointer || type.kind == TypeKind::Function ||
            type.kind == TypeKind::File) {
            std::string value = new_temp("mv_ptr");
            emit_line(value + " = load ptr, ptr " + src);
            emit_line("store ptr " + value + ", ptr " + dst);
            emit_line("store ptr null, ptr " + src);
            return;
        }
        std::string type_text = llvm_type(type);
        std::string value = new_temp("mv_load");
        emit_line(value + " = load " + type_text + ", ptr " + src);
        emit_line("store " + type_text + " " + value + ", ptr " + dst);
    }

    void CodeGenerator::emit_deep_copy(const AST::Type& type, const std::string& dst,
        const std::string& src) {
        if (type.kind == TypeKind::String) {
            emit_line("call void @gallt_string_assign(ptr " + dst + ", ptr " + src + ")");
            return;
        }
        if (type.kind == TypeKind::Array && type.element_type) {
            std::size_t count = type.array_size.value_or(0);
            std::string ir = llvm_type(type);
            for (std::size_t i = 0; i < count; ++i) {
                std::string element_dst = new_temp("dc_dst");
                std::string element_src = new_temp("dc_src");
                emit_line(element_dst + " = getelementptr " + ir + ", ptr " + dst +
                    ", i64 0, i64 " + std::to_string(i));
                emit_line(element_src + " = getelementptr " + ir + ", ptr " + src +
                    ", i64 0, i64 " + std::to_string(i));
                emit_deep_copy(*type.element_type, element_dst, element_src);
            }
            return;
        }
        if (type.kind == TypeKind::Pointer) {
            if (!deep_copyable_pointee(type)) {
                std::string value = new_temp("dc_ptr");
                emit_line(value + " = load ptr, ptr " + src);
                emit_line("store ptr " + value + ", ptr " + dst);
                return;
            }
            std::string original = new_temp("dc_src_ptr");
            emit_line(original + " = load ptr, ptr " + src);
            std::string is_null = new_temp("dc_is_null");
            emit_line(is_null + " = icmp eq ptr " + original + ", null");
            std::string null_label = new_label("dc_null");
            std::string copy_label = new_label("dc_alloc");
            std::string end_label = new_label("dc_end");
            emit_line("br i1 " + is_null + ", label %" + null_label + ", label %" + copy_label);
            start_block(null_label);
            emit_line("store ptr null, ptr " + dst);
            emit_line("br label %" + end_label);
            start_block(copy_label);
            std::size_t pointee_size = type_size(*type.pointee_type);
            std::string allocated = new_temp("dc_alloc_ptr");
            emit_line(allocated + " = call ptr @gallt_alloc_bytes(i64 " +
                std::to_string(pointee_size) + ")");
            emit_deep_copy(*type.pointee_type, allocated, original);
            emit_line("store ptr " + allocated + ", ptr " + dst);
            emit_line("br label %" + end_label);
            start_block(end_label);
            return;
        }
        if (type.kind == TypeKind::Struct) {
            auto it = struct_by_name_.find(type.struct_name);
            if (it != struct_by_name_.end() && it->second != nullptr) {
                std::string ir = llvm_type(type);
                for (std::size_t i = 0; i < it->second->members.size(); ++i) {
                    std::string field_dst = new_temp("dc_field_dst");
                    std::string field_src = new_temp("dc_field_src");
                    emit_line(field_dst + " = getelementptr " + ir + ", ptr " + dst +
                        ", i32 0, i32 " + std::to_string(i));
                    emit_line(field_src + " = getelementptr " + ir + ", ptr " + src +
                        ", i32 0, i32 " + std::to_string(i));
                    emit_deep_copy(it->second->members[i].type, field_dst, field_src);
                }
                return;
            }
        }
        std::string type_text = llvm_type(type);
        std::string value = new_temp("dc_load");
        emit_line(value + " = load " + type_text + ", ptr " + src);
        emit_line("store " + type_text + " " + value + ", ptr " + dst);
    }

    std::string CodeGenerator::operand_address(AST::Expression* expr, AST::Type* out_type) {
        if (expr == nullptr) return std::string();
        AST::Type type = resolved_type(expr);
        if (out_type != nullptr) *out_type = type;
        std::string address = gen_address(expr);
        if (!address.empty()) return address;
        ExprValue value = gen_expr(expr);
        if (out_type != nullptr) *out_type = value.type;
        if (!value.address.empty()) return value.address;
        std::string storage = emit_alloca(llvm_type(value.type), "operand_tmp");
        emit_line("store " + llvm_type(value.type) + " " + value.value + ", ptr " + storage);
        return storage;
    }

    void CodeGenerator::collect_constructor_defaults(const std::string& ctor_name,
        std::vector<AST::Expression*>& args) {
        auto it = function_by_name_.find(ctor_name);
        if (it == function_by_name_.end() || it->second == nullptr) return;
        AST::FunctionDefinition* def = it->second;
        const std::size_t arg_offset = def->parameters.empty() ? 0 : 1;
        const std::size_t total_args = def->parameters.size() - arg_offset;
        if (args.size() >= total_args) return;
        for (std::size_t j = args.size(); j < total_args; ++j) {
            const std::size_t param_index = j + arg_offset;
            if (param_index < def->param_defaults.size() &&
                def->param_defaults[param_index] != nullptr) {
                args.push_back(def->param_defaults[param_index].get());
            }
        }
    }

    bool CodeGenerator::is_struct_returning_call(AST::Expression* expr) const {
        auto* call = dynamic_cast<AST::PostfixExpression*>(expr);
        if (call == nullptr ||
            call->op != AST::PostfixExpression::Operator::FunctionCall) {
            return false;
        }
        auto* base = dynamic_cast<AST::PrimaryExpression*>(call->base.get());
        if (base == nullptr || base->kind != AST::PrimaryExpression::Kind::Identifier) {
            return false;
        }
        if (extern_by_name_.find(base->identifier) != extern_by_name_.end()) {
            return false;   
        }
        auto it = function_by_name_.find(base->identifier);
        if (it == function_by_name_.end() || it->second == nullptr) return false;
        return returns_via_sret(it->second->return_type);
    }

    void CodeGenerator::emit_struct_return(AST::Expression* expr, const AST::Type& type,
        const std::string& sret) {
        AST::Expression* source_expr = expr;
        bool is_move = false;
        if (auto* cm = dynamic_cast<AST::PrimaryExpression*>(expr)) {
            if (cm->kind == AST::PrimaryExpression::Kind::CopyMove) {
                is_move = cm->copy_move_kind == AST::PrimaryExpression::CopyMoveKind::Move;
                source_expr = cm->paren_expr.get();
            }
        }
        AST::Type source_type;
        std::string source_address = operand_address(source_expr, &source_type);
        if (source_address.empty()) return;

        AST::StructDefinition* def = nullptr;
        auto struct_it = struct_by_name_.find(type.struct_name);
        if (struct_it != struct_by_name_.end()) def = struct_it->second;

        if (is_move && def != nullptr && !def->move_constructor_name.empty()) {
            std::string callee = function_reference(def->move_constructor_name);
            if (!callee.empty()) {
                emit_line("call void " + callee + "(ptr " + sret + ", ptr " +
                    source_address + ")");
                return;
            }
        }
        if (!is_move && def != nullptr && !def->copy_constructor_name.empty()) {
            std::string callee = function_reference(def->copy_constructor_name);
            if (!callee.empty()) {
                emit_line("call void " + callee + "(ptr " + sret + ", ptr " +
                    source_address + ")");
                return;
            }
        }
        if (is_move) {
            emit_memberwise_move(type, sret, source_address, false);
        }
        else {
            emit_memberwise_copy(type, sret, source_address, false);
        }
    }

} 
