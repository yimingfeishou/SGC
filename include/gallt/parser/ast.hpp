#ifndef GALLT_PARSER_AST_HPP
#define GALLT_PARSER_AST_HPP

#include "../common/source_location.hpp"
#include "../common/token.hpp"
#include <vector>
#include <memory>
#include <string>
#include <string_view>
#include <optional>
#include <cstdio>

namespace gallt {
    namespace AST {

        class Node;
        class TopLevel;
        class Statement;
        class Expression;
        class Type;
        class Initializer;
        class Program;

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

        struct GenericArgument;
        struct GenericRef;

        enum class TypeKind {
            Int, Lint, Uint, Luint, Float, Double, Char, Uchar, Bool, String, File, Void,
            Array, Pointer, Struct, Function
        };

        struct Type {
            TypeKind kind;

            std::shared_ptr<Type> element_type;
            std::optional<size_t> array_size;

            std::shared_ptr<Type> pointee_type;

            std::string struct_name;

            std::shared_ptr<GenericRef> generic_ref;

            bool is_const = false;

            std::shared_ptr<Type> return_type;
            std::vector<Type> parameter_types;
            bool is_variadic = false;
            std::shared_ptr<Type> variadic_element_type;

            Type() : kind(TypeKind::Void) {}

            explicit Type(TypeKind k) : kind(k) {}

            static Type make_int() { return Type(TypeKind::Int); }

            static Type make_lint() { return Type(TypeKind::Lint); }

            static Type make_uint() { return Type(TypeKind::Uint); }

            static Type make_luint() { return Type(TypeKind::Luint); }

            static Type make_float() { return Type(TypeKind::Float); }

            static Type make_double() { return Type(TypeKind::Double); }

            static Type make_char() { return Type(TypeKind::Char); }

            static Type make_uchar() { return Type(TypeKind::Uchar); }

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

            static Type make_function(std::shared_ptr<Type> ret,
                const std::vector<Type>& params, bool variadic = false,
                std::shared_ptr<Type> element = nullptr) {
                Type t(TypeKind::Function);
                t.return_type = ret;
                t.parameter_types = params;
                t.is_variadic = variadic;
                t.variadic_element_type = std::move(element);
                return t;
            }

            static std::string_view strip_integer_suffix(std::string_view lexeme) noexcept;
            static Type integer_literal_type(std::string_view lexeme) noexcept;
            static Type float_literal_type(std::string_view lexeme) noexcept;

            bool operator==(const Type& other) const;
            bool operator!=(const Type& other) const { return !(*this == other); }

            bool is_integer() const {
                return kind == TypeKind::Int || kind == TypeKind::Lint ||
                    kind == TypeKind::Uint || kind == TypeKind::Luint ||
                    kind == TypeKind::Char || kind == TypeKind::Uchar ||
                    kind == TypeKind::Bool;
            }

            bool is_signed_integer() const {
                return kind == TypeKind::Char || kind == TypeKind::Int ||
                    kind == TypeKind::Lint;
            }

            bool is_unsigned_integer() const {
                return kind == TypeKind::Uchar || kind == TypeKind::Uint ||
                    kind == TypeKind::Luint || kind == TypeKind::Bool;
            }

            int promotion_rank() const {
                switch (kind) {
                case TypeKind::Char:
                case TypeKind::Bool: return 1;
                case TypeKind::Uchar: return 2;
                case TypeKind::Int: return 3;
                case TypeKind::Uint: return 4;
                case TypeKind::Lint: return 5;
                case TypeKind::Luint: return 6;
                case TypeKind::Float: return 7;
                case TypeKind::Double: return 8;
                default: return 0;
                }
            }

            int integer_bit_width() const {
                switch (kind) {
                case TypeKind::Char:
                case TypeKind::Uchar:
                case TypeKind::Bool: return 8;
                case TypeKind::Int:
                case TypeKind::Uint: return 32;
                case TypeKind::Lint:
                case TypeKind::Luint: return 64;
                default: return -1;
                }
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

        struct ExpressionParameterBody;

        struct GenericArgument {
            bool is_type = true;
            bool is_pack_expansion = false;
            std::string pack_name;
            Type type;
            long long int_value = 0;
            double float_value = 0.0;
            bool float_constant = false;
            bool is_string_constant = false;
            std::string string_constant;
            std::string text;
            Type constant_actual_type;
            std::shared_ptr<Expression> expression;
            bool is_expr = false;
            std::string expr_name;
            std::vector<Type> expr_param_types;
            std::vector<std::string> expr_param_names;
            Type expr_return_type;
            std::shared_ptr<ExpressionParameterBody> expr_body;
            bool expr_shorthand = false;

            std::string normalize() const;
        };

        struct ExpressionParameterBody {
            SourceLocation location;
            std::vector<std::unique_ptr<Statement>> statements;
        };

        struct GenericRef {
            std::string generic_name;
            std::vector<std::string> namespace_path;
            std::string display_name;
            std::vector<GenericArgument> arguments;
            std::string member;
            bool member_scope_access = false;
            SourceLocation location;

            std::string to_string() const;
            std::string mangle() const;
        };

        inline std::string GenericArgument::normalize() const {
            if (is_pack_expansion) {
                return pack_name + "...";
            }

            if (is_expr) {
                std::string out = "expr " + expr_name + "(";
                for (std::size_t i = 0; i < expr_param_types.size(); ++i) {
                    if (i != 0) { out += ","; }
                    out += expr_param_types[i].to_string();
                }
                out += ")";
                out += text;
                return out;
            }

            if (is_type) {
                return type.to_string();
            }

            if (is_string_constant) {
                return "\"" + string_constant + "\"";
            }

            if (float_constant) {
                char buf[64];
                std::snprintf(buf, sizeof(buf), "%g", float_value);
                return std::string(buf);
            }

            return std::to_string(int_value);
        }

        inline std::string GenericRef::to_string() const {
            std::string out;
            if (!display_name.empty()) {
                out = display_name;
            } else {
                for (const std::string& part : namespace_path) {
                    out += part;
                    out += "::";
                }
                out += generic_name;
            }

            out += "<";

            for (std::size_t i = 0; i < arguments.size(); ++i) {
                if (i != 0) { out += ", "; }
                out += arguments[i].normalize();
            }

            out += ">";

            if (!member.empty()) {
                out += (member_scope_access ? "::" : ".") + member;
            }

            return out;
        }

        inline std::string GenericRef::mangle() const {
            std::string out;
            for (const std::string& part : namespace_path) {
                out += part;
                out += "$";
            }

            out += generic_name;

            for (const GenericArgument& arg : arguments) {
                out += "$";
                std::string norm = arg.normalize();
                for (char c : norm) {
                    switch (c) {
                    case '*': out += "P"; break;
                    case '[': out += "A"; break;
                    case ']': out += "Z"; break;
                    case '<': out += "L"; break;
                    case '>': out += "G"; break;
                    case ',': out += "C"; break;
                    case ' ': break;
                    case '.': out += "D"; break;
                    case ':': out += "S"; break;
                    case '\"': out += "Q"; break;
                    case '$': out += "X"; break;
                    default:
                        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                            (c >= '0' && c <= '9') || c == '_') {
                            out += c;
                        } else {
                            char buffer[8];
                            std::snprintf(buffer, sizeof(buffer), "_%02X",
                                static_cast<unsigned>(static_cast<unsigned char>(c)));
                            out += buffer;
                        }
                        break;
                    }
                }
            }

            if (!member.empty()) {
                out += "$" + member;
            }
            return out;
        }

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
            bool from_paren_call = false;

            ArrayInitializer(SourceLocation loc, std::vector<std::unique_ptr<Initializer>> elems)
                : Initializer(loc), elements(std::move(elems)) {
            }

            bool is_array() const override { return true; }
        };

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

        class GuideStatement : public TopLevel {
        public:
            std::string path;

            GuideStatement(SourceLocation loc, std::string_view p)
                : Node(loc), TopLevel(loc), path(p) {
            }
            virtual ~GuideStatement() noexcept = default;
        };

        class ClibStatement : public TopLevel {
        public:
            std::string library_name;

            ClibStatement(SourceLocation loc, std::string_view lib)
                : Node(loc), TopLevel(loc), library_name(lib) {
            }
            virtual ~ClibStatement() noexcept = default;
        };

        class ExternDeclaration : public TopLevel {
        public:
            Type return_type;
            std::string name;
            std::string c_symbol_name;
            std::string library;
            std::vector<Type> parameters;
            std::vector<std::string> param_names;
            bool c_variadic = false;

            ExternDeclaration(SourceLocation loc, Type ret, std::string_view n,
                std::string_view lib, std::vector<Type> params,
                std::vector<std::string> pnames)
                : Node(loc), TopLevel(loc),
                return_type(std::move(ret)), name(n), library(lib),
                parameters(std::move(params)), param_names(std::move(pnames)) {
            }
            virtual ~ExternDeclaration() noexcept = default;
        };

        class FunctionDefinition : public TopLevel {
        public:
            Type return_type;
            std::string name;
            std::vector<Type> parameters;
            std::vector<std::string> param_names;
            std::vector<std::unique_ptr<Expression>> param_defaults;
            std::unique_ptr<Statement> body;
            bool is_operator = false;
            std::string overloaded_operator;
            bool is_conversion_operator = false;
            Type conversion_target_type;
            bool operator_postfix_dummy = false;
            bool is_export = false;
            bool is_variadic = false;

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

        struct SpecialMemberFunction {
            enum class Kind {
                Constructor,
                Destructor,
                CopyConstructor,
                MoveConstructor,
                CopyAssignment,
                MoveAssignment,
            };

            Kind kind = Kind::Constructor;
            Type parameter_type;
            std::string parameter_name;
            std::vector<Type> parameters;
            std::vector<std::string> parameter_names;
            std::vector<std::unique_ptr<Expression>> parameter_defaults;
            std::unique_ptr<Statement> body;
            SourceLocation location;

            SpecialMemberFunction(SourceLocation loc, Kind k) : kind(k), location(loc) {}

            const char* kind_name() const {
                switch (kind) {
                case Kind::Constructor: return "constructor";
                case Kind::Destructor: return "destructor";
                case Kind::CopyConstructor: return "copy_constructor";
                case Kind::MoveConstructor: return "move_constructor";
                case Kind::CopyAssignment: return "copy_assignment";
                case Kind::MoveAssignment: return "move_assignment";
                }
                return "special_member";
            }
        };

        class StructDefinition : public TopLevel, public Statement {
        public:
            struct Member {
                Type type;
                std::string name;
                std::optional<size_t> array_size;
                std::unique_ptr<Expression> array_size_expr;
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
            bool no_copy = false;
            bool no_move = false;
            bool needs_destruction = false;
            std::string destructor_name;
            std::string copy_constructor_name;
            std::string move_constructor_name;
            std::string copy_assignment_name;
            std::string move_assignment_name;
            std::vector<std::string> constructor_names;
            std::vector<std::vector<Type>> constructor_param_types;
            std::string ctor_overload_name;
            std::vector<std::unique_ptr<SpecialMemberFunction>> special_members;

            StructDefinition(SourceLocation loc, std::string_view n, std::vector<Member> mems)
                : Node(loc), TopLevel(loc), Statement(loc), name(n), members(std::move(mems)) {
            }
            virtual ~StructDefinition() noexcept = default;

            bool is_block() const override { return false; }
        };

        struct GenericConstraint {
            enum class Kind { Any, Struct, Pointer, Array, File, Types };
            Kind kind = Kind::Any;
            std::vector<Type> types;
            std::string text;

            std::string to_string() const {
                if (!text.empty()) { return text; }
                switch (kind) {
                case Kind::Any: return "any";
                case Kind::Struct: return "struct";
                case Kind::Pointer: return "pointer";
                case Kind::Array: return "array";
                case Kind::File: return "file";
                case Kind::Types: break;
                }
                return "any";
            }
        };

        struct GenericParameter {
            std::string name;
            bool is_type = true;
            bool is_pack = false;
            Type constant_type;
            bool constant_type_is_parameter = false;
            std::string constant_type_parameter;
            GenericConstraint constraint;
            SourceLocation location;
            bool is_expr = false;
            std::vector<Type> expr_param_types;
            std::vector<std::string> expr_param_names;
            Type expr_return_type;
        };

        struct GenericPatternArg {
            bool is_constant = false;
            bool free_constant = false;
            Type type;
            long long int_value = 0;
            double float_value = 0.0;
            bool float_constant = false;
            bool string_constant = false;
            std::string string_value;
            std::string text;
        };

        class GenericDefinition : public TopLevel, public Statement {
        public:
            std::string name;
            std::vector<GenericParameter> parameters;
            std::vector<GenericPatternArg> patterns;
            bool is_specialization = false;
            bool primary_shaped = true;
            std::vector<std::unique_ptr<TopLevel>> members;
            std::vector<std::unique_ptr<Statement>> compile_time_items;
            SourceLocation location;

            GenericDefinition(SourceLocation loc, std::string_view n)
                : Node(loc), TopLevel(loc), Statement(loc), name(n), location(loc) {
            }
            virtual ~GenericDefinition() noexcept = default;
        };

        class InstantiationStatement : public TopLevel, public Statement {
        public:
            GenericRef reference;

            InstantiationStatement(SourceLocation loc, GenericRef ref)
                : Node(loc), TopLevel(loc), Statement(loc), reference(std::move(ref)) {
            }
            virtual ~InstantiationStatement() noexcept = default;
        };

        class NamespaceDefinition : public TopLevel, public Statement {
        public:
            std::string name;
            std::vector<std::unique_ptr<TopLevel>> members;
            SourceLocation location;

            NamespaceDefinition(SourceLocation loc, std::string_view n)
                : Node(loc), TopLevel(loc), Statement(loc), name(n), location(loc) {
            }
            virtual ~NamespaceDefinition() noexcept = default;
        };

        class AccessNamespaceStatement : public TopLevel, public Statement {
        public:
            std::vector<std::string> path;
            SourceLocation location;

            AccessNamespaceStatement(SourceLocation loc, std::vector<std::string> p)
                : Node(loc), TopLevel(loc), Statement(loc), path(std::move(p)), location(loc) {
            }
            virtual ~AccessNamespaceStatement() noexcept = default;
        };

        class AdditionNamespaceStatement : public TopLevel, public Statement {
        public:
            std::string name;
            std::vector<std::unique_ptr<TopLevel>> members;
            SourceLocation location;

            AdditionNamespaceStatement(SourceLocation loc, std::string_view n)
                : Node(loc), TopLevel(loc), Statement(loc), name(n), location(loc) {
            }
            virtual ~AdditionNamespaceStatement() noexcept = default;
        };

        class CondDefinition : public TopLevel, public Statement {
        public:
            std::string name;
            std::unique_ptr<Expression> value;

            CondDefinition(SourceLocation loc, std::string_view n,
                std::unique_ptr<Expression> v)
                : Node(loc), TopLevel(loc), Statement(loc), name(n),
                value(std::move(v)) {
            }
            virtual ~CondDefinition() noexcept = default;
        };

        class UncondDefinition : public TopLevel, public Statement {
        public:
            std::string name;

            UncondDefinition(SourceLocation loc, std::string_view n)
                : Node(loc), TopLevel(loc), Statement(loc), name(n) {
            }
            virtual ~UncondDefinition() noexcept = default;
        };

        class ConditionalBlock : public TopLevel, public Statement {
        public:
            std::unique_ptr<Expression> condition;
            std::unique_ptr<Statement> then_block;
            std::unique_ptr<Statement> else_block;

            ConditionalBlock(SourceLocation loc,
                std::unique_ptr<Expression> cond,
                std::unique_ptr<Statement> then_stmt,
                std::unique_ptr<Statement> else_stmt)
                : Node(loc), TopLevel(loc), Statement(loc),
                condition(std::move(cond)),
                then_block(std::move(then_stmt)),
                else_block(std::move(else_stmt)) {
            }
            virtual ~ConditionalBlock() noexcept = default;
        };

        class TopLevelBlock : public TopLevel, public Statement {
        public:
            std::vector<std::unique_ptr<TopLevel>> items;

            explicit TopLevelBlock(SourceLocation loc)
                : Node(loc), TopLevel(loc), Statement(loc) {
            }
            virtual ~TopLevelBlock() noexcept = default;
        };

        class Program : public Node {
        public:
            std::vector<std::unique_ptr<TopLevel>> top_levels;
            std::vector<std::unique_ptr<Statement>> global_initializers;

            Program(SourceLocation loc, std::vector<std::unique_ptr<TopLevel>> tl)
                : Node(loc), top_levels(std::move(tl)) {
            }
            virtual ~Program() noexcept = default;
        };

        class EmptyStatement : public Statement {
        public:
            explicit EmptyStatement(SourceLocation loc) : Node(loc), Statement(loc) {}
            virtual ~EmptyStatement() noexcept = default;
        };

        class Block : public Statement {
        public:
            std::vector<std::unique_ptr<Statement>> statements;

            Block(SourceLocation loc, std::vector<std::unique_ptr<Statement>> stmts)
                : Node(loc), Statement(loc), statements(std::move(stmts)) {
            }
            virtual ~Block() noexcept = default;

            bool is_block() const override { return true; }
        };

        class VariableDeclaration : public Statement, public TopLevel {
        public:
            Type type;
            std::string name;
            std::optional<size_t> array_size;
            std::unique_ptr<Expression> array_size_expr;
            std::optional<Type> function_pointer_type;
            std::unique_ptr<Initializer> initializer;

            bool constructed_by_lowering = false;

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

        class BreakStatement : public Statement {
        public:
            explicit BreakStatement(SourceLocation loc) : Node(loc), Statement(loc) {}
            virtual ~BreakStatement() noexcept = default;
        };

        class ReturnStatement : public Statement {
        public:
            std::unique_ptr<Expression> value;

            ReturnStatement(SourceLocation loc, std::unique_ptr<Expression> val = nullptr)
                : Node(loc), Statement(loc), value(std::move(val)) {
            }
            virtual ~ReturnStatement() noexcept = default;
        };

        class ExpressionStatement : public Statement {
        public:
            std::unique_ptr<Expression> expr;

            ExpressionStatement(SourceLocation loc, std::unique_ptr<Expression> e)
                : Node(loc), Statement(loc), expr(std::move(e)) {
            }
            virtual ~ExpressionStatement() noexcept = default;
        };

        class DestructStatement : public Statement {
        public:
            std::unique_ptr<Expression> target;
            std::string lowered_dtor;

            DestructStatement(SourceLocation loc, std::unique_ptr<Expression> t)
                : Node(loc), Statement(loc), target(std::move(t)) {
            }
            virtual ~DestructStatement() noexcept = default;
        };

        class EmitStatement : public Statement {
        public:
            std::vector<std::unique_ptr<Expression>> pieces;
            std::vector<std::unique_ptr<Node>> block_items;
            bool is_block = false;

            EmitStatement(SourceLocation loc, std::vector<std::unique_ptr<Expression>> p)
                : Node(loc), Statement(loc), pieces(std::move(p)), is_block(false) {
            }
            explicit EmitStatement(SourceLocation loc)
                : Node(loc), Statement(loc), is_block(true) {
            }
            virtual ~EmitStatement() noexcept = default;
        };

        class Expression : public Node {
        public:
            explicit Expression(SourceLocation loc) : Node(loc) {}
            virtual ~Expression() noexcept = default;
            virtual bool is_lvalue() const { return false; }
        };

        class AssignmentExpression : public Expression {
        public:
            enum class Operator {
                Assign, PlusAssign, MinusAssign, AndAssign, OrAssign, XorAssign,
                ShiftLeftAssign, ShiftRightAssign
            };

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

        class ConditionalExpression : public Expression {
        public:
            std::unique_ptr<Expression> condition;
            std::unique_ptr<Expression> then_expr;
            std::unique_ptr<Expression> else_expr;

            ConditionalExpression(SourceLocation loc,
                std::unique_ptr<Expression> cond,
                std::unique_ptr<Expression> then_value,
                std::unique_ptr<Expression> else_value)
                : Expression(loc), condition(std::move(cond)),
                then_expr(std::move(then_value)), else_expr(std::move(else_value)) {
            }
            virtual ~ConditionalExpression() noexcept = default;
        };

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

        class BitwiseExpression : public Expression {
        public:
            enum class Operator { And, Xor, Or };

            std::unique_ptr<Expression> left;
            Operator op;
            std::unique_ptr<Expression> right;

            BitwiseExpression(SourceLocation loc,
                std::unique_ptr<Expression> lhs,
                Operator op_,
                std::unique_ptr<Expression> rhs)
                : Expression(loc), left(std::move(lhs)), op(op_), right(std::move(rhs)) {
            }
            virtual ~BitwiseExpression() noexcept = default;
        };

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

        class ShiftExpression : public Expression {
        public:
            enum class Operator { Left, Right };

            std::unique_ptr<Expression> left;
            Operator op;
            std::unique_ptr<Expression> right;

            ShiftExpression(SourceLocation loc,
                std::unique_ptr<Expression> lhs,
                Operator op_,
                std::unique_ptr<Expression> rhs)
                : Expression(loc), left(std::move(lhs)), op(op_), right(std::move(rhs)) {
            }
            virtual ~ShiftExpression() noexcept = default;
        };

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

        class UnaryExpression : public Expression {
        public:
            enum class Operator {
                Increment, Decrement, LogicalNot, AddressOf, Dereference,
                UnaryPlus, UnaryMinus, BitwiseNot
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

        class PostfixExpression : public Expression {
        public:
            enum class Operator {
                Subscript, FunctionCall, Cast,
                Increment, Decrement, Dot, Arrow, PackExpand
            };

            std::unique_ptr<Expression> base;
            Operator op;
            std::unique_ptr<Expression> subscript_expr;
            std::vector<std::unique_ptr<Expression>> arguments;
            std::vector<Expression*> appended_defaults;
            std::vector<Expression*> borrowed_arguments;
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

        class CompileTimePropertyExpression : public Expression {
        public:
            std::unique_ptr<Expression> receiver;
            std::string property;
            std::vector<std::unique_ptr<Expression>> arguments;

            CompileTimePropertyExpression(SourceLocation loc,
                std::unique_ptr<Expression> recv, std::string_view prop,
                std::vector<std::unique_ptr<Expression>> args)
                : Expression(loc), receiver(std::move(recv)), property(prop),
                arguments(std::move(args)) {
            }
            virtual ~CompileTimePropertyExpression() noexcept = default;
        };

        class PrimaryExpression : public Expression {
        public:
            enum class Kind {
                Literal, Identifier, Parens, Null, Heap,
                QualifiedName, Construct, CopyMove, PlacementConstruct,
                NamespaceQualified
            };

            enum class CopyMoveKind { Copy, Move, DeepCopy, ShallowCopy };

            Kind kind;

            Token literal_token;
            std::string identifier;
            std::unique_ptr<Expression> paren_expr;
            Type heap_type;
            std::unique_ptr<Expression> heap_size;

            std::shared_ptr<GenericRef> generic_ref;
            std::vector<std::string> qualified_path;
            Type construct_type;
            std::vector<std::unique_ptr<Expression>> construct_args;
            std::unique_ptr<Expression> placement_target;
            std::string lowered_ctor;
            bool value_temporary = false;
            CopyMoveKind copy_move_kind = CopyMoveKind::Copy;

            PrimaryExpression(SourceLocation loc, Token lit)
                : Expression(loc), kind(Kind::Literal), literal_token(lit) {
            }

            PrimaryExpression(SourceLocation loc, std::string_view id)
                : Expression(loc), kind(Kind::Identifier), identifier(id) {
            }

            PrimaryExpression(SourceLocation loc, std::unique_ptr<Expression> expr)
                : Expression(loc), kind(Kind::Parens), paren_expr(std::move(expr)) {
            }

            static std::unique_ptr<PrimaryExpression> make_null(SourceLocation loc) {
                auto node = std::make_unique<PrimaryExpression>(loc, std::string_view("null"));
                node->kind = Kind::Null;
                return node;
            }

            PrimaryExpression(SourceLocation loc, Type type, std::unique_ptr<Expression> size)
                : Expression(loc), kind(Kind::Heap), heap_type(std::move(type)), heap_size(std::move(size)) {
            }

            PrimaryExpression(SourceLocation loc, GenericRef ref)
                : Expression(loc), kind(Kind::QualifiedName),
                generic_ref(std::make_shared<GenericRef>(std::move(ref))) {
            }

            PrimaryExpression(SourceLocation loc, std::vector<std::string> path)
                : Expression(loc), kind(Kind::NamespaceQualified),
                qualified_path(std::move(path)) {
            }

            PrimaryExpression(SourceLocation loc, Type type,
                std::vector<std::unique_ptr<Expression>> args,
                std::unique_ptr<Expression> placement)
                : Expression(loc),
                kind(placement ? Kind::PlacementConstruct : Kind::Construct),
                construct_type(std::move(type)),
                construct_args(std::move(args)),
                placement_target(std::move(placement)) {
            }

            PrimaryExpression(SourceLocation loc, CopyMoveKind cm, std::unique_ptr<Expression> operand)
                : Expression(loc), kind(Kind::CopyMove), copy_move_kind(cm),
                paren_expr(std::move(operand)) {
            }

            virtual ~PrimaryExpression() noexcept = default;

            bool is_lvalue() const override {
                return kind == Kind::Identifier;
            }
        };

    }
}

#endif
