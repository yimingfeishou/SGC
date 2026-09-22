#ifndef GALLT_PARSER_PARSER_HPP
#define GALLT_PARSER_PARSER_HPP

#include "ast.hpp"
#include "../lexer/lexer.hpp"
#include "../common/diagnostics.hpp"
#include <vector>
#include <memory>
#include <optional>
#include <unordered_set>

namespace gallt {

    class Parser {
    public:
        Parser(Lexer& lexer, DiagnosticEngine& diag);
        ~Parser() = default;

        Parser(const Parser&) = delete;
        Parser& operator=(const Parser&) = delete;

        std::unique_ptr<AST::Program> parse();

        void seed_generic_names(const std::unordered_set<std::string>& names);

        const std::unordered_set<std::string>& generic_names() const {
            return seen_generics_;
        }

    private:
        Lexer& lexer_;
        DiagnosticEngine& diag_;
        Token current_;
        Token peek_;
        bool has_peek_ = false;
        bool has_error_ = false;

        mutable std::vector<Token> tokens_;
        std::size_t token_index_ = 0;
        mutable bool lexer_exhausted_ = false;

        void ensure_tokens(std::size_t n) const;
        Token lookahead(std::size_t n) const;
        TokenType lookahead_type(std::size_t n) const;
        std::size_t mark() const { return token_index_; }
        void reset_to(std::size_t m);

        std::unordered_set<std::string> seen_generics_;

        std::unordered_set<std::string> declared_type_names_;
        std::unordered_set<std::string> declared_value_names_;

        int loop_depth_ = 0;

        static constexpr int kMaxExpressionNesting = 1024;
        static constexpr std::size_t kMaxExpressionTokens = 10000;
        static constexpr int kMaxBlockNesting = 1024;
        static constexpr int kMaxInitializerNesting = 1024;

        int expression_depth_ = 0;
        std::size_t expression_tokens_ = 0;
        bool complexity_limit_hit_ = false;
        int block_depth_ = 0;
        int initializer_depth_ = 0;

        bool in_error_recovery_ = false;
        int generic_ct_depth_ = 0;
        bool in_expr_argument_ = false;

        void advance();
        void peek_token();
        bool expect(TokenType type, const std::string& err_msg);
        bool match(TokenType type);
        void report_error(ErrorCode code, const std::string& msg);
        void report_error_at(SourceLocation loc, ErrorCode code, const std::string& msg);
        void report_error_template(ErrorCode code, const std::vector<std::string>& values);
        void report_error_template_at(SourceLocation loc, ErrorCode code,
            const std::vector<std::string>& values);

        void skip_newlines();

        void synchronize();

        std::unique_ptr<AST::TopLevel> parse_top_level();
        std::unique_ptr<AST::GuideStatement> parse_guide_statement();
        std::unique_ptr<AST::ClibStatement> parse_clib_statement();
        std::unique_ptr<AST::ExternDeclaration> parse_extern_declaration();
        std::unique_ptr<AST::TopLevel> parse_function_definition(
            bool exported = false, SourceLocation export_location = SourceLocation{});
        std::unique_ptr<AST::TopLevel> parse_export_definition();
        std::unique_ptr<AST::TopLevel> parse_export_member(const char* context);
        const char* export_kind_for_token(TokenType type) const;
        bool at_operator_definition() const;
        bool at_operator_parameter_list() const;
        bool at_operator_symbol(TokenType type) const;
        bool compound_shift_assignment() const;
        bool shift_followed_by_assign() const;
        bool at_shift_or_assign_head() const;
        std::unique_ptr<AST::TopLevel> parse_operator_definition();
        bool looks_like_operator_definition() const;
        std::string operator_token_text() const;
        std::string expression_argument_text(std::size_t from, std::size_t to) const;
        std::unique_ptr<AST::StructDefinition> parse_struct_definition();
        std::unique_ptr<AST::NamespaceDefinition> parse_namespace_definition();
        std::unique_ptr<AST::AccessNamespaceStatement> parse_access_namespace();
        std::unique_ptr<AST::AdditionNamespaceStatement> parse_addition_namespace();
        std::vector<std::unique_ptr<AST::TopLevel>> parse_namespace_members();
        std::unique_ptr<AST::EmitStatement> parse_emit_statement(bool inside_generic);
        std::unique_ptr<AST::TopLevel> parse_condition_statement();
        std::unique_ptr<AST::Statement> parse_condition_node(bool top_level);
        std::unique_ptr<AST::ConditionalBlock> parse_conditional_block(bool top_level);
        std::unique_ptr<AST::Statement> parse_conditional_branch_top_level();
        std::unique_ptr<AST::Expression> parse_condition_expression();
        std::unique_ptr<AST::Statement> parse_top_level_block();
        std::unique_ptr<AST::Statement> parse_generic_compile_time_item();
        bool emit_item_starts_with_function_definition() const;
        std::unique_ptr<AST::Expression> parse_property_argument();
        bool check_identifier_name(std::string& out, const char* context);
        std::unique_ptr<AST::TopLevel> parse_generic_definition();
        bool generic_param_list_is_primary_shaped() const;
        std::vector<AST::GenericArgument> parse_generic_arguments();
        AST::GenericRef parse_generic_reference(std::string_view name);
        AST::GenericConstraint parse_generic_constraint();
        std::unique_ptr<AST::TopLevel> parse_generic_member();
        std::unique_ptr<AST::Expression> parse_compile_time_expression();
        std::unique_ptr<AST::SpecialMemberFunction> parse_special_member_function();
        bool at_special_member_keyword() const;
        bool looks_like_generic_instantiation() const;
        std::size_t scan_generic_instantiation_end() const;
        std::size_t scan_expr_argument_instantiation_end() const;
        bool at_struct_attribute() const;
        bool at_declaration_start() const;
        std::unique_ptr<AST::VariableDeclaration> parse_variable_declaration_with_type(
            AST::Type base_type, bool allow_empty_array = true);

        std::unique_ptr<AST::Statement> parse_statement();
        std::unique_ptr<AST::Statement> parse_declaration_or_statement();
        std::unique_ptr<AST::VariableDeclaration> parse_variable_declaration();
        std::unique_ptr<AST::IfStatement> parse_if_statement();
        std::unique_ptr<AST::ForStatement> parse_for_statement();
        std::unique_ptr<AST::WhileStatement> parse_while_statement();
        std::unique_ptr<AST::BreakStatement> parse_break_statement();
        std::unique_ptr<AST::ReturnStatement> parse_return_statement();
        std::unique_ptr<AST::Block> parse_block();
        std::unique_ptr<AST::ExpressionStatement> parse_expression_statement();
        std::unique_ptr<AST::EmptyStatement> parse_empty_statement();

        AST::Type parse_type(bool allow_void = true,
            bool allow_function_suffix = true);
        AST::Type parse_type_specifier(bool allow_void = true);

        std::optional<AST::Type> parse_function_pointer_suffix(AST::Type base_type);

        AST::Type finish_declarator_type(AST::Type base, bool allow_empty_array,
            std::optional<size_t>* out_array_size = nullptr,
            std::unique_ptr<AST::Expression>* out_size_expr = nullptr);

        std::pair<std::vector<AST::Type>, std::vector<std::string>> parse_parameter_list(
            std::vector<std::unique_ptr<AST::Expression>>* defaults = nullptr);

        std::unique_ptr<AST::Initializer> parse_initializer();

        std::unique_ptr<AST::Expression> parse_expression();
        std::unique_ptr<AST::Expression> parse_assignment_expression();
        std::unique_ptr<AST::Expression> parse_conditional_expression();
        std::unique_ptr<AST::Expression> parse_logical_or_expression();
        std::unique_ptr<AST::Expression> parse_logical_and_expression();
        std::unique_ptr<AST::Expression> parse_bitwise_expression();
        std::unique_ptr<AST::Expression> parse_comparison_expression();
        std::unique_ptr<AST::Expression> parse_shift_expression();
        std::unique_ptr<AST::Expression> parse_additive_expression();
        std::unique_ptr<AST::Expression> parse_multiplicative_expression();
        std::unique_ptr<AST::Expression> parse_power_expression();
        std::unique_ptr<AST::Expression> parse_unary_expression();
        std::unique_ptr<AST::Expression> parse_postfix_expression();
        std::unique_ptr<AST::Expression> parse_primary_expression();

        std::unique_ptr<AST::Expression> parse_postfix_operator(
            std::unique_ptr<AST::Expression> base);

        bool looks_like_pointer_type_cast() const;
        bool cast_type_has_function_pointer_suffix() const;
        bool declaration_name_after_star_suffix(std::size_t index) const;
        bool declaration_name_after_pointer_suffix(std::size_t index) const;

        std::vector<std::unique_ptr<AST::Expression>> parse_argument_list();

        std::unique_ptr<AST::Statement> parse_for_init();

        std::unique_ptr<AST::Expression> parse_optional_expression();

        bool is_stmt_end() const;
        bool expect_stmt_end(const std::string& context);

        std::optional<size_t> evaluate_constant_expression(AST::Expression* expr);
        std::optional<size_t> try_parse_integer_literal();

        SourceLocation current_location() const;

        bool is_valid_identifier(const std::string& name) const;
    };

}

#endif
