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

    TypeChecker::TypeChecker(DiagnosticEngine& diag,
        const std::unordered_map<const AST::Expression*, std::string>&
            expression_free_identifiers,
        const std::unordered_map<const AST::Expression*,
            std::tuple<std::string, std::size_t, AST::Type>>&
            expression_argument_casts,
        bool require_main)
        : diag_(diag), expression_free_identifiers_(expression_free_identifiers),
        expression_argument_casts_(expression_argument_casts),
        require_main_(require_main) {
    }

    bool TypeChecker::check_program(AST::Program* program) {
        if (program == nullptr) {
            return false;
        }
        program_ = program;

        enter_scope();

        declare_builtin_functions();

        for (auto& top : program->top_levels) {
            if (auto* struct_def = dynamic_cast<AST::StructDefinition*>(top.get())) {
                if (struct_defs_.find(struct_def->name) != struct_defs_.end()) {
                    report_error(struct_def->location, ErrorCode::RedefinedIdentifier,
                        "struct '" + struct_def->name + "' already defined");
                }
                else {
                    struct_defs_[struct_def->name] = struct_def;
                    Symbol sym = Symbol::make_struct(struct_def->name, struct_def->location, struct_def);
                    if (!sym_table_.declare(sym)) {
                        report_error(struct_def->location, ErrorCode::RedefinedIdentifier,
                            "struct '" + struct_def->name + "' already declared");
                    }
                }
            }
        }

        collect_operator_overloads();

        for (auto& top : program->top_levels) {
            if (auto* func = dynamic_cast<AST::FunctionDefinition*>(top.get())) {
                collect_local_structs(func->body.get());
            }
            else if (auto* strct = dynamic_cast<AST::StructDefinition*>(top.get())) {
                for (auto& member : strct->special_members) {
                    if (member != nullptr && member->body != nullptr) {
                        collect_local_structs(member->body.get());
                    }
                }
            }
        }

        for (auto& top : program->top_levels) {
            check_top_level(top.get());
        }

        for (auto& stmt : program->global_initializers) {
            check_statement(stmt.get());
        }

        if (require_main_) {
            verify_main_function();
        }

        exit_scope();

        return !diag_.has_errors();
    }

    void TypeChecker::check_top_level(AST::TopLevel* node) {
        if (auto* guide = dynamic_cast<AST::GuideStatement*>(node)) {
            check_guide_statement(guide);
        }
        else if (auto* clib = dynamic_cast<AST::ClibStatement*>(node)) {
            check_clib_statement(clib);
        }
        else if (auto* ext = dynamic_cast<AST::ExternDeclaration*>(node)) {
            check_extern_declaration(ext);
        }
        else if (auto* func = dynamic_cast<AST::FunctionDefinition*>(node)) {
            check_function_definition(func);
        }
        else if (auto* var = dynamic_cast<AST::VariableDeclaration*>(node)) {
            check_variable_declaration(var);
        }
        else if (auto* st = dynamic_cast<AST::StructDefinition*>(node)) {
            check_struct_definition(st);
        }
        else {
            report_error(node->location, ErrorCode::ExpressionSyntaxError,
                "unknown top-level node");
        }
    }

    void TypeChecker::check_guide_statement(AST::GuideStatement* node) {
        if (node->path.empty()) {
            report_error(node->location, ErrorCode::LibraryNotFound,
                "guide path cannot be empty");
        }
    }

    void TypeChecker::check_clib_statement(AST::ClibStatement* node) {
        if (node->library_name.empty()) {
            report_error(node->location, ErrorCode::LibraryNotFound,
                "clib library name cannot be empty");
        }
    }

    void TypeChecker::check_statement(AST::Statement* stmt) {
        if (auto* block = dynamic_cast<AST::Block*>(stmt)) {
            check_block(block);
        }
        else if (auto* var = dynamic_cast<AST::VariableDeclaration*>(stmt)) {
            check_variable_declaration(var);
        }
        else if (auto* if_ = dynamic_cast<AST::IfStatement*>(stmt)) {
            check_if_statement(if_);
        }
        else if (auto* for_ = dynamic_cast<AST::ForStatement*>(stmt)) {
            check_for_statement(for_);
        }
        else if (auto* while_ = dynamic_cast<AST::WhileStatement*>(stmt)) {
            check_while_statement(while_);
        }
        else if (auto* br = dynamic_cast<AST::BreakStatement*>(stmt)) {
            check_break_statement(br);
        }
        else if (auto* ret = dynamic_cast<AST::ReturnStatement*>(stmt)) {
            check_return_statement(ret);
        }
        else if (auto* expr = dynamic_cast<AST::ExpressionStatement*>(stmt)) {
            check_expression_statement(expr);
        }
       else if (auto* empty = dynamic_cast<AST::EmptyStatement*>(stmt)) {
       }
        else if (auto* destruct_stmt = dynamic_cast<AST::DestructStatement*>(stmt)) {
            AST::Type target_type = check_expression(destruct_stmt->target.get());
            if (target_type.kind != TypeKind::Pointer) {
                report_error(destruct_stmt->location, ErrorCode::FreeNonPointer,
                    "destruct requires a pointer, got '" + target_type.to_string() + "'");
            }
           else if (!is_null_literal_expr(destruct_stmt->target.get()) &&
               (!target_type.pointee_type ||
               target_type.pointee_type->kind != TypeKind::Struct)) {
                diag_.report_error_template(destruct_stmt->location,
                    ErrorCode::DestructNonConstructed, std::vector<std::string>{});
           }
        }
        else if (auto* struct_def = dynamic_cast<AST::StructDefinition*>(stmt)) {
            if (struct_defs_.find(struct_def->name) != struct_defs_.end()) {
                report_error(struct_def->location, ErrorCode::RedefinedIdentifier,
                    "struct '" + struct_def->name + "' already defined in this scope");
            }
            else {
                struct_defs_[struct_def->name] = struct_def;
                Symbol sym = Symbol::make_struct(struct_def->name, struct_def->location, struct_def);
                if (!sym_table_.declare(sym)) {
                    report_error(struct_def->location, ErrorCode::RedefinedIdentifier,
                        "struct '" + struct_def->name + "' already declared");
                }
                check_struct_definition(struct_def);
            }
        }
        else {
            report_error(stmt->location, ErrorCode::ExpressionSyntaxError,
                "unknown statement type");
        }
    }

    void TypeChecker::check_block(AST::Block* block) {
        enter_scope();
        for (auto& stmt : block->statements) {
            check_statement(stmt.get());
        }
        exit_scope();
    }

    AST::StructDefinition* TypeChecker::get_struct_definition(const std::string& name) const {
        auto it = struct_defs_.find(name);
        if (it != struct_defs_.end()) return it->second;
        auto predeclared = predeclared_structs_.find(name);
        return (predeclared != predeclared_structs_.end()) ? predeclared->second : nullptr;
    }

    const AST::StructDefinition::Member* TypeChecker::get_struct_member(const AST::Type& struct_type,
        std::string_view member_name) const {
        if (struct_type.kind != TypeKind::Struct) return nullptr;
        auto* def = get_struct_definition(struct_type.struct_name);
        if (!def) return nullptr;
        for (const auto& m : def->members) {
            if (m.name == member_name) {
                return &m;
            }
        }
        return nullptr;
    }

    Symbol* TypeChecker::lookup_symbol(std::string_view name, bool report_error) {
        return sym_table_.lookup(name);
    }

    const Symbol* TypeChecker::lookup_symbol(std::string_view name, bool report_error) const {
        return sym_table_.lookup(name);
    }

    void TypeChecker::report_error(SourceLocation loc, ErrorCode code, const std::string& msg) {
        diag_.report_error(loc, code, msg);
    }

    void TypeChecker::report_error(ErrorCode code, const std::string& msg) {
        SourceLocation loc = current_function_ ? current_function_->location : SourceLocation{};
        diag_.report_error(loc, code, msg);
    }

    void TypeChecker::report_warning(SourceLocation loc, ErrorCode code, const std::string& msg) {
        diag_.report_warning(loc, code, msg);
    }

    void TypeChecker::verify_main_function() {
        auto* sym = sym_table_.lookup("main");
        if (sym == nullptr) {
            report_error(SourceLocation{}, ErrorCode::UndefinedFunction,
                "main function not found");
            return;
        }
        if (sym->kind != SymbolKind::Function) {
            report_error(sym->declaration_loc, ErrorCode::MainSignatureError,
                "main must be a function");
            return;
        }
        if (sym->type.kind != TypeKind::Int) {
            report_error(sym->declaration_loc, ErrorCode::MainReturnTypeError,
                "main function must return int, got '" + sym->type.to_string() + "'");
        }
        bool valid = false;
        if (sym->param_types.empty()) {
            valid = true;
        }
        else if (sym->param_types.size() == 2) {
            if (sym->param_types[0].kind == TypeKind::Int) {
                if (sym->param_types[1].kind == TypeKind::Pointer) {
                    auto ptr_to = sym->param_types[1].pointee_type;
                    if (ptr_to && ptr_to->kind == TypeKind::Pointer) {
                        auto ptr_to_char = ptr_to->pointee_type;
                        if (ptr_to_char && ptr_to_char->kind == TypeKind::Char) {
                            valid = true;
                        }
                    }
                }
            }
            if (!valid) {
                report_error(sym->declaration_loc, ErrorCode::MainSignatureError,
                    "main parameter list must be () or (int count, char* array[])");
            }
        }
        else {
            report_error(sym->declaration_loc, ErrorCode::MainSignatureError,
                "main parameter list must be () or (int count, char* array[])");
        }
    }

    void TypeChecker::declare_builtin_functions() {
        if (builtins_declared_) return;
        builtins_declared_ = true;

        Symbol sym_input = Symbol::make_function("input", AST::Type::make_int(),
            {}, {}, SourceLocation{}, nullptr);
        sym_input.function_node = reinterpret_cast<AST::FunctionDefinition*>(1);
        sym_table_.declare(sym_input);

        Symbol sym_output = Symbol::make_function("output", AST::Type::make_void(),
            { AST::Type::make_string() }, {}, SourceLocation{}, nullptr);
        sym_output.function_node = reinterpret_cast<AST::FunctionDefinition*>(2);
        sym_table_.declare(sym_output);

        Symbol sym_size = Symbol::make_function("size", AST::Type::make_int(),
            { AST::Type::make_void() }, {}, SourceLocation{}, nullptr);
        sym_size.function_node = reinterpret_cast<AST::FunctionDefinition*>(3);
        sym_table_.declare(sym_size);

        Symbol sym_align = Symbol::make_function("align", AST::Type::make_int(),
            { AST::Type::make_void() }, {}, SourceLocation{}, nullptr);
        sym_align.function_node = reinterpret_cast<AST::FunctionDefinition*>(4);
        sym_table_.declare(sym_align);

        Symbol sym_free = Symbol::make_function("free", AST::Type::make_void(),
            { AST::Type::make_pointer(std::make_shared<AST::Type>(AST::Type::make_void())) },
            {}, SourceLocation{}, nullptr);
        sym_free.function_node = reinterpret_cast<AST::FunctionDefinition*>(5);
        sym_table_.declare(sym_free);

        AST::Type file_handle = AST::Type::make_pointer(
            std::make_shared<AST::Type>(AST::Type::make_file()));
        AST::Type buffer_ptr = AST::Type::make_pointer(
            std::make_shared<AST::Type>(AST::Type::make_void()));

        auto declare_file_builtin = [&](std::string_view name, AST::Type result,
                                        std::vector<AST::Type> params) {
            Symbol sym = Symbol::make_function(name, std::move(result),
                params, {}, SourceLocation{}, nullptr);
            sym.function_node = reinterpret_cast<AST::FunctionDefinition*>(100);
            sym_table_.declare(sym);
        };

        declare_file_builtin("fileopen", file_handle,
            { AST::Type::make_string(), AST::Type::make_string() });
        declare_file_builtin("fileclose", AST::Type::make_bool(), { file_handle });
        declare_file_builtin("fileflush", AST::Type::make_bool(), { file_handle });
        declare_file_builtin("fileread", AST::Type::make_int(),
            { file_handle, buffer_ptr, AST::Type::make_int() });
        declare_file_builtin("filewrite", AST::Type::make_int(),
            { file_handle, AST::Type::make_string() });
        declare_file_builtin("filewritebytes", AST::Type::make_int(),
            { file_handle, buffer_ptr, AST::Type::make_int() });
        declare_file_builtin("filegetc", AST::Type::make_int(), { file_handle });
        declare_file_builtin("fileputc", AST::Type::make_int(),
            { file_handle, AST::Type::make_int() });
        declare_file_builtin("filereadline", AST::Type::make_string(), { file_handle });
        declare_file_builtin("filewriteline", AST::Type::make_int(),
            { file_handle, AST::Type::make_string() });
        declare_file_builtin("fileseek", AST::Type::make_bool(),
            { file_handle, AST::Type::make_int(), AST::Type::make_int() });
        declare_file_builtin("filetell", AST::Type::make_int(), { file_handle });
        declare_file_builtin("fileeof", AST::Type::make_bool(), { file_handle });
        declare_file_builtin("fileerror", AST::Type::make_int(), { file_handle });
        declare_file_builtin("fileremove", AST::Type::make_bool(),
            { AST::Type::make_string() });
        declare_file_builtin("filerename", AST::Type::make_bool(),
            { AST::Type::make_string(), AST::Type::make_string() });
        declare_file_builtin("fileexists", AST::Type::make_bool(),
            { AST::Type::make_string() });
        declare_file_builtin("filesize", AST::Type::make_int(),
            { AST::Type::make_string() });
        declare_file_builtin("filecopy", AST::Type::make_bool(),
            { AST::Type::make_string(), AST::Type::make_string() });
        declare_file_builtin("filemkdir", AST::Type::make_bool(),
            { AST::Type::make_string() });
        declare_file_builtin("fileremovedir", AST::Type::make_bool(),
            { AST::Type::make_string() });

        auto declare_string_builtin = [&](std::string_view name, AST::Type result,
                                           std::vector<AST::Type> params) {
            Symbol sym = Symbol::make_function(name, std::move(result),
                params, {}, SourceLocation{}, nullptr);
            sym.function_node = reinterpret_cast<AST::FunctionDefinition*>(101);
            sym_table_.declare(sym);
        };

        AST::Type string_ref = AST::Type::make_pointer(
            std::make_shared<AST::Type>(AST::Type::make_string()));
        AST::Type str = AST::Type::make_string();
        AST::Type int_value = AST::Type::make_int();
        AST::Type char_value = AST::Type::make_char();

        declare_string_builtin("strlen", int_value, { str });
        declare_string_builtin("strconcat", str, { str, str });
        declare_string_builtin("strcopy", str, { str });
        declare_string_builtin("strmove", str, { string_ref });
        declare_string_builtin("strcompare", int_value, { str, str });
        declare_string_builtin("strcontains", AST::Type::make_bool(), { str, str });
        declare_string_builtin("strsubstr", str, { str, int_value, int_value });
        declare_string_builtin("strfind", int_value, { str, str });
        declare_string_builtin("strreplace", str, { str, str, str });
        declare_string_builtin("strupper", str, { str });
        declare_string_builtin("strlower", str, { str });
        declare_string_builtin("strtrim", str, { str });
        declare_string_builtin("strcharat", char_value, { str, int_value });
        declare_string_builtin("strsetchar", AST::Type::make_bool(),
            { string_ref, int_value, char_value });
        declare_string_builtin("strsplitcount", int_value, { str, str });
        declare_string_builtin("strsplitget", str, { str, str, int_value });
        declare_string_builtin("strread", str, {});
        declare_string_builtin("strwrite", AST::Type::make_void(), { str });
        declare_string_builtin("strwritefile", int_value, { file_handle, str });
    }

    void TypeChecker::enter_scope() {
        sym_table_.enter_scope();
        const_values_.emplace_back();
    }

    void TypeChecker::exit_scope() {
        sym_table_.exit_scope();
        if (!const_values_.empty()) {
            const_values_.pop_back();
        }
    }

}
