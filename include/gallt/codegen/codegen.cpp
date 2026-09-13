#include "codegen.hpp"

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
namespace {

    // 将字符串字面量 token 的原始文本解码为字节串（含转义序列）
    // Decode the raw string literal lexeme into bytes, applying escape sequences
    std::string decode_escaped_bytes(std::string_view raw, bool is_char);

    // Gallt 0.2.txt §17 文件操作内置函数名（与类型检查器中的表保持一致）
    // File-operation builtin names from Gallt 0.2.txt §17 (kept in sync with the checker)
    bool is_file_builtin_name(const std::string& name) {
        static const std::unordered_set<std::string> names = {
            "fileopen", "fileclose", "fileflush", "fileread", "filewrite",
            "filewritebytes", "filegetc", "fileputc", "filereadline",
            "filewriteline", "fileseek", "filetell", "fileeof", "fileerror",
            "fileremove", "filerename", "fileexists", "filesize", "filecopy",
            "filemkdir", "fileremovedir",
        };
        return names.find(name) != names.end();
    }

    int hex_value(char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    }

    int octal_value(char c) {
        return (c >= '0' && c <= '7') ? c - '0' : -1;
    }

    std::string llvm_escape_bytes(const std::string& bytes) {
        // 生成 LLVM c"..." 常量文本
        // Produce LLVM c"..." constant text
        std::string out = "c\"";
        for (unsigned char b : bytes) {
            char nibbles[] = "0123456789abcdef";
            switch (b) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\22"; break;
            case '\n': out += "\\0A"; break;
            case '\r': out += "\\0D"; break;
            case '\t': out += "\\09"; break;
            default:
                if (b >= 0x20 && b <= 0x7E) {
                    out.push_back(static_cast<char>(b));
                } else {
                    out += "\\";
                    out.push_back(nibbles[(b >> 4) & 0xF]);
                    out.push_back(nibbles[b & 0xF]);
                }
            }
        }
        out += '"';
        return out;
    }

    std::string llvm_float_constant_text(std::string text) {
        // LLVM 的浮点常量要求文本包含小数点或指数，否则会被当作整数常量
        // LLVM fp constants need '.' or an exponent; otherwise they parse as integers
        if (text.find_first_of(".eE") == std::string::npos) {
            text += ".0";
        }
        return text;
    }

    std::string strip_literal_quotes(std::string_view lexeme) {
        if (lexeme.size() >= 2 &&
            ((lexeme.front() == '"' && lexeme.back() == '"') ||
             (lexeme.front() == '\'' && lexeme.back() == '\''))) {
            lexeme.remove_prefix(1);
            lexeme.remove_suffix(1);
        }
        return std::string(lexeme);
    }

    std::string decode_escaped_bytes(std::string_view raw, bool is_char) {
    // 参照 Gallt 0.2.txt §9 转义表
        // Reference §9 escape table
        std::string content = strip_literal_quotes(raw);
        std::string out;
        for (size_t i = 0; i < content.size(); ++i) {
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
                    int d = hex_value(content[i + 1]);
                    if (d < 0) break;
                    ++i;
                    value = value * 16 + d;
                    ++digits;
                }
                out.push_back(static_cast<char>(value));
                break;
            }
            default:
                if (octal_value(e) >= 0) {
                    int value = octal_value(e);
                    int digits = 1;
                    while (i + 1 < content.size() && digits < 3) {
                        int d = octal_value(content[i + 1]);
                        if (d < 0) break;
                        ++i;
                        value = value * 8 + d;
                        ++digits;
                    }
                    out.push_back(static_cast<char>(value));
                } else {
                    // 未知转义按字面字符保留
                    // Unknown escapes keep the literal character
                    out.push_back(e);
                }
                break;
            }
        }
        if (is_char && !out.empty()) {
            return std::string(1, out.front());
        }
        return out;
    }

} // anonymous namespace

    CodeGenerator::CodeGenerator(
        AST::Program* program,
        const std::unordered_map<const AST::Expression*, AST::Type>& expression_types,
        const std::unordered_map<const AST::PrimaryExpression*,
            const AST::FunctionDefinition*>& resolved_functions,
        const std::unordered_map<const AST::PrimaryExpression*,
            const AST::ExternDeclaration*>& resolved_externs)
        : program_(program), expression_types_(expression_types),
        resolved_functions_(resolved_functions), resolved_externs_(resolved_externs) {
    }

    std::string CodeGenerator::new_temp(const char* hint) {
        // 直接拼接字符串，避免为每个临时名构造 ostringstream
        // Plain concatenation avoids constructing an ostringstream per temporary
        return "%" + std::string(hint) + std::to_string(temp_counter_++);
    }

    std::string CodeGenerator::new_label(const char* hint) {
        return "blk_" + std::string(hint) + "_" + std::to_string(label_counter_++);
    }

    // 分配栈空间：alloca 指令先缓存，函数结束时统一插入入口块
    // Allocate stack space; the alloca line is buffered and inserted into the entry block
    std::string CodeGenerator::emit_alloca(const std::string& type_text, const char* hint) {
        std::string name = new_temp(hint);
        hoisted_allocas_.push_back(name + " = alloca " + type_text);
        return name;
    }

    void CodeGenerator::flush_hoisted_allocas() {
        if (hoisted_allocas_.empty()) return;
        std::size_t pos = hoist_insert_index_;
        if (pos > lines_.size()) pos = lines_.size();
        lines_.insert(lines_.begin() + static_cast<std::ptrdiff_t>(pos),
            hoisted_allocas_.begin(), hoisted_allocas_.end());
        hoisted_allocas_.clear();
    }

    void CodeGenerator::emit_line(const std::string& line) {
        // 若设置了尚未输出的块标签，先在指令前插入 label
        // Insert a pending label before the next instruction when needed
        if (!current_label_.empty() && emitted_labels_.count(current_label_) == 0) {
            lines_.push_back(current_label_ + ":");
            emitted_labels_.insert(current_label_);
        }
        lines_.push_back(line);
        // 记录基本块是否已被终止指令结束
        // Track whether the current block ends with a terminator
        // 使用 starts_with 直接比较，避免每行构造临时 std::string
        // Use starts_with directly instead of building a temporary std::string per line
        if (line.starts_with("ret ") || line.starts_with("br ") ||
            line.starts_with("unreachable") || line.starts_with("switch ") ||
            line.starts_with("invoke ")) {
            current_block_terminated_ = true;
        } else {
            current_block_terminated_ = false;
        }
    }

    void CodeGenerator::start_block(const std::string& label) {
        // 一个基本块开始前先输出 label
        // Print a label before instructions of a basic block
        if (!current_label_.empty() && emitted_labels_.count(current_label_) == 0) {
            // 放弃尚未写入任何指令的悬挂标签（例如 return 之后的空块）
            // Drop a dangling label with no instructions after return
            current_label_.clear();
        }
        if (emitted_labels_.insert(label).second) {
            emit_line(label + ":");
        }
        current_label_ = label;
        current_block_terminated_ = false;
    }

    AST::Type CodeGenerator::resolved_type(const AST::Expression* expr) const {
        auto it = expression_types_.find(expr);
        return it == expression_types_.end() ? AST::Type::make_void() : it->second;
    }

    std::string CodeGenerator::struct_type_name(const std::string& name) const {
        return "%struct.gallt." + name;
    }

    std::string CodeGenerator::llvm_type(const AST::Type& type) {
        switch (type.kind) {
        case TypeKind::Int: return "i32";
        case TypeKind::Float: return "float";
        case TypeKind::Double: return "double";
        case TypeKind::Char: return "i8";
        case TypeKind::Bool: return "i8";
        case TypeKind::String: return "%struct.gallt.string";
        // Gallt 0.2.txt §2：file 为不透明类型，占用平台指针大小
        // Gallt 0.2.txt §2: file is opaque and pointer-sized
        case TypeKind::File: return "ptr";
        case TypeKind::Void: return "void";
        case TypeKind::Pointer:
        case TypeKind::Function:
            return "ptr";
        case TypeKind::Array: {
            std::size_t n = type.array_size.has_value() ? *type.array_size : 0u;
            return "[" + std::to_string(n) + " x " +
                (type.element_type ? llvm_type(*type.element_type) : "i8") + "]";
        }
        case TypeKind::Struct:
            return struct_type_name(type.struct_name);
        }
        return "void";
    }

    void CodeGenerator::collect_structs() {
        struct_by_name_.clear();
        struct_defs_.clear();

        // 收集语句中可能定义的局部结构体
        // Collect local structs that may appear inside statements
        std::function<void(AST::Statement*)> walk = [&](AST::Statement* stmt) {
            collect_structs_in_statement(stmt);
            if (auto* block = dynamic_cast<AST::Block*>(stmt)) {
                for (auto& s : block->statements) walk(s.get());
            } else if (auto* ifs = dynamic_cast<AST::IfStatement*>(stmt)) {
                walk(ifs->then_block.get());
                if (ifs->else_block) walk(ifs->else_block.get());
            } else if (auto* for_ = dynamic_cast<AST::ForStatement*>(stmt)) {
                if (for_->init) walk(for_->init.get());
                if (for_->body) walk(for_->body.get());
            } else if (auto* while_ = dynamic_cast<AST::WhileStatement*>(stmt)) {
                if (while_->body) walk(while_->body.get());
            }
        };

        for (auto& top : program_->top_levels) {
            if (auto* st = dynamic_cast<AST::StructDefinition*>(top.get())) {
                if (!struct_by_name_.count(st->name)) {
                    struct_by_name_[st->name] = st;
                    struct_defs_.push_back(st);
                }
            } else if (auto* func = dynamic_cast<AST::FunctionDefinition*>(top.get())) {
                if (func->body) walk(func->body.get());
            }
        }
    }

    void CodeGenerator::collect_structs_in_statement(AST::Statement* stmt) {
        if (auto* st = dynamic_cast<AST::StructDefinition*>(stmt)) {
            if (!struct_by_name_.count(st->name)) {
                struct_by_name_[st->name] = st;
                struct_defs_.push_back(st);
            }
        }
    }

    bool CodeGenerator::type_contains_string(const AST::Type& type) {
        if (type.kind == TypeKind::String) return true;
        if (type.kind == TypeKind::Array && type.element_type) {
            return type_contains_string(*type.element_type);
        }
        if (type.kind == TypeKind::Struct) {
            auto it = struct_by_name_.find(type.struct_name);
            if (it != struct_by_name_.end()) {
                for (const auto& m : it->second->members) {
                    if (type_contains_string(m.type)) return true;
                }
            }
        }
        return false;
    }

    bool CodeGenerator::generate() {
        collect_structs();
        collect_global_variables();
        collect_function_signatures();
        lines_.clear();
        temp_counter_ = 0;
        label_counter_ = 0;
        emitted_labels_.clear();
        string_literals_.clear();
        string_literal_ids_.clear();
        link_libraries_.clear();
        for (const auto& top : program_->top_levels) {
            if (auto* clib = dynamic_cast<AST::ClibStatement*>(top.get())) {
                link_libraries_.push_back(clib->library_name);
            }
        }

        emit_preamble();
        emit_struct_types();
        emit_runtime_declarations();
        emit_function_declarations();
        emit_global_variables();
        emit_functions();
        emit_global_initializer();
        // 用户 main 之后析构全局对象（Gallt 0.3.txt §20）
        // Destroy global objects right after the user main returns (Gallt 0.3.txt §20)
        emit_main_wrapper();
        // 字符串常量可定义在函数之后；LLVM 支持前向引用模块级全局量
        // String globals may follow functions; LLVM supports forward references
        emit_string_constants();

        std::ostringstream out;
        for (const std::string& line : lines_) {
            out << line << '\n';
        }
        ir_ = out.str();
        return true;
    }

    void CodeGenerator::emit_preamble() {
        // 模块元数据：MSVC x64 Windows 目标
        // Module metadata: MSVC x64 Windows target
        emit_line("source_filename = \"gallt\"");
        emit_line("target triple = \"x86_64-pc-windows-msvc\"");
        // string 是文档规定的 32 字节值类型
        // string is the documented 32-byte value type
        emit_line("%struct.gallt.string = type { i64, i64, [16 x i8] }");
    }

    void CodeGenerator::emit_struct_types() {
        // 结构体按依赖顺序输出；指针依赖不构成拓扑边
        // Emit structs in dependency order; pointer references do not form edges
        std::unordered_set<std::string> emitted;
        std::function<void(AST::StructDefinition*)> visit = [&](AST::StructDefinition* def) {
            if (emitted.count(def->name)) return;
            emitted.insert(def->name);
            for (const auto& m : def->members) {
                std::function<void(const AST::Type&)> deps = [&](const AST::Type& t) {
                    if (t.kind == TypeKind::Struct) {
                        auto it = struct_by_name_.find(t.struct_name);
                        if (it != struct_by_name_.end()) visit(it->second);
                    } else if (t.kind == TypeKind::Array && t.element_type) {
                        deps(*t.element_type);
                    }
                };
                deps(m.type);
            }
            std::string body = "{ ";
            for (size_t i = 0; i < def->members.size(); ++i) {
                if (i != 0) body += ", ";
                body += llvm_type(def->members[i].type);
            }
            body += " }";
            emit_line(struct_type_name(def->name) + " = type " + body);
        };
        for (AST::StructDefinition* def : struct_defs_) {
            visit(def);
        }
    }

    void CodeGenerator::emit_runtime_declarations() {
        // 所有 runtime 函数由随编译器提供的 C 运行库实现
        // Runtime helpers are implemented in the bundled C runtime
        emit_line("declare void @gallt_output_i32(i32)");
        emit_line("declare void @gallt_output_f32(float)");
        emit_line("declare void @gallt_output_f64(double)");
        emit_line("declare void @gallt_output_char(i8)");
        emit_line("declare void @gallt_output_bool(i8)");
        emit_line("declare void @gallt_output_ptr(ptr)");
        emit_line("declare void @gallt_output_string(ptr)");
        emit_line("declare void @gallt_input_i32(ptr)");
        emit_line("declare void @gallt_input_f32(ptr)");
        emit_line("declare void @gallt_input_f64(ptr)");
        emit_line("declare void @gallt_input_char(ptr)");
        emit_line("declare void @gallt_input_bool(ptr)");
        emit_line("declare void @gallt_input_string(ptr)");
        emit_line("declare void @gallt_string_assign(ptr, ptr)");
        emit_line("declare void @gallt_string_init(ptr, ptr, i64)");
        emit_line("declare void @gallt_string_destroy(ptr)");
        emit_line("declare void @gallt_string_concat(ptr, ptr, ptr)");
        emit_line("declare void @gallt_string_from_i32(ptr, i32)");
        emit_line("declare void @gallt_string_from_f32(ptr, float)");
        emit_line("declare void @gallt_string_from_f64(ptr, double)");
        emit_line("declare void @gallt_string_from_char(ptr, i8)");
        emit_line("declare void @gallt_string_from_bool(ptr, i8)");
        emit_line("declare ptr @gallt_string_cstr(ptr)");
        emit_line("declare ptr @gallt_alloc_bytes(i64)");
        emit_line("declare void @gallt_free_ptr(ptr)");
        // RTER 0002：函数指针调用时指针为空（Gallt 0.4.txt 错误表）
        // RTER 0002: calling a null function pointer (Gallt 0.4.txt error table)
        emit_line("declare void @gallt_check_fptr(ptr)");
        // Gallt 0.2.txt §17：文件操作运行库
        // Gallt 0.2.txt §17: file-operation runtime
        emit_line("declare ptr @gallt_file_open(ptr, ptr)");
        emit_line("declare i8 @gallt_file_close(ptr)");
        emit_line("declare i8 @gallt_file_flush(ptr)");
        emit_line("declare i32 @gallt_file_read(ptr, ptr, i32)");
        emit_line("declare i32 @gallt_file_write(ptr, ptr)");
        emit_line("declare i32 @gallt_file_write_bytes(ptr, ptr, i32)");
        emit_line("declare i32 @gallt_file_getc(ptr)");
        emit_line("declare i32 @gallt_file_putc(ptr, i32)");
        emit_line("declare void @gallt_file_readline(ptr, ptr)");
        emit_line("declare i32 @gallt_file_writeline(ptr, ptr)");
        emit_line("declare i8 @gallt_file_seek(ptr, i32, i32)");
        emit_line("declare i32 @gallt_file_tell(ptr)");
        emit_line("declare i8 @gallt_file_eof(ptr)");
        emit_line("declare i32 @gallt_file_error(ptr)");
        emit_line("declare i8 @gallt_file_remove(ptr)");
        emit_line("declare i8 @gallt_file_rename(ptr, ptr)");
        emit_line("declare i8 @gallt_file_exists(ptr)");
        emit_line("declare i32 @gallt_file_size(ptr)");
        emit_line("declare i8 @gallt_file_copy(ptr, ptr)");
        emit_line("declare i8 @gallt_file_mkdir(ptr)");
        emit_line("declare i8 @gallt_file_removedir(ptr)");
        emit_line("declare void @llvm.memcpy.p0.p0.i64(ptr, ptr, i64, i1)");
        emit_line("declare void @llvm.memset.p0.i64(ptr, i8, i64, i1)");
        emit_line("declare double @pow(double, double)");
    }

    void CodeGenerator::emit_string_constants() {
        for (const StringLiteralConstant& c : string_literals_) {
            // Gallt 源码为 UTF-8，string 的 length 按字节数保存，不做代码页转换
            // Gallt source is UTF-8 and string.length counts bytes; no codepage conversion is made
            // 空串仍需一个字节的终止符数组，避免出现零长数组 GEP
            // An empty string keeps a one-byte array so no zero-length GEP is emitted
            std::size_t array_len = c.bytes.empty() ? 1u : c.bytes.size();
            std::string bytes = c.bytes.empty() ? std::string(1, '\0') : c.bytes;
            emit_line("@" + c.llvm_name + " = private constant [" +
                std::to_string(array_len) + " x i8] " +
                llvm_escape_bytes(bytes));
        }
    }

    std::string CodeGenerator::function_llvm_name_for_source(std::string_view name) const {
        // 用户 main 以 @glt_main 生 成，运行时由 sgc 生成的 @main 包装函数调用它，
        // 从而保证“全局对象在 main 之前构造、程序结束时析构”完全由我们控制
        // （Gallt 0.3.txt §20）；其余用户函数加前缀避免与运行库冲突。
        // The user's main is emitted as @glt_main and called by the generated @main
        // wrapper, so global construction/destruction ordering is fully under our control
        // (Gallt 0.3.txt §20); other user functions are prefixed.
        if (name == "main") return "@glt_main";
        return "@glt_" + std::string(name);
    }

    void CodeGenerator::emit_function_declarations() {
        // 只声明真正需要外部链接的 extern 函数；本地函数可前向引用
        // Declare only extern functions; local functions may be forward-referenced
        std::unordered_set<std::string> defined_names;
        for (const auto& top : program_->top_levels) {
            if (auto* func = dynamic_cast<AST::FunctionDefinition*>(top.get())) {
                defined_names.insert(func->name);
            }
        }
        for (const auto& top : program_->top_levels) {
            auto* ext = dynamic_cast<AST::ExternDeclaration*>(top.get());
            if (!ext) continue;
            if (defined_names.count(ext->name)) continue;
            // 每个 IR 符号只声明一次（同名不同签名的 extern 使用修饰名区分）
            // Declare each IR symbol once (same-named externs with different signatures are
            // decorated so they do not collide)
            std::string symbol = extern_ir_symbol(ext);
            if (!declared_extern_symbols_.insert(symbol).second) continue;
            std::string ret = llvm_type(ext->return_type);
            if (ext->return_type.kind == TypeKind::Function) ret = "ptr";
            std::string sig = ret + " @" + symbol + "(";
            for (size_t i = 0; i < ext->parameters.size(); ++i) {
                if (i != 0) sig += ", ";
                sig += llvm_type(ext->parameters[i]);
            }
            sig += ")";
            emit_line("declare " + sig);
        }
    }

    void CodeGenerator::collect_global_variables() {
        global_vars_.clear();
        for (const auto& top : program_->top_levels) {
            if (auto* var = dynamic_cast<AST::VariableDeclaration*>(top.get())) {
                global_vars_.push_back(var);
            }
        }
    }

    void CodeGenerator::collect_function_signatures() {
        function_by_name_.clear();
        extern_by_name_.clear();
        for (const auto& top : program_->top_levels) {
            if (auto* func = dynamic_cast<AST::FunctionDefinition*>(top.get())) {
                function_by_name_[func->name] = func;
            } else if (auto* ext = dynamic_cast<AST::ExternDeclaration*>(top.get())) {
                extern_by_name_[ext->name] = ext;
            }
        }
    }

    void CodeGenerator::emit_global_variables() {
        global_symbols_.clear();
        for (AST::VariableDeclaration* var : global_vars_) {
            std::string address = "@glt_g_" + var->name;
            std::string type_text = llvm_type(var->type);
            emit_line(address + " = global " + type_text + " zeroinitializer");
            LocalInfo info;
            info.type = var->type;
            info.address = address;
            global_symbols_[var->name] = std::move(info);
        }
    }

    void CodeGenerator::emit_global_initializer() {
        // 全局析构函数始终生成（@main 包装函数会调用它）
        // The deinitializer is always emitted; the @main wrapper calls it
        emit_global_deinit_function();
        if (global_vars_.empty()) return;

        // 全局变量通过 CRT 支持的构造函数在 main 之前初始化
        // Global variables initialize before main through CRT-supported constructors
        scopes_.clear();
        cleanup_scopes_.clear();
        push_scope();
        emitted_labels_.clear();
        current_label_.clear();
        current_block_terminated_ = true;

        emit_line("define void @glt_global_init() {");
        std::string entry = new_label("entry");
        start_block(entry);
        hoisted_allocas_.clear();
        hoist_insert_index_ = lines_.size();
        for (AST::VariableDeclaration* var : global_vars_) {
            if (var->initializer) {
                emit_initializer_to_address(var, "@glt_g_" + var->name);
            }
            else if (var->type.kind == TypeKind::Struct) {
                // Gallt 0.3.txt §20：默认构造函数对成员执行默认初始化，
                // 有默认值的成员使用默认值
                // The default constructor applies declared member defaults
                AST::ArrayInitializer empty_init(var->location,
                    std::vector<std::unique_ptr<AST::Initializer>>{});
                emit_struct_brace_initialization("@glt_g_" + var->name, var->type,
                    &empty_init);
            }
        }
        // Gallt 0.3.txt §20：全局对象的构造函数调用（在聚合初始化之后执行）
        // Gallt 0.3.txt §20: constructor calls for global objects (after aggregate init)
        for (auto& stmt : program_->global_initializers) {
            emit_statement(stmt.get());
        }
        emit_line("ret void");
        std::string end_label = new_label("function_end");
        if (!current_block_terminated_) {
            emit_line("br label %" + end_label);
        }
        start_block(end_label);
        emit_line("ret void");
        flush_hoisted_allocas();
        emit_line("}");

        emit_line("@llvm.global_ctors = appending global [1 x { i32, ptr, ptr }] "
            "[ { i32, ptr, ptr } { i32 65535, ptr @glt_global_init, ptr null } ]");
    }

    void CodeGenerator::emit_global_deinit_function() {
        // 全局对象在程序结束时按逆序析构（Gallt 0.3.txt §20）；
        // 该函数始终生成（即使没有全局对象），由 @main 包装函数在用户 main 返回后调用
        // Global objects are destroyed in reverse order at exit; the function is always
        // emitted and invoked by the @main wrapper right after the user main returns
        scopes_.clear();
        cleanup_scopes_.clear();
        push_scope();
        emitted_labels_.clear();
        current_label_.clear();
        current_block_terminated_ = true;
        emit_line("define void @glt_global_deinit() {");
        std::string deinit_entry = new_label("entry");
        start_block(deinit_entry);
        hoisted_allocas_.clear();
        hoist_insert_index_ = lines_.size();
        for (auto it = global_vars_.rbegin(); it != global_vars_.rend(); ++it) {
            AST::VariableDeclaration* var = *it;
            bool destructible = false;
            if (var->type.kind == TypeKind::Struct) {
                auto def_it = struct_by_name_.find(var->type.struct_name);
                destructible = def_it != struct_by_name_.end() &&
                    def_it->second != nullptr && def_it->second->needs_destruction;
            }
            if (!destructible && !type_contains_string(var->type)) continue;
            emit_destroy_string_at(var->type, "@glt_g_" + var->name);
        }
        emit_line("ret void");
        std::string deinit_end = new_label("function_end");
        if (!current_block_terminated_) {
            emit_line("br label %" + deinit_end);
        }
        start_block(deinit_end);
        emit_line("ret void");
        flush_hoisted_allocas();
        emit_line("}");
    }

    void CodeGenerator::emit_main_wrapper() {
        // Gallt 0.3.txt §20：全局对象在 main 之前构造（@llvm.global_ctors）、
        // 在程序结束时按逆序析构。析构由本包装函数在用户 main 返回后调用，
        // 避免依赖 CRT 的 .CRT$XT 终止段（该时机下 stdio 行为不可靠）。
        // Gallt 0.3.txt §20: globals construct before main (@llvm.global_ctors) and are
        // destroyed in reverse order at exit. The wrapper calls the deinitializer right
        // after the user main returns instead of relying on CRT .CRT$XT terminators.
        AST::FunctionDefinition* main_func = nullptr;
        for (const auto& top : program_->top_levels) {
            if (auto* func = dynamic_cast<AST::FunctionDefinition*>(top.get())) {
                if (func->name == "main") {
                    main_func = func;
                    break;
                }
            }
        }
        if (main_func == nullptr) return;

        std::string entry = new_label("main_wrapper");
        emit_line("define i32 @main(i32 %argc, ptr %argv) {");
        start_block(entry);
        // 参数按用户 main 的签名转发（int main() / int main(int count, char* array[])）
        // Forward parameters according to the user's main signature
        std::string args;
        if (main_func->parameters.size() >= 2) {
            std::string second_type = llvm_type(main_func->parameters[1]);
            args = "i32 %argc, " + second_type + " %argv";
        }
        else if (main_func->parameters.size() == 1) {
            args = "i32 %argc";
        }
        std::string ret_type = llvm_type(main_func->return_type);
        if (ret_type == "void" || main_func->return_type.kind == TypeKind::Void) {
            emit_line("call void @glt_main(" + args + ")");
            emit_line("call void @glt_global_deinit()");
            emit_line("ret i32 0");
            emit_line("}");
            return;
        }
        std::string result = new_temp("main_result");
        emit_line(result + " = call " + ret_type + " @glt_main(" + args + ")");
        emit_line("call void @glt_global_deinit()");
        emit_line("ret i32 " + result);
        emit_line("}");
    }

    void CodeGenerator::emit_initializer_to_address(AST::VariableDeclaration* decl,
        const std::string& address) {
        if (!decl->initializer) return;
        if (auto* expr_init = dynamic_cast<AST::ExpressionInitializer*>(decl->initializer.get())) {
            ExprValue value = gen_expr(expr_init->expr.get());
            emit_aggregate_assign(address, decl->type, value);
            return;
        }
        auto* arr_init = dynamic_cast<AST::ArrayInitializer*>(decl->initializer.get());
        if (!arr_init) return;
        if (decl->type.kind == TypeKind::Array) {
            // 全局数组初始化：递归处理嵌套花括号（第 7/14 章）
            // Global array initialization recurses into nested braces (§7/§14)
            emit_array_brace_initialization(address, decl->type, arr_init);
        } else if (decl->type.kind == TypeKind::Struct) {
            emit_struct_brace_initialization(address, decl->type, arr_init);
        }
    }

    CodeGenerator::LocalInfo* CodeGenerator::lookup_local(const std::string& name) {
        // 从最内层作用域向外查找
        // Search from the innermost scope outward
        for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
            auto found = it->find(name);
            if (found != it->end()) return &found->second;
        }
        auto global = global_symbols_.find(name);
        if (global != global_symbols_.end()) return &global->second;
        return nullptr;
    }

    void CodeGenerator::push_scope() {
        scopes_.emplace_back();
        cleanup_scopes_.emplace_back();
    }

    void CodeGenerator::pop_scope() {
        if (scopes_.empty()) return;
        if (cleanup_scopes_.size() == scopes_.size()) {
            std::vector<CleanupRecord>& owned = cleanup_scopes_.back();
            for (auto it = owned.rbegin(); it != owned.rend(); ++it) {
                emit_destroy_string_at(it->type, it->address);
            }
            owned.clear();
            cleanup_scopes_.pop_back();
        }
        scopes_.pop_back();
    }

    void CodeGenerator::register_string_cleanup(const std::string& address,
        const AST::Type& type) {
        if (cleanup_scopes_.empty()) return;
        cleanup_scopes_.back().push_back(CleanupRecord{ type, address });
    }

    void CodeGenerator::destroy_owned_string(ExprValue& value) {
        if (value.owned_string.empty()) return;
        emit_line("call void @gallt_string_destroy(ptr " + value.owned_string + ")");
        value.owned_string.clear();
    }

    void CodeGenerator::destroy_statement_temporaries() {
        // 逆序析构当前语句创建的值临时对象（Gallt 0.3.txt §20）
        // Destroy value temporaries of the current statement in reverse order
        while (!statement_temporaries_.empty()) {
            CleanupRecord record = statement_temporaries_.back();
            statement_temporaries_.pop_back();
            emit_destroy_string_at(record.type, record.address);
        }
    }

    void CodeGenerator::destroy_active_cleanup_scopes(std::size_t until_depth) {
        if (cleanup_scopes_.size() <= until_depth) return;
        for (std::size_t i = cleanup_scopes_.size(); i-- > until_depth;) {
            std::vector<CleanupRecord>& owned = cleanup_scopes_[i];
            for (auto it = owned.rbegin(); it != owned.rend(); ++it) {
                emit_destroy_string_at(it->type, it->address);
            }
            owned.clear();
        }
    }

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
            if (it == struct_by_name_.end()) return;
            std::string struct_ir = llvm_type(type);
            const AST::StructDefinition* def = it->second;
            // 用户定义析构函数：直接调用，由析构函数负责成员释放
            // A user destructor is called directly; it owns member cleanup
            if (!def->destructor_name.empty()) {
                std::string callee = function_reference(def->destructor_name);
                if (!callee.empty()) {
                    emit_line("call void " + callee + "(ptr " + address + ")");
                }
                return;
            }
            for (std::size_t i = 0; i < def->members.size(); ++i) {
                std::string field = new_temp("cleanup_field");
                emit_line(field + " = getelementptr " + struct_ir +
                    ", ptr " + address + ", i32 0, i32 " + std::to_string(i));
                emit_destroy_string_at(def->members[i].type, field);
            }
        }
    }

    void CodeGenerator::emit_functions() {
        for (const auto& top : program_->top_levels) {
            if (auto* func = dynamic_cast<AST::FunctionDefinition*>(top.get())) {
                emit_function(func);
            }
        }
    }

    void CodeGenerator::emit_function(AST::FunctionDefinition* func) {
        scopes_.clear();
        cleanup_scopes_.clear();
        push_scope();
        emitted_labels_.clear();
        current_label_.clear();
        current_function_ = func;
        break_labels_.clear();
        hoisted_allocas_.clear();
        hoist_insert_index_ = 0;

        std::string name = function_llvm_name_for_source(func->name);
        std::string ret = llvm_type(func->return_type);
        if (func->return_type.kind == TypeKind::Function) ret = "ptr";
        // Gallt 0.3.txt §20：结构体返回值改用 sret 约定（调用者分配、被调用者构造）
        // §20: struct returns use the sret convention (caller allocates, callee constructs)
        bool sret = returns_via_sret(func->return_type);

        std::string header = "define " + ret + " " + name + "(";
        if (sret) {
            ret = "void";
            header = "define void " + name + "(ptr %__sret_ret";
        }
        for (size_t i = 0; i < func->parameters.size(); ++i) {
            if (i != 0 || sret) header += ", ";
            header += llvm_type(func->parameters[i]);
            std::string param_name = (i < func->param_names.size() && !func->param_names[i].empty())
                ? func->param_names[i]
                : "_arg" + std::to_string(i);
            header += " %" + param_name;
        }
        header += ") {";
        emit_line(header);
        current_sret_pointer_ = sret ? std::string("%__sret_ret") : std::string();

        // 参数先复制到 alloca，便于取地址和统一左值语义
        // Copy parameters into allocas so address-of and lvalue semantics are uniform
        std::string entry_label = new_label("entry");
        start_block(entry_label);
        // 入口块标签之后即为 alloca 的插入点
        // Insertion point for hoisted allocas: right after the entry label
        hoist_insert_index_ = lines_.size();
        for (size_t i = 0; i < func->parameters.size(); ++i) {
            std::string param_name = (i < func->param_names.size() && !func->param_names[i].empty())
                ? func->param_names[i]
                : "_arg" + std::to_string(i);
            std::string type_text = llvm_type(func->parameters[i]);
            std::string address = emit_alloca(type_text, ("alloca_" + param_name).c_str());
            std::string incoming = "%" + param_name;
            const AST::Type& param_type = func->parameters[i];
            if (param_type.kind == TypeKind::Struct || param_type.kind == TypeKind::String) {
                // Gallt 0.3.txt §20：按值传递的 struct / string 形参是一个自动对象，
                // 必须在函数入口对它执行拷贝构造（string 深拷贝；struct 调用其拷贝构造
                // 或按默认拷贝构造逐成员拷贝），并在函数退出时析构
                // Gallt 0.3.txt §20: a by-value struct/string parameter is an automatic
                // object — copy-construct it at entry (string deep copy; struct uses its
                // copy constructor or the default member-wise copy) and destroy it at exit
                emit_line("store " + type_text + " zeroinitializer, ptr " + address);
                std::string shadow = emit_alloca(type_text, "param_incoming");
                emit_line("store " + type_text + " " + incoming + ", ptr " + shadow);
                emit_memberwise_copy(param_type, address, shadow, false);
                bool needs_cleanup = (param_type.kind == TypeKind::String) ||
                    type_contains_string(param_type);
                if (!needs_cleanup && param_type.kind == TypeKind::Struct) {
                    auto def_it = struct_by_name_.find(param_type.struct_name);
                    needs_cleanup = def_it != struct_by_name_.end() && def_it->second != nullptr &&
                        def_it->second->needs_destruction;
                }
                if (needs_cleanup) {
                    register_string_cleanup(address, param_type);
                }
            } else {
                emit_line("store " + type_text + " " + incoming + ", ptr " + address);
            }
            LocalInfo info;
            info.type = func->parameters[i];
            info.address = address;
            scopes_[0][param_name] = std::move(info);
        }

        if (func->body) {
            emit_block(static_cast<AST::Block*>(func->body.get()), true);
        }

        // 结束块：所有路径返回后补齐默认返回，保证函数有终止指令
        // End block: after returning paths, provide a default terminator
        std::string end_label = new_label("function_end");
        if (!current_block_terminated_) {
            emit_line("br label %" + end_label);
        }
        start_block(end_label);
        destroy_active_cleanup_scopes(0);
        if (func->return_type.kind == TypeKind::Void || sret) {
            emit_line("ret void");
        } else {
            std::string type_text = llvm_type(func->return_type);
            std::string zero = "zeroinitializer";
            if (func->return_type.kind == TypeKind::Int) zero = "0";
            else if (func->return_type.kind == TypeKind::Float) zero = "0.0";
            else if (func->return_type.kind == TypeKind::Double) zero = "0.0";
            else if (func->return_type.kind == TypeKind::Char || func->return_type.kind == TypeKind::Bool) zero = "0";
            else if (func->return_type.kind == TypeKind::Pointer ||
                func->return_type.kind == TypeKind::Function) zero = "null";
            emit_line("ret " + type_text + " " + zero);
        }
        // 插入本函数收集到的 alloca（位于入口块）
        // Insert this function's collected allocas into the entry block
        flush_hoisted_allocas();
        emit_line("}");
        pop_scope();
    }

    void CodeGenerator::emit_block(AST::Block* block, bool new_scope) {
        if (block == nullptr) return;
        if (new_scope) {
            push_scope();
        }
        for (const auto& stmt : block->statements) {
            // 语句执行结束时析构该完整表达式创建的值临时对象（Gallt 0.3.txt §20）
            // Destroy the value temporaries of the full expression when the statement ends
            std::size_t temp_mark = statement_temporaries_.size();
            emit_statement(stmt.get());
            if (!current_block_terminated_) {
                while (statement_temporaries_.size() > temp_mark) {
                    CleanupRecord record = statement_temporaries_.back();
                    statement_temporaries_.pop_back();
                    emit_destroy_string_at(record.type, record.address);
                }
            }
            else {
                statement_temporaries_.resize(temp_mark);
            }
        }
        if (new_scope) {
            pop_scope();
        }
    }

    void CodeGenerator::emit_statement(AST::Statement* stmt) {
        if (stmt == nullptr) return;
        if (auto* block = dynamic_cast<AST::Block*>(stmt)) {
            emit_block(block, true);
        } else if (auto* decl = dynamic_cast<AST::VariableDeclaration*>(stmt)) {
            emit_variable_declaration(decl);
        } else if (auto* destruct_stmt = dynamic_cast<AST::DestructStatement*>(stmt)) {
            // destruct [指针]（Gallt 0.3.txt §20）：先析构，再释放内存；
            // 第 20 章规定对 null 指针执行 destruct 是无操作，因此先做运行期判空
            // destruct [pointer]: destroy first, then release the storage; a null pointer is
            // a no-op per §20, so emit a runtime null guard
            ExprValue target = gen_expr(destruct_stmt->target.get());
            std::string pointer = !target.value.empty() ? target.value
                : (!target.address.empty() ? target.address : std::string());
            if (!pointer.empty() && target.type.kind == TypeKind::Pointer &&
                target.type.pointee_type) {
                std::string body_label = new_label("destruct");
                std::string end_label = new_label("destruct_end");
                std::string is_null = new_temp("destruct_isnull");
                emit_line(is_null + " = icmp eq ptr " + pointer + ", null");
                emit_line("br i1 " + is_null + ", label %" + end_label +
                    ", label %" + body_label);
                start_block(body_label);
                emit_destroy_string_at(*target.type.pointee_type, pointer);
                emit_line("call void @gallt_free_ptr(ptr " + pointer + ")");
                emit_line("br label %" + end_label);
                start_block(end_label);
            }
        } else if (auto* if_stmt = dynamic_cast<AST::IfStatement*>(stmt)) {
            ExprValue cond = gen_expr(if_stmt->condition.get());
            std::string cond_i1 = truth_condition(cond.value, cond.type);
            std::string then_label = new_label("then");
            std::string else_label = new_label("else");
            std::string end_label = new_label("endif");
            std::string has_else = if_stmt->else_block ? "1" : "0";
            emit_line("br i1 " + cond_i1 + ", label %" + then_label +
                ", label %" + else_label);
            start_block(then_label);
            emit_block(static_cast<AST::Block*>(if_stmt->then_block.get()), true);
            emit_line("br label %" + end_label);
            start_block(else_label);
            if (if_stmt->else_block) {
                emit_block(static_cast<AST::Block*>(if_stmt->else_block.get()), true);
            }
            emit_line("br label %" + end_label);
            start_block(end_label);
        } else if (auto* for_stmt = dynamic_cast<AST::ForStatement*>(stmt)) {
            std::size_t break_depth = cleanup_scopes_.size();
            push_scope();
            if (for_stmt->init) emit_statement(for_stmt->init.get());

            std::string cond_label = new_label("forcond");
            std::string body_label = new_label("forbody");
            std::string step_label = new_label("forstep");
            std::string end_label = new_label("forend");
            emit_line("br label %" + cond_label);
            start_block(cond_label);
            if (for_stmt->condition) {
                ExprValue cond = gen_expr(for_stmt->condition.get());
                emit_line("br i1 " + truth_condition(cond.value, cond.type) +
                    ", label %" + body_label + ", label %" + end_label);
            } else {
                emit_line("br label %" + body_label);
            }

            start_block(body_label);
            break_labels_.push_back(end_label);
            break_cleanup_depths_.push_back(break_depth);
            if (for_stmt->body) {
                emit_block(static_cast<AST::Block*>(for_stmt->body.get()), true);
            }
            break_labels_.pop_back();
            break_cleanup_depths_.pop_back();
            emit_line("br label %" + step_label);

            start_block(step_label);
            if (for_stmt->step) {
                (void)gen_expr(for_stmt->step.get());
            }
            emit_line("br label %" + cond_label);

            start_block(end_label);
            pop_scope();
        } else if (auto* while_stmt = dynamic_cast<AST::WhileStatement*>(stmt)) {
            std::string cond_label = new_label("whilecond");
            std::string body_label = new_label("whilebody");
            std::string end_label = new_label("whileend");
            emit_line("br label %" + cond_label);
            start_block(cond_label);
            ExprValue cond = gen_expr(while_stmt->condition.get());
            emit_line("br i1 " + truth_condition(cond.value, cond.type) +
                ", label %" + body_label + ", label %" + end_label);
            start_block(body_label);
            break_labels_.push_back(end_label);
            break_cleanup_depths_.push_back(cleanup_scopes_.size());
            if (while_stmt->body) {
                emit_block(static_cast<AST::Block*>(while_stmt->body.get()), true);
            }
            break_labels_.pop_back();
            break_cleanup_depths_.pop_back();
            emit_line("br label %" + cond_label);
            start_block(end_label);
        } else if (auto* break_stmt = dynamic_cast<AST::BreakStatement*>(stmt)) {
            // TypeChecker 已保证 break 处于循环内
            // TypeChecker guarantees that break appears inside a loop
            if (!break_labels_.empty()) {
                if (!break_cleanup_depths_.empty()) {
                    destroy_active_cleanup_scopes(break_cleanup_depths_.back());
                }
                emit_line("br label %" + break_labels_.back());
                current_label_ = new_label("afterbreak");
            }
        } else if (auto* ret = dynamic_cast<AST::ReturnStatement*>(stmt)) {
            AST::Type ret_type = current_function_
                ? current_function_->return_type
                : AST::Type::make_void();
            // Gallt 0.3.txt §20：结构体返回值在调用者提供的存储上做拷贝/移动构造
            // §20: a struct return copy/move-constructs into the caller-provided storage
            if (ret->value && returns_via_sret(ret_type) && !current_sret_pointer_.empty()) {
                emit_struct_return(ret->value.get(), ret_type, current_sret_pointer_);
                destroy_statement_temporaries();
                destroy_active_cleanup_scopes(0);
                emit_line("ret void");
                current_label_ = new_label("afterret");
                return;
            }
            if (ret->value) {
                ExprValue value = gen_expr(ret->value.get());
                std::string string_ret_storage;
                if (ret_type.kind == TypeKind::String &&
                    value.type.kind == TypeKind::String) {
                    if (!value.owned_string.empty()) {
                        // 临时结果的所有权随 return 转移给调用者
                        // The temporary result's ownership moves to the caller
                        string_ret_storage = value.owned_string;
                        value.owned_string.clear();
                    } else if (!value.address.empty()) {
                        // 局部变量返回前先制作不被释放的返回副本
                        // Returning a local variable first creates a retained copy
                        string_ret_storage =
                            emit_alloca("%struct.gallt.string", "retstring");
                        emit_line("store %struct.gallt.string zeroinitializer, ptr " +
                            string_ret_storage);
                        ExprValue copy_source;
                        copy_source.type = value.type;
                        copy_source.address = value.address;
                        emit_string_assign(string_ret_storage, copy_source);
                    }
                    std::string agg = new_temp("retstringval");
                    emit_line(agg + " = load %struct.gallt.string, ptr " + string_ret_storage);
                    value.value = agg;
                }
                std::string converted = convert_value(value.value, value.type, ret_type);
                std::string type_text = llvm_type(ret_type);
                if (ret_type.kind == TypeKind::Function) {
                    type_text = "ptr";
                }
                // 清除函数栈内仍然存在的 string 局部变量
                // Destroy remaining string locals in the current stack frame
                // 完整表达式结束：先析构本次 return 表达式中创建的临时对象
                // End of the full expression: destroy the temporaries of the return value
                destroy_statement_temporaries();
                destroy_active_cleanup_scopes(0);
                emit_line("ret " + type_text + " " + converted);
            } else {
                destroy_statement_temporaries();
                destroy_active_cleanup_scopes(0);
                emit_line("ret void");
            }
            // return 之后处于不可达区域；为后续源码指令开启新块
            // Code after a return is unreachable; start a fresh block for later source
            current_label_ = new_label("afterret");
        } else if (auto* expr_stmt = dynamic_cast<AST::ExpressionStatement*>(stmt)) {
            emit_expression_statement(expr_stmt);
        } else if (auto* empty = dynamic_cast<AST::EmptyStatement*>(stmt)) {
            (void)empty;
        } else if (auto* struct_def = dynamic_cast<AST::StructDefinition*>(stmt)) {
            (void)struct_def;
        }
    }

    void CodeGenerator::emit_expression_statement(AST::ExpressionStatement* stmt) {
        if (stmt->expr) {
            ExprValue value = gen_expr(stmt->expr.get());
            // 丢弃表达式结果前释放该表达式拥有的临时 string
            // Release temporary strings owned by the expression before discarding it
            destroy_owned_string(value);
        }
    }

    void CodeGenerator::emit_variable_declaration(AST::VariableDeclaration* decl) {
        std::string type_text = llvm_type(decl->type);
        // alloca 名唯一（避免同名变量/形参产生重复 SSA 名），并提升到函数入口块
        // Unique alloca name plus hoisting into the function entry block
        std::string address = emit_alloca(type_text, ("alloca_" + decl->name).c_str());
        LocalInfo info;
        info.type = decl->type;
        info.address = address;
        if (scopes_.empty()) push_scope();
        scopes_.back()[decl->name] = std::move(info);

        // 含 string 的对象先整体清零，保证初始化和释放可安全处理旧值
        // Zero string-containing objects first so assignment/destruction is safe
        if (type_contains_string(decl->type)) {
            register_string_cleanup(address, decl->type);
            std::string n = std::to_string(type_size(decl->type));
            emit_line("call void @llvm.memset.p0.i64(ptr " + address +
                ", i8 0, i64 " + n + ", i1 false)");
        }
        else if (decl->type.kind == TypeKind::Struct) {
            // 需要析构（用户析构函数或含析构成员）的对象登记清理
            // Objects needing destruction register a cleanup record
            auto it = struct_by_name_.find(decl->type.struct_name);
            if (it != struct_by_name_.end() && it->second != nullptr &&
                it->second->needs_destruction) {
                register_string_cleanup(address, decl->type);
            }
        }

        // 数组在声明处分配；函数指针变量也使用普通 alloca
        // Arrays allocate at declaration; function-pointer variables use normal allocas
        if (!decl->initializer) {
            // Gallt 0.3.txt §20：默认构造函数对成员执行默认初始化，成员有默认值时使用默认值
            // Gallt 0.3.txt §20: the default constructor default-initializes members and
            // applies declared member defaults
            if (decl->type.kind == TypeKind::Struct) {
                AST::ArrayInitializer empty_init(decl->location,
                    std::vector<std::unique_ptr<AST::Initializer>>{});
                emit_struct_brace_initialization(address, decl->type, &empty_init);
            }
            return;
        }

        if (auto* expr_init = dynamic_cast<AST::ExpressionInitializer*>(decl->initializer.get())) {
            // copy / move / deep_copy / shallow_copy（Gallt 0.3.txt §20）
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
            // 复制消除：`T v = f()` 直接把 v 的存储交给结构体返回值的 sret 约定，
            // 既避免多余拷贝，也避免对同一存储自我赋值
            // Copy elision: for `T v = f()` the variable's storage is handed to the
            // struct-returning callee, avoiding both an extra copy and a self-assignment
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
            // 局部数组初始化：递归处理嵌套花括号（第 7/14 章）
            // Local array initialization recurses into nested braces (§7/§14)
            emit_array_brace_initialization(address, decl->type, arr_init);
            // 未提供的元素保持声明时的未初始化状态
            // Missing elements remain uninitialized as declared
            return;
        }

        if (arr_init && decl->type.kind == TypeKind::Struct) {
            emit_struct_brace_initialization(address, decl->type, arr_init);
            return;
        }
    }

    std::size_t CodeGenerator::type_size(const AST::Type& type) const {
        auto round_up = [](std::size_t v, std::size_t a) -> std::size_t {
            if (a <= 1) return v;
            return (v + a - 1) / a * a;
        };
        switch (type.kind) {
        case TypeKind::Int:
        case TypeKind::Float: return 4;
        case TypeKind::Double: return 8;
        case TypeKind::Char:
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
            if (it == struct_by_name_.end()) return 0;
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
        case TypeKind::Float: return 4;
        case TypeKind::Double: return 8;
        case TypeKind::Char:
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
            if (it == struct_by_name_.end()) return 1;
            std::size_t align = 1;
            for (const auto& m : it->second->members) {
                align = std::max(align, type_align(m.type));
            }
            return align;
        }
        }
        return 1;
    }

    void CodeGenerator::emit_string_assign(const std::string& dest_address, const ExprValue& source) {
        // string 赋值等价于深拷贝，由运行库处理旧值释放与内容复制
        // String assignment deep-copies; the runtime releases the old value and copies content
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

        // 标量/指针/函数指针：先转换类型再 store
        // Scalars, pointers, and function pointers convert then store
        if (dest_type.kind != TypeKind::Struct && dest_type.kind != TypeKind::Array) {
            std::string converted = convert_value(source.value, source.type, dest_type);
            std::string type_text = llvm_type(dest_type);
            emit_line("store " + type_text + " " + converted + ", ptr " + dest_address);
            return;
        }

        if (dest_type.kind == TypeKind::Array) {
            // 第 7 章禁止数组整体赋值（类型检查器已诊断）；此处的整体拷贝只是
            // 保证任何残余路径都有定义明确的行为，而不是静默丢弃初始化
            // §7 forbids whole-array assignment (diagnosed by the type checker); this copy
            // keeps any remaining path well-defined instead of silently dropping the store
            std::string type_text = llvm_type(dest_type);
            if (!source.address.empty()) {
                std::string loaded = new_temp("arraycopy");
                emit_line(loaded + " = load " + type_text + ", ptr " + source.address);
                emit_line("store " + type_text + " " + loaded + ", ptr " + dest_address);
            }
            else if (!source.value.empty()) {
                emit_line("store " + type_text + " " + source.value + ", ptr " + dest_address);
            }
            return;
        }

        if (dest_type.kind == TypeKind::Struct) {
            // Gallt 0.3.txt §20：结构体赋值/初始化语义由默认拷贝构造/拷贝赋值决定
            // （逐成员；string 深拷贝、struct 调用其拷贝函数、数组逐元素、裸指针浅拷贝），
            // 不再使用旧的“整体浅拷贝 + string 补丁”规则。
            // Gallt 0.3.txt §20: struct copy semantics come from the default copy
            // constructor/assignment (member-wise), not from the removed shallow+patch rule.
            std::string type_text = llvm_type(dest_type);
            std::string source_storage = source.address;
            if (source_storage.empty()) {
                std::string loaded = source.value;
                if (loaded.empty()) return;
                source_storage = emit_alloca(type_text, "agg_temp");
                emit_line("store " + type_text + " " + loaded + ", ptr " + source_storage);
            }
            emit_memberwise_copy(dest_type, dest_address, source_storage, is_assignment);
        }
    }

    void CodeGenerator::emit_deep_copy_string_members(const AST::Type& struct_type,
        const std::string& dest_address, const std::string& src_address) {
        // 递归复制结构体中的 string 成员，保证文档要求的深拷贝语义
        // Recursively deep-copy string members to satisfy documented semantics
        // （Gallt 0.3 第 20 章：默认拷贝构造/拷贝赋值对 string 成员同样深拷贝）
        // (Gallt 0.3 §20: default copy construction/assignment also deep-copies strings)
        // 说明：Gallt 0.3 第 20 章的默认拷贝构造/拷贝赋值同样对 string 成员深拷贝，
        // 因此该路径与默认特殊成员函数语义一致，保留供既有的结构体初始化路径复用。
        // Gallt 0.3 §20 default copy construction/assignment also deep-copies string
        // members, so this path stays semantically consistent with the default members.
        auto it = struct_by_name_.find(struct_type.struct_name);
        if (it == struct_by_name_.end() || it->second == nullptr) return;
        const AST::StructDefinition* def = it->second;
        std::string struct_ir = llvm_type(struct_type);
        for (size_t i = 0; i < def->members.size(); ++i) {
            const auto& member = def->members[i];
            std::string src_field = new_temp("deep_src");
            std::string dst_field = new_temp("deep_dst");
            emit_line(src_field + " = getelementptr " + struct_ir +
                ", ptr " + src_address + ", i32 0, i32 " + std::to_string(i));
            emit_line(dst_field + " = getelementptr " + struct_ir +
                ", ptr " + dest_address + ", i32 0, i32 " + std::to_string(i));
            if (member.type.kind == TypeKind::String) {
                ExprValue source;
                source.type = member.type;
                source.address = src_field;
                emit_string_assign(dst_field, source);
            } else if (member.type.kind == TypeKind::Struct &&
                type_contains_string(member.type)) {
                emit_deep_copy_string_members(member.type, dst_field, src_field);
            } else if (member.type.kind == TypeKind::Array &&
                type_contains_string(member.type) && member.type.array_size.has_value()) {
                std::string elem_ir = llvm_type(*member.type.element_type);
                std::string array_ir = llvm_type(member.type);
                size_t n = *member.type.array_size;
                for (size_t j = 0; j < n; ++j) {
                    std::string se = new_temp("deep_se");
                    std::string de = new_temp("deep_de");
                    emit_line(se + " = getelementptr " + array_ir +
                        ", ptr " + src_field + ", i64 0, i64 " + std::to_string(j));
                    emit_line(de + " = getelementptr " + array_ir +
                        ", ptr " + dst_field + ", i64 0, i64 " + std::to_string(j));
                    if (member.type.element_type->kind == TypeKind::String) {
                        ExprValue source;
                        source.type = *member.type.element_type;
                        source.address = se;
                        emit_string_assign(de, source);
                    } else if (member.type.element_type->kind == TypeKind::Struct) {
                        emit_deep_copy_string_members(*member.type.element_type, de, se);
                    }
                }
            }
        }
    }

    void CodeGenerator::emit_struct_brace_initialization(const std::string& address,
        const AST::Type& struct_type, AST::ArrayInitializer* init) {
        auto it = struct_by_name_.find(struct_type.struct_name);
        if (it == struct_by_name_.end() || it->second == nullptr) return;
        AST::StructDefinition* def = it->second;
        std::string struct_ir_type = llvm_type(struct_type);
        for (size_t i = 0; i < init->elements.size(); ++i) {
            if (i >= def->members.size()) break;
            const AST::StructDefinition::Member& member = def->members[i];
            std::string field_ptr = new_temp("field");
            emit_line(field_ptr + " = getelementptr " + struct_ir_type +
                ", ptr " + address + ", i32 0, i32 " + std::to_string(i));
            AST::Initializer* element = init->elements[i].get();
            if (auto* nested = dynamic_cast<AST::ArrayInitializer*>(element)) {
                if (member.type.kind == TypeKind::Struct) {
                    emit_struct_brace_initialization(field_ptr, member.type, nested);
                } else if (member.type.kind == TypeKind::Array) {
                    // 嵌套数组初始化：递归处理嵌套花括号与逐元素写入
                    // Nested array initialization: recurse into nested braces/elements
                    emit_array_brace_initialization(field_ptr, member.type, nested);
                }
            } else if (auto* e = dynamic_cast<AST::ExpressionInitializer*>(element)) {
                ExprValue value = gen_expr(e->expr.get());
                emit_aggregate_assign(field_ptr, member.type, value);
            }
        }
        // 未提供的成员按结构体默认值初始化
        // Members omitted from the initializer use struct defaults
        for (size_t i = init->elements.size(); i < def->members.size(); ++i) {
            const AST::StructDefinition::Member& member = def->members[i];
            if (!member.initializer) continue;
            std::string field_ptr = new_temp("defaultfield");
            emit_line(field_ptr + " = getelementptr " + struct_ir_type +
                ", ptr " + address + ", i32 0, i32 " + std::to_string(i));
            if (auto* e = dynamic_cast<AST::ExpressionInitializer*>(member.initializer.get())) {
                ExprValue value = gen_expr(e->expr.get());
                emit_aggregate_assign(field_ptr, member.type, value);
            }
        }
    }

    void CodeGenerator::emit_array_brace_initialization(const std::string& address,
        const AST::Type& array_type, AST::ArrayInitializer* init) {
        // Gallt 0.3.txt 第 7/14 章：数组的花括号初始化逐元素写入；元素本身是数组时
        // 递归进入更深一层花括号，元素是 struct 时按第 20 章的拷贝语义写入
        // Gallt 0.3.txt §7/§14: brace initialization writes elements one by one; nested
        // arrays recurse into deeper braces and struct elements use §20 copy semantics
        if (init == nullptr || !array_type.element_type) return;
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
        // 未提供的元素保持未初始化状态（第 7 章：部分初始化时其余元素未定义）
        // Elements omitted from the list stay uninitialized (§7 partial initialization)
    }

    CodeGenerator::ExprValue CodeGenerator::gen_expr(AST::Expression* expr) {
        // 未检查/未知表达式统一返回 void，正常编译不会进入
        // Unknown expressions return void; a validated program never reaches here
        if (expr == nullptr) {
            return ExprValue{};
        }
        if (auto* assign = dynamic_cast<AST::AssignmentExpression*>(expr)) {
            ExprValue left = gen_expr(assign->left.get());
            if (left.address.empty()) return ExprValue{};

            if (assign->op == AST::AssignmentExpression::Operator::Assign) {
                // Gallt 0.3.txt §20：结构体赋值由拷贝赋值/移动赋值决定；
                // 这里先解析右值的拷贝/移动语义，避免对 move(...) 右值重复求值
                // §20: struct assignment uses copy/move assignment; the right-hand side is
                // analysed first so a move(...) operand is not evaluated twice
                bool is_move = false;
                AST::Expression* source_expr = assign->right.get();
                if (auto* cm = dynamic_cast<AST::PrimaryExpression*>(source_expr)) {
                    if (cm->kind == AST::PrimaryExpression::Kind::CopyMove) {
                        source_expr = cm->paren_expr.get();
                    }
                }
                if (auto* cm = dynamic_cast<AST::PrimaryExpression*>(assign->right.get())) {
                    if (cm->kind == AST::PrimaryExpression::Kind::CopyMove &&
                        cm->copy_move_kind == AST::PrimaryExpression::CopyMoveKind::Move) {
                        is_move = true;
                    }
                }
                if (left.type.kind == AST::TypeKind::Struct && source_expr != nullptr) {
                    std::string source_address = operand_address(source_expr);
                    if (!source_address.empty()) {
                        if (is_move) {
                            emit_memberwise_move(left.type, left.address, source_address, true);
                        }
                        else {
                            emit_memberwise_copy(left.type, left.address, source_address, true);
                        }
                        ExprValue result;
                        result.type = left.type;
                        result.address = left.address;
                        result.is_lvalue = true;
                        result.value = new_temp("assign_result");
                        emit_line(result.value + " = load " + llvm_type(left.type) +
                            ", ptr " + left.address);
                        return result;
                    }
                }
            }
            ExprValue right = gen_expr(assign->right.get());
            if (assign->op == AST::AssignmentExpression::Operator::Assign) {
                emit_aggregate_assign(left.address, left.type, right, true);
            } else {
                // += 与 -= 展开为左值读、运算、写回
                // Compound assignment expands to load, arithmetic, store
                std::string old_value = new_temp("old");
                std::string type_text = llvm_type(left.type);
                emit_line(old_value + " = load " + type_text + ", ptr " + left.address);
                std::string right_value = right.value;
                AST::Type rhs_type = right.type;
                if (left.type.kind == TypeKind::Pointer && rhs_type.is_integer()) {
                    std::string idx64 = convert_value(right_value, rhs_type, Type::make_int());
                    std::string idx = new_temp("pari");
                    emit_line(idx + " = sext i32 " + idx64 + " to i64");
                    std::string pointee_type = left.type.pointee_type ? llvm_type(*left.type.pointee_type) : "i8";
                    std::string result = new_temp("parr");
                    if (assign->op == AST::AssignmentExpression::Operator::MinusAssign) {
                        std::string neg = new_temp("neg");
                        emit_line(neg + " = sub i64 0, " + idx);
                        emit_line(result + " = getelementptr " + pointee_type +
                            ", ptr " + old_value + ", i64 " + neg);
                    } else {
                        emit_line(result + " = getelementptr " + pointee_type +
                            ", ptr " + old_value + ", i64 " + idx);
                    }
                    emit_line("store ptr " + result + ", ptr " + left.address);
                } else {
                    std::string result = new_temp("cmpd");
                    std::string type_text2 = llvm_type(left.type);
                    AST::Type op_type = left.type.is_integer()
                        ? Type::make_int()
                        : left.type;
                    std::string lv = convert_value(old_value, left.type, op_type);
                    std::string rv = convert_value(right_value, rhs_type, op_type);
                    if (op_type.kind == TypeKind::Float) {
                        emit_line(result + " = " +
                            (assign->op == AST::AssignmentExpression::Operator::PlusAssign ? "fadd" : "fsub") +
                            " float " + lv + ", " + rv);
                    } else if (op_type.kind == TypeKind::Double) {
                        emit_line(result + " = " +
                            (assign->op == AST::AssignmentExpression::Operator::PlusAssign ? "fadd" : "fsub") +
                            " double " + lv + ", " + rv);
                    } else {
                        emit_line(result + " = " +
                            (assign->op == AST::AssignmentExpression::Operator::PlusAssign ? "add" : "sub") +
                            " i32 " + lv + ", " + rv);
                    }
                    std::string converted = convert_value(result, op_type, left.type);
                    emit_line("store " + type_text2 + " " + converted + ", ptr " + left.address);
                }
            }
            ExprValue result;
            result.type = resolved_type(expr);
            result.address = left.address;
            result.is_lvalue = true;
            std::string type_text = llvm_type(result.type);
            result.value = new_temp("assignresult");
            emit_line(result.value + " = load " + type_text + ", ptr " + result.address);
            return result;
        }
        if (auto* paren = dynamic_cast<AST::PrimaryExpression*>(expr)) {
            return gen_primary(paren);
        }
        if (auto* unary = dynamic_cast<AST::UnaryExpression*>(expr)) {
            return gen_unary(unary);
        }
        if (auto* post = dynamic_cast<AST::PostfixExpression*>(expr)) {
            return gen_postfix(post);
        }
        if (auto* logical = dynamic_cast<AST::LogicalOrExpression*>(expr)) {
            ExprValue left = gen_expr(logical->left.get());
            std::string left_i1 = truth_condition(left.value, left.type);
            std::string eval_label = new_label("oreval");
            std::string true_label = new_label("ortrue");
            std::string end_label = new_label("orend");
            emit_line("br i1 " + left_i1 + ", label %" + true_label +
                ", label %" + eval_label);

            start_block(eval_label);
            ExprValue right = gen_expr(logical->right.get());
            std::string rhs_i1 = truth_condition(right.value, right.type);
            std::string rhs_i8 = new_temp("or_rhs");
            emit_line(rhs_i8 + " = zext i1 " + rhs_i1 + " to i8");
            emit_line("br label %" + end_label);

            start_block(true_label);
            emit_line("br label %" + end_label);

            start_block(end_label);
            ExprValue result;
            result.type = Type::make_bool();
            result.value = new_temp("or_result");
            emit_line(result.value + " = phi i8 [ 1, %" + true_label +
                " ], [ " + rhs_i8 + ", %" + eval_label + " ]");
            return result;
        }
        if (auto* logical_and = dynamic_cast<AST::LogicalAndExpression*>(expr)) {
            ExprValue left = gen_expr(logical_and->left.get());
            std::string left_i1 = truth_condition(left.value, left.type);
            std::string eval_label = new_label("andeval");
            std::string false_label = new_label("andfalse");
            std::string end_label = new_label("andend");
            emit_line("br i1 " + left_i1 + ", label %" + eval_label +
                ", label %" + false_label);

            start_block(eval_label);
            ExprValue right = gen_expr(logical_and->right.get());
            std::string rhs_i1 = truth_condition(right.value, right.type);
            std::string rhs_i8 = new_temp("and_rhs");
            emit_line(rhs_i8 + " = zext i1 " + rhs_i1 + " to i8");
            emit_line("br label %" + end_label);

            start_block(false_label);
            emit_line("br label %" + end_label);

            start_block(end_label);
            ExprValue result;
            result.type = Type::make_bool();
            result.value = new_temp("and_result");
            emit_line(result.value + " = phi i8 [ 0, %" + false_label +
                " ], [ " + rhs_i8 + ", %" + eval_label + " ]");
            return result;
        }
        if (auto* comp = dynamic_cast<AST::ComparisonExpression*>(expr)) {
            ExprValue l = gen_expr(comp->left.get());
            ExprValue r = gen_expr(comp->right.get());
            std::string cmp = "icmp ";
            if (l.type.kind == TypeKind::Float || r.type.kind == TypeKind::Float ||
                l.type.kind == TypeKind::Double || r.type.kind == TypeKind::Double) {
                AST::Type common = (l.type.kind == TypeKind::Double || r.type.kind == TypeKind::Double)
                    ? Type::make_double() : Type::make_float();
                std::string lv = convert_value(l.value, l.type, common);
                std::string rv = convert_value(r.value, r.type, common);
                cmp = "fcmp ";
                cmp += (common.kind == TypeKind::Double ? "double " : "float ");
                std::string op;
                switch (comp->op) {
                case AST::ComparisonExpression::Operator::Equal: op = "oeq"; break;
                case AST::ComparisonExpression::Operator::NotEqual: op = "one"; break;
                case AST::ComparisonExpression::Operator::Greater: op = "ogt"; break;
                case AST::ComparisonExpression::Operator::Less: op = "olt"; break;
                case AST::ComparisonExpression::Operator::GreaterEqual: op = "oge"; break;
                case AST::ComparisonExpression::Operator::LessEqual: op = "ole"; break;
                }
                std::string t = new_temp("fcmp");
                emit_line(t + " = fcmp " + op + " " + (common.kind == TypeKind::Double ? "double" : "float") +
                    " " + lv + ", " + rv);
                ExprValue result;
                result.type = Type::make_bool();
                result.value = new_temp("bool");
                emit_line(result.value + " = zext i1 " + t + " to i8");
                return result;
            }
            // Gallt 0.4.txt §13：函数指针（TypeKind::Function）与 null 的
            // `==` / `!=` 比较也走指针地址比较路径
            // Gallt 0.4.txt §13: function pointers compare through the pointer-address
            // path as well (they are represented as `ptr` values)
            bool left_is_address = l.type.kind == TypeKind::Pointer ||
                l.type.kind == TypeKind::File || l.type.kind == TypeKind::Function;
            bool right_is_address = r.type.kind == TypeKind::Pointer ||
                r.type.kind == TypeKind::File || r.type.kind == TypeKind::Function;
            if (left_is_address || right_is_address) {
                // 整数与指针比较时先把整数转成 i64 并与地址整数比较
                // Compare pointer/integer mixes by integerizing the address
                std::string lv;
                std::string rv;
                if (left_is_address) {
                    lv = new_temp("ptrtoint_l");
                    emit_line(lv + " = ptrtoint ptr " + l.value + " to i64");
                } else {
                    lv = to_i64_value(l.value, l.type);
                }
                if (right_is_address) {
                    rv = new_temp("ptrtoint_r");
                    emit_line(rv + " = ptrtoint ptr " + r.value + " to i64");
                } else {
                    rv = to_i64_value(r.value, r.type);
                }
                std::string op;
                switch (comp->op) {
                case AST::ComparisonExpression::Operator::Equal: op = "eq"; break;
                case AST::ComparisonExpression::Operator::NotEqual: op = "ne"; break;
                case AST::ComparisonExpression::Operator::Greater: op = "sgt"; break;
                case AST::ComparisonExpression::Operator::Less: op = "slt"; break;
                case AST::ComparisonExpression::Operator::GreaterEqual: op = "sge"; break;
                case AST::ComparisonExpression::Operator::LessEqual: op = "sle"; break;
                }
                std::string t = new_temp("icmp");
                emit_line(t + " = icmp " + op + " i64 " + lv + ", " + rv);
                ExprValue result;
                result.type = Type::make_bool();
                result.value = new_temp("bool");
                emit_line(result.value + " = zext i1 " + t + " to i8");
                return result;
            }
            // 普通整数/浮点比较已在上面覆盖；这里用于整型
            // Ordinary integer comparisons fall through here
            AST::Type common = l.type.is_floating() || r.type.is_floating()
                ? (l.type.kind == TypeKind::Double || r.type.kind == TypeKind::Double
                    ? Type::make_double() : Type::make_float())
                : Type::make_int();
            if (common.kind == TypeKind::Float || common.kind == TypeKind::Double) {
                return ExprValue{};
            }
            std::string lv = convert_value(l.value, l.type, common);
            std::string rv = convert_value(r.value, r.type, common);
            std::string op;
            switch (comp->op) {
            case AST::ComparisonExpression::Operator::Equal: op = "eq"; break;
            case AST::ComparisonExpression::Operator::NotEqual: op = "ne"; break;
            case AST::ComparisonExpression::Operator::Greater: op = "sgt"; break;
            case AST::ComparisonExpression::Operator::Less: op = "slt"; break;
            case AST::ComparisonExpression::Operator::GreaterEqual: op = "sge"; break;
            case AST::ComparisonExpression::Operator::LessEqual: op = "sle"; break;
            }
            std::string t = new_temp("icmp");
            emit_line(t + " = icmp " + op + " i32 " + lv + ", " + rv);
            ExprValue result;
            result.type = Type::make_bool();
            result.value = new_temp("bool");
            emit_line(result.value + " = zext i1 " + t + " to i8");
            return result;
        }
        if (auto* add = dynamic_cast<AST::AdditiveExpression*>(expr)) {
            ExprValue l = gen_expr(add->left.get());
            ExprValue r = gen_expr(add->right.get());
            AST::Type result_type = resolved_type(expr);
            if (result_type.kind == TypeKind::String) {
                return gen_binary_string_plus(add->left.get(), add->right.get());
            }
            // 指针 + 整数 / 整数 + 指针
            if (l.type.kind == TypeKind::Pointer || r.type.kind == TypeKind::Pointer) {
                bool left_ptr = l.type.kind == TypeKind::Pointer;
                AST::Type ptr_type = left_ptr ? l.type : r.type;
                std::string pointee_ir = ptr_type.pointee_type ? llvm_type(*ptr_type.pointee_type) : "i8";
                std::string base_ptr = left_ptr ? l.value : r.value;
                std::string out = new_temp("ptrmath");
                if (add->op == AST::AdditiveExpression::Operator::Minus &&
                    l.type.kind == TypeKind::Pointer && r.type.kind == TypeKind::Pointer) {
                    // ptr1 - ptr2：地址差除以元素大小
                    // ptr1 - ptr2 divides the byte difference by the element size
                    std::string ia = new_temp("pia");
                    std::string ib = new_temp("pib");
                    emit_line(ia + " = ptrtoint ptr " + l.value + " to i64");
                    emit_line(ib + " = ptrtoint ptr " + r.value + " to i64");
                    std::string diff = new_temp("pd");
                    emit_line(diff + " = sub i64 " + ia + ", " + ib);
                    std::size_t elem_size = ptr_type.pointee_type ? type_size(*ptr_type.pointee_type) : 1u;
                    out = new_temp("ptrsub");
                    emit_line(out + " = sdiv i64 " + diff + ", " + std::to_string(elem_size));
                    // 文档规定指针差返回 int，而地址差在 IR 中为 i64
                    // Pointer difference is int per the spec; IR address math is i64
                    std::string truncated = new_temp("ptrsubint");
                    emit_line(truncated + " = trunc i64 " + out + " to i32");
                    out = truncated;
                    ExprValue v;
                    v.type = result_type;
                    v.value = out;
                    return v;
                }
                AST::Type int_type = left_ptr ? r.type : l.type;
                std::string int_val = left_ptr ? r.value : l.value;
                std::string idx = to_i64_value(int_val, int_type);
                if (add->op == AST::AdditiveExpression::Operator::Plus) {
                    emit_line(out + " = getelementptr " + pointee_ir +
                        ", ptr " + base_ptr + ", i64 " + idx);
                } else {
                    std::string neg = new_temp("negi");
                    emit_line(neg + " = sub i64 0, " + idx);
                    emit_line(out + " = getelementptr " + pointee_ir +
                        ", ptr " + base_ptr + ", i64 " + neg);
                }
                ExprValue v;
                v.type = result_type;
                v.value = out;
                return v;
            }
            AST::Type common = result_type;
            std::string opcode;
            std::string lv = convert_value(l.value, l.type, common);
            std::string rv = convert_value(r.value, r.type, common);
            std::string ir_type = llvm_type(common);
            if (common.kind == TypeKind::Float || common.kind == TypeKind::Double) {
                opcode = add->op == AST::AdditiveExpression::Operator::Plus ? "fadd" : "fsub";
            } else {
                // 整数统一按 i32 运算，char/bool 结果最后截断
                // Integer arithmetic runs in i32; char/bool results are truncated later
                AST::Type wide = Type::make_int();
                lv = convert_value(l.value, l.type, wide);
                rv = convert_value(r.value, r.type, wide);
                ir_type = "i32";
                common = wide;
                opcode = add->op == AST::AdditiveExpression::Operator::Plus ? "add" : "sub";
            }
            std::string tmp = new_temp("arith");
            emit_line(tmp + " = " + opcode + " " + ir_type + " " + lv + ", " + rv);
            ExprValue v;
            v.type = result_type;
            v.value = convert_value(tmp, common, result_type);
            return v;
        }
        if (auto* mul = dynamic_cast<AST::MultiplicativeExpression*>(expr)) {
            ExprValue l = gen_expr(mul->left.get());
            ExprValue r = gen_expr(mul->right.get());
            AST::Type result_type = resolved_type(expr);
            AST::Type common = result_type;
            std::string lv = convert_value(l.value, l.type, common);
            std::string rv = convert_value(r.value, r.type, common);
            std::string ir_type = llvm_type(common);
            std::string opcode;
            if (common.kind == TypeKind::Float || common.kind == TypeKind::Double) {
                opcode = mul->op == AST::MultiplicativeExpression::Operator::Multiply ? "fmul" : "fdiv";
            } else {
                AST::Type wide = Type::make_int();
                lv = convert_value(l.value, l.type, wide);
                rv = convert_value(r.value, r.type, wide);
                common = wide;
                ir_type = "i32";
                // Gallt 0.2.txt §3：% 与 *、/ 同级，整数取余使用 srem
                // Gallt 0.2.txt §3: '%' shares the precedence of '*' and '/'; integer
                // remainder is emitted as srem
                switch (mul->op) {
                case AST::MultiplicativeExpression::Operator::Multiply: opcode = "mul"; break;
                case AST::MultiplicativeExpression::Operator::Divide:   opcode = "sdiv"; break;
                case AST::MultiplicativeExpression::Operator::Remainder: opcode = "srem"; break;
                }
            }
            std::string tmp = new_temp("mul");
            emit_line(tmp + " = " + opcode + " " + ir_type + " " + lv + ", " + rv);
            ExprValue v;
            v.type = result_type;
            v.value = convert_value(tmp, common, result_type);
            return v;
        }
        if (auto* pow = dynamic_cast<AST::PowerExpression*>(expr)) {
            ExprValue l = gen_expr(pow->left.get());
            ExprValue r = gen_expr(pow->right.get());
            AST::Type result_type = resolved_type(expr);
            std::string ld = convert_value(l.value, l.type, Type::make_double());
            std::string rd = convert_value(r.value, r.type, Type::make_double());
            std::string tmp = new_temp("pow");
            emit_line(tmp + " = call double @pow(double " + ld + ", double " + rd + ")");
            ExprValue v;
            v.type = result_type;
            v.value = convert_value(tmp, Type::make_double(), result_type);
            return v;
        }
        return ExprValue{};
    }

    std::string CodeGenerator::to_i64_value(const std::string& value, const AST::Type& type) {
        if (type.kind == TypeKind::Char) {
            std::string out = new_temp("sextchar");
            emit_line(out + " = sext i8 " + value + " to i64");
            return out;
        }
        if (type.kind == TypeKind::Bool) {
            std::string out = new_temp("zextbool");
            emit_line(out + " = zext i8 " + value + " to i64");
            return out;
        }
        if (type.kind == TypeKind::Int) {
            std::string out = new_temp("sextint");
            emit_line(out + " = sext i32 " + value + " to i64");
            return out;
        }
        if (type.kind == TypeKind::Float) {
            std::string out = new_temp("fptosi");
            emit_line(out + " = fptosi float " + value + " to i64");
            return out;
        }
        if (type.kind == TypeKind::Double) {
            std::string out = new_temp("fptosi");
            emit_line(out + " = fptosi double " + value + " to i64");
            return out;
        }
        return value;
    }

    std::string CodeGenerator::convert_value(const std::string& value,
        const AST::Type& from, const AST::Type& to) {
        // 算术类型间转换；指针/函数指针之间直接通过 ptr bitcast
        // Convert arithmetic types; pointer and function-pointer values pass through as ptr
        if (from == to) return value;
        if (from.kind == TypeKind::Array &&
            (to.kind == TypeKind::Pointer || to.kind == TypeKind::Function)) {
            // 数组名已按首元素指针求值，直接作为指针使用
            // An array expression already evaluates to its decayed element pointer
            return value;
        }
       if (to.kind == TypeKind::Pointer || to.kind == TypeKind::Function ||
           from.kind == TypeKind::Pointer || from.kind == TypeKind::Function ||
           to.kind == TypeKind::File || from.kind == TypeKind::File) {
           // 从整数常量 0 转空指针由调用者负责；这里保留 ptr 值
            // Integer zero to null is handled by callers; keep pointer values as-is
            if (from.kind == TypeKind::Pointer || from.kind == TypeKind::Function ||
                from.kind == TypeKind::File) return value;
            if (to.kind == TypeKind::Pointer || to.kind == TypeKind::Function) {
                std::string out = new_temp("inttoptr");
                if (from.kind == TypeKind::Int) {
                    emit_line(out + " = inttoptr i32 " + value + " to ptr");
                } else if (from.kind == TypeKind::Char) {
                    emit_line(out + " = inttoptr i8 " + value + " to ptr");
                } else {
                    emit_line(out + " = inttoptr i64 0 to ptr");
                }
                return out;
            }
        }
        // 整数与浮点互转
        // Conversions between integers and floating point
        std::string tmp = new_temp("cast");
        if (to.kind == TypeKind::Float) {
            if (from.kind == TypeKind::Double) {
                emit_line(tmp + " = fptrunc double " + value + " to float");
            } else if (from.kind == TypeKind::Int || from.kind == TypeKind::Char) {
                if (from.kind == TypeKind::Int) {
                    emit_line(tmp + " = sitofp i32 " + value + " to float");
                } else {
                    emit_line(tmp + " = sitofp i8 " + value + " to float");
                }
            } else if (from.kind == TypeKind::Bool) {
                emit_line(tmp + " = uitofp i8 " + value + " to float");
            }
            return tmp;
        }
        if (to.kind == TypeKind::Double) {
            if (from.kind == TypeKind::Float) {
                emit_line(tmp + " = fpext float " + value + " to double");
            } else if (from.kind == TypeKind::Int || from.kind == TypeKind::Char) {
                if (from.kind == TypeKind::Int) {
                    emit_line(tmp + " = sitofp i32 " + value + " to double");
                } else {
                    emit_line(tmp + " = sitofp i8 " + value + " to double");
                }
            } else if (from.kind == TypeKind::Bool) {
                emit_line(tmp + " = uitofp i8 " + value + " to double");
            }
            return tmp;
        }
        if (from.kind == TypeKind::Float) {
            if (to.kind == TypeKind::Int) {
                emit_line(tmp + " = fptosi float " + value + " to i32");
            } else if (to.kind == TypeKind::Char) {
                emit_line(tmp + " = fptosi float " + value + " to i8");
            } else if (to.kind == TypeKind::Bool) {
                emit_line(tmp + " = fptoui float " + value + " to i8");
            }
            return tmp;
        }
        if (from.kind == TypeKind::Double) {
            if (to.kind == TypeKind::Int) {
                emit_line(tmp + " = fptosi double " + value + " to i32");
            } else if (to.kind == TypeKind::Char) {
                emit_line(tmp + " = fptosi double " + value + " to i8");
            } else if (to.kind == TypeKind::Bool) {
                emit_line(tmp + " = fptoui double " + value + " to i8");
            }
            return tmp;
        }
        // 整型间：char/bool 使用 i8，int 使用 i32
        // Integer-to-integer conversions are i8/i32 based
        std::string from_ir = from.kind == TypeKind::Int ? "i32" : "i8";
        std::string to_ir = to.kind == TypeKind::Int ? "i32" : "i8";
        bool to_unsigned = to.kind == TypeKind::Bool;
        if (from_ir == to_ir) return value;
        if (to_ir == "i32" && from_ir == "i8") {
            if (from.kind == TypeKind::Bool) {
                emit_line(tmp + " = zext i8 " + value + " to i32");
            } else {
                emit_line(tmp + " = sext i8 " + value + " to i32");
            }
        } else if (to_ir == "i8" && from_ir == "i32") {
            emit_line(tmp + " = trunc i32 " + value + " to i8");
        }
        (void)to_unsigned;
        return tmp;
    }

    std::string CodeGenerator::truth_condition(const std::string& value, const AST::Type& type) {
        if (type.kind == TypeKind::Bool) {
            std::string tmp = new_temp("cond");
            emit_line(tmp + " = trunc i8 " + value + " to i1");
            return tmp;
        }
        if (type.kind == TypeKind::Int) {
            std::string tmp = new_temp("cond");
            emit_line(tmp + " = icmp ne i32 " + value + ", 0");
            return tmp;
        }
        if (type.kind == TypeKind::Char) {
            std::string tmp = new_temp("cond");
            emit_line(tmp + " = icmp ne i8 " + value + ", 0");
            return tmp;
        }
        if (type.kind == TypeKind::Float) {
            std::string tmp = new_temp("cond");
            emit_line(tmp + " = fcmp une float " + value + ", 0.0");
            return tmp;
        }
        if (type.kind == TypeKind::Double) {
            std::string tmp = new_temp("cond");
            emit_line(tmp + " = fcmp une double " + value + ", 0.0");
            return tmp;
        }
        if (type.kind == TypeKind::Pointer || type.kind == TypeKind::Function ||
            type.kind == TypeKind::File) {
            std::string tmp = new_temp("cond");
            emit_line(tmp + " = icmp ne ptr " + value + ", null");
            return tmp;
        }
        return "true";
    }

    CodeGenerator::ExprValue CodeGenerator::gen_primary(AST::PrimaryExpression* expr) {
        ExprValue out;
        out.type = resolved_type(expr);
        switch (expr->kind) {
        case AST::PrimaryExpression::Kind::Literal: {
            switch (expr->literal_token.type) {
            case TokenType::IntegerLiteral: {
                std::string_view lexeme = expr->literal_token.lexeme;
                long long value = 0;
                int base = 10;
                if (lexeme.size() > 2 && lexeme[0] == '0' &&
                    (lexeme[1] == 'x' || lexeme[1] == 'X')) {
                    base = 16;
                    lexeme = lexeme.substr(2);
                } else if (lexeme.size() > 1 && lexeme[0] == '0') {
                    base = 8;
                    lexeme = lexeme.substr(1);
                }
                auto res = std::from_chars(lexeme.data(), lexeme.data() + lexeme.size(), value, base);
                out.value = std::to_string(value);
                break;
            }
            case TokenType::FloatLiteral: {
                std::string text = std::string(expr->literal_token.lexeme);
                if (!text.empty() && (text.back() == 'f' || text.back() == 'F')) {
                    text.pop_back();
                }
                double d = std::strtod(text.c_str(), nullptr);
                std::ostringstream fmt;
                fmt.precision(17);
                fmt << d;
                out.value = llvm_float_constant_text(fmt.str());
                break;
            }
            case TokenType::CharLiteral: {
                std::string bytes = decode_escaped_bytes(expr->literal_token.lexeme, true);
                out.value = std::to_string(static_cast<int>(static_cast<unsigned char>(
                    bytes.empty() ? '\0' : bytes[0])));
                break;
            }
            case TokenType::BoolLiteral:
                out.value = expr->literal_token.lexeme == "true" ? "1" : "0";
                break;
            case TokenType::StringLiteral: {
                std::string type_text = llvm_type(Type::make_string());
                std::string temp = emit_alloca(type_text, "stringtemp");
                // 具体字节数据在下面按字面量注册并填充
                // Fill the literal bytes through the shared helper below
                std::string bytes = decode_escaped_bytes(expr->literal_token.lexeme, false);
                auto found = string_literal_ids_.find(bytes);
                std::string global;
                if (found != string_literal_ids_.end()) {
                    global = found->second;
                } else {
                    global = "str" + std::to_string(string_literals_.size());
                    string_literal_ids_[bytes] = global;
                    string_literals_.push_back({ bytes, global });
                }
                std::string ptr = new_temp("strdata");
                std::size_t array_len = bytes.empty() ? 1u : bytes.size();
                emit_line(ptr + " = getelementptr [" + std::to_string(array_len) +
                    " x i8], ptr @" + global + ", i64 0, i64 0");
                emit_line("call void @gallt_string_init(ptr " + temp +
                    ", ptr " + ptr + ", i64 " + std::to_string(bytes.size()) + ")");
                out.value = temp;
                out.owned_string = temp;
                break;
            }
            default:
                break;
            }
            return out;
        }
        case AST::PrimaryExpression::Kind::Identifier: {
            LocalInfo* local = lookup_local(expr->identifier);
            if (local != nullptr) {
                out.is_lvalue = true;
                out.address = local->address;
                if (local->type.kind == TypeKind::Array) {
                    // 数组表达式退化为首元素指针
                    // Array expressions decay to a pointer to the first element
                    out.value = new_temp("arraydecay");
                    std::string elem_type = llvm_type(*local->type.element_type);
                    std::size_t n = local->type.array_size.value_or(0);
                    std::string array_type = "[" + std::to_string(n) + " x " + elem_type + "]";
                    emit_line(out.value + " = getelementptr " + array_type +
                        ", ptr " + local->address + ", i64 0, i64 0");
                } else {
                    std::string type_text = llvm_type(local->type);
                    out.value = new_temp("load");
                    emit_line(out.value + " = load " + type_text + ", ptr " + local->address);
                }
                return out;
            }
            // 函数名作为值（第 13 章）：产生函数指针常量。
            // 优先使用类型检查阶段解析出的重载版本，其次按名字查找用户函数 / extern。
            // A function name used as a value yields a function pointer constant (§13); the
            // checker's overload resolution is preferred, then a name lookup.
            {
                std::string reference = function_reference_for(expr);
                if (reference.empty()) {
                    reference = function_reference(expr->identifier);
                }
                if (!reference.empty()) {
                    out.value = reference;
                    return out;
                }
            }
            // 未定义标识符在语义阶段已报告，这里不继续
            // Undefined identifiers were reported semantically; stop here
            return out;
        }
        case AST::PrimaryExpression::Kind::Parens:
            return gen_expr(expr->paren_expr.get());
        case AST::PrimaryExpression::Kind::Null:
            out.value = "null";
            return out;
        case AST::PrimaryExpression::Kind::Heap: {
            AST::Type alloc_type = expr->heap_type;
            std::string count = "1";
            if (expr->heap_size) {
                ExprValue n = gen_expr(expr->heap_size.get());
                count = to_i64_value(n.value, n.type);
            }
            std::size_t elem_size = type_size(alloc_type);
            std::string total = new_temp("heapsize");
            emit_line(total + " = mul i64 " + count + ", " + std::to_string(elem_size));
            out.value = new_temp("heap");
            emit_line(out.value + " = call ptr @gallt_alloc_bytes(i64 " + total + ")");
            return out;
        }
        case AST::PrimaryExpression::Kind::Construct:
        case AST::PrimaryExpression::Kind::PlacementConstruct: {
            // construct [类型]([实参]) [at [指针]]（Gallt 0.3.txt §20）
            // construct [type]([args]) [at [pointer]] (Gallt 0.3.txt §20)
            AST::Type constructed = expr->construct_type;
            std::string storage;
            if (expr->kind == AST::PrimaryExpression::Kind::PlacementConstruct) {
                ExprValue target = gen_expr(expr->placement_target.get());
                storage = target.value;
            }
            else {
                std::size_t elem_size = type_size(constructed);
                storage = new_temp("construct");
                emit_line(storage + " = call ptr @gallt_alloc_bytes(i64 " +
                    std::to_string(elem_size) + ")");
            }
            out.type = AST::Type::make_pointer(std::make_shared<AST::Type>(constructed));
            out.value = storage;
            // 用户构造函数
            if (!expr->lowered_ctor.empty()) {
                // 第 20 章：优先使用类型检查阶段按实参类型解析出的构造函数重载
                // §20: prefer the constructor overload resolved by the type checker
                std::string ctor_name = expr->lowered_ctor;
                if (auto resolved = resolved_functions_.find(expr);
                    resolved != resolved_functions_.end() && resolved->second != nullptr) {
                    ctor_name = resolved->second->name;
                }
                std::string callee = function_reference(ctor_name);
                if (!callee.empty()) {
                    // 第 8 章：补齐构造函数被省略的默认实参
                    // §8: fill in the constructor's omitted default arguments
                    std::vector<AST::Expression*> ctor_args;
                    for (auto& arg : expr->construct_args) ctor_args.push_back(arg.get());
                    collect_constructor_defaults(ctor_name, ctor_args);
                    std::string call_text = "call void " + callee + "(ptr " + storage;
                    for (AST::Expression* arg : ctor_args) {
                        ExprValue value = gen_expr(arg);
                        std::string type_text = llvm_type(value.type);
                        if (value.type.kind == TypeKind::String) {
                            std::string addr = !value.address.empty() ? value.address : value.value;
                            std::string agg = new_temp("ctorstr");
                            emit_line(agg + " = load %struct.gallt.string, ptr " + addr);
                            value.value = agg;
                            type_text = "%struct.gallt.string";
                        }
                        call_text += ", " + type_text + " " + value.value;
                        destroy_owned_string(value);
                    }
                    call_text += ")";
                    emit_line(call_text);
                }
                return out;
            }
            // 无用户构造函数：Gallt 0.3.txt §20 要求“执行默认初始化”
            // （成员有默认值时使用默认值，无默认值保持未初始化；不按实参做聚合赋值）
            // No user constructor: §20 requires default initialization (declared member
            // defaults apply; remaining members stay uninitialized)
            AST::ArrayInitializer empty_init(expr->location,
                std::vector<std::unique_ptr<AST::Initializer>>{});
            emit_struct_brace_initialization(storage, constructed, &empty_init);
            return out;
        }
        case AST::PrimaryExpression::Kind::CopyMove: {
            // Gallt 0.3.txt §20：表达式中的 copy/move/deep_copy/shallow_copy
            // 物化一个临时对象并在完整表达式结束时析构
            // §20: copy/move/deep_copy/shallow_copy in an expression materialize a temporary
            // that is destroyed at the end of its full expression
            AST::Type operand_type;
            std::string source_address = operand_address(expr->paren_expr.get(), &operand_type);
            if (source_address.empty() ||
                (operand_type.kind != AST::TypeKind::Struct &&
                    operand_type.kind != AST::TypeKind::String)) {
                return gen_expr(expr->paren_expr.get());
            }
            std::string temp = emit_alloca(llvm_type(operand_type), "copy_move_temp");
            switch (expr->copy_move_kind) {
            case AST::PrimaryExpression::CopyMoveKind::Copy:
                emit_memberwise_copy(operand_type, temp, source_address, false);
                break;
            case AST::PrimaryExpression::CopyMoveKind::Move:
                emit_memberwise_move(operand_type, temp, source_address, false);
                break;
            case AST::PrimaryExpression::CopyMoveKind::DeepCopy:
                emit_deep_copy(operand_type, temp, source_address);
                break;
            case AST::PrimaryExpression::CopyMoveKind::ShallowCopy:
                emit_shallow_copy(operand_type, temp, source_address);
                break;
            }
            // 临时对象在完整表达式结束时析构（浅拷贝的双重释放属文档规定的未定义行为）
            // The temporary is destroyed at the end of the full expression (a shallow copy
            // that double-frees is documented undefined behaviour)
            bool needs_cleanup = operand_type.kind == AST::TypeKind::String ||
                type_contains_string(operand_type);
            if (!needs_cleanup && operand_type.kind == AST::TypeKind::Struct) {
                auto def_it = struct_by_name_.find(operand_type.struct_name);
                needs_cleanup = def_it != struct_by_name_.end() && def_it->second != nullptr &&
                    def_it->second->needs_destruction;
            }
            if (needs_cleanup) {
                CleanupRecord record;
                record.type = operand_type;
                record.address = temp;
                statement_temporaries_.push_back(record);
            }
            out.type = operand_type;
            out.address = temp;
            out.is_lvalue = true;
            out.value = new_temp("copy_move_value");
            emit_line(out.value + " = load " + llvm_type(operand_type) + ", ptr " + temp);
            return out;
        }
        case AST::PrimaryExpression::Kind::QualifiedName:
            // 泛型限定名已在展开阶段解析为具体名字，正常编译不会到达
            // Generic qualified names are resolved during expansion; unreachable normally
            return out;
        case AST::PrimaryExpression::Kind::NamespaceQualified:
            // 命名空间限定名已在命名空间降低阶段解析为具体名字，正常编译不会到达
            // Namespace-qualified names are resolved by the namespace lowering pass;
            // unreachable normally
            return out;
        }
        return out;
    }

    std::string CodeGenerator::gen_address(AST::Expression* expr) {
        if (auto* prim = dynamic_cast<AST::PrimaryExpression*>(expr)) {
            if (prim->kind == AST::PrimaryExpression::Kind::Identifier) {
                LocalInfo* local = lookup_local(prim->identifier);
                if (local) return local->address;
            }
            if (prim->kind == AST::PrimaryExpression::Kind::Parens && prim->paren_expr) {
                return gen_address(prim->paren_expr.get());
            }
            return std::string();
        }
        if (auto* unary = dynamic_cast<AST::UnaryExpression*>(expr)) {
            if (unary->op == AST::UnaryExpression::Operator::Dereference) {
                ExprValue p = gen_expr(unary->operand.get());
                return p.value;
            }
            return std::string();
        }
        if (auto* post = dynamic_cast<AST::PostfixExpression*>(expr)) {
            if (post->op == AST::PostfixExpression::Operator::Subscript) {
                ExprValue base = gen_expr(post->base.get());
                ExprValue idx = gen_expr(post->subscript_expr.get());
                std::string i64 = to_i64_value(idx.value, idx.type);
                std::string elem_type;
                std::string base_pointer = base.value;
                if (base.type.kind == TypeKind::Array && base.type.element_type) {
                    elem_type = llvm_type(*base.type.element_type);
                    // 数组类型的左值必须取其存储地址（数组退化为首元素指针），
                    // 否则会把 load 出来的聚合值当作指针使用
                    // An array-typed lvalue must use its storage address; using the loaded
                    // aggregate value as a pointer is invalid IR
                    std::string address = gen_address(post->base.get());
                    if (!address.empty()) {
                        base_pointer = address;
                    }
                } else if (base.type.kind == TypeKind::Pointer && base.type.pointee_type) {
                    elem_type = llvm_type(*base.type.pointee_type);
                } else {
                    return std::string();
                }
                std::string ptr = new_temp("indexptr");
                emit_line(ptr + " = getelementptr " + elem_type +
                    ", ptr " + base_pointer + ", i64 " + i64);
                return ptr;
            }
            if (post->op == AST::PostfixExpression::Operator::Dot ||
                post->op == AST::PostfixExpression::Operator::Arrow) {
                ExprValue base = gen_expr(post->base.get());
                AST::Type struct_type;
                std::string base_addr;
                if (post->op == AST::PostfixExpression::Operator::Arrow) {
                    if (base.type.kind == TypeKind::Pointer && base.type.pointee_type) {
                        struct_type = *base.type.pointee_type;
                    }
                    base_addr = base.value;
                } else {
                    struct_type = base.type;
                    if (base.is_lvalue && !base.address.empty()) {
                        base_addr = base.address;
                    } else {
                        return std::string();
                    }
                }
                if (struct_type.kind != TypeKind::Struct) return std::string();
                auto struct_it = struct_by_name_.find(struct_type.struct_name);
                if (struct_it == struct_by_name_.end()) return std::string();
                size_t index = 0;
                bool found = false;
                for (size_t i = 0; i < struct_it->second->members.size(); ++i) {
                    if (struct_it->second->members[i].name == post->member_name) {
                        index = i;
                        found = true;
                        break;
                    }
                }
                if (!found) return std::string();
                std::string ptr = new_temp("memberptr");
                emit_line(ptr + " = getelementptr " + llvm_type(struct_type) +
                    ", ptr " + base_addr + ", i32 0, i32 " + std::to_string(index));
                return ptr;
            }
            if (post->op == AST::PostfixExpression::Operator::Cast ||
                post->op == AST::PostfixExpression::Operator::Increment ||
                post->op == AST::PostfixExpression::Operator::Decrement) {
                return std::string();
            }
        }
        return std::string();
    }

    std::string CodeGenerator::gen_pointer_value(AST::Expression* expr) {
        return gen_expr(expr).value;
    }

    CodeGenerator::ExprValue CodeGenerator::gen_unary(AST::UnaryExpression* expr) {
        ExprValue out;
        out.type = resolved_type(expr);
        if (expr->op == AST::UnaryExpression::Operator::AddressOf) {
            // &函数名 -> 函数指针常量
            // &function-name yields a function pointer constant
            if (auto* prim = dynamic_cast<AST::PrimaryExpression*>(expr->operand.get())) {
                if (prim->kind == AST::PrimaryExpression::Kind::Identifier) {
                    // 优先使用类型检查阶段解析出的重载版本（第 18 章）
                    // Prefer the overload resolved by the type checker (§18)
                    std::string reference = function_reference_for(prim);
                    if (reference.empty()) {
                        reference = function_reference(prim->identifier);
                    }
                    if (!reference.empty()) {
                        out.value = reference;
                        return out;
                    }
                }
            }
            std::string address = gen_address(expr->operand.get());
            if (!address.empty()) {
                out.value = address;
            }
            return out;
        }
        if (expr->op == AST::UnaryExpression::Operator::Dereference) {
            ExprValue p = gen_expr(expr->operand.get());
            if (p.type.kind != TypeKind::Pointer) return out;
            out.is_lvalue = true;
            out.address = p.value;
            out.type = resolved_type(expr);
            std::string type_text = llvm_type(out.type);
            out.value = new_temp("deref");
            emit_line(out.value + " = load " + type_text + ", ptr " + p.value);
            return out;
        }
        if (expr->op == AST::UnaryExpression::Operator::LogicalNot) {
            ExprValue v = gen_expr(expr->operand.get());
            std::string cond = truth_condition(v.value, v.type);
            std::string notval = new_temp("notcond");
            emit_line(notval + " = xor i1 " + cond + ", true");
            out.value = new_temp("notbool");
            emit_line(out.value + " = zext i1 " + notval + " to i8");
            return out;
        }
        if (expr->op == AST::UnaryExpression::Operator::UnaryPlus ||
            expr->op == AST::UnaryExpression::Operator::UnaryMinus) {
            // 一元正负号：先求值操作数，再按结果类型生成取负/取正
            // Unary signs: evaluate the operand, then negate (or just promote)
            ExprValue v = gen_expr(expr->operand.get());
            out.type = resolved_type(expr);
            if (expr->op == AST::UnaryExpression::Operator::UnaryPlus) {
                // 一元正号只做类型提升，不产生额外运算
                // Unary plus only applies the type promotion
                out.value = convert_value(v.value, v.type, out.type);
                return out;
            }
            if (out.type.kind == TypeKind::Float || out.type.kind == TypeKind::Double) {
                std::string type_text = llvm_type(out.type);
                std::string operand_value = convert_value(v.value, v.type, out.type);
                out.value = new_temp("fneg");
                emit_line(out.value + " = fneg " + type_text + " " + operand_value);
                return out;
            }
            // 整数取负：在 i32 上做 0 - x，再按结果类型截断（与二元算术一致）
            // Integer negation: 0 - x in i32, then truncate to the result type
            std::string operand_value = convert_value(v.value, v.type, Type::make_int());
            std::string negated = new_temp("neg");
            emit_line(negated + " = sub i32 0, " + operand_value);
            out.value = convert_value(negated, Type::make_int(), out.type);
            return out;
        }
        if (expr->op == AST::UnaryExpression::Operator::Increment ||
            expr->op == AST::UnaryExpression::Operator::Decrement) {
            std::string address = gen_address(expr->operand.get());
            ExprValue old = gen_expr(expr->operand.get());
            if (address.empty()) return out;
            std::string one;
            std::string type_text = llvm_type(old.type);
            if (old.type.kind == TypeKind::Pointer) {
                std::string pointee = old.type.pointee_type ? llvm_type(*old.type.pointee_type) : "i8";
                std::string delta = expr->op == AST::UnaryExpression::Operator::Increment ? "1" : "-1";
                std::string ptr = new_temp("incptr");
                emit_line(ptr + " = getelementptr " + pointee + ", ptr " + old.value +
                    ", i64 " + delta);
                emit_line("store ptr " + ptr + ", ptr " + address);
                out.value = ptr;
                return out;
            }
            std::string opcode;
            if (old.type.kind == TypeKind::Float || old.type.kind == TypeKind::Double) {
                opcode = expr->op == AST::UnaryExpression::Operator::Increment ? "fadd" : "fsub";
                one = (old.type.kind == TypeKind::Double) ? "1.0" : "1.0";
            } else {
                opcode = expr->op == AST::UnaryExpression::Operator::Increment ? "add" : "sub";
                one = "1";
            }
            std::string updated = new_temp("updated");
            emit_line(updated + " = " + opcode + " " + type_text + " " + old.value + ", " + one);
            emit_line("store " + type_text + " " + updated + ", ptr " + address);
            out.value = updated;
            return out;
        }
        return out;
    }

    std::string CodeGenerator::function_reference(const std::string& name) {
        // 查程序中的用户函数与 extern 声明，返回 LLVM 函数名
        // Find a user function or extern declaration and return its LLVM name
        if (name == "main") return "@glt_main";
        for (const auto& top : program_->top_levels) {
            if (auto* f = dynamic_cast<AST::FunctionDefinition*>(top.get())) {
                if (f->name == name) return "@glt_" + name;
            }
        }
        for (const auto& top : program_->top_levels) {
            if (auto* e = dynamic_cast<AST::ExternDeclaration*>(top.get())) {
                if (e->name == name) return "@" + extern_ir_symbol(e);
            }
        }
        return std::string();
    }

    std::string CodeGenerator::extern_ir_symbol(const AST::ExternDeclaration* ext) const {
        // C 符号名；当同名 extern 具有不同签名时，为保证 IR 合法改用带签名的修饰名
        // （此时要求 C 库导出对应的修饰符号；C 本身不支持同名重载）
        // The C symbol; when same-named externs differ in signature the symbol is decorated
        // so the IR stays valid (the C library must then export that decorated name)
        auto cached = extern_ir_symbols_.find(ext);
        if (cached != extern_ir_symbols_.end()) return cached->second;
        std::string candidate = ext->c_symbol_name;
        for (const auto& top : program_->top_levels) {
            auto* other = dynamic_cast<AST::ExternDeclaration*>(top.get());
            if (other == nullptr || other == ext) continue;
            if (other->c_symbol_name != ext->c_symbol_name) continue;
            // 同一个 C 符号被两个签名使用 → 需要修饰名区分
            // Two signatures share one C symbol -> decorate to keep the IR valid
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

    CodeGenerator::ExprValue CodeGenerator::gen_postfix(AST::PostfixExpression* expr) {
        ExprValue out;
        out.type = resolved_type(expr);
        switch (expr->op) {
        case AST::PostfixExpression::Operator::Subscript: {
            std::string address = gen_address(expr);
            if (address.empty()) return out;
            out.is_lvalue = true;
            out.address = address;
            if (out.type.kind == TypeKind::Array && out.type.element_type) {
                // 多维数组的一行本身是数组类型：退化为首元素指针，而不是 load 聚合值
                // （Gallt 0.3.txt §7：数组表达式退化为指向首元素的指针）
                // A row of a multi-dimensional array is itself an array: decay to a pointer
                // to its first element instead of loading the aggregate (§7)
                out.value = new_temp("arraydecay");
                emit_line(out.value + " = getelementptr " + llvm_type(out.type) +
                    ", ptr " + address + ", i64 0, i64 0");
                return out;
            }
            std::string type_text = llvm_type(out.type);
            out.value = new_temp("subscript_load");
            emit_line(out.value + " = load " + type_text + ", ptr " + address);
            return out;
        }
        case AST::PostfixExpression::Operator::Dot:
        case AST::PostfixExpression::Operator::Arrow: {
            std::string address = gen_address(expr);
            if (address.empty()) return out;
            out.is_lvalue = true;
            out.address = address;
            if (out.type.kind == TypeKind::Array && out.type.element_type) {
                // 数组成员（含多维数组的一行）同样退化为首元素指针
                // Array members (including a row of a multi-dimensional array) decay too
                out.value = new_temp("arraydecay");
                emit_line(out.value + " = getelementptr " + llvm_type(out.type) +
                    ", ptr " + address + ", i64 0, i64 0");
                return out;
            }
            std::string type_text = llvm_type(out.type);
            out.value = new_temp("member_load");
            emit_line(out.value + " = load " + type_text + ", ptr " + address);
            return out;
        }
        case AST::PostfixExpression::Operator::Cast: {
            ExprValue operand = gen_expr(expr->base.get());
            out.value = convert_value(operand.value, operand.type, expr->cast_type);
            return out;
        }
        case AST::PostfixExpression::Operator::Increment:
        case AST::PostfixExpression::Operator::Decrement: {
            std::string address = gen_address(expr->base.get());
            if (address.empty()) return out;
            std::string type_text = llvm_type(out.type);
            std::string old_val = new_temp("postold");
            emit_line(old_val + " = load " + type_text + ", ptr " + address);
            std::string updated = new_temp("postnew");
            std::string one = (out.type.kind == TypeKind::Float ||
                out.type.kind == TypeKind::Double) ? "1.0" : "1";
            std::string opcode;
            if (out.type.kind == TypeKind::Float || out.type.kind == TypeKind::Double) {
                opcode = expr->op == AST::PostfixExpression::Operator::Increment ? "fadd" : "fsub";
            } else if (out.type.kind == TypeKind::Pointer) {
                std::string pointee = out.type.pointee_type ? llvm_type(*out.type.pointee_type) : "i8";
                std::string step = expr->op == AST::PostfixExpression::Operator::Increment ? "1" : "-1";
                updated = new_temp("postptr");
                emit_line(updated + " = getelementptr " + pointee + ", ptr " + old_val +
                    ", i64 " + step);
                emit_line("store ptr " + updated + ", ptr " + address);
                out.value = old_val;
                out.is_lvalue = true;
                out.address = address;
                return out;
            } else {
                opcode = expr->op == AST::PostfixExpression::Operator::Increment ? "add" : "sub";
            }
            emit_line(updated + " = " + opcode + " " + type_text + " " + old_val + ", " + one);
            emit_line("store " + type_text + " " + updated + ", ptr " + address);
            out.value = old_val;
            out.is_lvalue = true;
            out.address = address;
            return out;
        }
        case AST::PostfixExpression::Operator::FunctionCall: {
            AST::PrimaryExpression* direct = nullptr;
            if (auto* prim = dynamic_cast<AST::PrimaryExpression*>(expr->base.get())) {
                if (prim->kind == AST::PrimaryExpression::Kind::Identifier) {
                    direct = prim;
                }
            }
            std::string direct_name = direct ? direct->identifier : std::string();
            if (direct_name == "output") {
                emit_output_call(expr);
                return out;
            }
            if (direct_name == "input") {
                emit_input_call(expr, out);
                return out;
            }
            if (direct_name == "free") {
                emit_free_call(expr);
                return out;
            }
            // Gallt 0.2.txt §17：文件操作内置函数
            // Gallt 0.2.txt §17: file-operation builtins
            if (is_file_builtin_name(direct_name)) {
                return emit_file_builtin_call(expr, direct_name);
            }
            if (direct_name == "size" || direct_name == "align") {
                return emit_size_align_call(expr, direct_name == "size");
            }
            // 显式析构调用：【对象】.destructor() 或 【指针】->destructor()（Gallt 0.3.txt §20）
            // Explicit destructor call: obj.destructor() / ptr->destructor()
            // T(args) 值临时对象（Gallt 0.3.txt §20）：在栈上构造，完整表达式结束时析构
            // T(args) value temporary: constructed on the stack, destroyed at the end of the
            // full expression
            if (direct != nullptr) {
                auto struct_it = struct_by_name_.find(direct->identifier);
                if (struct_it != struct_by_name_.end() && struct_it->second != nullptr &&
                    !struct_it->second->constructor_names.empty()) {
                    AST::StructDefinition* def = struct_it->second;
                    AST::Type temp_type = AST::Type::make_struct(direct->identifier);
                    std::string storage = emit_alloca(llvm_type(temp_type), "value_temp");
                    // 第 20 章：类型检查阶段已按实参类型解析出构造函数；回退到按个数区间选择
                    // §20: the checker already resolved the constructor by argument type;
                    // fall back to the argument-count range only when it is unavailable
                    std::string resolved_ctor_name;
                    if (auto resolved = resolved_functions_.find(direct);
                        resolved != resolved_functions_.end() && resolved->second != nullptr) {
                        resolved_ctor_name = resolved->second->name;
                    }
                    // 按实参个数区间（含默认参数）选择构造函数
                    // Pick the constructor by argument-count range, honouring defaults
                    std::size_t index = 0;
                    for (std::size_t i = 0; i < def->constructor_names.size(); ++i) {
                        const std::vector<AST::Type>& params = def->constructor_param_types[i];
                        std::size_t required = params.size();
                        auto fit = function_by_name_.find(def->constructor_names[i]);
                        if (fit != function_by_name_.end() && fit->second != nullptr) {
                            const std::vector<std::unique_ptr<AST::Expression>>& defaults =
                                fit->second->param_defaults;
                            for (std::size_t k = defaults.size(); k > 0; --k) {
                                if (defaults[k - 1] != nullptr) required = k - 1;
                                else break;
                            }
                        }
                        if (expr->arguments.size() <= params.size() &&
                            expr->arguments.size() >= required) {
                            index = i;
                            break;
                        }
                    }
                    std::string ctor_name = resolved_ctor_name.empty()
                        ? def->constructor_names[index] : resolved_ctor_name;
                    std::string callee = function_reference(ctor_name);
                    if (!callee.empty()) {
                        // 第 8 章：补齐构造函数被省略的默认实参
                        // §8: fill in the constructor's omitted default arguments
                        std::vector<AST::Expression*> ctor_args;
                        for (auto& arg : expr->arguments) ctor_args.push_back(arg.get());
                        collect_constructor_defaults(ctor_name, ctor_args);
                        std::string call_text = "call void " + callee + "(ptr " + storage;
                        const std::vector<AST::Type>& params =
                            def->constructor_param_types[index];
                        for (std::size_t i = 0; i < ctor_args.size(); ++i) {
                            ExprValue value = gen_expr(ctor_args[i]);
                            AST::Type want = i < params.size() ? params[i] : value.type;
                            if (want.kind == TypeKind::String &&
                                value.type.kind == TypeKind::String) {
                                std::string addr = !value.address.empty()
                                    ? value.address : value.value;
                                std::string agg = new_temp("temp_str");
                                emit_line(agg + " = load %struct.gallt.string, ptr " + addr);
                                call_text += ", %struct.gallt.string " + agg;
                            }
                            else {
                                call_text += ", " + llvm_type(want) + " " +
                                    convert_value(value.value, value.type, want);
                            }
                            destroy_owned_string(value);
                        }
                        call_text += ")";
                        emit_line(call_text);
                    }
                    // 登记析构（所在完整表达式结束时调用）
                    if (def->needs_destruction) {
                        CleanupRecord record;
                        record.type = temp_type;
                        record.address = storage;
                        statement_temporaries_.push_back(record);
                    }
                    out.type = temp_type;
                    out.value = storage;
                    out.address = storage;
                    out.is_lvalue = true;
                    return out;
                }
            }
            if (direct == nullptr) {
                if (auto* member_access = dynamic_cast<AST::PostfixExpression*>(expr->base.get())) {
                    if ((member_access->op == AST::PostfixExpression::Operator::Dot ||
                        member_access->op == AST::PostfixExpression::Operator::Arrow) &&
                        member_access->member_name == "destructor") {
                        AST::Type owner = resolved_type(member_access->base.get());
                        if (member_access->op == AST::PostfixExpression::Operator::Arrow &&
                            owner.kind == TypeKind::Pointer && owner.pointee_type) {
                            owner = *owner.pointee_type;
                        }
                        if (owner.kind == TypeKind::Struct) {
                            // 箭头形式传递指针值；点形式传递对象地址
                            // Arrow passes the pointer value; dot passes the object address
                            ExprValue base_value = gen_expr(member_access->base.get());
                            std::string address = base_value.value;
                            if (member_access->op == AST::PostfixExpression::Operator::Dot) {
                                std::string object_address =
                                    gen_address(member_access->base.get());
                                if (!object_address.empty()) address = object_address;
                            }
                            auto it = struct_by_name_.find(owner.struct_name);
                            if (it != struct_by_name_.end() && it->second != nullptr &&
                                !it->second->destructor_name.empty()) {
                                std::string callee =
                                    function_reference(it->second->destructor_name);
                                if (!callee.empty() && !address.empty()) {
                                    emit_line("call void " + callee + "(ptr " + address + ")");
                                }
                            }
                        }
                        return out;
                    }
                }
            }

            // 收集函数签名，用于参数转换与 call 返回类型
            // Gather the signature used for argument coercion and call return type
            AST::Type return_type = out.type;
            AST::Type func_type;
            std::vector<AST::Type> params;
            std::string callee;
            bool pointer_call = false;
            if (!direct_name.empty()) {
                if (direct_name == "main") {
                    callee = "@main";
                } else {
                    // 第 18 章：类型检查阶段已解析出具体重载；优先按其“当前”名字生成调用，
                    // 这样即便重载集在之后被命名修饰，调用点依然指向正确的函数
                    // §18: the checker already resolved the overload; prefer its current name so
                    // a call site stays valid even if the overload set is mangled later
                    if (AST::PrimaryExpression* callee_node =
                        dynamic_cast<AST::PrimaryExpression*>(expr->base.get())) {
                        auto resolved = resolved_functions_.find(callee_node);
                        if (resolved != resolved_functions_.end() && resolved->second != nullptr) {
                            const AST::FunctionDefinition* f = resolved->second;
                            callee = "@glt_" + f->name;
                            params = f->parameters;
                            func_type = AST::Type::make_function(
                                std::make_shared<AST::Type>(f->return_type), params);
                        }
                        else {
                            auto resolved_ext = resolved_externs_.find(callee_node);
                            if (resolved_ext != resolved_externs_.end() &&
                                resolved_ext->second != nullptr) {
                                const AST::ExternDeclaration* e = resolved_ext->second;
                                callee = "@" + extern_ir_symbol(e);
                                params = e->parameters;
                                func_type = AST::Type::make_function(
                                    std::make_shared<AST::Type>(e->return_type), params);
                            }
                        }
                    }
                    if (!callee.empty()) {
                        // 已通过解析结果确定，跳过按名字查找
                    }
                    else {
                    // 使用预建的签名索引（O(1)），避免每次都扫描全部顶层节点
                    // Use the prebuilt signature index instead of scanning every call
                    auto fit = function_by_name_.find(direct_name);
                    if (fit != function_by_name_.end()) {
                        AST::FunctionDefinition* f = fit->second;
                        callee = "@glt_" + f->name;
                        params = f->parameters;
                        func_type = AST::Type::make_function(
                            std::make_shared<AST::Type>(f->return_type), params);
                    } else {
                        auto eit = extern_by_name_.find(direct_name);
                        if (eit != extern_by_name_.end()) {
                            AST::ExternDeclaration* e = eit->second;
                            callee = "@" + extern_ir_symbol(e);
                            params = e->parameters;
                            func_type = AST::Type::make_function(
                                std::make_shared<AST::Type>(e->return_type), params);
                        }
                    }
                    }
                    if (callee.empty()) {
                        // 不是函数/外部函数时，把它当作函数指针变量调用
                        // If it is not a function or extern, call it as a function pointer variable
                        direct_name.clear();
                        ExprValue base = gen_expr(expr->base.get());
                        if (base.type.kind == TypeKind::Function) {
                            func_type = base.type;
                            params = func_type.parameter_types;
                        } else if (base.type.kind == TypeKind::Pointer &&
                            base.type.pointee_type &&
                            base.type.pointee_type->kind == TypeKind::Function) {
                            func_type = *base.type.pointee_type;
                            params = func_type.parameter_types;
                        }
                        pointer_call = true;
                        callee = base.value;
                    }
                }
            } else {
                ExprValue base = gen_expr(expr->base.get());
                if (base.type.kind == TypeKind::Function) {
                    func_type = base.type;
                    params = func_type.parameter_types;
                } else if (base.type.kind == TypeKind::Pointer && base.type.pointee_type &&
                    base.type.pointee_type->kind == TypeKind::Function) {
                    func_type = *base.type.pointee_type;
                    params = func_type.parameter_types;
                }
                pointer_call = true;
                callee = base.value;
            }
            if (callee.empty() || func_type.return_type == nullptr) {
                return out;
            }
            return_type = *func_type.return_type;

            // 将实参转为形参类型；extern string 参数按 C 字符串处理
            // Coerce arguments; extern string parameters become C strings
           std::vector<std::string> ir_args;
           std::vector<std::string> owned_args;
            // 实参 = 调用点实参 + 默认参数补齐（Gallt 0.3.txt §8）
            // Arguments = call-site arguments + filled-in defaults (Gallt 0.3.txt §8)
            std::vector<AST::Expression*> all_args;
            all_args.reserve(expr->arguments.size() + expr->appended_defaults.size());
            for (auto& a : expr->arguments) all_args.push_back(a.get());
            for (AST::Expression* d : expr->appended_defaults) all_args.push_back(d);
            for (size_t i = 0; i < all_args.size(); ++i) {
                ExprValue arg = gen_expr(all_args[i]);
                if (!arg.owned_string.empty()) {
                    owned_args.push_back(arg.owned_string);
                }
                AST::Type want = (i < params.size()) ? params[i] : arg.type;
                bool extern_call = !direct_name.empty() && function_is_extern(direct_name);
                if (want.kind == TypeKind::String && extern_call) {
                    // C 字符串：先取得 string 的数据指针
                    // C strings: obtain the data pointer of the Gallt string
                    std::string addr = !arg.address.empty() ? arg.address : arg.value;
                    ir_args.push_back(string_cstr_pointer(addr));
                } else if (want.kind == TypeKind::String && arg.type.kind == TypeKind::String) {
                    // Gallt 函数按值传递 string：先从栈地址加载聚合值
                    // Gallt functions pass string by value; load the aggregate from its address
                    std::string addr = !arg.address.empty() ? arg.address : arg.value;
                    std::string agg = new_temp("stringarg");
                    emit_line(agg + " = load %struct.gallt.string, ptr " + addr);
                    ir_args.push_back(agg);
                } else if (want.kind == TypeKind::Struct && arg.type.kind == TypeKind::Struct) {
                    // 结构体按值传递：从存储地址加载聚合值（Gallt 0.3.txt §14/§20）
                    // Structs pass by value: load the aggregate from its storage address
                    std::string addr = !arg.address.empty() ? arg.address : arg.value;
                    std::string agg = new_temp("structarg");
                    emit_line(agg + " = load " + llvm_type(want) + ", ptr " + addr);
                    ir_args.push_back(agg);
                } else {
                    ir_args.push_back(convert_value(arg.value, arg.type, want));
                }
            }

            std::string ret_ir = llvm_type(return_type);
            if (return_type.kind == TypeKind::Function) ret_ir = "ptr";
            if (pointer_call && !callee.empty()) {
                // RTER 0002：调用空函数指针时由运行库报告运行时错误
                // RTER 0002: the runtime reports an error when a null function pointer
                // is called
                emit_line("call void @gallt_check_fptr(ptr " + callee + ")");
            }
            // Gallt 0.3.txt §20：结构体返回值使用 sret 约定（调用者分配存储，
            // 被调用者在其上执行拷贝/移动构造）；extern 函数保持 C ABI 的按值返回
            // §20: struct returns use sret (caller storage, callee constructs); extern
            // functions keep the C ABI by-value return
            bool extern_call = !direct_name.empty() && function_is_extern(direct_name);
            bool sret_call = returns_via_sret(return_type) && !extern_call;
            std::string sret_storage;
            if (sret_call) {
                if (!pending_sret_destination_.empty()) {
                    // `T v = f()`：直接把 v 的存储作为返回对象，避免多余拷贝（复制消除）
                    // `T v = f()`: use v's own storage for the returned object (copy elision)
                    sret_storage = pending_sret_destination_;
                    pending_sret_destination_.clear();
                }
                else {
                    sret_storage = emit_alloca(llvm_type(return_type), "sret_temp");
                    bool needs_cleanup = type_contains_string(return_type);
                    if (!needs_cleanup) {
                        auto def_it = struct_by_name_.find(return_type.struct_name);
                        needs_cleanup = def_it != struct_by_name_.end() &&
                            def_it->second != nullptr && def_it->second->needs_destruction;
                    }
                    if (needs_cleanup) {
                        CleanupRecord record;
                        record.type = return_type;
                        record.address = sret_storage;
                        statement_temporaries_.push_back(record);
                    }
                }
                ret_ir = "void";
            }
            std::string call_text = "call " + ret_ir + " " + callee + "(";
            if (sret_call) {
                call_text += "ptr " + sret_storage;
            }
            for (size_t i = 0; i < ir_args.size(); ++i) {
                if (i != 0 || sret_call) call_text += ", ";
                AST::Type want = (i < params.size()) ? params[i] : out.type;
                if (want.kind == TypeKind::Function) want = Type::make_pointer(
                    std::make_shared<Type>(Type::make_void()));
                std::string want_type = llvm_type(want);
                if (direct_name.empty()) {
                    // 函数指针调用时参数类型来自函数类型
                    // Function-pointer arguments use the captured function signature
                    want_type = i < params.size() ? llvm_type(params[i]) : "ptr";
                } else if (!direct_name.empty() && function_is_extern(direct_name) &&
                    want.kind == TypeKind::String) {
                    want_type = "ptr";
                } else if (want.kind == TypeKind::String) {
                    want_type = "%struct.gallt.string";
                }
                call_text += want_type + " " + ir_args[i];
            }
            call_text += ")";
            if (sret_call) {
                emit_line(call_text);
                out.type = return_type;
                out.address = sret_storage;
                out.is_lvalue = true;
                out.value = new_temp("sret_value");
                emit_line(out.value + " = load " + llvm_type(return_type) +
                    ", ptr " + sret_storage);
            }
            else if (return_type.kind == TypeKind::Void) {
                emit_line(call_text);
            } else {
                out.value = new_temp("callresult");
                emit_line(out.value + " = " + call_text);
                if (return_type.kind == TypeKind::String) {
                    std::string storage = emit_alloca("%struct.gallt.string", "string_return");
                    emit_line("store %struct.gallt.string " + out.value +
                        ", ptr " + storage);
                    out.value = storage;
                    out.address = storage;
                    out.owned_string = storage;
                }
            }
            for (const std::string& owned : owned_args) {
                emit_line("call void @gallt_string_destroy(ptr " + owned + ")");
            }
            return out;
        }
        }
        return out;
    }

    bool CodeGenerator::function_is_extern(const std::string& name) {
        // 预建索引查表（原来每次调用都要扫描全部顶层节点）
        // Index lookup instead of a full scan of every top-level node per call
        return extern_by_name_.find(name) != extern_by_name_.end();
    }

    std::string CodeGenerator::string_cstr_pointer(const std::string& value) {
        std::string tmp = new_temp("cstr");
        emit_line(tmp + " = call ptr @gallt_string_cstr(ptr " + value + ")");
        return tmp;
    }

    void CodeGenerator::emit_output_call(AST::PostfixExpression* call) {
        for (auto& arg_expr : call->arguments) {
            ExprValue arg = gen_expr(arg_expr.get());
            std::string owned = arg.owned_string;
            switch (arg.type.kind) {
            case TypeKind::Int:
                emit_line("call void @gallt_output_i32(i32 " + arg.value + ")");
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
            default:
                break;
            }
            if (!owned.empty()) {
                emit_line("call void @gallt_string_destroy(ptr " + owned + ")");
            }
        }
    }

    void CodeGenerator::emit_input_call(AST::PostfixExpression* call, ExprValue& result) {
        if (call->arguments.empty()) {
            // 无参数 input 读取一个 int，作为表达式值返回
            // Zero-argument input reads one int and yields it as the call result
            std::string ptr = emit_alloca("i32", "input_slot");
            emit_line("call void @gallt_input_i32(ptr " + ptr + ")");
            std::string val = new_temp("inputval");
            emit_line(val + " = load i32, ptr " + ptr);
            // 关键：把读取结果作为表达式值返回（否则调用点会得到空 SSA 值）
            // The loaded value must be handed back, otherwise callers see an empty value
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
            // 释放前先销毁堆对象内部仍持有的 string
            // Destroy string contents held by the heap object before freeing it
            emit_destroy_string_at(*arg.type.pointee_type, arg.value);
        }
        emit_line("call void @gallt_free_ptr(ptr " + arg.value + ")");
    }

    // ============================================================================
    // 文件操作内置函数（Gallt 0.2.txt §17）
    // File-operation builtins (Gallt 0.2.txt §17)
    // ============================================================================

    CodeGenerator::ExprValue CodeGenerator::emit_file_builtin_call(
        AST::PostfixExpression* call, const std::string& name) {
        ExprValue out;
        out.type = resolved_type(call);

        // 先求值全部实参，再发出调用；拥有堆存储的临时 string 在调用后销毁
        // Evaluate all arguments first, emit the call, then release temporary strings
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
                // 变量优先于类型名；类型名仅在无同名变量时使用
                // Variables take priority over type names
                LocalInfo* local = lookup_local(id);
                if (local != nullptr) {
                    target = local->type;
                } else {
                    if (id == "int") target = Type::make_int();
                    else if (id == "float") target = Type::make_float();
                    else if (id == "double") target = Type::make_double();
                    else if (id == "char") target = Type::make_char();
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
            // 若既不是类型名，则按表达式求值并取其类型
            // If it is not a type name, evaluate the expression and use its type
            ExprValue v = gen_expr(arg);
            target = v.type;
        }
        std::size_t n = is_size ? type_size(target) : type_align(target);
        out.value = std::to_string(n);
        return out;
    }

    CodeGenerator::ExprValue CodeGenerator::gen_binary_string_plus(
        AST::Expression* left, AST::Expression* right) {
        ExprValue l = gen_expr(left);
        ExprValue r = gen_expr(right);
        ExprValue out;
        out.type = Type::make_string();
        // 记录为标量操作数新建的临时 string，调用后释放（否则每次拼接都会泄漏）
        // Track temporaries created for scalar operands and release them afterwards
        bool ls_owned = false;
        bool rs_owned = false;
        std::string ls = string_value_or_converted(l, &ls_owned);
        std::string rs = string_value_or_converted(r, &rs_owned);
        out.value = emit_alloca(llvm_type(Type::make_string()), "concat_result");
        // 目标槽必须显式清零：运行库写入前不依赖其初始内容（防御未初始化内存）
        // Zero the destination first: the runtime must never observe uninitialized bytes
        emit_line("call void @llvm.memset.p0.i64(ptr " + out.value +
            ", i8 0, i64 32, i1 false)");
        emit_line("call void @gallt_string_concat(ptr " + out.value +
            ", ptr " + ls + ", ptr " + rs + ")");
        if (ls_owned) {
            emit_line("call void @gallt_string_destroy(ptr " + ls + ")");
        }
        if (rs_owned) {
            emit_line("call void @gallt_string_destroy(ptr " + rs + ")");
        }
        destroy_owned_string(l);
        destroy_owned_string(r);
        out.owned_string = out.value;
        return out;
    }

    std::string CodeGenerator::string_value_or_converted(const ExprValue& v, bool* owned_temp) {
        if (owned_temp != nullptr) *owned_temp = false;
        if (v.type.kind == TypeKind::String) {
            return !v.address.empty() ? v.address : v.value;
        }
        std::string result = emit_alloca(llvm_type(Type::make_string()), "scalarstring");
        if (owned_temp != nullptr) *owned_temp = true;
        switch (v.type.kind) {
        case TypeKind::Int:
            emit_line("call void @gallt_string_from_i32(ptr " + result +
                ", i32 " + v.value + ")");
            break;
        case TypeKind::Float:
            emit_line("call void @gallt_string_from_f32(ptr " + result +
                ", float " + v.value + ")");
            break;
        case TypeKind::Double:
            emit_line("call void @gallt_string_from_f64(ptr " + result +
                ", double " + v.value + ")");
            break;
        case TypeKind::Char:
            emit_line("call void @gallt_string_from_char(ptr " + result +
                ", i8 " + v.value + ")");
            break;
        case TypeKind::Bool:
            emit_line("call void @gallt_string_from_bool(ptr " + result +
                ", i8 " + v.value + ")");
            break;
        default:
            break;
        }
        return result;
    }

    std::string CodeGenerator::runtime_c_source() {
        // 随编译生成的 C 运行库提供 I/O、string、堆分配等后端支持
        // The bundled C runtime provides I/O, string, and heap support
        return R"GALLT_C(
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <direct.h>

typedef struct gallt_string {
    int64_t length;
    int64_t capacity;
    union {
        char bytes[16];
        char* heap;
    } data;
} gallt_string;

static const char* gallt_string_data_ptr(const gallt_string* s) {
    return s->length <= 15 ? s->data.bytes : s->data.heap;
}

static void gallt_string_free_contents(gallt_string* s) {
    if (s->length > 15 && s->data.heap != NULL) {
        free(s->data.heap);
    }
    memset(s, 0, sizeof(*s));
}

static gallt_string gallt_string_from_bytes_impl(const char* data, int64_t len) {
    gallt_string result;
    memset(&result, 0, sizeof(result));
    result.length = len;
    if (len <= 15) {
        result.capacity = 16;
        if (len > 0) memcpy(result.data.bytes, data, (size_t)len);
        if (len < 16) result.data.bytes[len] = '\0';
        return result;
    }
    char* buffer = (char*)malloc((size_t)len + 1);
    if (buffer == NULL) {
        result.length = 0;
        result.capacity = 0;
        return result;
    }
    memcpy(buffer, data, (size_t)len);
    buffer[len] = '\0';
    result.capacity = len + 1;
    result.data.heap = buffer;
    return result;
}

void gallt_string_init(gallt_string* out, const char* data, int64_t len) {
    if (data == NULL || len < 0) len = 0;
    if (out != NULL) {
        *out = gallt_string_from_bytes_impl(data == NULL ? "" : data, len);
    }
}

void gallt_string_assign(gallt_string* dest, const gallt_string* src) {
    if (dest == NULL || src == NULL) return;
    gallt_string replacement = gallt_string_from_bytes_impl(
        gallt_string_data_ptr(src), src->length);
    gallt_string_free_contents(dest);
    *dest = replacement;
}

void gallt_string_destroy(gallt_string* s) {
    if (s != NULL) gallt_string_free_contents(s);
}

const char* gallt_string_cstr(const gallt_string* s) {
    if (s == NULL) return "";
    return gallt_string_data_ptr(s);
}

void gallt_string_concat(gallt_string* out, const gallt_string* a, const gallt_string* b) {
    if (out == NULL || a == NULL || b == NULL) return;
    int64_t total = a->length + b->length;
    char* temp = (char*)malloc((size_t)(total == 0 ? 1 : total));
    if (temp == NULL) {
        gallt_string_free_contents(out);
        return;
    }
    if (a->length > 0) memcpy(temp, gallt_string_data_ptr(a), (size_t)a->length);
    if (b->length > 0) memcpy(temp + a->length, gallt_string_data_ptr(b), (size_t)b->length);
    gallt_string replacement = gallt_string_from_bytes_impl(temp, total);
    free(temp);
    /* 不释放 out 的旧内容：out 通常是编译器新建的栈临时量，读取其中未初始化的
       length/heap 会把随机地址当作堆指针释放，造成堆损坏。调用方如需覆盖已有
       string，会自行使用 gallt_string_assign（它负责释放旧内容）。 */
    /* Do not free out's previous contents: out is normally a fresh compiler
       temporary, and reading its uninitialized length/heap could free a garbage
       pointer and corrupt the heap. Callers overwriting an existing string use
       gallt_string_assign, which releases the old contents. */
    *out = replacement;
}

static int gallt_format_fp(double v, char* buffer, size_t size) {
    int n = snprintf(buffer, size, "%g", v);
    if (n < 0) return 0;
    if ((size_t)n + 2 >= size) return n;
    // 整数型浮点按文档/测试期望补 ".0"，例如 10 -> 10.0
    // Integer-valued floats keep a trailing ".0", e.g. 10 -> 10.0
    if (strpbrk(buffer, ".eEnNiI") == NULL) {
        buffer[n] = '.';
        buffer[n + 1] = '0';
        buffer[n + 2] = '\0';
        return n + 2;
    }
    return n;
}

void gallt_output_string(const gallt_string* s) {
    if (s == NULL) return;
    fwrite(gallt_string_data_ptr(s), 1, (size_t)s->length, stdout);
    fflush(stdout);
}

void gallt_output_i32(int32_t v) { printf("%d", (int)v); fflush(stdout); }
void gallt_output_f32(float v) {
    char buffer[80];
    int n = gallt_format_fp((double)v, buffer, sizeof(buffer));
    fwrite(buffer, 1, (size_t)n, stdout);
    fflush(stdout);
}
void gallt_output_f64(double v) {
    char buffer[80];
    int n = gallt_format_fp(v, buffer, sizeof(buffer));
    fwrite(buffer, 1, (size_t)n, stdout);
    fflush(stdout);
}
void gallt_output_char(char v) { putchar((unsigned char)v); fflush(stdout); }
void gallt_output_bool(unsigned char v) { fputs(v ? "true" : "false", stdout); fflush(stdout); }
void gallt_output_ptr(const void* v) { printf("%p", v); fflush(stdout); }

void gallt_input_i32(int32_t* p) { if (scanf("%d", p) != 1 && p) *p = 0; }
void gallt_input_f32(float* p) { if (scanf("%f", p) != 1 && p) *p = 0; }
void gallt_input_f64(double* p) { if (scanf("%lf", p) != 1 && p) *p = 0; }
void gallt_input_char(char* p) { if (scanf(" %c", p) != 1 && p) *p = 0; }
void gallt_input_bool(unsigned char* p) {
    int v = 0;
    if (scanf("%d", &v) != 1) v = 0;
    if (p) *p = v ? 1 : 0;
}

void gallt_input_string(gallt_string* p) {
    if (p == NULL) return;
    char buffer[8192];
    if (fgets(buffer, (int)sizeof(buffer), stdin) == NULL) buffer[0] = '\0';
    buffer[sizeof(buffer) - 1] = '\0';
    size_t len = strlen(buffer);
    while (len > 0 && (buffer[len - 1] == '\n' || buffer[len - 1] == '\r')) {
        buffer[--len] = '\0';
    }
    gallt_string replacement = gallt_string_from_bytes_impl(buffer, (int64_t)len);
    gallt_string_free_contents(p);
    *p = replacement;
}

static gallt_string gallt_string_from_int64(int64_t v) {
    char buffer[64];
    int n = snprintf(buffer, sizeof(buffer), "%lld", (long long)v);
    return gallt_string_from_bytes_impl(buffer, n < 0 ? 0 : n);
}

void gallt_string_from_i32(gallt_string* out, int32_t v) {
    if (out != NULL) *out = gallt_string_from_int64(v);
}
void gallt_string_from_char(gallt_string* out, char v) {
    if (out != NULL) *out = gallt_string_from_bytes_impl(&v, 1);
}
void gallt_string_from_bool(gallt_string* out, unsigned char v) {
    if (out == NULL) return;
    *out = v ? gallt_string_from_bytes_impl("true", 4)
             : gallt_string_from_bytes_impl("false", 5);
}
void gallt_string_from_f32(gallt_string* out, float v) {
    if (out == NULL) return;
    char buffer[64];
    int n = gallt_format_fp((double)v, buffer, sizeof(buffer));
    *out = gallt_string_from_bytes_impl(buffer, n < 0 ? 0 : n);
}
void gallt_string_from_f64(gallt_string* out, double v) {
    if (out == NULL) return;
    char buffer[64];
    int n = gallt_format_fp(v, buffer, sizeof(buffer));
    *out = gallt_string_from_bytes_impl(buffer, n < 0 ? 0 : n);
}

void* gallt_alloc_bytes(int64_t size) {
    if (size <= 0) size = 1;
    return calloc(1, (size_t)size);
}

void gallt_free_ptr(void* ptr) {
    free(ptr);
}

/* RTER 0002: calling a null function pointer is a runtime error (Gallt 0.4 error table).
   RTER 0002：函数指针调用时指针为空。运行库消息统一为英文。 */
void gallt_check_fptr(void* fn) {
    if (fn == NULL) {
        fprintf(stderr, "RTER 0002: null function pointer call\n");
        exit(1);
    }
}

/* ---------------------------------------------------------------------------
 * Gallt 0.2.txt §17 文件操作运行库
 * File-operation runtime (Gallt 0.2.txt §17)
 *
 * file 句柄在运行库中是不透明结构，Gallt 侧仅以 file*（ptr）保存。
 * The file handle is an opaque runtime structure; Gallt keeps only file* (ptr).
 * ------------------------------------------------------------------------- */

typedef struct gallt_file {
    FILE* fp;
    /* fileerror(): 0 无错误、1 句柄无效、2 权限不足、3 路径不存在、4 路径已存在、
       5 读取失败、6 写入失败、7 定位失败、8 磁盘已满、9 未知错误 */
    int error;
    int eof;
    /* 运行库登记表：句柄对象与句柄槽在进程退出时统一释放（Gallt 0.3.txt §17） */
    /* Runtime registry: objects and slots are released at process exit (§17) */
    struct gallt_file* next;
    void* slot;
} gallt_file;

/* 句柄槽：Gallt 的 file* 指向一个 8 字节槽，槽内保存不透明句柄对象的地址。
   这样 file（对象值）与 file*（对象地址）语义自洽：
   *fp 复制句柄值、&f 得到可用的 file*、关闭后同一槽的副本读作 null。
   Handle slot: a Gallt file* points at an 8-byte slot holding the opaque handle object,
   so *fp copies the handle value, &f yields a usable file*, and pointer copies of a
   closed slot read as null. */
typedef void* gallt_file_slot;

static gallt_file* gallt_file_registry = NULL;
static int gallt_file_exit_registered = 0;

static void gallt_file_release_all(void) {
    gallt_file* h = gallt_file_registry;
    while (h != NULL) {
        gallt_file* next = h->next;
        if (h->fp != NULL) {
            fclose(h->fp);
            h->fp = NULL;
        }
        free(h->slot);
        free(h);
        h = next;
    }
    gallt_file_registry = NULL;
}

static void gallt_file_register(gallt_file* handle) {
    handle->next = gallt_file_registry;
    gallt_file_registry = handle;
    if (!gallt_file_exit_registered) {
        atexit(gallt_file_release_all);
        gallt_file_exit_registered = 1;
    }
}

/* 从句柄槽取出不透明对象；槽本身或槽内容为 null 时返回 null */
/* Load the opaque object from a handle slot; null slot or null content yields null */
static gallt_file* gallt_file_deref(void* slot) {
    if (slot == NULL) return NULL;
    return *(gallt_file**)slot;
}

static int gallt_errno_code(void) {
    switch (errno) {
    case ENOENT: return 3;
    case EACCES:
    case EPERM:  return 2;
    case EEXIST: return 4;
    case ENOSPC: return 8;
    default:     return 9;
    }
}

static void gallt_file_set_error(gallt_file* handle, int code) {
    if (handle != NULL) handle->error = code;
}

static char* gallt_cstr_from_string(const gallt_string* s) {
    int64_t len = (s == NULL) ? 0 : s->length;
    if (len < 0) len = 0;
    char* buffer = (char*)malloc((size_t)len + 1);
    if (buffer == NULL) return NULL;
    if (len > 0) memcpy(buffer, gallt_string_data_ptr(s), (size_t)len);
    buffer[len] = '\0';
    return buffer;
}

/* 句柄参数一律是句柄槽地址；关闭后（含经副本关闭）统一按“句柄无效或已关闭”处理 */
/* Handle arguments are always slot addresses; a closed handle reports error 1 */
static gallt_file* gallt_file_valid(void* slot) {
    gallt_file* h = gallt_file_deref(slot);
    if (h == NULL || h->fp == NULL) {
        gallt_file_set_error(h, 1);
        return NULL;
    }
    return h;
}

/* 校验打开模式；MSVC 的 fopen 对非法模式会触发无效参数处理（进程终止），
   因此先按文档（Gallt 0.2.txt §17）筛选，非法模式直接返回 null。 */
static int gallt_valid_file_mode(const char* mode) {
    static const char* valid_modes[] = {
        "r", "w", "a", "r+", "w+", "a+",
        "rb", "wb", "ab", "r+b", "w+b", "a+b",
    };
    if (mode == NULL) return 0;
    for (size_t i = 0; i < sizeof(valid_modes) / sizeof(valid_modes[0]); ++i) {
        if (strcmp(mode, valid_modes[i]) == 0) return 1;
    }
    return 0;
}

void* gallt_file_open(const gallt_string* path, const gallt_string* mode) {
    if (path == NULL || mode == NULL) return NULL;
    char* path_cstr = gallt_cstr_from_string(path);
    char* mode_cstr = gallt_cstr_from_string(mode);
    if (path_cstr == NULL || mode_cstr == NULL) {
        free(path_cstr);
        free(mode_cstr);
        return NULL;
    }
    if (!gallt_valid_file_mode(mode_cstr)) {
        /* 无效模式：按文档返回 null，而不是让 CRT 终止进程 */
        free(path_cstr);
        free(mode_cstr);
        return NULL;
    }
    /* 运行库内部统一按二进制方式打开：保证 filewrite/filewriteline 返回的字节数与
       filesize/filetell 观察到的大小一致（文本模式会把 '\n' 展开为 CRLF）。
       filereadline 仍会去掉行尾的 '\r'，因此读取 CRLF 文本同样正确。 */
    char open_mode[8];
    size_t mi = 0;
    while (mode_cstr[mi] != '\0' && mi + 1 < sizeof(open_mode)) {
        open_mode[mi] = mode_cstr[mi];
        ++mi;
    }
    open_mode[mi] = '\0';
    if (strchr(open_mode, 'b') == NULL && mi + 1 < sizeof(open_mode)) {
        open_mode[mi] = 'b';
        open_mode[mi + 1] = '\0';
    }
    FILE* fp = fopen(path_cstr, open_mode);
    free(path_cstr);
    free(mode_cstr);
    if (fp == NULL) return NULL;
    gallt_file* handle = (gallt_file*)calloc(1, sizeof(gallt_file));
    if (handle == NULL) {
        fclose(fp);
        return NULL;
    }
    gallt_file_slot* slot = (gallt_file_slot*)calloc(1, sizeof(gallt_file_slot));
    if (slot == NULL) {
        fclose(fp);
        free(handle);
        return NULL;
    }
    *slot = handle;
    handle->fp = fp;
    handle->error = 0;
    handle->eof = 0;
    handle->slot = slot;
    gallt_file_register(handle);
    return slot;
}

int8_t gallt_file_close(void* slot) {
    gallt_file* h = gallt_file_deref(slot);
    if (h == NULL || h->fp == NULL) return 0;
    FILE* fp = h->fp;
    /* 先标记关闭，再写入槽：所有指向同一句柄的副本随后都视为已关闭（§17） */
    /* Mark closed first, then clear the slot: every copy then reads as invalid (§17) */
    h->fp = NULL;
    int rc = fclose(fp);
    h->error = (rc == 0) ? 0 : gallt_errno_code();
    *(gallt_file**)slot = NULL;
    int ok = (rc == 0) ? 1 : 0;
    return (int8_t)ok;
}

int8_t gallt_file_flush(void* handle) {
    gallt_file* h = gallt_file_valid(handle);
    if (h == NULL) return 0;
    if (fflush(h->fp) != 0) {
        gallt_file_set_error(h, 6);
        return 0;
    }
    h->error = 0;
    return 1;
}

int32_t gallt_file_read(void* handle, void* buffer, int32_t count) {
    gallt_file* h = gallt_file_valid(handle);
    if (h == NULL) return -1;
    if (buffer == NULL || count < 0) {
        gallt_file_set_error(h, 5);
        return -1;
    }
    size_t want = (size_t)count;
    size_t got = fread(buffer, 1, want, h->fp);
    h->eof = feof(h->fp) ? 1 : 0;
    if (got < want && ferror(h->fp)) {
        gallt_file_set_error(h, 5);
        return -1;
    }
    h->error = 0;
    return (int32_t)got;
}

int32_t gallt_file_write(void* handle, const gallt_string* s) {
    gallt_file* h = gallt_file_valid(handle);
    if (h == NULL) return -1;
    if (s == NULL) {
        gallt_file_set_error(h, 6);
        return -1;
    }
    int64_t len = s->length;
    if (len <= 0) {
        h->error = 0;
        return 0;
    }
    size_t written = fwrite(gallt_string_data_ptr(s), 1, (size_t)len, h->fp);
    if (written != (size_t)len) {
        gallt_file_set_error(h, errno == ENOSPC ? 8 : 6);
        return -1;
    }
    h->error = 0;
    return (int32_t)written;
}

int32_t gallt_file_write_bytes(void* handle, const void* buffer, int32_t count) {
    gallt_file* h = gallt_file_valid(handle);
    if (h == NULL) return -1;
    if (buffer == NULL || count < 0) {
        gallt_file_set_error(h, 6);
        return -1;
    }
    if (count == 0) {
        h->error = 0;
        return 0;
    }
    size_t written = fwrite(buffer, 1, (size_t)count, h->fp);
    if (written != (size_t)count) {
        gallt_file_set_error(h, errno == ENOSPC ? 8 : 6);
        return -1;
    }
    h->error = 0;
    return (int32_t)written;
}

int32_t gallt_file_getc(void* handle) {
    gallt_file* h = gallt_file_valid(handle);
    if (h == NULL) return -1;
    int c = fgetc(h->fp);
    if (c == EOF) {
        h->eof = 1;
        h->error = ferror(h->fp) ? 5 : 0;
        return -1;
    }
    h->error = 0;
    return (int32_t)(c & 0xFF);
}

int32_t gallt_file_putc(void* handle, int32_t ch) {
    gallt_file* h = gallt_file_valid(handle);
    if (h == NULL) return -1;
    int c = fputc(ch & 0xFF, h->fp);
    if (c == EOF) {
        gallt_file_set_error(h, errno == ENOSPC ? 8 : 6);
        return -1;
    }
    h->error = 0;
    return (int32_t)(c & 0xFF);
}

void gallt_file_readline(gallt_string* out, void* handle) {
    if (out == NULL) return;
    gallt_file* h = gallt_file_valid(handle);
    if (h == NULL) {
        *out = gallt_string_from_bytes_impl("", 0);
        return;
    }
    char* buffer = NULL;
    size_t capacity = 0;
    size_t length = 0;
    int c = EOF;
    while ((c = fgetc(h->fp)) != EOF) {
        if (c == '\n') break;
        if (length + 1 > capacity) {
            size_t next = (capacity == 0) ? 128 : capacity * 2;
            char* grown = (char*)realloc(buffer, next);
            if (grown == NULL) break;
            buffer = grown;
            capacity = next;
        }
        buffer[length++] = (char)c;
    }
    if (c == EOF) h->eof = 1;
    /* 行尾的 '\r' 不计入返回的字符串（CRLF 文本文件） */
    if (length > 0 && buffer != NULL && buffer[length - 1] == '\r') length--;
    if (buffer == NULL) {
        *out = gallt_string_from_bytes_impl("", 0);
    } else {
        *out = gallt_string_from_bytes_impl(buffer, (int64_t)length);
        free(buffer);
    }
    h->error = 0;
}

int32_t gallt_file_writeline(void* handle, const gallt_string* s) {
    gallt_file* h = gallt_file_valid(handle);
    if (h == NULL) return -1;
    int64_t len = (s == NULL) ? 0 : s->length;
    size_t written = 0;
    if (len > 0) {
        written = fwrite(gallt_string_data_ptr(s), 1, (size_t)len, h->fp);
        if (written != (size_t)len) {
            gallt_file_set_error(h, errno == ENOSPC ? 8 : 6);
            return -1;
        }
    }
    if (fputc('\n', h->fp) == EOF) {
        gallt_file_set_error(h, errno == ENOSPC ? 8 : 6);
        return -1;
    }
    h->error = 0;
    return (int32_t)(written + 1);
}

int8_t gallt_file_seek(void* handle, int32_t offset, int32_t origin) {
    gallt_file* h = gallt_file_valid(handle);
    if (h == NULL) return 0;
    int whence = SEEK_SET;
    switch (origin) {
    case 0: whence = SEEK_SET; break;
    case 1: whence = SEEK_CUR; break;
    case 2: whence = SEEK_END; break;
    default:
        gallt_file_set_error(h, 7);
        return 0;
    }
    if (fseek(h->fp, (long)offset, whence) != 0) {
        gallt_file_set_error(h, 7);
        return 0;
    }
    h->eof = 0;
    h->error = 0;
    return 1;
}

int32_t gallt_file_tell(void* handle) {
    gallt_file* h = gallt_file_valid(handle);
    if (h == NULL) return -1;
    long pos = ftell(h->fp);
    if (pos < 0) {
        gallt_file_set_error(h, 7);
        return -1;
    }
    h->error = 0;
    return (int32_t)pos;
}

int8_t gallt_file_eof(void* handle) {
    gallt_file* h = gallt_file_valid(handle);
    if (h == NULL) return 0;
    if (h->eof || feof(h->fp)) return 1;
    return 0;
}

int32_t gallt_file_error(void* slot) {
    gallt_file* h = gallt_file_deref(slot);
    /* 槽为空、槽内容为空、或句柄已关闭 → 1“句柄无效或已关闭”（§17 错误码表） */
    /* Null slot, null content, or a closed handle yields 1 "invalid or closed" (§17) */
    if (h == NULL || h->fp == NULL) return 1;
    return h->error;
}

int8_t gallt_file_remove(const gallt_string* path) {
    char* p = gallt_cstr_from_string(path);
    if (p == NULL) return 0;
    int rc = remove(p);
    free(p);
    return (int8_t)(rc == 0 ? 1 : 0);
}

int8_t gallt_file_rename(const gallt_string* from, const gallt_string* to) {
    char* a = gallt_cstr_from_string(from);
    char* b = gallt_cstr_from_string(to);
    if (a == NULL || b == NULL) {
        free(a);
        free(b);
        return 0;
    }
    int rc = rename(a, b);
    free(a);
    free(b);
    return (int8_t)(rc == 0 ? 1 : 0);
}

int8_t gallt_file_exists(const gallt_string* path) {
    char* p = gallt_cstr_from_string(path);
    if (p == NULL) return 0;
    FILE* fp = fopen(p, "rb");
    if (fp != NULL) {
        fclose(fp);
        free(p);
        return 1;
    }
    free(p);
    return 0;
}

int32_t gallt_file_size(const gallt_string* path) {
    char* p = gallt_cstr_from_string(path);
    if (p == NULL) return -1;
    FILE* fp = fopen(p, "rb");
    if (fp == NULL) {
        free(p);
        return -1;
    }
    free(p);
    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return -1;
    }
    long size = ftell(fp);
    fclose(fp);
    if (size < 0) return -1;
    return (int32_t)size;
}

int8_t gallt_file_copy(const gallt_string* from, const gallt_string* to) {
    char* a = gallt_cstr_from_string(from);
    char* b = gallt_cstr_from_string(to);
    if (a == NULL || b == NULL) {
        free(a);
        free(b);
        return 0;
    }
    FILE* in = fopen(a, "rb");
    if (in == NULL) {
        free(a);
        free(b);
        return 0;
    }
    FILE* out = fopen(b, "wb");
    if (out == NULL) {
        fclose(in);
        free(a);
        free(b);
        return 0;
    }
    char chunk[8192];
    size_t n = 0;
    int ok = 1;
    while ((n = fread(chunk, 1, sizeof(chunk), in)) > 0) {
        if (fwrite(chunk, 1, n, out) != n) {
            ok = 0;
            break;
        }
    }
    if (ferror(in)) ok = 0;
    fclose(in);
    if (fclose(out) != 0) ok = 0;
    free(a);
    free(b);
    return (int8_t)(ok ? 1 : 0);
}

int8_t gallt_file_mkdir(const gallt_string* path) {
    char* p = gallt_cstr_from_string(path);
    if (p == NULL) return 0;
    int rc = _mkdir(p);
    free(p);
    return (int8_t)(rc == 0 ? 1 : 0);
}

int8_t gallt_file_removedir(const gallt_string* path) {
    char* p = gallt_cstr_from_string(path);
    if (p == NULL) return 0;
    int rc = _rmdir(p);
    free(p);
    return (int8_t)(rc == 0 ? 1 : 0);
}
)GALLT_C";
    }

} // namespace gallt
