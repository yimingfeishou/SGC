// semantic/generic_expander.hpp
// 编译期泛型展开（单态化）—— Gallt 0.3.txt §19
// Compile-time generic expansion (monomorphization) — Gallt 0.3.txt §19
//
// 该阶段在类型检查之前运行：把 generics 块、实例化语句与限定名引用展开为
// 具体的结构体/函数定义（命名修饰后），使后续的类型检查与代码生成只需处理
// 0.2 已有的具体程序结构。
// This pass runs before type checking: generic blocks, instantiation statements and
// qualified references are expanded into concrete (mangled) structs/functions so the
// existing type checker and code generator only see plain, concrete program shapes.

#ifndef GALLT_SEMANTIC_GENERIC_EXPANDER_HPP
#define GALLT_SEMANTIC_GENERIC_EXPANDER_HPP

#include "../common/diagnostics.hpp"
#include "../parser/ast.hpp"

#include <optional>
#include <deque>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace gallt {

    class GenericExpander {
    public:
        explicit GenericExpander(DiagnosticEngine& diag);

        // 展开整个程序；返回 false 表示已报告错误
        // Expand the whole program; returns false once an error was reported
        bool expand(AST::Program* program);

        // 已展开的实例（供类型检查阶段复用）
        // Expanded instantiations (reused by the type checker)
        const std::unordered_map<std::string, AST::StructDefinition*>& instantiated_structs() const {
            return instantiated_structs_;
        }

    private:
        // ---- 泛型注册表 ----
        // ---- Generic registry ----
        struct GenericEntry {
            AST::GenericDefinition* primary = nullptr;
            std::vector<AST::GenericDefinition*> specializations;
        };

        // 编译期常量绑定值
        // Compile-time constant binding
        struct ConstantValue {
            long long int_value = 0;
            double float_value = 0.0;
            bool is_float = false;
            // Gallt 0.4.txt §19：字符串编译期常量参数（如 Emiter<string, "123">）
            // 与 Emit String 的拼接、参数类型属性一起使用
            // Gallt 0.4.txt §19: string compile-time constant parameters (e.g.
            // Emiter<string, "123">), used by Emit String concatenation
            bool is_string = false;
            std::string string_value;
        };

        // 类型/常量替换环境
        // Substitution environment for type and constant parameters
        struct Substitution {
            std::unordered_map<std::string, AST::Type> types;
            std::unordered_map<std::string, ConstantValue> constants;
            // 编译期常量参数的声明类型（用于把 N 替换为对应类型的字面量，§19）
            // Declared types of constant parameters (so N becomes a typed literal, §19)
            std::unordered_map<std::string, AST::Type> constant_types;
            // 泛型块内成员名 → 命名修饰后的具体名字（结构体或函数）
            // Block member name -> mangled concrete name (struct or function)
            std::unordered_map<std::string, std::string> members;
            // 当前实例的引用（泛型名 + 实参，member 为空）
            // The instance this environment belongs to (generic name + args, no member)
            AST::GenericRef instance;
        };

        // 短名绑定
        // Short-name binding
        struct ShortBinding {
            std::string target;      // 命名修饰后的成员名 / mangled member name
            bool is_type = false;    // true: 结构体类型名；false: 函数名
            std::string instance;    // 来源实例（用于幂等判断）
        };

        DiagnosticEngine& diag_;
        AST::Program* program_ = nullptr;

        std::unordered_map<std::string, GenericEntry> generics_;
        std::unordered_map<std::string, AST::StructDefinition*> struct_defs_;
        std::unordered_map<std::string, AST::FunctionDefinition*> func_defs_;
        std::unordered_map<std::string, AST::ExternDeclaration*> extern_defs_;
        std::unordered_map<std::string, AST::StructDefinition*> instantiated_structs_;

        // 已实例化的成员名（幂等）：mangled member name → true
        // Already instantiated member names (idempotence)
        std::unordered_set<std::string> instantiated_members_;
        // 已整体实例化的泛型：instance mangled name → true
        std::unordered_set<std::string> instantiated_blocks_;
        // 每个实例化块内的结构体成员名（用于类型上下文的单成员自动选定）
        // Struct member names per instantiated block (single-member type resolution)
        std::unordered_map<std::string, std::vector<std::string>> instance_struct_members_;
        // 每个实例中“主泛型有、所选特化块没有”的成员（引用时报 ER 0075）
        // Members present in the primary but missing from the selected specialization (ER 0075)
        std::unordered_map<std::string, std::unordered_set<std::string>> missing_members_;
        // 各作用域中可见的实例（用于 ER 0075 的诊断定位）
        // Instances visible in each scope (used to diagnose ER 0075)
        std::vector<std::unordered_set<std::string>> instance_scopes_;

        // 作用域栈：普通声明的名字集合 + 短名绑定
        // Scope stack: declared ordinary names + short-name bindings
        std::vector<std::unordered_set<std::string>> declared_scopes_;
        std::vector<std::unordered_map<std::string, ShortBinding>> short_scopes_;

        // 展开结果：实例化成员被插入到触发它们的声明之前
        // Expansion output: instantiated members are inserted before the declaration that
        // triggered them, so that "define before use" still holds
        std::vector<std::unique_ptr<AST::TopLevel>>* output_ = nullptr;
        // 词素池：为合成字面量提供稳定的 string_view 存储。
        // 必须使用 deque：vector 扩容会使先前字面量的 string_view 失效
        // (a vector would reallocate and dangle earlier literals' string_views)
        std::deque<std::string> lexeme_pool_;
        // Gallt 0.4.txt §19：Emit String 生成的源码缓冲区。
        // 解析出的 AST 中字符串字面量的 string_view 指向这些缓冲区，因此必须
        // 在整个展开期间（以及随后的类型检查/代码生成）保持有效，故用 deque。
        // Source buffers produced by Emit String. String literals in the parsed AST
        // keep string_views into them, so they must outlive expansion and the later
        // type-checking/code-generation passes; a deque guarantees stable addresses.
        std::deque<std::string> emit_source_pool_;
        // 实例的实参种类签名（用于 ER 0079 规范化冲突检测）
        // Argument-kind signature per instance (ER 0079 normalization collision)
        std::unordered_map<std::string, std::string> instantiated_kind_signatures_;

        bool had_error_ = false;

        // ---- 阶段 ----
        void collect_generics();
        void collect_declarations();
        void expand_top_levels();
        void expand_top_level(AST::TopLevel* node);
        void expand_statement(AST::Statement* stmt);
        void expand_expression(std::unique_ptr<AST::Expression>& expr);
        void expand_initializer(AST::Initializer* init);

        // ---- 实例化 ----
        // 确保 ref 指代的实例已展开；member 为空表示整体实例化
        // Ensure the instantiation referenced by `ref` exists
        std::optional<std::string> ensure_instantiation(const AST::GenericRef& ref,
            bool whole_block);
        // 解析类型中的泛型引用（递归）
        void resolve_type(AST::Type& type);
        // 把类型中的泛型实例转换为具体类型（返回是否成功）
        bool substitute_type(const AST::Type& in, const Substitution& sub, AST::Type& out);

        // ---- 克隆 + 替换 ----
        std::unique_ptr<AST::Statement> clone_statement(const AST::Statement* stmt,
            const Substitution& sub);
        std::unique_ptr<AST::Expression> clone_expression(const AST::Expression* expr,
            const Substitution& sub);
        // 由编译期常量生成字面量表达式
        // Build a literal expression from a compile-time constant
        std::unique_ptr<AST::Expression> make_constant_literal(SourceLocation loc,
            const ConstantValue& value);
        std::unique_ptr<AST::Initializer> clone_initializer(const AST::Initializer* init,
            const Substitution& sub);
        std::unique_ptr<AST::FunctionDefinition> clone_function(
            const AST::FunctionDefinition* func, const Substitution& sub,
            const std::string& name);
        AST::StructDefinition::Member clone_member(const AST::StructDefinition::Member& member,
            const Substitution& sub);
        // 克隆特殊成员函数（第 19 章 + 第 20 章）：实例化泛型结构体时必须一并复制
        // constructor/destructor/copy_constructor/move_constructor/copy_assignment/move_assignment，
        // 否则实例化类型会丢失全部生命周期语义
        // Clone a special member function: instantiating a generic struct must carry the
        // constructor/destructor/copy/move members over, otherwise the instantiated type
        // loses all §20 lifetime semantics
        std::unique_ptr<AST::SpecialMemberFunction> clone_special_member(
            const AST::SpecialMemberFunction* member, const Substitution& sub);

        // ---- 特化匹配 ----
        // 把主泛型形态的参数列表转换为自由标识符模式（第二次出现的主泛型形态 → 偏特化）
        // Convert a parameter list into free-identifier patterns (later primary-shaped
        // definitions become partial specializations)
        std::vector<AST::GenericPatternArg> parameters_as_free_patterns(
            AST::GenericDefinition* def) const;
        // 结构相同的参数/模式列表视为重复定义（ER 0062）
        // Structurally identical pattern lists are duplicate definitions (ER 0062)
        bool duplicate_pattern_signature(const GenericEntry& entry,
            AST::GenericDefinition* def) const;
        struct MatchResult {
            bool matched = false;
            Substitution substitution;
        };
        MatchResult match_specialization(AST::GenericDefinition* def,
            const AST::GenericRef& ref);
        // 单位置模式偏序：从 A 的角度比较 A 与 B
        // Single-position pattern order, from A's perspective
        enum class PatternOrder { Better, Equal, Worse, Incomparable };
        PatternOrder compare_pattern(const AST::GenericPatternArg& a,
            const AST::GenericPatternArg& b) const;
        PatternOrder compare_pattern_type(const AST::Type& a, const AST::Type& b) const;
        // A 至少与 B 一样特化
        bool at_least_as_specialized(AST::GenericDefinition* a,
            AST::GenericDefinition* b) const;
        // A 比 B 更特化
        bool more_specialized(AST::GenericDefinition* a, AST::GenericDefinition* b) const;
        // 模式结构签名（自由标识符归一化），用于重复定义判定
        std::string pattern_signature(const AST::GenericPatternArg& pattern) const;

        // ---- 约束检查 ----
        bool constraint_satisfied(const AST::GenericConstraint& constraint,
            const AST::Type& arg);

        // ---- 常量 ----
        bool resolve_constant_argument(const AST::GenericArgument& arg,
            const Substitution* sub, ConstantValue& out);
        // 按替换环境求值编译期常量表达式（支持算术/比较/逻辑/类型转换/size/align）
        // Evaluate a compile-time constant expression under the substitution
        bool evaluate_with_substitution(const AST::Expression* expr, const Substitution& sub,
            ConstantValue& out);
        // 类型大小/对齐解析（内建类型与已实例化结构体；支持泛型参数替换后的类型名）
        // Type layout resolution for size/align (builtins and instantiated structs)
        bool resolve_type_layout(const std::string& type_name, std::size_t& size,
            std::size_t& align, const Substitution& sub) const;
        // 生成常量参数的对应类型字面量（第 19 章：在泛型体内作为对应类型的常量使用）
        // Build a typed literal for a constant parameter (§19)
        std::unique_ptr<AST::Expression> make_typed_constant_literal(SourceLocation loc,
            const ConstantValue& value, const AST::Type& type);

        // ---- 作用域与短名 ----
        void push_scope();
        void pop_scope();
        void declare_name(const std::string& name);
        bool name_declared_in_current_scope(const std::string& name) const;
        // owning_generic：产生该短名的泛型名。泛型名本身不视为“已有声明”冲突，
        // 因为实例化短名是该泛型的具象化，而非另一份独立声明（第 19 章示例中
        // 成员名与泛型名相同时保持可用）。
        // A generic's own name is not treated as a conflicting declaration: the short
        // name is that generic's concrete realization, not a separate declaration.
        void register_short_name(const std::string& name, const ShortBinding& binding,
            const std::string& owning_generic, SourceLocation loc);

        // ---- 诊断 ----
        void report(SourceLocation loc, ErrorCode code, const std::vector<std::string>& values);
        void report(SourceLocation loc, ErrorCode code, const std::string& message);

        // ---- Gallt 0.4.txt §19：编译期代码生成与编译期逻辑 ----
        // ---- Gallt 0.4.txt §19: compile-time code generation and logic ----
        //
        // 泛型块顶部的编译期项（emit / 编译期 if）按从上到下的顺序处理：Emit String
        // 片段累积成源码后整体解析，Emit Block 直接插入其 AST 成员，编译期 if 只
        // 保留条件为真的分支。
        // Compile-time items at the top of a generic block are processed top to bottom:
        // Emit String pieces accumulate into source text which is parsed as a unit,
        // Emit Block inserts its AST members directly, and a compile-time if keeps only
        // the branch whose condition is true.
        bool process_compile_time_items(
            const std::vector<std::unique_ptr<AST::Statement>>& items,
            const Substitution& sub, std::string& text, SourceLocation& text_loc,
            std::vector<AST::TopLevel*>& member_ptrs,
            std::vector<std::unique_ptr<AST::TopLevel>>& owner);
        // 解析累积的 Emit String 源码，把结果作为实例成员加入
        bool flush_emit_string(const std::string& text, SourceLocation loc,
            std::vector<AST::TopLevel*>& member_ptrs,
            std::vector<std::unique_ptr<AST::TopLevel>>& owner);
        // Emit String 片段求值（字符串字面量 / 编译期常量参数 / 属性 / 拼接）
        bool eval_emit_piece(const AST::Expression* expr, const Substitution& sub,
            std::string& out);
        // 编译期 if 条件求值（属性比较 / 逻辑组合 / 布尔字面量）
        bool eval_compile_time_condition(const AST::Expression* expr, const Substitution& sub,
            bool& out);
        // 编译期布尔属性（is_integer、is_same<...> 等）
        bool eval_bool_property(const AST::Expression* receiver, const std::string& property,
            const std::vector<const AST::Expression*>& args, const Substitution& sub,
            bool& out);
        // 编译期属性值：参数的大小 / 对齐（返回 false 表示不适用）
        bool eval_size_align_property(const std::string& parameter, const std::string& property,
            const Substitution& sub, long long& out);
        // 参数（类型参数或编译期常量参数）对应的类型
        bool param_type_of(const std::string& name, const Substitution& sub, AST::Type& out) const;
        // 编译期属性实参（类型名 / 参数名）解析为类型
        bool property_argument_type(const std::string& text, const Substitution& sub,
            AST::Type& out) const;
        bool base_type_of(const std::string& name, const Substitution& sub,
            AST::Type& out) const;
        // 编译期可转换性（is_convertible）
        bool property_convertible(const AST::Type& from, const AST::Type& to) const;
        // 字符串字面量解码（转义序列处理，与第 9 章一致）
        bool decode_string_literal(std::string_view lexeme, std::string& out) const;
        // 表达式的可读文本（仅用于诊断占位符）
        std::string expression_text(const AST::Expression* expr) const;
        // 把泛型体内对编译期属性的引用改写为常量字面量（克隆阶段调用）
        std::unique_ptr<AST::Expression> rewrite_property_expression(
            const AST::PostfixExpression* expr, const Substitution& sub);

        void add_top_level(std::unique_ptr<AST::TopLevel> node);
    };

} // namespace gallt

#endif // GALLT_SEMANTIC_GENERIC_EXPANDER_HPP
