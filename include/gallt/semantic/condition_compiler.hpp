#ifndef GALLT_SEMANTIC_CONDITION_COMPILER_HPP
#define GALLT_SEMANTIC_CONDITION_COMPILER_HPP

#include "../common/diagnostics.hpp"
#include "../parser/ast.hpp"
#include <deque>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace gallt {

    class ConditionCompiler {
    public:
        explicit ConditionCompiler(DiagnosticEngine& diag);

        bool run(AST::Program* program);

    private:
        struct ConditionValue {
            bool defined = false;
            long long value = 0;
        };

        struct Evaluation {
            bool defined = false;
            long long value = 0;
        };

        DiagnosticEngine& diag_;
        std::map<std::string, ConditionValue> conditions_;
        std::deque<std::string> lexeme_pool_;
        bool had_error_ = false;

        void process_top_level_list(std::vector<std::unique_ptr<AST::TopLevel>>& nodes);
        void process_top_level_node(AST::TopLevel* node);
        void process_top_level_statement(std::unique_ptr<AST::Statement>& stmt,
            std::vector<std::unique_ptr<AST::TopLevel>>& kept);
        void process_statement(std::unique_ptr<AST::Statement>& stmt);
        void process_statement_list(std::vector<std::unique_ptr<AST::Statement>>& stmts);
        void process_block(AST::Block* block);

        void handle_condition_definition(AST::CondDefinition* node);
        void handle_condition_removal(AST::UncondDefinition* node);
        bool evaluate_condition(AST::Expression* expr, Evaluation& out);
        bool evaluateNamedCondition(const std::string& name, SourceLocation loc,
            Evaluation& out);
        std::optional<long long> literal_value(const AST::Expression* expr) const;
        bool is_boolean_expression(const AST::Expression* expr) const;
        bool is_condition_name(const AST::Expression* expr) const;
        Token make_bool_token(SourceLocation loc, bool value);
        void transform_expression(AST::Expression* expr);
        void transform_initializer(AST::Initializer* init);
        void resolve_condition_references(AST::Expression* expr);
        void sanitize_condition_references(AST::Expression* expr);
        std::unique_ptr<AST::Expression> make_integer_literal(SourceLocation loc,
            long long value);

        void report(SourceLocation loc, ErrorCode code, const std::vector<std::string>& values);
        void report(SourceLocation loc, ErrorCode code, const std::string& message);
    };

}

#endif
