#ifndef GALLT_SEMANTIC_TYPE_CHECKER_HPP
#define GALLT_SEMANTIC_TYPE_CHECKER_HPP

#include "symbol_table.hpp"
#include "constexpr_function.hpp"
#include "../common/diagnostics.hpp"
#include "../parser/ast.hpp"
#include <deque>
#include <memory>
#include <vector>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace gallt {

    class TypeChecker {
    public:
        TypeChecker(DiagnosticEngine& diag,
            const std::unordered_map<const AST::Expression*, std::string>&
                expression_free_identifiers = {},
            const std::unordered_map<const AST::Expression*,
                std::tuple<std::string, std::size_t, AST::Type>>&
                expression_argument_casts = {},
            bool require_main = true);
        ~TypeChecker() = default;

        TypeChecker(const TypeChecker&) = delete;
        TypeChecker& operator=(const TypeChecker&) = delete;

        bool check_program(AST::Program* program);

        void apply_constexpr_folding(AST::Program& program);

        bool has_errors() const { return diag_.has_errors(); }

        SymbolTable& get_symbol_table() { return sym_table_; }

        const std::unordered_map<const AST::Expression*, AST::Type>& expression_types() const {
            return expression_types_;
        }

        const std::unordered_map<const AST::PrimaryExpression*, const AST::FunctionDefinition*>&
            resolved_functions() const { return resolved_functions_; }
        const std::unordered_map<const AST::PrimaryExpression*, const AST::ExternDeclaration*>&
            resolved_externs() const { return resolved_externs_; }

        const std::unordered_map<const AST::Expression*, AST::FunctionDefinition*>&
            resolved_operators() const { return resolved_operators_; }

        void set_expression_call_sites(
            const std::unordered_map<const AST::Expression*, AST::Type>& sites) {
            expression_call_sites_ = sites;
        }

    private:
        DiagnosticEngine& diag_;
        const std::unordered_map<const AST::Expression*, std::string>&
            expression_free_identifiers_;
        const std::unordered_map<const AST::Expression*,
            std::tuple<std::string, std::size_t, AST::Type>>&
            expression_argument_casts_;
        SymbolTable sym_table_;
        AST::Program* program_ = nullptr;
        std::unordered_map<const AST::Expression*, AST::Type> expression_call_sites_;
        std::unordered_map<std::string, AST::StructDefinition*> predeclared_structs_;

        AST::FunctionDefinition* current_function_ = nullptr;

        int loop_depth_ = 0;
        bool require_main_ = true;

        bool builtins_declared_ = false;

        std::unordered_map<std::string, AST::StructDefinition*> struct_defs_;

        std::unordered_map<const AST::Expression*, AST::Type> expression_types_;
        std::unordered_set<const AST::FunctionDefinition*> declared_functions_;

        bool is_declared_function(const AST::FunctionDefinition* node) const {
            return node != nullptr && declared_functions_.count(node) != 0;
        }

        std::unordered_map<const AST::PrimaryExpression*, const AST::FunctionDefinition*>
            resolved_functions_;
        std::unordered_map<const AST::PrimaryExpression*, const AST::ExternDeclaration*>
            resolved_externs_;
        std::unordered_map<const AST::Expression*, AST::FunctionDefinition*> resolved_operators_;
        std::unordered_map<std::string, std::vector<AST::FunctionDefinition*>> operator_overloads_;
        std::unordered_set<const AST::FunctionDefinition*> redefined_operators_;

        void collect_operator_overloads();
        struct VariadicPack {
            std::string name;
            AST::Type element;
        };
        std::vector<VariadicPack> variadic_packs_;
        const VariadicPack* find_variadic_pack(const std::string& name) const;
        static bool pack_base_identifier(const AST::Expression* expr,
            std::string& out);
        AST::Type check_pack_property(AST::PostfixExpression* expr,
            const VariadicPack& pack);
        AST::Type check_pack_subscript(AST::PostfixExpression* expr,
            const VariadicPack& pack);
        AST::Type check_pack_get(AST::PostfixExpression* call,
            const VariadicPack& pack);
        bool pack_expansion_target(const AST::Expression* expr,
            const VariadicPack*& out, SourceLocation& loc) const;
        bool expression_mentions_variadic_pack(const AST::Expression* expr) const;
        static AST::Type function_type_of(const Symbol& symbol);
        bool report_expansion_against_fixed_signature(AST::PostfixExpression* call,
            const std::vector<AST::Type>& parameter_types);
        bool report_runtime_pack_expansion_in_initializer(
            const AST::ArrayInitializer* init);
        void check_variadic_argument_list(AST::PostfixExpression* call,
            const std::vector<AST::Type>& fixed_types, const AST::Type& element,
            const AST::FunctionDefinition* node);
        bool validate_operator_definition(AST::FunctionDefinition* node);
        bool is_custom_type(const AST::Type& type) const;
        AST::FunctionDefinition* resolve_user_operator(const std::string& op,
            const std::vector<AST::Type>& operand_types, SourceLocation loc);
        bool try_user_defined_operator(AST::Expression* expr, AST::Type& out);
        std::unordered_map<const AST::FunctionDefinition*, std::string> mangled_functions_;
        std::unordered_map<const AST::ExternDeclaration*, std::string> mangled_externs_;
        std::unordered_set<std::string> used_mangled_names_;
        const AST::Type* expected_type_ = nullptr;

        void check_top_level(AST::TopLevel* node);

        class NodeChecker;
        class FoldRewriter;
        bool validate_export_function(AST::FunctionDefinition* node);
        void check_guide_statement(AST::GuideStatement* node);
        void check_clib_statement(AST::ClibStatement* node);
        void check_extern_declaration(AST::ExternDeclaration* node);
        void check_function_definition(AST::FunctionDefinition* node);
        void check_struct_definition(AST::StructDefinition* node);

        void check_statement(AST::Statement* stmt);
        void check_block(AST::Block* block);
        void check_variable_declaration(AST::VariableDeclaration* decl);
        void check_array_initializer(AST::ArrayInitializer* init, const AST::Type& array_type,
            SourceLocation loc);
        void check_struct_initializer(AST::ArrayInitializer* init, const AST::Type& struct_type,
            SourceLocation loc);
        void check_initializer_expressions(AST::Initializer* init);
        void check_if_statement(AST::IfStatement* if_stmt);
        void check_for_statement(AST::ForStatement* for_stmt);
        void check_while_statement(AST::WhileStatement* while_stmt);
        void check_break_statement(AST::BreakStatement* break_stmt);
        void check_return_statement(AST::ReturnStatement* return_stmt);
        void check_expression_statement(AST::ExpressionStatement* expr_stmt);

        AST::Type check_expression(AST::Expression* expr, bool allow_void = false);

        AST::Type check_assignment(AST::AssignmentExpression* expr);
        AST::Type check_conditional(AST::ConditionalExpression* expr);
        AST::Type check_logical_or(AST::LogicalOrExpression* expr);
        AST::Type check_logical_and(AST::LogicalAndExpression* expr);
        AST::Type check_bitwise(AST::BitwiseExpression* expr);
        AST::Type check_comparison(AST::ComparisonExpression* expr);
        AST::Type check_shift(AST::ShiftExpression* expr);
        AST::Type check_additive(AST::AdditiveExpression* expr);
        AST::Type check_multiplicative(AST::MultiplicativeExpression* expr);
        AST::Type check_power(AST::PowerExpression* expr);
        AST::Type check_unary(AST::UnaryExpression* expr);
        AST::Type check_postfix(AST::PostfixExpression* expr);
        AST::Type check_primary(AST::PrimaryExpression* expr);

        AST::Type check_file_builtin_call(AST::PostfixExpression* expr,
            const std::string& func_name);
        AST::Type check_string_builtin_call(AST::PostfixExpression* expr,
            const std::string& func_name);

        bool can_implicit_convert(const AST::Type& from, const AST::Type& to);

        AST::Type usual_arithmetic_conversion(const AST::Type& left, const AST::Type& right);

        bool is_numeric_type(const AST::Type& type) const;

        bool is_integer_type(const AST::Type& type) const;

        bool is_bool_type(const AST::Type& type) const;

        bool is_pointer_type(const AST::Type& type) const;

        bool is_struct_type(const AST::Type& type) const;

        bool is_function_type(const AST::Type& type) const;

        bool is_array_type(const AST::Type& type) const;

        static AST::Type decay_array_type(const AST::Type& type);

        static bool function_signatures_match(const AST::Type& expected,
            const AST::Type& actual);

        bool is_void_type(const AST::Type& type) const;

        bool is_file_type(const AST::Type& type) const;

        bool is_file_pointer_type(const AST::Type& type) const;

        AST::StructDefinition* get_struct_definition(const std::string& name) const;

        const AST::StructDefinition::Member* get_struct_member(const AST::Type& struct_type,
            std::string_view member_name) const;

        Symbol* lookup_symbol(std::string_view name, bool report_error = true);
        const Symbol* lookup_symbol(std::string_view name, bool report_error = true) const;

        void report_error(SourceLocation loc, ErrorCode code, const std::string& msg);
        void report_error(ErrorCode code, const std::string& msg);
        void report_error_template(SourceLocation loc, ErrorCode code,
            const std::vector<std::string>& values);
        void report_warning(SourceLocation loc, ErrorCode code, const std::string& msg);

        void mangle_overload_set(const std::string& name);
        Symbol* resolve_overload_call(const std::string& name, AST::PostfixExpression* call,
            AST::PrimaryExpression* callee);
        int conversion_rank(const AST::Type& from, const AST::Type& to);
        std::string overload_signature(const std::vector<AST::Type>& params) const;
        std::string unique_mangled_name(const std::string& base);
        void record_function_resolution(AST::PrimaryExpression* callee, const Symbol& symbol);
        Symbol* select_overload_by_target_type(std::vector<Symbol>& set, const AST::Type& target,
            const std::string& name, SourceLocation loc);
        bool overloads_ambiguous_by_defaults(const Symbol& a, const Symbol& b) const;
        Symbol* resolve_constructor_overload(const std::string& struct_name,
            const std::vector<AST::Type>& arg_types, const std::vector<bool>& arg_is_null,
            SourceLocation loc);
        bool type_layout(const AST::Type& type, std::size_t& size, std::size_t& align) const;
        bool type_is_copyable(const AST::Type& type) const;
        bool type_is_movable(const AST::Type& type) const;
        static bool is_move_expression(const AST::Expression* expr);
        static bool is_expression_parameter_temp(const AST::Initializer* init);
        bool is_address_of_const_identifier(const AST::Expression* expr);

        void verify_main_function();

        void declare_builtin_functions();

        bool is_complete_type(const AST::Type& type);
        void collect_local_structs(AST::Statement* stmt);

        std::size_t type_size_of(const AST::Type& type);
        std::size_t type_align_of(const AST::Type& type);
        bool type_layout_of_name(const std::string& name, std::size_t& size,
            std::size_t& align);
        bool type_contains_struct_by_value(const AST::Type& type,
            const std::string& target, std::vector<std::string>& visited);

        std::optional<size_t> evaluate_const_expression(AST::Expression* expr);
        bool is_constant_integer_expression(AST::Expression* expr, size_t* out_value = nullptr);

        ConstexprInterpreter constexpr_interpreter_;
        std::unordered_map<const AST::PostfixExpression*, ConstexprValue>
            constexpr_calls_;
        std::vector<std::unordered_map<std::string, ConstexprValue>>
            constexpr_values_;
        std::deque<std::string> constexpr_lexemes_;
        const AST::FunctionDefinition* constexpr_owner_ = nullptr;
        int constexpr_owner_depth_ = 0;

        void configure_constexpr_host();
        bool constexpr_type_is_compilable(const AST::Type& type,
            std::string& offender_type, std::string& offender_member);
        void validate_constexpr_types(AST::FunctionDefinition* node);
        void validate_constexpr_statement(const AST::Statement* stmt);
        void validate_constexpr_expression(const AST::Expression* expr);
        void validate_constexpr_call_expression(
            const AST::PostfixExpression* call);
        void report_constexpr_operation(const AST::Node* node,
            const std::string& operation, bool side_effect);
        void check_constexpr_call(AST::PostfixExpression* call, const Symbol& callee);
        bool evaluate_constexpr_expression(const AST::Expression* expr,
            ConstexprValue& out);
        bool evaluate_constexpr_initializer(const AST::Initializer* init,
            const AST::Type& type, ConstexprValue& out);
        bool evaluate_constexpr_function_call(
            const AST::FunctionDefinition* function,
            const std::vector<ConstexprValue>& arguments, ConstexprValue& out);
        const AST::FunctionDefinition* resolve_constexpr_function(
            const std::string& name, const std::vector<ConstexprValue>& arguments);
        void record_constexpr_declaration(AST::VariableDeclaration* decl);

        void fold_top_level(AST::TopLevel* node);
        void fold_statement(AST::Statement* stmt);
        void fold_initializer(std::unique_ptr<AST::Initializer>& holder);
        void fold_expression(std::unique_ptr<AST::Expression>& holder);
        void fold_expression_tree(AST::Expression* expr);
        std::unique_ptr<AST::Expression> make_constexpr_literal(
            SourceLocation loc, const ConstexprValue& value);
        std::unique_ptr<AST::Initializer> make_constexpr_initializer(
            SourceLocation loc, const AST::Type& type, const ConstexprValue& value);

        struct ConstValue {
            long long int_value = 0;
            double float_value = 0.0;
            bool is_float = false;
        };

        std::vector<std::unordered_map<std::string, ConstValue>> const_values_;
        int layout_depth_ = 0;
        void enter_scope();
        void exit_scope();
        void record_const_value(const std::string& name, AST::Expression* initializer,
            const AST::Type& type);
        std::optional<long long> evaluate_const_integer_expression(AST::Expression* expr);
        std::optional<long long> evaluate_signed_const_integer_expression(
            AST::Expression* expr);
        void check_const_declaration(AST::VariableDeclaration* decl);
        bool is_compile_time_constant_expression(AST::Expression* expr);
        bool is_constexpr_call_expression(const AST::PostfixExpression* call);
        static std::string expression_display_name(const AST::Expression* expr);

    };

}

#endif
