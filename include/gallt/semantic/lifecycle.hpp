// semantic/lifecycle.hpp
// 对象生命周期与构造析构降低 —— Gallt 0.3.txt §20
// Object lifetime / constructor / destructor lowering — Gallt 0.3.txt §20
//
// 该阶段在泛型展开之后、类型检查之前运行：把结构体的特殊成员函数降低为
// 普通（命名修饰后的）函数，并把构造/析构/拷贝/移动的使用点改写为对它们的调用。
// Runs after generic expansion and before type checking: it lowers special member
// functions into plain (mangled) functions and rewrites construction/destruction and
// copy/move uses into calls to them.

#ifndef GALLT_SEMANTIC_LIFECYCLE_HPP
#define GALLT_SEMANTIC_LIFECYCLE_HPP

#include "../common/diagnostics.hpp"
#include "../parser/ast.hpp"

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace gallt {

    class LifecycleLowering {
    public:
        explicit LifecycleLowering(DiagnosticEngine& diag);

        // 返回 false 表示已报告错误
        // Returns false once an error was reported
        bool run(AST::Program* program);

    private:
        // 单个结构体的生命周期信息
        // Lifetime information of one struct
        struct StructInfo {
            AST::StructDefinition* def = nullptr;
            std::vector<AST::SpecialMemberFunction*> constructors;
            AST::SpecialMemberFunction* destructor = nullptr;
            AST::SpecialMemberFunction* copy_constructor = nullptr;
            AST::SpecialMemberFunction* move_constructor = nullptr;
            AST::SpecialMemberFunction* copy_assignment = nullptr;
            AST::SpecialMemberFunction* move_assignment = nullptr;
            std::string dtor_name;
            std::string copy_ctor_name;
            std::string move_ctor_name;
            std::string copy_assign_name;
            std::string move_assign_name;
            std::vector<std::string> ctor_names;
            bool no_copy = false;
            bool no_move = false;
        };

        DiagnosticEngine& diag_;
        AST::Program* program_ = nullptr;
        std::unordered_map<std::string, StructInfo> structs_;
        std::unordered_map<std::string, AST::StructDefinition*> struct_defs_;
        bool had_error_ = false;
        // 当前函数内的局部变量类型（用于赋值语句的拷贝/移动改写）
        // Local variable types in the current function (for copy/move assignment rewrites)
        std::unordered_map<std::string, AST::Type> local_types_;
        // construct 分配得到的指针名与已 destruct 的指针名（ER 0092 / ER 0093）
        // Names holding construct-allocated pointers and already destructed names
        std::unordered_set<std::string> constructed_pointers_;
        std::unordered_set<std::string> destructed_pointers_;
        // 明确不是 construct 分配（heap/取地址/数组退化）的指针名 → ER 0092
        // Pointers that are definitely not construct-allocated (heap/address-of/array)
        std::unordered_set<std::string> non_construct_pointers_;
        // 置为 null 的指针名：对其 destruct 是无操作（§20）
        // Pointers set to null: destruct on them is a no-op (§20)
        std::unordered_set<std::string> null_pointers_;
        // 别名分组：指向同一对象的指针共享组号；组内已 destruct 后再次 destruct → ER 0093
        // Alias groups: pointers to the same object share a group id; a second destruct of
        // any member of a destructed group is ER 0093
        std::unordered_map<std::string, int> alias_group_;
        std::unordered_set<int> destructed_groups_;
        int next_alias_group_ = 0;

        // ---- 收集与校验 ----
        void collect_structs();
        void validate_special_members();
        void collect_declarations();

        // ---- 降低特殊成员 ----
        void lower_special_members();
        std::unique_ptr<AST::FunctionDefinition> lower_special_member(
            AST::StructDefinition* def, AST::SpecialMemberFunction* member,
            const std::string& name);

        // ---- 使用点改写 ----
        void rewrite_top_level(AST::TopLevel* node);
        // 返回是否需要重新遍历
        void rewrite_statement(AST::Statement* stmt);
        // 记录指针来源/别名关系（第 20 章：destruct 的对象必须由 construct 分配）
        // Record a pointer's origin/alias relation (§20: destruct requires a construct object)
        void track_pointer_source(const std::string& name, const AST::Expression* expr);
        // 收集 construct/heap 分配的指针名，用于 destruct 检查
        void collect_pointer_allocations(AST::Statement* stmt);
        // 改写变量声明的初始化（构造/拷贝/移动初始化）
        // Rewrite a variable declaration's initializer (construction/copy/move init)
        void rewrite_declaration(AST::VariableDeclaration* decl,
            std::vector<std::unique_ptr<AST::Statement>>& insert_after);
        void rewrite_expression(AST::Expression* expr);
        // 把 x = y / x = move(y) 改写为 copy_assignment / move_assignment 调用
        // Rewrite x = y / x = move(y) into a copy/move-assignment call
        void rewrite_assignment_call(AST::ExpressionStatement* stmt,
            AST::AssignmentExpression* assign, const std::string& func_name);

        // ---- 工具 ----
        // 成员引用改写：body 中直接书写的成员名 -> __this->member
        void rewrite_member_references(AST::Statement* stmt,
            const std::unordered_set<std::string>& members,
            const std::unordered_set<std::string>& locals);
        // 找出与实参类型最匹配的构造函数；返回 nullptr 表示无匹配
        AST::SpecialMemberFunction* select_constructor(const StructInfo& info,
            const std::vector<AST::Expression*>& args, bool* ambiguous);
        // 实参 → 形参的转换等级（第 18 章偏序；-1 不可转换，未知返回 kArgRankUnknown）
        // Argument -> parameter conversion rank (§18 ordering; -1 = no conversion)
        int argument_rank(const AST::Type& param, const AST::Expression* arg) const;
        bool type_matches_argument(const AST::Type& param, const AST::Expression* arg) const;
        AST::Type infer_literal_type(const AST::Expression* expr) const;

        bool has_destructor(const std::string& struct_name) const;
        bool type_needs_destruction(const AST::Type& type) const;
        void mark_needs_destruction(const std::string& name);
        // 成员级可拷贝 / 可移动判定（默认特殊成员函数是否可生成，§20）
        // Member-level copyable/movable checks (whether default special members exist)
        bool is_copyable(const AST::Type& type) const;
        bool is_movable(const AST::Type& type) const;

        // 使用标准文档模板并按顺序填充占位符
        // Report with the standard-document template, filling placeholders in order
        void report_template(SourceLocation loc, ErrorCode code,
            const std::vector<std::string>& values);
        void report(SourceLocation loc, ErrorCode code, const std::string& message);
    };

} // namespace gallt

#endif // GALLT_SEMANTIC_LIFECYCLE_HPP
