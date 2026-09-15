// parser/ast.cpp
// AST 辅助函数实现 —— 类型相等比较与文本表示
// AST helper implementation — type equality and text representation

#include "ast.hpp"

#include <cctype>
#include <sstream>

namespace gallt {
    namespace AST {

        std::string_view Type::strip_integer_suffix(std::string_view lexeme) noexcept {
            // 后缀最多两个字符且只由 l/L/u/U 组成；hex 数字 A-F 不在集合内，
            // 因此 `0xFF` 不会被误剥离。
            // A suffix is at most two characters from l/L/u/U; hexadecimal digits A-F
            // are not in the set, so `0xFF` is never stripped.
            std::size_t count = 0;
            while (count < 2 && count + 1 < lexeme.size()) {
                const char c = lexeme[lexeme.size() - 1 - count];
                if (c == 'l' || c == 'L' || c == 'u' || c == 'U') {
                    ++count;
                }
                else {
                    break;
                }
            }
            if (count != 0) {
                lexeme.remove_suffix(count);
            }
            return lexeme;
        }

        Type Type::integer_literal_type(std::string_view lexeme) noexcept {
            const std::string_view digits = strip_integer_suffix(lexeme);
            std::string_view suffix = lexeme.substr(digits.size());
            auto lower = [](char c) {
                return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            };
            if (suffix.size() == 2 && lower(suffix[0]) == 'l' && lower(suffix[1]) == 'u') {
                return make_luint();
            }
            if (suffix.size() == 1 && lower(suffix[0]) == 'l') {
                return make_lint();
            }
            if (suffix.size() == 1 && lower(suffix[0]) == 'u') {
                return make_uint();
            }
            // 默认（含无效后缀：词法阶段已报 ER 0025）
            // Default, including invalid suffixes already reported as ER 0025
            return make_int();
        }

        Type Type::float_literal_type(std::string_view lexeme) noexcept {
            if (!lexeme.empty()) {
                const char last = lexeme.back();
                if (last == 'f' || last == 'F') {
                    return make_float();
                }
            }
            return make_double();
        }

        bool Type::operator==(const Type& other) const {
            // 依次比较类型种类与递归负载；struct 用名称、function 比较完整签名
            // Compare kind and recursive payloads; structs compare by name and functions by signature
            //
            // 0.4.1 §2：const 只限制被限定对象的可修改性，不改变值的类型；
            // 因此这里有意忽略 is_const（`const int` 与 `int` 视为同一类型，
            // 与 C++20 的顶层 const 忽略规则一致）。
            // 0.4.1 §2: const only restricts mutability and does not change the type of
            // a value, so is_const is intentionally ignored here (`const int` and `int`
            // compare equal, matching C++20 top-level-const rules).
            if (kind != other.kind) {
                return false;
            }
            switch (kind) {
            case TypeKind::Int:
            case TypeKind::Lint:
            case TypeKind::Uint:
            case TypeKind::Luint:
            case TypeKind::Float:
            case TypeKind::Double:
            case TypeKind::Char:
            case TypeKind::Uchar:
            case TypeKind::Bool:
            case TypeKind::String:
            case TypeKind::File:
            case TypeKind::Void:
                return true;
            case TypeKind::Array:
                if (array_size.has_value() != other.array_size.has_value()) {
                    return false;
                }
                if (array_size.has_value() && *array_size != *other.array_size) {
                    return false;
                }
                return element_type && other.element_type
                    ? *element_type == *other.element_type
                    : element_type == other.element_type;
            case TypeKind::Pointer:
                return pointee_type && other.pointee_type
                    ? *pointee_type == *other.pointee_type
                    : pointee_type == other.pointee_type;
            case TypeKind::Struct:
                return struct_name == other.struct_name;
            case TypeKind::Function:
                if (parameter_types.size() != other.parameter_types.size()) {
                    return false;
                }
                for (size_t i = 0; i < parameter_types.size(); ++i) {
                    if (!(parameter_types[i] == other.parameter_types[i])) {
                        return false;
                    }
                }
                return return_type && other.return_type
                    ? *return_type == *other.return_type
                    : return_type == other.return_type;
            }
            return false;
        }

        std::string Type::to_string() const {
            std::ostringstream out;
            // 顶层 const 限定符（0.4.1 §2）在类型文本中保留，便于诊断显示
            // The top-level const qualifier (0.4.1 §2) is kept in the textual form for
            // diagnostics
            if (is_const) {
                out << "const ";
            }
            switch (kind) {
            case TypeKind::Int:    out << "int"; break;
            case TypeKind::Lint:   out << "lint"; break;
            case TypeKind::Uint:   out << "uint"; break;
            case TypeKind::Luint:  out << "luint"; break;
            case TypeKind::Float:  out << "float"; break;
            case TypeKind::Double: out << "double"; break;
            case TypeKind::Char:   out << "char"; break;
            case TypeKind::Uchar:  out << "uchar"; break;
            case TypeKind::Bool:   out << "bool"; break;
            case TypeKind::String: out << "string"; break;
            case TypeKind::File:   out << "file"; break;
            case TypeKind::Void:   out << "void"; break;
            case TypeKind::Array:
                if (element_type) {
                    out << element_type->to_string();
                } else {
                    out << "?";
                }
                out << '[';
                if (array_size.has_value()) {
                    out << *array_size;
                }
                out << ']';
                break;
            case TypeKind::Pointer:
                if (pointee_type) {
                    out << pointee_type->to_string();
                } else {
                    out << "void";
                }
                out << '*';
                break;
            case TypeKind::Struct:
                if (generic_ref) {
                    // 泛型实例成员按限定名显示，例如 Box<int>.Example
                    // Instantiated generic members print with their qualified name
                    out << generic_ref->to_string();
                } else {
                    out << "struct " << struct_name;
                }
                break;
            case TypeKind::Function:
                if (return_type) {
                    out << return_type->to_string();
                } else {
                    out << "void";
                }
                out << '(';
                for (size_t i = 0; i < parameter_types.size(); ++i) {
                    if (i != 0) {
                        out << ", ";
                    }
                    out << parameter_types[i].to_string();
                }
                out << ")*";
                break;
            }
            return out.str();
        }

    } // namespace AST
} // namespace gallt
