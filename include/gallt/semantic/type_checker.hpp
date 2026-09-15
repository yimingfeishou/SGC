// semantic/type_checker.hpp
// 类型检查器 —— 执行语义分析、类型推导、符号解析、错误报告
// Type Checker — performs semantic analysis, type inference, symbol resolution, and error reporting

#ifndef GALLT_SEMANTIC_TYPE_CHECKER_HPP
#define GALLT_SEMANTIC_TYPE_CHECKER_HPP

#include "symbol_table.hpp"
#include "../common/diagnostics.hpp"
#include "../parser/ast.hpp"
#include <memory>
#include <vector>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace gallt {

    // ============================================================================
    // 类型检查器 (TypeChecker)
    // 负责：
    //   - 构建符号表（全局、函数、块作用域）
    //   - 检查类型一致性
    //   - 隐式类型转换（int < float < double）
    //   - 函数签名验证
    //   - 主函数验证
    //   - 控制流检查（break 必须在循环内）
    //   - 报告所有语义错误（ER 0001 ~ ER 0059）
    // ============================================================================

    class TypeChecker {
    public:
        // ---- 构造与析构 ----
        TypeChecker(DiagnosticEngine& diag);
        ~TypeChecker() = default;

        // 禁止拷贝
        TypeChecker(const TypeChecker&) = delete;
        TypeChecker& operator=(const TypeChecker&) = delete;

        // ---- 主接口 ----
        // 检查整个程序，返回 true 表示无错误，false 表示有错误
        bool check_program(AST::Program* program);

        // 检查是否发生错误（可用于驱动编译流程）
        bool has_errors() const { return diag_.has_errors(); }

        // 获取符号表（用于代码生成阶段）
        SymbolTable& get_symbol_table() { return sym_table_; }

        // 获取表达式 -> 已推导类型的映射（供代码生成阶段复用语义结果）
        // Expression-to-derived-type map, reused by code generation
        const std::unordered_map<const AST::Expression*, AST::Type>& expression_types() const {
            return expression_types_;
        }

        // 函数名解析结果（第 18 章）：标识符 → 具体函数 / extern 声明。
        // 代码生成阶段据此取“当前”名字，避免重载集之后被命名修饰而使调用点失效。
        // Function-name resolutions (§18): identifier -> concrete function/extern. Codegen
        // reads the current name through these, so later mangling cannot invalidate a call
        // site that was resolved earlier.
        const std::unordered_map<const AST::PrimaryExpression*, const AST::FunctionDefinition*>&
            resolved_functions() const { return resolved_functions_; }
        const std::unordered_map<const AST::PrimaryExpression*, const AST::ExternDeclaration*>&
            resolved_externs() const { return resolved_externs_; }

    private:
        // ---- 成员变量 ----
        DiagnosticEngine& diag_;
        SymbolTable sym_table_;
        AST::Program* program_ = nullptr;

        // 当前正在检查的函数（用于 return 检查）
        AST::FunctionDefinition* current_function_ = nullptr;

        // 当前循环深度（用于 break 检查）
        int loop_depth_ = 0;

        // 内置函数是否已在本实例的符号表中注册
        // Whether the builtin functions were declared in this instance's symbol table
        bool builtins_declared_ = false;

        // 记录已定义的结构体（用于成员查找）
        std::unordered_map<std::string, AST::StructDefinition*> struct_defs_;

        // 表达式类型缓存：每个检查过的表达式记录其最终类型
        // Cache of the final type of every checked expression
        std::unordered_map<const AST::Expression*, AST::Type> expression_types_;

        // 函数名解析与命名修饰缓存（键为 AST 节点指针，避免容器重分配导致失效）
        // Function-name resolutions and mangling caches (keyed by AST node pointers)
        std::unordered_map<const AST::PrimaryExpression*, const AST::FunctionDefinition*>
            resolved_functions_;
        std::unordered_map<const AST::PrimaryExpression*, const AST::ExternDeclaration*>
            resolved_externs_;
        std::unordered_map<const AST::FunctionDefinition*, std::string> mangled_functions_;
        std::unordered_map<const AST::ExternDeclaration*, std::string> mangled_externs_;
        std::unordered_set<std::string> used_mangled_names_;
        // 当前表达式所期望的目标类型（按函数指针类型选择重载时使用）
        // Expected target type of the expression currently being checked
        const AST::Type* expected_type_ = nullptr;

        // ---- 顶层检查 ----
        void check_top_level(AST::TopLevel* node);
        void check_guide_statement(AST::GuideStatement* node);
        void check_clib_statement(AST::ClibStatement* node);
        void check_extern_declaration(AST::ExternDeclaration* node);
        void check_function_definition(AST::FunctionDefinition* node);
        void check_struct_definition(AST::StructDefinition* node);

        // ---- 语句检查 ----
        void check_statement(AST::Statement* stmt);
        void check_block(AST::Block* block);
        void check_variable_declaration(AST::VariableDeclaration* decl);
        // 逐维校验（多维）数组的花括号初始化列表（第 7 章 / ER 0015 / ER 0016）
        // Validate the brace initializer of a (multi-dimensional) array dimension by dimension
        void check_array_initializer(AST::ArrayInitializer* init, const AST::Type& array_type,
            SourceLocation loc);
        // 校验结构体花括号初始化列表（成员顺序、嵌套结构体/数组成员，第 14 章）
        // Validate a struct brace initializer (member order, nested struct/array members, §14)
        void check_struct_initializer(AST::ArrayInitializer* init, const AST::Type& struct_type,
            SourceLocation loc);
        void check_if_statement(AST::IfStatement* if_stmt);
        void check_for_statement(AST::ForStatement* for_stmt);
        void check_while_statement(AST::WhileStatement* while_stmt);
        void check_break_statement(AST::BreakStatement* break_stmt);
        void check_return_statement(AST::ReturnStatement* return_stmt);
        void check_expression_statement(AST::ExpressionStatement* expr_stmt);

        // ---- 表达式检查 ----
        // 检查表达式，返回其类型（可能经过隐式转换）
        AST::Type check_expression(AST::Expression* expr, bool allow_void = false);

        // 各表达式节点的具体检查（返回类型）
        AST::Type check_assignment(AST::AssignmentExpression* expr);
        AST::Type check_logical_or(AST::LogicalOrExpression* expr);
        AST::Type check_logical_and(AST::LogicalAndExpression* expr);
        AST::Type check_comparison(AST::ComparisonExpression* expr);
        AST::Type check_additive(AST::AdditiveExpression* expr);
        AST::Type check_multiplicative(AST::MultiplicativeExpression* expr);
        AST::Type check_power(AST::PowerExpression* expr);
        AST::Type check_unary(AST::UnaryExpression* expr);
        AST::Type check_postfix(AST::PostfixExpression* expr);
        AST::Type check_primary(AST::PrimaryExpression* expr);

        // 文件操作内置函数检查（Gallt 0.2.txt §17，错误码 ER 0052 ~ ER 0059）
        // File-operation builtin checks (Gallt 0.2.txt §17, error codes ER 0052 - ER 0059)
        AST::Type check_file_builtin_call(AST::PostfixExpression* expr,
            const std::string& func_name);

        // ---- 类型工具 ----
        // 隐式类型转换：如果可能，将 from_type 提升为 to_type，否则返回 false
        bool can_implicit_convert(const AST::Type& from, const AST::Type& to);

        // 获取通常算术转换后的类型（用于二元运算符）
        AST::Type usual_arithmetic_conversion(const AST::Type& left, const AST::Type& right);

        // 判断类型是否为数值类型（整数或浮点）
        bool is_numeric_type(const AST::Type& type) const;

        // 判断类型是否为整数类型（int, char, bool）
        bool is_integer_type(const AST::Type& type) const;

        // 判断类型是否为布尔类型
        bool is_bool_type(const AST::Type& type) const;

        // 判断类型是否为指针类型
        bool is_pointer_type(const AST::Type& type) const;

        // 判断类型是否为结构体类型
        bool is_struct_type(const AST::Type& type) const;

        // 判断类型是否为函数类型
        bool is_function_type(const AST::Type& type) const;

        // 判断类型是否为数组类型
        bool is_array_type(const AST::Type& type) const;

        // 判断类型是否为 void
        bool is_void_type(const AST::Type& type) const;

        // 判断类型是否为 file 对象类型（非 file*）
        // Check whether the type is the opaque file object type (not file*)
        bool is_file_type(const AST::Type& type) const;

        // 判断类型是否为 file* 句柄类型
        // Check whether the type is the file* handle type
        bool is_file_pointer_type(const AST::Type& type) const;

        // 获取结构体定义（通过类型名称）
        AST::StructDefinition* get_struct_definition(const std::string& name) const;

        // 获取结构体成员（通过结构体类型和成员名）
        const AST::StructDefinition::Member* get_struct_member(const AST::Type& struct_type,
            std::string_view member_name) const;

        // ---- 符号查找辅助 ----
        Symbol* lookup_symbol(std::string_view name, bool report_error = true);
        const Symbol* lookup_symbol(std::string_view name, bool report_error = true) const;

        // ---- 错误报告辅助 ----
        void report_error(SourceLocation loc, ErrorCode code, const std::string& msg);
        void report_error(ErrorCode code, const std::string& msg);  // 使用当前位置（通常是 current token）
        void report_warning(SourceLocation loc, ErrorCode code, const std::string& msg);

        // ---- 0.3 §18 重载决议 ----
        // ---- 0.3 §18 overload resolution ----
        // 为当前作用域的重载集分配唯一名字（命名修饰）
        void mangle_overload_set(const std::string& name);
        // 重载调用决议：返回选中符号（失败返回 nullptr 并报错）
        Symbol* resolve_overload_call(const std::string& name, AST::PostfixExpression* call,
            AST::PrimaryExpression* callee);
        // 某个实参到形参的转换等级：0 精确匹配，1 隐式转换，-1 不可转换
        int conversion_rank(const AST::Type& from, const AST::Type& to);
        // 重载命名修饰使用的完整参数签名（第 18 章）
        // Full parameter-type signature used for overload name mangling (§18)
        std::string overload_signature(const std::vector<AST::Type>& params) const;
        std::string unique_mangled_name(const std::string& base);
        // 记录一次函数名解析（供代码生成取当前名字）
        // Record a function-name resolution so codegen can read the current name
        void record_function_resolution(AST::PrimaryExpression* callee, const Symbol& symbol);
        // 按目标函数指针类型在重载集中选择（第 18 章 / 第 13 章）
        // Select an overload by the expected function-pointer type (§18/§13)
        Symbol* select_overload_by_target_type(std::vector<Symbol>& set, const AST::Type& target,
            const std::string& name, SourceLocation loc);
        // 定义阶段检查：两个重载是否因默认参数而在某次调用上必然二义
        // Definition-stage check: do two overloads inevitably become ambiguous for some call?
        bool overloads_ambiguous_by_defaults(const Symbol& a, const Symbol& b) const;
        // ---- 第 20 章：特殊成员重载决议与生命周期检查 ----
        // ---- §20: special-member overload resolution and lifetime checks ----
        // 按实参类型解析构造函数重载集（二义性报 ER 0096）
        Symbol* resolve_constructor_overload(const std::string& struct_name,
            const std::vector<AST::Type>& arg_types, const std::vector<bool>& arg_is_null,
            SourceLocation loc);
        // 类型大小/对齐（与代码生成一致；用于 Placement 构造检查）
        bool type_layout(const AST::Type& type, std::size_t& size, std::size_t& align) const;
        // 类型的默认拷贝/移动特殊成员是否可用（[nocopy]/[nomove] 与成员递归，第 20 章）
        bool type_is_copyable(const AST::Type& type) const;
        bool type_is_movable(const AST::Type& type) const;
        // 判断表达式是否为 move(...)（第 20 章移动语义）
        static bool is_move_expression(const AST::Expression* expr);

        // ---- 主函数验证 ----
        void verify_main_function();

        // ---- 初始化 ----
        // 预置内置函数（input, output, heap, free, size, align）
        void declare_builtin_functions();

        // 检查类型是否完整（非 void，且非未定义的结构体）
        bool is_complete_type(const AST::Type& type);

        // ---- 结构体完整性检查 ----
        void check_struct_completeness(AST::StructDefinition* struct_def);

        // ---- 数组初始化检查 ----
        void check_array_initialization(AST::VariableDeclaration* decl);

        // ---- 常量表达式求值（用于数组大小） ----
        std::optional<size_t> evaluate_const_expression(AST::Expression* expr);
        bool is_constant_integer_expression(AST::Expression* expr, size_t* out_value = nullptr);

        // ---- 0.4.1 const qualifier semantics (Gallt 0.4.1.txt sections 2 and 7) ----
        // Integer values of const constants, layered by scope, used to evaluate
        // constant integer expressions such as array lengths (0.4.1 sections 2 and 7).
        std::vector<std::unordered_map<std::string, long long>> const_values_;
        void enter_scope();
        void exit_scope();
        // Record the integer value of a const constant (0.4.1 section 2).
        void record_const_value(const std::string& name, AST::Expression* initializer,
            const AST::Type& type);
        // Evaluate a constant integer expression, resolving const names (0.4.1 section 7).
        std::optional<long long> evaluate_const_integer_expression(AST::Expression* expr);
        // Validate a const object declaration: the initializer must be evaluable at
        // compile time (ER 0116).
        void check_const_declaration(AST::VariableDeclaration* decl);
        // Decide whether an expression is a compile-time constant expression (section 19).
        bool is_compile_time_constant_expression(AST::Expression* expr);
        // Display name of an lvalue expression (the '[identifier]' of ER 0115).
        static std::string expression_display_name(const AST::Expression* expr);

        // ---- 语句结束符（分号/换行）检查 ----
        // 已由语法分析器处理，语义层不需要再检查
    };

} // namespace gallt

#endif // GALLT_SEMANTIC_TYPE_CHECKER_HPP
