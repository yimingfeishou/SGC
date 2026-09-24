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
        struct ExpressionCallSite {
            std::string name;
            AST::Type declared_return_type;
            std::vector<AST::Type> declared_parameter_types;
            SourceLocation location;
            int depth = 0;
        };

        explicit GenericExpander(DiagnosticEngine& diag);

        bool expand(AST::Program* program);

        const std::unordered_map<std::string, AST::StructDefinition*>& instantiated_structs() const {
            return instantiated_structs_;
        }

        const std::unordered_map<const AST::Expression*, ExpressionCallSite>&
            expression_call_sites() const {
            return expression_call_sites_;
        }

        const std::unordered_map<const AST::Expression*, std::string>&
            expression_free_identifiers() const {
            return expression_free_identifiers_;
        }

        const std::unordered_map<const AST::Expression*,
            std::tuple<std::string, std::size_t, AST::Type>>&
            expression_argument_casts() const {
            return expression_argument_casts_;
        }

    private:
        struct GenericEntry {
            AST::GenericDefinition* primary = nullptr;
            std::vector<AST::GenericDefinition*> specializations;
        };

        struct ConstantValue {
            long long int_value = 0;
            double float_value = 0.0;
            bool is_float = false;
            bool is_string = false;
            std::string string_value;
        };

        struct Substitution {
            std::unordered_map<std::string, AST::Type> types;
            std::unordered_map<std::string, ConstantValue> constants;
            std::unordered_map<std::string, AST::Type> constant_types;
            std::unordered_map<std::string, std::string> members;
            std::unordered_map<std::string, std::vector<AST::Type>> type_packs;
            std::unordered_map<std::string, std::vector<ConstantValue>> const_packs;
            std::unordered_map<std::string, AST::Type> const_pack_element;
            std::unordered_map<std::string, std::vector<std::string>>
                fixed_pack_members;
            struct ExpressionBinding {
                std::string name;
                std::vector<AST::Type> parameter_types;
                std::vector<std::string> parameter_names;
                AST::Type return_type;
                std::shared_ptr<AST::ExpressionParameterBody> body;
                std::string normalized;
            };
            std::unordered_map<std::string, ExpressionBinding> expressions;
            std::unordered_map<std::string, std::string> renames;
            std::unordered_map<std::string, const AST::Expression*> expr_arguments;
            std::unordered_map<std::string, AST::Type> expr_argument_types;
            AST::GenericRef instance;
        };

        struct ShortBinding {
            std::string target;
            bool is_type = false;
            std::string instance;
        };

        DiagnosticEngine& diag_;
        AST::Program* program_ = nullptr;

        std::unordered_map<std::string, GenericEntry> generics_;
        std::unordered_map<std::string, AST::StructDefinition*> struct_defs_;
        std::unordered_map<std::string, AST::FunctionDefinition*> func_defs_;
        std::unordered_map<std::string, AST::ExternDeclaration*> extern_defs_;
        std::unordered_map<std::string, AST::StructDefinition*> instantiated_structs_;
        std::unordered_map<const AST::Expression*, ExpressionCallSite> expression_call_sites_;
        int expression_expansion_depth_ = 0;
        const AST::FunctionDefinition* current_function_ = nullptr;
        std::vector<std::unique_ptr<AST::Statement>>* pending_hoisted_ = nullptr;
        std::vector<std::unique_ptr<AST::Expression>> expr_argument_storage_;
        int expr_temp_counter_ = 0;
        bool in_expression_body_ = false;
        std::unordered_map<const AST::Expression*, std::string>
            expression_free_identifiers_;
        std::unordered_map<const AST::Expression*,
            std::tuple<std::string, std::size_t, AST::Type>>
            expression_argument_casts_;
        std::unordered_map<std::string, std::size_t> expression_parameter_index_;

        std::unordered_set<std::string> instantiated_members_;
        std::unordered_set<std::string> instantiated_blocks_;
        std::unordered_map<std::string, std::vector<std::string>> instance_struct_members_;
        std::unordered_map<std::string, std::unordered_set<std::string>> missing_members_;
        std::vector<std::unordered_set<std::string>> instance_scopes_;

        std::vector<std::unordered_set<std::string>> declared_scopes_;
        std::vector<std::unordered_map<std::string, ShortBinding>> short_scopes_;

        std::vector<std::unique_ptr<AST::TopLevel>>* output_ = nullptr;
        std::deque<std::string> lexeme_pool_;
        std::deque<std::string> emit_source_pool_;
        std::unordered_map<std::string, std::string> instantiated_kind_signatures_;

        bool had_error_ = false;

        void collect_generics();
        void collect_declarations();
        void expand_top_level(AST::TopLevel* node);
        void expand_statement(AST::Statement* stmt);
        void expand_expression(std::unique_ptr<AST::Expression>& expr);
        void expand_initializer(AST::Initializer* init);

        std::optional<std::string> ensure_instantiation(const AST::GenericRef& ref,
            bool whole_block);
        void resolve_type(AST::Type& type);
        bool substitute_type(const AST::Type& in, const Substitution& sub, AST::Type& out);

        std::unique_ptr<AST::Statement> clone_statement(const AST::Statement* stmt,
            const Substitution& sub);
        std::unique_ptr<AST::Statement> clone_statement_impl(const AST::Statement* stmt,
            const Substitution& sub);
        std::unique_ptr<AST::Statement> clone_substatement(const AST::Statement* stmt,
            const Substitution& sub);
        bool emit_body_into_temp(const std::vector<std::unique_ptr<AST::Statement>>& stmts,
            const Substitution& sub, const std::string& temp_name,
            const AST::Type& result_type,
            std::vector<std::unique_ptr<AST::Statement>>& out);
        bool body_always_returns(const AST::Statement* stmt) const;
        std::unique_ptr<AST::Expression> clone_expression(const AST::Expression* expr,
            const Substitution& sub);
        std::unique_ptr<AST::Expression> make_constant_literal(SourceLocation loc,
            const ConstantValue& value);
        std::unique_ptr<AST::Initializer> clone_initializer(const AST::Initializer* init,
            const Substitution& sub);
        std::unique_ptr<AST::FunctionDefinition> clone_function(
            const AST::FunctionDefinition* func, const Substitution& sub,
            const std::string& name);
        AST::StructDefinition::Member clone_member(const AST::StructDefinition::Member& member,
            const Substitution& sub);
        AST::StructDefinition::Member clone_member_impl(
            const AST::StructDefinition::Member& member, const Substitution& sub);
        std::unique_ptr<AST::SpecialMemberFunction> clone_special_member(
            const AST::SpecialMemberFunction* member, const Substitution& sub);

        std::vector<AST::GenericPatternArg> parameters_as_free_patterns(
            AST::GenericDefinition* def) const;
        bool duplicate_pattern_signature(const GenericEntry& entry,
            AST::GenericDefinition* def) const;
        struct MatchResult {
            bool matched = false;
            Substitution substitution;
        };
        MatchResult match_specialization(AST::GenericDefinition* def,
            const AST::GenericRef& ref);
        enum class PatternOrder { Better, Equal, Worse, Incomparable };
        PatternOrder compare_pattern(const AST::GenericPatternArg& a,
            const AST::GenericPatternArg& b) const;
        PatternOrder compare_pattern_type(const AST::Type& a, const AST::Type& b) const;
        bool at_least_as_specialized(AST::GenericDefinition* a,
            AST::GenericDefinition* b) const;
        bool more_specialized(AST::GenericDefinition* a, AST::GenericDefinition* b) const;
        std::string pattern_signature(const AST::GenericPatternArg& pattern) const;

        bool constraint_satisfied(const AST::GenericConstraint& constraint,
            const AST::Type& arg);

        bool resolve_constant_argument(const AST::GenericArgument& arg,
            const Substitution* sub, ConstantValue& out);
        bool is_pack_name(const Substitution& sub, const std::string& name) const;
        static bool pack_identifier_name(const AST::Expression* expr,
            std::string& out);
        static ConstantValue pack_element_zero(const AST::Type& type);
        std::unique_ptr<AST::Expression> make_bool_literal(SourceLocation loc,
            bool value);
        std::unique_ptr<AST::Expression> rewrite_pack_expression(
            const AST::PostfixExpression* expr, const Substitution& sub);
        bool pack_type_projection(const AST::PostfixExpression* access,
            const Substitution& sub, std::string& out);
        void expand_pack_arguments(std::vector<AST::GenericArgument>& arguments,
            const Substitution& sub, SourceLocation loc);
        bool evaluate_with_substitution(const AST::Expression* expr, const Substitution& sub,
            ConstantValue& out);
        bool resolve_type_layout(const std::string& type_name, std::size_t& size,
            std::size_t& align, const Substitution& sub) const;
        bool layout_of_composite_type(const AST::Type& type, std::size_t& size,
            std::size_t& align, const Substitution& sub) const;
        std::unique_ptr<AST::Expression> make_typed_constant_literal(SourceLocation loc,
            const ConstantValue& value, const AST::Type& type);

        void push_scope();
        void pop_scope();
        void declare_name(const std::string& name);
        bool name_declared_in_current_scope(const std::string& name) const;
        void register_short_name(const std::string& name, const ShortBinding& binding,
            const std::string& owning_generic, SourceLocation loc);

        void report(SourceLocation loc, ErrorCode code, const std::vector<std::string>& values);
        void report(SourceLocation loc, ErrorCode code, const std::string& message);

        bool process_compile_time_items(
            const std::vector<std::unique_ptr<AST::Statement>>& items,
            const Substitution& sub, std::string& text, SourceLocation& text_loc,
            std::vector<AST::TopLevel*>& member_ptrs,
            std::vector<std::unique_ptr<AST::TopLevel>>& owner);
        bool flush_emit_string(const std::string& text, SourceLocation loc,
            std::vector<AST::TopLevel*>& member_ptrs,
            std::vector<std::unique_ptr<AST::TopLevel>>& owner);
        bool eval_emit_piece(const AST::Expression* expr, const Substitution& sub,
            std::string& out);
        bool eval_compile_time_condition(const AST::Expression* expr, const Substitution& sub,
            bool& out);
        bool eval_bool_property(const AST::Expression* receiver, const std::string& property,
            const std::vector<const AST::Expression*>& args, const Substitution& sub,
            bool& out);
        bool eval_size_align_property(const std::string& parameter, const std::string& property,
            const Substitution& sub, long long& out);
        bool param_type_of(const std::string& name, const Substitution& sub, AST::Type& out) const;
        bool property_argument_type(const std::string& text, const Substitution& sub,
            AST::Type& out) const;
        bool base_type_of(const std::string& name, const Substitution& sub,
            AST::Type& out) const;
        bool property_convertible(const AST::Type& from, const AST::Type& to) const;
        bool decode_string_literal(std::string_view lexeme, std::string& out) const;
        std::string expression_text(const AST::Expression* expr) const;
        std::unique_ptr<AST::Expression> rewrite_property_expression(
            const AST::PostfixExpression* expr, const Substitution& sub);

        void add_top_level(std::unique_ptr<AST::TopLevel> node);

        static std::string signature_text(const std::vector<AST::Type>& types);
        static constexpr int kMaxExpressionExpansionDepth = 64;
        bool validate_expression_body(const AST::GenericParameter& param,
            const Substitution::ExpressionBinding& binding, SourceLocation loc);
        bool statement_is_allowed_in_expression_body(const AST::Statement* stmt,
            SourceLocation& bad_loc, ErrorCode& code, std::string& detail);
        void collect_declared_names(const AST::Statement* stmt,
            std::unordered_set<std::string>& out) const;
        void collect_free_identifiers(const AST::Expression* expr,
            const std::unordered_set<std::string>& bound,
            std::unordered_set<std::string>& out) const;
        void collect_free_identifiers_in_statement(const AST::Statement* stmt,
            std::unordered_set<std::string>& out) const;
        std::unique_ptr<AST::Expression> expand_expression_call(
            const AST::FunctionDefinition* owner, const Substitution& sub,
            const std::string& name,
            std::vector<std::unique_ptr<AST::Expression>>& arguments,
            SourceLocation loc, bool& ok);
    };

}

#endif
