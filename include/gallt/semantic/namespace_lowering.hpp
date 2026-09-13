// semantic/namespace_lowering.hpp
// 命名空间降低 —— Gallt 0.4.txt §21
// Namespace lowering — Gallt 0.4.txt §21
//
// 该阶段在泛型展开之前运行：把命名空间定义、追加命名空间与 access namespace
// 降低为“带内部（命名修饰）名字的普通顶层声明 + 名字别名”，使后续的类型检查与
// 代码生成只需处理 0.2/0.3 已有的具体程序结构。
// This pass runs before generic expansion: namespace definitions, addition namespaces
// and access-namespace statements are lowered into ordinary top-level declarations with
// internal (mangled) names plus name aliases, so the existing type checker and code
// generator only see the concrete program shapes they already handle.
//
// 与 Gallt 0.4.txt §21 的对应关系 / Relation to Gallt 0.4.txt §21:
//   namespace N { ... }              → 成员改名 N$member 并提升到顶层
//                                      (members renamed to N$member, hoisted to top level)
//   addition namespace N { ... }     → 合并进已存在的 N（冲突报 ER 0114）
//   access namespace N[/member]      → 当前作用域别名（冲突报 ER 0113）
//   A::B::member                     → 内部名（ER 0100/0102/0105 诊断）

#ifndef GALLT_SEMANTIC_NAMESPACE_LOWERING_HPP
#define GALLT_SEMANTIC_NAMESPACE_LOWERING_HPP

#include "../common/diagnostics.hpp"
#include "../parser/ast.hpp"

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace gallt {

    class NamespaceLowering {
    public:
        explicit NamespaceLowering(DiagnosticEngine& diag);

        // 降低整个程序；返回 false 表示已报告错误
        // Lower the whole program; returns false once an error was reported
        bool run(AST::Program* program);

    private:
        // 一个命名空间：直接成员（短名 → 内部名）与直接子命名空间短名
        // One namespace: direct members (short name -> internal name) and the short
        // names of its direct child namespaces
        struct NamespaceInfo {
            std::unordered_map<std::string, std::string> members;
            // 成员种类（generic / struct / function / variable / extern），用于
            // 命名空间限定泛型实例化的名称查找（Gallt 0.4.txt §19/§21）
            // Member kind (generic/struct/function/variable/extern), used by the
            // namespace-qualified generic instantiation lookup (Gallt 0.4.txt §19/§21)
            std::unordered_map<std::string, std::string> member_kinds;
            std::unordered_set<std::string> child_namespaces;
            SourceLocation location;
        };

        // 作用域：《Gallt 0.4.txt》§5 查找规则所需的声明集合与命名空间别名集合
        // A scope: declarations plus namespace aliases, as needed by §5 lookup
        struct Scope {
            std::unordered_map<std::string, std::string> declared;  // 名 → 类型文本
            std::unordered_map<std::string, std::string> aliases;   // 名 → 目标名
        };

        DiagnosticEngine& diag_;
        AST::Program* program_ = nullptr;

        std::map<std::string, NamespaceInfo> namespaces_;   // 全名 → 命名空间信息
        std::vector<Scope> scopes_;
        std::string namespace_prefix_;                      // 当前命名空间全名
        bool had_error_ = false;
        // 类型重写时用于诊断的位置（类型自身不带位置）
        // Location used while rewriting types (a Type carries no location)
        SourceLocation type_location_hint_;

        // ---- 阶段一：收集命名空间并为成员分配内部名 ----
        // ---- Phase 1: collect namespaces and assign internal names ----
        void collect_members(const std::vector<std::unique_ptr<AST::TopLevel>>& nodes,
            const std::string& prefix, bool addition, SourceLocation loc);
        void declare_namespace_member(const std::string& prefix, const std::string& name,
            const std::string& kind, bool addition, SourceLocation loc);
        // 把命名空间成员声明的名字改为内部名
        // Rename a namespace member declaration to its internal name
        void rename_namespace_member(AST::TopLevel* node);

        // ---- 阶段二：作用域遍历与降低 ----
        // ---- Phase 2: scoped walk and lowering ----
        void lower_top_levels(std::vector<std::unique_ptr<AST::TopLevel>>& nodes,
            std::vector<std::unique_ptr<AST::TopLevel>>& out, bool in_namespace_body);
        void lower_namespace_body(AST::NamespaceDefinition* def,
            std::vector<std::unique_ptr<AST::TopLevel>>& out);
        void lower_addition_body(AST::AdditionNamespaceStatement* def,
            std::vector<std::unique_ptr<AST::TopLevel>>& out);
        // 处理 `access namespace ...`：把目标对象引入当前作用域（ER 0103/0113）
        // Handle `access namespace ...`: import the target into the current scope
        void handle_access_namespace(AST::AccessNamespaceStatement* node);

        // ---- 作用域 ----
        void push_scope();
        void pop_scope();
        // 在作用域栈中查找名字目标（别名）；declared 中的名字遮蔽外层别名
        // Look a name up through the scope stack: declarations shadow outer aliases
        std::optional<std::string> lookup_alias(const std::string& name) const;
        // 名字在当前作用域是否已有声明（用于 ER 0113）
        // Whether the name is already declared in the current scope (used by ER 0113)
        bool declared_in_current_scope(const std::string& name) const;
        // 名字的声明类型文本（用于 ER 0105 的 '[type]' 占位符）
        // Declared type text of a name (the '[type]' placeholder of ER 0105)
        std::optional<std::string> declared_type_of(const std::string& name) const;
        void declare_name(const std::string& name, const std::string& type_text);
        // 进入命名空间体：把其成员与直接子命名空间登记为别名
        // Enter a namespace body: register its members and child namespaces as aliases
        void enter_namespace_scope(const std::string& full_name);

        // 限定名解析：`A::B::member` → 内部名（失败时报告 ER 0100/0102/0105）
        // Qualified-name resolution: `A::B::member` -> internal name
        std::optional<std::string> resolve_qualified(const std::vector<std::string>& path,
            SourceLocation loc, bool type_context);

        // ---- 重写 ----
        // 泛型引用重写：命名空间限定名解析（ER 0100/0102/0061/0105）与别名展开
        // Generic-reference rewriting: namespace-qualified resolution
        // (ER 0100/0102/0061/0105) plus alias expansion
        void rewrite_generic_ref(AST::GenericRef& ref);
        // 解析命名空间路径（不含末段）；成功返回命名空间全名
        // Resolve a namespace path (without the final component); returns its full name
        std::optional<std::string> resolve_namespace_path(
            const std::vector<std::string>& parts, SourceLocation loc);
        void rewrite_type(AST::Type& type);
        void rewrite_expression(std::unique_ptr<AST::Expression>& expr);
        // 泛型实参中的表达式以 shared_ptr 持有（Gallt 0.3.txt §19）
        // Expressions inside generic arguments are held by shared_ptr (Gallt 0.3.txt §19)
        void rewrite_shared_expression(std::shared_ptr<AST::Expression>& expr);
        // 不替换节点本身，只递归重写子节点
        // Rewrite only the children, without replacing the node itself
        void rewrite_expression_nested(AST::Expression* expr);
        // 顶层可替换的节点（限定名 / 别名标识符）
        // Top-level replaceable nodes (qualified names / aliased identifiers)
        std::optional<std::string> expression_replacement(AST::Expression* expr,
            SourceLocation& loc);
        void rewrite_initializer(AST::Initializer* init);
        void rewrite_statement(AST::Statement* stmt);
        void rewrite_top_level(AST::TopLevel* node);
        // 表达式是否应当被别名重写（内层声明遮蔽）
        // Whether an identifier expression should be rewritten through an alias
        std::optional<std::string> rewrite_identifier(const std::string& name) const;

        void report(SourceLocation loc, ErrorCode code, const std::vector<std::string>& values);
        void report(SourceLocation loc, ErrorCode code, const std::string& message);
        // 把 `A::B` 形式的全名转成内部名 `A$B`
        // Turn a fully-qualified name `A::B` into the internal name `A$B`
        static std::string internal_name(const std::string& qualified);
        static std::string join_path(const std::string& prefix, const std::string& name);
    };

} // namespace gallt

#endif // GALLT_SEMANTIC_NAMESPACE_LOWERING_HPP
