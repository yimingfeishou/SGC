// parser/ast.hpp
// 抽象语法树 (AST) 节点定义 —— 完整表示 Gallt 语言的所有语法结构
// Abstract Syntax Tree (AST) node definitions — full representation of all Gallt language constructs

#ifndef GALLT_PARSER_AST_HPP
#define GALLT_PARSER_AST_HPP

#include "../common/source_location.hpp"
#include "../common/token.hpp"
#include <vector>
#include <memory>
#include <string>
#include <string_view>
#include <optional>

namespace gallt {
    namespace AST {

        // ============================================================================
        // 前向声明 (Forward declarations)
        // ============================================================================

        class Node;
        class TopLevel;
        class Statement;
        class Expression;
        class Type;
        class Initializer;
        class Program;

        // ============================================================================
        // 基类: AST 节点 (Base class: AST Node)
        // ============================================================================

        class Node {
        public:
            SourceLocation location;

            explicit Node(SourceLocation loc) : location(loc) {}
            virtual ~Node() noexcept = default;

            Node(const Node&) = delete;
            Node& operator=(const Node&) = delete;
            Node(Node&&) = default;
            Node& operator=(Node&&) = default;
        };

        // ============================================================================
        // 类型系统 (Type System)
        // ============================================================================

        enum class TypeKind {
            Int, Float, Double, Char, Bool, String, File, Void,
            Array, Pointer, Struct, Function
        };

        struct Type {
            TypeKind kind;

            // 对于 Array: element_type, size (0 表示未指定)
            std::shared_ptr<Type> element_type;
            std::optional<size_t> array_size;

            // 对于 Pointer: pointee_type
            std::shared_ptr<Type> pointee_type;

            // 对于 Struct: struct_name
            std::string struct_name;

            // 对于 Function: return_type, parameter_types
            std::shared_ptr<Type> return_type;
            std::vector<Type> parameter_types;

            Type() : kind(TypeKind::Void) {}
            explicit Type(TypeKind k) : kind(k) {}

            static Type make_int() { return Type(TypeKind::Int); }
            static Type make_float() { return Type(TypeKind::Float); }
            static Type make_double() { return Type(TypeKind::Double); }
            static Type make_char() { return Type(TypeKind::Char); }
            static Type make_bool() { return Type(TypeKind::Bool); }
            static Type make_string() { return Type(TypeKind::String); }
            static Type make_file() { return Type(TypeKind::File); }
            static Type make_void() { return Type(TypeKind::Void); }

            static Type make_array(std::shared_ptr<Type> elem, std::optional<size_t> size = std::nullopt) {
                Type t(TypeKind::Array);
                t.element_type = elem;
                t.array_size = size;
                return t;
            }

            static Type make_pointer(std::shared_ptr<Type> pointee) {
                Type t(TypeKind::Pointer);
                t.pointee_type = pointee;
                return t;
            }

            static Type make_struct(const std::string& name) {
                Type t(TypeKind::Struct);
                t.struct_name = name;
                return t;
            }

            static Type make_function(std::shared_ptr<Type> ret, const std::vector<Type>& params) {
                Type t(TypeKind::Function);
                t.return_type = ret;
                t.parameter_types = params;
                return t;
            }

            bool operator==(const Type& other) const;
            bool operator!=(const Type& other) const { return !(*this == other); }

            bool is_integer() const {
                return kind == TypeKind::Int || kind == TypeKind::Char || kind == TypeKind::Bool;
            }
            bool is_floating() const {
                return kind == TypeKind::Float || kind == TypeKind::Double;
            }
            bool is_arithmetic() const {
                return is_integer() || is_floating();
            }
            bool is_scalar() const {
                return is_arithmetic() || kind == TypeKind::Pointer || kind == TypeKind::Bool || kind == TypeKind::Char;
            }
            bool is_assignable() const {
                return kind != TypeKind::Void && kind != TypeKind::Function;
            }
            std::string to_string() const;
        };

        // ============================================================================
        // 初始化器 (Initializer)
        // ============================================================================

        class Initializer : public Node {
        public:
            explicit Initializer(SourceLocation loc) : Node(loc) {}
            virtual ~Initializer() noexcept = default;
            virtual bool is_expression() const { return false; }
            virtual bool is_array() const { return false; }
        };

        class ExpressionInitializer : public Initializer {
        public:
            std::unique_ptr<Expression> expr;

            ExpressionInitializer(SourceLocation loc, std::unique_ptr<Expression> e)
                : Initializer(loc), expr(std::move(e)) {
            }

            bool is_expression() const override { return true; }
        };

        class ArrayInitializer : public Initializer {
        public:
            std::vector<std::unique_ptr<Initializer>> elements;

            ArrayInitializer(SourceLocation loc, std::vector<std::unique_ptr<Initializer>> elems)
                : Initializer(loc), elements(std::move(elems)) {
            }

            bool is_array() const override { return true; }
        };

        // ============================================================================
        // 顶层与语句基类 (虚继承，使 StructDefinition 可同时为两者)
        // ============================================================================

        class TopLevel : virtual public Node {
        public:
            explicit TopLevel(SourceLocation loc) : Node(loc) {}
            virtual ~TopLevel() noexcept = default;
        };

        class Statement : virtual public Node {
        public:
            explicit Statement(SourceLocation loc) : Node(loc) {}
            virtual ~Statement() noexcept = default;
            virtual bool is_block() const { return false; }
        };

        // ============================================================================
        // 顶层节点 (Top-Level Nodes)
        // ============================================================================

        // guide "string_literal"
        class GuideStatement : public TopLevel {
        public:
            std::string path;

            GuideStatement(SourceLocation loc, std::string_view p)
                : Node(loc), TopLevel(loc), path(p) {
            }
            virtual ~GuideStatement() noexcept = default;
        };

        // clib("string_literal")
        class ClibStatement : public TopLevel {
        public:
            std::string library_name;

            ClibStatement(SourceLocation loc, std::string_view lib)
                : Node(loc), TopLevel(loc), library_name(lib) {
            }
            virtual ~ClibStatement() noexcept = default;
        };

        // extern return_type identifier from identifier ( parameter_list )
        class ExternDeclaration : public TopLevel {
        public:
            Type return_type;
            std::string name;
            std::string library;
            std::vector<Type> parameters;
            std::vector<std::string> param_names;

            ExternDeclaration(SourceLocation loc, Type ret, std::string_view n,
                std::string_view lib, std::vector<Type> params,
                std::vector<std::string> pnames)
                : Node(loc), TopLevel(loc),
                return_type(std::move(ret)), name(n), library(lib),
                parameters(std::move(params)), param_names(std::move(pnames)) {
            }
            virtual ~ExternDeclaration() noexcept = default;
        };

        // function_definition
        class FunctionDefinition : public TopLevel {
        public:
            Type return_type;
            std::string name;
            std::vector<Type> parameters;
            std::vector<std::string> param_names;
            std::unique_ptr<Statement> body;

            FunctionDefinition(SourceLocation loc, Type ret, std::string_view n,
                std::vector<Type> params, std::vector<std::string> pnames,
                std::unique_ptr<Statement> b)
                : Node(loc), TopLevel(loc),
                return_type(std::move(ret)), name(n),
                parameters(std::move(params)), param_names(std::move(pnames)),
                body(std::move(b)) {
            }
            virtual ~FunctionDefinition() noexcept = default;
        };

        // struct_definition （同时是 TopLevel 和 Statement）
        class StructDefinition : public TopLevel, public Statement {
        public:
            struct Member {
                Type type;
                std::string name;
                std::optional<size_t> array_size;
                std::optional<Type> function_pointer_type;
                std::unique_ptr<Initializer> initializer;
                SourceLocation location;

                Member(SourceLocation loc, Type t, std::string_view n,
                    std::optional<size_t> arr_sz = std::nullopt,
                    std::optional<Type> func_ptr = std::nullopt,
                    std::unique_ptr<Initializer> init = nullptr)
                    : type(std::move(t)), name(n), array_size(arr_sz),
                    function_pointer_type(std::move(func_ptr)),
                    initializer(std::move(init)), location(loc) {
                }
            };

            std::string name;
            std::vector<Member> members;

            StructDefinition(SourceLocation loc, std::string_view n, std::vector<Member> mems)
                : Node(loc), TopLevel(loc), Statement(loc), name(n), members(std::move(mems)) {
            }
            virtual ~StructDefinition() noexcept = default;

            bool is_block() const override { return false; } // 不是块
        };

        // Program 包含所有顶层节点
        class Program : public Node {
        public:
            std::vector<std::unique_ptr<TopLevel>> top_levels;

            Program(SourceLocation loc, std::vector<std::unique_ptr<TopLevel>> tl)
                : Node(loc), top_levels(std::move(tl)) {
            }
            virtual ~Program() noexcept = default;
        };

        // ============================================================================
        // 语句 (Statements)
        // ============================================================================

        // 空语句
        class EmptyStatement : public Statement {
        public:
            explicit EmptyStatement(SourceLocation loc) : Node(loc), Statement(loc) {}
            virtual ~EmptyStatement() noexcept = default;
        };

        // 块语句
        class Block : public Statement {
        public:
            std::vector<std::unique_ptr<Statement>> statements;

            Block(SourceLocation loc, std::vector<std::unique_ptr<Statement>> stmts)
                : Node(loc), Statement(loc), statements(std::move(stmts)) {
            }
            virtual ~Block() noexcept = default;

            bool is_block() const override { return true; }
        };

        // 变量声明
        class VariableDeclaration : public Statement, public TopLevel {
        public:
            Type type;
            std::string name;
            std::optional<size_t> array_size;
            std::optional<Type> function_pointer_type;
            std::unique_ptr<Initializer> initializer;

            VariableDeclaration(SourceLocation loc, Type t, std::string_view n,
                std::optional<size_t> arr_sz = std::nullopt,
                std::optional<Type> func_ptr = std::nullopt,
                std::unique_ptr<Initializer> init = nullptr)
                : Node(loc), Statement(loc), TopLevel(loc),
                type(std::move(t)), name(n),
                array_size(arr_sz), function_pointer_type(std::move(func_ptr)),
                initializer(std::move(init)) {
            }
            virtual ~VariableDeclaration() noexcept = default;
        };

        // if 语句
        class IfStatement : public Statement {
        public:
            std::unique_ptr<Expression> condition;
            std::unique_ptr<Statement> then_block;
            std::unique_ptr<Statement> else_block;

            IfStatement(SourceLocation loc,
                std::unique_ptr<Expression> cond,
                std::unique_ptr<Statement> then_stmt,
                std::unique_ptr<Statement> else_stmt = nullptr)
                : Node(loc), Statement(loc),
                condition(std::move(cond)),
                then_block(std::move(then_stmt)),
                else_block(std::move(else_stmt)) {
            }
            virtual ~IfStatement() noexcept = default;
        };

        // for 语句
        class ForStatement : public Statement {
        public:
            std::unique_ptr<Statement> init;
            std::unique_ptr<Expression> condition;
            std::unique_ptr<Expression> step;
            std::unique_ptr<Statement> body;

            ForStatement(SourceLocation loc,
                std::unique_ptr<Statement> init_stmt,
                std::unique_ptr<Expression> cond,
                std::unique_ptr<Expression> step_expr,
                std::unique_ptr<Statement> body_stmt)
                : Node(loc), Statement(loc),
                init(std::move(init_stmt)),
                condition(std::move(cond)),
                step(std::move(step_expr)),
                body(std::move(body_stmt)) {
            }
            virtual ~ForStatement() noexcept = default;
        };

        // while 语句
        class WhileStatement : public Statement {
        public:
            std::unique_ptr<Expression> condition;
            std::unique_ptr<Statement> body;

            WhileStatement(SourceLocation loc,
                std::unique_ptr<Expression> cond,
                std::unique_ptr<Statement> body_stmt)
                : Node(loc), Statement(loc),
                condition(std::move(cond)),
                body(std::move(body_stmt)) {
            }
            virtual ~WhileStatement() noexcept = default;
        };

        // break 语句
        class BreakStatement : public Statement {
        public:
            explicit BreakStatement(SourceLocation loc) : Node(loc), Statement(loc) {}
            virtual ~BreakStatement() noexcept = default;
        };

        // return 语句
        class ReturnStatement : public Statement {
        public:
            std::unique_ptr<Expression> value;

            ReturnStatement(SourceLocation loc, std::unique_ptr<Expression> val = nullptr)
                : Node(loc), Statement(loc), value(std::move(val)) {
            }
            virtual ~ReturnStatement() noexcept = default;
        };

        // 表达式语句
        class ExpressionStatement : public Statement {
        public:
            std::unique_ptr<Expression> expr;

            ExpressionStatement(SourceLocation loc, std::unique_ptr<Expression> e)
                : Node(loc), Statement(loc), expr(std::move(e)) {
            }
            virtual ~ExpressionStatement() noexcept = default;
        };

        // ============================================================================
        // 表达式 (Expressions)
        // ============================================================================

        class Expression : public Node {
        public:
            explicit Expression(SourceLocation loc) : Node(loc) {}
            virtual ~Expression() noexcept = default;
            virtual bool is_lvalue() const { return false; }
        };

        // 赋值表达式
        class AssignmentExpression : public Expression {
        public:
            enum class Operator { Assign, PlusAssign, MinusAssign };

            std::unique_ptr<Expression> left;
            Operator op;
            std::unique_ptr<Expression> right;

            AssignmentExpression(SourceLocation loc,
                std::unique_ptr<Expression> lhs,
                Operator op_,
                std::unique_ptr<Expression> rhs)
                : Expression(loc), left(std::move(lhs)), op(op_), right(std::move(rhs)) {
            }
            virtual ~AssignmentExpression() noexcept = default;
            bool is_lvalue() const override { return false; }
        };

        // 逻辑或
        class LogicalOrExpression : public Expression {
        public:
            std::unique_ptr<Expression> left;
            std::unique_ptr<Expression> right;

            LogicalOrExpression(SourceLocation loc,
                std::unique_ptr<Expression> lhs,
                std::unique_ptr<Expression> rhs)
                : Expression(loc), left(std::move(lhs)), right(std::move(rhs)) {
            }
            virtual ~LogicalOrExpression() noexcept = default;
        };

        // 逻辑与
        class LogicalAndExpression : public Expression {
        public:
            std::unique_ptr<Expression> left;
            std::unique_ptr<Expression> right;

            LogicalAndExpression(SourceLocation loc,
                std::unique_ptr<Expression> lhs,
                std::unique_ptr<Expression> rhs)
                : Expression(loc), left(std::move(lhs)), right(std::move(rhs)) {
            }
            virtual ~LogicalAndExpression() noexcept = default;
        };

        // 比较表达式
        class ComparisonExpression : public Expression {
        public:
            enum class Operator { Greater, Less, Equal, NotEqual, GreaterEqual, LessEqual };

            std::unique_ptr<Expression> left;
            Operator op;
            std::unique_ptr<Expression> right;

            ComparisonExpression(SourceLocation loc,
                std::unique_ptr<Expression> lhs,
                Operator op_,
                std::unique_ptr<Expression> rhs)
                : Expression(loc), left(std::move(lhs)), op(op_), right(std::move(rhs)) {
            }
            virtual ~ComparisonExpression() noexcept = default;
        };

        // 加法表达式
        class AdditiveExpression : public Expression {
        public:
            enum class Operator { Plus, Minus };

            std::unique_ptr<Expression> left;
            Operator op;
            std::unique_ptr<Expression> right;

            AdditiveExpression(SourceLocation loc,
                std::unique_ptr<Expression> lhs,
                Operator op_,
                std::unique_ptr<Expression> rhs)
                : Expression(loc), left(std::move(lhs)), op(op_), right(std::move(rhs)) {
            }
            virtual ~AdditiveExpression() noexcept = default;
        };

        // 乘法表达式
        class MultiplicativeExpression : public Expression {
        public:
            enum class Operator { Multiply, Divide, Remainder };

            std::unique_ptr<Expression> left;
            Operator op;
            std::unique_ptr<Expression> right;

            MultiplicativeExpression(SourceLocation loc,
                std::unique_ptr<Expression> lhs,
                Operator op_,
                std::unique_ptr<Expression> rhs)
                : Expression(loc), left(std::move(lhs)), op(op_), right(std::move(rhs)) {
            }
            virtual ~MultiplicativeExpression() noexcept = default;
        };

        // 幂运算
        class PowerExpression : public Expression {
        public:
            std::unique_ptr<Expression> left;
            std::unique_ptr<Expression> right;

            PowerExpression(SourceLocation loc,
                std::unique_ptr<Expression> lhs,
                std::unique_ptr<Expression> rhs)
                : Expression(loc), left(std::move(lhs)), right(std::move(rhs)) {
            }
            virtual ~PowerExpression() noexcept = default;
        };

        // 一元表达式
        class UnaryExpression : public Expression {
        public:
            // UnaryPlus/UnaryMinus：一元正负号（与 C 一致，按整数提升规则处理）
            // UnaryPlus/UnaryMinus: standard unary sign operators (C promotion rules)
            enum class Operator {
                Increment, Decrement, LogicalNot, AddressOf, Dereference,
                UnaryPlus, UnaryMinus
            };

            Operator op;
            std::unique_ptr<Expression> operand;

            UnaryExpression(SourceLocation loc,
                Operator op_,
                std::unique_ptr<Expression> operand_)
                : Expression(loc), op(op_), operand(std::move(operand_)) {
            }
            virtual ~UnaryExpression() noexcept = default;

            bool is_lvalue() const override {
                return op == Operator::Dereference;
            }
        };

        // 后缀表达式
        class PostfixExpression : public Expression {
        public:
            enum class Operator {
                Subscript, FunctionCall, Cast,
                Increment, Decrement, Dot, Arrow
            };

            std::unique_ptr<Expression> base;
            Operator op;
            std::unique_ptr<Expression> subscript_expr;
            std::vector<std::unique_ptr<Expression>> arguments;
            Type cast_type;
            std::string member_name;

            PostfixExpression(SourceLocation loc,
                std::unique_ptr<Expression> base_,
                Operator op_,
                std::unique_ptr<Expression> subscript = nullptr,
                std::vector<std::unique_ptr<Expression>> args = {},
                Type cast = Type(),
                std::string_view member = "")
                : Expression(loc),
                base(std::move(base_)), op(op_),
                subscript_expr(std::move(subscript)),
                arguments(std::move(args)),
                cast_type(std::move(cast)),
                member_name(member) {
            }
            virtual ~PostfixExpression() noexcept = default;

            bool is_lvalue() const override {
                return op == Operator::Subscript ||
                    op == Operator::Dot ||
                    op == Operator::Arrow ||
                    op == Operator::Increment ||
                    op == Operator::Decrement;
            }
        };

        // 主要表达式
        class PrimaryExpression : public Expression {
        public:
            enum class Kind {
                Literal, Identifier, Parens, Null, Heap
            };

            Kind kind;

            Token literal_token;
            std::string identifier;
            std::unique_ptr<Expression> paren_expr;
            Type heap_type;
            std::unique_ptr<Expression> heap_size;

            PrimaryExpression(SourceLocation loc, Token lit)
                : Expression(loc), kind(Kind::Literal), literal_token(lit) {
            }

            PrimaryExpression(SourceLocation loc, std::string_view id)
                : Expression(loc), kind(Kind::Identifier), identifier(id) {
            }

            PrimaryExpression(SourceLocation loc, std::unique_ptr<Expression> expr)
                : Expression(loc), kind(Kind::Parens), paren_expr(std::move(expr)) {
            }

            PrimaryExpression(SourceLocation loc, bool /*is_null*/)
                : Expression(loc), kind(Kind::Null) {
            }

            PrimaryExpression(SourceLocation loc, Type type, std::unique_ptr<Expression> size)
                : Expression(loc), kind(Kind::Heap), heap_type(std::move(type)), heap_size(std::move(size)) {
            }

            virtual ~PrimaryExpression() noexcept = default;

            bool is_lvalue() const override {
                return kind == Kind::Identifier;
            }
        };

    } // namespace AST
} // namespace gallt

#endif // GALLT_PARSER_AST_HPP
