#include "codegen.hpp"
#include "codegen_detail.hpp"
#include "../semantic/constant_folding.hpp"
#include "../semantic/lifecycle_transfer.hpp"
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

            for (std::size_t i = n; i-- > 0;) {
                std::string elem = new_temp("cleanup_elem");
                emit_line(elem + " = getelementptr " + array_ir +
                    ", ptr " + address + ", i64 0, i64 " + std::to_string(i));
                emit_destroy_string_at(*type.element_type, elem);
            }

            return;
        }

        if (type.kind == TypeKind::Struct) {
            const AST::StructDefinition* def = lookup_struct_definition(type.struct_name);
            if (def == nullptr) { return; }
            std::string struct_ir = llvm_type(type);
            require_lifecycle_functions(def);

            if (!def->destructor_name.empty()) {
                std::string callee = function_reference(def->destructor_name);
                if (!callee.empty()) {
                    emit_line("call void " + callee + "(ptr " + address + ")");
                }
                return;
            }

            if (lifecycle_owner_ != def) {
                std::string callee = function_reference(
                    lifecycle_symbol(def, LifecycleKind::Destructor));
                if (!callee.empty()) {
                    emit_line("call void " + callee + "(ptr " + address + ")");
                    return;
                }
            }

            for (std::size_t i = def->members.size(); i-- > 0;) {
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
        const AST::Type& dest_type, ExprValue& source, bool is_assignment,
        bool prefer_move) {
        if (source.value.empty() && source.address.empty()) {
            if (diagnostics_ != nullptr) {
                diagnostics_->report_error(SourceLocation{},
                    ErrorCode::ExpressionSyntaxError,
                    "aggregate assignment source cannot be materialized");
            }
            return;
        }

        if (dest_type.kind == TypeKind::String) {
            emit_string_assign(dest_address, source);
            destroy_owned_string(source);
            return;
        }

        if (dest_type.kind != TypeKind::Struct && dest_type.kind != TypeKind::Array) {
            if (source.value.empty()) {
                if (diagnostics_ != nullptr) {
                    diagnostics_->report_error(SourceLocation{},
                        ErrorCode::ExpressionSyntaxError,
                        "aggregate assignment source has no value");
                }
                return;
            }
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
            if (!materialize_value_view(source, "agg_temp")) {
                if (diagnostics_ != nullptr) {
                    diagnostics_->report_error(SourceLocation{},
                        ErrorCode::ExpressionSyntaxError,
                        "aggregate assignment source cannot be materialized");
                }
                return;
            }

            if (prefer_move) {
                emit_memberwise_move(dest_type, dest_address, source.address, is_assignment);
            } else {
                emit_memberwise_copy(dest_type, dest_address, source.address, is_assignment);
            }
        }
    }

    int CodeGenerator::default_constructor_index(const AST::StructDefinition* def) {
        if (def == nullptr) { return -1; }
        if (def->constructor_names.empty()) { return 0; }

        for (std::size_t i = 0; i < def->constructor_names.size(); ++i) {
            const std::size_t user_params = i < def->constructor_param_types.size()
                ? def->constructor_param_types[i].size() : 0;
            AST::FunctionDefinition* ctor = constructor_function_for(def,
                i < def->constructor_param_types.size()
                    ? def->constructor_param_types[i]
                    : std::vector<AST::Type>{});
            std::vector<AST::Expression*> supplied_defaults;
            if (ctor != nullptr) {
                collect_constructor_defaults(ctor->name, supplied_defaults);
            }
            if (supplied_defaults.size() >= user_params) {
                return static_cast<int>(i) + 1;
            }
        }

        return -1;
    }

    AST::FunctionDefinition* CodeGenerator::constructor_function_for(
        const AST::StructDefinition* def,
        const std::vector<AST::Type>& user_parameters) const {
        if (def == nullptr) { return nullptr; }

        for (const auto& top : program_->top_levels) {
            auto* function = dynamic_cast<AST::FunctionDefinition*>(top.get());

            if (function == nullptr || function->owning_struct != def ||
                function->name.rfind("__sgc_ctor$", 0) != 0 ||
                function->parameters.empty() ||
                function->parameters.size() - 1 != user_parameters.size()) {
                continue;
            }

            bool matches = true;

            for (std::size_t i = 0; i < user_parameters.size(); ++i) {
                if (!(function->parameters[i + 1] == user_parameters[i])) {
                    matches = false;
                    break;
                }
            }

            if (matches) { return function; }
        }

        return nullptr;
    }

    bool CodeGenerator::emit_struct_default_constructor(const std::string& address,
        const AST::StructDefinition* def, SourceLocation loc) {
        if (def == nullptr) { return false; }
        require_lifecycle_functions(def);
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
            std::string callee = function_reference(
                lifecycle_symbol(def, LifecycleKind::Constructor));
            if (callee.empty()) { return false; }
            emit_line("call void " + callee + "(ptr " + address + ")");
            return true;
        }

        const std::size_t index = static_cast<std::size_t>(ctor_index - 1);
        const std::string& ctor_name = def->constructor_names[index];
        const std::vector<AST::Type>* params =
            index < def->constructor_param_types.size()
                ? &def->constructor_param_types[index] : nullptr;
        AST::FunctionDefinition* ctor = constructor_function_for(def,
            params != nullptr ? *params : std::vector<AST::Type>{});
        std::string callee = ctor != nullptr ? function_reference(ctor->name)
            : function_reference(ctor_name);
        if (callee.empty()) {
            callee = source_function_symbol(ctor != nullptr ? ctor->name : ctor_name);
        }
        std::vector<AST::Expression*> ctor_args;
        collect_constructor_defaults(ctor != nullptr ? ctor->name : ctor_name,
            ctor_args);
        std::string call_text = "call void " + callee + "(ptr " + address;

        for (std::size_t i = 0; i < ctor_args.size(); ++i) {
            bool explicit_move = false;
            bool explicit_copy = false;
            AST::Expression* argument = unwrap_copy_move_argument(ctor_args[i],
                explicit_move, explicit_copy);
            ExprValue value = gen_expr(argument);
            AST::Type want = (params != nullptr && i < params->size())
                ? (*params)[i] : value.type;
            if (aggregate_parameter_uses_pointer(want) &&
                (value.type.kind == TypeKind::Struct ||
                    value.type.kind == TypeKind::String)) {
                call_text += ", ptr " + aggregate_call_argument(want, value,
                    !explicit_copy &&
                        (explicit_move || !expression_is_lvalue(argument)));
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

    void CodeGenerator::emit_explicit_constructor_call(
        const std::string& address, const std::string& ctor_name,
        std::vector<AST::Expression*>& arguments, SourceLocation loc) {
        std::string callee = function_reference(ctor_name);
        if (callee.empty()) {
            callee = source_function_symbol(ctor_name);
        }
        AST::FunctionDefinition* ctor_def = nullptr;
        auto found = function_by_name_.find(ctor_name);
        if (found != function_by_name_.end()) {
            ctor_def = found->second;
        }
        std::vector<AST::Expression*> call_arguments = arguments;
        collect_constructor_defaults(ctor_name, call_arguments);
        std::string call_text = "call void " + callee + "(ptr " + address;
        std::vector<std::string> owned_arguments;

        for (std::size_t i = 0; i < call_arguments.size(); ++i) {
            bool explicit_move = false;
            bool explicit_copy = false;
            AST::Expression* argument = unwrap_copy_move_argument(
                call_arguments[i], explicit_move, explicit_copy);
            ExprValue value = gen_expr(argument);
            AST::Type want = value.type;
            const std::size_t parameter_index = i + 1;

            if (ctor_def != nullptr &&
                parameter_index < ctor_def->parameters.size()) {
                want = ctor_def->parameters[parameter_index];
            }

            if (aggregate_parameter_uses_pointer(want) &&
                (value.type.kind == TypeKind::Struct ||
                    value.type.kind == TypeKind::String)) {
                call_text += ", ptr " + aggregate_call_argument(want, value,
                    !explicit_copy &&
                        (explicit_move || !expression_is_lvalue(argument)));
            } else {
                call_text += ", " + llvm_type(want) + " " +
                    convert_value(value.value, value.type, want);
            }

            if (!value.owned_string.empty()) {
                owned_arguments.push_back(value.owned_string);
            }
        }

        call_text += ")";
        emit_line(call_text);
        for (const std::string& owned : owned_arguments) {
            emit_line("call void @gallt_string_destroy(ptr " + owned + ")");
        }
        (void)loc;
    }

    void CodeGenerator::emit_struct_brace_initialization(const std::string& address,
        const AST::Type& struct_type, AST::ArrayInitializer* init) {
        require_lifecycle_functions(struct_type);

        AST::StructDefinition* def = const_cast<AST::StructDefinition*>(
            lookup_struct_definition(struct_type.struct_name));
        if (def == nullptr) { return; }

        if (init != nullptr && !def->constructor_names.empty()) {
            std::vector<AST::Expression*> arguments;
            bool expressions_only = true;

            for (const auto& element : init->elements) {
                auto* expression = dynamic_cast<AST::ExpressionInitializer*>(
                    element.get());
                if (expression == nullptr) {
                    expressions_only = false;
                    break;
                }
                arguments.push_back(expression->expr.get());
            }

            if (expressions_only && !init->lowered_ctor.empty()) {
                emit_explicit_constructor_call(address, init->lowered_ctor,
                    arguments, init->location);
                return;
            }
        }

        if (init != nullptr && init->elements.empty() &&
            def->constructor_names.empty()) {
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
                emit_aggregate_assign(field_ptr, member.type, value, false,
                    lifecycle_transfer::is_move_source(e->expr.get()));
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
                    emit_aggregate_assign(field_ptr, member.type, value, false,
                        lifecycle_transfer::is_move_source(e->expr.get()));
                    handled = true;
                }
            }

            if (!handled) {
                if (member.type.kind == TypeKind::Struct) {
                    const AST::StructDefinition* member_def =
                        lookup_struct_definition(member.type.struct_name);
                    if (member_def != nullptr &&
                        default_constructor_index(member_def) >= 0) {
                        handled = emit_struct_default_constructor(field_ptr,
                            member_def, member.location);
                    }
                }
            }

            if (!handled) {
                emit_line("store " + llvm_type(member.type) +
                    " zeroinitializer, ptr " + field_ptr);
            }
        }
    }

    void CodeGenerator::emit_constructor_member_defaults(const std::string& address,
        const AST::StructDefinition* def) {
        if (def == nullptr) { return; }

        const AST::Type struct_type = AST::Type::make_struct(def->name);
        const std::string struct_ir_type = llvm_type(struct_type);

        for (std::size_t i = 0; i < def->members.size(); ++i) {
            const AST::StructDefinition::Member& member = def->members[i];
            std::string field_ptr = new_temp("constructor_default");
            emit_line(field_ptr + " = getelementptr " + struct_ir_type +
                ", ptr " + address + ", i32 0, i32 " + std::to_string(i));

            if (member.initializer != nullptr) {
                if (auto* nested = dynamic_cast<AST::ArrayInitializer*>(
                    member.initializer.get())) {
                    if (member.type.kind == TypeKind::Array) {
                        emit_array_brace_initialization(field_ptr, member.type, nested);
                    } else if (member.type.kind == TypeKind::Struct) {
                        emit_struct_brace_initialization(field_ptr, member.type, nested);
                    }
                } else if (auto* expression = dynamic_cast<AST::ExpressionInitializer*>(
                    member.initializer.get())) {
                    ExprValue value = gen_expr(expression->expr.get());
                    emit_aggregate_assign(field_ptr, member.type, value, false,
                        lifecycle_transfer::is_move_source(expression->expr.get()));
                }
                continue;
            }

            if (member.type.kind == TypeKind::Struct) {
                const AST::StructDefinition* found =
                    lookup_struct_definition(member.type.struct_name);

                if (found != nullptr &&
                    default_constructor_index(found) >= 0) {
                    emit_struct_default_constructor(field_ptr, found,
                        member.location);
                }
            }
        }
    }

    void CodeGenerator::emit_array_brace_initialization(const std::string& address,
        const AST::Type& array_type, AST::ArrayInitializer* init) {
        if (init == nullptr || !array_type.element_type) { return; }
        require_lifecycle_functions(array_type);
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
                emit_aggregate_assign(element_ptr, element_type, value, false,
                    lifecycle_transfer::is_move_source(e->expr.get()));
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
