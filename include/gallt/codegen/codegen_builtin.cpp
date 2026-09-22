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

    std::string CodeGenerator::function_reference(const std::string& name) {
        if (name == "main") return "@glt_main";
        for (const auto& top : program_->top_levels) {
            if (auto* f = dynamic_cast<AST::FunctionDefinition*>(top.get())) {
                if (f->name == name) return source_function_symbol(name);
            }
        }
        for (const auto& top : program_->top_levels) {
            if (auto* e = dynamic_cast<AST::ExternDeclaration*>(top.get())) {
                if (e->name == name) return "@" + extern_ir_symbol(e);
            }
        }
        if (lifecycle_symbols_.count(name) != 0) {
            return "@glt_" + name;
        }
        return std::string();
    }

    bool CodeGenerator::ast_function_exists(const std::string& name) const {
        if (name.empty()) return false;
        for (const auto& top : program_->top_levels) {
            if (auto* f = dynamic_cast<AST::FunctionDefinition*>(top.get())) {
                if (f->name == name) return true;
            }
        }
        return false;
    }

    std::string CodeGenerator::extern_ir_symbol(const AST::ExternDeclaration* ext) const {
        auto cached = extern_ir_symbols_.find(ext);
        if (cached != extern_ir_symbols_.end()) return cached->second;
        std::string candidate = ext->c_symbol_name;
        for (const auto& top : program_->top_levels) {
            auto* other = dynamic_cast<AST::ExternDeclaration*>(top.get());
            if (other == nullptr || other == ext) continue;
            if (other->c_symbol_name != ext->c_symbol_name) continue;
            if (!(other->parameters == ext->parameters &&
                other->return_type == ext->return_type)) {
                std::string type_text = ext->return_type.to_string();
                for (const AST::Type& p : ext->parameters) type_text += "$" + p.to_string();
                std::string decorated;
                for (char c : type_text) {
                    if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '$') {
                        decorated += c;
                    }
                }
                candidate = ext->c_symbol_name + "$" + decorated;
                break;
            }
        }
        extern_ir_symbols_[ext] = candidate;
        return candidate;
    }

    std::string CodeGenerator::function_reference_for(const AST::PrimaryExpression* callee) {
        if (callee == nullptr) return std::string();
        auto fit = resolved_functions_.find(callee);
        if (fit != resolved_functions_.end() && fit->second != nullptr) {
            std::string reference = function_reference(fit->second->name);
            if (!reference.empty()) return reference;
        }
        auto eit = resolved_externs_.find(callee);
        if (eit != resolved_externs_.end() && eit->second != nullptr) {
            return "@" + extern_ir_symbol(eit->second);
        }
        return std::string();
    }

    bool CodeGenerator::function_is_extern(const std::string& name) {
        return extern_by_name_.find(name) != extern_by_name_.end();
    }

    void CodeGenerator::emit_output_call(AST::PostfixExpression* call) {
        for (std::size_t arg_index = 0; arg_index < call->arguments.size();
            ++arg_index) {
            auto& arg_expr = call->arguments[arg_index];
            ExprValue arg = gen_expr(arg_expr.get());
            std::string owned = arg.owned_string;
            switch (arg.type.kind) {
            case TypeKind::Int:
                emit_line("call void @gallt_output_i32(i32 " + arg.value + ")");
                break;
            case TypeKind::Uint:
                emit_line("call void @gallt_output_u32(i32 " + arg.value + ")");
                break;
            case TypeKind::Lint:
                emit_line("call void @gallt_output_i64(i64 " + arg.value + ")");
                break;
            case TypeKind::Luint:
                emit_line("call void @gallt_output_u64(i64 " + arg.value + ")");
                break;
            case TypeKind::Uchar:
                emit_line("call void @gallt_output_char(i8 " + arg.value + ")");
                break;
            case TypeKind::Float:
                emit_line("call void @gallt_output_f32(float " + arg.value + ")");
                break;
            case TypeKind::Double:
                emit_line("call void @gallt_output_f64(double " + arg.value + ")");
                break;
            case TypeKind::Char:
                emit_line("call void @gallt_output_char(i8 " + arg.value + ")");
                break;
            case TypeKind::Bool:
                emit_line("call void @gallt_output_bool(i8 " + arg.value + ")");
                break;
            case TypeKind::String: {
                std::string addr = !arg.address.empty() ? arg.address : arg.value;
                emit_line("call void @gallt_output_string(ptr " + addr + ")");
                break;
            }
            case TypeKind::Pointer:
            case TypeKind::Array:
            case TypeKind::Function:
                emit_line("call void @gallt_output_ptr(ptr " + arg.value + ")");
                break;
            case TypeKind::Void:
            case TypeKind::Struct:
            case TypeKind::File:
            default:
                if (diagnostics_ != nullptr) {
                    diagnostics_->report_error_template(call->location,
                        ErrorCode::FunctionArgTypeMismatch,
                        { std::to_string(arg_index + 1), "printable value",
                          arg.type.to_string() });
                }
                break;
            }
            if (!owned.empty()) {
                emit_line("call void @gallt_string_destroy(ptr " + owned + ")");
            }
        }
    }

    void CodeGenerator::emit_input_call(AST::PostfixExpression* call, ExprValue& result) {
        if (call->arguments.empty()) {
            std::string ptr = emit_alloca("i32", "input_slot");
            emit_line("call void @gallt_input_i32(ptr " + ptr + ")");
            std::string val = new_temp("inputval");
            emit_line(val + " = load i32, ptr " + ptr);
            result.value = val;
            result.type = Type::make_int();
            return;
        }
        ExprValue arg = gen_expr(call->arguments[0].get());
        std::string address = gen_address(call->arguments[0].get());
        if (address.empty()) return;
        switch (arg.type.kind) {
        case TypeKind::Int:
            emit_line("call void @gallt_input_i32(ptr " + address + ")");
            break;
        case TypeKind::Uint:
            emit_line("call void @gallt_input_u32(ptr " + address + ")");
            break;
        case TypeKind::Lint:
            emit_line("call void @gallt_input_i64(ptr " + address + ")");
            break;
        case TypeKind::Luint:
            emit_line("call void @gallt_input_u64(ptr " + address + ")");
            break;
        case TypeKind::Uchar:
            emit_line("call void @gallt_input_uchar(ptr " + address + ")");
            break;
        case TypeKind::Float:
            emit_line("call void @gallt_input_f32(ptr " + address + ")");
            break;
        case TypeKind::Double:
            emit_line("call void @gallt_input_f64(ptr " + address + ")");
            break;
        case TypeKind::Char:
            emit_line("call void @gallt_input_char(ptr " + address + ")");
            break;
        case TypeKind::Bool:
            emit_line("call void @gallt_input_bool(ptr " + address + ")");
            break;
        case TypeKind::String:
            emit_line("call void @gallt_input_string(ptr " + address + ")");
            break;
        default:
            break;
        }
    }

    void CodeGenerator::emit_free_call(AST::PostfixExpression* call) {
        if (call->arguments.size() != 1) return;
        ExprValue arg = gen_expr(call->arguments[0].get());
        if (arg.type.kind == TypeKind::Pointer && arg.type.pointee_type &&
            type_contains_string(*arg.type.pointee_type)) {
            emit_destroy_string_at(*arg.type.pointee_type, arg.value);
        }
        emit_line("call void @gallt_free_ptr(ptr " + arg.value + ")");
    }

    CodeGenerator::ExprValue CodeGenerator::emit_file_builtin_call(
        AST::PostfixExpression* call, const std::string& name) {
        ExprValue out;
        out.type = resolved_type(call);

        std::vector<ExprValue> args;
        args.reserve(call->arguments.size());
        for (auto& arg_expr : call->arguments) {
            args.push_back(gen_expr(arg_expr.get()));
        }

        auto string_operand = [](const ExprValue& v) -> std::string {
            if (v.type.kind == TypeKind::String) {
                return !v.address.empty() ? v.address : v.value;
            }
            return v.value;
        };
        auto pointer_operand = [](const ExprValue& v) -> std::string {
            if (!v.value.empty()) return v.value;
            return v.address;
        };
        auto int_operand = [&](const ExprValue& v) -> std::string {
            return convert_value(v.value, v.type, Type::make_int());
        };
        auto destroy_temporaries = [&]() {
            for (const ExprValue& v : args) {
                if (!v.owned_string.empty()) {
                    emit_line("call void @gallt_string_destroy(ptr " + v.owned_string + ")");
                }
            }
        };
        auto bail_out = [&]() {
            destroy_temporaries();
            return out;
        };

        const std::size_t argc = args.size();

        if (name == "fileopen") {
            if (argc != 2) return bail_out();
            out.value = new_temp("fileopen");
            emit_line(out.value + " = call ptr @gallt_file_open(ptr " +
                string_operand(args[0]) + ", ptr " + string_operand(args[1]) + ")");
            destroy_temporaries();
            return out;
        }
        if (name == "fileclose") {
            if (argc != 1) return bail_out();
            out.value = new_temp("fileclose");
            emit_line(out.value + " = call i8 @gallt_file_close(ptr " +
                pointer_operand(args[0]) + ")");
            destroy_temporaries();
            return out;
        }
        if (name == "fileflush") {
            if (argc != 1) return bail_out();
            out.value = new_temp("fileflush");
            emit_line(out.value + " = call i8 @gallt_file_flush(ptr " +
                pointer_operand(args[0]) + ")");
            destroy_temporaries();
            return out;
        }
        if (name == "fileread") {
            if (argc != 3) return bail_out();
            out.value = new_temp("fileread");
            emit_line(out.value + " = call i32 @gallt_file_read(ptr " +
                pointer_operand(args[0]) + ", ptr " + pointer_operand(args[1]) +
                ", i32 " + int_operand(args[2]) + ")");
            destroy_temporaries();
            return out;
        }
        if (name == "filewrite") {
            if (argc != 2) return bail_out();
            out.value = new_temp("filewrite");
            emit_line(out.value + " = call i32 @gallt_file_write(ptr " +
                pointer_operand(args[0]) + ", ptr " + string_operand(args[1]) + ")");
            destroy_temporaries();
            return out;
        }
        if (name == "filewritebytes") {
            if (argc != 3) return bail_out();
            out.value = new_temp("filewritebytes");
            emit_line(out.value + " = call i32 @gallt_file_write_bytes(ptr " +
                pointer_operand(args[0]) + ", ptr " + pointer_operand(args[1]) +
                ", i32 " + int_operand(args[2]) + ")");
            destroy_temporaries();
            return out;
        }
        if (name == "filegetc") {
            if (argc != 1) return bail_out();
            out.value = new_temp("filegetc");
            emit_line(out.value + " = call i32 @gallt_file_getc(ptr " +
                pointer_operand(args[0]) + ")");
            destroy_temporaries();
            return out;
        }
        if (name == "fileputc") {
            if (argc != 2) return bail_out();
            out.value = new_temp("fileputc");
            emit_line(out.value + " = call i32 @gallt_file_putc(ptr " +
                pointer_operand(args[0]) + ", i32 " + int_operand(args[1]) + ")");
            destroy_temporaries();
            return out;
        }
        if (name == "filereadline") {
            if (argc != 1) return bail_out();
            std::string storage = emit_alloca("%struct.gallt.string", "fileline");
            emit_line("call void @gallt_file_readline(ptr " + storage + ", ptr " +
                pointer_operand(args[0]) + ")");
            out.value = storage;
            out.address = storage;
            out.owned_string = storage;
            destroy_temporaries();
            return out;
        }
        if (name == "filewriteline") {
            if (argc != 2) return bail_out();
            out.value = new_temp("filewriteline");
            emit_line(out.value + " = call i32 @gallt_file_writeline(ptr " +
                pointer_operand(args[0]) + ", ptr " + string_operand(args[1]) + ")");
            destroy_temporaries();
            return out;
        }
        if (name == "fileseek") {
            if (argc != 3) return bail_out();
            out.value = new_temp("fileseek");
            emit_line(out.value + " = call i8 @gallt_file_seek(ptr " +
                pointer_operand(args[0]) + ", i32 " + int_operand(args[1]) +
                ", i32 " + int_operand(args[2]) + ")");
            destroy_temporaries();
            return out;
        }
        if (name == "filetell") {
            if (argc != 1) return bail_out();
            out.value = new_temp("filetell");
            emit_line(out.value + " = call i32 @gallt_file_tell(ptr " +
                pointer_operand(args[0]) + ")");
            destroy_temporaries();
            return out;
        }
        if (name == "fileeof") {
            if (argc != 1) return bail_out();
            out.value = new_temp("fileeof");
            emit_line(out.value + " = call i8 @gallt_file_eof(ptr " +
                pointer_operand(args[0]) + ")");
            destroy_temporaries();
            return out;
        }
        if (name == "fileerror") {
            if (argc != 1) return bail_out();
            out.value = new_temp("fileerror");
            emit_line(out.value + " = call i32 @gallt_file_error(ptr " +
                pointer_operand(args[0]) + ")");
            destroy_temporaries();
            return out;
        }
        if (name == "fileremove") {
            if (argc != 1) return bail_out();
            out.value = new_temp("fileremove");
            emit_line(out.value + " = call i8 @gallt_file_remove(ptr " +
                string_operand(args[0]) + ")");
            destroy_temporaries();
            return out;
        }
        if (name == "filerename") {
            if (argc != 2) return bail_out();
            out.value = new_temp("filerename");
            emit_line(out.value + " = call i8 @gallt_file_rename(ptr " +
                string_operand(args[0]) + ", ptr " + string_operand(args[1]) + ")");
            destroy_temporaries();
            return out;
        }
        if (name == "fileexists") {
            if (argc != 1) return bail_out();
            out.value = new_temp("fileexists");
            emit_line(out.value + " = call i8 @gallt_file_exists(ptr " +
                string_operand(args[0]) + ")");
            destroy_temporaries();
            return out;
        }
        if (name == "filesize") {
            if (argc != 1) return bail_out();
            out.value = new_temp("filesize");
            emit_line(out.value + " = call i32 @gallt_file_size(ptr " +
                string_operand(args[0]) + ")");
            destroy_temporaries();
            return out;
        }
        if (name == "filecopy") {
            if (argc != 2) return bail_out();
            out.value = new_temp("filecopy");
            emit_line(out.value + " = call i8 @gallt_file_copy(ptr " +
                string_operand(args[0]) + ", ptr " + string_operand(args[1]) + ")");
            destroy_temporaries();
            return out;
        }
        if (name == "filemkdir") {
            if (argc != 1) return bail_out();
            out.value = new_temp("filemkdir");
            emit_line(out.value + " = call i8 @gallt_file_mkdir(ptr " +
                string_operand(args[0]) + ")");
            destroy_temporaries();
            return out;
        }
        if (name == "fileremovedir") {
            if (argc != 1) return bail_out();
            out.value = new_temp("fileremovedir");
            emit_line(out.value + " = call i8 @gallt_file_removedir(ptr " +
                string_operand(args[0]) + ")");
            destroy_temporaries();
            return out;
        }

        destroy_temporaries();
        return out;
    }

    CodeGenerator::ExprValue CodeGenerator::emit_string_builtin_call(
        AST::PostfixExpression* call, const std::string& name) {
        ExprValue out;
        out.type = resolved_type(call);

        std::vector<ExprValue> args;
        args.reserve(call->arguments.size());
        for (auto& arg_expr : call->arguments) {
            args.push_back(gen_expr(arg_expr.get()));
        }

        auto string_operand = [](const ExprValue& v) -> std::string {
            if (v.type.kind == TypeKind::String) {
                return !v.address.empty() ? v.address : v.value;
            }
            return v.value;
        };
        auto int_operand = [&](std::size_t i) -> std::string {
            return convert_value(args[i].value, args[i].type, Type::make_int());
        };
        auto char_operand = [&](std::size_t i) -> std::string {
            return convert_value(args[i].value, args[i].type, Type::make_char());
        };
        auto file_operand = [&](std::size_t i) -> std::string {
            if (!args[i].value.empty()) return args[i].value;
            return args[i].address;
        };
        auto destroy_temporaries = [&]() {
            for (const ExprValue& v : args) {
                if (!v.owned_string.empty()) {
                    emit_line("call void @gallt_string_destroy(ptr " + v.owned_string + ")");
                }
            }
        };
        auto emit_string_result = [&](const std::string& callee,
                                      const std::vector<std::string>& operands) {
            std::string slot = emit_alloca(llvm_type(Type::make_string()), "strresult");
            emit_line("call void @llvm.memset.p0.i64(ptr " + slot +
                ", i8 0, i64 32, i1 false)");
            std::string text = "call void @" + callee + "(ptr " + slot;
            for (const std::string& operand : operands) {
                text += ", ptr " + operand;
            }
            text += ")";
            emit_line(text);
            destroy_temporaries();
            out.value = slot;
            out.address = slot;
            out.owned_string = slot;
            return out;
        };

        const std::size_t argc = args.size();
        if (name == "strlen") {
            if (argc != 1) { destroy_temporaries(); return out; }
            out.value = new_temp("strlen");
            emit_line(out.value + " = call i32 @gallt_string_length(ptr " +
                string_operand(args[0]) + ")");
            destroy_temporaries();
            return out;
        }
        if (name == "strconcat") {
            if (argc != 2) { destroy_temporaries(); return out; }
            return emit_string_result("gallt_string_concat",
                { string_operand(args[0]), string_operand(args[1]) });
        }
        if (name == "strcopy") {
            if (argc != 1) { destroy_temporaries(); return out; }
            return emit_string_result("gallt_string_copy",
                { string_operand(args[0]) });
        }
        if (name == "strmove") {
            if (argc != 1) { destroy_temporaries(); return out; }
            return emit_string_result("gallt_string_move",
                { string_operand(args[0]) });
        }
        if (name == "strcompare") {
            if (argc != 2) { destroy_temporaries(); return out; }
            out.value = new_temp("strcompare");
            emit_line(out.value + " = call i32 @gallt_string_compare(ptr " +
                string_operand(args[0]) + ", ptr " + string_operand(args[1]) + ")");
            destroy_temporaries();
            return out;
        }
        if (name == "strcontains") {
            if (argc != 2) { destroy_temporaries(); return out; }
            out.value = new_temp("strcontains");
            emit_line(out.value + " = call i8 @gallt_string_contains(ptr " +
                string_operand(args[0]) + ", ptr " + string_operand(args[1]) + ")");
            destroy_temporaries();
            return out;
        }
        if (name == "strsubstr") {
            if (argc != 3) { destroy_temporaries(); return out; }
            std::string slot = emit_alloca(llvm_type(Type::make_string()), "strresult");
            emit_line("call void @llvm.memset.p0.i64(ptr " + slot +
                ", i8 0, i64 32, i1 false)");
            emit_line("call void @gallt_string_substr(ptr " + slot + ", ptr " +
                string_operand(args[0]) + ", i32 " + int_operand(1) + ", i32 " +
                int_operand(2) + ")");
            destroy_temporaries();
            out.value = slot;
            out.address = slot;
            out.owned_string = slot;
            return out;
        }
        if (name == "strfind") {
            if (argc != 2) { destroy_temporaries(); return out; }
            out.value = new_temp("strfind");
            emit_line(out.value + " = call i32 @gallt_string_find(ptr " +
                string_operand(args[0]) + ", ptr " + string_operand(args[1]) + ")");
            destroy_temporaries();
            return out;
        }
        if (name == "strreplace") {
            if (argc != 3) { destroy_temporaries(); return out; }
            return emit_string_result("gallt_string_replace",
                { string_operand(args[0]), string_operand(args[1]),
                  string_operand(args[2]) });
        }
        if (name == "strupper") {
            if (argc != 1) { destroy_temporaries(); return out; }
            return emit_string_result("gallt_string_upper",
                { string_operand(args[0]) });
        }
        if (name == "strlower") {
            if (argc != 1) { destroy_temporaries(); return out; }
            return emit_string_result("gallt_string_lower",
                { string_operand(args[0]) });
        }
        if (name == "strtrim") {
            if (argc != 1) { destroy_temporaries(); return out; }
            return emit_string_result("gallt_string_trim",
                { string_operand(args[0]) });
        }
        if (name == "strcharat") {
            if (argc != 2) { destroy_temporaries(); return out; }
            out.value = new_temp("strcharat");
            emit_line(out.value + " = call i8 @gallt_string_char_at(ptr " +
                string_operand(args[0]) + ", i32 " + int_operand(1) + ")");
            destroy_temporaries();
            return out;
        }
        if (name == "strsetchar") {
            if (argc != 3) { destroy_temporaries(); return out; }
            out.value = new_temp("strsetchar");
            emit_line(out.value + " = call i8 @gallt_string_set_char(ptr " +
                string_operand(args[0]) + ", i32 " + int_operand(1) + ", i8 " +
                char_operand(2) + ")");
            destroy_temporaries();
            return out;
        }
        if (name == "strsplitcount") {
            if (argc != 2) { destroy_temporaries(); return out; }
            out.value = new_temp("strsplitcount");
            emit_line(out.value + " = call i32 @gallt_string_split_count(ptr " +
                string_operand(args[0]) + ", ptr " + string_operand(args[1]) + ")");
            destroy_temporaries();
            return out;
        }
        if (name == "strsplitget") {
            if (argc != 3) { destroy_temporaries(); return out; }
            std::string slot = emit_alloca(llvm_type(Type::make_string()), "strresult");
            emit_line("call void @llvm.memset.p0.i64(ptr " + slot +
                ", i8 0, i64 32, i1 false)");
            emit_line("call void @gallt_string_split_at(ptr " + slot + ", ptr " +
                string_operand(args[0]) + ", ptr " + string_operand(args[1]) +
                ", i32 " + int_operand(2) + ")");
            destroy_temporaries();
            out.value = slot;
            out.address = slot;
            out.owned_string = slot;
            return out;
        }
        if (name == "strread") {
            if (argc != 0) { destroy_temporaries(); return out; }
            std::string slot = emit_alloca(llvm_type(Type::make_string()), "strresult");
            emit_line("call void @llvm.memset.p0.i64(ptr " + slot +
                ", i8 0, i64 32, i1 false)");
            emit_line("call void @gallt_string_read(ptr " + slot + ")");
            out.value = slot;
            out.address = slot;
            out.owned_string = slot;
            return out;
        }
        if (name == "strwrite") {
            if (argc != 1) { destroy_temporaries(); return out; }
            emit_line("call void @gallt_string_write(ptr " +
                string_operand(args[0]) + ")");
            destroy_temporaries();
            return out;
        }
        if (name == "strwritefile") {
            if (argc != 2) { destroy_temporaries(); return out; }
            out.value = new_temp("strwritefile");
            emit_line(out.value + " = call i32 @gallt_string_write_file(ptr " +
                file_operand(0) + ", ptr " + string_operand(args[1]) + ")");
            destroy_temporaries();
            return out;
        }
        destroy_temporaries();
        return out;
    }

    CodeGenerator::ExprValue CodeGenerator::emit_size_align_call(
        AST::PostfixExpression* call, bool is_size) {
        ExprValue out;
        out.type = Type::make_int();
        if (call->arguments.size() != 1) {
            out.value = "0";
            return out;
        }
        AST::Expression* arg = call->arguments[0].get();
        AST::Type target;
        if (auto* prim = dynamic_cast<AST::PrimaryExpression*>(arg)) {
            if (prim->kind == AST::PrimaryExpression::Kind::Identifier) {
                std::string id = prim->identifier;
                LocalInfo* local = lookup_local(id);
                if (local != nullptr) {
                    target = local->type;
                } else {
                    if (id == "int") target = Type::make_int();
                    else if (id == "lint") target = Type::make_lint();
                    else if (id == "uint") target = Type::make_uint();
                    else if (id == "luint") target = Type::make_luint();
                    else if (id == "float") target = Type::make_float();
                    else if (id == "double") target = Type::make_double();
                    else if (id == "char") target = Type::make_char();
                    else if (id == "uchar") target = Type::make_uchar();
                    else if (id == "bool") target = Type::make_bool();
                    else if (id == "string") target = Type::make_string();
                    else if (id == "file") target = Type::make_file();
                    else if (id == "void") target = Type::make_void();
                    else {
                        auto sit = struct_by_name_.find(id);
                        if (sit != struct_by_name_.end()) target = Type::make_struct(id);
                    }
                }
            }
        }
        if (target.kind == TypeKind::Void) {
            ExprValue v = gen_expr(arg);
            target = v.type;
        }
        std::size_t n = is_size ? type_size(target) : type_align(target);
        out.value = std::to_string(n);
        return out;
    }

}
