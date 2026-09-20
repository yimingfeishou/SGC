#ifndef GALLT_CODEGEN_CODEGEN_HPP
#define GALLT_CODEGEN_CODEGEN_HPP
#include "../common/diagnostics.hpp"
#include "../parser/ast.hpp"
#include <string>
#include <deque>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace gallt {

    class CodeGenerator {
    public:
        CodeGenerator(AST::Program* program,
            const std::unordered_map<const AST::Expression*, AST::Type>& expression_types,
            const std::unordered_map<const AST::PrimaryExpression*,
                const AST::FunctionDefinition*>& resolved_functions = {},
            const std::unordered_map<const AST::PrimaryExpression*,
                const AST::ExternDeclaration*>& resolved_externs = {},
            const std::unordered_map<const AST::Expression*,
                AST::FunctionDefinition*>& resolved_operators = {},
            DiagnosticEngine* diagnostics = nullptr,
            int debug_symbols_level = 0);

        bool generate();

        const std::string& ir() const { return ir_; }

        const std::vector<std::string>& link_libraries() const { return link_libraries_; }

        static std::string runtime_c_source();

    private:
        AST::Program* program_;
        std::unordered_map<const AST::Expression*, AST::Type> expression_types_;
        std::unordered_map<const AST::PrimaryExpression*, const AST::FunctionDefinition*>
            resolved_functions_;
        const std::unordered_map<const AST::PrimaryExpression*, const AST::ExternDeclaration*>&
            resolved_externs_;
        const std::unordered_map<const AST::Expression*, AST::FunctionDefinition*>&
            resolved_operators_;
        DiagnosticEngine* diagnostics_ = nullptr;
        int debug_level_ = 0;
        bool debug_location_valid_ = false;
        SourceLocation debug_location_;
        unsigned debug_next_id_ = 5;
        unsigned debug_subprogram_id_ = 0;
        unsigned debug_shared_subroutine_id_ = 0;
        unsigned debug_expression_id_ = 0;
        std::string debug_source_file_;
        std::string debug_source_dir_;
        std::vector<std::string> debug_metadata_;
        std::unordered_map<std::string, unsigned> debug_type_ids_;
        std::unordered_map<std::string, unsigned> debug_location_ids_;
        std::unordered_map<std::string, unsigned> debug_subroutine_ids_;
        std::string ir_;
        std::vector<std::string> link_libraries_;
        std::vector<AST::StructDefinition*> struct_defs_;
        std::unordered_map<std::string, AST::StructDefinition*> struct_by_name_;
        struct LocalInfo {
            AST::Type type;
            std::string address;
            bool is_constant = false;
            std::string constant_text;
            const AST::Expression* constant_expr = nullptr;
        };
        struct ConstantNumeric {
            long long int_value = 0;
            double float_value = 0.0;
            bool is_float = false;
        };
        struct CleanupRecord {
            AST::Type type;
            std::string address;
        };
        std::vector<AST::VariableDeclaration*> global_vars_;
        std::vector<AST::VariableDeclaration*> const_globals_;
        std::vector<AST::VariableDeclaration*> runtime_const_globals_;
        std::vector<std::string> constant_aggregate_globals_;
        unsigned constant_aggregate_counter_ = 0;
        std::unordered_map<std::string, ConstantNumeric> constant_values_;
        enum class LifecycleKind {
            Constructor,
            Destructor,
            CopyConstructor,
            MoveConstructor,
            CopyAssignment,
            MoveAssignment
        };
        bool emitting_lifecycle_body_ = false;
        const AST::StructDefinition* lifecycle_owner_ = nullptr;
        std::unordered_set<std::string> lifecycle_symbols_;
        std::unordered_set<std::string> emitted_lifecycle_bodies_;
        std::vector<CleanupRecord> statement_temporaries_;
        std::unordered_map<std::string, LocalInfo> global_symbols_;
        std::unordered_map<std::string, AST::FunctionDefinition*> function_by_name_;
        std::unordered_map<std::string, AST::ExternDeclaration*> extern_by_name_;
        struct StringLiteralConstant {
            std::string bytes;
            std::string llvm_name;
        };
        std::vector<StringLiteralConstant> string_literals_;
        std::unordered_map<std::string, std::string> string_literal_ids_;
        std::vector<std::unique_ptr<AST::Expression>> operator_extra_nodes_;
        std::deque<std::string> lexeme_pool_;

        std::vector<std::string> lines_;
        unsigned temp_counter_ = 0;
        unsigned label_counter_ = 0;
        std::string current_label_;
        std::unordered_set<std::string> emitted_labels_;
        bool current_block_terminated_ = true;

        static bool returns_via_sret(const AST::Type& type) {
            return type.kind == AST::TypeKind::Struct;
        }
        std::string current_sret_pointer_;      
        std::string pending_sret_destination_;  

        std::vector<std::string> hoisted_allocas_;
        std::size_t hoist_insert_index_ = 0;

        std::string new_temp(const char* hint);
        std::string new_label(const char* hint);
        std::string emit_alloca(const std::string& type_text, const char* hint);
        void flush_hoisted_allocas();
        void emit_line(const std::string& line);
        void start_block(const std::string& label);
        std::string llvm_type(const AST::Type& type);
        std::string struct_type_name(const std::string& name) const;

        void collect_structs();
        void collect_structs_in_statement(AST::Statement* stmt);
        bool type_contains_string(const AST::Type& type);

        void emit_preamble();
        void collect_debug_source_file();
        unsigned next_debug_id();
        unsigned debug_type_id(const AST::Type& type);
        unsigned debug_subroutine_id(const AST::Type& return_type,
            const std::vector<AST::Type>& parameters);
        unsigned debug_location_id(const SourceLocation& location);
        void begin_debug_function(AST::FunctionDefinition* func, std::string& suffix);
        void emit_debug_local_variable(const std::string& name, const AST::Type& type,
            const std::string& address, const SourceLocation& location);
        void emit_debug_metadata();
        void emit_struct_types();
        void emit_string_constants();
        void emit_runtime_declarations();
        void emit_function_declarations();
        void emit_functions();
        void emit_lifecycle_functions();
        void register_lifecycle_symbols();
        void emit_lifecycle_body(AST::StructDefinition* def, LifecycleKind kind,
            const std::string& name);
        std::string lifecycle_symbol(const AST::StructDefinition* def,
            LifecycleKind kind) const;
        bool ast_function_exists(const std::string& name) const;
        void collect_global_variables();
        void collect_function_signatures();
        void emit_global_variables();
        void emit_global_initializer();
        void emit_global_deinit_function();
        void emit_main_wrapper();
        void emit_initializer_to_address(AST::VariableDeclaration* decl,
            const std::string& address);
        std::string function_llvm_name_for_source(std::string_view name) const;
        std::string function_reference(const std::string& name);
        std::string function_reference_for(const AST::PrimaryExpression* callee);
        std::string extern_ir_symbol(const AST::ExternDeclaration* ext) const;
        mutable std::unordered_map<const AST::ExternDeclaration*, std::string> extern_ir_symbols_;
        mutable std::unordered_set<std::string> declared_extern_symbols_;
        bool function_is_extern(const std::string& name);
        std::string string_cstr_pointer(const std::string& value);

        void emit_function(AST::FunctionDefinition* func);
        AST::FunctionDefinition* current_function_ = nullptr;
        std::vector<std::string> break_labels_;
        std::vector<std::size_t> break_cleanup_depths_;
        void emit_statement(AST::Statement* stmt);
        void destroy_statement_temporaries();
        void emit_block(AST::Block* block, bool new_scope);
        void emit_variable_declaration(AST::VariableDeclaration* decl);
        bool fold_constant_declaration(AST::VariableDeclaration* decl, LocalInfo& info);
        bool fold_constant_aggregate(AST::VariableDeclaration* decl, LocalInfo& info);
        bool build_constant_initializer(const AST::Initializer* init,
            const AST::Type& type, std::string& out);
        bool build_constant_scalar(const AST::Expression* expr,
            const AST::Type& type, std::string& out);
        void emit_constant_aggregate_globals();
        void emit_expression_statement(AST::ExpressionStatement* stmt);

        std::vector<std::unordered_map<std::string, LocalInfo>> scopes_;
        std::vector<std::vector<CleanupRecord>> cleanup_scopes_;
        LocalInfo* lookup_local(const std::string& name);
        void push_scope();
        void pop_scope();
        void register_string_cleanup(const std::string& address, const AST::Type& type);
        void emit_destroy_string_at(const AST::Type& type, const std::string& address);
        void destroy_active_cleanup_scopes(std::size_t until_depth);
        void discard_current_cleanup_scope();

        struct ExprValue {
            AST::Type type;      
            std::string value;   
            std::string address; 
            bool is_lvalue = false;
            std::string owned_string; 
        };

        ExprValue gen_expr(AST::Expression* expr);
        bool gen_operator_call(AST::Expression* expr, ExprValue& out);
        ExprValue emit_operator_invocation(AST::FunctionDefinition* callee,
            std::vector<AST::Expression*>& final_arguments, bool postfix_dummy,
            SourceLocation loc);
        ExprValue gen_primary(AST::PrimaryExpression* expr);
        ExprValue gen_unary(AST::UnaryExpression* expr);
        ExprValue gen_postfix(AST::PostfixExpression* expr);
        ExprValue gen_binary_string_plus(AST::Expression* left, AST::Expression* right);
        std::string string_value_or_converted(const ExprValue& v, bool* owned_temp = nullptr);
        void destroy_owned_string(ExprValue& value);
        std::string gen_address(AST::Expression* expr);
        std::string gen_pointer_value(AST::Expression* expr);

        void emit_string_assign(const std::string& dest_address, const ExprValue& source);
        void emit_struct_brace_initialization(const std::string& address,
            const AST::Type& struct_type, AST::ArrayInitializer* init);
        void emit_array_brace_initialization(const std::string& address,
            const AST::Type& array_type, AST::ArrayInitializer* init);
        void emit_aggregate_assign(const std::string& dest_address,
            const AST::Type& dest_type, ExprValue& source, bool is_assignment = false);
        void emit_memberwise_copy(const AST::Type& type, const std::string& dst,
            const std::string& src, bool is_assignment);
        void emit_memberwise_move(const AST::Type& type, const std::string& dst,
            const std::string& src, bool is_assignment);
        void emit_deep_copy(const AST::Type& type, const std::string& dst,
            const std::string& src);
        void emit_shallow_copy(const AST::Type& type, const std::string& dst,
            const std::string& src);
        bool type_is_copyable(const AST::Type& type) const;
        bool type_is_movable(const AST::Type& type) const;
        std::string operand_address(AST::Expression* expr, AST::Type* out_type = nullptr);
        void collect_constructor_defaults(const std::string& ctor_name,
            std::vector<AST::Expression*>& args);
        int default_constructor_index(const AST::StructDefinition* def);
        bool emit_struct_default_constructor(const std::string& address,
            const AST::StructDefinition* def, SourceLocation loc);
        void emit_struct_return(AST::Expression* expr, const AST::Type& type,
            const std::string& sret);
        bool is_struct_returning_call(AST::Expression* expr) const;
        bool deep_copyable_pointee(const AST::Type& pointer_type) const;

        std::string convert_value(const std::string& value, const AST::Type& from, const AST::Type& to);
        std::string truth_condition(const std::string& value, const AST::Type& type);
        std::string to_i64_value(const std::string& value, const AST::Type& type);
        AST::Type resolved_type(const AST::Expression* expr) const;

        void emit_output_call(AST::PostfixExpression* call);
        void emit_input_call(AST::PostfixExpression* call, ExprValue& result);
        void emit_free_call(AST::PostfixExpression* call);
        ExprValue emit_size_align_call(AST::PostfixExpression* call, bool is_size);
        ExprValue emit_file_builtin_call(AST::PostfixExpression* call,
            const std::string& name);

        std::size_t type_size(const AST::Type& type) const;
        std::size_t type_align(const AST::Type& type) const;
    };

} 

#endif 
