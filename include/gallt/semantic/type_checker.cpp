#include "../semantic/type_checker.hpp"
#include "../parser/ast_visitor.hpp"
#include "../semantic/diagnosed_registry.hpp"
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
        configure_constexpr_host();
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
                } else {
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
                declared_functions_.insert(func);
                collect_local_structs(func->body.get());
            } else if (auto* strct = dynamic_cast<AST::StructDefinition*>(top.get())) {
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

    class TypeChecker::NodeChecker : public AST::AstRewriter {
    public:
        explicit NodeChecker(TypeChecker& owner, bool top_level)
            : owner_(owner), top_level_(top_level) {
        }

    protected:
        bool EnterGuideStatement(AST::GuideStatement* node) override {
            owner_.check_guide_statement(node);
            return false;
        }

        bool EnterClibStatement(AST::ClibStatement* node) override {
            owner_.check_clib_statement(node);
            return false;
        }

        bool EnterExternDeclaration(AST::ExternDeclaration* node) override {
            owner_.check_extern_declaration(
                node);
            return false;
        }

        bool EnterFunctionDefinition(AST::FunctionDefinition* node) override {
            owner_.check_function_definition(
                node);
            return false;
        }

        bool EnterVariableDeclaration(AST::VariableDeclaration* node) override {
            owner_.check_variable_declaration(
                node);
            return false;
        }

        bool EnterStructDefinition(AST::StructDefinition* node) override {
            auto* def = node;

            if (top_level_) {
                owner_.check_struct_definition(def);
                return false;
            }

            if (owner_.struct_defs_.find(def->name) != owner_.struct_defs_.end()) {
                owner_.report_error(def->location, ErrorCode::RedefinedIdentifier,
                    "struct '" + def->name + "' already defined in this scope");
                return false;
            }

            owner_.struct_defs_[def->name] = def;
            Symbol sym = Symbol::make_struct(def->name, def->location, def);

            if (!owner_.sym_table_.declare(sym)) {
                owner_.report_error(def->location, ErrorCode::RedefinedIdentifier,
                    "struct '" + def->name + "' already declared");
            }

            owner_.check_struct_definition(def);
            return false;
        }

        bool EnterBlock(AST::Block*) override {
            owner_.enter_scope();
            return true;
        }

        void LeaveBlock(AST::Block*) override {
            owner_.exit_scope();
        }

        bool EnterIfStatement(AST::IfStatement* node) override {
            owner_.check_if_statement(node);
            return false;
        }

        bool EnterForStatement(AST::ForStatement* node) override {
            owner_.check_for_statement(node);
            return false;
        }

        bool EnterWhileStatement(AST::WhileStatement* node) override {
            owner_.check_while_statement(node);
            return false;
        }

        bool EnterBreakStatement(AST::BreakStatement* node) override {
            owner_.check_break_statement(node);
            return false;
        }

        bool EnterReturnStatement(AST::ReturnStatement* node) override {
            owner_.check_return_statement(node);
            return false;
        }

        bool EnterExpressionStatement(AST::ExpressionStatement* node) override {
            owner_.check_expression_statement(
                node);
            return false;
        }

        bool EnterEmptyStatement(AST::EmptyStatement*) override {
            return false;
        }

        bool EnterDestructStatement(AST::DestructStatement* node) override {
            auto* stmt = node;
            AST::Type target_type = owner_.check_expression(stmt->target.get());

            if (!target_type.is_error()) {
                if (target_type.kind != TypeKind::Pointer) {
                    owner_.report_error(stmt->location, ErrorCode::FreeNonPointer,
                        "destruct requires a pointer, got '" +
                        target_type.to_string() + "'");
                } else if (!is_null_literal_expr(stmt->target.get()) &&
                    (!target_type.pointee_type ||
                    target_type.pointee_type->kind != TypeKind::Struct)) {
                    owner_.report_error_template(stmt->location,
                        ErrorCode::DestructNonConstructed,
                        std::vector<std::string>{});
                }
            }

            return false;
        }

        bool EnterGenericDefinition(AST::GenericDefinition* node) override {
            return report_unknown_node(node);
        }

        bool EnterInstantiationStatement(AST::InstantiationStatement* node) override {
            return report_unknown_node(node);
        }

        bool EnterNamespaceDefinition(AST::NamespaceDefinition* node) override {
            return report_unknown_node(node);
        }

        bool EnterAccessNamespaceStatement(AST::AccessNamespaceStatement* node) override {
            return report_unknown_node(node);
        }

        bool EnterAdditionNamespaceStatement(AST::AdditionNamespaceStatement* node) override {
            return report_unknown_node(node);
        }

        bool EnterCondDefinition(AST::CondDefinition* node) override {
            return report_unknown_node(node);
        }

        bool EnterUncondDefinition(AST::UncondDefinition* node) override {
            return report_unknown_node(node);
        }

        bool EnterConditionalBlock(AST::ConditionalBlock* node) override {
            return report_unknown_node(node);
        }

        bool EnterTopLevelBlock(AST::TopLevelBlock* node) override {
            return report_unknown_node(node);
        }

        bool EnterEmitStatement(AST::EmitStatement* node) override {
            return report_unknown_node(node);
        }

    private:
        bool report_unknown_node(AST::Node* node) {
            owner_.report_error(node->location, ErrorCode::ExpressionSyntaxError,
                top_level_ ? "unknown top-level node" : "unknown statement type");
            return false;
        }

        TypeChecker& owner_;
        bool top_level_ = false;
    };

    void TypeChecker::check_top_level(AST::TopLevel* node) {
        if (node == nullptr) {
            return;
        }

        NodeChecker checker(*this, true);
        std::unique_ptr<AST::TopLevel> holder(node);
        checker.rewrite_top_level(holder);
        holder.release();
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
        if (stmt == nullptr) {
            return;
        }

        NodeChecker checker(*this, false);
        std::unique_ptr<AST::Statement> holder(stmt);
        checker.rewrite_statement(holder);
        holder.release();
    }

    void TypeChecker::check_block(AST::Block* block) {
        if (block == nullptr) {
            return;
        }

        NodeChecker checker(*this, false);
        std::unique_ptr<AST::Statement> holder(block);
        checker.rewrite_statement(holder);
        holder.release();
    }

    AST::StructDefinition* TypeChecker::get_struct_definition(const std::string& name) const {
        auto it = struct_defs_.find(name);
        if (it != struct_defs_.end()) { return it->second; }
        auto predeclared = predeclared_structs_.find(name);
        return (predeclared != predeclared_structs_.end()) ? predeclared->second : nullptr;
    }

    const AST::StructDefinition::Member* TypeChecker::get_struct_member(const AST::Type& struct_type,
        std::string_view member_name) const {
        if (struct_type.kind != TypeKind::Struct) { return nullptr; }
        auto* def = get_struct_definition(struct_type.struct_name);
        if (!def) { return nullptr; }

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
        if (!diagnosed_registry::reported_by_earlier_stage(diag_, loc,
            SemanticStage::TypeCheck)) {
            diag_.report_error(loc, code, msg);
        }

        diagnosed_registry::record(diag_, loc, SemanticStage::TypeCheck);
    }

    void TypeChecker::report_error(ErrorCode code, const std::string& msg) {
        SourceLocation loc = current_function_ ? current_function_->location : SourceLocation{};
        report_error(loc, code, msg);
    }

    void TypeChecker::report_error_template(SourceLocation loc, ErrorCode code,
        const std::vector<std::string>& values) {
        if (!diagnosed_registry::reported_by_earlier_stage(diag_, loc,
            SemanticStage::TypeCheck)) {
            diag_.report_error_template(loc, code, values);
        }

        diagnosed_registry::record(diag_, loc, SemanticStage::TypeCheck);
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
        } else if (sym->param_types.size() == 2) {
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
        } else {
            report_error(sym->declaration_loc, ErrorCode::MainSignatureError,
                "main parameter list must be () or (int count, char* array[])");
        }
    }

    void TypeChecker::declare_builtin_functions() {
        if (builtins_declared_) { return; }
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
        constexpr_values_.emplace_back();
    }

    void TypeChecker::exit_scope() {
        sym_table_.exit_scope();

        if (!const_values_.empty()) {
            const_values_.pop_back();
        }

        if (!constexpr_values_.empty()) {
            constexpr_values_.pop_back();
        }
    }

}
