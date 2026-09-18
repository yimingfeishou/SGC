#ifndef GALLT_SEMANTIC_TYPE_CHECKER_HPP
#define GALLT_SEMANTIC_TYPE_CHECKER_HPP

#include "symbol_table.hpp"
#include "../common/diagnostics.hpp"
#include "../parser/ast.hpp"
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
                expression_argument_casts = {});
        ~TypeChecker() = default;

        TypeChecker(const TypeChecker&) = delete;
        TypeChecker& operator=(const TypeChecker&) = delete;

        bool check_program(AST::Program* program);

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

    private:
        DiagnosticEngine& diag_;
        const std::unordered_map<const AST::Expression*, std::string>&
            expression_free_identifiers_;
        const std::unordered_map<const AST::Expression*,
            std::tuple<std::string, std::size_t, AST::Type>>&
            expression_argument_casts_;
        SymbolTable sym_table_;
        AST::Program* program_ = nullptr;

        AST::FunctionDefinition* current_function_ = nullptr;

        int loop_depth_ = 0;

        bool builtins_declared_ = false;

        std::unordered_map<std::string, AST::StructDefinition*> struct_defs_;

        std::unordered_map<const AST::Expression*, AST::Type> expression_types_;

        std::unordered_map<const AST::PrimaryExpression*, const AST::FunctionDefinition*>
            resolved_functions_;
        std::unordered_map<const AST::PrimaryExpression*, const AST::ExternDeclaration*>
            resolved_externs_;
        std::unordered_map<const AST::Expression*, AST::FunctionDefinition*> resolved_operators_;
        std::unordered_map<std::string, std::vector<AST::FunctionDefinition*>> operator_overloads_;

        void collect_operator_overloads();
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
        void check_if_statement(AST::IfStatement* if_stmt);
        void check_for_statement(AST::ForStatement* for_stmt);
        void check_while_statement(AST::WhileStatement* while_stmt);
        void check_break_statement(AST::BreakStatement* break_stmt);
        void check_return_statement(AST::ReturnStatement* return_stmt);
        void check_expression_statement(AST::ExpressionStatement* expr_stmt);

        AST::Type check_expression(AST::Expression* expr, bool allow_void = false);

        AST::Type check_assignment(AST::AssignmentExpression* expr);
        AST::Type check_logical_or(AST::LogicalOrExpression* expr);
        AST::Type check_logical_and(AST::LogicalAndExpression* expr);
        AST::Type check_comparison(AST::ComparisonExpression* expr);
        AST::Type check_additive(AST::AdditiveExpression* expr);
        AST::Type check_multiplicative(AST::MultiplicativeExpression* expr);
        AST::Type check_power(AST::PowerExpression* expr);
        AST::Type check_unary(AST::UnaryExpression* expr);
        AST::Type check_postfix(AST::PostfixExpression* expr);
        AST::Type check_primary(AST::PrimaryExpression* expr);

        AST::Type check_file_builtin_call(AST::PostfixExpression* expr,
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

        void verify_main_function();

        void declare_builtin_functions();

        bool is_complete_type(const AST::Type& type);

        void check_struct_completeness(AST::StructDefinition* struct_def);

        void check_array_initialization(AST::VariableDeclaration* decl);

        std::optional<size_t> evaluate_const_expression(AST::Expression* expr);
        bool is_constant_integer_expression(AST::Expression* expr, size_t* out_value = nullptr);

        struct ConstValue {
            long long int_value = 0;
            double float_value = 0.0;
            bool is_float = false;
        };

        std::vector<std::unordered_map<std::string, ConstValue>> const_values_;
        void enter_scope();
        void exit_scope();
        void record_const_value(const std::string& name, AST::Expression* initializer,
            const AST::Type& type);
        std::optional<long long> evaluate_const_integer_expression(AST::Expression* expr);
        void check_const_declaration(AST::VariableDeclaration* decl);
        bool is_compile_time_constant_expression(AST::Expression* expr);
        static std::string expression_display_name(const AST::Expression* expr);

    };

} 

#endif 
