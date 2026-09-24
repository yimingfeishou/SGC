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

    void CodeGenerator::emit_destroy_string_at(const AST::Type& type,
        const std::string& address) {
        if (type.kind == TypeKind::String) {
            emit_line("call void @gallt_string_destroy(ptr " + address + ")");
            return;
        }

        if (type.kind == TypeKind::Array && type.element_type) {
            std::string array_ir = llvm_type(type);
            std::size_t n = type.array_size.value_or(0);

            for (std::size_t i = 0; i < n; ++i) {
                std::string elem = new_temp("cleanup_elem");
                emit_line(elem + " = getelementptr " + array_ir +
                    ", ptr " + address + ", i64 0, i64 " + std::to_string(i));
                emit_destroy_string_at(*type.element_type, elem);
            }

            return;
        }

        if (type.kind == TypeKind::Struct) {
            auto it = struct_by_name_.find(type.struct_name);
            if (it == struct_by_name_.end()) { return; }
            std::string struct_ir = llvm_type(type);
            const AST::StructDefinition* def = it->second;

            if (!def->destructor_name.empty()) {
                std::string callee = function_reference(def->destructor_name);
                if (!callee.empty()) {
                    emit_line("call void " + callee + "(ptr " + address + ")");
                }
                return;
            }

            if (lifecycle_owner_ != def) {
                std::string callee = function_reference("__sgc_dtor$" + def->name);
                if (!callee.empty()) {
                    emit_line("call void " + callee + "(ptr " + address + ")");
                    return;
                }
            }

            for (std::size_t i = 0; i < def->members.size(); ++i) {
                std::string field = new_temp("cleanup_field");
                emit_line(field + " = getelementptr " + struct_ir +
                    ", ptr " + address + ", i32 0, i32 " + std::to_string(i));
                emit_destroy_string_at(def->members[i].type, field);
            }
        }
    }

    void CodeGenerator::emit_string_assign(const std::string& dest_address, const ExprValue& source) {
        std::string src_addr = !source.address.empty() ? source.address : source.value;
        emit_line("call void @gallt_string_assign(ptr " + dest_address +
            ", ptr " + src_addr + ")");
    }

    void CodeGenerator::emit_aggregate_assign(const std::string& dest_address,
        const AST::Type& dest_type, ExprValue& source, bool is_assignment) {
        if (dest_type.kind == TypeKind::String) {
            emit_string_assign(dest_address, source);
            destroy_owned_string(source);
            return;
        }

        if (dest_type.kind != TypeKind::Struct && dest_type.kind != TypeKind::Array) {
            std::string converted = convert_value(source.value, source.type, dest_type);
            std::string type_text = llvm_type(dest_type);
            emit_line("store " + type_text + " " + converted + ", ptr " + dest_address);
            return;
        }

        if (dest_type.kind == TypeKind::Array) {
            std::string type_text = llvm_type(dest_type);
            if (!source.address.empty()) {
                std::string loaded = new_temp("arraycopy");
                emit_line(loaded + " = load " + type_text + ", ptr " + source.address);
                emit_line("store " + type_text + " " + loaded + ", ptr " + dest_address);
            } else if (!source.value.empty()) {
                emit_line("store " + type_text + " " + source.value + ", ptr " + dest_address);
            }
            return;
        }

        if (dest_type.kind == TypeKind::Struct) {
            std::string type_text = llvm_type(dest_type);
            std::string source_storage = source.address;
            if (source_storage.empty()) {
                std::string loaded = source.value;
                if (loaded.empty()) { return; }
                source_storage = emit_alloca(type_text, "agg_temp");
                emit_line("store " + type_text + " " + loaded + ", ptr " + source_storage);
            }

            emit_memberwise_copy(dest_type, dest_address, source_storage, is_assignment);
        }
    }

    int CodeGenerator::default_constructor_index(const AST::StructDefinition* def) {
        if (def == nullptr) { return -1; }
        if (def->constructor_names.empty()) { return 0; }

        for (std::size_t i = 0; i < def->constructor_names.size(); ++i) {
            const std::size_t user_params = i < def->constructor_param_types.size()
                ? def->constructor_param_types[i].size() : 0;
            std::vector<AST::Expression*> supplied_defaults;
            collect_constructor_defaults(def->constructor_names[i], supplied_defaults);
            if (supplied_defaults.size() >= user_params) {
                return static_cast<int>(i) + 1;
            }
        }

        return -1;
    }

    bool CodeGenerator::emit_struct_default_constructor(const std::string& address,
        const AST::StructDefinition* def, SourceLocation loc) {
        if (def == nullptr) { return false; }
        if (lifecycle_owner_ == def) { return false; }
        const int ctor_index = default_constructor_index(def);

        if (ctor_index < 0) {
            std::size_t required = 0;

            if (!def->constructor_param_types.empty()) {
                required = def->constructor_param_types.front().size();

                for (const std::vector<AST::Type>& params : def->constructor_param_types) {
                    if (params.size() < required) { required = params.size(); }
                }
            }

            if (diagnostics_ != nullptr) {
                diagnostics_->report_error_template(loc,
                    ErrorCode::FunctionArgCountMismatch,
                    { std::to_string(required), "0" });
            }

            return true;
        }

        if (ctor_index == 0) {
            std::string callee = function_reference("__sgc_ctor$" + def->name);
            if (callee.empty()) { return false; }
            emit_line("call void " + callee + "(ptr " + address + ")");
            return true;
        }

        const std::size_t index = static_cast<std::size_t>(ctor_index - 1);
        const std::string& ctor_name = def->constructor_names[index];
        std::string callee = function_reference(ctor_name);
        if (callee.empty()) { return false; }
        std::vector<AST::Expression*> ctor_args;
        collect_constructor_defaults(ctor_name, ctor_args);
        const std::vector<AST::Type>* params =
            index < def->constructor_param_types.size()
                ? &def->constructor_param_types[index] : nullptr;
        std::string call_text = "call void " + callee + "(ptr " + address;

        for (std::size_t i = 0; i < ctor_args.size(); ++i) {
            ExprValue value = gen_expr(ctor_args[i]);
            AST::Type want = (params != nullptr && i < params->size())
                ? (*params)[i] : value.type;
            if (aggregate_parameter_uses_pointer(want) &&
                (value.type.kind == TypeKind::Struct ||
                    value.type.kind == TypeKind::String)) {
                call_text += ", ptr " + aggregate_argument_pointer(want, value);
            } else {
                call_text += ", " + llvm_type(want) + " " +
                    convert_value(value.value, value.type, want);
            }
            destroy_owned_string(value);
        }

        call_text += ")";
        emit_line(call_text);
        return true;
    }

    void CodeGenerator::emit_struct_brace_initialization(const std::string& address,
        const AST::Type& struct_type, AST::ArrayInitializer* init) {
        auto it = struct_by_name_.find(struct_type.struct_name);
        if (it == struct_by_name_.end() || it->second == nullptr) { return; }
        AST::StructDefinition* def = it->second;

        if (init != nullptr && init->elements.empty()) {
            if (emit_struct_default_constructor(address, def, init->location)) {
                return;
            }
        }

        std::string struct_ir_type = llvm_type(struct_type);

        for (size_t i = 0; i < init->elements.size(); ++i) {
            if (i >= def->members.size()) { break; }
            const AST::StructDefinition::Member& member = def->members[i];
            std::string field_ptr = new_temp("field");
            emit_line(field_ptr + " = getelementptr " + struct_ir_type +
                ", ptr " + address + ", i32 0, i32 " + std::to_string(i));
            AST::Initializer* element = init->elements[i].get();

            if (auto* nested = dynamic_cast<AST::ArrayInitializer*>(element)) {
                if (member.type.kind == TypeKind::Struct) {
                    emit_struct_brace_initialization(field_ptr, member.type, nested);
                } else if (member.type.kind == TypeKind::Array) {
                    emit_array_brace_initialization(field_ptr, member.type, nested);
                }
            } else if (auto* e = dynamic_cast<AST::ExpressionInitializer*>(element)) {
                ExprValue value = gen_expr(e->expr.get());
                emit_aggregate_assign(field_ptr, member.type, value);
            }
        }

        for (size_t i = init->elements.size(); i < def->members.size(); ++i) {
            const AST::StructDefinition::Member& member = def->members[i];
            std::string field_ptr = new_temp("defaultfield");
            emit_line(field_ptr + " = getelementptr " + struct_ir_type +
                ", ptr " + address + ", i32 0, i32 " + std::to_string(i));
            bool handled = false;
            if (member.initializer != nullptr) {
                if (auto* nested = dynamic_cast<AST::ArrayInitializer*>(member.initializer.get())) {
                    if (member.type.kind == TypeKind::Array) {
                        emit_array_brace_initialization(field_ptr, member.type, nested);
                        handled = true;
                    } else if (member.type.kind == TypeKind::Struct) {
                        emit_struct_brace_initialization(field_ptr, member.type, nested);
                        handled = true;
                    }
                } else if (auto* e =
                    dynamic_cast<AST::ExpressionInitializer*>(member.initializer.get())) {
                    ExprValue value = gen_expr(e->expr.get());
                    emit_aggregate_assign(field_ptr, member.type, value);
                    handled = true;
                }
            }

            if (!handled) {
                if (member.type.kind == TypeKind::Struct) {
                    auto member_def = struct_by_name_.find(member.type.struct_name);
                    if (member_def != struct_by_name_.end() &&
                        member_def->second != nullptr) {
                        handled = emit_struct_default_constructor(field_ptr,
                            member_def->second, member.location);
                    }
                }
            }

            if (!handled) {
                emit_line("store " + llvm_type(member.type) +
                    " zeroinitializer, ptr " + field_ptr);
            }
        }
    }

    void CodeGenerator::emit_array_brace_initialization(const std::string& address,
        const AST::Type& array_type, AST::ArrayInitializer* init) {
        if (init == nullptr || !array_type.element_type) { return; }
        const AST::Type& element_type = *array_type.element_type;
        const size_t provided = init->elements.size();
        const size_t count = std::min<size_t>(array_type.array_size.value_or(provided), provided);
        const std::string array_ir = llvm_type(array_type);

        for (size_t i = 0; i < count; ++i) {
            std::string element_ptr = new_temp("arrayelem");
            emit_line(element_ptr + " = getelementptr " + array_ir +
                ", ptr " + address + ", i64 0, i64 " + std::to_string(i));
            AST::Initializer* element = init->elements[i].get();
            if (auto* nested = dynamic_cast<AST::ArrayInitializer*>(element)) {
                if (element_type.kind == TypeKind::Struct) {
                    emit_struct_brace_initialization(element_ptr, element_type, nested);
                } else if (element_type.kind == TypeKind::Array) {
                    emit_array_brace_initialization(element_ptr, element_type, nested);
                }
                continue;
            }

            if (auto* e = dynamic_cast<AST::ExpressionInitializer*>(element)) {
                ExprValue value = gen_expr(e->expr.get());
                emit_aggregate_assign(element_ptr, element_type, value);
            }
        }

        const size_t total = array_type.array_size.value_or(count);

        for (size_t i = count; i < total; ++i) {
            std::string element_ptr = new_temp("arrayfill");
            emit_line(element_ptr + " = getelementptr " + array_ir +
                ", ptr " + address + ", i64 0, i64 " + std::to_string(i));
            if (element_type.kind == TypeKind::Struct) {
                AST::ArrayInitializer empty(init->location,
                    std::vector<std::unique_ptr<AST::Initializer>>{});
                emit_struct_brace_initialization(element_ptr, element_type, &empty);
            } else {
                emit_line("store " + llvm_type(element_type) +
                    " zeroinitializer, ptr " + element_ptr);
            }
        }
    }

}
