// semantic/type_checker.cpp
// 类型检查器实现 —— 执行语义分析、类型推导、符号解析、错误报告
// Type Checker implementation — performs semantic analysis, type inference, symbol resolution, error reporting

#include "../semantic/type_checker.hpp"
#include "../parser/ast.hpp"
#include <algorithm>
#include <cctype>
#include <charconv>
#include <system_error>
#include <unordered_set>
#include <functional>

using namespace gallt::AST;

namespace gallt {

    namespace {
        // ========================================================================
        // 文件操作内置函数签名表（Gallt 0.2.txt §17）
        // File-operation builtin signature table (Gallt 0.2.txt §17)
        // 说明：fileopen 等是内置函数标识符，不是关键字，因此在此登记并单独检查
        // Note: fileopen and friends are builtin function identifiers, not keywords
        // ========================================================================

        enum class FileParamKind {
            FileHandle,   // file*
            Buffer,       // 指针或数组（缓冲区）
            IntValue,     // int（一般整数参数，如 fileputc 的字节、fileseek 的偏移）
            SizeValue,    // int（读写字节数，对应 ER 0058）
            StringValue,  // string 路径或模式（对应 ER 0059）
            TextValue,    // string 内容（写入的文本，类型不符时为 ER 0053）
            SeekOrigin    // 起始位置，仅允许 0、1、2
        };

        enum class FileResultKind {
            FileHandlePtr, // file*
            Bool,
            Int,
            String
        };

        struct FileBuiltinInfo {
            std::vector<FileParamKind> params;
            FileResultKind result;
        };

        const std::unordered_map<std::string, FileBuiltinInfo>& file_builtin_table() {
            static const std::unordered_map<std::string, FileBuiltinInfo> table = {
                { "fileopen",       { { FileParamKind::StringValue, FileParamKind::StringValue }, FileResultKind::FileHandlePtr } },
                { "fileclose",      { { FileParamKind::FileHandle }, FileResultKind::Bool } },
                { "fileflush",      { { FileParamKind::FileHandle }, FileResultKind::Bool } },
                { "fileread",       { { FileParamKind::FileHandle, FileParamKind::Buffer, FileParamKind::SizeValue }, FileResultKind::Int } },
                { "filewrite",      { { FileParamKind::FileHandle, FileParamKind::TextValue }, FileResultKind::Int } },
                { "filewritebytes", { { FileParamKind::FileHandle, FileParamKind::Buffer, FileParamKind::SizeValue }, FileResultKind::Int } },
                { "filegetc",       { { FileParamKind::FileHandle }, FileResultKind::Int } },
                { "fileputc",       { { FileParamKind::FileHandle, FileParamKind::IntValue }, FileResultKind::Int } },
                { "filereadline",   { { FileParamKind::FileHandle }, FileResultKind::String } },
                { "filewriteline",  { { FileParamKind::FileHandle, FileParamKind::TextValue }, FileResultKind::Int } },
                { "fileseek",       { { FileParamKind::FileHandle, FileParamKind::IntValue, FileParamKind::SeekOrigin }, FileResultKind::Bool } },
                { "filetell",       { { FileParamKind::FileHandle }, FileResultKind::Int } },
                { "fileeof",        { { FileParamKind::FileHandle }, FileResultKind::Bool } },
                { "fileerror",      { { FileParamKind::FileHandle }, FileResultKind::Int } },
                { "fileremove",     { { FileParamKind::StringValue }, FileResultKind::Bool } },
                { "filerename",     { { FileParamKind::StringValue, FileParamKind::StringValue }, FileResultKind::Bool } },
                { "fileexists",     { { FileParamKind::StringValue }, FileResultKind::Bool } },
                { "filesize",       { { FileParamKind::StringValue }, FileResultKind::Int } },
                { "filecopy",       { { FileParamKind::StringValue, FileParamKind::StringValue }, FileResultKind::Bool } },
                { "filemkdir",      { { FileParamKind::StringValue }, FileResultKind::Bool } },
                { "fileremovedir",  { { FileParamKind::StringValue }, FileResultKind::Bool } },
            };
            return table;
        }

        bool is_file_builtin_name(const std::string& name) {
            return file_builtin_table().find(name) != file_builtin_table().end();
        }

        // 合法的文件打开模式（Gallt 0.2.txt §17：统一使用小写）
        // Valid file-open modes (Gallt 0.2.txt §17: lowercase only)
        bool is_valid_file_open_mode(std::string_view mode) {
            static const char* kModes[] = {
                "r", "w", "a", "r+", "w+", "a+",
                "rb", "wb", "ab", "r+b", "w+b", "a+b",
            };
            for (const char* candidate : kModes) {
                if (mode == candidate) return true;
            }
            return false;
        }

        // 从字符串字面量词素中取出内容（不做转义解析；含反斜杠时交由运行时判断）
        // Extract the content of a string literal lexeme (escapes are left to the runtime)
        bool string_literal_content(std::string_view lexeme, std::string& out) {
            if (lexeme.size() < 2 || lexeme.front() != '"' || lexeme.back() != '"') {
                return false;
            }
            std::string_view inner = lexeme.substr(1, lexeme.size() - 2);
            if (inner.find('\\') != std::string_view::npos) {
                return false; // 含转义序列，静态判断不安全
            }
            out.assign(inner);
            return true;
        }

    } // anonymous namespace

    // ============================================================================
    // 构造函数
    // ============================================================================

    TypeChecker::TypeChecker(DiagnosticEngine& diag)
        : diag_(diag) {
        // 内置函数声明在 check_program 开始时调用，此处不重复
    }

    // ============================================================================
    // 主接口
    // ============================================================================

    bool TypeChecker::check_program(AST::Program* program) {
        if (program == nullptr) {
            return false;
        }
        program_ = program;

        // 进入全局作用域
        sym_table_.enter_scope();

        // 声明内置函数（第一次调用时会注册）
        declare_builtin_functions();

        // 第一遍：收集所有结构体定义和函数声明（仅声明，不检查体）
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

        // 第二遍：检查所有顶层节点
        for (auto& top : program->top_levels) {
            check_top_level(top.get());
        }

        // 验证主函数
        verify_main_function();

        // 退出全局作用域
        sym_table_.exit_scope();

        return !diag_.has_errors();
    }

    // ============================================================================
    // 顶层检查
    // ============================================================================

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

    void TypeChecker::check_extern_declaration(AST::ExternDeclaration* node) {
        if (!is_complete_type(node->return_type) && node->return_type.kind != TypeKind::Void) {
            report_error(node->location, ErrorCode::ExpressionSyntaxError,
                "invalid return type in extern declaration");
            return;
        }
        for (const auto& p : node->parameters) {
            if (p.kind == TypeKind::Void) {
                report_error(node->location, ErrorCode::VoidParameter,
                    "parameter cannot have void type in extern declaration");
            }
            if (!is_complete_type(p)) {
                report_error(node->location, ErrorCode::ExpressionSyntaxError,
                    "incomplete parameter type in extern declaration");
            }
        }
        if (auto* existing = sym_table_.lookup(node->name)) {
            if (existing->kind == SymbolKind::Function) {
                report_error(node->location, ErrorCode::RedefinedFunction,
                    "function '" + node->name + "' already defined");
            }
            else {
                report_error(node->location, ErrorCode::RedefinedIdentifier,
                    "identifier '" + node->name + "' already declared");
            }
            return;
        }
        Symbol sym = Symbol::make_function(
            node->name, node->return_type,
            node->parameters, node->param_names,
            node->location, nullptr
        );
        sym.function_node = nullptr;
        if (!sym_table_.declare(sym)) {
            report_error(node->location, ErrorCode::RedefinedFunction,
                "function '" + node->name + "' already declared");
        }
    }

    void TypeChecker::check_function_definition(AST::FunctionDefinition* node) {
        if (!is_complete_type(node->return_type) && node->return_type.kind != TypeKind::Void) {
            report_error(node->location, ErrorCode::ExpressionSyntaxError,
                "invalid return type in function definition");
            return;
        }
        for (size_t i = 0; i < node->parameters.size(); ++i) {
            const auto& p = node->parameters[i];
            if (p.kind == TypeKind::Void) {
                report_error(node->location, ErrorCode::VoidParameter,
                    "parameter cannot have void type");
            }
            if (!is_complete_type(p) && p.kind != TypeKind::Void) {
                report_error(node->location, ErrorCode::ExpressionSyntaxError,
                    "incomplete parameter type for parameter " + std::to_string(i));
            }
            if (i < node->param_names.size() && !node->param_names[i].empty()) {
                for (size_t j = 0; j < i; ++j) {
                    if (node->param_names[j] == node->param_names[i]) {
                        report_error(node->location, ErrorCode::RedefinedIdentifier,
                            "parameter '" + node->param_names[i] + "' already declared");
                        break;
                    }
                }
            }
        }
        if (auto* existing = sym_table_.lookup(node->name)) {
            if (existing->kind == SymbolKind::Function) {
                if (existing->function_node == nullptr) {
                    // extern 声明，现在定义
                    if (!(existing->type == node->return_type &&
                        existing->param_types.size() == node->parameters.size() &&
                        std::equal(existing->param_types.begin(), existing->param_types.end(),
                            node->parameters.begin()))) {
                        report_error(node->location, ErrorCode::FunctionReturnTypeMismatch,
                            "function '" + node->name + "' extern declaration signature mismatch");
                    }
                    existing->function_node = node;
                    if (existing->param_names.empty() && !node->param_names.empty()) {
                        existing->param_names = node->param_names;
                    }
                }
                else {
                    report_error(node->location, ErrorCode::RedefinedFunction,
                        "function '" + node->name + "' already defined");
                }
                return;
            }
            else {
                report_error(node->location, ErrorCode::RedefinedIdentifier,
                    "identifier '" + node->name + "' already declared as non-function");
                return;
            }
        }
        Symbol sym = Symbol::make_function(
            node->name, node->return_type,
            node->parameters, node->param_names,
            node->location, node
        );
        if (!sym_table_.declare(sym)) {
            report_error(node->location, ErrorCode::RedefinedFunction,
                "function '" + node->name + "' already declared");
            return;
        }
        sym_table_.enter_scope();
        for (size_t i = 0; i < node->parameters.size(); ++i) {
            std::string pname = (i < node->param_names.size() && !node->param_names[i].empty())
                ? node->param_names[i]
                : "_param" + std::to_string(i);
            Symbol param_sym = Symbol::make_parameter(
                pname, node->parameters[i],
                node->location, i
            );
            if (!sym_table_.declare(param_sym)) {
                report_error(node->location, ErrorCode::RedefinedIdentifier,
                    "parameter '" + pname + "' already declared");
            }
        }
        current_function_ = node;
        if (node->body) {
            check_statement(node->body.get());
        }
        if (node->return_type.kind != TypeKind::Void) {
            bool has_return = false;
            std::function<void(AST::Statement*)> check_return_presence = [&](AST::Statement* stmt) {
                if (auto* ret = dynamic_cast<AST::ReturnStatement*>(stmt)) {
                    has_return = true;
                }
                else if (auto* block = dynamic_cast<AST::Block*>(stmt)) {
                    for (auto& s : block->statements) {
                        check_return_presence(s.get());
                    }
                }
                else if (auto* ifs = dynamic_cast<AST::IfStatement*>(stmt)) {
                    check_return_presence(ifs->then_block.get());
                    if (ifs->else_block) {
                        check_return_presence(ifs->else_block.get());
                    }
                }
                else if (auto* for_ = dynamic_cast<AST::ForStatement*>(stmt)) {
                    check_return_presence(for_->body.get());
                }
                else if (auto* while_ = dynamic_cast<AST::WhileStatement*>(stmt)) {
                    check_return_presence(while_->body.get());
                }
                };
            check_return_presence(node->body.get());
            if (!has_return) {
                report_error(node->location, ErrorCode::MissingReturnStatement,
                    "non-void function '" + node->name + "' must return a value");
            }
        }
        sym_table_.exit_scope();
        current_function_ = nullptr;
    }

    void TypeChecker::check_struct_definition(AST::StructDefinition* node) {
        for (size_t i = 0; i < node->members.size(); ++i) {
            for (size_t j = i + 1; j < node->members.size(); ++j) {
                if (node->members[i].name == node->members[j].name) {
                    report_error(node->members[j].location, ErrorCode::RedefinedIdentifier,
                        "struct member '" + node->members[j].name + "' already defined");
                }
            }
        }
        for (auto& member : node->members) {
            // 将声明符上的数组长度同步到类型本身
            // Sync the declarator array length into the type itself
            if (member.array_size.has_value() && !member.type.array_size.has_value() &&
                member.type.kind == TypeKind::Array) {
                member.type.array_size = member.array_size;
            }
            AST::Type& mem_type = member.type;
            if (mem_type.kind == TypeKind::Struct && mem_type.struct_name == node->name) {
                report_error(member.location, ErrorCode::StructSelfRefNonPtr,
                    "struct cannot directly contain itself; use pointer");
            }
            if (mem_type.kind == TypeKind::Pointer && mem_type.pointee_type) {
                if (mem_type.pointee_type->kind == TypeKind::Struct &&
                    mem_type.pointee_type->struct_name == node->name) {
                    // 允许自引用指针
                }
            }
            if (!is_complete_type(mem_type) && mem_type.kind != TypeKind::Void) {
                if (!(mem_type.kind == TypeKind::Pointer && mem_type.pointee_type &&
                    mem_type.pointee_type->kind == TypeKind::Struct &&
                    struct_defs_.find(mem_type.pointee_type->struct_name) == struct_defs_.end())) {
                    report_error(member.location, ErrorCode::ExpressionSyntaxError,
                        "incomplete type for struct member '" + member.name + "'");
                }
            }
            if (mem_type.kind == TypeKind::Function) {
                if (!is_complete_type(*mem_type.return_type) && mem_type.return_type->kind != TypeKind::Void) {
                    report_error(member.location, ErrorCode::ExpressionSyntaxError,
                        "invalid return type in function pointer member '" + member.name + "'");
                }
                for (const auto& p : mem_type.parameter_types) {
                    if (p.kind == TypeKind::Void) {
                        report_error(member.location, ErrorCode::VoidParameter,
                            "parameter cannot have void type in function pointer member");
                    }
                }
            }
            if (member.array_size.has_value() && member.array_size.value() == 0) {
                report_error(member.location, ErrorCode::ExpressionSyntaxError,
                    "array size must be positive");
            }
            if (member.initializer) {
                if (auto* expr_init = dynamic_cast<AST::ExpressionInitializer*>(member.initializer.get())) {
                    AST::Type init_type = check_expression(expr_init->expr.get());
                    if (!can_implicit_convert(init_type, mem_type)) {
                        report_error(member.location, ErrorCode::StructMemberTypeMismatch,
                            "initializer type '" + init_type.to_string() +
                            "' cannot be converted to member type '" + mem_type.to_string() + "'");
                    }
                }
                else if (auto* arr_init = dynamic_cast<AST::ArrayInitializer*>(member.initializer.get())) {
                    if (mem_type.kind != TypeKind::Array) {
                        report_error(member.location, ErrorCode::StructMemberTypeMismatch,
                            "array initializer for non-array member");
                    }
                    else {
                        size_t elem_count = arr_init->elements.size();
                        if (!mem_type.array_size.has_value()) {
                            if (elem_count == 0) {
                                report_error(member.location, ErrorCode::EmptyArrayInitializer,
                                    "cannot infer array size from empty initializer");
                            }
                            else {
                                mem_type.array_size = elem_count;
                                member.array_size = elem_count;
                            }
                        }
                        if (mem_type.array_size.has_value() && mem_type.array_size.value() != elem_count) {
                            report_error(member.location, ErrorCode::ArrayLengthMismatch,
                                "array initializer size " + std::to_string(elem_count) +
                                " does not match declared size " + std::to_string(mem_type.array_size.value()));
                        }
                        auto elem_type = mem_type.element_type;
                        for (auto& elem : arr_init->elements) {
                            if (auto* e = dynamic_cast<AST::ExpressionInitializer*>(elem.get())) {
                                AST::Type etype = check_expression(e->expr.get());
                                if (!can_implicit_convert(etype, *elem_type)) {
                                    report_error(member.location, ErrorCode::StructMemberTypeMismatch,
                                        "array element type mismatch");
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    // ============================================================================
    // 语句检查
    // ============================================================================

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
            // 空语句
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
        sym_table_.enter_scope();
        for (auto& stmt : block->statements) {
            check_statement(stmt.get());
        }
        sym_table_.exit_scope();
    }

    void TypeChecker::check_variable_declaration(AST::VariableDeclaration* decl) {
        // 将声明符上的数组长度同步到类型对象，避免重复维护两处状态
        // Sync the declarator array length into the type object
        if (decl->type.kind == TypeKind::Array) {
            if (decl->array_size.has_value() && !decl->type.array_size.has_value()) {
                decl->type.array_size = decl->array_size;
            }
            if (!decl->array_size.has_value() && decl->type.array_size.has_value()) {
                decl->array_size = decl->type.array_size;
            }
        }
        if (decl->type.kind == TypeKind::Array && !decl->array_size.has_value()) {
            if (auto* arr_init = dynamic_cast<AST::ArrayInitializer*>(decl->initializer.get())) {
                if (!arr_init->elements.empty()) {
                    decl->type.array_size = arr_init->elements.size();
                    decl->array_size = arr_init->elements.size();
                }
            }
        }
        if (!is_complete_type(decl->type) && decl->type.kind != TypeKind::Void) {
            report_error(decl->location, ErrorCode::ExpressionSyntaxError,
                "incomplete type in variable declaration");
            return;
        }
        if (decl->type.kind == TypeKind::Void) {
            report_error(decl->location, ErrorCode::ExpressionSyntaxError,
                "variable cannot have void type");
            return;
        }
        if (auto* existing = sym_table_.lookup_current(decl->name)) {
            report_error(decl->location, ErrorCode::RedefinedIdentifier,
                "variable '" + decl->name + "' already declared in this scope");
            return;
        }
        if (decl->type.kind == TypeKind::Array) {
            if (!decl->array_size.has_value()) {
                if (decl->initializer) {
                    if (auto* arr_init = dynamic_cast<AST::ArrayInitializer*>(decl->initializer.get())) {
                        size_t size = arr_init->elements.size();
                        if (size == 0) {
                            report_error(decl->location, ErrorCode::EmptyArrayInitializer,
                                "cannot infer array size from empty initializer");
                            return;
                        }
                        decl->type.array_size = size;
                        decl->array_size = size;
                    }
                    else if (auto* expr_init = dynamic_cast<AST::ExpressionInitializer*>(decl->initializer.get())) {
                        // 字符串字面量初始化 char[] 暂不支持
                        report_error(decl->location, ErrorCode::ExpressionSyntaxError,
                            "array initializer must be a braced list");
                        return;
                    }
                    else {
                        report_error(decl->location, ErrorCode::ExpressionSyntaxError,
                            "array size must be specified or inferred from initializer");
                        return;
                    }
                }
                else {
                    report_error(decl->location, ErrorCode::ArraySizeNotConstant,
                        "array size must be specified");
                    return;
                }
            }
            else if (decl->array_size.value() == 0) {
                report_error(decl->location, ErrorCode::ExpressionSyntaxError,
                    "array size must be positive");
                return;
            }
        }
        if (decl->initializer) {
            if (auto* expr_init = dynamic_cast<AST::ExpressionInitializer*>(decl->initializer.get())) {
                AST::Type init_type = check_expression(expr_init->expr.get());
                bool is_null = false;
                if (auto* primary = dynamic_cast<AST::PrimaryExpression*>(expr_init->expr.get())) {
                    if (primary->kind == AST::PrimaryExpression::Kind::Null) {
                        is_null = true;
                    }
                }
                if (!is_null || decl->type.kind != TypeKind::Pointer) {
                    if (!can_implicit_convert(init_type, decl->type)) {
                        report_error(decl->location, ErrorCode::AssignmentTypeMismatch,
                            "cannot initialize variable '" + decl->name +
                            "' with type '" + init_type.to_string() +
                            "' (expected '" + decl->type.to_string() + "')");
                    }
                }
            }
            else if (auto* arr_init = dynamic_cast<AST::ArrayInitializer*>(decl->initializer.get())) {
                if (decl->type.kind == TypeKind::Struct) {
                    // 结构体使用花括号初始化，顺序与成员声明顺序一致
                    // Structs are brace-initialized in member declaration order
                    AST::StructDefinition* struct_def = get_struct_definition(decl->type.struct_name);
                    if (struct_def == nullptr) {
                        report_error(decl->location, ErrorCode::ExpressionSyntaxError,
                            "unknown struct type '" + decl->type.struct_name + "'");
                    }
                    else if (arr_init->elements.size() > struct_def->members.size()) {
                        report_error(decl->location, ErrorCode::StructInitLengthMismatch,
                            "struct initializer has too many elements; expected at most " +
                            std::to_string(struct_def->members.size()) + ", got " +
                            std::to_string(arr_init->elements.size()));
                    }
                    else {
                        for (size_t i = 0; i < arr_init->elements.size(); ++i) {
                            const auto& member = struct_def->members[i];
                            AST::Initializer* element = arr_init->elements[i].get();
                            if (member.type.kind == TypeKind::Struct) {
                                if (auto* nested = dynamic_cast<AST::ArrayInitializer*>(element)) {
                                    AST::StructDefinition* nested_def =
                                        get_struct_definition(member.type.struct_name);
                                    if (nested_def && nested->elements.size() > nested_def->members.size()) {
                                        report_error(decl->location, ErrorCode::StructInitLengthMismatch,
                                            "nested struct initializer has too many elements");
                                    }
                                    for (size_t j = 0; j < nested->elements.size(); ++j) {
                                        if (auto* e = dynamic_cast<AST::ExpressionInitializer*>(nested->elements[j].get())) {
                                            AST::Type elem_type = check_expression(e->expr.get());
                                            if (!can_implicit_convert(elem_type, nested_def->members[j].type)) {
                                                report_error(e->location, ErrorCode::StructMemberTypeMismatch,
                                                    "initializer for member '" + nested_def->members[j].name +
                                                    "' does not match its type");
                                            }
                                        }
                                    }
                                }
                                else if (auto* e = dynamic_cast<AST::ExpressionInitializer*>(element)) {
                                    AST::Type init_type = check_expression(e->expr.get());
                                    if (!can_implicit_convert(init_type, member.type)) {
                                        report_error(e->location, ErrorCode::StructMemberTypeMismatch,
                                            "initializer for member '" + member.name +
                                            "' does not match its type");
                                    }
                                }
                            }
                            else if (auto* e = dynamic_cast<AST::ExpressionInitializer*>(element)) {
                                AST::Type init_type = check_expression(e->expr.get());
                                if (!can_implicit_convert(init_type, member.type)) {
                                    report_error(e->location, ErrorCode::StructMemberTypeMismatch,
                                        "initializer for member '" + member.name +
                                        "' does not match its type");
                                }
                            }
                            else {
                                report_error(decl->location, ErrorCode::ExpressionSyntaxError,
                                    "struct member requires an expression initializer");
                            }
                        }
                    }
                }
                else if (decl->type.kind != TypeKind::Array) {
                    report_error(decl->location, ErrorCode::AssignmentTypeMismatch,
                        "array initializer for non-array variable");
                }
                else {
                    size_t provided = arr_init->elements.size();
                    if (decl->array_size.has_value()) {
                        if (provided != decl->array_size.value()) {
                            report_error(decl->location, ErrorCode::ArrayLengthMismatch,
                                "initializer length " + std::to_string(provided) +
                                " does not match array size " + std::to_string(decl->array_size.value()));
                        }
                    }
                    auto elem_type = decl->type.element_type;
                    for (auto& elem : arr_init->elements) {
                        if (auto* e = dynamic_cast<AST::ExpressionInitializer*>(elem.get())) {
                            AST::Type etype = check_expression(e->expr.get());
                            if (!can_implicit_convert(etype, *elem_type)) {
                                report_error(decl->location, ErrorCode::AssignmentTypeMismatch,
                                    "array element type mismatch");
                            }
                        }
                    }
                }
            }
        }
        Symbol sym = Symbol::make_variable(decl->name, decl->type, decl->location,
            decl->initializer != nullptr);
        if (!sym_table_.declare(sym)) {
            report_error(decl->location, ErrorCode::RedefinedIdentifier,
                "variable '" + decl->name + "' already declared");
        }
    }

    void TypeChecker::check_if_statement(AST::IfStatement* if_stmt) {
        if (if_stmt->condition) {
            AST::Type cond_type = check_expression(if_stmt->condition.get());
            if (!is_bool_type(cond_type) && !cond_type.is_integer()) {
                if (!can_implicit_convert(cond_type, AST::Type::make_bool())) {
                    report_error(if_stmt->condition->location, ErrorCode::ConditionNotBoolean,
                        "if condition must be boolean or integer type, got '" + cond_type.to_string() + "'");
                }
            }
        }
        if (if_stmt->then_block) {
            check_statement(if_stmt->then_block.get());
        }
        if (if_stmt->else_block) {
            check_statement(if_stmt->else_block.get());
        }
    }

    void TypeChecker::check_for_statement(AST::ForStatement* for_stmt) {
        sym_table_.enter_scope();
        if (for_stmt->init) {
            check_statement(for_stmt->init.get());
        }
        if (for_stmt->condition) {
            AST::Type cond_type = check_expression(for_stmt->condition.get());
            if (!is_bool_type(cond_type) && !cond_type.is_integer()) {
                if (!can_implicit_convert(cond_type, AST::Type::make_bool())) {
                    report_error(for_stmt->condition->location, ErrorCode::ConditionNotBoolean,
                        "for condition must be boolean or integer type, got '" + cond_type.to_string() + "'");
                }
            }
        }
        if (for_stmt->step) {
            check_expression(for_stmt->step.get());
        }
        loop_depth_++;
        if (for_stmt->body) {
            check_statement(for_stmt->body.get());
        }
        loop_depth_--;
        sym_table_.exit_scope();
    }

    void TypeChecker::check_while_statement(AST::WhileStatement* while_stmt) {
        if (while_stmt->condition) {
            AST::Type cond_type = check_expression(while_stmt->condition.get());
            if (!is_bool_type(cond_type) && !cond_type.is_integer()) {
                if (!can_implicit_convert(cond_type, AST::Type::make_bool())) {
                    report_error(while_stmt->condition->location, ErrorCode::ConditionNotBoolean,
                        "while condition must be boolean or integer type, got '" + cond_type.to_string() + "'");
                }
            }
        }
        loop_depth_++;
        if (while_stmt->body) {
            check_statement(while_stmt->body.get());
        }
        loop_depth_--;
    }

    void TypeChecker::check_break_statement(AST::BreakStatement* break_stmt) {
        if (loop_depth_ == 0) {
            report_error(break_stmt->location, ErrorCode::BreakOutsideLoop,
                "break statement must be inside a loop");
        }
    }

    void TypeChecker::check_return_statement(AST::ReturnStatement* return_stmt) {
        if (current_function_ == nullptr) {
            report_error(return_stmt->location, ErrorCode::ReturnOutsideFunction,
                "return statement outside function");
            return;
        }
        AST::Type expected = current_function_->return_type;
        if (return_stmt->value) {
            AST::Type actual = check_expression(return_stmt->value.get());
            if (expected.kind == TypeKind::Void) {
                report_error(return_stmt->location, ErrorCode::VoidFunctionReturnsValue,
                    "void function cannot return a value");
            }
            else {
                if (!can_implicit_convert(actual, expected)) {
                    report_error(return_stmt->location, ErrorCode::FunctionReturnTypeMismatch,
                        "return type '" + actual.to_string() +
                        "' does not match function return type '" + expected.to_string() + "'");
                }
            }
        }
        else {
            if (expected.kind != TypeKind::Void) {
                report_error(return_stmt->location, ErrorCode::FunctionReturnTypeMismatch,
                    "non-void function must return a value");
            }
        }
    }

    void TypeChecker::check_expression_statement(AST::ExpressionStatement* expr_stmt) {
        if (expr_stmt->expr) {
            check_expression(expr_stmt->expr.get());
        }
    }

    // ============================================================================
    // 表达式检查
    // ============================================================================

    AST::Type TypeChecker::check_expression(AST::Expression* expr, bool allow_void) {
        if (expr == nullptr) {
            return AST::Type::make_void();
        }
        AST::Type result;
        if (auto* assign = dynamic_cast<AST::AssignmentExpression*>(expr)) {
            result = check_assignment(assign);
        }
        else if (auto* log_or = dynamic_cast<AST::LogicalOrExpression*>(expr)) {
            result = check_logical_or(log_or);
        }
        else if (auto* log_and = dynamic_cast<AST::LogicalAndExpression*>(expr)) {
            result = check_logical_and(log_and);
        }
        else if (auto* comp = dynamic_cast<AST::ComparisonExpression*>(expr)) {
            result = check_comparison(comp);
        }
        else if (auto* add = dynamic_cast<AST::AdditiveExpression*>(expr)) {
            result = check_additive(add);
        }
        else if (auto* mul = dynamic_cast<AST::MultiplicativeExpression*>(expr)) {
            result = check_multiplicative(mul);
        }
        else if (auto* pow = dynamic_cast<AST::PowerExpression*>(expr)) {
            result = check_power(pow);
        }
        else if (auto* unary = dynamic_cast<AST::UnaryExpression*>(expr)) {
            result = check_unary(unary);
        }
        else if (auto* post = dynamic_cast<AST::PostfixExpression*>(expr)) {
            result = check_postfix(post);
        }
        else if (auto* prim = dynamic_cast<AST::PrimaryExpression*>(expr)) {
            result = check_primary(prim);
        }
        else {
            report_error(expr->location, ErrorCode::ExpressionSyntaxError,
                "unknown expression type");
            result = AST::Type::make_void();
        }
        // 记录结果，供代码生成器在无需重复推导的情况下取得类型
        // Record the result so codegen can retrieve types without re-deriving them
        expression_types_[expr] = result;
        (void)allow_void;
        return result;
    }

    AST::Type TypeChecker::check_assignment(AST::AssignmentExpression* expr) {
        if (!expr->left->is_lvalue()) {
            report_error(expr->left->location, ErrorCode::ExpressionSyntaxError,
                "left-hand side of assignment must be an lvalue");
        }
        AST::Type left_type = check_expression(expr->left.get());
        AST::Type right_type = check_expression(expr->right.get());
        bool ok = false;
        if (expr->op == AST::AssignmentExpression::Operator::Assign) {
            ok = can_implicit_convert(right_type, left_type);
        }
        else if (expr->op == AST::AssignmentExpression::Operator::PlusAssign ||
            expr->op == AST::AssignmentExpression::Operator::MinusAssign) {
            if (is_numeric_type(left_type) && is_numeric_type(right_type)) {
                ok = can_implicit_convert(right_type, left_type);
            }
            else {
                if (left_type.kind == TypeKind::Pointer && right_type.is_integer()) {
                    ok = true;
                }
                else {
                    report_error(expr->location, ErrorCode::BinaryOperatorTypeMismatch,
                        "operator '" + std::string(expr->op == AST::AssignmentExpression::Operator::PlusAssign ? "+=" : "-=") +
                        "' requires arithmetic types or pointer and integer");
                    ok = false;
                }
            }
        }
        if (!ok) {
            report_error(expr->location, ErrorCode::AssignmentTypeMismatch,
                "cannot assign type '" + right_type.to_string() +
                "' to type '" + left_type.to_string() + "'");
        }
        return left_type;
    }

    AST::Type TypeChecker::check_logical_or(AST::LogicalOrExpression* expr) {
        AST::Type left = check_expression(expr->left.get());
        AST::Type right = check_expression(expr->right.get());
        if (!is_bool_type(left) && !left.is_integer()) {
            report_error(expr->left->location, ErrorCode::BinaryOperatorTypeMismatch,
                "left operand of '||' must be boolean or integer, got '" + left.to_string() + "'");
        }
        if (!is_bool_type(right) && !right.is_integer()) {
            report_error(expr->right->location, ErrorCode::BinaryOperatorTypeMismatch,
                "right operand of '||' must be boolean or integer, got '" + right.to_string() + "'");
        }
        return AST::Type::make_bool();
    }

    AST::Type TypeChecker::check_logical_and(AST::LogicalAndExpression* expr) {
        AST::Type left = check_expression(expr->left.get());
        AST::Type right = check_expression(expr->right.get());
        if (!is_bool_type(left) && !left.is_integer()) {
            report_error(expr->left->location, ErrorCode::BinaryOperatorTypeMismatch,
                "left operand of '&&' must be boolean or integer, got '" + left.to_string() + "'");
        }
        if (!is_bool_type(right) && !right.is_integer()) {
            report_error(expr->right->location, ErrorCode::BinaryOperatorTypeMismatch,
                "right operand of '&&' must be boolean or integer, got '" + right.to_string() + "'");
        }
        return AST::Type::make_bool();
    }

    AST::Type TypeChecker::check_comparison(AST::ComparisonExpression* expr) {
        AST::Type left = check_expression(expr->left.get());
        AST::Type right = check_expression(expr->right.get());
        bool ok = false;
        if (is_numeric_type(left) && is_numeric_type(right)) {
            ok = true;
        }
        else if (left.kind == TypeKind::Pointer && right.kind == TypeKind::Pointer) {
            if (left.pointee_type && right.pointee_type) {
                if (left.pointee_type->kind == TypeKind::Void || right.pointee_type->kind == TypeKind::Void) {
                    ok = true;
                }
                else if (*left.pointee_type == *right.pointee_type) {
                    ok = true;
                }
            }
        }
        else if (left.kind == TypeKind::Pointer && right.is_integer()) {
            ok = true;
        }
        else if (left.is_integer() && right.kind == TypeKind::Pointer) {
            ok = true;
        }
        if (!ok) {
            report_error(expr->location, ErrorCode::BinaryOperatorTypeMismatch,
                "comparison operands must be arithmetic types or compatible pointers, got '" +
                left.to_string() + "' and '" + right.to_string() + "'");
        }
        return AST::Type::make_bool();
    }

    AST::Type TypeChecker::check_additive(AST::AdditiveExpression* expr) {
        AST::Type left = check_expression(expr->left.get());
        AST::Type right = check_expression(expr->right.get());
        if (expr->op == AST::AdditiveExpression::Operator::Plus) {
            // 文档示例使用 string + 数值进行拼接；字符串连接返回 string
            // String concatenation with values is shown in §16 and returns string
            if (left.kind == TypeKind::String || right.kind == TypeKind::String) {
                bool left_ok = left.kind == TypeKind::String || is_numeric_type(left);
                bool right_ok = right.kind == TypeKind::String || is_numeric_type(right);
                if (!left_ok || !right_ok) {
                    report_error(expr->location, ErrorCode::BinaryOperatorTypeMismatch,
                        "operator '+' string concatenation requires string or arithmetic operands, got '" +
                        left.to_string() + "' and '" + right.to_string() + "'");
                    return AST::Type::make_void();
                }
                return AST::Type::make_string();
            }
            if (left.kind == TypeKind::Pointer && right.is_integer()) {
                return left;
            }
            else if (left.is_integer() && right.kind == TypeKind::Pointer) {
                return right;
            }
            else if (is_numeric_type(left) && is_numeric_type(right)) {
                return usual_arithmetic_conversion(left, right);
            }
            else {
                report_error(expr->location, ErrorCode::BinaryOperatorTypeMismatch,
                    "operator '+' requires arithmetic types or pointer and integer, got '" +
                    left.to_string() + "' and '" + right.to_string() + "'");
                return AST::Type::make_void();
            }
        }
        else {
            if (left.kind == TypeKind::Pointer && right.is_integer()) {
                return left;
            }
            else if (left.kind == TypeKind::Pointer && right.kind == TypeKind::Pointer) {
                if (left.pointee_type && right.pointee_type &&
                    (*left.pointee_type == *right.pointee_type ||
                        left.pointee_type->kind == TypeKind::Void ||
                        right.pointee_type->kind == TypeKind::Void)) {
                    return AST::Type::make_int();
                }
                else {
                    report_error(expr->location, ErrorCode::BinaryOperatorTypeMismatch,
                        "pointer subtraction requires compatible pointer types");
                    return AST::Type::make_void();
                }
            }
            else if (is_numeric_type(left) && is_numeric_type(right)) {
                return usual_arithmetic_conversion(left, right);
            }
            else {
                report_error(expr->location, ErrorCode::BinaryOperatorTypeMismatch,
                    "operator '-' requires arithmetic types or pointer and integer, got '" +
                    left.to_string() + "' and '" + right.to_string() + "'");
                return AST::Type::make_void();
            }
        }
    }

    AST::Type TypeChecker::check_multiplicative(AST::MultiplicativeExpression* expr) {
        AST::Type left = check_expression(expr->left.get());
        AST::Type right = check_expression(expr->right.get());
        if (expr->op == AST::MultiplicativeExpression::Operator::Remainder) {
            // Gallt 0.2.txt §3：% 与 *、/ 同级；文档未明确的操作数规则按 C 处理（仅整数）
            // Gallt 0.2.txt §3: '%' shares the precedence of '*' and '/'; unspecified
            // operand rules follow C (integers only)
            if (left.is_integer() && right.is_integer()) {
                return usual_arithmetic_conversion(left, right);
            }
            report_error(expr->location, ErrorCode::BinaryOperatorTypeMismatch,
                "operator '%' requires integer types, got '" +
                left.to_string() + "' and '" + right.to_string() + "'");
            return AST::Type::make_void();
        }
        if (is_numeric_type(left) && is_numeric_type(right)) {
            return usual_arithmetic_conversion(left, right);
        }
        else {
            report_error(expr->location, ErrorCode::BinaryOperatorTypeMismatch,
                "operator '" + std::string(expr->op == AST::MultiplicativeExpression::Operator::Multiply ? "*" : "/") +
                "' requires arithmetic types, got '" +
                left.to_string() + "' and '" + right.to_string() + "'");
            return AST::Type::make_void();
        }
    }

    AST::Type TypeChecker::check_power(AST::PowerExpression* expr) {
        AST::Type left = check_expression(expr->left.get());
        AST::Type right = check_expression(expr->right.get());
        if (is_numeric_type(left) && is_numeric_type(right)) {
            return left;
        }
        else {
            report_error(expr->location, ErrorCode::BinaryOperatorTypeMismatch,
                "operator '**' requires arithmetic types, got '" +
                left.to_string() + "' and '" + right.to_string() + "'");
            return AST::Type::make_void();
        }
    }

    AST::Type TypeChecker::check_unary(AST::UnaryExpression* expr) {
        AST::Type operand = check_expression(expr->operand.get());
        switch (expr->op) {
        case AST::UnaryExpression::Operator::Increment:
        case AST::UnaryExpression::Operator::Decrement:
            if (!expr->operand->is_lvalue()) {
                report_error(expr->operand->location, ErrorCode::ExpressionSyntaxError,
                    "operand of increment/decrement must be an lvalue");
            }
            if (!is_numeric_type(operand) && operand.kind != TypeKind::Pointer) {
                report_error(expr->location, ErrorCode::UnaryOperatorTypeMismatch,
                    "increment/decrement requires arithmetic or pointer type, got '" +
                    operand.to_string() + "'");
            }
            return operand;
        case AST::UnaryExpression::Operator::LogicalNot:
            if (!is_bool_type(operand) && !operand.is_integer()) {
                report_error(expr->location, ErrorCode::UnaryOperatorTypeMismatch,
                    "operator '!' requires boolean or integer type, got '" +
                    operand.to_string() + "'");
            }
            return AST::Type::make_bool();
        case AST::UnaryExpression::Operator::UnaryPlus:
        case AST::UnaryExpression::Operator::UnaryMinus: {
            // 一元正负号：要求算术类型；char/bool 按 C 的整数提升规则提升为 int
            // Unary signs require an arithmetic operand; char/bool promote to int (C rules)
            const char* op_text =
                (expr->op == AST::UnaryExpression::Operator::UnaryMinus) ? "-" : "+";
            if (!is_numeric_type(operand)) {
                report_error(expr->location, ErrorCode::UnaryOperatorTypeMismatch,
                    std::string("unary operator '") + op_text +
                    "' requires arithmetic type, got '" + operand.to_string() + "'");
                return AST::Type::make_void();
            }
            if (operand.is_integer()) {
                return AST::Type::make_int();
            }
            return operand;
        }
        case AST::UnaryExpression::Operator::AddressOf:
            // 函数名不是存储左值，但 &function 可取得函数指针
            // A function name is not a storage lvalue, but &function yields a function pointer
            if (operand.kind == TypeKind::Function) {
                return operand;
            }
            if (!expr->operand->is_lvalue()) {
                report_error(expr->operand->location, ErrorCode::AddressOfNonLValue,
                    "address-of operator requires an lvalue");
            }
            return AST::Type::make_pointer(std::make_shared<AST::Type>(operand));
        case AST::UnaryExpression::Operator::Dereference:
            if (operand.kind != TypeKind::Pointer) {
                report_error(expr->location, ErrorCode::DerefNonPointer,
                    "dereference operator requires pointer type, got '" +
                    operand.to_string() + "'");
                return AST::Type::make_void();
            }
            if (operand.pointee_type) {
                return *operand.pointee_type;
            }
            else {
                report_error(expr->location, ErrorCode::ExpressionSyntaxError,
                    "pointer has no pointee type");
                return AST::Type::make_void();
            }
        default:
            report_error(expr->location, ErrorCode::ExpressionSyntaxError,
                "unknown unary operator");
            return AST::Type::make_void();
        }
    }

    AST::Type TypeChecker::check_postfix(AST::PostfixExpression* expr) {
        AST::Type base_type = check_expression(expr->base.get());

        switch (expr->op) {
        case AST::PostfixExpression::Operator::Subscript: {
            if (base_type.kind != TypeKind::Array && base_type.kind != TypeKind::Pointer) {
                report_error(expr->location, ErrorCode::ExpressionSyntaxError,
                    "subscript operator requires array or pointer type, got '" +
                    base_type.to_string() + "'");
                return AST::Type::make_void();
            }
            if (expr->subscript_expr) {
                AST::Type index_type = check_expression(expr->subscript_expr.get());
                if (!index_type.is_integer()) {
                    report_error(expr->subscript_expr->location, ErrorCode::SubscriptNotInteger,
                        "array index must be integer type, got '" + index_type.to_string() + "'");
                }
            }
            if (base_type.kind == TypeKind::Array) {
                if (base_type.element_type) {
                    return *base_type.element_type;
                }
            }
            else if (base_type.kind == TypeKind::Pointer) {
                if (base_type.pointee_type) {
                    return *base_type.pointee_type;
                }
            }
            return AST::Type::make_void();
        }
        case AST::PostfixExpression::Operator::FunctionCall: {
            // 获取函数名（如果基表达式是标识符）
            std::string func_name;
            AST::PrimaryExpression* direct_primary = nullptr;
            if (auto* prim = dynamic_cast<AST::PrimaryExpression*>(expr->base.get())) {
                if (prim->kind == AST::PrimaryExpression::Kind::Identifier) {
                    func_name = prim->identifier;
                    direct_primary = prim;
                }
            }

            // ---- 内置函数特殊处理 ----
            // 参照 Gallt 0.2.txt §8: input, output, heap, free, size, align
            // 以及标准文档§14: size, align
            // 参照 Gallt 0.2.txt §17: fileopen, fileclose, ... 文件操作函数
            // File-operation builtins from Gallt 0.2.txt §17
            if (is_file_builtin_name(func_name)) {
                return check_file_builtin_call(expr, func_name);
            }
            if (func_name == "output") {
                // output 接受任意数量、任意类型的参数，不进行类型检查，只检查表达式有效性
                // output accepts any number and type of arguments; each argument is still validated
                for (auto& arg : expr->arguments) {
                    check_expression(arg.get());
                }
                return AST::Type::make_void();
            }
            else if (func_name == "size" || func_name == "align") {
                if (expr->arguments.size() != 1) {
                    report_error(expr->location, ErrorCode::FunctionArgCountMismatch,
                        func_name + " expects exactly one argument");
                    return AST::Type::make_void();
                }
                auto* arg = expr->arguments[0].get();
                bool is_type_name = false;
                // 检查参数是否为类型名（标识符且为已知类型）
                if (auto* prim_arg = dynamic_cast<AST::PrimaryExpression*>(arg)) {
                    if (prim_arg->kind == AST::PrimaryExpression::Kind::Identifier) {
                        std::string name = prim_arg->identifier;
                        // 基本类型名
                        if (name == "int" || name == "float" || name == "double" || name == "char" ||
                            name == "bool" || name == "string" || name == "file" ||
                            name == "void") {
                            is_type_name = true;
                        }
                        // 结构体类型名
                        else if (struct_defs_.find(name) != struct_defs_.end()) {
                            is_type_name = true;
                        }
                    }
                }
                if (is_type_name) {
                    // 类型名作为参数，直接返回 int（表示大小或对齐，按 C 标准为 size_t）
                    return AST::Type::make_int();
                }
                else {
                    // 否则当作表达式求值
                    AST::Type arg_type = check_expression(arg);
                    if (arg_type.kind == TypeKind::Void) {
                        report_error(arg->location, ErrorCode::InvalidTypeCast,
                            "cannot take size/align of void");
                    }
                    // 按 C 标准，sizeof 和 alignof 返回 size_t，我们映射为 int
                    return AST::Type::make_int();
                }
            }
            else if (func_name == "free") {
                if (expr->arguments.size() != 1) {
                    report_error(expr->location, ErrorCode::FunctionArgCountMismatch,
                        "free expects exactly one argument");
                    return AST::Type::make_void();
                }
                AST::Type arg_type = check_expression(expr->arguments[0].get());
                if (arg_type.kind != TypeKind::Pointer) {
                    report_error(expr->arguments[0]->location, ErrorCode::FreeNonPointer,
                        "free requires pointer type, got '" + arg_type.to_string() + "'");
                }
                return AST::Type::make_void();
            }
            else if (func_name == "input") {
                // 示例 input(a) 将值读入左值变量；无参数形式返回 int（读取一个整数）
                // Example input(a) reads into an lvalue; zero-argument form returns int
                if (expr->arguments.size() > 1) {
                    report_error(expr->location, ErrorCode::FunctionArgCountMismatch,
                        "input expects zero or one argument");
                    return AST::Type::make_void();
                }
                if (expr->arguments.size() == 1) {
                    AST::Expression* arg = expr->arguments[0].get();
                    if (!arg->is_lvalue()) {
                        report_error(arg->location, ErrorCode::ExpressionSyntaxError,
                            "input argument must be an lvalue");
                    }
                    AST::Type arg_type = check_expression(arg);
                    if (arg_type.kind == TypeKind::Array) {
                        report_error(arg->location, ErrorCode::ExpressionSyntaxError,
                            "input cannot read directly into an array; use an element or pointer");
                    }
                    return AST::Type::make_void();
                }
                return AST::Type::make_int();
            }
            // ---- 内置函数处理结束 ----

            // 否则为普通函数调用；区分直接命名函数与函数指针
            // Ordinary calls: direct named functions versus function pointers
            AST::Type func_type;
            bool is_direct_function = false;
            if (direct_primary != nullptr) {
                Symbol* symbol = sym_table_.lookup(direct_primary->identifier);
                if (symbol != nullptr && symbol->kind == SymbolKind::Function) {
                    func_type = AST::Type::make_function(
                        std::make_shared<AST::Type>(symbol->type),
                        symbol->param_types);
                    is_direct_function = true;
                }
            }
            if (!is_direct_function) {
                func_type = base_type;
                if (func_type.kind == TypeKind::Pointer && func_type.pointee_type &&
                    func_type.pointee_type->kind == TypeKind::Function) {
                    func_type = *func_type.pointee_type;
                }
            }
            if (func_type.kind != TypeKind::Function) {
                report_error(expr->location, ErrorCode::ExpressionSyntaxError,
                    "function call requires a function or function pointer, got '" +
                    base_type.to_string() + "'");
                return AST::Type::make_void();
            }
            size_t expected = func_type.parameter_types.size();
            size_t provided = expr->arguments.size();
            if (expected != provided) {
                report_error(expr->location, ErrorCode::FunctionArgCountMismatch,
                    "function expects " + std::to_string(expected) +
                    " arguments, but " + std::to_string(provided) + " provided");
                return AST::Type::make_void();
            }
            for (size_t i = 0; i < expected; ++i) {
                if (i < expr->arguments.size()) {
                    AST::Type arg_type = check_expression(expr->arguments[i].get());
                    bool compatible = can_implicit_convert(arg_type, func_type.parameter_types[i]);
                    // null 常量可赋给任意指针
                    // The null constant is assignable to every pointer type
                    if (!compatible && arg_type.kind == TypeKind::Pointer &&
                        arg_type.pointee_type && arg_type.pointee_type->kind == TypeKind::Void &&
                        func_type.parameter_types[i].kind == TypeKind::Pointer) {
                        if (auto* prim = dynamic_cast<AST::PrimaryExpression*>(expr->arguments[i].get())) {
                            if (prim->kind == AST::PrimaryExpression::Kind::Null) {
                                compatible = true;
                            }
                        }
                    }
                    if (!compatible) {
                        report_error(expr->arguments[i]->location, ErrorCode::FunctionArgTypeMismatch,
                            "argument " + std::to_string(i + 1) + " type '" + arg_type.to_string() +
                            "' does not match expected type '" + func_type.parameter_types[i].to_string() + "'");
                    }
                }
            }
            if (func_type.return_type) {
                return *func_type.return_type;
            }
            return AST::Type::make_void();
        }
        case AST::PostfixExpression::Operator::Cast: {
            if (!is_complete_type(expr->cast_type) && expr->cast_type.kind != TypeKind::Void) {
                report_error(expr->location, ErrorCode::ExpressionSyntaxError,
                    "cast target type is incomplete");
                return AST::Type::make_void();
            }
            if (expr->cast_type.kind == TypeKind::Void) {
                report_error(expr->location, ErrorCode::InvalidTypeCast,
                    "cannot cast to void");
                return AST::Type::make_void();
            }
            bool allowed = false;
            if (is_numeric_type(base_type) && is_numeric_type(expr->cast_type)) {
                allowed = true;
            }
            else if (base_type.kind == TypeKind::Pointer && expr->cast_type.kind == TypeKind::Pointer) {
                allowed = true;
            }
            else if (base_type.kind == TypeKind::Pointer && is_integer_type(expr->cast_type)) {
                allowed = true;
            }
            else if (is_integer_type(base_type) && expr->cast_type.kind == TypeKind::Pointer) {
                allowed = true;
            }
            else if (can_implicit_convert(base_type, expr->cast_type)) {
                allowed = true;
            }
            if (!allowed) {
                report_error(expr->location, ErrorCode::InvalidTypeCast,
                    "cannot cast type '" + base_type.to_string() +
                    "' to '" + expr->cast_type.to_string() + "'");
                return AST::Type::make_void();
            }
            return expr->cast_type;
        }
        case AST::PostfixExpression::Operator::Increment:
        case AST::PostfixExpression::Operator::Decrement: {
            if (!expr->base->is_lvalue()) {
                report_error(expr->base->location, ErrorCode::ExpressionSyntaxError,
                    "operand of increment/decrement must be an lvalue");
            }
            if (!is_numeric_type(base_type) && base_type.kind != TypeKind::Pointer) {
                report_error(expr->location, ErrorCode::UnaryOperatorTypeMismatch,
                    "increment/decrement requires arithmetic or pointer type, got '" +
                    base_type.to_string() + "'");
            }
            return base_type;
        }
        case AST::PostfixExpression::Operator::Dot: {
            if (base_type.kind != TypeKind::Struct) {
                report_error(expr->location, ErrorCode::DotOnNonStruct,
                    "dot operator requires struct type, got '" + base_type.to_string() + "'");
                return AST::Type::make_void();
            }
            const auto* member = get_struct_member(base_type, expr->member_name);
            if (!member) {
                report_error(expr->location, ErrorCode::StructMemberNotFound,
                    "struct has no member named '" + expr->member_name + "'");
                return AST::Type::make_void();
            }
            return member->type;
        }
        case AST::PostfixExpression::Operator::Arrow: {
            if (base_type.kind != TypeKind::Pointer) {
                report_error(expr->location, ErrorCode::ArrowOnNonStructPtr,
                    "arrow operator requires pointer to struct, got '" + base_type.to_string() + "'");
                return AST::Type::make_void();
            }
            auto pointee = base_type.pointee_type;
            if (!pointee || pointee->kind != TypeKind::Struct) {
                report_error(expr->location, ErrorCode::ArrowOnNonStructPtr,
                    "arrow operator requires pointer to struct, got '" + base_type.to_string() + "'");
                return AST::Type::make_void();
            }
            const auto* member = get_struct_member(*pointee, expr->member_name);
            if (!member) {
                report_error(expr->location, ErrorCode::StructMemberNotFound,
                    "struct has no member named '" + expr->member_name + "'");
                return AST::Type::make_void();
            }
            return member->type;
        }
        default:
            report_error(expr->location, ErrorCode::ExpressionSyntaxError,
                "unknown postfix operator");
            return AST::Type::make_void();
        }
    }

    // ============================================================================
    // 文件操作内置函数检查（Gallt 0.2.txt §17，错误码 ER 0052 ~ ER 0059）
    // File-operation builtin checks (Gallt 0.2.txt §17, error codes ER 0052 - ER 0059)
    // ============================================================================

    AST::Type TypeChecker::check_file_builtin_call(AST::PostfixExpression* expr,
        const std::string& func_name) {
        auto info_it = file_builtin_table().find(func_name);
        if (info_it == file_builtin_table().end()) {
            return AST::Type::make_void();
        }
        const FileBuiltinInfo& info = info_it->second;

        AST::Type result_type;
        switch (info.result) {
        case FileResultKind::FileHandlePtr:
            result_type = AST::Type::make_pointer(
                std::make_shared<AST::Type>(AST::Type::make_file()));
            break;
        case FileResultKind::Bool: result_type = AST::Type::make_bool(); break;
        case FileResultKind::Int: result_type = AST::Type::make_int(); break;
        case FileResultKind::String: result_type = AST::Type::make_string(); break;
        }

        if (expr->arguments.size() != info.params.size()) {
            // ER 0052: 参数数量不匹配
            report_error(expr->location, ErrorCode::FileArgCountMismatch,
                "file operation '" + func_name + "' expects " +
                std::to_string(info.params.size()) + " argument(s), but " +
                std::to_string(expr->arguments.size()) + " provided");
            // 实参仍需检查，避免遗漏其中的语义错误
            for (auto& arg : expr->arguments) {
                check_expression(arg.get());
            }
            return result_type;
        }

        for (size_t i = 0; i < expr->arguments.size(); ++i) {
            AST::Expression* arg = expr->arguments[i].get();
            AST::Type arg_type = check_expression(arg);
            const std::string index = std::to_string(i + 1);

            bool is_null_literal = false;
            if (auto* prim = dynamic_cast<AST::PrimaryExpression*>(arg)) {
                if (prim->kind == AST::PrimaryExpression::Kind::Null) {
                    is_null_literal = true;
                }
            }

            switch (info.params[i]) {
            case FileParamKind::FileHandle:
                // ER 0055: 文件操作要求 file*（null 视为空句柄，由运行库安全处理）
                if (!is_file_pointer_type(arg_type) && !is_null_literal) {
                    report_error(arg->location, ErrorCode::FileHandleRequired,
                        "file operation '" + func_name + "' argument " + index +
                        " requires 'file*', got '" + arg_type.to_string() + "'");
                }
                break;
            case FileParamKind::Buffer:
                // ER 0057: 缓冲区必须为指针（数组按 C 规则退化为指针）
                if (arg_type.kind != TypeKind::Pointer && arg_type.kind != TypeKind::Array) {
                    report_error(arg->location, ErrorCode::FileBufferNotPointer,
                        "file operation '" + func_name + "' argument " + index +
                        " buffer must be a pointer, got '" + arg_type.to_string() + "'");
                }
                break;
            case FileParamKind::IntValue:
                // ER 0053: 一般整数参数类型不匹配
                if (!arg_type.is_integer()) {
                    report_error(arg->location, ErrorCode::FileArgTypeMismatch,
                        "file operation '" + func_name + "' argument " + index +
                        " type mismatch: expected 'int', got '" + arg_type.to_string() + "'");
                }
                break;
            case FileParamKind::SizeValue:
                // ER 0058: 文件读写大小必须为 int
                if (!arg_type.is_integer()) {
                    report_error(arg->location, ErrorCode::FileSizeNotInt,
                        "file operation '" + func_name + "' argument " + index +
                        " size must be 'int', got '" + arg_type.to_string() + "'");
                }
                break;
            case FileParamKind::StringValue:
                // ER 0059: 路径或模式必须为 string
                if (arg_type.kind != TypeKind::String) {
                    report_error(arg->location, ErrorCode::FilePathOrModeNotString,
                        "file operation '" + func_name + "' argument " + index +
                        " must be 'string', got '" + arg_type.to_string() + "'");
                }
                break;
            case FileParamKind::TextValue:
                // ER 0053: 写入内容必须为 string
                if (arg_type.kind != TypeKind::String) {
                    report_error(arg->location, ErrorCode::FileArgTypeMismatch,
                        "file operation '" + func_name + "' argument " + index +
                        " type mismatch: expected 'string', got '" + arg_type.to_string() + "'");
                }
                break;
            case FileParamKind::SeekOrigin:
                // ER 0053: 起始位置必须是整数
                if (!arg_type.is_integer()) {
                    report_error(arg->location, ErrorCode::FileArgTypeMismatch,
                        "file operation '" + func_name + "' argument " + index +
                        " type mismatch: expected 'int', got '" + arg_type.to_string() + "'");
                }
                break;
            }

            // ER 0054: fileopen 的模式字符串（字面量时静态校验）
            if (func_name == "fileopen" && i == 1 && arg_type.kind == TypeKind::String) {
                if (auto* prim = dynamic_cast<AST::PrimaryExpression*>(arg)) {
                    if (prim->kind == AST::PrimaryExpression::Kind::Literal &&
                        prim->literal_token.type == TokenType::StringLiteral) {
                        std::string mode;
                        if (string_literal_content(prim->literal_token.lexeme, mode) &&
                            !is_valid_file_open_mode(mode)) {
                            report_error(arg->location, ErrorCode::FileOpenModeInvalid,
                                "invalid file open mode: '" + mode + "'");
                        }
                    }
                }
            }

            // ER 0056: fileseek 的起始位置仅允许 0、1、2（常量时静态校验）
            if (func_name == "fileseek" && i == 2) {
                if (auto* prim = dynamic_cast<AST::PrimaryExpression*>(arg)) {
                    if (prim->kind == AST::PrimaryExpression::Kind::Literal &&
                        prim->literal_token.type == TokenType::IntegerLiteral) {
                        long long origin = 0;
                        std::string_view lexeme = prim->literal_token.lexeme;
                        auto parsed = std::from_chars(lexeme.data(),
                            lexeme.data() + lexeme.size(), origin);
                        if (parsed.ec == std::errc() &&
                            origin != 0 && origin != 1 && origin != 2) {
                            report_error(arg->location, ErrorCode::FileSeekOriginInvalid,
                                "fileseek() origin must be 0, 1 or 2, got " +
                                std::to_string(origin));
                        }
                    }
                }
            }
        }
        return result_type;
    }

    AST::Type TypeChecker::check_primary(AST::PrimaryExpression* expr) {
        switch (expr->kind) {
        case AST::PrimaryExpression::Kind::Literal: {
            switch (expr->literal_token.type) {
            case TokenType::IntegerLiteral:
                return AST::Type::make_int();
            case TokenType::FloatLiteral:
                // 无 f/F 后缀的浮点字面量默认为 double（Gallt 0.2.txt §7）
                // A float literal defaults to double unless it carries f/F (§7)
                if (!expr->literal_token.lexeme.empty()) {
                    char last = expr->literal_token.lexeme.back();
                    if (last == 'f' || last == 'F') {
                        return AST::Type::make_float();
                    }
                }
                return AST::Type::make_double();
            case TokenType::CharLiteral:
                return AST::Type::make_char();
            case TokenType::StringLiteral:
                return AST::Type::make_string();
            case TokenType::BoolLiteral:
                return AST::Type::make_bool();
            default:
                report_error(expr->location, ErrorCode::ExpressionSyntaxError,
                    "unknown literal type");
                return AST::Type::make_void();
            }
        }
        case AST::PrimaryExpression::Kind::Identifier: {
            // 首先检查是否为内置函数名（仅用于检查，不在此处处理）
            // 但为了符号表查找，先看符号表
            auto* sym = sym_table_.lookup(expr->identifier);
            if (sym) {
                if (sym->kind == SymbolKind::Function) {
                    // 函数名的表达式类型是完整函数类型，调用/取地址时使用
                    // A function-name expression has full function type
                    return AST::Type::make_function(
                        std::make_shared<AST::Type>(sym->type),
                        sym->param_types);
                }
                return sym->type;
            }
            // 未找到，检查是否为结构体类型名（仅在 size/align 中作为类型名使用时会被特殊处理，这里正常报错）
            // 但如果单独作为表达式出现，则报错
            report_error(expr->location, ErrorCode::UndefinedIdentifier,
                "undefined identifier '" + expr->identifier + "'");
            return AST::Type::make_void();
        }
        case AST::PrimaryExpression::Kind::Parens: {
            if (expr->paren_expr) {
                return check_expression(expr->paren_expr.get());
            }
            return AST::Type::make_void();
        }
        case AST::PrimaryExpression::Kind::Null: {
            return AST::Type::make_pointer(std::make_shared<AST::Type>(AST::Type::make_void()));
        }
        case AST::PrimaryExpression::Kind::Heap: {
            if (!is_complete_type(expr->heap_type) && expr->heap_type.kind != TypeKind::Void) {
                report_error(expr->location, ErrorCode::HeapFirstArgNotType,
                    "heap() first argument must be a complete type");
                return AST::Type::make_void();
            }
            if (expr->heap_size) {
                AST::Type size_type = check_expression(expr->heap_size.get());
                if (!size_type.is_integer()) {
                    report_error(expr->heap_size->location, ErrorCode::HeapSecondArgNotInteger,
                        "heap() second argument must be integer type, got '" + size_type.to_string() + "'");
                }
                return AST::Type::make_pointer(std::make_shared<AST::Type>(expr->heap_type));
            }
            else {
                return AST::Type::make_pointer(std::make_shared<AST::Type>(expr->heap_type));
            }
        }
        default:
            report_error(expr->location, ErrorCode::ExpressionSyntaxError,
                "unknown primary expression kind");
            return AST::Type::make_void();
        }
    }

    // ============================================================================
    // 类型工具实现
    // ============================================================================

    bool TypeChecker::can_implicit_convert(const AST::Type& from, const AST::Type& to) {
        if (from == to) return true;
        if (is_numeric_type(from) && is_numeric_type(to)) {
            // 文档要求：右值精度高于左值时编译器截断，因此算术类型间都允许隐式转换
            // The standard says higher precision right values are truncated, so all arithmetic conversions are implicit
            return true;
        }
        if (from.kind == TypeKind::Pointer && to.kind == TypeKind::Pointer) {
            if (from.pointee_type && from.pointee_type->kind == TypeKind::Void) return true;
            if (to.pointee_type && to.pointee_type->kind == TypeKind::Void) return true;
            return from == to;
        }
        if (to.kind == TypeKind::Bool && from.is_integer()) return true;
        if (from.kind == TypeKind::Array && to.kind == TypeKind::Pointer) {
            if (from.element_type && to.pointee_type) {
                return *from.element_type == *to.pointee_type;
            }
        }
        return false;
    }

    AST::Type TypeChecker::usual_arithmetic_conversion(const AST::Type& left, const AST::Type& right) {
        if (left == right) return left;
        int rank_left = 0, rank_right = 0;
        if (left.kind == TypeKind::Bool || left.kind == TypeKind::Char || left.kind == TypeKind::Int) rank_left = 1;
        else if (left.kind == TypeKind::Float) rank_left = 2;
        else if (left.kind == TypeKind::Double) rank_left = 3;
        if (right.kind == TypeKind::Bool || right.kind == TypeKind::Char || right.kind == TypeKind::Int) rank_right = 1;
        else if (right.kind == TypeKind::Float) rank_right = 2;
        else if (right.kind == TypeKind::Double) rank_right = 3;
        return (rank_left >= rank_right) ? left : right;
    }

    bool TypeChecker::is_numeric_type(const AST::Type& type) const {
        return type.is_arithmetic();
    }

    bool TypeChecker::is_integer_type(const AST::Type& type) const {
        return type.is_integer();
    }

    bool TypeChecker::is_bool_type(const AST::Type& type) const {
        return type.kind == TypeKind::Bool;
    }

    bool TypeChecker::is_pointer_type(const AST::Type& type) const {
        return type.kind == TypeKind::Pointer;
    }

    bool TypeChecker::is_struct_type(const AST::Type& type) const {
        return type.kind == TypeKind::Struct;
    }

    bool TypeChecker::is_function_type(const AST::Type& type) const {
        return type.kind == TypeKind::Function;
    }

    bool TypeChecker::is_array_type(const AST::Type& type) const {
        return type.kind == TypeKind::Array;
    }

    bool TypeChecker::is_void_type(const AST::Type& type) const {
        return type.kind == TypeKind::Void;
    }

    bool TypeChecker::is_file_type(const AST::Type& type) const {
        return type.kind == TypeKind::File;
    }

    bool TypeChecker::is_file_pointer_type(const AST::Type& type) const {
        return type.kind == TypeKind::Pointer && type.pointee_type &&
            type.pointee_type->kind == TypeKind::File;
    }

    bool TypeChecker::is_complete_type(const AST::Type& type) {
        if (type.kind == TypeKind::Void) return true;
        if (type.kind == TypeKind::Int || type.kind == TypeKind::Float ||
            type.kind == TypeKind::Double || type.kind == TypeKind::Char ||
            type.kind == TypeKind::Bool || type.kind == TypeKind::String ||
            type.kind == TypeKind::File) {
            return true;
        }
        if (type.kind == TypeKind::Pointer) {
            return true;
        }
        if (type.kind == TypeKind::Array) {
            if (!type.array_size.has_value() || type.array_size.value() == 0) return false;
            if (type.element_type) return is_complete_type(*type.element_type);
            return false;
        }
        if (type.kind == TypeKind::Struct) {
            auto it = struct_defs_.find(type.struct_name);
            return it != struct_defs_.end();
        }
        if (type.kind == TypeKind::Function) {
            if (type.return_type && !is_complete_type(*type.return_type) && type.return_type->kind != TypeKind::Void) {
                return false;
            }
            for (const auto& p : type.parameter_types) {
                if (!is_complete_type(p) && p.kind != TypeKind::Void) {
                    return false;
                }
            }
            return true;
        }
        return false;
    }

    AST::StructDefinition* TypeChecker::get_struct_definition(const std::string& name) const {
        auto it = struct_defs_.find(name);
        return (it != struct_defs_.end()) ? it->second : nullptr;
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

    // ============================================================================
    // 错误报告辅助
    // ============================================================================

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

    // ============================================================================
    // 主函数验证
    // ============================================================================

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

    // ============================================================================
    // 内置函数声明
    // ============================================================================

    void TypeChecker::declare_builtin_functions() {
        // 每个 TypeChecker 实例各自注册一次：使用成员标志而非函数级 static，
        // 否则同一进程内新建的第二个实例将找不到内置函数
        // Declared once per TypeChecker instance (a function-local static would leak
        // the state across instances)
        if (builtins_declared_) return;
        builtins_declared_ = true;

        // 确保全局作用域存在（已在 check_program 中进入）
        // 注册 input
        Symbol sym_input = Symbol::make_function("input", AST::Type::make_int(),
            {}, {}, SourceLocation{}, nullptr);
        sym_input.function_node = reinterpret_cast<AST::FunctionDefinition*>(1);
        sym_table_.declare(sym_input);

        // 注册 output；参数可变，实际检查在调用路径特殊处理
        // output has variadic arguments and is handled specially at call sites
        Symbol sym_output = Symbol::make_function("output", AST::Type::make_void(),
            { AST::Type::make_string() }, {}, SourceLocation{}, nullptr);
        sym_output.function_node = reinterpret_cast<AST::FunctionDefinition*>(2);
        sym_table_.declare(sym_output);

        // 注册 size；其类型参数在调用路径特殊处理
        // size is registered; type-name arguments are handled at call sites
        Symbol sym_size = Symbol::make_function("size", AST::Type::make_int(),
            { AST::Type::make_void() }, {}, SourceLocation{}, nullptr);
        sym_size.function_node = reinterpret_cast<AST::FunctionDefinition*>(3);
        sym_table_.declare(sym_size);

        // 注册 align
        // align is registered with the same special-call handling as size
        Symbol sym_align = Symbol::make_function("align", AST::Type::make_int(),
            { AST::Type::make_void() }, {}, SourceLocation{}, nullptr);
        sym_align.function_node = reinterpret_cast<AST::FunctionDefinition*>(4);
        sym_table_.declare(sym_align);

        // 注册 free
        // free accepts a single pointer argument
        Symbol sym_free = Symbol::make_function("free", AST::Type::make_void(),
            { AST::Type::make_pointer(std::make_shared<AST::Type>(AST::Type::make_void())) },
            {}, SourceLocation{}, nullptr);
        sym_free.function_node = reinterpret_cast<AST::FunctionDefinition*>(5);
        sym_table_.declare(sym_free);

        // 注册 Gallt 0.2.txt §17 的文件操作内置函数
        // Register the Gallt 0.2.txt §17 file-operation builtins
        // 它们是内置函数标识符（不是关键字）；参数/返回值在 check_file_builtin_call 中校验
        // They are builtin function identifiers (not keywords); their signatures are
        // validated by check_file_builtin_call
        AST::Type file_handle = AST::Type::make_pointer(
            std::make_shared<AST::Type>(AST::Type::make_file()));
        AST::Type buffer_ptr = AST::Type::make_pointer(
            std::make_shared<AST::Type>(AST::Type::make_void()));

        auto declare_file_builtin = [&](std::string_view name, AST::Type result,
                                        std::vector<AST::Type> params) {
            Symbol sym = Symbol::make_function(name, std::move(result),
                params, {}, SourceLocation{}, nullptr);
            // 非空哨兵：调用路径由 check_file_builtin_call 处理，不参与普通调用
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
    }

    // ============================================================================
    // 辅助函数（实现未在类中定义的）
    // ============================================================================

    std::optional<size_t> TypeChecker::evaluate_const_expression(AST::Expression* expr) {
        if (auto* prim = dynamic_cast<AST::PrimaryExpression*>(expr)) {
            if (prim->kind == AST::PrimaryExpression::Kind::Literal &&
                prim->literal_token.type == TokenType::IntegerLiteral) {
                std::string_view lexeme = prim->literal_token.lexeme;
                size_t value = 0;
                auto [ptr, ec] = std::from_chars(lexeme.data(), lexeme.data() + lexeme.size(), value);
                if (ec == std::errc()) return value;
            }
        }
        return std::nullopt;
    }

    bool TypeChecker::is_constant_integer_expression(AST::Expression* expr, size_t* out_value) {
        if (auto val = evaluate_const_expression(expr)) {
            if (out_value) *out_value = *val;
            return true;
        }
        return false;
    }

} // namespace gallt
