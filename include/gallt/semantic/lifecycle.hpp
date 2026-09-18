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

        bool run(AST::Program* program);

    private:
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
        std::unordered_map<std::string, AST::Type> local_types_;
        std::unordered_set<std::string> constructed_pointers_;
        std::unordered_set<std::string> destructed_pointers_;
        std::unordered_set<std::string> non_construct_pointers_;
        std::unordered_set<std::string> null_pointers_;
        std::unordered_map<std::string, int> alias_group_;
        std::unordered_set<int> destructed_groups_;
        int next_alias_group_ = 0;

        void collect_structs();
        void validate_special_members();
        void check_copy_constructor_source(const StructInfo& info);
        void collect_declarations();

        void lower_special_members();
        std::unique_ptr<AST::FunctionDefinition> lower_special_member(
            AST::StructDefinition* def, AST::SpecialMemberFunction* member,
            const std::string& name);

        void rewrite_top_level(AST::TopLevel* node);
        void rewrite_statement(AST::Statement* stmt);
        void track_pointer_source(const std::string& name, const AST::Expression* expr);
        void collect_pointer_allocations(AST::Statement* stmt);
        void rewrite_declaration(AST::VariableDeclaration* decl,
            std::vector<std::unique_ptr<AST::Statement>>& insert_after);
        void rewrite_expression(AST::Expression* expr);
        void rewrite_assignment_call(AST::ExpressionStatement* stmt,
            AST::AssignmentExpression* assign, const std::string& func_name);

        void rewrite_member_references(AST::Statement* stmt,
            const std::unordered_set<std::string>& members,
            const std::unordered_set<std::string>& locals);
        AST::SpecialMemberFunction* select_constructor(const StructInfo& info,
            const std::vector<AST::Expression*>& args, bool* ambiguous);
        int argument_rank(const AST::Type& param, const AST::Expression* arg) const;
        bool type_matches_argument(const AST::Type& param, const AST::Expression* arg) const;
        AST::Type infer_literal_type(const AST::Expression* expr) const;

        bool has_destructor(const std::string& struct_name) const;
        bool type_needs_destruction(const AST::Type& type) const;
        void mark_needs_destruction(const std::string& name);
        bool is_copyable(const AST::Type& type) const;
        bool is_movable(const AST::Type& type) const;

        void report_template(SourceLocation loc, ErrorCode code,
            const std::vector<std::string>& values);
        void report(SourceLocation loc, ErrorCode code, const std::string& message);
    };

} 

#endif 
