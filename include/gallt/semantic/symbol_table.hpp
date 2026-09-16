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

    enum class SymbolKind {
        Variable,           
        Function,           
        Struct,             
        Parameter,          
        FunctionPointer,    
    };

    struct Symbol {
        std::string name;
        SymbolKind kind;
        AST::Type type;                 
        SourceLocation declaration_loc; 
        bool is_initialized = false;    
        bool is_mutable = true;         

        std::vector<AST::Type> param_types;
        std::vector<std::string> param_names;
        AST::FunctionDefinition* function_node = nullptr; 
        AST::ExternDeclaration* extern_node = nullptr;

        AST::StructDefinition* struct_node = nullptr;

        size_t param_index = 0;         

        Symbol() = default;
        Symbol(std::string_view n, SymbolKind k, AST::Type t, SourceLocation loc)
            : name(n), kind(k), type(std::move(t)), declaration_loc(loc) {
        }

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

    class Scope {
    public:
        Scope() = default;
        ~Scope() = default;

        bool declare(const Symbol& sym);

        bool declare_overload(const Symbol& sym);
        std::vector<Symbol>* overloads(std::string_view name);
        const std::vector<Symbol>* overloads(std::string_view name) const;

        Symbol* lookup(std::string_view name);
        const Symbol* lookup(std::string_view name) const;

        const std::unordered_map<std::string, Symbol>& get_symbols() const { return symbols_; }

    private:
        std::unordered_map<std::string, Symbol> symbols_; 
        std::unordered_map<std::string, std::vector<Symbol>> overloads_; 
    };

    class SymbolTable {
    public:
        SymbolTable() = default;
        ~SymbolTable() = default;

        void enter_scope();
        void exit_scope();
        size_t scope_depth() const { return scopes_.size(); }

        bool declare(const Symbol& sym);
        bool declare_overload(const Symbol& sym);

        Symbol* lookup(std::string_view name);
        const Symbol* lookup(std::string_view name) const;

        std::vector<Symbol>* lookup_overloads(std::string_view name);
        bool is_shadowed_by_non_function(std::string_view name) const;

        Symbol* lookup_current(std::string_view name);
        const Symbol* lookup_current(std::string_view name) const;

        static const AST::StructDefinition::Member* lookup_struct_member(
            const AST::StructDefinition* struct_def, std::string_view member_name);

        const Scope& current_scope() const { return *scopes_.back(); }

    private:
        std::vector<std::unique_ptr<Scope>> scopes_;
    };


    inline bool Scope::declare(const Symbol& sym) {
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
        if (scopes_.empty()) enter_scope();
        return scopes_.back()->declare(sym);
    }

    inline bool SymbolTable::declare_overload(const Symbol& sym) {
        if (scopes_.empty()) enter_scope();
        return scopes_.back()->declare_overload(sym);
    }

    inline Symbol* SymbolTable::lookup(std::string_view name) {
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
        for (auto it = scopes_.rbegin(); it != scopes_.rend(); ++it) {
            if (std::vector<Symbol>* set = (*it)->overloads(name)) {
                return set;
            }
            if ((*it)->lookup(name) != nullptr) {
                return nullptr;   
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

} 

#endif 
