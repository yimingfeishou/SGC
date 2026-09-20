#include "codegen.hpp"
#include "../runtime/crt_embedded.hpp"
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
namespace {

    std::string decode_escaped_bytes(std::string_view raw, bool is_char);

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

    bool builtin_type_by_name(const std::string& name, AST::Type& out) {
        if (name == "int") out = AST::Type::make_int();
        else if (name == "lint") out = AST::Type::make_lint();
        else if (name == "uint") out = AST::Type::make_uint();
        else if (name == "luint") out = AST::Type::make_luint();
        else if (name == "float") out = AST::Type::make_float();
        else if (name == "double") out = AST::Type::make_double();
        else if (name == "char") out = AST::Type::make_char();
        else if (name == "uchar") out = AST::Type::make_uchar();
        else if (name == "bool") out = AST::Type::make_bool();
        else if (name == "string") out = AST::Type::make_string();
        else if (name == "file") out = AST::Type::make_file();
        else if (name == "void") out = AST::Type::make_void();
        else return false;
        return true;
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
        if (text.empty()) {
            return text;
        }
        if (text == "inf" || text == "+inf") {
            return "0x7FF0000000000000";
        }
        if (text == "-inf") {
            return "0xFFF0000000000000";
        }
        if (text == "nan" || text == "+nan" || text == "-nan") {
            return "0x7FF8000000000000";
        }
        if (text.find('.') == std::string::npos) {
            const std::size_t exponent = text.find_first_of("eE");
            if (exponent == std::string::npos) {
                text += ".0";
            }
            else {
                text.insert(exponent, ".0");
            }
        }
        return text;
    }

    bool line_accepts_debug_metadata(const std::string& line) {
        if (line.empty()) return false;
        if (line.front() == '@' || line.front() == '!') return false;
        if (line.back() == ':') return false;
        if (line == "}") return false;
        if (line.rfind("define ", 0) == 0) return false;
        if (line.rfind("declare ", 0) == 0) return false;
        if (line.rfind("source_filename", 0) == 0) return false;
        if (line.rfind("target ", 0) == 0) return false;
        if (line.rfind("attributes ", 0) == 0) return false;
        if (line.rfind("phi ", 0) == 0) return false;
        if (line.find(" = phi ") != std::string::npos) return false;
        if (line.find(" = ") != std::string::npos) {
            if (line.find(" = global ") != std::string::npos) return false;
            if (line.find(" = private ") != std::string::npos) return false;
            if (line.find(" = constant ") != std::string::npos) return false;
            if (line.find(" = internal ") != std::string::npos) return false;
            if (line.find(" = type ") != std::string::npos) return false;
            return true;
        }
        static const char* kInstructionPrefixes[] = {
            "ret ", "br ", "switch ", "unreachable", "store ", "call ",
            "invoke ", "resume ", "fence ",
        };
        for (const char* prefix : kInstructionPrefixes) {
            if (line.rfind(prefix, 0) == 0) return true;
        }
        return false;
    }

    std::string escape_metadata_string(const std::string& text) {
        std::string out;
        out.reserve(text.size());
        for (char c : text) {
            if (c == '\\' || c == '"') {
                out.push_back('\\');
            }
            out.push_back(c);
        }
        return out;
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

} 

    CodeGenerator::CodeGenerator(
        AST::Program* program,
        const std::unordered_map<const AST::Expression*, AST::Type>& expression_types,
        const std::unordered_map<const AST::PrimaryExpression*,
            const AST::FunctionDefinition*>& resolved_functions,
        const std::unordered_map<const AST::PrimaryExpression*,
            const AST::ExternDeclaration*>& resolved_externs,
        const std::unordered_map<const AST::Expression*,
            AST::FunctionDefinition*>& resolved_operators,
        DiagnosticEngine* diagnostics, int debug_symbols_level)
        : program_(program), expression_types_(expression_types),
        resolved_functions_(resolved_functions), resolved_externs_(resolved_externs),
        resolved_operators_(resolved_operators), diagnostics_(diagnostics),
        debug_level_(debug_symbols_level) {
    }

    std::string CodeGenerator::new_temp(const char* hint) {
        return "%" + std::string(hint) + std::to_string(temp_counter_++);
    }

    std::string CodeGenerator::new_label(const char* hint) {
        return "blk_" + std::string(hint) + "_" + std::to_string(label_counter_++);
    }

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
        if (!current_label_.empty() && emitted_labels_.count(current_label_) == 0) {
            lines_.push_back(current_label_ + ":");
            emitted_labels_.insert(current_label_);
        }
        std::string emitted = line;
        if (debug_level_ >= 1 && debug_location_valid_ &&
            debug_subprogram_id_ != 0 &&
            line_accepts_debug_metadata(line)) {
            const unsigned location_id = debug_location_id(debug_location_);
            if (location_id != 0) {
                emitted += ", !dbg !" + std::to_string(location_id);
            }
        }
        lines_.push_back(emitted);
        if (line.starts_with("ret ") || line.starts_with("br ") ||
            line.starts_with("unreachable") || line.starts_with("switch ") ||
            line.starts_with("invoke ")) {
            current_block_terminated_ = true;
        } else {
            current_block_terminated_ = false;
        }
    }

    void CodeGenerator::start_block(const std::string& label) {
        if (!current_label_.empty() && emitted_labels_.count(current_label_) == 0) {
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
        case TypeKind::Lint: return "i64";
        case TypeKind::Uint: return "i32";
        case TypeKind::Luint: return "i64";
        case TypeKind::Float: return "float";
        case TypeKind::Double: return "double";
        case TypeKind::Char: return "i8";
        case TypeKind::Uchar: return "i8";
        case TypeKind::Bool: return "i8";
        case TypeKind::String: return "%struct.gallt.string";
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
        register_lifecycle_symbols();
        collect_debug_source_file();
        lines_.clear();
        temp_counter_ = 0;
        label_counter_ = 0;
        emitted_labels_.clear();
        string_literals_.clear();
        string_literal_ids_.clear();
        constant_aggregate_globals_.clear();
        constant_aggregate_counter_ = 0;
        debug_metadata_.clear();
        debug_type_ids_.clear();
        debug_location_ids_.clear();
        debug_subroutine_ids_.clear();
        debug_next_id_ = 5;
        debug_subprogram_id_ = 0;
        debug_shared_subroutine_id_ = 0;
        debug_expression_id_ = 0;
        debug_location_valid_ = false;
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
        emit_lifecycle_functions();
        emit_global_initializer();
        emit_main_wrapper();
        emit_string_constants();
        emit_constant_aggregate_globals();
        emit_debug_metadata();

        std::ostringstream out;
        for (const std::string& line : lines_) {
            out << line << '\n';
        }
        ir_ = out.str();
        return true;
    }

    void CodeGenerator::emit_preamble() {
        emit_line("source_filename = \"gallt\"");
        emit_line("target triple = \"x86_64-pc-windows-msvc\"");
        emit_line("%struct.gallt.string = type { i64, i64, [16 x i8] }");
    }

    void CodeGenerator::collect_debug_source_file() {
        debug_source_file_.clear();
        debug_source_dir_.clear();
        for (const auto& top : program_->top_levels) {
            if (top == nullptr) continue;
            if (!top->location.filename.empty()) {
                debug_source_file_ = std::string(top->location.filename);
                break;
            }
        }
        if (debug_source_file_.empty()) return;
        const std::size_t slash = debug_source_file_.find_last_of("/\\");
        if (slash != std::string::npos) {
            debug_source_dir_ = debug_source_file_.substr(0, slash);
            debug_source_file_ = debug_source_file_.substr(slash + 1);
        }
    }

    unsigned CodeGenerator::next_debug_id() {
        return debug_next_id_++;
    }

    unsigned CodeGenerator::debug_type_id(const AST::Type& type) {
        if (type.kind == TypeKind::Void) return 0;
        const std::string key = type.to_string();
        auto cached = debug_type_ids_.find(key);
        if (cached != debug_type_ids_.end()) return cached->second;
        const unsigned id = next_debug_id();
        debug_type_ids_[key] = id;
        std::string text;
        switch (type.kind) {
        case TypeKind::Int:
            text = "!DIBasicType(name: \"int\", size: 32, encoding: DW_ATE_signed)";
            break;
        case TypeKind::Lint:
            text = "!DIBasicType(name: \"lint\", size: 64, encoding: DW_ATE_signed)";
            break;
        case TypeKind::Uint:
            text = "!DIBasicType(name: \"uint\", size: 32, encoding: DW_ATE_unsigned)";
            break;
        case TypeKind::Luint:
            text = "!DIBasicType(name: \"luint\", size: 64, encoding: DW_ATE_unsigned)";
            break;
        case TypeKind::Float:
            text = "!DIBasicType(name: \"float\", size: 32, encoding: DW_ATE_float)";
            break;
        case TypeKind::Double:
            text = "!DIBasicType(name: \"double\", size: 64, encoding: DW_ATE_float)";
            break;
        case TypeKind::Char:
            text = "!DIBasicType(name: \"char\", size: 8, encoding: DW_ATE_signed_char)";
            break;
        case TypeKind::Uchar:
            text = "!DIBasicType(name: \"uchar\", size: 8, encoding: DW_ATE_unsigned_char)";
            break;
        case TypeKind::Bool:
            text = "!DIBasicType(name: \"bool\", size: 8, encoding: DW_ATE_boolean)";
            break;
        case TypeKind::String:
            text = "!DICompositeType(tag: DW_TAG_structure_type, name: \"string\", "
                "size: 256, identifier: \"gallt.string\")";
            break;
        case TypeKind::File:
            text = "!DIBasicType(name: \"file\", size: 64, encoding: DW_ATE_unsigned)";
            break;
        case TypeKind::Pointer: {
            const unsigned base = type.pointee_type
                ? debug_type_id(*type.pointee_type) : 0;
            text = "!DIDerivedType(tag: DW_TAG_pointer_type, baseType: " +
                (base != 0 ? "!" + std::to_string(base) : std::string("null")) +
                ", size: 64)";
            break;
        }
        case TypeKind::Function:
            text = "!DIDerivedType(tag: DW_TAG_pointer_type, baseType: null, size: 64)";
            break;
        case TypeKind::Array: {
            const unsigned element = type.element_type
                ? debug_type_id(*type.element_type) : 0;
            const unsigned subrange = next_debug_id();
            const unsigned elements = next_debug_id();
            const long long count = type.array_size.has_value()
                ? static_cast<long long>(*type.array_size) : -1LL;
            debug_metadata_.push_back("!" + std::to_string(subrange) +
                " = !DISubrange(count: " + std::to_string(count) + ", lowerBound: 0)");
            debug_metadata_.push_back("!" + std::to_string(elements) + " = !{!" +
                std::to_string(subrange) + "}");
            text = "!DICompositeType(tag: DW_TAG_array_type, baseType: " +
                (element != 0 ? "!" + std::to_string(element) : std::string("null")) +
                ", size: " + std::to_string(type_size(type) * 8) +
                ", elements: !" + std::to_string(elements) + ")";
            break;
        }
        case TypeKind::Struct: {
            auto found = struct_by_name_.find(type.struct_name);
            if (found == struct_by_name_.end() || found->second == nullptr) {
                text = "!DICompositeType(tag: DW_TAG_structure_type, name: \"" +
                    escape_metadata_string(type.struct_name) + "\", size: " +
                    std::to_string(type_size(type) * 8) + ")";
                break;
            }
            AST::StructDefinition* def = found->second;
            std::vector<unsigned> member_ids;
            std::size_t offset = 0;
            for (const AST::StructDefinition::Member& member : def->members) {
                const unsigned member_id = next_debug_id();
                const unsigned member_type = debug_type_id(member.type);
                debug_metadata_.push_back("!" + std::to_string(member_id) +
                    " = !DIDerivedType(tag: DW_TAG_member, name: \"" +
                    escape_metadata_string(member.name) + "\", scope: !" +
                    std::to_string(id) + ", file: !1, line: " +
                    std::to_string(member.location.line > 0 ? member.location.line : 1) +
                    ", baseType: " +
                    (member_type != 0 ? "!" + std::to_string(member_type)
                                      : std::string("null")) +
                    ", size: " + std::to_string(type_size(member.type) * 8) +
                    ", offset: " + std::to_string(offset * 8) + ")");
                const std::size_t alignment = type_align(member.type);
                offset += type_size(member.type);
                if (alignment > 1) {
                    offset = (offset + alignment - 1) / alignment * alignment;
                }
                member_ids.push_back(member_id);
            }
            const unsigned elements = next_debug_id();
            std::string list = "!" + std::to_string(elements) + " = !{";
            for (std::size_t i = 0; i < member_ids.size(); ++i) {
                if (i != 0) list += ", ";
                list += "!" + std::to_string(member_ids[i]);
            }
            list += "}";
            debug_metadata_.push_back(list);
            text = "!DICompositeType(tag: DW_TAG_structure_type, name: \"" +
                escape_metadata_string(def->name) + "\", file: !1, line: " +
                std::to_string(def->location.line > 0 ? def->location.line : 1) +
                ", size: " + std::to_string(type_size(type) * 8) +
                ", elements: !" + std::to_string(elements) +
                ", identifier: \"gallt.struct." +
                escape_metadata_string(def->name) + "\")";
            break;
        }
        default:
            return 0;
        }
        debug_metadata_.push_back("!" + std::to_string(id) + " = " + text);
        return id;
    }

    unsigned CodeGenerator::debug_subroutine_id(const AST::Type& return_type,
        const std::vector<AST::Type>& parameters) {
        if (debug_level_ < 2) {
            if (debug_shared_subroutine_id_ == 0) {
                const unsigned types_id = next_debug_id();
                debug_shared_subroutine_id_ = next_debug_id();
                debug_metadata_.push_back("!" + std::to_string(types_id) + " = !{null}");
                debug_metadata_.push_back("!" + std::to_string(debug_shared_subroutine_id_) +
                    " = !DISubroutineType(types: !" + std::to_string(types_id) + ")");
            }
            return debug_shared_subroutine_id_;
        }
        std::string key = return_type.to_string();
        for (const AST::Type& parameter : parameters) {
            key += ",";
            key += parameter.to_string();
        }
        auto cached = debug_subroutine_ids_.find(key);
        if (cached != debug_subroutine_ids_.end()) return cached->second;
        const unsigned id = next_debug_id();
        const unsigned types_id = next_debug_id();
        debug_subroutine_ids_[key] = id;
        const unsigned return_id = debug_type_id(return_type);
        std::string list = "!" + std::to_string(types_id) + " = !{";
        list += (return_id != 0 ? "!" + std::to_string(return_id) : std::string("null"));
        for (const AST::Type& parameter : parameters) {
            const unsigned parameter_id = debug_type_id(parameter);
            list += ", ";
            list += (parameter_id != 0 ? "!" + std::to_string(parameter_id)
                                       : std::string("null"));
        }
        list += "}";
        debug_metadata_.push_back(list);
        debug_metadata_.push_back("!" + std::to_string(id) +
            " = !DISubroutineType(types: !" + std::to_string(types_id) + ")");
        return id;
    }

    unsigned CodeGenerator::debug_location_id(const SourceLocation& location) {
        if (debug_level_ < 1 || debug_subprogram_id_ == 0) return 0;
        const unsigned line = location.line > 0
            ? static_cast<unsigned>(location.line) : 1u;
        const unsigned column = location.column > 0
            ? static_cast<unsigned>(location.column) : 1u;
        const std::string key = std::to_string(debug_subprogram_id_) + ":" +
            std::to_string(line) + ":" + std::to_string(column);
        auto cached = debug_location_ids_.find(key);
        if (cached != debug_location_ids_.end()) return cached->second;
        const unsigned id = next_debug_id();
        debug_location_ids_[key] = id;
        debug_metadata_.push_back("!" + std::to_string(id) +
            " = !DILocation(line: " + std::to_string(line) + ", column: " +
            std::to_string(column) + ", scope: !" +
            std::to_string(debug_subprogram_id_) + ")");
        return id;
    }

    void CodeGenerator::begin_debug_function(AST::FunctionDefinition* func,
        std::string& suffix) {
        suffix.clear();
        debug_subprogram_id_ = 0;
        debug_location_valid_ = false;
        if (debug_level_ < 1 || func == nullptr) return;
        const unsigned line = func->location.line > 0
            ? static_cast<unsigned>(func->location.line) : 1u;
        debug_subprogram_id_ = next_debug_id();
        const unsigned signature = debug_subroutine_id(func->return_type,
            func->parameters);
        const std::string ir_name = function_llvm_name_for_source(func->name);
        std::string symbol = ir_name;
        if (!symbol.empty() && symbol.front() == '@') symbol.erase(0, 1);
        debug_metadata_.push_back("!" + std::to_string(debug_subprogram_id_) +
            " = distinct !DISubprogram(name: \"" +
            escape_metadata_string(func->name) + "\", linkageName: \"" +
            escape_metadata_string(symbol) + "\", scope: !1, file: !1, line: " +
            std::to_string(line) + ", type: !" + std::to_string(signature) +
            ", scopeLine: " + std::to_string(line) +
            ", spFlags: DISPFlagDefinition, unit: !0)");
        suffix = " !dbg !" + std::to_string(debug_subprogram_id_);
        debug_location_ = func->location;
        debug_location_valid_ = true;
    }

    void CodeGenerator::emit_debug_local_variable(const std::string& name,
        const AST::Type& type, const std::string& address,
        const SourceLocation& location) {
        if (debug_level_ < 2 || debug_subprogram_id_ == 0) return;
        if (address.empty() || name.empty()) return;
        if (debug_expression_id_ == 0) {
            debug_expression_id_ = next_debug_id();
            debug_metadata_.push_back("!" + std::to_string(debug_expression_id_) +
                " = !DIExpression()");
        }
        const unsigned line = location.line > 0
            ? static_cast<unsigned>(location.line) : 1u;
        const unsigned variable_id = next_debug_id();
        const unsigned type_id = debug_type_id(type);
        debug_metadata_.push_back("!" + std::to_string(variable_id) +
            " = !DILocalVariable(name: \"" + escape_metadata_string(name) +
            "\", scope: !" + std::to_string(debug_subprogram_id_) + ", file: !1, line: " +
            std::to_string(line) + ", type: " +
            (type_id != 0 ? "!" + std::to_string(type_id) : std::string("null")) + ")");
        emit_line("call void @llvm.dbg.declare(metadata ptr " + address +
            ", metadata !" + std::to_string(variable_id) + ", metadata !" +
            std::to_string(debug_expression_id_) + ")");
    }

    void CodeGenerator::emit_debug_metadata() {
        if (debug_level_ < 1) return;
        current_label_.clear();
        const char* emission = debug_level_ >= 2 ? "FullDebug" : "LineTablesOnly";
        lines_.push_back("!llvm.dbg.cu = !{!0}");
        lines_.push_back("!llvm.module.flags = !{!2, !3, !4}");
        lines_.push_back("!2 = !{i32 2, !\"CodeView\", i32 1}");
        lines_.push_back("!3 = !{i32 2, !\"Debug Info Version\", i32 3}");
        lines_.push_back("!4 = !{i32 1, !\"wchar_size\", i32 4}");
        lines_.push_back("!0 = distinct !DICompileUnit(language: DW_LANG_C99, file: !1, "
            "producer: \"sgc Standard Gallt Compiler\", isOptimized: false, "
            "runtimeVersion: 0, emissionKind: " + std::string(emission) + ")");
        lines_.push_back("!1 = !DIFile(filename: \"" +
            escape_metadata_string(debug_source_file_) + "\", directory: \"" +
            escape_metadata_string(debug_source_dir_) + "\")");
        for (const std::string& entry : debug_metadata_) {
            lines_.push_back(entry);
        }
    }

    void CodeGenerator::emit_struct_types() {
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
        emit_line("declare void @gallt_output_i32(i32)");
        emit_line("declare void @gallt_output_u32(i32)");
        emit_line("declare void @gallt_output_i64(i64)");
        emit_line("declare void @gallt_output_u64(i64)");
        emit_line("declare void @gallt_output_f32(float)");
        emit_line("declare void @gallt_output_f64(double)");
        emit_line("declare void @gallt_output_char(i8)");
        emit_line("declare void @gallt_output_bool(i8)");
        emit_line("declare void @gallt_output_ptr(ptr)");
        emit_line("declare void @gallt_output_string(ptr)");
        emit_line("declare void @gallt_input_i32(ptr)");
        emit_line("declare void @gallt_input_u32(ptr)");
        emit_line("declare void @gallt_input_i64(ptr)");
        emit_line("declare void @gallt_input_u64(ptr)");
        emit_line("declare void @gallt_input_uchar(ptr)");
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
        emit_line("declare void @gallt_string_from_u32(ptr, i32)");
        emit_line("declare void @gallt_string_from_i64(ptr, i64)");
        emit_line("declare void @gallt_string_from_u64(ptr, i64)");
        emit_line("declare void @gallt_string_from_f32(ptr, float)");
        emit_line("declare void @gallt_string_from_f64(ptr, double)");
        emit_line("declare void @gallt_string_from_char(ptr, i8)");
        emit_line("declare void @gallt_string_from_bool(ptr, i8)");
        emit_line("declare ptr @gallt_string_cstr(ptr)");
        emit_line("declare ptr @gallt_alloc_bytes(i64)");
        emit_line("declare void @gallt_free_ptr(ptr)");
        emit_line("declare void @gallt_check_fptr(ptr)");
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
          if (debug_level_ >= 2) {
              emit_line("declare void @llvm.dbg.declare(metadata, metadata, metadata)");
          }
          emit_line("declare double @pow(double, double)");
    }

    void CodeGenerator::emit_string_constants() {
        for (const StringLiteralConstant& c : string_literals_) {
            std::size_t array_len = c.bytes.empty() ? 1u : c.bytes.size();
            std::string bytes = c.bytes.empty() ? std::string(1, '\0') : c.bytes;
            emit_line("@" + c.llvm_name + " = private constant [" +
                std::to_string(array_len) + " x i8] " +
                llvm_escape_bytes(bytes));
        }
    }

    std::string CodeGenerator::function_llvm_name_for_source(std::string_view name) const {
        if (name == "main") return "@glt_main";
        return "@glt_" + std::string(name);
    }

    void CodeGenerator::emit_function_declarations() {
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
        const_globals_.clear();
        runtime_const_globals_.clear();
        for (const auto& top : program_->top_levels) {
            if (auto* var = dynamic_cast<AST::VariableDeclaration*>(top.get())) {
                if (var->type.is_const) {
                    const_globals_.push_back(var);
                }
                else {
                    global_vars_.push_back(var);
                }
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
        for (AST::VariableDeclaration* var : const_globals_) {
            LocalInfo info;
            if (fold_constant_declaration(var, info)) {
                global_symbols_[var->name] = std::move(info);
                continue;
            }
            runtime_const_globals_.push_back(var);
            std::string address = "@glt_g_" + var->name;
            emit_line(address + " = global " + llvm_type(var->type) +
                " zeroinitializer");
            LocalInfo fallback;
            fallback.type = var->type;
            fallback.address = address;
            global_symbols_[var->name] = std::move(fallback);
        }
        for (AST::VariableDeclaration* var : global_vars_) {
            std::string address = "@glt_g_" + var->name;
            std::string type_text = llvm_type(var->type);
            std::string definition = address + " = global " + type_text +
                " zeroinitializer";
            if (debug_level_ >= 2) {
                const unsigned variable_id = next_debug_id();
                const unsigned type_id = debug_type_id(var->type);
                const unsigned line = var->location.line > 0
                    ? static_cast<unsigned>(var->location.line) : 1u;
                debug_metadata_.push_back("!" + std::to_string(variable_id) +
                    " = distinct !DIGlobalVariable(name: \"" +
                    escape_metadata_string(var->name) + "\", scope: !1, file: !1, line: " +
                    std::to_string(line) + ", type: " +
                    (type_id != 0 ? "!" + std::to_string(type_id)
                                  : std::string("null")) +
                    ", isLocal: false, isDefinition: true)");
                definition += ", !dbg !" + std::to_string(variable_id);
            }
            emit_line(definition);
            LocalInfo info;
            info.type = var->type;
            info.address = address;
            global_symbols_[var->name] = std::move(info);
        }
    }

    void CodeGenerator::emit_global_initializer() {
        emit_global_deinit_function();
        if (global_vars_.empty() && runtime_const_globals_.empty()) return;

        debug_subprogram_id_ = 0;
        debug_location_valid_ = false;
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
                AST::ArrayInitializer empty_init(var->location,
                    std::vector<std::unique_ptr<AST::Initializer>>{});
                emit_struct_brace_initialization("@glt_g_" + var->name, var->type,
                    &empty_init);
            }
        }
        for (AST::VariableDeclaration* var : runtime_const_globals_) {
            if (var->initializer) {
                emit_initializer_to_address(var, "@glt_g_" + var->name);
            }
            else if (var->type.kind == TypeKind::Struct) {
                AST::ArrayInitializer empty_init(var->location,
                    std::vector<std::unique_ptr<AST::Initializer>>{});
                emit_struct_brace_initialization("@glt_g_" + var->name, var->type,
                    &empty_init);
            }
        }
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
        debug_subprogram_id_ = 0;
        debug_location_valid_ = false;
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
        for (auto it = runtime_const_globals_.rbegin();
            it != runtime_const_globals_.rend(); ++it) {
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
        debug_subprogram_id_ = 0;
        debug_location_valid_ = false;
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
            emit_array_brace_initialization(address, decl->type, arr_init);
        } else if (decl->type.kind == TypeKind::Struct) {
            emit_struct_brace_initialization(address, decl->type, arr_init);
        }
    }

    CodeGenerator::LocalInfo* CodeGenerator::lookup_local(const std::string& name) {
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
        }
    }

    void CodeGenerator::discard_current_cleanup_scope() {
        if (!cleanup_scopes_.empty()) {
            cleanup_scopes_.back().clear();
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
        std::string debug_suffix;
        begin_debug_function(func, debug_suffix);
        if (!debug_suffix.empty()) {
            header.insert(header.size() - 2, debug_suffix);
        }
        emit_line(header);
        current_sret_pointer_ = sret ? std::string("%__sret_ret") : std::string();

        std::string entry_label = new_label("entry");
        start_block(entry_label);
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
        flush_hoisted_allocas();
        emit_line("}");
        discard_current_cleanup_scope();
        pop_scope();
    }

    void CodeGenerator::emit_block(AST::Block* block, bool new_scope) {
        if (block == nullptr) return;
        if (new_scope) {
            push_scope();
        }
        for (const auto& stmt : block->statements) {
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
        if (debug_level_ >= 1 && debug_location_valid_) {
            debug_location_ = stmt->location;
        }
        if (auto* block = dynamic_cast<AST::Block*>(stmt)) {
            emit_block(block, true);
        } else if (auto* decl = dynamic_cast<AST::VariableDeclaration*>(stmt)) {
            emit_variable_declaration(decl);
        } else if (auto* destruct_stmt = dynamic_cast<AST::DestructStatement*>(stmt)) {
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
                        string_ret_storage = value.owned_string;
                        value.owned_string.clear();
                    } else if (!value.address.empty()) {
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
                destroy_statement_temporaries();
                destroy_active_cleanup_scopes(0);
                emit_line("ret " + type_text + " " + converted);
            } else {
                destroy_statement_temporaries();
                destroy_active_cleanup_scopes(0);
                emit_line("ret void");
            }
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
            destroy_owned_string(value);
        }
    }

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
            if (!builtin_type_by_name(name, type)) return false;
            size = type_size(type);
            align = type_align(type);
            return true;
        };
        ctx.lookup_constant = [this](const std::string& name, long long& int_value,
            double& float_value, bool& is_float) {
            auto found = constant_values_.find(name);
            if (found == constant_values_.end()) return false;
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
        }
        else {
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
        if (expr == nullptr) return false;
        if (type.kind == TypeKind::String) {
            auto* prim = dynamic_cast<const AST::PrimaryExpression*>(expr);
            if (prim == nullptr ||
                prim->kind != AST::PrimaryExpression::Kind::Literal ||
                prim->literal_token.type != TokenType::StringLiteral) {
                return false;
            }
            std::string bytes = decode_escaped_bytes(prim->literal_token.lexeme, false);
            if (bytes.size() > 15) return false;
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
            if (found == constant_values_.end()) return false;
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
        if (init == nullptr) return false;
        if (auto* expr_init = dynamic_cast<const AST::ExpressionInitializer*>(init)) {
            return build_constant_scalar(expr_init->expr.get(), type, out);
        }
        auto* arr_init = dynamic_cast<const AST::ArrayInitializer*>(init);
        if (arr_init == nullptr) return false;
        if (type.kind == TypeKind::Array) {
            if (!type.element_type) return false;
            const std::size_t count =
                type.array_size.value_or(arr_init->elements.size());
            std::string text = "[";
            for (std::size_t i = 0; i < count; ++i) {
                if (i != 0) text += ", ";
                if (i < arr_init->elements.size()) {
                    std::string element;
                    if (!build_constant_initializer(arr_init->elements[i].get(),
                        *type.element_type, element)) {
                        return false;
                    }
                    text += element;
                }
                else {
                    text += "zeroinitializer";
                }
            }
            text += "]";
            out = text;
            return true;
        }
        if (type.kind == TypeKind::Struct) {
            auto it = struct_by_name_.find(type.struct_name);
            if (it == struct_by_name_.end() || it->second == nullptr) return false;
            AST::StructDefinition* def = it->second;
            if (arr_init->elements.size() > def->members.size()) return false;
            std::string text = "{";
            for (std::size_t i = 0; i < def->members.size(); ++i) {
                if (i != 0) text += ", ";
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
        if (decl == nullptr || decl->initializer == nullptr) return false;
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
        if (constant_aggregate_globals_.empty()) return;
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
                if (scopes_.empty()) push_scope();
                scopes_.back()[decl->name] = std::move(constant);
                return;
            }
        }
        std::string type_text = llvm_type(decl->type);
        std::string address = emit_alloca(type_text, ("alloca_" + decl->name).c_str());
        LocalInfo info;
        info.type = decl->type;
        info.address = address;
        if (scopes_.empty()) push_scope();
        scopes_.back()[decl->name] = std::move(info);
        emit_debug_local_variable(decl->name, decl->type, address, decl->location);

        if (type_contains_string(decl->type)) {
            register_string_cleanup(address, decl->type);
            std::string n = std::to_string(type_size(decl->type));
            emit_line("call void @llvm.memset.p0.i64(ptr " + address +
                ", i8 0, i64 " + n + ", i1 false)");
        }
        else if (decl->type.kind == TypeKind::Struct) {
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
            if (a <= 1) return v;
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
            }
            else if (!source.value.empty()) {
                emit_line("store " + type_text + " " + source.value + ", ptr " + dest_address);
            }
            return;
        }

        if (dest_type.kind == TypeKind::Struct) {
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

    int CodeGenerator::default_constructor_index(const AST::StructDefinition* def) {
        if (def == nullptr) return -1;
        if (def->constructor_names.empty()) return 0;
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
        if (def == nullptr) return false;
        if (lifecycle_owner_ == def) return false;
        const int ctor_index = default_constructor_index(def);
        if (ctor_index < 0) {
            std::size_t required = 0;
            if (!def->constructor_param_types.empty()) {
                required = def->constructor_param_types.front().size();
                for (const std::vector<AST::Type>& params : def->constructor_param_types) {
                    if (params.size() < required) required = params.size();
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
            if (callee.empty()) return false;
            emit_line("call void " + callee + "(ptr " + address + ")");
            return true;
        }
        const std::size_t index = static_cast<std::size_t>(ctor_index - 1);
        const std::string& ctor_name = def->constructor_names[index];
        std::string callee = function_reference(ctor_name);
        if (callee.empty()) return false;
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
            if (want.kind == TypeKind::String && value.type.kind == TypeKind::String) {
                std::string addr = !value.address.empty() ? value.address : value.value;
                std::string aggregate = new_temp("temp_str");
                emit_line(aggregate + " = load %struct.gallt.string, ptr " + addr);
                call_text += ", %struct.gallt.string " + aggregate;
            }
            else {
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
        if (it == struct_by_name_.end() || it->second == nullptr) return;
        AST::StructDefinition* def = it->second;
        if (init != nullptr && init->elements.empty()) {
            if (emit_struct_default_constructor(address, def, init->location)) {
                return;
            }
        }
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
                    }
                    else if (member.type.kind == TypeKind::Struct) {
                        emit_struct_brace_initialization(field_ptr, member.type, nested);
                        handled = true;
                    }
                }
                else if (auto* e =
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
        const size_t total = array_type.array_size.value_or(count);
        for (size_t i = count; i < total; ++i) {
            std::string element_ptr = new_temp("arrayfill");
            emit_line(element_ptr + " = getelementptr " + array_ir +
                ", ptr " + address + ", i64 0, i64 " + std::to_string(i));
            if (element_type.kind == TypeKind::Struct) {
                AST::ArrayInitializer empty(init->location,
                    std::vector<std::unique_ptr<AST::Initializer>>{});
                emit_struct_brace_initialization(element_ptr, element_type, &empty);
            }
            else {
                emit_line("store " + llvm_type(element_type) +
                    " zeroinitializer, ptr " + element_ptr);
            }
        }
    }

    bool CodeGenerator::gen_operator_call(AST::Expression* expr, ExprValue& out) {
        auto found = resolved_operators_.find(expr);
        if (found == resolved_operators_.end() || found->second == nullptr) {
            return false;
        }
        AST::FunctionDefinition* callee = found->second;
        std::vector<AST::Expression*> final_arguments;
        bool postfix_dummy = false;
        if (auto* comp = dynamic_cast<AST::ComparisonExpression*>(expr)) {
            final_arguments = { comp->left.get(), comp->right.get() };
        }
        else if (auto* add = dynamic_cast<AST::AdditiveExpression*>(expr)) {
            final_arguments = { add->left.get(), add->right.get() };
        }
        else if (auto* mul = dynamic_cast<AST::MultiplicativeExpression*>(expr)) {
            final_arguments = { mul->left.get(), mul->right.get() };
        }
        else if (auto* pow = dynamic_cast<AST::PowerExpression*>(expr)) {
            final_arguments = { pow->left.get(), pow->right.get() };
        }
        else if (auto* land = dynamic_cast<AST::LogicalAndExpression*>(expr)) {
            final_arguments = { land->left.get(), land->right.get() };
        }
        else if (auto* lor = dynamic_cast<AST::LogicalOrExpression*>(expr)) {
            final_arguments = { lor->left.get(), lor->right.get() };
        }
        else if (auto* unary = dynamic_cast<AST::UnaryExpression*>(expr)) {
            final_arguments = { unary->operand.get() };
        }
        else if (auto* post = dynamic_cast<AST::PostfixExpression*>(expr)) {
            final_arguments = { post->base.get() };
            postfix_dummy = post->op == AST::PostfixExpression::Operator::Increment ||
                post->op == AST::PostfixExpression::Operator::Decrement;
            if (post->op == AST::PostfixExpression::Operator::Subscript &&
                post->subscript_expr != nullptr) {
                final_arguments.push_back(post->subscript_expr.get());
            }
        }
        else if (auto* assign = dynamic_cast<AST::AssignmentExpression*>(expr)) {
            if (assign->op == AST::AssignmentExpression::Operator::Assign) {
                return false;
            }
            final_arguments = { assign->left.get(), assign->right.get() };
        }
        else {
            return false;
        }
        out = emit_operator_invocation(callee, final_arguments, postfix_dummy,
            expr->location);
        return true;
    }

    CodeGenerator::ExprValue CodeGenerator::emit_operator_invocation(
        AST::FunctionDefinition* callee,
        std::vector<AST::Expression*>& final_arguments, bool postfix_dummy,
        SourceLocation loc) {
        if (postfix_dummy && callee->parameters.size() != 2) {
            postfix_dummy = false;
        }
        std::vector<std::unique_ptr<AST::Expression>> address_wrappers;
        std::vector<AST::UnaryExpression*> borrowed_wrappers;
        for (std::size_t i = 0; i < final_arguments.size(); ++i) {
            AST::Expression* operand = final_arguments[i];
            if (operand == nullptr || i >= callee->parameters.size()) {
                continue;
            }
            if (callee->parameters[i].kind != TypeKind::Pointer) {
                continue;
            }
            AST::Type operand_type = resolved_type(operand);
            if (operand_type.kind == TypeKind::Pointer ||
                operand_type.kind == TypeKind::Array ||
                operand_type.kind == TypeKind::Void) {
                continue;
            }
            if (!operand->is_lvalue()) {
                continue;
            }
            auto wrapper = std::make_unique<AST::UnaryExpression>(operand->location,
                AST::UnaryExpression::Operator::AddressOf,
                std::unique_ptr<AST::Expression>(operand));
            AST::UnaryExpression* wrapper_ptr = wrapper.get();
            expression_types_[wrapper_ptr] = AST::Type::make_pointer(
                std::make_shared<AST::Type>(operand_type));
            final_arguments[i] = wrapper_ptr;
            borrowed_wrappers.push_back(wrapper_ptr);
            address_wrappers.push_back(std::move(wrapper));
        }
        if (postfix_dummy) {
            lexeme_pool_.push_back("0");
            Token dummy(TokenType::IntegerLiteral, loc,
                std::string_view(lexeme_pool_.back()));
            auto dummy_node = std::make_unique<AST::PrimaryExpression>(loc, dummy);
            final_arguments.push_back(dummy_node.get());
            address_wrappers.push_back(std::move(dummy_node));
        }
        lexeme_pool_.push_back(callee->name);
        auto callee_name = std::make_unique<AST::PrimaryExpression>(loc,
            std::string_view(lexeme_pool_.back()));
        auto* callee_ptr = callee_name.get();
        auto call = std::make_unique<AST::PostfixExpression>(loc,
            std::unique_ptr<AST::Expression>(callee_name.release()),
            AST::PostfixExpression::Operator::FunctionCall);
        call->borrowed_arguments = final_arguments;
        AST::PostfixExpression* call_ptr = call.get();
        operator_extra_nodes_.push_back(std::move(call));
        for (auto& wrapper : address_wrappers) {
            operator_extra_nodes_.push_back(std::move(wrapper));
        }
        resolved_functions_[callee_ptr] = callee;
        expression_types_[call_ptr] = callee->return_type;
        ExprValue result = gen_postfix(call_ptr);
        for (AST::UnaryExpression* wrapper : borrowed_wrappers) {
            if (wrapper->operand != nullptr) {
                wrapper->operand.release();
            }
        }
        return result;
    }

    CodeGenerator::ExprValue CodeGenerator::gen_expr(AST::Expression* expr) {
        if (expr == nullptr) {
            return ExprValue{};
        }
        {
            ExprValue operator_value;
            if (gen_operator_call(expr, operator_value)) {
                return operator_value;
            }
        }
        if (auto* assign = dynamic_cast<AST::AssignmentExpression*>(expr)) {
            ExprValue left = gen_expr(assign->left.get());
            if (left.address.empty()) return ExprValue{};

            if (assign->op == AST::AssignmentExpression::Operator::Assign) {
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
            bool left_is_address = l.type.kind == TypeKind::Pointer ||
                l.type.kind == TypeKind::File || l.type.kind == TypeKind::Function;
            bool right_is_address = r.type.kind == TypeKind::Pointer ||
                r.type.kind == TypeKind::File || r.type.kind == TypeKind::Function;
            if (left_is_address || right_is_address) {
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
            AST::Type common = Type::make_int();
            if (l.type.integer_bit_width() > 0 && r.type.integer_bit_width() > 0) {
                common = (l.type.promotion_rank() >= r.type.promotion_rank())
                    ? l.type : r.type;
                if (common.integer_bit_width() == 8) {
                    common = Type::make_int();
                }
            }
            if (common.kind == TypeKind::Float || common.kind == TypeKind::Double) {
                return ExprValue{};
            }
            std::string lv = convert_value(l.value, l.type, common);
            std::string rv = convert_value(r.value, r.type, common);
            const char* prefix = common.is_unsigned_integer() ? "u" : "s";
            std::string op;
            switch (comp->op) {
            case AST::ComparisonExpression::Operator::Equal: op = "eq"; break;
            case AST::ComparisonExpression::Operator::NotEqual: op = "ne"; break;
            case AST::ComparisonExpression::Operator::Greater: op = std::string(prefix) + "gt"; break;
            case AST::ComparisonExpression::Operator::Less: op = std::string(prefix) + "lt"; break;
            case AST::ComparisonExpression::Operator::GreaterEqual: op = std::string(prefix) + "ge"; break;
            case AST::ComparisonExpression::Operator::LessEqual: op = std::string(prefix) + "le"; break;
            }
            std::string t = new_temp("icmp");
            emit_line(t + " = icmp " + op + " " + llvm_type(common) + " " + lv + ", " + rv);
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
            if (l.type.kind == TypeKind::Pointer || r.type.kind == TypeKind::Pointer) {
                bool left_ptr = l.type.kind == TypeKind::Pointer;
                AST::Type ptr_type = left_ptr ? l.type : r.type;
                std::string pointee_ir = ptr_type.pointee_type ? llvm_type(*ptr_type.pointee_type) : "i8";
                std::string base_ptr = left_ptr ? l.value : r.value;
                std::string out = new_temp("ptrmath");
                if (add->op == AST::AdditiveExpression::Operator::Minus &&
                    l.type.kind == TypeKind::Pointer && r.type.kind == TypeKind::Pointer) {
                    std::string ia = new_temp("pia");
                    std::string ib = new_temp("pib");
                    emit_line(ia + " = ptrtoint ptr " + l.value + " to i64");
                    emit_line(ib + " = ptrtoint ptr " + r.value + " to i64");
                    std::string diff = new_temp("pd");
                    emit_line(diff + " = sub i64 " + ia + ", " + ib);
                    std::size_t elem_size = ptr_type.pointee_type ? type_size(*ptr_type.pointee_type) : 1u;
                    out = new_temp("ptrsub");
                    emit_line(out + " = sdiv i64 " + diff + ", " + std::to_string(elem_size));
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
                if (common.integer_bit_width() == 8) {
                    common = Type::make_int();
                }
                AST::Type wide = common;
                lv = convert_value(l.value, l.type, wide);
                rv = convert_value(r.value, r.type, wide);
                ir_type = llvm_type(wide);
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
                if (common.integer_bit_width() == 8) {
                    common = Type::make_int();
                }
                AST::Type wide = common;
                lv = convert_value(l.value, l.type, wide);
                rv = convert_value(r.value, r.type, wide);
                ir_type = llvm_type(wide);
                const bool unsigned_op = result_type.is_unsigned_integer();
                switch (mul->op) {
                case AST::MultiplicativeExpression::Operator::Multiply: opcode = "mul"; break;
                case AST::MultiplicativeExpression::Operator::Divide:
                    opcode = unsigned_op ? "udiv" : "sdiv"; break;
                case AST::MultiplicativeExpression::Operator::Remainder:
                    opcode = unsigned_op ? "urem" : "srem"; break;
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
            if (result_type.is_integer() && l.type.is_integer() &&
                r.type.is_integer()) {
                AST::Type work_type = result_type;
                if (work_type.integer_bit_width() == 8) {
                    work_type = Type::make_int();
                }
                std::string work_ir = llvm_type(work_type);
                std::string base_addr = emit_alloca(work_ir, "powbase");
                std::string acc_addr = emit_alloca(work_ir, "powacc");
                std::string idx_addr = emit_alloca("i64", "powidx");
                emit_line("store " + work_ir + " " +
                    convert_value(l.value, l.type, work_type) + ", ptr " + base_addr);
                emit_line("store " + work_ir + " " +
                    convert_value("1", Type::make_int(), work_type) +
                    ", ptr " + acc_addr);
                emit_line("store i64 0, ptr " + idx_addr);
                std::string exponent = to_i64_value(r.value, r.type);
                std::string cond_label = new_label("powcond");
                std::string body_label = new_label("powbody");
                std::string end_label = new_label("powend");
                emit_line("br label %" + cond_label);
                start_block(cond_label);
                std::string index = new_temp("powi");
                emit_line(index + " = load i64, ptr " + idx_addr);
                std::string in_range = new_temp("powin");
                emit_line(in_range + " = icmp slt i64 " + index + ", " + exponent);
                emit_line("br i1 " + in_range + ", label %" + body_label +
                    ", label %" + end_label);
                start_block(body_label);
                std::string acc = new_temp("powaccv");
                emit_line(acc + " = load " + work_ir + ", ptr " + acc_addr);
                std::string base = new_temp("powbasev");
                emit_line(base + " = load " + work_ir + ", ptr " + base_addr);
                std::string product = new_temp("powmul");
                emit_line(product + " = mul " + work_ir + " " + acc + ", " + base);
                emit_line("store " + work_ir + " " + product + ", ptr " + acc_addr);
                std::string next_index = new_temp("pownext");
                emit_line(next_index + " = add i64 " + index + ", 1");
                emit_line("store i64 " + next_index + ", ptr " + idx_addr);
                emit_line("br label %" + cond_label);
                start_block(end_label);
                ExprValue v;
                v.type = result_type;
                std::string accumulated = new_temp("powres");
                emit_line(accumulated + " = load " + work_ir + ", ptr " + acc_addr);
                v.value = convert_value(accumulated, work_type, result_type);
                return v;
            }
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
        const int bits = type.integer_bit_width();
        if (bits > 0) {
            if (bits == 64) return value;
            std::string out = new_temp("int64");
            const char* from_ir = (bits == 32) ? "i32" : "i8";
            const char* opcode = type.is_unsigned_integer() ? "zext" : "sext";
            emit_line(out + " = " + opcode + " " + from_ir + " " + value + " to i64");
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
        AST::Type from_type = from;
        AST::Type to_type = to;
        from_type.is_const = false;
        to_type.is_const = false;
        if (from_type == to_type) return value;
        auto int_ir = [](int bits) -> const char* {
            switch (bits) {
            case 64: return "i64";
            case 32: return "i32";
            default: return "i8";
            }
        };
        if (from.kind == TypeKind::Array &&
            (to.kind == TypeKind::Pointer || to.kind == TypeKind::Function)) {
            return value;
        }
        if (from.kind == TypeKind::Pointer || from.kind == TypeKind::Function) {
            if (to.kind == TypeKind::Pointer || to.kind == TypeKind::Function ||
                to.kind == TypeKind::File) {
                return value;
            }
            if (to.kind == TypeKind::String || to.kind == TypeKind::Struct ||
                to.kind == TypeKind::Array) {
                return "zeroinitializer";
            }
            const int to_bits = to.integer_bit_width();
            if (to_bits > 0) {
                std::string converted = new_temp("ptrtoint");
                emit_line(converted + " = ptrtoint ptr " + value + " to " +
                    int_ir(to_bits));
                return converted;
            }
            if (to.kind == TypeKind::Float || to.kind == TypeKind::Double) {
                std::string converted = new_temp("ptrtoint");
                emit_line(converted + " = ptrtoint ptr " + value + " to i64");
                std::string result = new_temp("cast");
                emit_line(result + " = sitofp i64 " + converted + " to " +
                    (to.kind == TypeKind::Float ? "float" : "double"));
                return result;
            }
            return value;
        }
        if (to.kind == TypeKind::Pointer || to.kind == TypeKind::Function ||
            to.kind == TypeKind::File || from.kind == TypeKind::File) {
            if (from.kind == TypeKind::File) return value;
            if (to.kind == TypeKind::Pointer || to.kind == TypeKind::Function) {
                std::string out = new_temp("inttoptr");
                const int from_bits = from.integer_bit_width();
                if (from_bits > 0) {
                    emit_line(out + " = inttoptr " + int_ir(from_bits) + " " + value +
                        " to ptr");
                } else {
                    emit_line(out + " = inttoptr i64 0 to ptr");
                }
                return out;
            }
        }
        std::string tmp = new_temp("cast");
        if (to.kind == TypeKind::Float) {
            if (from.kind == TypeKind::Double) {
                emit_line(tmp + " = fptrunc double " + value + " to float");
                return tmp;
            } else if (from.integer_bit_width() > 0) {
                const bool unsigned_from = from.is_unsigned_integer();
                emit_line(tmp + std::string(" = ") + (unsigned_from ? "uitofp " : "sitofp ") +
                    int_ir(from.integer_bit_width()) + " " + value + " to float");
                return tmp;
            }
            return value;
        }
        if (to.kind == TypeKind::Double) {
            if (from.kind == TypeKind::Float) {
                emit_line(tmp + " = fpext float " + value + " to double");
                return tmp;
            } else if (from.integer_bit_width() > 0) {
                const bool unsigned_from = from.is_unsigned_integer();
                emit_line(tmp + std::string(" = ") + (unsigned_from ? "uitofp " : "sitofp ") +
                    int_ir(from.integer_bit_width()) + " " + value + " to double");
                return tmp;
            }
            return value;
        }
        if (from.kind == TypeKind::Float || from.kind == TypeKind::Double) {
            const int to_bits = to.integer_bit_width();
            if (to_bits <= 0) return value;
            const bool unsigned_to = to.is_unsigned_integer();
            emit_line(tmp + std::string(" = ") + (unsigned_to ? "fptoui " : "fptosi ") +
                (from.kind == TypeKind::Float ? "float" : "double") + " " + value +
                " to " + int_ir(to_bits));
            return tmp;
        }
        const int from_bits = from.integer_bit_width();
        const int to_bits = to.integer_bit_width();
        if (from_bits <= 0 || to_bits <= 0) return value;
        if (from_bits == to_bits) return value;
        const char* from_ir = int_ir(from_bits);
        const char* to_ir = int_ir(to_bits);
        if (from_bits < to_bits) {
            const char* opcode = from.is_unsigned_integer() ? "zext" : "sext";
            emit_line(tmp + " = " + opcode + " " + from_ir + " " + value + " to " + to_ir);
        }
        else {
            emit_line(tmp + " = trunc " + from_ir + " " + value + " to " + to_ir);
        }
        return tmp;
    }

    std::string CodeGenerator::truth_condition(const std::string& value, const AST::Type& type) {
        if (type.kind == TypeKind::Bool) {
            std::string tmp = new_temp("cond");
            emit_line(tmp + " = trunc i8 " + value + " to i1");
            return tmp;
        }
        if (type.integer_bit_width() > 0) {
            std::string tmp = new_temp("cond");
            const int bits = type.integer_bit_width();
            emit_line(tmp + " = icmp ne " + std::string(bits == 64 ? "i64" :
                (bits == 32 ? "i32" : "i8")) + " " + value + ", 0");
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
                std::string_view lexeme =
                    AST::Type::strip_integer_suffix(expr->literal_token.lexeme);
                int base = 10;
                if (lexeme.size() > 2 && lexeme[0] == '0' &&
                    (lexeme[1] == 'x' || lexeme[1] == 'X')) {
                    base = 16;
                    lexeme = lexeme.substr(2);
                } else if (lexeme.size() > 1 && lexeme[0] == '0') {
                    base = 8;
                    lexeme = lexeme.substr(1);
                }
                long long value = 0;
                auto res = std::from_chars(lexeme.data(), lexeme.data() + lexeme.size(),
                    value, base);
                if (res.ec == std::errc()) {
                    out.value = std::to_string(value);
                }
                else {
                    unsigned long long uvalue = 0;
                    auto ures = std::from_chars(lexeme.data(),
                        lexeme.data() + lexeme.size(), uvalue, base);
                    out.value = (ures.ec == std::errc())
                        ? std::to_string(uvalue)
                        : "0";
                }
                break;
            }
            case TokenType::FloatLiteral: {
                std::string text = std::string(expr->literal_token.lexeme);
                bool single_precision = false;
                if (!text.empty() && (text.back() == 'f' || text.back() == 'F')) {
                    text.pop_back();
                    single_precision = true;
                }
                double d = std::strtod(text.c_str(), nullptr);
                if (single_precision || resolved_type(expr).kind == TypeKind::Float) {
                    d = static_cast<double>(static_cast<float>(d));
                }
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
                if (local->is_constant) {
                    out.type = local->type;
                    if (!local->address.empty()) {
                        out.address = local->address;
                        if (local->type.kind == TypeKind::Array &&
                            local->type.element_type) {
                            out.value = new_temp("constdecay");
                            emit_line(out.value + " = getelementptr " +
                                llvm_type(local->type) + ", ptr " + local->address +
                                ", i64 0, i64 0");
                            return out;
                        }
                        out.is_lvalue = true;
                        out.value = new_temp("constload");
                        emit_line(out.value + " = load " + llvm_type(local->type) +
                            ", ptr " + local->address);
                        return out;
                    }
                    if (local->constant_expr != nullptr) {
                        return gen_expr(const_cast<AST::Expression*>(local->constant_expr));
                    }
                    out.value = local->constant_text;
                    return out;
                }
                out.is_lvalue = true;
                out.address = local->address;
                if (local->type.kind == TypeKind::Array) {
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
            if (!expr->lowered_ctor.empty()) {
                std::string ctor_name = expr->lowered_ctor;
                if (auto resolved = resolved_functions_.find(expr);
                    resolved != resolved_functions_.end() && resolved->second != nullptr) {
                    ctor_name = resolved->second->name;
                }
                std::string callee = function_reference(ctor_name);
                if (!callee.empty()) {
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
            AST::ArrayInitializer empty_init(expr->location,
                std::vector<std::unique_ptr<AST::Initializer>>{});
            emit_struct_brace_initialization(storage, constructed, &empty_init);
            return out;
        }
        case AST::PrimaryExpression::Kind::CopyMove: {
            AST::Type operand_type;
            std::string source_address = operand_address(expr->paren_expr.get(), &operand_type);
            if (source_address.empty() ||
                (operand_type.kind != AST::TypeKind::Struct &&
                    operand_type.kind != AST::TypeKind::String)) {
                return gen_expr(expr->paren_expr.get());
            }
            std::string temp = emit_alloca(llvm_type(operand_type), "copy_move_temp");
            emit_line("store " + llvm_type(operand_type) + " zeroinitializer, ptr " + temp);
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
            return out;
        case AST::PrimaryExpression::Kind::NamespaceQualified:
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
                ExprValue base;
                bool operator_arrow = false;
                if (post->op == AST::PostfixExpression::Operator::Arrow) {
                    auto arrow_it = resolved_operators_.find(post->base.get());
                    if (arrow_it != resolved_operators_.end() &&
                        arrow_it->second != nullptr) {
                        std::vector<AST::Expression*> arrow_arguments = {
                            post->base.get()
                        };
                        ExprValue arrow_result = emit_operator_invocation(
                            arrow_it->second, arrow_arguments, false, post->location);
                        if (arrow_result.type.kind == TypeKind::Pointer) {
                            base = arrow_result;
                            operator_arrow = true;
                        }
                    }
                }
                if (!operator_arrow) {
                    base = gen_expr(post->base.get());
                }
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
            if (auto* prim = dynamic_cast<AST::PrimaryExpression*>(expr->operand.get())) {
                if (prim->kind == AST::PrimaryExpression::Kind::Identifier) {
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
            ExprValue v = gen_expr(expr->operand.get());
            out.type = resolved_type(expr);
            if (expr->op == AST::UnaryExpression::Operator::UnaryPlus) {
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
            AST::Type neg_type = v.type;
            if (neg_type.integer_bit_width() <= 0) {
                neg_type = Type::make_int();
            }
            else if (neg_type.integer_bit_width() == 8) {
                neg_type = Type::make_int();
            }
            std::string operand_value = convert_value(v.value, v.type, neg_type);
            std::string negated = new_temp("neg");
            emit_line(negated + " = sub " + llvm_type(neg_type) + " 0, " + operand_value);
            out.value = convert_value(negated, neg_type, out.type);
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

    std::string CodeGenerator::lifecycle_symbol(const AST::StructDefinition* def,
        LifecycleKind kind) const {
        if (def == nullptr) return std::string();
        switch (kind) {
        case LifecycleKind::Constructor:
            return def->constructor_names.empty()
                ? ("__sgc_ctor$" + def->name) : std::string();
        case LifecycleKind::Destructor:
            return def->destructor_name.empty()
                ? ("__sgc_dtor$" + def->name) : std::string();
        case LifecycleKind::CopyConstructor:
            if (def->no_copy || ast_function_exists(def->copy_constructor_name)) {
                return std::string();
            }
            return "__sgc_copyctor$" + def->name;
        case LifecycleKind::MoveConstructor:
            if (def->no_move || ast_function_exists(def->move_constructor_name)) {
                return std::string();
            }
            return "__sgc_movector$" + def->name;
        case LifecycleKind::CopyAssignment:
            if (def->no_copy || ast_function_exists(def->copy_assignment_name)) {
                return std::string();
            }
            return "__sgc_copyassign$" + def->name;
        case LifecycleKind::MoveAssignment:
            if (def->no_move || ast_function_exists(def->move_assignment_name)) {
                return std::string();
            }
            return "__sgc_moveassign$" + def->name;
        }
        return std::string();
    }

    void CodeGenerator::emit_lifecycle_functions() {
        const LifecycleKind kinds[] = {
            LifecycleKind::Constructor,
            LifecycleKind::Destructor,
            LifecycleKind::CopyConstructor,
            LifecycleKind::MoveConstructor,
            LifecycleKind::CopyAssignment,
            LifecycleKind::MoveAssignment,
        };
        for (AST::StructDefinition* def : struct_defs_) {
            if (def == nullptr) continue;
            for (LifecycleKind kind : kinds) {
                const std::string symbol = lifecycle_symbol(def, kind);
                if (symbol.empty() || lifecycle_symbols_.count(symbol) == 0) continue;
                emit_lifecycle_body(def, kind, symbol);
            }
        }
    }

    void CodeGenerator::register_lifecycle_symbols() {
        const LifecycleKind kinds[] = {
            LifecycleKind::Constructor,
            LifecycleKind::Destructor,
            LifecycleKind::CopyConstructor,
            LifecycleKind::MoveConstructor,
            LifecycleKind::CopyAssignment,
            LifecycleKind::MoveAssignment,
        };
        for (AST::StructDefinition* def : struct_defs_) {
            if (def == nullptr) continue;
            const AST::Type struct_type = AST::Type::make_struct(def->name);
            for (LifecycleKind kind : kinds) {
                std::string symbol = lifecycle_symbol(def, kind);
                if (symbol.empty()) continue;
                if (kind == LifecycleKind::CopyConstructor ||
                    kind == LifecycleKind::CopyAssignment) {
                    if (!type_is_copyable(struct_type)) continue;
                }
                if (kind == LifecycleKind::MoveConstructor ||
                    kind == LifecycleKind::MoveAssignment) {
                    if (!type_is_movable(struct_type)) continue;
                }
                lifecycle_symbols_.insert(symbol);
            }
        }
    }

    void CodeGenerator::emit_lifecycle_body(AST::StructDefinition* def,
        LifecycleKind kind, const std::string& name) {
        debug_subprogram_id_ = 0;
        debug_location_valid_ = false;
        const AST::Type struct_type = AST::Type::make_struct(def->name);
        scopes_.clear();
        cleanup_scopes_.clear();
        push_scope();
        emitted_labels_.clear();
        current_label_.clear();
        break_labels_.clear();
        current_function_ = nullptr;
        hoisted_allocas_.clear();
        hoist_insert_index_ = 0;
        current_block_terminated_ = true;
        current_sret_pointer_.clear();
        pending_sret_destination_.clear();
        statement_temporaries_.clear();

        const bool two_parameters = kind != LifecycleKind::Constructor &&
            kind != LifecycleKind::Destructor;
        std::string header = "define void @glt_" + name + "(ptr %this";
        if (two_parameters) header += ", ptr %source";
        header += ") {";
        emit_line(header);
        start_block(new_label("entry"));
        hoist_insert_index_ = lines_.size();

        emitting_lifecycle_body_ = true;
        lifecycle_owner_ = def;
        switch (kind) {
        case LifecycleKind::Constructor: {
            AST::ArrayInitializer empty(def->location,
                std::vector<std::unique_ptr<AST::Initializer>>{});
            emit_struct_brace_initialization("%this", struct_type, &empty);
            break;
        }
        case LifecycleKind::Destructor: {
            const std::string ir = llvm_type(struct_type);
            for (std::size_t i = 0; i < def->members.size(); ++i) {
                std::string field = new_temp("dtor_field");
                emit_line(field + " = getelementptr " + ir + ", ptr %this, i32 0, i32 " +
                    std::to_string(i));
                emit_destroy_string_at(def->members[i].type, field);
            }
            break;
        }
        case LifecycleKind::CopyConstructor:
            emit_memberwise_copy(struct_type, "%this", "%source", false);
            break;
        case LifecycleKind::MoveConstructor:
            emit_memberwise_move(struct_type, "%this", "%source", false);
            break;
        case LifecycleKind::CopyAssignment:
            emit_memberwise_copy(struct_type, "%this", "%source", true);
            break;
        case LifecycleKind::MoveAssignment:
            emit_memberwise_move(struct_type, "%this", "%source", true);
            break;
        }
        lifecycle_owner_ = nullptr;
        emitting_lifecycle_body_ = false;

        if (!current_block_terminated_) {
            destroy_active_cleanup_scopes(0);
            emit_line("ret void");
        }
        flush_hoisted_allocas();
        emit_line("}");
        discard_current_cleanup_scope();
        pop_scope();
        emitted_lifecycle_bodies_.insert(name);
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
            if (is_file_builtin_name(direct_name)) {
                return emit_file_builtin_call(expr, direct_name);
            }
            if (direct_name == "size" || direct_name == "align") {
                return emit_size_align_call(expr, direct_name == "size");
            }
            if (direct != nullptr) {
                auto struct_it = struct_by_name_.find(direct->identifier);
                if (struct_it != struct_by_name_.end() && struct_it->second != nullptr &&
                    struct_it->second->constructor_names.empty() &&
                    expr->arguments.empty()) {
                    AST::StructDefinition* def = struct_it->second;
                    AST::Type temp_type = AST::Type::make_struct(direct->identifier);
                    std::string storage = emit_alloca(llvm_type(temp_type), "value_temp");
                    std::string callee = function_reference("__sgc_ctor$" + def->name);
                    if (!callee.empty()) {
                        emit_line("call void " + callee + "(ptr " + storage + ")");
                    }
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
                if (struct_it != struct_by_name_.end() && struct_it->second != nullptr &&
                    !struct_it->second->constructor_names.empty()) {
                    AST::StructDefinition* def = struct_it->second;
                    AST::Type temp_type = AST::Type::make_struct(direct->identifier);
                    std::string storage = emit_alloca(llvm_type(temp_type), "value_temp");
                    std::string resolved_ctor_name;
                    if (auto resolved = resolved_functions_.find(direct);
                        resolved != resolved_functions_.end() && resolved->second != nullptr) {
                        resolved_ctor_name = resolved->second->name;
                    }
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
                            ExprValue base_value = gen_expr(member_access->base.get());
                            std::string address = base_value.value;
                            if (member_access->op == AST::PostfixExpression::Operator::Dot) {
                                std::string object_address =
                                    gen_address(member_access->base.get());
                                if (!object_address.empty()) address = object_address;
                            }
                            auto it = struct_by_name_.find(owner.struct_name);
                            if (it != struct_by_name_.end() && it->second != nullptr) {
                                AST::StructDefinition* def = it->second;
                                std::string symbol = def->destructor_name;
                                if (symbol.empty()) {
                                    symbol = "__sgc_dtor$" + def->name;
                                }
                                std::string callee = function_reference(symbol);
                                if (!callee.empty() && !address.empty()) {
                                    emit_line("call void " + callee + "(ptr " + address + ")");
                                }
                            }
                        }
                        return out;
                    }
                }
            }

            AST::Type return_type = out.type;
            AST::Type func_type;
            std::vector<AST::Type> params;
            std::string callee;
            bool pointer_call = false;
            if (!direct_name.empty()) {
                if (direct_name == "main") {
                    callee = "@main";
                } else {
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
                    }
                    else {
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

           std::vector<std::string> ir_args;
            std::vector<std::string> owned_args;
            std::vector<AST::Expression*> all_args;
            if (!expr->arguments.empty() || !expr->appended_defaults.empty()) {
                all_args.reserve(expr->arguments.size() + expr->appended_defaults.size());
                for (auto& a : expr->arguments) all_args.push_back(a.get());
                for (AST::Expression* d : expr->appended_defaults) all_args.push_back(d);
            }
            else {
                all_args = expr->borrowed_arguments;
            }
            for (size_t i = 0; i < all_args.size(); ++i) {
                ExprValue arg = gen_expr(all_args[i]);
                if (!arg.owned_string.empty()) {
                    owned_args.push_back(arg.owned_string);
                }
                AST::Type want = (i < params.size()) ? params[i] : arg.type;
                bool extern_call = !direct_name.empty() && function_is_extern(direct_name);
                if (want.kind == TypeKind::String && extern_call) {
                    std::string addr = !arg.address.empty() ? arg.address : arg.value;
                    ir_args.push_back(string_cstr_pointer(addr));
                } else if (want.kind == TypeKind::String && arg.type.kind == TypeKind::String) {
                    std::string addr = !arg.address.empty() ? arg.address : arg.value;
                    std::string agg = new_temp("stringarg");
                    emit_line(agg + " = load %struct.gallt.string, ptr " + addr);
                    ir_args.push_back(agg);
                } else if (want.kind == TypeKind::Struct && arg.type.kind == TypeKind::Struct) {
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
                emit_line("call void @gallt_check_fptr(ptr " + callee + ")");
            }
            bool extern_call = !direct_name.empty() && function_is_extern(direct_name);
            bool sret_call = returns_via_sret(return_type) && !extern_call;
            std::string sret_storage;
            if (sret_call) {
                if (!pending_sret_destination_.empty()) {
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
        return extern_by_name_.find(name) != extern_by_name_.end();
    }

    std::string CodeGenerator::string_cstr_pointer(const std::string& value) {
        std::string tmp = new_temp("cstr");
        emit_line(tmp + " = call ptr @gallt_string_cstr(ptr " + value + ")");
        return tmp;
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

    CodeGenerator::ExprValue CodeGenerator::gen_binary_string_plus(
        AST::Expression* left, AST::Expression* right) {
        ExprValue l = gen_expr(left);
        ExprValue r = gen_expr(right);
        ExprValue out;
        out.type = Type::make_string();
        bool ls_owned = false;
        bool rs_owned = false;
        std::string ls = string_value_or_converted(l, &ls_owned);
        std::string rs = string_value_or_converted(r, &rs_owned);
        out.value = emit_alloca(llvm_type(Type::make_string()), "concat_result");
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
        case TypeKind::Uint:
            emit_line("call void @gallt_string_from_u32(ptr " + result +
                ", i32 " + v.value + ")");
            break;
        case TypeKind::Lint:
            emit_line("call void @gallt_string_from_i64(ptr " + result +
                ", i64 " + v.value + ")");
            break;
        case TypeKind::Luint:
            emit_line("call void @gallt_string_from_u64(ptr " + result +
                ", i64 " + v.value + ")");
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
        case TypeKind::Uchar:
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
        return std::string(kGalltRuntimeCSource);
    }

} 
