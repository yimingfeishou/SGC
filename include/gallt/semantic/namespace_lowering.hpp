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

        bool run(AST::Program* program);

    private:
        struct NamespaceInfo {
            std::unordered_map<std::string, std::string> members;
            std::unordered_map<std::string, std::string> member_kinds;
            std::unordered_set<std::string> child_namespaces;
            SourceLocation location;
        };

        struct Scope {
            std::unordered_map<std::string, std::string> declared;
            std::unordered_map<std::string, std::string> aliases;
        };

        DiagnosticEngine& diag_;
        AST::Program* program_ = nullptr;

        std::map<std::string, NamespaceInfo> namespaces_;
        std::vector<Scope> scopes_;
        std::string namespace_prefix_;
        bool had_error_ = false;
        SourceLocation type_location_hint_;

        void collect_members(const std::vector<std::unique_ptr<AST::TopLevel>>& nodes,
            const std::string& prefix, bool addition, SourceLocation loc);
        void declare_namespace_member(const std::string& prefix, const std::string& name,
            const std::string& kind, bool addition, SourceLocation loc);
        void rename_namespace_member(AST::TopLevel* node);

        void lower_top_levels(std::vector<std::unique_ptr<AST::TopLevel>>& nodes,
            std::vector<std::unique_ptr<AST::TopLevel>>& out, bool in_namespace_body);
        void lower_namespace_body(AST::NamespaceDefinition* def,
            std::vector<std::unique_ptr<AST::TopLevel>>& out);
        void lower_addition_body(AST::AdditionNamespaceStatement* def,
            std::vector<std::unique_ptr<AST::TopLevel>>& out);
        void handle_access_namespace(AST::AccessNamespaceStatement* node);

        void push_scope();
        void pop_scope();
        std::optional<std::string> lookup_alias(const std::string& name) const;
        bool declared_in_current_scope(const std::string& name) const;
        std::optional<std::string> declared_type_of(const std::string& name) const;
        void declare_name(const std::string& name, const std::string& type_text);
        void enter_namespace_scope(const std::string& full_name);

        std::optional<std::string> resolve_qualified(const std::vector<std::string>& path,
            SourceLocation loc, bool type_context);

        void rewrite_generic_ref(AST::GenericRef& ref);
        std::optional<std::string> resolve_namespace_path(
            const std::vector<std::string>& parts, SourceLocation loc);
        void rewrite_type(AST::Type& type);
        void rewrite_expression(std::unique_ptr<AST::Expression>& expr);
        void rewrite_shared_expression(std::shared_ptr<AST::Expression>& expr);
        void rewrite_expression_nested(AST::Expression* expr);
        std::optional<std::string> expression_replacement(AST::Expression* expr,
            SourceLocation& loc);
        void rewrite_initializer(AST::Initializer* init);
        void rewrite_statement(AST::Statement* stmt);
        void rewrite_top_level(AST::TopLevel* node);
        std::optional<std::string> rewrite_identifier(const std::string& name) const;

        void report(SourceLocation loc, ErrorCode code, const std::vector<std::string>& values);
        void report(SourceLocation loc, ErrorCode code, const std::string& message);
        static std::string internal_name(const std::string& qualified);
        static std::string join_path(const std::string& prefix, const std::string& name);
    };

}

#endif
