#ifndef GALLT_SEMANTIC_CONSTEXPR_FUNCTION_HPP
#define GALLT_SEMANTIC_CONSTEXPR_FUNCTION_HPP

#include "../parser/ast.hpp"
#include <cstddef>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace gallt {

    struct ConstexprValue {
        AST::Type type;
        long long int_value = 0;
        double float_value = 0.0;
        std::string string_value;
        std::vector<ConstexprValue> elements;

        bool valid() const { return type.kind != AST::TypeKind::Error; }

        static ConstexprValue invalid();
        static ConstexprValue make_void();
        static ConstexprValue make_integer(long long value, const AST::Type& type);
        static ConstexprValue make_floating(double value, const AST::Type& type);
        static ConstexprValue make_bool(bool value);
        static ConstexprValue make_string(std::string value);
        static ConstexprValue make_struct(std::string name,
            std::vector<ConstexprValue> members);
        static ConstexprValue make_array(const AST::Type& element_type,
            std::vector<ConstexprValue> elements);

        bool is_arithmetic_value() const {
            return type.is_arithmetic();
        }

        bool as_bool() const;
        long long as_integer() const;
        double as_floating() const;
    };

    enum class ConstexprFailure {
        None,
        Unsupported,
        LimitExceeded,
    };

    class ConstexprInterpreter {
    public:
        struct Host {
            std::function<bool(const std::string& name, ConstexprValue& out)>
                lookup_constant;
            std::function<const AST::FunctionDefinition*(const std::string& name,
                const std::vector<ConstexprValue>& arguments)> resolve_function;
            std::function<bool(const AST::Type& type, std::size_t& size,
                std::size_t& align)> layout_of_type;
            std::function<bool(const std::string& name, std::size_t& size,
                std::size_t& align)> layout_of_name;
            std::function<const AST::StructDefinition*(const std::string& name)>
                lookup_struct;
        };

        ConstexprInterpreter() = default;
        explicit ConstexprInterpreter(Host host);

        void set_host(Host host) { host_ = std::move(host); }
        const Host& host() const { return host_; }

        bool call(const AST::FunctionDefinition* function,
            const std::vector<ConstexprValue>& arguments, ConstexprValue& out);

        bool evaluate(const AST::Expression* expr, ConstexprValue& out);

        bool evaluate_initializer(const AST::Initializer* init,
            const AST::Type& type, ConstexprValue& out);

        ConstexprFailure failure() const { return failure_; }

        void reset_failure() { failure_ = ConstexprFailure::None; }

        static bool coerce(const ConstexprValue& value, const AST::Type& target,
            ConstexprValue& out);

        bool default_value(const AST::Type& type, ConstexprValue& out);

        static bool is_compilable_type(const AST::Type& type,
            const std::function<const AST::StructDefinition*(const std::string&)>& lookup);

    private:
        enum class ArithmeticOp { Add, Subtract, Multiply, Divide, Remainder, Power };
        enum class CompareOp { Greater, Less, Equal, NotEqual, GreaterEqual, LessEqual };
        enum class BitwiseOp { And, Xor, Or };
        enum class ShiftOp { Left, Right };
        enum class AssignOp {
            Assign, Add, Subtract, And, Or, Xor, ShiftLeft, ShiftRight
        };

        struct Control {
            enum class Kind { Normal, Break, Return } kind = Kind::Normal;
            ConstexprValue value;
        };

        Host host_;
        std::vector<std::unordered_map<std::string, ConstexprValue>> scopes_;
        ConstexprFailure failure_ = ConstexprFailure::None;
        std::size_t call_depth_ = 0;
        std::size_t steps_ = 0;

        bool fail(ConstexprFailure failure);

        bool eval(const AST::Expression* expr, ConstexprValue& out);
        bool eval_primary(const AST::PrimaryExpression* expr, ConstexprValue& out);
        bool eval_postfix(const AST::PostfixExpression* expr, ConstexprValue& out);
        bool eval_call(const AST::PostfixExpression* expr, ConstexprValue& out);
        bool eval_named_call(const std::string& name,
            const AST::PostfixExpression* call, ConstexprValue& out);
        bool eval_string_builtin(const std::string& name,
            const std::vector<ConstexprValue>& arguments, ConstexprValue& out);
        bool apply_arithmetic(ArithmeticOp op, const ConstexprValue& left,
            const ConstexprValue& right, ConstexprValue& out);
        bool apply_comparison(CompareOp op, const ConstexprValue& left,
            const ConstexprValue& right, ConstexprValue& out);
        bool apply_bitwise(BitwiseOp op, const ConstexprValue& left,
            const ConstexprValue& right, ConstexprValue& out);
        bool apply_shift(ShiftOp op, const ConstexprValue& left,
            const ConstexprValue& right, ConstexprValue& out);
        bool apply_unary(int op, const ConstexprValue& operand, ConstexprValue& out);

        Control exec_statement(const AST::Statement* stmt);
        Control exec_block(const AST::Block* block);
        Control exec_variable_declaration(const AST::VariableDeclaration* decl);
        Control exec_if(const AST::IfStatement* stmt);
        Control exec_for(const AST::ForStatement* stmt);
        Control exec_while(const AST::WhileStatement* stmt);
        Control exec_assignment(const AST::AssignmentExpression* expr);

        bool eval_initializer(const AST::Initializer* init, const AST::Type& type,
            ConstexprValue& out);
        bool eval_array_initializer(const AST::ArrayInitializer* init,
            const AST::Type& type, ConstexprValue& out);

        ConstexprValue* lookup_local(const std::string& name);
        ConstexprValue* lvalue(const AST::Expression* expr);

        bool step();
        bool evaluate_condition(const AST::Expression* expr, bool& out);

        static AST::Type arithmetic_result_type(const ConstexprValue& left,
            const ConstexprValue& right);
        static long long wrap_integer(long long value, AST::TypeKind kind);
        static ConstexprValue wrap_numeric(const ConstexprValue& value);
        ConstexprValue* struct_member(ConstexprValue& container,
            const std::string& member);
    };

}

#endif
