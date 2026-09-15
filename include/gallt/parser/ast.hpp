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
#include <cstdio>

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

        // ---- 泛型实参 / 泛型引用 (Gallt 0.3.txt §19) ----
        // ---- Generic arguments / generic references (Gallt 0.3.txt §19) ----

        struct GenericArgument;  // 前向声明 / forward declaration
        struct GenericRef;       // 前向声明 / forward declaration

        enum class TypeKind {
            // 0.4.1 §2 新增整型/无符号类型：lint（64 位有符号）、uint（32 位无符号）、
            // luint（64 位无符号）、uchar（8 位无符号）
            // Integer/unsigned kinds added in 0.4.1 §2: lint (64-bit signed),
            // uint (32-bit unsigned), luint (64-bit unsigned) and uchar (8-bit unsigned)
            Int, Lint, Uint, Luint, Float, Double, Char, Uchar, Bool, String, File, Void,
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

            // 泛型实例成员类型（Gallt 0.3.txt §19）：例如 Box<int>.Example
            // Generic instantiated member type, e.g. Box<int>.Example
            std::shared_ptr<GenericRef> generic_ref;

            // 0.4.1 §2 类型限定符 const：被限定对象是编译期常量，不可修改
            // 0.4.1 §2 type qualifier const: the qualified object is a compile-time
            // constant and cannot be modified
            bool is_const = false;

            // 对于 Function: return_type, parameter_types
            std::shared_ptr<Type> return_type;
            std::vector<Type> parameter_types;

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

            static Type make_function(std::shared_ptr<Type> ret, const std::vector<Type>& params) {
                Type t(TypeKind::Function);
                t.return_type = ret;
                t.parameter_types = params;
                return t;
            }

            // ---- 0.4.1 §7 数值字面量后缀 ----
            // ---- 0.4.1 §7 numeric literal suffixes ----
            // 去掉整数字面量后缀（l/L、u/U、lu/Lu/lU/LU）；十六进制数字 A-F 不受影响
            // Strip an integer literal suffix (l/L, u/U, lu/Lu/lU/LU); hex digits A-F
            // are left untouched
            static std::string_view strip_integer_suffix(std::string_view lexeme) noexcept;
            // 整数字面量的默认类型（0.4.1 §7）
            // Default type of an integer literal (0.4.1 §7)
            static Type integer_literal_type(std::string_view lexeme) noexcept;
            // 浮点字面量的默认类型：f/F → float，否则 double（0.4.1 §7）
            // Default type of a floating literal: f/F selects float, otherwise double
            static Type float_literal_type(std::string_view lexeme) noexcept;

            bool operator==(const Type& other) const;
            bool operator!=(const Type& other) const { return !(*this == other); }

            bool is_integer() const {
                return kind == TypeKind::Int || kind == TypeKind::Lint ||
                    kind == TypeKind::Uint || kind == TypeKind::Luint ||
                    kind == TypeKind::Char || kind == TypeKind::Uchar ||
                    kind == TypeKind::Bool;
            }
            // 有符号整数类型：char、int、lint（0.4.1 §2）
            // Signed integer kinds: char, int and lint (0.4.1 §2)
            bool is_signed_integer() const {
                return kind == TypeKind::Char || kind == TypeKind::Int ||
                    kind == TypeKind::Lint;
            }
            // 无符号类型：uchar、uint、luint、bool（0.4.1 §2）
            // Unsigned kinds: uchar, uint, luint and bool (0.4.1 §2)
            bool is_unsigned_integer() const {
                return kind == TypeKind::Uchar || kind == TypeKind::Uint ||
                    kind == TypeKind::Luint || kind == TypeKind::Bool;
            }
            // 0.4.1 §7 类型提升优先级位置：char < uchar < int < uint < lint < luint <
            // float < double。bool 不在文档给出的优先级链上，按 C++20 与 char 同档。
            // Promotion position on the 0.4.1 §7 ladder: char < uchar < int < uint <
            // lint < luint < float < double. bool is not on the documented ladder and
            // shares char's slot per C++20.
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
            // 整数类型的位宽（-1 表示不是整数类型）；与代码生成保持一致
            // Bit width of an integer kind (-1 when not an integer); kept consistent
            // with code generation
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

        // 单个泛型实参：类型实参或编译期常量实参
        // A single generic argument: either a type argument or a compile-time constant
        struct GenericArgument {
            bool is_type = true;              // true: 类型实参；false: 编译期常量实参
            Type type;                        // is_type 时有效
            long long int_value = 0;          // 常量实参的整数取值
            double float_value = 0.0;         // 常量实参的浮点取值
            bool float_constant = false;      // 常量实参是否为浮点
            // Gallt 0.4.txt §19：字符串编译期常量实参（如 Emiter<string, "123">）。
            // 字符串实参必须进入规范化与命名修饰，否则不同字符串常量会得到同一实体。
            // Gallt 0.4.txt §19: string compile-time constant arguments. They must take
            // part in normalization and mangling, otherwise distinct string constants
            // would collapse into one entity.
            bool is_string_constant = false;
            std::string string_constant;      // 已去引号的字符串常量文本
            std::string text;                 // 规范化的实参文本（用于命名修饰与诊断）
            // 常量实参的静态类型（如 string 字面量），用于 ER 0070
            // Static type of a constant argument (e.g. a string literal), used by ER 0070
            Type constant_actual_type;
            // 常量实参的原始表达式：延迟到泛型展开阶段按已绑定常量参数求值
            // （支持 N*2、N+1、int(N)、size(T) 等，Gallt 0.3.txt §19）
            // The original constant-argument expression, evaluated during expansion once the
            // constant parameters are bound (supports N*2, N+1, int(N), size(T), ...)
            std::shared_ptr<Expression> expression;

            std::string normalize() const;
        };

        // 泛型引用：[泛型名]<[实参列表]> [ '.' [成员标识符] ]
        // A generic reference: [generic]<[args]> [ '.' [member] ]
        struct GenericRef {
            std::string generic_name;
            // Gallt 0.4.txt §19/§21：命名空间限定的泛型实例化 `ns1::ns2::Box<int>`。
            // 命名空间降低阶段把 generic_name 改写为内部名（含命名空间路径），并把
            // 源级限定名记入 display_name 供诊断显示。
            // Gallt 0.4.txt §19/§21: namespace-qualified generic instantiation
            // `ns1::ns2::Box<int>`. The namespace lowering pass rewrites generic_name to
            // the internal name (which encodes the namespace path) and records the
            // source-level qualified name in display_name for diagnostics.
            std::vector<std::string> namespace_path;
            std::string display_name;
            std::vector<GenericArgument> arguments;
            std::string member;               // 为空表示整体实例化
            // 成员访问使用的运算符（Gallt 0.4.txt §19）：
            //   true  → `[泛型名]<[实参列表]>::[成员标识符]`，按成员实例化并引入短名
            //   false → `[泛型名]<[实参列表]>.[成员标识符]`，限定名，只直接解析
            // Member-access operator used (Gallt 0.4.txt §19):
            //   true  → per-member instantiation, which also introduces a short name
            //   false → qualified name, which always resolves directly
            bool member_scope_access = false;
            SourceLocation location;

            // 规范化文本，例如 Box<int>.Example
            std::string to_string() const;
            // 命名的规范化（命名修饰），例如 Box$int$Example
            std::string mangle() const;
        };

        inline std::string GenericArgument::normalize() const {
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
            // 诊断显示：优先使用源级限定名（命名空间降低后 generic_name 是内部名）
            // Diagnostics prefer the source-level qualified name: after namespace
            // lowering `generic_name` holds the internal (mangled) name
            std::string out;
            if (!display_name.empty()) {
                out = display_name;
            }
            else {
                for (const std::string& part : namespace_path) {
                    out += part;
                    out += "::";
                }
                out += generic_name;
            }
            out += "<";
            for (std::size_t i = 0; i < arguments.size(); ++i) {
                if (i != 0) out += ", ";
                out += arguments[i].normalize();
            }
            out += ">";
            if (!member.empty()) {
                // Gallt 0.4.txt §19：按成员实例化用 `::`，限定名用 `.`
                // Gallt 0.4.txt §19: per-member instantiation uses `::`, qualified names `.`
                out += (member_scope_access ? "::" : ".") + member;
            }
            return out;
        }

        inline std::string GenericRef::mangle() const {
            // 命名修饰至少包含泛型名、实参列表的规范化表示与成员名
            // Name mangling carries at least the generic name, the normalized argument
            // list, and the member name
            //
            // Gallt 0.4.txt §19/§21：命名空间限定的泛型实例化把命名空间路径并入
            // 命名修饰（命名空间降低阶段会把路径折叠进 generic_name 并清空
            // namespace_path，因此两种状态都产生同一结果）
            // Namespace-qualified instantiations fold the namespace path into the
            // mangled name (namespace lowering folds the path into generic_name and
            // clears namespace_path, so both states produce the same result)
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
                        }
                        else {
                            // 其它字符用十六进制转义，保证命名修饰始终产生合法标识符
                            // Escape anything else so mangled names stay valid identifiers
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
            // C 库导出的符号名：声明被重载命名修饰后 name 会变化，C 符号名保持不变
            // The C symbol exported by the library: `name` may be mangled for overloading,
            // while the C symbol stays stable
            std::string c_symbol_name;
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
            // 默认参数（Gallt 0.3.txt §8）：与 parameters 等长，nullptr 表示无默认值
            // Default arguments (Gallt 0.3.txt §8): same length as parameters, nullptr = none
            std::vector<std::unique_ptr<Expression>> param_defaults;
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

        // 结构体特殊成员函数声明（Gallt 0.3.txt §20）
        // Struct special member function declaration (Gallt 0.3.txt §20)
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
            Type parameter_type;             // copy/move 系列：源对象指针类型
            std::string parameter_name;      // copy/move 系列：源对象形参名
            std::vector<Type> parameters;    // 构造函数形参
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

        // struct_definition （同时是 TopLevel 和 Statement）
        class StructDefinition : public TopLevel, public Statement {
        public:
            struct Member {
                Type type;
                std::string name;
                std::optional<size_t> array_size;
                // 非常量字面量的数组长度表达式（泛型编译期常量参数 N 等）
                // Non-literal array length expression (e.g. generic constant parameter N)
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
            // [nocopy] / [nomove] 标记（Gallt 0.3.txt §20）
            // [nocopy] / [nomove] attributes (Gallt 0.3.txt §20)
            bool no_copy = false;
            bool no_move = false;
            // 0.3 §20：降低后的特殊成员信息（由生命周期降低阶段填写）
            // 0.3 §20: lowered special-member info (filled by the lifecycle lowering)
            bool needs_destruction = false;
            std::string destructor_name;
            std::string copy_constructor_name;
            std::string move_constructor_name;
            std::string copy_assignment_name;
            std::string move_assignment_name;
            // 降低后的构造函数列表（供 T(args) 值临时对象按实参选择）
            // Lowered constructor list (lets T(args) value temporaries pick by arguments)
            std::vector<std::string> constructor_names;
            std::vector<std::vector<Type>> constructor_param_types;
            // 构造函数重载集名字（第 18/20 章）：所有 constructor 降低为同名重载，
            // 由类型检查器按实参类型做重载决议（二义性报 ER 0096）
            // Shared overload name of the constructors: all `constructor`s lower to one
            // overloaded name resolved by the type checker (ambiguity -> ER 0096)
            std::string ctor_overload_name;
            // 特殊成员函数定义（Gallt 0.3.txt §20）
            // Special member function definitions (Gallt 0.3.txt §20)
            std::vector<std::unique_ptr<SpecialMemberFunction>> special_members;

            StructDefinition(SourceLocation loc, std::string_view n, std::vector<Member> mems)
                : Node(loc), TopLevel(loc), Statement(loc), name(n), members(std::move(mems)) {
            }
            virtual ~StructDefinition() noexcept = default;

            bool is_block() const override { return false; } // 不是块
        };

        // ---- 编译期泛型 (Gallt 0.3.txt §19) ----
        // ---- Compile-time generics (Gallt 0.3.txt §19) ----

        // 类型参数约束
        // Type parameter constraint
        struct GenericConstraint {
            enum class Kind { Any, Struct, Pointer, Array, File, Types };
            Kind kind = Kind::Any;
            std::vector<Type> types;          // Kind::Types 时的具体类型集合（含单个具体类型）
            std::string text;                 // 原始文本，用于诊断

            std::string to_string() const {
                if (!text.empty()) return text;
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

        // 泛型参数（类型参数或编译期常量参数）
        // Generic parameter (type parameter or compile-time constant parameter)
        struct GenericParameter {
            std::string name;
            bool is_type = true;              // true: 类型参数；false: 编译期常量参数
            Type constant_type;               // 常量参数声明的类型（[类型] N）
            bool constant_type_is_parameter = false; // （[泛型参数] N）
            std::string constant_type_parameter;
            GenericConstraint constraint;     // 类型参数的约束
            SourceLocation location;
        };

        // 特化/偏特化模式实参
        // Specialization pattern argument
        struct GenericPatternArg {
            bool is_constant = false;
            // 自由的编译期常量模式（把主泛型形态的常量参数转为偏特化时使用）：
            // 该位置匹配任意编译期常量
            // Free compile-time constant pattern (used when a primary-shaped definition
            // is reinterpreted as a partial specialization): matches any constant
            bool free_constant = false;
            Type type;                        // 类型模式（自由标识符以 Struct 名称表示）
            long long int_value = 0;
            double float_value = 0.0;
            bool float_constant = false;
            // Gallt 0.4.txt §19（修正后）：字符串字面量是编译期常量表达式的允许成分，
            // 因此字符串常量可以作为特化/偏特化的常量模式
            // Gallt 0.4.txt §19 (corrected): string literals are allowed components of
            // compile-time constant expressions, so they can be constant patterns
            bool string_constant = false;
            std::string string_value;
            std::string text;
        };

        // generics [标识符]<[参数列表]> { ... }
        class GenericDefinition : public TopLevel, public Statement {
        public:
            std::string name;
            std::vector<GenericParameter> parameters;
            std::vector<GenericPatternArg> patterns;   // 特化/偏特化时非空
            bool is_specialization = false;
            // 形参列表形态：true 表示全部为参数声明（主泛型形态）
            // Shape of the argument list: true when it is a parameter list (primary shape)
            bool primary_shaped = true;
            std::vector<std::unique_ptr<TopLevel>> members; // StructDefinition / FunctionDefinition / 非法语句
            // Gallt 0.4.txt §19：泛型块顶部的编译期代码生成语句（emit / 编译期 if），
            // 按源码从上到下的顺序保存
            // Gallt 0.4.txt §19: compile-time code-generation items at the top of a
            // generic block (emit / compile-time if), kept in source order
            std::vector<std::unique_ptr<Statement>> compile_time_items;
            SourceLocation location;

            GenericDefinition(SourceLocation loc, std::string_view n)
                : Node(loc), TopLevel(loc), Statement(loc), name(n), location(loc) {
            }
            virtual ~GenericDefinition() noexcept = default;
        };

        // 实例化语句：Box<int> 或 Box<int>.Example
        class InstantiationStatement : public TopLevel, public Statement {
        public:
            GenericRef reference;

            InstantiationStatement(SourceLocation loc, GenericRef ref)
                : Node(loc), TopLevel(loc), Statement(loc), reference(std::move(ref)) {
            }
            virtual ~InstantiationStatement() noexcept = default;
        };

        // ---- Gallt 0.4.txt §21：命名空间 ----
        // ---- Gallt 0.4.txt §21: namespaces ----

        // namespace [命名空间标识符] { ... }
        class NamespaceDefinition : public TopLevel, public Statement {
        public:
            std::string name;
            // 成员声明：结构体 / 函数 / 全局变量 / 泛型 / 嵌套命名空间 /
            // access namespace / addition namespace / 实例化语句
            // Member declarations: structs, functions, global variables, generics,
            // nested namespaces, access/addition namespace and instantiations
            std::vector<std::unique_ptr<TopLevel>> members;
            SourceLocation location;

            NamespaceDefinition(SourceLocation loc, std::string_view n)
                : Node(loc), TopLevel(loc), Statement(loc), name(n), location(loc) {
            }
            virtual ~NamespaceDefinition() noexcept = default;
        };

        // access namespace [命名空间标识符[/命名空间中的对象]]
        class AccessNamespaceStatement : public TopLevel, public Statement {
        public:
            // 路径分量：`mynamespace` 或 `mynamespace` + `i`（单成员引入）
            // Path components: `mynamespace`, or `mynamespace` plus `i`
            std::vector<std::string> path;
            SourceLocation location;

            AccessNamespaceStatement(SourceLocation loc, std::vector<std::string> p)
                : Node(loc), TopLevel(loc), Statement(loc), path(std::move(p)), location(loc) {
            }
            virtual ~AccessNamespaceStatement() noexcept = default;
        };

        // addition namespace [命名空间标识符] { ... }
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

        // Program 包含所有顶层节点
        class Program : public Node {
        public:
            std::vector<std::unique_ptr<TopLevel>> top_levels;
            // Gallt 0.3.txt §20：全局对象的构造调用（在 main 之前执行）
            // Gallt 0.3.txt §20: constructor calls for global objects (run before main)
            std::vector<std::unique_ptr<Statement>> global_initializers;

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
            std::unique_ptr<Expression> array_size_expr;
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

        // destruct [表达式]（Gallt 0.3.txt §20：手动析构并释放）
        // destruct [expression] (Gallt 0.3.txt §20: manual destruction + release)
        class DestructStatement : public Statement {
        public:
            std::unique_ptr<Expression> target;
            // 降低后的析构函数名（由生命周期降低阶段填写）
            // Lowered destructor name (filled by the lifecycle lowering)
            std::string lowered_dtor;

            DestructStatement(SourceLocation loc, std::unique_ptr<Expression> t)
                : Node(loc), Statement(loc), target(std::move(t)) {
            }
            virtual ~DestructStatement() noexcept = default;
        };

        // emit "[要插入的字符串内容]" / emit { ... }（Gallt 0.4.txt §19）
        // Emit String / Emit Block (Gallt 0.4.txt §19)
        //
        // 该节点只能出现在泛型块顶部（或其编译期 if 分支内），否则报 ER 0106。
        // This node may only appear at the top of a generic block (or inside a
        // compile-time if branch there); anywhere else it is ER 0106.
        class EmitStatement : public Statement {
        public:
            // Emit String：按顺序拼接的编译期字符串片段（逗号分隔的片段也按顺序拼接）
            // Emit String: compile-time string pieces concatenated in order (both `+`
            // chains and comma-separated pieces concatenate, §19 examples)
            std::vector<std::unique_ptr<Expression>> pieces;
            // Emit Block：AST 插入内容（成员定义等）
            // Emit Block: the AST content to insert (member definitions, ...)
            // 元素可能是成员定义（FunctionDefinition / StructDefinition）或语句
            // Elements may be member definitions (FunctionDefinition / StructDefinition)
            // or statements, so the common Node base is used
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
            // 默认参数补齐（Gallt 0.3.txt §8）：指向被调函数定义中的默认值表达式
            // Filled-in default arguments (Gallt 0.3.txt §8): non-owning pointers into the
            // callee's parameter defaults
            std::vector<Expression*> appended_defaults;
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

        // 编译期属性表达式（Gallt 0.4.txt §19）
        // Compile-time property expression (Gallt 0.4.txt §19)
        //
        // 形如 `[参数标识符].is_same<[参数标识符], ...>`、`.is_convertible<...>`、
        // `.has_member<"[成员标识符]", ...>`：带 `<...>` 实参列表的属性无法用普通
        // 后缀 `.` 表达（`<` 会被当作比较运算符），因此在语法阶段单独建节点。
        // Properties carrying a `<...>` argument list cannot be written with a plain
        // postfix `.` (the `<` would parse as a comparison), so they get a node of
        // their own. `.size` / `.align` / `.typename` / `.is_*` stay ordinary Dot
        // postfix expressions and are interpreted by the expander.
        class CompileTimePropertyExpression : public Expression {
        public:
            std::unique_ptr<Expression> receiver;     // 接收者（泛型参数标识符）
            std::string property;                     // is_same / is_convertible / has_member
            std::vector<std::unique_ptr<Expression>> arguments;  // <...> 中的实参

            CompileTimePropertyExpression(SourceLocation loc,
                std::unique_ptr<Expression> recv, std::string_view prop,
                std::vector<std::unique_ptr<Expression>> args)
                : Expression(loc), receiver(std::move(recv)), property(prop),
                arguments(std::move(args)) {
            }
            virtual ~CompileTimePropertyExpression() noexcept = default;
        };

        // 主要表达式
        class PrimaryExpression : public Expression {
        public:
            enum class Kind {
                Literal, Identifier, Parens, Null, Heap,
                // 0.3 新增：泛型实例成员限定名、construct、copy/move 系列
                // Added in 0.3: generic qualified name, construct, copy/move family
                QualifiedName, Construct, CopyMove, PlacementConstruct,
                // 0.4 新增：命名空间限定名 `A::B::member`（Gallt 0.4.txt §21）
                // Added in 0.4: namespace-qualified name (Gallt 0.4.txt §21)
                NamespaceQualified
            };

            // copy/move 系列内建操作
            // copy/move family builtin operations
            enum class CopyMoveKind { Copy, Move, DeepCopy, ShallowCopy };

            Kind kind;

            Token literal_token;
            std::string identifier;
            std::unique_ptr<Expression> paren_expr;
            Type heap_type;
            std::unique_ptr<Expression> heap_size;

            // Kind::QualifiedName：Box<int>.Example
            std::shared_ptr<GenericRef> generic_ref;
            // Kind::NamespaceQualified：`A::B::member` 的完整分量（由名字空间降低
            // 阶段切分为命名空间路径 + 成员名）
            // Kind::NamespaceQualified: the full component list of `A::B::member`
            // (split into namespace path + member by the namespace lowering pass)
            std::vector<std::string> qualified_path;
            // Kind::Construct / PlacementConstruct：construct T(args) [at ptr]
            Type construct_type;
            std::vector<std::unique_ptr<Expression>> construct_args;
            std::unique_ptr<Expression> placement_target;
            // 降低后的构造函数名（由生命周期降低阶段填写）
            // Lowered constructor name (filled by the lifecycle lowering)
            std::string lowered_ctor;
            // true 表示该 construct 表达式是“值临时对象”（T(args) 作为表达式），
            // 其存储位于栈上，并在所在完整表达式结束时析构（Gallt 0.3.txt §20）
            // True when this construct expression is a value temporary T(args); it lives on
            // the stack and is destroyed at the end of its full expression
            bool value_temporary = false;
            // Kind::CopyMove
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

            // 注意：不要提供 `PrimaryExpression(loc, bool)` 之类的重载——
            // `"literal"` 会优先按标准转换匹配到 bool，从而静默构造出 Null 节点。
            // Note: never add a `(loc, bool)` overload — a `const char*` argument would
            // bind to it through the standard conversion and silently build a Null node.
            // 空指针字面量请使用 make_null。
            // Use make_null for the null-pointer literal.
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

            // 命名空间限定名：A::B::member（Gallt 0.4.txt §21）
            // Namespace-qualified name: A::B::member (Gallt 0.4.txt §21)
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

    } // namespace AST
} // namespace gallt

#endif // GALLT_PARSER_AST_HPP
