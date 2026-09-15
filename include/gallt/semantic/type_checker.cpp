// semantic/type_checker.cpp
// 类型检查器实现 —— 执行语义分析、类型推导、符号解析、错误报告
// Type Checker implementation — performs semantic analysis, type inference, symbol resolution, error reporting

#include "../semantic/type_checker.hpp"
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

    namespace {
        // 判断表达式是否为 null 字面量（用于 file 与 null 的比较/赋值）
        // Whether the expression is the null literal (file vs null handling)
        bool is_null_literal_expr(const AST::Expression* expr) {
            auto* prim = dynamic_cast<const AST::PrimaryExpression*>(expr);
            return prim != nullptr && prim->kind == AST::PrimaryExpression::Kind::Null;
        }

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
        enter_scope();

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

        // 全局对象的构造调用（Gallt 0.3.txt §20）：在全局作用域内检查，
        // 此时所有函数与全局变量都已声明
        // Constructor calls for global objects: checked in the global scope after every
        // function and global variable has been declared
        for (auto& stmt : program->global_initializers) {
            check_statement(stmt.get());
        }

        // 验证主函数
        verify_main_function();

        // 退出全局作用域
        exit_scope();

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
        // 0.3 §18：同一作用域内允许函数重载（参数列表不同）；完全相同视为重复定义
        // 0.3 §18: same-scope overloads allowed; identical parameter lists are redefinitions
        Symbol sym = Symbol::make_function(
            node->name, node->return_type,
            node->parameters, node->param_names,
            node->location, nullptr);
        sym.extern_node = node;
        Symbol* existing = sym_table_.lookup_current(node->name);
        if (existing != nullptr) {
            if (existing->kind != SymbolKind::Function) {
                report_error(node->location, ErrorCode::RedefinedIdentifier,
                    "identifier '" + node->name + "' already declared");
                return;
            }
            // extern 与用户函数、多个 extern 之间构成重载集（第 18 章）
            // An extern shares an overload set with user functions and other externs (§18)
            if (!sym_table_.declare_overload(sym)) {
                report_error(node->location, ErrorCode::RedefinedFunction,
                    "function '" + node->name +
                    "' already declared with the same parameter list");
                return;
            }
            if (std::vector<Symbol>* set = sym_table_.lookup_overloads(node->name)) {
                for (const Symbol& other : *set) {
                    if (&other == &(*set).back()) continue;
                    if (overloads_ambiguous_by_defaults(other, (*set).back())) {
                        report_error(node->location, ErrorCode::OverloadAmbiguous, { node->name });
                        break;
                    }
                }
            }
            mangle_overload_set(node->name);
            return;
        }
        if (!sym_table_.declare(sym)) {
            report_error(node->location, ErrorCode::RedefinedFunction,
                "function '" + node->name + "' already declared");
            return;
        }
        sym_table_.declare_overload(sym);
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
                    bool same_signature = existing->param_types.size() == node->parameters.size() &&
                        std::equal(existing->param_types.begin(), existing->param_types.end(),
                            node->parameters.begin());
                    if (same_signature) {
                        if (existing->type != node->return_type) {
                            report_error(node->location, ErrorCode::FunctionReturnTypeMismatch,
                                "function '" + node->name +
                                "' does not match its extern declaration return type");
                        }
                        existing->function_node = node;
                        if (existing->param_names.empty() && !node->param_names.empty()) {
                            existing->param_names = node->param_names;
                        }
                    }
                    else {
                        // 与 extern 同名但签名不同 → 作为重载参与决议（第 18 章）
                        // Same extern name with a different signature becomes an overload
                        Symbol overload_sym = Symbol::make_function(node->name, node->return_type,
                            node->parameters, node->param_names, node->location, node);
                        if (!sym_table_.declare_overload(overload_sym)) {
                            report_error(node->location, ErrorCode::RedefinedFunction,
                                "function '" + node->name +
                                "' already defined with the same parameter list");
                            return;
                        }
                        if (std::vector<Symbol>* set = sym_table_.lookup_overloads(node->name)) {
                            for (const Symbol& other : *set) {
                                if (&other == &(*set).back()) continue;
                                if (overloads_ambiguous_by_defaults(other, (*set).back())) {
                                    report_error(node->location, ErrorCode::OverloadAmbiguous,
                                        { node->name });
                                    break;
                                }
                            }
                        }
                        mangle_overload_set(node->name);
                    }
                }
                else {
                    // 0.3 §18：同名但参数列表不同 → 重载
                    Symbol overload_sym = Symbol::make_function(
                        node->name, node->return_type, node->parameters, node->param_names,
                        node->location, node);
                    if (!sym_table_.declare_overload(overload_sym)) {
                        report_error(node->location, ErrorCode::RedefinedFunction,
                            "function '" + node->name +
                            "' already defined with the same parameter list");
                        return;
                    }
                    // 第 18 章：定义阶段检查默认参数导致的必然二义性
                    // §18: definition-stage check for inevitable ambiguity caused by defaults
                    if (std::vector<Symbol>* set = sym_table_.lookup_overloads(node->name)) {
                        for (const Symbol& other : *set) {
                            if (&other == &(*set).back()) continue;
                            if (overloads_ambiguous_by_defaults(other, (*set).back())) {
                                report_error(node->location, ErrorCode::OverloadAmbiguous,
                                    { node->name });
                                break;
                            }
                        }
                    }
                    // 重载集需要唯一名字供代码生成使用
                    mangle_overload_set(node->name);
                }
            }
            else {
                report_error(node->location, ErrorCode::RedefinedIdentifier,
                    "identifier '" + node->name + "' already declared as non-function");
                return;
            }
        }
        else {
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
            sym_table_.declare_overload(sym);
        }
       enter_scope();
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
        // 默认参数在函数自身作用域内做类型检查（Gallt 0.3.txt §8）
        // 默认参数在函数自身作用域内做类型检查（Gallt 0.3.txt §8）
        // Default arguments are type-checked inside the function's own scope
        for (size_t i = 0; i < node->param_defaults.size() && i < node->parameters.size(); ++i) {
            if (node->param_defaults[i] == nullptr) continue;
            AST::Type default_type = check_expression(node->param_defaults[i].get());
            if (!can_implicit_convert(default_type, node->parameters[i])) {
                report_error(node->param_defaults[i]->location,
                    ErrorCode::FunctionArgTypeMismatch,
                    "default argument " + std::to_string(i + 1) + " of function '" +
                    node->name + "' has type '" + default_type.to_string() +
                    "' which does not match parameter type '" +
                    node->parameters[i].to_string() + "'");
            }
        }
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
        exit_scope();
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
            // 0.4.1 §7：成员数组的长度同样必须是常量整数表达式（可引用 const 常量）
            // 0.4.1 §7: member array lengths must also be constant integer expressions
            // (const constants may be referenced)
            if (member.type.kind == TypeKind::Array && !member.array_size.has_value() &&
                member.array_size_expr != nullptr) {
                auto value = evaluate_const_integer_expression(member.array_size_expr.get());
                if (value.has_value() && *value > 0) {
                    member.array_size = static_cast<std::size_t>(*value);
                    member.type.array_size = member.array_size;
                }
                else {
                    report_error(member.location, ErrorCode::ArraySizeNotConstant,
                        "array size in a declaration must be a constant integer expression");
                }
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
        else if (auto* destruct_stmt = dynamic_cast<AST::DestructStatement*>(stmt)) {
            // destruct [指针]（Gallt 0.3.txt §20）
            // destruct [pointer] (Gallt 0.3.txt §20)
            AST::Type target_type = check_expression(destruct_stmt->target.get());
            if (target_type.kind != TypeKind::Pointer) {
                report_error(destruct_stmt->location, ErrorCode::FreeNonPointer,
                    "destruct requires a pointer, got '" + target_type.to_string() + "'");
            }
           else if (!target_type.pointee_type ||
               target_type.pointee_type->kind != TypeKind::Struct) {
                // ER 0092：destruct 只能作用于 construct 分配的对象
                // ER 0092: destruct only applies to objects allocated by construct
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

    void TypeChecker::check_variable_declaration(AST::VariableDeclaration* decl) {
        // 0.4.1 §2：const 对象必须在编译期求值，且不能存储变量（ER 0116）
        // 0.4.1 §2: a const object must be evaluated at compile time and cannot store
        // a variable (ER 0116)
        if (decl->type.is_const) {
            check_const_declaration(decl);
        }
        // 将声明符上的数组长度同步到类型对象，避免重复维护两处状态
        // Sync the declarator array length into the type object
        if (decl->type.kind == TypeKind::Array) {
            if (decl->array_size.has_value() && !decl->type.array_size.has_value()) {
                decl->type.array_size = decl->array_size;
            }
            if (!decl->array_size.has_value() && decl->type.array_size.has_value()) {
                decl->array_size = decl->type.array_size;
            }
            // 0.4.1 §7：数组长度必须是常量整数表达式；支持引用 const 常量（0.4.1 §2）
            // 0.4.1 §7: an array length must be a constant integer expression; const
            // constants may be referenced (0.4.1 §2)
            if (!decl->array_size.has_value() && decl->array_size_expr != nullptr) {
                auto value = evaluate_const_integer_expression(decl->array_size_expr.get());
                if (value.has_value() && *value > 0) {
                    decl->type.array_size = static_cast<std::size_t>(*value);
                    decl->array_size = static_cast<std::size_t>(*value);
                }
                else {
                    report_error(decl->array_size_expr->location,
                        ErrorCode::ArraySizeNotConstant,
                        "array size in a declaration must be a constant integer expression");
                }
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
                // 目标类型：变量类型（用于函数指针上下文中的重载选择，第 18 章）
                // Expected type: the declared type (function-pointer overload selection, §18)
                const AST::Type* saved_expected = expected_type_;
                expected_type_ = &decl->type;
                AST::Type init_type = check_expression(expr_init->expr.get());
                expected_type_ = saved_expected;
                // 第 20 章：[nocopy] / [nomove] 在**所有**拷贝/移动上下文都生效
                // §20: [nocopy]/[nomove] apply in every copy/move context
                if (decl->type.kind == TypeKind::Struct && init_type.kind == TypeKind::Struct &&
                    init_type.struct_name == decl->type.struct_name) {
                    if (is_move_expression(expr_init->expr.get())) {
                        if (!type_is_movable(decl->type)) {
                            diag_.report_error_template(decl->location,
                                ErrorCode::NoMoveViolation, { decl->type.struct_name });
                        }
                    }
                    else if (!type_is_copyable(decl->type)) {
                        diag_.report_error_template(decl->location,
                            ErrorCode::NoCopyViolation, { decl->type.struct_name });
                    }
                }
                bool is_null = false;
                if (auto* primary = dynamic_cast<AST::PrimaryExpression*>(expr_init->expr.get())) {
                    if (primary->kind == AST::PrimaryExpression::Kind::Null) {
                        is_null = true;
                    }
                }
               if (!is_null || decl->type.kind != TypeKind::Pointer) {
                   if (!can_implicit_convert(init_type, decl->type)) {
                       // file 句柄可用 null 初始化（Gallt 0.3.txt §2/§17）
                       // A `file` handle may be initialized with null
                       if (is_null && decl->type.kind == TypeKind::File) {
                           // 允许
                       }
                       else {
                       report_error(decl->location, ErrorCode::AssignmentTypeMismatch,
                           "cannot initialize variable '" + decl->name +
                           "' with type '" + init_type.to_string() +
                           "' (expected '" + decl->type.to_string() + "')");
                       }
                   }
               }
            }
            else if (auto* arr_init = dynamic_cast<AST::ArrayInitializer*>(decl->initializer.get())) {
                if (decl->type.kind == TypeKind::Struct) {
                    // 结构体使用花括号初始化，顺序与成员声明顺序一致（第 14 章）
                    // Structs are brace-initialized in member declaration order (§14)
                    check_struct_initializer(arr_init, decl->type, decl->location);
                }
                else if (decl->type.kind != TypeKind::Array) {
                    report_error(decl->location, ErrorCode::AssignmentTypeMismatch,
                        "array initializer for non-array variable");
                }
                else {
                    // （多维）数组初始化：逐维校验形状与长度（第 7 章 / ER 0015 / ER 0016）
                    // (Multi-dimensional) array initialization validates every dimension
                    check_array_initializer(arr_init, decl->type, decl->location);
                }
            }
        }
        Symbol sym = Symbol::make_variable(decl->name, decl->type, decl->location,
            decl->initializer != nullptr);
        // 0.4.1 §2：const 变量不可修改
        // 0.4.1 §2: const variables are not mutable
        if (decl->type.is_const) {
            sym.is_mutable = false;
        }
        if (!sym_table_.declare(sym)) {
            report_error(decl->location, ErrorCode::RedefinedIdentifier,
                "variable '" + decl->name + "' already declared");
        }
    }

    void TypeChecker::check_struct_initializer(AST::ArrayInitializer* init,
        const AST::Type& struct_type, SourceLocation loc) {
        // 第 14 章：结构体花括号初始化按成员声明顺序逐项校验；嵌套结构体与数组成员
        // 使用各自的内层花括号，并递归校验其成员 / 元素
        // §14: struct brace initialization follows member declaration order; nested structs
        // and array members use their own braces and are validated recursively
        if (init == nullptr) return;
        AST::StructDefinition* def = get_struct_definition(struct_type.struct_name);
        if (def == nullptr) {
            report_error(loc, ErrorCode::ExpressionSyntaxError,
                "unknown struct type '" + struct_type.struct_name + "'");
            return;
        }
        if (init->elements.size() > def->members.size()) {
            report_error(loc, ErrorCode::StructInitLengthMismatch,
                "struct initializer has too many elements; expected at most " +
                std::to_string(def->members.size()) + ", got " +
                std::to_string(init->elements.size()));
            return;
        }
        // 成员的目标类型：用于函数指针成员初始化时的重载选择（第 18 章）
        // Member target type: overload selection for function-pointer members (§18)
        auto check_member_expression = [&](AST::Expression* e, const AST::Type& target) {
            const AST::Type* saved = expected_type_;
            expected_type_ = &target;
            AST::Type result = check_expression(e);
            expected_type_ = saved;
            return result;
        };
        for (std::size_t i = 0; i < init->elements.size(); ++i) {
            const auto& member = def->members[i];
            AST::Initializer* element = init->elements[i].get();
            if (auto* nested = dynamic_cast<AST::ArrayInitializer*>(element)) {
                if (member.type.kind == TypeKind::Struct) {
                    check_struct_initializer(nested, member.type, loc);
                }
                else if (member.type.kind == TypeKind::Array) {
                    // 逐维校验（含多维数组成员）
                    // Validate every dimension (multi-dimensional members too)
                    check_array_initializer(nested, member.type, loc);
                }
                else {
                    report_error(element->location, ErrorCode::ExpressionSyntaxError,
                        "struct member requires an expression initializer");
                }
                continue;
            }
            auto* e = dynamic_cast<AST::ExpressionInitializer*>(element);
            if (e == nullptr) continue;
            if (member.type.kind == TypeKind::Array) {
                // 第 7/14 章：数组成员必须用花括号逐元素初始化；
                // 数组不能由数组值整体初始化（与“数组不能整体赋值”一致）
                // §7/§14: an array member requires a braced element list; an array value
                // cannot initialize an array member as a whole
                report_error(e->location, ErrorCode::StructMemberTypeMismatch,
                    "array member '" + member.name + "' requires a braced initializer");
                check_member_expression(e->expr.get(), member.type);
                continue;
            }
            AST::Type init_type = check_member_expression(e->expr.get(), member.type);
            if (!can_implicit_convert(init_type, member.type)) {
                report_error(e->location, ErrorCode::StructMemberTypeMismatch,
                    "initializer for member '" + member.name + "' does not match its type");
            }
        }
    }

    void TypeChecker::check_array_initializer(AST::ArrayInitializer* init,
        const AST::Type& array_type, SourceLocation loc) {
        // 第 7 章 / ER 0015 / ER 0016：数组（含多维）的花括号初始化逐维校验——
        // 每一维的元素个数必须与声明长度一致（长度由初始化列表推断的维度除外），
        // 数组元素是数组时必须使用内层花括号，其它元素的类型必须可隐式转换。
        // §7 / ER 0015 / ER 0016: validate every dimension of a brace initializer; each
        // dimension must provide exactly the declared number of elements (unless the size
        // was inferred), array elements require their own braces, and element types must
        // be implicitly convertible.
        if (init == nullptr || array_type.kind != TypeKind::Array) return;
        AST::Type element = array_type.element_type ? *array_type.element_type
            : AST::Type::make_void();
        const std::size_t provided = init->elements.size();
        if (array_type.array_size.has_value()) {
            if (provided != array_type.array_size.value()) {
                report_error(loc, ErrorCode::ArrayLengthMismatch,
                    "initializer length " + std::to_string(provided) +
                    " does not match array size " +
                    std::to_string(array_type.array_size.value()));
            }
        }
        else if (provided == 0) {
            report_error(loc, ErrorCode::EmptyArrayInitializer,
                "cannot infer array size from empty initializer");
        }
        for (auto& item : init->elements) {
            if (auto* nested = dynamic_cast<AST::ArrayInitializer*>(item.get())) {
                if (element.kind == TypeKind::Struct) {
                    // 结构体元素用内层花括号提供成员值（第 14 章，见 gen_033/ct_033）
                    // A struct element uses its own braces for member values (§14)
                    check_struct_initializer(nested, element, item->location);
                }
                else if (element.kind == TypeKind::Array) {
                    check_array_initializer(nested, element, item->location);
                }
                else {
                    report_error(item->location, ErrorCode::AssignmentTypeMismatch,
                        "nested braced initializer for a non-aggregate element");
                }
                continue;
            }
            auto* e = dynamic_cast<AST::ExpressionInitializer*>(item.get());
            if (e == nullptr) continue;
            // 元素的目标类型用于按目标函数指针类型选择重载（第 18 章）
            // The element target type drives overload selection by target type (§18)
            const AST::Type* saved_expected = expected_type_;
            expected_type_ = &element;
            AST::Type value_type = check_expression(e->expr.get());
            expected_type_ = saved_expected;
            if (element.kind == TypeKind::Array) {
                report_error(e->location, ErrorCode::AssignmentTypeMismatch,
                    "array element requires a braced initializer");
                continue;
            }
            if (!can_implicit_convert(value_type, element)) {
                report_error(e->location, ErrorCode::AssignmentTypeMismatch,
                    "array element type mismatch");
            }
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
        enter_scope();
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
        exit_scope();
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
            // 目标类型：函数返回类型（返回函数指针时按类型选择重载，第 18 章）
            // Expected type: the function return type (§18)
            const AST::Type* saved_expected = expected_type_;
            expected_type_ = &expected;
            AST::Type actual = check_expression(return_stmt->value.get());
            expected_type_ = saved_expected;
            // 第 20 章：返回结构体时按拷贝/移动构造语义检查 [nocopy]/[nomove]
            // §20: returning a struct copy/move-constructs it, so [nocopy]/[nomove] apply
            if (expected.kind == TypeKind::Struct && actual.kind == TypeKind::Struct &&
                actual.struct_name == expected.struct_name) {
                if (is_move_expression(return_stmt->value.get())) {
                    if (!type_is_movable(expected)) {
                        diag_.report_error_template(return_stmt->location,
                            ErrorCode::NoMoveViolation, { expected.struct_name });
                    }
                }
                else if (!type_is_copyable(expected)) {
                    diag_.report_error_template(return_stmt->location,
                        ErrorCode::NoCopyViolation, { expected.struct_name });
                }
            }
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
        // 0.4.1 §2：const 常量不可修改（ER 0115）
        // 0.4.1 §2: a const constant cannot be modified (ER 0115)
        if (left_type.is_const) {
            diag_.report_error_template(expr->left->location, ErrorCode::ConstModification,
                { expression_display_name(expr->left.get()) });
        }
        // 目标类型：左值类型（函数指针赋值时按目标类型选择重载，第 18 章）
        // Expected type: the left-hand type (§18 function-pointer overload selection)
        const AST::Type* saved_expected = expected_type_;
        expected_type_ = &left_type;
        AST::Type right_type = check_expression(expr->right.get());
        expected_type_ = saved_expected;
        // 第 20 章：[nocopy]/[nomove] 同样约束赋值（含嵌套上下文）
        // §20: [nocopy]/[nomove] also constrain assignment, including nested contexts
        if (expr->op == AST::AssignmentExpression::Operator::Assign &&
            left_type.kind == TypeKind::Struct && right_type.kind == TypeKind::Struct &&
            left_type.struct_name == right_type.struct_name) {
            if (is_move_expression(expr->right.get())) {
                if (!type_is_movable(left_type)) {
                    diag_.report_error_template(expr->location, ErrorCode::NoMoveViolation,
                        { left_type.struct_name });
                }
            }
            else if (!type_is_copyable(left_type)) {
                diag_.report_error_template(expr->location, ErrorCode::NoCopyViolation,
                    { left_type.struct_name });
            }
        }
        // Gallt 0.3.txt §20：数组本身不能整体赋值（ER 0030）
        // Gallt 0.3.txt §20: an array as a whole cannot be assigned (ER 0030)
        if (left_type.kind == TypeKind::Array) {
            report_error(expr->location, ErrorCode::ArrayOperatorNotSupported,
                "array type does not support assignment");
            return left_type;
        }
       bool ok = false;
       if (expr->op == AST::AssignmentExpression::Operator::Assign) {
           ok = can_implicit_convert(right_type, left_type);
            // Gallt 0.3.txt §2/§17：file 为不透明类型，允许用 null 置空句柄
            // Gallt 0.3.txt §2/§17: a `file` handle may be cleared with null
            if (!ok && left_type.kind == TypeKind::File) {
                if (auto* prim = dynamic_cast<AST::PrimaryExpression*>(expr->right.get())) {
                    if (prim->kind == AST::PrimaryExpression::Kind::Null) ok = true;
                }
            }
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
        bool reported = false;   // 已给出更精确的诊断（ER 0043 / 关系比较）
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
       // Gallt 0.3.txt §2/§17：file 是不透明类型，可与 null 比较
       // Gallt 0.3.txt §2/§17: `file` is opaque and may be compared with null
       else if (left.kind == TypeKind::File && right.kind == TypeKind::File) {
           ok = true;
       }
        else if ((left.kind == TypeKind::File || is_file_pointer_type(left)) &&
            is_null_literal_expr(expr->right.get())) {
            ok = true;
        }
       else if ((right.kind == TypeKind::File || is_file_pointer_type(right)) &&
            is_null_literal_expr(expr->left.get())) {
            ok = true;
        }
        // Gallt 0.4.txt §13：函数指针的比较规则
        //   - `fptr == null` / `fptr != null` 合法（结果 bool）
        //   - 同签名函数指针之间的 `==` / `!=` 合法
        //   - 关系比较（> < >= <=）不适用于函数指针 → ER 0004
        //   - 不同签名的函数指针比较 → ER 0043
        // Gallt 0.4.txt §13: comparison rules for function pointers
        //   - equality/inequality with null is valid (yields bool)
        //   - equality/inequality between same-signature function pointers is valid
        //   - relational operators do not apply to function pointers (ER 0004)
        //   - comparing different signatures is ER 0043
        else if (left.kind == TypeKind::Function || right.kind == TypeKind::Function) {
            bool is_equal_op = (expr->op == AST::ComparisonExpression::Operator::Equal ||
                expr->op == AST::ComparisonExpression::Operator::NotEqual);
            const AST::Type& fn_side = (left.kind == TypeKind::Function) ? left : right;
            const AST::Type& other_side = (left.kind == TypeKind::Function) ? right : left;
            bool other_is_null = is_null_literal_expr(left.kind == TypeKind::Function
                ? expr->right.get() : expr->left.get());
            if (!is_equal_op) {
                report_error(expr->location, ErrorCode::BinaryOperatorTypeMismatch,
                    "relational comparison is not allowed for function pointers");
                reported = true;
            }
            else if (other_is_null) {
                ok = true;
            }
            else if (other_side.kind == TypeKind::Function) {
                bool same_signature = fn_side.parameter_types.size() ==
                    other_side.parameter_types.size();
                if (same_signature) {
                    for (std::size_t i = 0; i < fn_side.parameter_types.size(); ++i) {
                        if (!(fn_side.parameter_types[i] == other_side.parameter_types[i])) {
                            same_signature = false;
                            break;
                        }
                    }
                }
                if (same_signature) {
                    same_signature = fn_side.return_type && other_side.return_type &&
                        (*fn_side.return_type == *other_side.return_type);
                }
                if (same_signature) {
                    ok = true;
                }
                else {
                    // ER 0043：函数指针类型不匹配
                    // ER 0043: function pointer signature mismatch
                    report_error(expr->location, ErrorCode::FuncPtrTypeMismatch,
                        "function pointer signature mismatch in comparison");
                    reported = true;
                }
            }
            else {
                report_error(expr->location, ErrorCode::BinaryOperatorTypeMismatch,
                    "comparison operands must be compatible function pointers, got '" +
                    left.to_string() + "' and '" + right.to_string() + "'");
                reported = true;
            }
        }
       if (!ok && !reported) {
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
            // Gallt 0.3.txt §7：字符串 + 字符串 = 拼接；数字 + 数字 = 算术；
            // 字符串与数字混合 → 类型不匹配（ER 0004）
            // Gallt 0.3.txt §7: string+string concatenates, number+number computes, and a
            // mixed string/number pair is a type mismatch
            if (left.kind == TypeKind::String || right.kind == TypeKind::String) {
                if (left.kind == TypeKind::String && right.kind == TypeKind::String) {
                    return AST::Type::make_string();
                }
                if (is_numeric_type(left) && is_numeric_type(right)) {
                    // 两侧均非字符串：交由下方算术分支处理
                    // Neither side is a string: fall through to the arithmetic branch
                }
                else {
                    report_error(expr->location, ErrorCode::BinaryOperatorTypeMismatch,
                        "operator '+' cannot mix string and numeric operands, got '" +
                        left.to_string() + "' and '" + right.to_string() + "'");
                    return AST::Type::make_void();
                }
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
        // 第 18 章 / 第 13 章：&重载函数名 时按目标函数指针类型选择重载
        // §18/§13: &overloaded-name selects the overload matching the target function pointer
        if (expr->op == AST::UnaryExpression::Operator::AddressOf) {
            if (auto* prim = dynamic_cast<AST::PrimaryExpression*>(expr->operand.get())) {
                if (prim->kind == AST::PrimaryExpression::Kind::Identifier) {
                    std::vector<Symbol>* set = sym_table_.lookup_overloads(prim->identifier);
                    if (set != nullptr && !set->empty()) {
                        Symbol* chosen = nullptr;
                        if (set->size() == 1) {
                            chosen = &(*set)[0];
                        }
                        else if (expected_type_ != nullptr) {
                            chosen = select_overload_by_target_type(*set, *expected_type_,
                                prim->identifier, expr->location);
                        }
                        else {
                            report_error(expr->location, ErrorCode::OverloadAmbiguous,
                                { prim->identifier });
                        }
                        if (chosen == nullptr) return AST::Type::make_void();
                        record_function_resolution(prim, *chosen);
                        return AST::Type::make_function(
                            std::make_shared<AST::Type>(chosen->type), chosen->param_types);
                    }
                }
            }
        }
        AST::Type operand = check_expression(expr->operand.get());
        switch (expr->op) {
        case AST::UnaryExpression::Operator::Increment:
        case AST::UnaryExpression::Operator::Decrement:
            if (!expr->operand->is_lvalue()) {
                report_error(expr->operand->location, ErrorCode::ExpressionSyntaxError,
                    "operand of increment/decrement must be an lvalue");
            }
            // 0.4.1 §2：自增/自减同样修改对象，const 常量报 ER 0115
            // 0.4.1 §2: increment/decrement also modifies the object, so a const
            // constant reports ER 0115
            if (operand.is_const) {
                diag_.report_error_template(expr->operand->location,
                    ErrorCode::ConstModification,
                    { expression_display_name(expr->operand.get()) });
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
            // 0.3 §18：同名重载集先做重载决议，再检查类型
            // 0.3 §18: resolve the same-scope overload set before type checking
            if (direct_primary != nullptr && !func_name.empty()) {
                std::vector<Symbol>* set = sym_table_.lookup_overloads(func_name);
                if (set != nullptr && set->size() > 1) {
                    Symbol* chosen = resolve_overload_call(func_name, expr, direct_primary);
                    if (chosen == nullptr) return AST::Type::make_void();
                    return chosen->type;
                }
            }
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
                        if (name == "int" || name == "lint" || name == "uint" ||
                            name == "luint" || name == "float" || name == "double" ||
                            name == "char" || name == "uchar" ||
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
            // T(args) 作为表达式：构造一个值临时对象（Gallt 0.3.txt §20）
            // T(args) as an expression constructs a value temporary (Gallt 0.3.txt §20)
            if (direct_primary != nullptr && struct_defs_.find(func_name) != struct_defs_.end()) {
                // T(args) 值临时对象：按实参类型解析构造函数重载（第 20 章，ER 0096）
                // T(args) value temporary: resolve the constructor overload set by argument
                // type (§20, ambiguity -> ER 0096)
                std::vector<AST::Type> arg_types;
                std::vector<bool> arg_is_null;
                for (auto& arg : expr->arguments) {
                    arg_types.push_back(check_expression(arg.get()));
                    bool is_null = false;
                    if (auto* prim = dynamic_cast<AST::PrimaryExpression*>(arg.get())) {
                        if (prim->kind == AST::PrimaryExpression::Kind::Null) is_null = true;
                    }
                    arg_is_null.push_back(is_null);
                }
                Symbol* ctor = resolve_constructor_overload(func_name, arg_types, arg_is_null,
                    expr->location);
                if (ctor != nullptr && ctor->function_node != nullptr) {
                    resolved_functions_[direct_primary] = ctor->function_node;
                    for (std::size_t i = 0; i < arg_types.size() &&
                        i + 1 < ctor->param_types.size(); ++i) {
                        // 第 0 个形参是 this 指针，实参从第 1 个形参开始对应
                        // Parameter 0 is `this`; arguments start at parameter 1
                        bool compatible = can_implicit_convert(arg_types[i],
                            ctor->param_types[i + 1]);
                        if (!compatible && arg_is_null[i] &&
                            ctor->param_types[i + 1].kind == TypeKind::Pointer) {
                            compatible = true;
                        }
                        if (!compatible) {
                            report_error(expr->arguments[i]->location,
                                ErrorCode::FunctionArgTypeMismatch,
                                "constructor argument " + std::to_string(i + 1) +
                                " type '" + arg_types[i].to_string() +
                                "' does not match parameter type '" +
                                ctor->param_types[i + 1].to_string() + "'");
                        }
                    }
                }
                return AST::Type::make_struct(func_name);
            }
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
            // 默认参数（Gallt 0.3.txt §8）：缺少的实参由被调函数定义补齐
            // Default arguments (Gallt 0.3.txt §8): missing arguments come from the callee
            std::vector<AST::Expression*> defaults;
            if (direct_primary != nullptr) {
                Symbol* callee_sym = sym_table_.lookup(direct_primary->identifier);
                if (callee_sym != nullptr && callee_sym->function_node != nullptr) {
                    for (auto& d : callee_sym->function_node->param_defaults) {
                        defaults.push_back(d.get());
                    }
                }
            }
            size_t required = expected;
            for (size_t i = defaults.size(); i > 0; --i) {
                if (defaults[i - 1] != nullptr) required = i - 1;
                else break;
            }
            if (provided > expected || provided < required) {
                report_error(expr->location, ErrorCode::FunctionArgCountMismatch,
                    "function expects " + std::to_string(expected) +
                    " arguments, but " + std::to_string(provided) + " provided");
                return AST::Type::make_void();
            }
            if (provided < expected) {
                for (size_t i = provided; i < expected; ++i) {
                    if (i < defaults.size() && defaults[i] != nullptr) {
                        expr->appended_defaults.push_back(defaults[i]);
                    }
                }
            }
            for (size_t i = 0; i < expected; ++i) {
                if (i < expr->arguments.size()) {
                    // 目标类型：形参类型（回调/函数指针实参的按类型选择重载，第 18 章）
                    // Expected type: the parameter type (§18 callback overload selection)
                    const AST::Type* saved_expected = expected_type_;
                    expected_type_ = &func_type.parameter_types[i];
                    AST::Type arg_type = check_expression(expr->arguments[i].get());
                    expected_type_ = saved_expected;
                    // 第 20 章：按值传递结构体形参要拷贝构造实参（[nocopy]/[nomove] 生效）
                    // §20: passing a struct by value copy-constructs it, so [nocopy]/[nomove]
                    // apply to by-value struct arguments
                    if (func_type.parameter_types[i].kind == TypeKind::Struct &&
                        arg_type.kind == TypeKind::Struct &&
                        arg_type.struct_name == func_type.parameter_types[i].struct_name) {
                        if (is_move_expression(expr->arguments[i].get())) {
                            if (!type_is_movable(func_type.parameter_types[i])) {
                                diag_.report_error_template(expr->arguments[i]->location,
                                    ErrorCode::NoMoveViolation,
                                    { func_type.parameter_types[i].struct_name });
                            }
                        }
                        else if (!type_is_copyable(func_type.parameter_types[i])) {
                            diag_.report_error_template(expr->arguments[i]->location,
                                ErrorCode::NoCopyViolation,
                                { func_type.parameter_types[i].struct_name });
                        }
                    }
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
            // 0.4.1 §2：后缀自增/自减修改 const 常量时报 ER 0115
            // 0.4.1 §2: postfix increment/decrement of a const constant reports ER 0115
            if (base_type.is_const) {
                diag_.report_error_template(expr->base->location,
                    ErrorCode::ConstModification,
                    { expression_display_name(expr->base.get()) });
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
            if (expr->member_name == "destructor") {
                return AST::Type::make_function(
                    std::make_shared<AST::Type>(AST::Type::make_void()),
                    std::vector<AST::Type>{});
            }
           if (!member) {
               report_error(expr->location, ErrorCode::StructMemberNotFound,
                   "struct has no member named '" + expr->member_name + "'");
                return AST::Type::make_void();
            }
            // 0.4.1 §2 + C++20：const 结构体对象的成员同样是常量
            // 0.4.1 §2 with the C++20 fallback: members of a const struct object are
            // const as well
            AST::Type member_type = member->type;
            if (base_type.is_const) {
                member_type.is_const = true;
            }
            return member_type;
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
            // 显式析构调用 [指针]->destructor()（Gallt 0.3.txt §20）
            // Explicit destructor invocation [ptr]->destructor() (Gallt 0.3.txt §20)
            if (expr->member_name == "destructor") {
                return AST::Type::make_function(
                    std::make_shared<AST::Type>(AST::Type::make_void()),
                    std::vector<AST::Type>{});
            }
           const auto* member = get_struct_member(*pointee, expr->member_name);
           if (!member) {
               report_error(expr->location, ErrorCode::StructMemberNotFound,
                   "struct has no member named '" + expr->member_name + "'");
                return AST::Type::make_void();
            }
            // 指向 const 结构体的指针：成员按常量处理
            // A pointer to a const struct yields const members
            AST::Type member_type = member->type;
            if (pointee->is_const) {
                member_type.is_const = true;
            }
            return member_type;
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
                // 0.4.1 §7：整数默认 int，l 后缀 → lint，u 后缀 → uint，
                // lu 后缀 → luint
                // 0.4.1 §7: an integer literal is int by default; the l suffix selects
                // lint, u selects uint and lu selects luint
                return AST::Type::integer_literal_type(expr->literal_token.lexeme);
            case TokenType::FloatLiteral:
                // 无 f/F 后缀的浮点字面量默认为 double（Gallt 0.4.1 §7）
                // A float literal defaults to double unless it carries f/F (0.4.1 §7)
                return AST::Type::float_literal_type(expr->literal_token.lexeme);
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
            // 第 18 章 / 第 13 章：函数名作为值出现在需要函数指针的上下文时，
            // 按目标类型选择重载版本
            // §18/§13: a function name used as a value in a function-pointer context selects
            // the overload matching the expected target type
            if (std::vector<Symbol>* set = sym_table_.lookup_overloads(expr->identifier)) {
                if (set->size() > 1 && expected_type_ != nullptr) {
                    Symbol* chosen = select_overload_by_target_type(*set, *expected_type_,
                        expr->identifier, expr->location);
                    if (chosen != nullptr) {
                        record_function_resolution(expr, *chosen);
                        return AST::Type::make_function(
                            std::make_shared<AST::Type>(chosen->type), chosen->param_types);
                    }
                }
            }
            // 首先检查是否为内置函数名（仅用于检查，不在此处处理）
            // 但为了符号表查找，先看符号表
            auto* sym = sym_table_.lookup(expr->identifier);
            if (sym) {
                if (sym->kind == SymbolKind::Function) {
                    // 函数名的表达式类型是完整函数类型，调用/取地址时使用
                    // A function-name expression has full function type
                    record_function_resolution(expr, *sym);
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
        case AST::PrimaryExpression::Kind::Construct:
        case AST::PrimaryExpression::Kind::PlacementConstruct: {
            // construct [类型]([实参]) [at [指针]]（Gallt 0.3.txt §20）
            // construct [type]([args]) [at [pointer]] (Gallt 0.3.txt §20)
           if (expr->construct_type.kind != TypeKind::Struct) {
                diag_.report_error_template(expr->location, ErrorCode::SpecialMemberOnNonStruct,
                    { expr->construct_type.to_string() });
                return AST::Type::make_void();
            }
            if (!is_complete_type(expr->construct_type)) {
                report_error(expr->location, ErrorCode::ExpressionSyntaxError,
                    "construct target type is incomplete");
                return AST::Type::make_void();
            }
            for (auto& arg : expr->construct_args) {
                check_expression(arg.get());
            }
            // 第 20 章：按实参类型解析构造函数重载（含默认参数；二义性 ER 0096），
            // 并把结果记录下来供代码生成使用
            // §20: resolve the constructor overload set by argument type (defaults included,
            // ambiguity -> ER 0096) and record it for codegen
            {
                std::vector<AST::Type> arg_types;
                std::vector<bool> arg_is_null;
                for (auto& arg : expr->construct_args) {
                    const AST::Type* cached = nullptr;
                    auto found = expression_types_.find(arg.get());
                    if (found != expression_types_.end()) cached = &found->second;
                    arg_types.push_back(cached != nullptr ? *cached : AST::Type::make_void());
                    bool is_null = false;
                    if (auto* prim = dynamic_cast<AST::PrimaryExpression*>(arg.get())) {
                        if (prim->kind == AST::PrimaryExpression::Kind::Null) is_null = true;
                    }
                    arg_is_null.push_back(is_null);
                }
                Symbol* ctor = resolve_constructor_overload(expr->construct_type.struct_name,
                    arg_types, arg_is_null, expr->location);
                if (ctor != nullptr && ctor->function_node != nullptr) {
                    resolved_functions_[expr] = ctor->function_node;
                    // 实参类型检查（ER 0012）
                    for (std::size_t i = 0; i < arg_types.size() &&
                        i + 1 < ctor->param_types.size(); ++i) {
                        // 第 0 个形参是 this 指针，实参从第 1 个形参开始对应
                        // Parameter 0 is `this`; arguments start at parameter 1
                        bool compatible = can_implicit_convert(arg_types[i],
                            ctor->param_types[i + 1]);
                        if (!compatible && arg_is_null[i] &&
                            ctor->param_types[i + 1].kind == TypeKind::Pointer) {
                            compatible = true;
                        }
                        if (!compatible) {
                            report_error(expr->construct_args[i]->location,
                                ErrorCode::FunctionArgTypeMismatch,
                                "constructor argument " + std::to_string(i + 1) +
                                " type '" + arg_types[i].to_string() +
                                "' does not match parameter type '" +
                                ctor->param_types[i + 1].to_string() + "'");
                        }
                    }
                }
            }
            if (expr->kind == AST::PrimaryExpression::Kind::PlacementConstruct) {
                AST::Type target_type = check_expression(expr->placement_target.get());
                bool invalid = false;
                if (target_type.kind != TypeKind::Pointer) {
                    invalid = true;
                }
                else if (auto* target_prim =
                    dynamic_cast<AST::PrimaryExpression*>(expr->placement_target.get())) {
                    if (target_prim->kind == AST::PrimaryExpression::Kind::Null) invalid = true;
                }
                // 第 20 章：目标内存必须足够大且对齐（ER 0094）；void* 无法静态判定时接受
                // §20: the target memory must be large enough and aligned (ER 0094); void*
                // cannot be checked statically and is accepted
                if (!invalid && target_type.pointee_type &&
                    target_type.pointee_type->kind != TypeKind::Void) {
                    std::size_t target_size = 0;
                    std::size_t target_align = 1;
                    std::size_t needed_size = 0;
                    std::size_t needed_align = 1;
                    if (type_layout(*target_type.pointee_type, target_size, target_align) &&
                        type_layout(expr->construct_type, needed_size, needed_align)) {
                        if (target_size < needed_size || target_align < needed_align) {
                            invalid = true;
                        }
                    }
                }
                if (invalid) {
                    diag_.report_error_template(expr->location,
                        ErrorCode::PlacementTargetInvalid, std::vector<std::string>{});
                }
            }
            return AST::Type::make_pointer(
                std::make_shared<AST::Type>(expr->construct_type));
        }
        case AST::PrimaryExpression::Kind::CopyMove: {
            // copy/move/deep_copy/shallow_copy（Gallt 0.3.txt §20）
            AST::Type operand = check_expression(expr->paren_expr.get());
            return operand;
        }
        case AST::PrimaryExpression::Kind::QualifiedName: {
           report_error(expr->location, ErrorCode::GenericUndefined,
                expr->generic_ref ? expr->generic_ref->generic_name : std::string());
            return AST::Type::make_void();
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
        // Gallt 0.4.txt §13/§14：null（void*）可初始化/赋值给函数指针类型
        // Gallt 0.4.txt §13/§14: null (void*) initializes/assigns function pointers
        // `fptr == null` / `fptr != null` 的比较在 check_comparison 中处理
        // Comparing a function pointer with null is handled in check_comparison
        if (from.kind == TypeKind::Pointer && to.kind == TypeKind::Function) {
            if (from.pointee_type && from.pointee_type->kind == TypeKind::Void) return true;
        }
        if (from.kind == TypeKind::Function && to.kind == TypeKind::Pointer) {
            if (to.pointee_type && to.pointee_type->kind == TypeKind::Void) return true;
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
        // 算术运算的结果是右值：const 限定不会传染到表达式类型上
        // The result of an arithmetic operation is an rvalue, so const does not
        // propagate into the expression type.
        auto without_const = [](AST::Type t) {
            t.is_const = false;
            return t;
        };
        if (left == right) return without_const(left);
        // 0.4.1 §7：提升优先级 char < uchar < int < uint < lint < luint < float < double；
        // 低精度自动提升为高精度，二者同档时沿用 0.4 的规则（取左值类型）。
        // 0.4.1 §7 promotion ladder: char < uchar < int < uint < lint < luint <
        // float < double. A lower-precision operand is promoted to the higher one; for
        // equal ranks the 0.4 rule (left operand wins) is preserved.
        const int rank_left = left.promotion_rank();
        const int rank_right = right.promotion_rank();
        return without_const((rank_left >= rank_right) ? left : right);
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
        if (type.kind == TypeKind::Int || type.kind == TypeKind::Lint ||
            type.kind == TypeKind::Uint || type.kind == TypeKind::Luint ||
            type.kind == TypeKind::Float ||
            type.kind == TypeKind::Double || type.kind == TypeKind::Char ||
            type.kind == TypeKind::Uchar ||
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

    // ============================================================================
    // 0.4.1 §2/§7：const 限定符语义
    // 0.4.1 §2/§7: const qualifier semantics
    // ============================================================================

    void TypeChecker::enter_scope() {
        // 符号表作用域与 const 常量取值表同步进出（0.4.1 §2）
        // The symbol-table scope and the const value table enter/leave together
        sym_table_.enter_scope();
        const_values_.emplace_back();
    }

    void TypeChecker::exit_scope() {
        sym_table_.exit_scope();
        if (!const_values_.empty()) {
            const_values_.pop_back();
        }
    }

    std::optional<long long> TypeChecker::evaluate_const_integer_expression(
        AST::Expression* expr) {
        if (expr == nullptr) return std::nullopt;
        ConstantEvaluationContext ctx;
        // 常量名解析：按作用域由内向外查找已记录的 const 常量（0.4.1 §2）
        // Constant-name resolution: search the recorded const constants from the
        // innermost scope outward (0.4.1 §2)
        ctx.lookup_constant = [this](const std::string& name, long long& int_value,
            double& float_value, bool& is_float) {
            for (auto it = const_values_.rbegin(); it != const_values_.rend(); ++it) {
                auto found = it->find(name);
                if (found != it->end()) {
                    int_value = found->second;
                    float_value = static_cast<double>(found->second);
                    is_float = false;
                    return true;
                }
            }
            return false;
        };
        long long int_value = 0;
        double float_value = 0.0;
        bool is_float = false;
        if (!evaluate_constant_expression(expr, int_value, float_value, is_float, ctx)) {
            return std::nullopt;
        }
        if (is_float) {
            const auto truncated = static_cast<long long>(float_value);
            if (static_cast<double>(truncated) != float_value) return std::nullopt;
            int_value = truncated;
        }
        if (int_value < 0) return std::nullopt;
        return int_value;
    }

    void TypeChecker::record_const_value(const std::string& name, AST::Expression* initializer,
        const AST::Type& type) {
        // 只有整数/字符/布尔常量可用于常量整数表达式
        // Only integer, character and boolean constants take part in constant integer
        // expressions
        if (!type.is_integer()) return;
        auto value = evaluate_const_integer_expression(initializer);
        if (!value.has_value()) return;
        if (const_values_.empty()) {
            const_values_.emplace_back();
        }
        const_values_.back()[name] = *value;
    }

    std::string TypeChecker::expression_display_name(const AST::Expression* expr) {
        using namespace AST;
        if (expr == nullptr) return "<expression>";
        if (auto* prim = dynamic_cast<const PrimaryExpression*>(expr)) {
            switch (prim->kind) {
            case PrimaryExpression::Kind::Identifier:
                return prim->identifier;
            case PrimaryExpression::Kind::Parens:
                return expression_display_name(prim->paren_expr.get());
            default:
                return "<expression>";
            }
        }
        if (auto* post = dynamic_cast<const PostfixExpression*>(expr)) {
            switch (post->op) {
            case PostfixExpression::Operator::Subscript:
            case PostfixExpression::Operator::Increment:
            case PostfixExpression::Operator::Decrement:
                return expression_display_name(post->base.get());
            case PostfixExpression::Operator::Dot:
            case PostfixExpression::Operator::Arrow:
                if (!post->member_name.empty()) return post->member_name;
                return expression_display_name(post->base.get());
            default:
                return "<expression>";
            }
        }
        if (auto* un = dynamic_cast<const UnaryExpression*>(expr)) {
            if (un->op == UnaryExpression::Operator::Dereference ||
                un->op == UnaryExpression::Operator::Increment ||
                un->op == UnaryExpression::Operator::Decrement) {
                return expression_display_name(un->operand.get());
            }
        }
        return "<expression>";
    }

    bool TypeChecker::is_compile_time_constant_expression(AST::Expression* expr) {
        using namespace AST;
        if (expr == nullptr) return false;

        // 字面量（整数、浮点、字符、字符串、布尔）都是编译期常量（0.4.1 §19）
        // Literals (integer, float, char, string, bool) are compile-time constants (§19)
        if (auto* prim = dynamic_cast<PrimaryExpression*>(expr)) {
            switch (prim->kind) {
            case PrimaryExpression::Kind::Literal:
                return true;
            case PrimaryExpression::Kind::Parens:
                return is_compile_time_constant_expression(prim->paren_expr.get());
            case PrimaryExpression::Kind::Identifier: {
                // 只允许引用其它 const 常量；普通变量不是编译期常量（ER 0116）
                // Only other const constants may be referenced; a plain variable is
                // not a compile-time constant (ER 0116)
                Symbol* sym = lookup_symbol(prim->identifier, false);
                return sym != nullptr && sym->type.is_const;
            }
            default:
                return false;
            }
        }

        if (auto* un = dynamic_cast<UnaryExpression*>(expr)) {
            switch (un->op) {
            case UnaryExpression::Operator::UnaryPlus:
            case UnaryExpression::Operator::UnaryMinus:
            case UnaryExpression::Operator::LogicalNot:
                return is_compile_time_constant_expression(un->operand.get());
            default:
                // 取地址、解引用、自增、自减不允许出现在编译期常量表达式中
                // Address-of, dereference and increment/decrement are not allowed
                return false;
            }
        }
        if (auto* e = dynamic_cast<AdditiveExpression*>(expr)) {
            return is_compile_time_constant_expression(e->left.get()) &&
                is_compile_time_constant_expression(e->right.get());
        }
        if (auto* e = dynamic_cast<MultiplicativeExpression*>(expr)) {
            return is_compile_time_constant_expression(e->left.get()) &&
                is_compile_time_constant_expression(e->right.get());
        }
        if (auto* e = dynamic_cast<PowerExpression*>(expr)) {
            return is_compile_time_constant_expression(e->left.get()) &&
                is_compile_time_constant_expression(e->right.get());
        }
        if (auto* e = dynamic_cast<ComparisonExpression*>(expr)) {
            return is_compile_time_constant_expression(e->left.get()) &&
                is_compile_time_constant_expression(e->right.get());
        }
        if (auto* e = dynamic_cast<LogicalAndExpression*>(expr)) {
            return is_compile_time_constant_expression(e->left.get()) &&
                is_compile_time_constant_expression(e->right.get());
        }
        if (auto* e = dynamic_cast<LogicalOrExpression*>(expr)) {
            return is_compile_time_constant_expression(e->left.get()) &&
                is_compile_time_constant_expression(e->right.get());
        }
        if (auto* post = dynamic_cast<PostfixExpression*>(expr)) {
            switch (post->op) {
            case PostfixExpression::Operator::Cast:
                // 类型转换：转换结果仍须是编译期常量（0.4.1 §19）
                // A cast keeps the result constant only when the operand is constant
                return is_compile_time_constant_expression(post->base.get()) &&
                    (post->cast_type.is_arithmetic() ||
                        post->cast_type.kind == TypeKind::String);
            case PostfixExpression::Operator::FunctionCall: {
                // 编译期内建函数 size / align 在参数可确定时是编译期常量（§19）
                // The compile-time builtins size/align are constant when the argument
                // is determinable (§19)
                auto* callee = dynamic_cast<PrimaryExpression*>(post->base.get());
                if (callee == nullptr ||
                    callee->kind != PrimaryExpression::Kind::Identifier) {
                    return false;
                }
                if (callee->identifier != "size" && callee->identifier != "align") {
                    return false;
                }
                return post->arguments.size() == 1 &&
                    is_compile_time_constant_expression(post->arguments[0].get());
            }
            default:
                // 下标、成员访问、自增自减不是编译期常量表达式
                // Subscript, member access and increment/decrement are not
                return false;
            }
        }
        // 赋值、构造、拷贝/移动等含副作用，均不是编译期常量表达式
        // Assignment, construction and copy/move carry side effects and are not
        // compile-time constant expressions
        return false;
    }

    void TypeChecker::check_const_declaration(AST::VariableDeclaration* decl) {
        const std::string& name = decl->name;
        // 没有初始化器 → 常量无法在编译期求值（ER 0116）
        // Without an initializer the constant cannot be evaluated at compile time
        if (decl->initializer == nullptr) {
            diag_.report_error_template(decl->location,
                ErrorCode::ConstCannotStoreVariable, { name });
            return;
        }
        // 记录可在常量整数表达式（如数组长度）中使用的常量取值（0.4.1 §2/§7）
        // Record the value of a constant so later constant integer expressions (array
        // lengths) can use it (0.4.1 §2/§7)
        if (auto* expr_init = dynamic_cast<AST::ExpressionInitializer*>(decl->initializer.get())) {
            record_const_value(name, expr_init->expr.get(), decl->type);
        }
        // 递归校验初始化器中的每个表达式都是编译期常量表达式
        // Validate every initializer expression recursively
        std::function<void(AST::Initializer*)> check_initializer =
            [&](AST::Initializer* init) {
                if (init == nullptr) return;
                if (auto* expr_init = dynamic_cast<AST::ExpressionInitializer*>(init)) {
                    if (!is_compile_time_constant_expression(expr_init->expr.get())) {
                        diag_.report_error_template(init->location,
                            ErrorCode::ConstCannotStoreVariable, { name });
                    }
                    return;
                }
                if (auto* arr_init = dynamic_cast<AST::ArrayInitializer*>(init)) {
                    for (auto& element : arr_init->elements) {
                        check_initializer(element.get());
                    }
                }
            };
        check_initializer(decl->initializer.get());
    }

    // ============================================================================
    // 0.3 §18：函数重载决议
    // 0.3 §18: function overload resolution
    // ============================================================================

    void TypeChecker::mangle_overload_set(const std::string& name) {
        // 第 18 章：命名修饰至少包含参数类型签名，且跨作用域、泛型实例化与外部链接唯一
        // §18: mangling carries the full parameter-type signature and is unique across
        // scopes, generic instantiations and external linkage
        std::vector<Symbol>* set = sym_table_.lookup_overloads(name);
        if (set == nullptr) return;
        for (Symbol& sym : *set) {
            const std::string base = name + "$" + overload_signature(sym.param_types);
            if (sym.function_node != nullptr) {
                auto it = mangled_functions_.find(sym.function_node);
                if (it == mangled_functions_.end()) {
                    it = mangled_functions_
                        .emplace(sym.function_node, unique_mangled_name(base)).first;
                }
                sym.function_node->name = it->second;
            }
            else if (sym.extern_node != nullptr) {
                auto it = mangled_externs_.find(sym.extern_node);
                if (it == mangled_externs_.end()) {
                    it = mangled_externs_
                        .emplace(sym.extern_node, unique_mangled_name(base)).first;
                }
                sym.extern_node->name = it->second;
            }
        }
    }

    namespace {
        // 把类型名/文本转换为 LLVM 标识符安全的字符序列
        // Turn a type name / text into LLVM-identifier-safe characters
        std::string sanitize_identifier(std::string_view text) {
            std::string out;
            out.reserve(text.size());
            for (char c : text) {
                switch (c) {
                case '<': out += 'L'; break;
                case '>': out += 'G'; break;
                case '*': out += 'P'; break;
                case '[': out += 'A'; break;
                case ']': out += 'Z'; break;
                case ',': out += 'C'; break;
                case '.': out += 'D'; break;
                case ' ': break;
                default: out += c; break;
                }
            }
            return out;
        }
    }

    std::string TypeChecker::overload_signature(const std::vector<AST::Type>& params) const {
        // 参数类型签名：结构性编码，保证“相同类型列表 → 相同签名”
        // Structural encoding of the parameter list: identical lists map to identical text
        std::function<std::string(const AST::Type&)> encode = [&](const AST::Type& t) -> std::string {
            switch (t.kind) {
            case TypeKind::Int: return "i";
            case TypeKind::Lint: return "l";
            case TypeKind::Uint: return "u";
            case TypeKind::Luint: return "q";
            case TypeKind::Float: return "f";
            case TypeKind::Double: return "d";
            case TypeKind::Char: return "c";
            case TypeKind::Uchar: return "h";
            case TypeKind::Bool: return "b";
            case TypeKind::String: return "s";
            case TypeKind::File: return "F";
            case TypeKind::Void: return "v";
            case TypeKind::Pointer:
                return "P" + (t.pointee_type ? encode(*t.pointee_type) : std::string("v"));
            case TypeKind::Array:
                return "A" + (t.array_size.has_value() ? std::to_string(*t.array_size) : "u") +
                    (t.element_type ? encode(*t.element_type) : std::string("v"));
            case TypeKind::Struct: return "S" + sanitize_identifier(t.struct_name);
            case TypeKind::Function: {
                std::string out = "R" + (t.return_type ? encode(*t.return_type) : std::string("v"));
                for (const AST::Type& p : t.parameter_types) out += "_" + encode(p);
                return out;
            }
            }
            return "x";
        };
        if (params.empty()) return "void";
        std::string out;
        for (const AST::Type& p : params) {
            if (!out.empty()) out += "_";
            out += encode(p);
        }
        return out;
    }

    std::string TypeChecker::unique_mangled_name(const std::string& base) {
        // 冲突时追加序号，保证整个程序内唯一（跨作用域/泛型实例化）
        // Append an index on collision so the name is unique program-wide
        std::string candidate = base;
        int suffix = 2;
        while (!used_mangled_names_.insert(candidate).second) {
            candidate = base + "$" + std::to_string(suffix++);
        }
        return candidate;
    }

    void TypeChecker::record_function_resolution(AST::PrimaryExpression* callee,
        const Symbol& symbol) {
        if (callee == nullptr) return;
        if (symbol.function_node != nullptr) {
            resolved_functions_[callee] = symbol.function_node;
        }
        else if (symbol.extern_node != nullptr) {
            resolved_externs_[callee] = symbol.extern_node;
        }
    }

    Symbol* TypeChecker::select_overload_by_target_type(std::vector<Symbol>& set,
        const AST::Type& target, const std::string& name, SourceLocation loc) {
        // 目标类型可能是函数类型或函数指针类型（第 13 章）
        // The target may be a function type or a pointer to a function type (§13)
        AST::Type target_function = target;
        if (target.kind == TypeKind::Pointer && target.pointee_type &&
            target.pointee_type->kind == TypeKind::Function) {
            target_function = *target.pointee_type;
        }
        if (target_function.kind != TypeKind::Function) return nullptr;
        Symbol* best = nullptr;
        for (Symbol& sym : set) {
            if (sym.param_types.size() != target_function.parameter_types.size()) continue;
            if (!(sym.type == *target_function.return_type)) continue;
            bool same = true;
            for (std::size_t i = 0; i < sym.param_types.size(); ++i) {
                if (!(sym.param_types[i] == target_function.parameter_types[i])) {
                    same = false;
                    break;
                }
            }
            if (!same) continue;
            if (best != nullptr) {
                report_error(loc, ErrorCode::OverloadAmbiguous, { name });
                return nullptr;
            }
            best = &sym;
        }
        if (best == nullptr) {
            // 没有签名完全一致的重载 → 函数指针类型不匹配（ER 0043）
            // No overload matches the target signature → function-pointer mismatch
            report_error(loc, ErrorCode::FuncPtrTypeMismatch,
                "no overload of '" + name + "' matches the target function pointer type '" +
                target_function.to_string() + "'");
        }
        return best;
    }

    bool TypeChecker::overloads_ambiguous_by_defaults(const Symbol& a, const Symbol& b) const {
        // 两个重载在某实参个数上都被调用（含默认参数），且该个数范围内所有已提供位置的
        // 形参类型完全相同 → 该调用必然二义（第 18 章）
        // When both overloads accept some argument count (through defaults) and every
        // provided position has identical parameter types, that call is inevitably ambiguous
        auto required_arity = [](const Symbol& sym) -> std::size_t {
            std::size_t required = sym.param_types.size();
            if (sym.function_node != nullptr) {
                const auto& defaults = sym.function_node->param_defaults;
                for (std::size_t i = defaults.size(); i > 0; --i) {
                    if (defaults[i - 1] != nullptr) required = i - 1;
                    else break;
                }
            }
            return required;
        };
        const std::size_t a_min = required_arity(a);
        const std::size_t b_min = required_arity(b);
        const std::size_t a_max = a.param_types.size();
        const std::size_t b_max = b.param_types.size();
        const std::size_t lo = std::max(a_min, b_min);
        const std::size_t hi = std::min(a_max, b_max);
        for (std::size_t arity = lo; arity <= hi; ++arity) {
            bool identical = true;
            for (std::size_t i = 0; i < arity; ++i) {
                if (!(a.param_types[i] == b.param_types[i])) {
                    identical = false;
                    break;
                }
            }
            if (identical) return true;
        }
        return false;
    }

    Symbol* TypeChecker::resolve_constructor_overload(const std::string& struct_name,
        const std::vector<AST::Type>& arg_types, const std::vector<bool>& arg_is_null,
        SourceLocation loc) {
        // 第 20 章：构造函数降低为同名重载集，这里按实参类型做重载决议；
        // 与普通重载一致地使用转换等级偏序与默认参数，二义性报 ER 0096
        // §20: constructors form an overload set; resolve by argument type with the same
        // conversion ordering and default arguments; ambiguity is ER 0096
        std::vector<Symbol>* set = sym_table_.lookup_overloads("__sgc_ctor$" + struct_name);
        if (set == nullptr || set->empty()) {
            if (!arg_types.empty()) {
                report_error(loc, ErrorCode::FunctionArgCountMismatch,
                    "type '" + struct_name + "' has no constructor taking " +
                    std::to_string(arg_types.size()) + " argument(s)");
            }
            return nullptr;
        }
        struct Candidate {
            Symbol* symbol = nullptr;
            std::vector<int> ranks;
        };
        std::vector<Candidate> viable;
        for (Symbol& sym : *set) {
            // 降低后的构造函数第一个形参是 `this` 指针，不参与实参匹配（第 20 章）
            // The lowered constructor's first parameter is the hidden `this` pointer and does
            // not take part in argument matching (§20)
            if (sym.param_types.empty()) continue;
            const std::size_t max_params = sym.param_types.size() - 1;
            std::size_t required = max_params;
            if (sym.function_node != nullptr) {
                const auto& defaults = sym.function_node->param_defaults;
                for (std::size_t i = defaults.size(); i > 1; --i) {
                    if (defaults[i - 1] != nullptr) required = (i - 1) - 1;
                    else break;
                }
            }
            if (arg_types.size() > max_params || arg_types.size() < required) continue;
            Candidate candidate;
            candidate.symbol = &sym;
            bool ok = true;
            for (std::size_t i = 0; i < arg_types.size(); ++i) {
                int rank = conversion_rank(arg_types[i], sym.param_types[i + 1]);
                if (rank < 0 && i < arg_is_null.size() && arg_is_null[i] &&
                    sym.param_types[i + 1].kind == TypeKind::Pointer) {
                    rank = 0;
                }
                if (rank < 0) {
                    ok = false;
                    break;
                }
                candidate.ranks.push_back(rank);
            }
            if (ok) viable.push_back(std::move(candidate));
        }
        if (viable.empty()) {
            report_error(loc, ErrorCode::FunctionArgTypeMismatch,
                "no constructor of '" + struct_name + "' matches the given arguments");
            return nullptr;
        }
        int best = -1;
        bool ambiguous = false;
        for (std::size_t i = 0; i < viable.size(); ++i) {
            bool is_best = true;
            for (std::size_t j = 0; j < viable.size(); ++j) {
                if (i == j) continue;
                const std::vector<int>& a = viable[i].ranks;
                const std::vector<int>& b = viable[j].ranks;
                bool i_better = false;
                bool j_better = false;
                for (std::size_t k = 0; k < a.size() && k < b.size(); ++k) {
                    if (a[k] < b[k]) i_better = true;
                    if (a[k] > b[k]) j_better = true;
                }
                if (!(i_better && !j_better)) {
                    is_best = false;
                    break;
                }
            }
            if (is_best) {
                if (best != -1) ambiguous = true;
                else best = static_cast<int>(i);
            }
        }
        if (ambiguous || best < 0) {
            // 第 20 章：特殊成员重载决议二义性 → ER 0096
            // §20: special-member overload ambiguity is ER 0096
            report_error(loc, ErrorCode::SpecialMemberAmbiguous, { struct_name });
            return nullptr;
        }
        return viable[static_cast<std::size_t>(best)].symbol;
    }

    bool TypeChecker::type_layout(const AST::Type& type, std::size_t& size,
        std::size_t& align) const {
        // 与代码生成一致的大小/对齐计算（第 14 章；用于 Placement 构造检查）
        // Size/alignment identical to codegen (§14; used by the placement check)
        auto round_up = [](std::size_t value, std::size_t alignment) {
            if (alignment <= 1) return value;
            return (value + alignment - 1) / alignment * alignment;
        };
        switch (type.kind) {
        // 0.4.1 §2：int/uint 4 字节、lint/luint/double 8 字节、
        // char/uchar/bool 1 字节；对齐与占用一致
        // 0.4.1 §2: int/uint are 4 bytes, lint/luint/double 8 bytes, char/uchar/bool
        // one byte; alignment equals the size for these scalar kinds
        case TypeKind::Int: case TypeKind::Uint:
        case TypeKind::Float: size = 4; align = 4; return true;
        case TypeKind::Lint: case TypeKind::Luint:
        case TypeKind::Double: size = 8; align = 8; return true;
        case TypeKind::Char: case TypeKind::Uchar:
        case TypeKind::Bool: size = 1; align = 1; return true;
        case TypeKind::String: size = 32; align = 8; return true;
        case TypeKind::File: size = 8; align = 8; return true;
        case TypeKind::Pointer:
        case TypeKind::Function: size = 8; align = 8; return true;
        case TypeKind::Array: {
            std::size_t element_size = 0;
            std::size_t element_align = 1;
            if (!type.element_type || !type_layout(*type.element_type, element_size, element_align)) {
                return false;
            }
            size = element_size * type.array_size.value_or(0);
            align = element_align;
            return true;
        }
        case TypeKind::Struct: {
            auto def = struct_defs_.find(type.struct_name);
            if (def == struct_defs_.end() || def->second == nullptr) return false;
            std::size_t offset = 0;
            std::size_t max_align = 1;
            for (const auto& member : def->second->members) {
                std::size_t member_size = 0;
                std::size_t member_align = 1;
                if (!type_layout(member.type, member_size, member_align)) return false;
                max_align = std::max(max_align, member_align);
                offset = round_up(offset, member_align);
                offset += member_size;
            }
            size = round_up(offset, max_align);
            align = max_align;
            return true;
        }
        default:
            return false;
        }
    }

    bool TypeChecker::type_is_copyable(const AST::Type& type) const {
        // 第 20 章：未标记 [nocopy] 且所有成员可拷贝时，默认拷贝构造/拷贝赋值才生成
        // §20: the default copy members exist unless [nocopy] or a member is not copyable
        switch (type.kind) {
        case TypeKind::Array:
            return type.element_type ? type_is_copyable(*type.element_type) : true;
        case TypeKind::Struct: {
            auto def = struct_defs_.find(type.struct_name);
            if (def == struct_defs_.end() || def->second == nullptr) return true;
            if (def->second->no_copy) return false;
            for (const auto& member : def->second->members) {
                if (!type_is_copyable(member.type)) return false;
            }
            return true;
        }
        default:
            return true;
        }
    }

    bool TypeChecker::type_is_movable(const AST::Type& type) const {
        switch (type.kind) {
        case TypeKind::Array:
            return type.element_type ? type_is_movable(*type.element_type) : true;
        case TypeKind::Struct: {
            auto def = struct_defs_.find(type.struct_name);
            if (def == struct_defs_.end() || def->second == nullptr) return true;
            if (def->second->no_move) return false;
            for (const auto& member : def->second->members) {
                if (!type_is_movable(member.type)) return false;
            }
            return true;
        }
        default:
            return true;
        }
    }

    bool TypeChecker::is_move_expression(const AST::Expression* expr) {
        auto* cm = dynamic_cast<const AST::PrimaryExpression*>(expr);
        return cm != nullptr && cm->kind == AST::PrimaryExpression::Kind::CopyMove &&
            cm->copy_move_kind == AST::PrimaryExpression::CopyMoveKind::Move;
    }

    int TypeChecker::conversion_rank(const AST::Type& from, const AST::Type& to) {
        // 第 18 章：精确匹配优于隐式转换；转换后更接近形参类型者更优。
        // 采用两级排序：等级（0 精确 / 1 提升 / 2 转换）× 100 + 转换距离，
        // 值越小越优，因此既满足“精确优先”，也满足“更接近形参类型者更优”。
        // §18: exact matches beat implicit conversions, and a conversion landing closer to
        // the parameter type wins. The rank is (class * 100 + distance), smaller is better.
        constexpr int kExact = 0;
        constexpr int kPromotion = 100;
        constexpr int kConversion = 200;
        // 算术链位置：0.4.1 §7 的提升优先级 char < uchar < int < uint < lint < luint <
        // float < double；bool 不在文档链上，按 C++20 与 char 同档。
        // Position on the arithmetic ladder per 0.4.1 §7: char < uchar < int < uint <
        // lint < luint < float < double. bool is not on the documented ladder and
        // shares char's slot per C++20.
        auto arithmetic_position = [](const AST::Type& t) -> int {
            switch (t.kind) {
            case TypeKind::Char:
            case TypeKind::Bool:
                return 0;
            case TypeKind::Uchar: return 1;
            case TypeKind::Int: return 2;
            case TypeKind::Uint: return 3;
            case TypeKind::Lint: return 4;
            case TypeKind::Luint: return 5;
            case TypeKind::Float: return 6;
            case TypeKind::Double: return 7;
            default: return -1;
            }
        };
        if (from == to) return 0;
        if (from.kind == TypeKind::Pointer && to.kind == TypeKind::Pointer) {
            if (from.pointee_type && to.pointee_type && *from.pointee_type == *to.pointee_type) {
                return kExact;
            }
            // 指针转换：void* 视为一次转换，其它同为一档
            // Pointer conversions: void* is one conversion; others share the same class
            return kConversion;
        }
        const int from_pos = arithmetic_position(from);
        const int to_pos = arithmetic_position(to);
        if (from_pos >= 0 && to_pos >= 0) {
            // 提升按 C++20 的整数提升/浮点提升定义（文档 §18 只要求“精确匹配优于
            // 隐式转换”，未细化提升分类）：
            //   - bool/char/uchar → int/uint 为整数提升
            //   - float → double 为浮点提升
            // 其余均为隐式转换（含截断），与 0.4 的判定保持一致。
            // Promotions follow C++20 integer/floating promotions (§18 only requires
            // "exact match beats implicit conversion"):
            //   - bool/char/uchar -> int/uint is an integral promotion
            //   - float -> double is a floating promotion
            // Everything else is a conversion (including truncation), as in 0.4.
            const bool small_integer = from.kind == TypeKind::Bool ||
                from.kind == TypeKind::Char || from.kind == TypeKind::Uchar;
            const bool integral_promotion = small_integer &&
                (to.kind == TypeKind::Int || to.kind == TypeKind::Uint);
            const bool floating_promotion = from.kind == TypeKind::Float &&
                to.kind == TypeKind::Double;
            const bool promotion = integral_promotion || floating_promotion;
            int distance = std::abs(to_pos - from_pos);
            if (distance == 0) distance = 1;   // char ↔ bool 等同档不同类型
            return (promotion ? kPromotion : kConversion) + distance;
        }
        if (can_implicit_convert(from, to)) return kConversion;
        return -1;
    }

    Symbol* TypeChecker::resolve_overload_call(const std::string& name,
        AST::PostfixExpression* call, AST::PrimaryExpression* callee) {
        std::vector<Symbol>* set = sym_table_.lookup_overloads(name);
        if (set == nullptr || set->empty()) return nullptr;

        // 实参类型只求值一次，避免重复诊断
        // Argument types are computed once, avoiding duplicate diagnostics
        std::vector<AST::Type> arg_types;
        std::vector<bool> arg_is_null;
        for (auto& arg : call->arguments) {
            arg_types.push_back(check_expression(arg.get()));
            bool is_null = false;
            if (auto* prim = dynamic_cast<AST::PrimaryExpression*>(arg.get())) {
                if (prim->kind == AST::PrimaryExpression::Kind::Null) is_null = true;
            }
            arg_is_null.push_back(is_null);
        }
        const std::size_t provided = call->arguments.size();

        struct Candidate {
            Symbol* sym = nullptr;
            std::vector<int> ranks;
        };
        std::vector<Candidate> viable;
        for (Symbol& sym : *set) {
            const std::size_t max_params = sym.param_types.size();
            std::size_t required = max_params;
            if (sym.function_node != nullptr) {
                const auto& defaults = sym.function_node->param_defaults;
                for (std::size_t i = defaults.size(); i > 0; --i) {
                    if (defaults[i - 1]) {
                        required = i - 1;
                    }
                    else {
                        break;
                    }
                }
            }
            if (provided > max_params || provided < required) continue;
            Candidate candidate;
            candidate.sym = &sym;
            bool ok = true;
            for (std::size_t i = 0; i < provided; ++i) {
                int rank = conversion_rank(arg_types[i], sym.param_types[i]);
                if (rank < 0 && arg_is_null[i] && sym.param_types[i].kind == TypeKind::Pointer) {
                    rank = 0;
                }
                if (rank < 0) {
                    ok = false;
                    break;
                }
                candidate.ranks.push_back(rank);
            }
            if (ok) viable.push_back(std::move(candidate));
        }

        if (viable.empty()) {
            report_error(call->location, ErrorCode::FunctionArgTypeMismatch,
                "no viable overload of '" + name + "' for the given arguments");
            return nullptr;
        }

        int best = -1;
        bool ambiguous = false;
        for (std::size_t i = 0; i < viable.size(); ++i) {
            bool is_best = true;
            for (std::size_t j = 0; j < viable.size(); ++j) {
                if (i == j) continue;
                const std::vector<int>& a = viable[i].ranks;
                const std::vector<int>& b = viable[j].ranks;
                bool i_better = false;
                bool j_better = false;
                for (std::size_t k = 0; k < a.size() && k < b.size(); ++k) {
                    if (a[k] < b[k]) i_better = true;
                    if (a[k] > b[k]) j_better = true;
                }
                if (!(i_better && !j_better)) {
                    is_best = false;
                    break;
                }
            }
            if (is_best) {
                if (best != -1) ambiguous = true;
                else best = static_cast<int>(i);
            }
        }
        if (ambiguous || best < 0) {
            report_error(call->location, ErrorCode::OverloadAmbiguous, { name });
            return nullptr;
        }

        Symbol* chosen = viable[static_cast<std::size_t>(best)].sym;
        if (callee != nullptr) {
            // 第 18 章：extern 与用户函数共用重载集，命名修饰后的名字分别取自
            // ExternDeclaration::name / FunctionDefinition::name
            // §18: externs and user functions share overload sets; the mangled name comes
            // from ExternDeclaration::name or FunctionDefinition::name respectively
            std::string resolved_name;
            if (chosen->function_node != nullptr) {
                resolved_name = chosen->function_node->name;
            }
            else if (chosen->extern_node != nullptr) {
                resolved_name = chosen->extern_node->name;
            }
            if (!resolved_name.empty()) callee->identifier = resolved_name;
            record_function_resolution(callee, *chosen);
        }
        // 默认参数补齐（记录指向被调函数定义中默认值表达式的非拥有指针）
        // Fill in default arguments (non-owning pointers into the callee's defaults)
        if (chosen->function_node != nullptr && provided < chosen->param_types.size()) {
            const auto& defaults = chosen->function_node->param_defaults;
            for (std::size_t i = provided; i < chosen->param_types.size(); ++i) {
                if (i < defaults.size() && defaults[i]) {
                    call->appended_defaults.push_back(defaults[i].get());
                }
            }
        }
        return chosen;
    }

} // namespace gallt
