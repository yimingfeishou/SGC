// semantic/symbol_table.hpp
// 符号表 —— 管理作用域、标识符声明、类型信息
// Symbol Table — manages scopes, identifier declarations, and type information

#ifndef GALLT_SEMANTIC_SYMBOL_TABLE_HPP
#define GALLT_SEMANTIC_SYMBOL_TABLE_HPP

#include "../common/source_location.hpp"
#include "../parser/ast.hpp"
#include <string>
#include <string_view>
#include <vector>
#include <unordered_map>
#include <memory>
#include <optional>

namespace gallt {

    // ============================================================================
    // 符号种类 (Symbol Kind)
    // ============================================================================

    enum class SymbolKind {
        Variable,           // 变量
        Function,           // 函数
        Struct,             // 结构体
        Parameter,          // 函数形参
        FunctionPointer,    // 函数指针变量
    };

    // ============================================================================
    // 符号信息 (Symbol)
    // ============================================================================

    struct Symbol {
        std::string name;
        SymbolKind kind;
        AST::Type type;                 // 符号的类型
        SourceLocation declaration_loc; // 声明位置
        bool is_initialized = false;    // 是否已初始化（变量）
        bool is_mutable = true;         // 是否可修改（默认true）

        // 仅当 kind == Function 时有效
        std::vector<AST::Type> param_types;
        std::vector<std::string> param_names;
        AST::FunctionDefinition* function_node = nullptr; // 指向AST节点（用于代码生成）
        // 仅当符号来自 extern 声明时有效（extern 函数参与重载，见第 18 章）
        // Valid when the symbol comes from an extern declaration (extern functions take
        // part in overload resolution, Gallt 0.3.txt §18)
        AST::ExternDeclaration* extern_node = nullptr;

        // 仅当 kind == Struct 时有效
        AST::StructDefinition* struct_node = nullptr;

        // 仅当 kind == Parameter 时有效
        size_t param_index = 0;         // 在参数列表中的索引

        // 构造函数
        Symbol() = default;
        Symbol(std::string_view n, SymbolKind k, AST::Type t, SourceLocation loc)
            : name(n), kind(k), type(std::move(t)), declaration_loc(loc) {
        }

        // 便捷工厂
        static Symbol make_variable(std::string_view name, AST::Type type, SourceLocation loc, bool init = false) {
            Symbol sym(name, SymbolKind::Variable, std::move(type), loc);
            sym.is_initialized = init;
            return sym;
        }

        static Symbol make_parameter(std::string_view name, AST::Type type, SourceLocation loc, size_t idx) {
            Symbol sym(name, SymbolKind::Parameter, std::move(type), loc);
            sym.param_index = idx;
            return sym;
        }

        static Symbol make_function(std::string_view name, AST::Type ret_type,
            const std::vector<AST::Type>& params,
            const std::vector<std::string>& param_names,
            SourceLocation loc, AST::FunctionDefinition* node = nullptr) {
            Symbol sym(name, SymbolKind::Function, std::move(ret_type), loc);
            sym.param_types = params;
            sym.param_names = param_names;
            sym.function_node = node;
            return sym;
        }

        static Symbol make_struct(std::string_view name, SourceLocation loc, AST::StructDefinition* node = nullptr) {
            Symbol sym(name, SymbolKind::Struct, AST::Type::make_struct(std::string(name)), loc);
            sym.struct_node = node;
            return sym;
        }
    };

    // ============================================================================
    // 作用域 (Scope)
    // ============================================================================

    class Scope {
    public:
        Scope() = default;
        ~Scope() = default;

        // 声明符号，返回是否成功（失败表示重复定义）
        bool declare(const Symbol& sym);

        // ---- 0.3：重载集 (Gallt 0.3.txt §18) ----
        // ---- 0.3: overload sets (Gallt 0.3.txt §18) ----
        // 在当前作用域登记一个函数重载；返回 false 表示签名重复（ER 0010）
        bool declare_overload(const Symbol& sym);
        // 当前作用域的同名重载集（可能为空）
        std::vector<Symbol>* overloads(std::string_view name);
        const std::vector<Symbol>* overloads(std::string_view name) const;

        // 查找符号（在当前作用域内）
        Symbol* lookup(std::string_view name);
        const Symbol* lookup(std::string_view name) const;

        // 获取所有符号（用于调试）
        const std::unordered_map<std::string, Symbol>& get_symbols() const { return symbols_; }

    private:
        std::unordered_map<std::string, Symbol> symbols_; // 符号名 -> 符号信息
        std::unordered_map<std::string, std::vector<Symbol>> overloads_; // 重载集
    };

    // ============================================================================
    // 符号表 (SymbolTable)
    // 管理作用域栈，支持嵌套作用域
    // ============================================================================

    class SymbolTable {
    public:
        SymbolTable() = default;
        ~SymbolTable() = default;

        // ---- 作用域管理 ----
        // 进入新作用域
        void enter_scope();
        // 退出当前作用域（销毁该作用域内所有符号）
        void exit_scope();
        // 获取当前作用域深度（全局为0）
        size_t scope_depth() const { return scopes_.size(); }

        // ---- 符号声明 ----
        // 在当前作用域声明符号，返回是否成功（失败表示重复定义）
        bool declare(const Symbol& sym);
        // 在当前作用域登记函数重载（签名重复返回 false）
        bool declare_overload(const Symbol& sym);

        // ---- 符号查找 ----
        // 从当前作用域向上查找符号（包括所有外层作用域）
        Symbol* lookup(std::string_view name);
        const Symbol* lookup(std::string_view name) const;

        // 按 0.3 §5 的查找规则取得同名重载集：
        // 每一层先按函数名收集同名重载集，若存在则停止向外查找
        // Look up the overload set of a name following the 0.3 §5 rules
        std::vector<Symbol>* lookup_overloads(std::string_view name);
        // 是否为内层非函数声明所遮蔽（用于区分 ER 0001/E 0017）
        // Whether an inner non-function declaration shadows the name
        bool is_shadowed_by_non_function(std::string_view name) const;

        // 仅在当前作用域查找（不向上）
        Symbol* lookup_current(std::string_view name);
        const Symbol* lookup_current(std::string_view name) const;

        // ---- 结构体成员查找 ----
        // 在指定结构体类型中查找成员
        static const AST::StructDefinition::Member* lookup_struct_member(
            const AST::StructDefinition* struct_def, std::string_view member_name);

        // ---- 获取当前作用域（用于调试） ----
        const Scope& current_scope() const { return *scopes_.back(); }

    private:
        std::vector<std::unique_ptr<Scope>> scopes_;
    };

    // ============================================================================
    // 符号表/作用域内联实现
    // Inline implementations of the scope/symbol-table operations
    // ============================================================================

    inline bool Scope::declare(const Symbol& sym) {
        // 重复名称不能在同一作用域声明
        // Duplicate names cannot be declared in the same scope
        return symbols_.emplace(sym.name, sym).second;
    }

    inline Symbol* Scope::lookup(std::string_view name) {
        auto it = symbols_.find(std::string(name));
        return it == symbols_.end() ? nullptr : &it->second;
    }

    inline const Symbol* Scope::lookup(std::string_view name) const {
        auto it = symbols_.find(std::string(name));
        return it == symbols_.end() ? nullptr : &it->second;
    }

    inline bool Scope::declare_overload(const Symbol& sym) {
        // 同一作用域内参数类型完全相同（含数量）视为重复定义（ER 0010）
        // An identical parameter type list in one scope is a redefinition (ER 0010)
        auto& set = overloads_[sym.name];
        for (const Symbol& existing : set) {
            if (existing.param_types.size() != sym.param_types.size()) continue;
            bool same = true;
            for (std::size_t i = 0; i < existing.param_types.size(); ++i) {
                if (!(existing.param_types[i] == sym.param_types[i])) {
                    same = false;
                    break;
                }
            }
            if (same) return false;
        }
        set.push_back(sym);
        return true;
    }

    inline std::vector<Symbol>* Scope::overloads(std::string_view name) {
        auto it = overloads_.find(std::string(name));
        return it == overloads_.end() ? nullptr : &it->second;
    }

    inline const std::vector<Symbol>* Scope::overloads(std::string_view name) const {
        auto it = overloads_.find(std::string(name));
        return it == overloads_.end() ? nullptr : &it->second;
    }

    inline void SymbolTable::enter_scope() {
        scopes_.push_back(std::make_unique<Scope>());
    }

    inline void SymbolTable::exit_scope() {
        if (!scopes_.empty()) {
            scopes_.pop_back();
        }
    }

    inline bool SymbolTable::declare(const Symbol& sym) {
        // 所有作用域都作为隐藏上下文；声明只发生在最内层作用域
        // Declarations happen only in the innermost active scope
        if (scopes_.empty()) enter_scope();
        return scopes_.back()->declare(sym);
    }

    inline bool SymbolTable::declare_overload(const Symbol& sym) {
        if (scopes_.empty()) enter_scope();
        return scopes_.back()->declare_overload(sym);
    }

    inline Symbol* SymbolTable::lookup(std::string_view name) {
        // 从内到外查找
        // Search from inner to outer scopes
        for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
            if (Symbol* s = (*it)->lookup(name)) return s;
        }
        return nullptr;
    }

    inline const Symbol* SymbolTable::lookup(std::string_view name) const {
        for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
            if (const Symbol* s = (*it)->lookup(name)) return s;
        }
        return nullptr;
    }

    inline std::vector<Symbol>* SymbolTable::lookup_overloads(std::string_view name) {
        // 每一层：先看重载集，再看普通声明（0.3 §5 查找规则）
        // Per scope: overload set first, then ordinary declarations (0.3 §5)
        for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
            if (std::vector<Symbol>* set = (*it)->overloads(name)) {
                return set;
            }
            if ((*it)->lookup(name) != nullptr) {
                return nullptr;   // 被内层普通声明遮蔽
            }
        }
        return nullptr;
    }

    inline bool SymbolTable::is_shadowed_by_non_function(std::string_view name) const {
        for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
            if ((*it)->overloads(name) != nullptr) return false;
            const Symbol* s = (*it)->lookup(name);
            if (s != nullptr) {
                return s->kind != SymbolKind::Function;
            }
        }
        return false;
    }

    inline Symbol* SymbolTable::lookup_current(std::string_view name) {
        if (scopes_.empty()) return nullptr;
        return scopes_.back()->lookup(name);
    }

    inline const Symbol* SymbolTable::lookup_current(std::string_view name) const {
        if (scopes_.empty()) return nullptr;
        return scopes_.back()->lookup(name);
    }

    inline const AST::StructDefinition::Member* SymbolTable::lookup_struct_member(
        const AST::StructDefinition* struct_def, std::string_view member_name) {
        if (struct_def == nullptr) return nullptr;
        for (const auto& member : struct_def->members) {
            if (member.name == member_name) return &member;
        }
        return nullptr;
    }

} // namespace gallt

#endif // GALLT_SEMANTIC_SYMBOL_TABLE_HPP
