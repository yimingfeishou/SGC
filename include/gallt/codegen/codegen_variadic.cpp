#include "codegen.hpp"
#include "codegen_detail.hpp"
#include <string>
#include <vector>

using namespace gallt::AST;

namespace gallt {

    const CodeGenerator::VariadicPackLocal* CodeGenerator::find_variadic_local(
        const std::string& name) const {
        for (auto it = variadic_locals_.rbegin(); it != variadic_locals_.rend(); ++it) {
            if (it->name == name) {
                return &(*it);
            }
        }

        return nullptr;
    }

    bool CodeGenerator::pack_identifier_base(const AST::Expression* expr,
        std::string& out) {
        auto* prim = dynamic_cast<const AST::PrimaryExpression*>(expr);

        if (prim == nullptr ||
            prim->kind != AST::PrimaryExpression::Kind::Identifier) {
            return false;
        }

        out = prim->identifier;
        return true;
    }

    std::string CodeGenerator::zero_initializer_text(const AST::Type& type) {
        switch (type.kind) {
        case TypeKind::Float:
        case TypeKind::Double:
            return "0.0";
        case TypeKind::Pointer:
        case TypeKind::Function:
            return "null";
        case TypeKind::Struct:
        case TypeKind::String:
        case TypeKind::Array:
            return "zeroinitializer";
        default:
            return "0";
        }
    }

    std::string CodeGenerator::pack_data_value(const VariadicPackLocal& pack) {
        std::string value = new_temp("packdata");
        emit_line(value + " = load ptr, ptr " + pack.data_slot);
        return value;
    }

    std::string CodeGenerator::pack_length_value(const VariadicPackLocal& pack) {
        std::string value = new_temp("packlen");
        emit_line(value + " = load i32, ptr " + pack.length_slot);
        return value;
    }

    std::string CodeGenerator::pack_element_address(const VariadicPackLocal& pack,
        const std::string& index) {
        std::string data = pack_data_value(pack);
        std::string element = llvm_type(pack.element);
        std::string pointer = new_temp("packelemptr");
        emit_line(pointer + " = getelementptr " + element + ", ptr " + data +
            ", i32 " + index);
        return pointer;
    }

    CodeGenerator::ExprValue CodeGenerator::pack_guarded_element(
        const VariadicPackLocal& pack, const std::string& index) {
        ExprValue out;
        out.type = pack.element;
        std::string element = llvm_type(pack.element);
        std::string storage = emit_alloca(element, "packresult");
        emit_line("store " + element + " " + zero_initializer_text(pack.element) +
            ", ptr " + storage);
        std::string length = pack_length_value(pack);
        std::string lower = new_temp("packlow");
        emit_line(lower + " = icmp sge i32 " + index + ", 0");
        std::string upper = new_temp("packhigh");
        emit_line(upper + " = icmp slt i32 " + index + ", " + length);
        std::string in_range = new_temp("packinrange");
        emit_line(in_range + " = and i1 " + lower + ", " + upper);
        std::string load_label = new_label("packload");
        std::string end_label = new_label("packend");
        emit_line("br i1 " + in_range + ", label %" + load_label + ", label %" +
            end_label);
        start_block(load_label);
        std::string address = pack_element_address(pack, index);
        std::string loaded = new_temp("packvalue");
        emit_line(loaded + " = load " + element + ", ptr " + address);
        emit_line("store " + element + " " + loaded + ", ptr " + storage);
        emit_line("br label %" + end_label);
        start_block(end_label);
        out.value = new_temp("packelement");
        emit_line(out.value + " = load " + element + ", ptr " + storage);
        out.address = storage;
        out.is_lvalue = true;
        return out;
    }

    CodeGenerator::ExprValue CodeGenerator::gen_pack_subscript(
        AST::PostfixExpression* expr, const VariadicPackLocal& pack) {
        ExprValue out;
        out.type = pack.element;

        if (expr->subscript_expr == nullptr) {
            return out;
        }

        ExprValue index = gen_expr(expr->subscript_expr.get());
        std::string index_text = convert_value(index.value, index.type,
            AST::Type::make_int());
        std::string address = pack_element_address(pack, index_text);
        out.address = address;
        out.is_lvalue = true;
        out.value = new_temp("packsubscript");
        emit_line(out.value + " = load " + llvm_type(pack.element) + ", ptr " +
            address);
        return out;
    }

    CodeGenerator::ExprValue CodeGenerator::gen_pack_property(
        AST::PostfixExpression* expr, const VariadicPackLocal& pack) {
        ExprValue out;

        if (expr->member_name == "length") {
            out.type = AST::Type::make_int();
            out.value = pack_length_value(pack);
            return out;
        }

        if (expr->member_name == "empty") {
            out.type = AST::Type::make_bool();
            std::string length = pack_length_value(pack);
            std::string flag = new_temp("packempty");
            emit_line(flag + " = icmp eq i32 " + length + ", 0");
            out.value = new_temp("packemptyi8");
            emit_line(out.value + " = zext i1 " + flag + " to i8");
            return out;
        }

        if (expr->member_name == "data") {
            out.type = AST::Type::make_pointer(
                std::make_shared<AST::Type>(pack.element));
            out.value = pack_data_value(pack);
            return out;
        }

        if (expr->member_name == "first") {
            return pack_guarded_element(pack, "0");
        }

        if (expr->member_name == "last") {
            std::string length = pack_length_value(pack);
            std::string index = new_temp("packlast");
            emit_line(index + " = sub i32 " + length + ", 1");
            return pack_guarded_element(pack, index);
        }

        out.type = AST::Type::make_void();
        return out;
    }

    bool CodeGenerator::gen_variadic_call_arguments(
        std::vector<AST::Expression*>& all_args,
        const std::vector<AST::Type>& params, std::vector<std::string>& ir_args,
        std::vector<std::string>& ir_arg_types,
        std::vector<std::string>& owned_args, std::string& heap_storage) {
        const std::size_t fixed = params.size() - 1;
        const AST::Type& element = params.back();
        const std::string element_type = llvm_type(element);
        heap_storage.clear();

        for (std::size_t i = 0; i < all_args.size() && i < fixed; ++i) {
            ExprValue arg = gen_expr(all_args[i]);
            if (!arg.owned_string.empty()) {
                owned_args.push_back(arg.owned_string);
            }
            const bool aggregate_argument =
                aggregate_parameter_uses_pointer(params[i]) &&
                (arg.type.kind == TypeKind::Struct ||
                    arg.type.kind == TypeKind::String);

            if (aggregate_argument) {
                ir_args.push_back(aggregate_argument_pointer(params[i], arg));
            } else {
                ir_args.push_back(convert_value(arg.value, arg.type, params[i]));
            }
            ir_arg_types.push_back(parameter_ir_type(params[i]));
        }

        const std::size_t tail = all_args.size() > fixed
            ? all_args.size() - fixed : 0;

        auto expansion_source = [&](std::size_t index) -> const VariadicPackLocal* {
            auto* post = dynamic_cast<AST::PostfixExpression*>(all_args[index]);
            if (post == nullptr ||
                post->op != AST::PostfixExpression::Operator::PackExpand) {
                return nullptr;
            }

            std::string name;
            if (!pack_identifier_base(post->base.get(), name)) {
                return nullptr;
            }

            return find_variadic_local(name);
        };

        std::size_t expansion_count = 0;

        for (std::size_t k = 0; k < tail; ++k) {
            if (expansion_source(fixed + k) != nullptr) {
                ++expansion_count;
            }
        }

        if (tail == 0) {
            ir_args.push_back("null");
            ir_args.push_back("0");
            ir_arg_types.push_back("ptr");
            ir_arg_types.push_back("i32");
            return true;
        }

        if (tail == 1 && expansion_count == 1) {
            const VariadicPackLocal* source = expansion_source(fixed);
            ir_args.push_back(pack_data_value(*source));
            ir_args.push_back(pack_length_value(*source));
            ir_arg_types.push_back("ptr");
            ir_arg_types.push_back("i32");
            return true;
        }

        if (expansion_count == 0) {
            std::string storage = emit_alloca(
                "[" + std::to_string(tail) + " x " + element_type + "]",
                "packstorage");

            for (std::size_t i = 0; i < tail; ++i) {
                std::string slot = new_temp("packslot");
                emit_line(slot + " = getelementptr [" + std::to_string(tail) +
                    " x " + element_type + "], ptr " + storage + ", i32 0, i32 " +
                    std::to_string(i));
                store_pack_element(element, slot, all_args[fixed + i], owned_args);
            }

            ir_args.push_back(storage);
            ir_args.push_back(std::to_string(tail));
            ir_arg_types.push_back("ptr");
            ir_arg_types.push_back("i32");
            return true;
        }

        const std::size_t element_size = type_size(element);
        const std::string element_bytes = std::to_string(element_size == 0 ? 1
            : element_size);
        std::string total = "0";

        for (std::size_t k = 0; k < tail; ++k) {
            const VariadicPackLocal* source = expansion_source(fixed + k);
            std::string sum = new_temp("packtotal");

            if (source == nullptr) {
                emit_line(sum + " = add i32 " + total + ", 1");
            } else {
                emit_line(sum + " = add i32 " + total + ", " +
                    pack_length_value(*source));
            }
            total = sum;
        }

        std::string total64 = new_temp("packtotal64");
        emit_line(total64 + " = sext i32 " + total + " to i64");
        std::string bytes = new_temp("packbytes");
        emit_line(bytes + " = mul i64 " + total64 + ", " + element_bytes);
        std::string base = new_temp("packheap");
        emit_line(base + " = call ptr @gallt_alloc_bytes(i64 " + bytes + ")");
        std::string offset = "0";

        for (std::size_t k = 0; k < tail; ++k) {
            const VariadicPackLocal* source = expansion_source(fixed + k);
            std::string slot = new_temp("packslot");
            emit_line(slot + " = getelementptr " + element_type + ", ptr " +
                base + ", i32 " + offset);
            std::string next = new_temp("packoffset");

            if (source == nullptr) {
                store_pack_element(element, slot, all_args[fixed + k], owned_args);
                emit_line(next + " = add i32 " + offset + ", 1");
            } else {
                std::string length = pack_length_value(*source);
                std::string source_data = pack_data_value(*source);
                std::string non_empty = new_temp("packnonempty");
                emit_line(non_empty + " = icmp sgt i32 " + length + ", 0");
                std::string copy_label = new_label("packcopy");
                std::string after_label = new_label("packafter");
                emit_line("br i1 " + non_empty + ", label %" + copy_label +
                    ", label %" + after_label);
                start_block(copy_label);
                std::string length64 = new_temp("packlen64");
                emit_line(length64 + " = sext i32 " + length + " to i64");
                std::string copy_bytes = new_temp("packcopybytes");
                emit_line(copy_bytes + " = mul i64 " + length64 + ", " +
                    element_bytes);
                emit_line("call void @llvm.memcpy.p0.p0.i64(ptr align 1 " + slot +
                    ", ptr align 1 " + source_data + ", i64 " + copy_bytes +
                    ", i1 false)");
                emit_line("br label %" + after_label);
                start_block(after_label);
                emit_line(next + " = add i32 " + offset + ", " + length);
            }
            offset = next;
        }

        ir_args.push_back(base);
        ir_args.push_back(total);
        ir_arg_types.push_back("ptr");
        ir_arg_types.push_back("i32");
        heap_storage = base;
        return true;
    }

    void CodeGenerator::store_pack_element(const AST::Type& element,
        const std::string& slot, AST::Expression* expr,
        std::vector<std::string>& owned_args) {
        ExprValue arg = gen_expr(expr);
        if (!arg.owned_string.empty()) {
            owned_args.push_back(arg.owned_string);
        }

        const std::string element_type = llvm_type(element);
        std::string converted;

        if (aggregate_parameter_uses_pointer(element) &&
            (arg.type.kind == TypeKind::Struct ||
                arg.type.kind == TypeKind::String)) {
            std::string pointer = aggregate_argument_pointer(element, arg);

            if (pointer.empty()) {
                converted = zero_initializer_text(element);
            } else {
                converted = new_temp("packaggregate");
                emit_line(converted + " = load " + element_type + ", ptr " +
                    pointer);
            }
        } else {
            converted = convert_value(arg.value, arg.type, element);
        }

        emit_line("store " + element_type + " " + converted + ", ptr " + slot);
    }

    void CodeGenerator::emit_output_value(const AST::Type& type,
        const std::string& address, const std::string& value) {
        switch (type.kind) {
        case TypeKind::Int:
            emit_line("call void @gallt_output_i32(i32 " + value + ")");
            break;
        case TypeKind::Uint:
            emit_line("call void @gallt_output_u32(i32 " + value + ")");
            break;
        case TypeKind::Lint:
            emit_line("call void @gallt_output_i64(i64 " + value + ")");
            break;
        case TypeKind::Luint:
            emit_line("call void @gallt_output_u64(i64 " + value + ")");
            break;
        case TypeKind::Float:
            emit_line("call void @gallt_output_f32(float " + value + ")");
            break;
        case TypeKind::Double:
            emit_line("call void @gallt_output_f64(double " + value + ")");
            break;
        case TypeKind::Char:
        case TypeKind::Uchar:
            emit_line("call void @gallt_output_char(i8 " + value + ")");
            break;
        case TypeKind::Bool:
            emit_line("call void @gallt_output_bool(i8 " + value + ")");
            break;
        case TypeKind::String:
            emit_line("call void @gallt_output_string(ptr " + address + ")");
            break;
        case TypeKind::Pointer:
        case TypeKind::Function:
        case TypeKind::Array:
            emit_line("call void @gallt_output_ptr(ptr " + address + ")");
            break;
        default:
            break;
        }
    }

    bool CodeGenerator::emit_output_pack_expansion(AST::Expression* expr) {
        auto* post = dynamic_cast<AST::PostfixExpression*>(expr);
        if (post == nullptr ||
            post->op != AST::PostfixExpression::Operator::PackExpand) {
            return false;
        }

        std::string name;
        if (!pack_identifier_base(post->base.get(), name)) {
            return false;
        }

        const VariadicPackLocal* pack = find_variadic_local(name);
        if (pack == nullptr) {
            return false;
        }

        std::string counter = emit_alloca("i32", "packindex");
        emit_line("store i32 0, ptr " + counter);
        std::string condition_label = new_label("outpackcond");
        std::string body_label = new_label("outpackbody");
        std::string end_label = new_label("outpackend");
        emit_line("br label %" + condition_label);
        start_block(condition_label);
        std::string index = new_temp("packindex");
        emit_line(index + " = load i32, ptr " + counter);
        std::string in_range = new_temp("packinrange");
        emit_line(in_range + " = icmp slt i32 " + index + ", " +
            pack_length_value(*pack));
        emit_line("br i1 " + in_range + ", label %" + body_label + ", label %" +
            end_label);
        start_block(body_label);
        std::string address = pack_element_address(*pack, index);
        std::string value;

        switch (pack->element.kind) {
        case TypeKind::Pointer:
        case TypeKind::Function:
        case TypeKind::Array:
            value = address;
            break;
        case TypeKind::String:
            break;
        default:
            value = new_temp("packvalue");
            emit_line(value + " = load " + llvm_type(pack->element) + ", ptr " +
                address);
            break;
        }

        emit_output_value(pack->element, address, value);
        std::string next = new_temp("packnext");
        emit_line(next + " = add i32 " + index + ", 1");
        emit_line("store i32 " + next + ", ptr " + counter);
        emit_line("br label %" + condition_label);
        start_block(end_label);
        return true;
    }

}
