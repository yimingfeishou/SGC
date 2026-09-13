#ifndef GALLT_CODEGEN_CODEGEN_HPP
#define GALLT_CODEGEN_CODEGEN_HPP

// codegen/codegen.hpp
// LLVM IR 文本生成器 —— 将已通过语义检查的 AST 转换为 LLVM IR
// LLVM IR text generator — converts a semantically checked AST into LLVM IR

#include "../common/diagnostics.hpp"
#include "../parser/ast.hpp"

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace gallt {

    class CodeGenerator {
    public:
        // 构造：接收 AST 与类型检查器缓存的表达式类型
        // Construct with the AST and expression types cached by the type checker
        CodeGenerator(AST::Program* program,
            const std::unordered_map<const AST::Expression*, AST::Type>& expression_types,
            const std::unordered_map<const AST::PrimaryExpression*,
                const AST::FunctionDefinition*>& resolved_functions = {},
            const std::unordered_map<const AST::PrimaryExpression*,
                const AST::ExternDeclaration*>& resolved_externs = {});

        // 生成模块级 LLVM IR 文本；成功返回 true
        // Generate module-level LLVM IR text; returns true on success
        bool generate();

        // 生成的 LLVM IR（generate 成功后有效）
        // Generated LLVM IR (valid after a successful generate call)
        const std::string& ir() const { return ir_; }

        // 记录到的待链接 C 库（clib 语句提供）
        // C libraries requested by clib statements, for the driver/linker
        const std::vector<std::string>& link_libraries() const { return link_libraries_; }

        // 返回嵌入的 C 运行库源码，供 LLVM 链接生成可执行文件
        // Returns the embedded C runtime used when linking the executable
        static std::string runtime_c_source();

    private:
        AST::Program* program_;
        const std::unordered_map<const AST::Expression*, AST::Type>& expression_types_;
        // 类型检查阶段解析出的函数名（第 18 章）：代码生成据此取当前名字
        // Function names resolved by the type checker (§18); codegen reads current names
        const std::unordered_map<const AST::PrimaryExpression*, const AST::FunctionDefinition*>&
            resolved_functions_;
        const std::unordered_map<const AST::PrimaryExpression*, const AST::ExternDeclaration*>&
            resolved_externs_;
        std::string ir_;
        std::vector<std::string> link_libraries_;
        std::vector<AST::StructDefinition*> struct_defs_;
        std::unordered_map<std::string, AST::StructDefinition*> struct_by_name_;
        struct LocalInfo {
            AST::Type type;
            std::string address;
        };
        // 需要清理的字符串容器：记录地址和完整类型，以便递归释放嵌套 string
        // String-containing objects to clean up: store the full type so nested
        // string members can be released recursively
        struct CleanupRecord {
            AST::Type type;
            std::string address;
        };
        std::vector<AST::VariableDeclaration*> global_vars_;
        // 当前语句中创建的值临时对象（完整表达式结束时析构，Gallt 0.3.txt §20）
        // Value temporaries created by the current statement (destroyed at the end of the
        // full expression, Gallt 0.3.txt §20)
        std::vector<CleanupRecord> statement_temporaries_;
        std::unordered_map<std::string, LocalInfo> global_symbols_;
        std::unordered_map<std::string, AST::FunctionDefinition*> function_by_name_;
        std::unordered_map<std::string, AST::ExternDeclaration*> extern_by_name_;
        struct StringLiteralConstant {
            std::string bytes;
            std::string llvm_name;
        };
        std::vector<StringLiteralConstant> string_literals_;
        std::unordered_map<std::string, std::string> string_literal_ids_;

        // ---- IR 编写辅助 ----
        std::vector<std::string> lines_;
        unsigned temp_counter_ = 0;
        unsigned label_counter_ = 0;
        std::string current_label_;
        std::unordered_set<std::string> emitted_labels_;
        bool current_block_terminated_ = true;

        // ---- 结构体返回值的 sret 约定（Gallt 0.3.txt §20）----
        // ---- sret convention for struct returns (Gallt 0.3.txt §20) ----
        // 结构体返回值由调用者提供存储（隐藏的首个指针参数），被调用者在其上执行
        // 拷贝/移动构造，从而保证返回对象经过特殊成员构造而不是裸字节拷贝
        // Struct returns pass hidden storage provided by the caller; the callee copy/move
        // constructs into it, so the returned object is built by its special members
        static bool returns_via_sret(const AST::Type& type) {
            return type.kind == AST::TypeKind::Struct;
        }
        std::string current_sret_pointer_;      // 被调用侧：当前函数的 sret 指针
        std::string pending_sret_destination_;  // 调用侧：`T v = f()` 的直接初始化目标

        // 已生成但尚未插入到函数入口块的 alloca 指令，以及插入位置
        // Alloca instructions pending insertion into the function entry block
        std::vector<std::string> hoisted_allocas_;
        std::size_t hoist_insert_index_ = 0;

        std::string new_temp(const char* hint);
        std::string new_label(const char* hint);
        // 栈分配：alloca 统一提升到函数入口块，避免循环体内反复分配导致栈增长；
        // 名称由 new_temp 保证唯一（同名变量不会产生重复的 SSA 名）
        // Stack allocation: allocas are hoisted into the function entry block so that
        // allocations inside loops cannot grow the stack, and the name is unique
        std::string emit_alloca(const std::string& type_text, const char* hint);
        void flush_hoisted_allocas();
        void emit_line(const std::string& line);
        void start_block(const std::string& label);
        std::string llvm_type(const AST::Type& type);
        std::string struct_type_name(const std::string& name) const;

        // ---- 类型信息 ----
        void collect_structs();
        void collect_structs_in_statement(AST::Statement* stmt);
        bool type_contains_string(const AST::Type& type);

        // ---- 代码生成主流程 ----
        void emit_preamble();
        void emit_struct_types();
        void emit_string_constants();
        void emit_runtime_declarations();
        void emit_function_declarations();
        void emit_functions();
        void collect_global_variables();
        // 预建函数/外部函数签名索引，供调用点 O(1) 查询
        // Prebuilt function/extern signature index for O(1) call-site lookup
        void collect_function_signatures();
        void emit_global_variables();
        void emit_global_initializer();
        // Gallt 0.3.txt §20：全局对象析构函数（始终生成，由 @main 包装函数调用）
        // Gallt 0.3.txt §20: global-object deinitializer (always emitted; called by @main)
        void emit_global_deinit_function();
        // Gallt 0.3.txt §20：生成 @main 包装函数，负责在用户 main 之后析构全局对象
        // Gallt 0.3.txt §20: emit the @main wrapper that destroys global objects after main
        void emit_main_wrapper();
        void emit_initializer_to_address(AST::VariableDeclaration* decl,
            const std::string& address);
        std::string function_llvm_name_for_source(std::string_view name) const;
        std::string function_reference(const std::string& name);
        // 通过表达式取得函数引用（优先使用类型检查阶段的解析结果）
        // Resolve a function reference through an expression (prefers the checker's result)
        std::string function_reference_for(const AST::PrimaryExpression* callee);
        // extern 的 IR 符号名（C 符号；同名不同签名时使用修饰名以保证 IR 合法）
        // IR symbol of an extern (the C symbol; decorated when signatures collide)
        std::string extern_ir_symbol(const AST::ExternDeclaration* ext) const;
        mutable std::unordered_map<const AST::ExternDeclaration*, std::string> extern_ir_symbols_;
        mutable std::unordered_set<std::string> declared_extern_symbols_;
        bool function_is_extern(const std::string& name);
        std::string string_cstr_pointer(const std::string& value);

        void emit_function(AST::FunctionDefinition* func);
        AST::FunctionDefinition* current_function_ = nullptr;
        std::vector<std::string> break_labels_;
        std::vector<std::size_t> break_cleanup_depths_;
        void emit_statement(AST::Statement* stmt);
        // 析构当前语句创建的值临时对象（逆序）
        // Destroy the value temporaries created by the current statement (in reverse)
        void destroy_statement_temporaries();
        void emit_block(AST::Block* block, bool new_scope);
        void emit_variable_declaration(AST::VariableDeclaration* decl);
        void emit_expression_statement(AST::ExpressionStatement* stmt);

        // ---- 作用域与局部变量 ----
        std::vector<std::unordered_map<std::string, LocalInfo>> scopes_;
        std::vector<std::vector<CleanupRecord>> cleanup_scopes_;
        LocalInfo* lookup_local(const std::string& name);
        void push_scope();
        void pop_scope();
        void register_string_cleanup(const std::string& address, const AST::Type& type);
        void emit_destroy_string_at(const AST::Type& type, const std::string& address);
        void destroy_active_cleanup_scopes(std::size_t until_depth);

        // ---- 表达式代码生成 ----
        struct ExprValue {
            AST::Type type;      // 表达式类型（来自类型检查器）
            std::string value;   // SSA 值（已加载）
            std::string address; // 若为左值，指向对象的指针
            bool is_lvalue = false;
            std::string owned_string; // 若为拥有堆存储的临时 string，其栈地址
        };

        ExprValue gen_expr(AST::Expression* expr);
        ExprValue gen_primary(AST::PrimaryExpression* expr);
        ExprValue gen_unary(AST::UnaryExpression* expr);
        ExprValue gen_postfix(AST::PostfixExpression* expr);
        ExprValue gen_binary_string_plus(AST::Expression* left, AST::Expression* right);
        // 将值转换为 string 操作数；owned_temp 标记返回的是新建的临时量（需释放）
        // Convert a value to a string operand; owned_temp marks a created temporary
        std::string string_value_or_converted(const ExprValue& v, bool* owned_temp = nullptr);
        void destroy_owned_string(ExprValue& value);
        std::string gen_address(AST::Expression* expr);
        std::string gen_pointer_value(AST::Expression* expr);

        std::string load_string_address(const std::string& address);
        void emit_string_assign(const std::string& dest_address, const ExprValue& source);
        void emit_struct_brace_initialization(const std::string& address,
            const AST::Type& struct_type, AST::ArrayInitializer* init);
        // 数组（含嵌套数组 / 数组的 struct 元素）的花括号初始化（第 7/14 章）
        // Brace initialization of arrays, including nested arrays and struct elements
        void emit_array_brace_initialization(const std::string& address,
            const AST::Type& array_type, AST::ArrayInitializer* init);
        void emit_aggregate_assign(const std::string& dest_address,
            const AST::Type& dest_type, ExprValue& source, bool is_assignment = false);
        void emit_deep_copy_string_members(const AST::Type& struct_type,
            const std::string& dest_address, const std::string& src_address);
        // ---- Gallt 0.3.txt §20：默认拷贝/移动/深拷贝/浅拷贝语义 ----
        // ---- Gallt 0.3.txt §20: default copy/move/deep-copy/shallow-copy semantics ----
        // 逐成员拷贝（默认拷贝构造 / 默认拷贝赋值）
        void emit_memberwise_copy(const AST::Type& type, const std::string& dst,
            const std::string& src, bool is_assignment);
        // 逐成员移动（默认移动构造 / 默认移动赋值）
        void emit_memberwise_move(const AST::Type& type, const std::string& dst,
            const std::string& src, bool is_assignment);
        // deep_copy：递归复制所有引用的资源，不依赖拷贝构造函数
        void emit_deep_copy(const AST::Type& type, const std::string& dst,
            const std::string& src);
        // shallow_copy：仅复制对象本身的内存数据
        void emit_shallow_copy(const AST::Type& type, const std::string& dst,
            const std::string& src);
        // 成员可拷贝 / 可移动（用于默认特殊成员函数是否可生成）
        bool type_is_copyable(const AST::Type& type) const;
        bool type_is_movable(const AST::Type& type) const;
        // 取表达式的存储地址（左值取地址；否则物化到临时量）
        std::string operand_address(AST::Expression* expr, AST::Type* out_type = nullptr);
        // 追加构造函数被省略的默认实参（第 8 章 / 第 20 章）
        // Append the constructor's omitted default arguments (§8/§20)
        void collect_constructor_defaults(const std::string& ctor_name,
            std::vector<AST::Expression*>& args);
        // 在调用者提供的存储上完成结构体返回值的拷贝/移动构造（第 20 章）
        // Copy/move-construct a struct return value into the caller-provided storage
        void emit_struct_return(AST::Expression* expr, const AST::Type& type,
            const std::string& sret);
        // 判断表达式是否为“返回结构体的 Gallt 函数直接调用”（供复制消除使用）
        // Whether the expression is a direct call to a struct-returning Gallt function
        bool is_struct_returning_call(AST::Expression* expr) const;
        // 指针成员深拷贝的目标类型信息
        bool deep_copyable_pointee(const AST::Type& pointer_type) const;

        // ---- 类型转换与运算辅助 ----
        std::string convert_value(const std::string& value, const AST::Type& from, const AST::Type& to);
        std::string truth_condition(const std::string& value, const AST::Type& type);
        std::string to_i64_value(const std::string& value, const AST::Type& type);
        AST::Type resolved_type(const AST::Expression* expr) const;

        // ---- 内置调用 ----
        void emit_output_call(AST::PostfixExpression* call);
        // input(...) 读取值；无参形式把读到的整数作为表达式结果返回
        // input(...) reads a value; the zero-argument form yields the read int
        void emit_input_call(AST::PostfixExpression* call, ExprValue& result);
        void emit_free_call(AST::PostfixExpression* call);
        ExprValue emit_size_align_call(AST::PostfixExpression* call, bool is_size);
        // 文件操作内置调用（Gallt 0.2.txt §17）
        // File-operation builtin calls (Gallt 0.2.txt §17)
        ExprValue emit_file_builtin_call(AST::PostfixExpression* call,
            const std::string& name);

        // ---- 布局计算 ----
        std::size_t type_size(const AST::Type& type) const;
        std::size_t type_align(const AST::Type& type) const;
        std::vector<std::pair<std::size_t, std::size_t>> struct_member_layout(
            const AST::StructDefinition* def) const;
    };

} // namespace gallt

#endif // GALLT_CODEGEN_CODEGEN_HPP
