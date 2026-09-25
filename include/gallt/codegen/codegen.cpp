#include "codegen.hpp"
#include "codegen_detail.hpp"
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
    using namespace codegen_detail;

    CodeGenerator::CodeGenerator(
        AST::Program* program,
        const std::unordered_map<const AST::Expression*, AST::Type>& expression_types,
        const std::unordered_map<const AST::PrimaryExpression*,
            const AST::FunctionDefinition*>& resolved_functions,
        const std::unordered_map<const AST::PrimaryExpression*,
            const AST::ExternDeclaration*>& resolved_externs,
        const std::unordered_map<const AST::Expression*,
            AST::FunctionDefinition*>& resolved_operators,
        DiagnosticEngine* diagnostics, int debug_symbols_level,
        bool emit_entry_point, bool no_runtime, bool gallt_abi)
        : program_(program), expression_types_(expression_types),
        resolved_functions_(resolved_functions), resolved_externs_(resolved_externs),
        resolved_operators_(resolved_operators), diagnostics_(diagnostics),
        debug_level_(debug_symbols_level), emit_entry_point_(emit_entry_point),
        no_runtime_(no_runtime), gallt_abi_(gallt_abi) {
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
        if (hoisted_allocas_.empty()) { return; }
        std::size_t pos = hoist_insert_index_;
        if (pos > lines_.size()) { pos = lines_.size(); }
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
                for (auto& s : block->statements) { walk(s.get()); }
            } else if (auto* ifs = dynamic_cast<AST::IfStatement*>(stmt)) {
                walk(ifs->then_block.get());
                if (ifs->else_block) { walk(ifs->else_block.get()); }
            } else if (auto* for_ = dynamic_cast<AST::ForStatement*>(stmt)) {
                if (for_->init) { walk(for_->init.get()); }
                if (for_->body) { walk(for_->body.get()); }
            } else if (auto* while_ = dynamic_cast<AST::WhileStatement*>(stmt)) {
                if (while_->body) { walk(while_->body.get()); }
            }
        };

        for (auto& top : program_->top_levels) {
            if (auto* st = dynamic_cast<AST::StructDefinition*>(top.get())) {
                if (!struct_by_name_.count(st->name)) {
                    struct_by_name_[st->name] = st;
                    struct_defs_.push_back(st);
                }
            } else if (auto* func = dynamic_cast<AST::FunctionDefinition*>(top.get())) {
                if (func->body) { walk(func->body.get()); }
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
        if (type.kind == TypeKind::String) { return true; }
        if (type.kind == TypeKind::Array && type.element_type) {
            return type_contains_string(*type.element_type);
        }
        if (type.kind == TypeKind::Struct) {
            auto it = struct_by_name_.find(type.struct_name);
            if (it != struct_by_name_.end()) {
                for (const auto& m : it->second->members) {
                    if (type_contains_string(m.type)) { return true; }
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

        if (emit_entry_point_) {
            emit_main_wrapper();
        }

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
            if (top == nullptr) { continue; }
            if (!top->location.filename.empty()) {
                debug_source_file_ = std::string(top->location.filename);
                break;
            }
        }

        if (debug_source_file_.empty()) { return; }
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
        if (type.kind == TypeKind::Void) { return 0; }
        const std::string key = type.to_string();
        auto cached = debug_type_ids_.find(key);
        if (cached != debug_type_ids_.end()) { return cached->second; }
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
                if (i != 0) { list += ", "; }
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
        if (cached != debug_subroutine_ids_.end()) { return cached->second; }
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
        if (debug_level_ < 1 || debug_subprogram_id_ == 0) { return 0; }
        const unsigned line = location.line > 0
            ? static_cast<unsigned>(location.line) : 1u;
        const unsigned column = location.column > 0
            ? static_cast<unsigned>(location.column) : 1u;
        const std::string key = std::to_string(debug_subprogram_id_) + ":" +
            std::to_string(line) + ":" + std::to_string(column);
        auto cached = debug_location_ids_.find(key);

        if (cached != debug_location_ids_.end()) { return cached->second; }
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

        if (debug_level_ < 1 || func == nullptr) { return; }
        const unsigned line = func->location.line > 0
            ? static_cast<unsigned>(func->location.line) : 1u;
        debug_subprogram_id_ = next_debug_id();
        const unsigned signature = debug_subroutine_id(func->return_type,
            func->parameters);
        const std::string ir_name = function_llvm_name_for_source(func->name);
        std::string symbol = ir_name;
        if (!symbol.empty() && symbol.front() == '@') { symbol.erase(0, 1); }

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
        if (debug_level_ < 2 || debug_subprogram_id_ == 0) { return; }
        if (address.empty() || name.empty()) { return; }
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
        if (debug_level_ < 1) { return; }
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
            if (emitted.count(def->name)) { return; }
            emitted.insert(def->name);

            for (const auto& m : def->members) {
                std::function<void(const AST::Type&)> deps = [&](const AST::Type& t) {
                    if (t.kind == TypeKind::Struct) {
                        auto it = struct_by_name_.find(t.struct_name);
                        if (it != struct_by_name_.end()) { visit(it->second); }
                    } else if (t.kind == TypeKind::Array && t.element_type) {
                        deps(*t.element_type);
                    }
                };
                deps(m.type);
            }

            std::string body = "{ ";
            for (size_t i = 0; i < def->members.size(); ++i) {
                if (i != 0) { body += ", "; }
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
        emit_line("declare i32 @gallt_string_length(ptr)");
        emit_line("declare i32 @gallt_string_compare(ptr, ptr)");
        emit_line("declare void @gallt_string_copy(ptr, ptr)");
        emit_line("declare void @gallt_string_move(ptr, ptr)");
        emit_line("declare void @gallt_string_substr(ptr, ptr, i32, i32)");
        emit_line("declare i32 @gallt_string_find(ptr, ptr)");
        emit_line("declare i8 @gallt_string_contains(ptr, ptr)");
        emit_line("declare void @gallt_string_replace(ptr, ptr, ptr, ptr)");
        emit_line("declare void @gallt_string_upper(ptr, ptr)");
        emit_line("declare void @gallt_string_lower(ptr, ptr)");
        emit_line("declare void @gallt_string_trim(ptr, ptr)");
        emit_line("declare i8 @gallt_string_char_at(ptr, i32)");
        emit_line("declare i8 @gallt_string_set_char(ptr, i32, i8)");
        emit_line("declare i32 @gallt_string_split_count(ptr, ptr)");
        emit_line("declare void @gallt_string_split_at(ptr, ptr, ptr, i32)");
        emit_line("declare void @gallt_string_read(ptr)");
        emit_line("declare void @gallt_string_write(ptr)");
        emit_line("declare i32 @gallt_string_write_file(ptr, ptr)");
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
        return source_function_symbol(std::string(name));
    }

    std::string CodeGenerator::source_function_symbol(const std::string& name) const {
        if (name == "main") { return "@glt_main"; }
        if (is_exported_function(name)) { return "@" + name; }
        return "@glt_" + name;
    }

    bool CodeGenerator::aggregate_parameter_uses_pointer(const AST::Type& type) {
        return type.kind == TypeKind::Struct || type.kind == TypeKind::String;
    }

    std::string CodeGenerator::parameter_ir_type(const AST::Type& type) {
        if (aggregate_parameter_uses_pointer(type)) { return "ptr"; }
        return llvm_type(type);
    }

    std::string CodeGenerator::aggregate_argument_pointer(const AST::Type& type,
        ExprValue& value) {
        if (!value.address.empty()) { return value.address; }
        if (value.value.empty()) { return std::string(); }
        if (type.kind == TypeKind::String || value.type.kind == TypeKind::String) {
            return value.value;
        }
        std::string storage = emit_alloca(llvm_type(type), "argcopy");
        emit_line("store " + llvm_type(type) + " " + value.value +
            ", ptr " + storage);
        return storage;
    }

    bool CodeGenerator::is_exported_function(const std::string& name) const {
        return std::find(exported_functions_.begin(), exported_functions_.end(),
            name) != exported_functions_.end();
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
            if (!ext) { continue; }
            if (defined_names.count(ext->name)) { continue; }
            std::string symbol = extern_ir_symbol(ext);
            if (!declared_extern_symbols_.insert(symbol).second) { continue; }

            std::string ret = llvm_type(ext->return_type);
            if (ext->return_type.kind == TypeKind::Function) { ret = "ptr"; }
            std::string sig = ret + " @" + symbol + "(";
            bool sret_declaration = gallt_abi_ && returns_via_sret(ext->return_type);
            if (sret_declaration) { sig = "void @" + symbol + "(ptr"; }

            for (size_t i = 0; i < ext->parameters.size(); ++i) {
                if (i != 0 || sret_declaration) { sig += ", "; }
                sig += parameter_ir_type(ext->parameters[i]);
            }

            if (ext->c_variadic) {
                if (!ext->parameters.empty() || sret_declaration) { sig += ", "; }
                sig += "...";
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
                } else {
                    global_vars_.push_back(var);
                }
            }
        }
    }

    void CodeGenerator::collect_function_signatures() {
        function_by_name_.clear();
        extern_by_name_.clear();
        exported_functions_.clear();

        for (const auto& top : program_->top_levels) {
            if (auto* func = dynamic_cast<AST::FunctionDefinition*>(top.get())) {
                function_by_name_[func->name] = func;
                if (func->is_export) {
                    if (!is_exported_function(func->name)) {
                        exported_functions_.push_back(func->name);
                    }
                }
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
            emit_line(address + " = " + module_local_prefix() + "global " +
                llvm_type(var->type) +
                " zeroinitializer");

            LocalInfo fallback;
            fallback.type = var->type;
            fallback.address = address;
            global_symbols_[var->name] = std::move(fallback);
        }

        for (AST::VariableDeclaration* var : global_vars_) {
            std::string address = "@glt_g_" + var->name;
            std::string type_text = llvm_type(var->type);
            std::string definition = address + " = " + module_local_prefix() +
                "global " + type_text + " zeroinitializer";

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
        if (global_vars_.empty() && runtime_const_globals_.empty()) { return; }

        debug_subprogram_id_ = 0;
        debug_location_valid_ = false;
        scopes_.clear();
        cleanup_scopes_.clear();
        push_scope();
        emitted_labels_.clear();
        current_label_.clear();
        current_block_terminated_ = true;

        emit_line("define " + module_local_prefix() + "void @glt_global_init() {");
        std::string entry = new_label("entry");
        start_block(entry);
        hoisted_allocas_.clear();
        hoist_insert_index_ = lines_.size();

        for (AST::VariableDeclaration* var : global_vars_) {
            if (var->initializer) {
                emit_initializer_to_address(var, "@glt_g_" + var->name);
            } else if (var->type.kind == TypeKind::Struct) {
                AST::ArrayInitializer empty_init(var->location,
                    std::vector<std::unique_ptr<AST::Initializer>>{});
                emit_struct_brace_initialization("@glt_g_" + var->name, var->type,
                    &empty_init);
            }
        }

        for (AST::VariableDeclaration* var : runtime_const_globals_) {
            if (var->initializer) {
                emit_initializer_to_address(var, "@glt_g_" + var->name);
            } else if (var->type.kind == TypeKind::Struct) {
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

        emit_line("define " + module_local_prefix() +
            "void @glt_global_deinit() {");
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
            if (!destructible && !type_contains_string(var->type)) { continue; }
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
            if (!destructible && !type_contains_string(var->type)) { continue; }
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

        if (main_func == nullptr) { return; }

        std::string entry = new_label("main_wrapper");
        emit_line("define i32 @main(i32 %argc, ptr %argv) {");
        start_block(entry);
        std::string args;
        if (main_func->parameters.size() >= 2) {
            std::string second_type = llvm_type(main_func->parameters[1]);
            args = "i32 %argc, " + second_type + " %argv";
        } else if (main_func->parameters.size() == 1) {
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
        if (!decl->initializer) { return; }

        if (auto* expr_init = dynamic_cast<AST::ExpressionInitializer*>(decl->initializer.get())) {
            ExprValue value = gen_expr(expr_init->expr.get());
            emit_aggregate_assign(address, decl->type, value);
            return;
        }

        auto* arr_init = dynamic_cast<AST::ArrayInitializer*>(decl->initializer.get());
        if (!arr_init) { return; }
        if (decl->type.kind == TypeKind::Array) {
            emit_array_brace_initialization(address, decl->type, arr_init);
        } else if (decl->type.kind == TypeKind::Struct) {
            emit_struct_brace_initialization(address, decl->type, arr_init);
        }
    }

    CodeGenerator::LocalInfo* CodeGenerator::lookup_local(const std::string& name) {
        for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
            auto found = it->find(name);
            if (found != it->end()) { return &found->second; }
        }

        auto global = global_symbols_.find(name);
        if (global != global_symbols_.end()) { return &global->second; }
        return nullptr;
    }

    void CodeGenerator::push_scope() {
        scopes_.emplace_back();
        cleanup_scopes_.emplace_back();
    }

    void CodeGenerator::pop_scope() {
        if (scopes_.empty()) { return; }

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
        if (cleanup_scopes_.empty()) { return; }
        cleanup_scopes_.back().push_back(CleanupRecord{ type, address });
    }

    void CodeGenerator::destroy_owned_string(ExprValue& value) {
        if (value.owned_string.empty()) { return; }
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
        if (cleanup_scopes_.size() <= until_depth) { return; }
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
        if (func->return_type.kind == TypeKind::Function) { ret = "ptr"; }
        bool sret = returns_via_sret(func->return_type);

        const std::string linkage = (func->name != "main" &&
            !is_exported_function(func->name)) ? module_local_prefix() : std::string();
        std::string header = "define " + linkage + ret + " " + name + "(";
        if (sret) {
            ret = "void";
            header = "define " + linkage + "void " + name + "(ptr %__sret_ret";
        }

        const bool variadic = func->is_variadic && !func->parameters.empty() &&
            func->param_names.size() == func->parameters.size() &&
            !func->param_names.back().empty();
        const size_t fixed_params = variadic
            ? func->parameters.size() - 1 : func->parameters.size();
        variadic_locals_.clear();
        bool header_has_parameter = sret;

        for (size_t i = 0; i < fixed_params; ++i) {
            if (header_has_parameter) { header += ", "; }
            header_has_parameter = true;
            header += parameter_ir_type(func->parameters[i]);
            std::string param_name = (i < func->param_names.size() && !func->param_names[i].empty())
                ? func->param_names[i]
                : "_arg" + std::to_string(i);
            header += " %" + param_name;
        }

        std::string pack_name;
        if (variadic) {
            pack_name = func->param_names.back();
            if (header_has_parameter) { header += ", "; }
            header_has_parameter = true;
            header += "ptr %" + pack_name + "$data";
            header += ", i32 %" + pack_name + "$len";
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

        for (size_t i = 0; i < fixed_params; ++i) {
            std::string param_name = (i < func->param_names.size() && !func->param_names[i].empty())
                ? func->param_names[i]
                : "_arg" + std::to_string(i);
            std::string type_text = llvm_type(func->parameters[i]);
            std::string address = emit_alloca(type_text, ("alloca_" + param_name).c_str());
            std::string incoming = "%" + param_name;
            const AST::Type& param_type = func->parameters[i];
            if (aggregate_parameter_uses_pointer(param_type)) {
                emit_line("store " + type_text + " zeroinitializer, ptr " + address);
                emit_memberwise_copy(param_type, address, incoming, false);

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

        if (variadic) {
            VariadicPackLocal pack;
            pack.name = pack_name;
            pack.element = func->parameters.back();
            pack.data_slot = emit_alloca("ptr", "packdata");
            pack.length_slot = emit_alloca("i32", "packlen");
            emit_line("store ptr %" + pack_name + "$data, ptr " + pack.data_slot);
            emit_line("store i32 %" + pack_name + "$len, ptr " + pack.length_slot);
            variadic_locals_.push_back(std::move(pack));
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
            if (func->return_type.kind == TypeKind::Int) {
                zero = "0";
            } else if (func->return_type.kind == TypeKind::Float) {
                zero = "0.0";
            } else if (func->return_type.kind == TypeKind::Double) {
                zero = "0.0";
            } else if (func->return_type.kind == TypeKind::Char || func->return_type.kind == TypeKind::Bool) {
                zero = "0";
            } else if (func->return_type.kind == TypeKind::Pointer ||
                func->return_type.kind == TypeKind::Function) {
                zero = "null";
            }
            emit_line("ret " + type_text + " " + zero);
        }

        flush_hoisted_allocas();
        emit_line("}");
        discard_current_cleanup_scope();
        pop_scope();
    }

    void CodeGenerator::emit_block(AST::Block* block, bool new_scope) {
        if (block == nullptr) { return; }
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
            } else {
                statement_temporaries_.resize(temp_mark);
            }
        }

        if (new_scope) {
            pop_scope();
        }
    }

    void CodeGenerator::emit_statement(AST::Statement* stmt) {
        if (stmt == nullptr) { return; }
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
            if (for_stmt->init) { emit_statement(for_stmt->init.get()); }

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

    std::string CodeGenerator::runtime_c_source() {
        return std::string(kGalltRuntimeCSource);
    }

}
