#include "ast.hpp"
#include <cctype>
#include <sstream>

namespace gallt {
    namespace AST {

        std::string_view Type::strip_integer_suffix(std::string_view lexeme) noexcept {
            std::size_t count = 0;

            while (count < 2 && count + 1 < lexeme.size()) {
                const char c = lexeme[lexeme.size() - 1 - count];
                if (c == 'l' || c == 'L' || c == 'u' || c == 'U') {
                    ++count;
                } else {
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
            if (kind != other.kind) {
                return false;
            }

            if (is_const != other.is_const) {
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
                if (is_variadic != other.is_variadic) {
                    return false;
                }

                if (parameter_types.size() != other.parameter_types.size()) {
                    return false;
                }

                for (size_t i = 0; i < parameter_types.size(); ++i) {
                    if (!(parameter_types[i] == other.parameter_types[i])) {
                        return false;
                    }
                }

                if (is_variadic) {
                    if (static_cast<bool>(variadic_element_type) !=
                        static_cast<bool>(other.variadic_element_type)) {
                        return false;
                    }

                    if (variadic_element_type && other.variadic_element_type &&
                        !(*variadic_element_type == *other.variadic_element_type)) {
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

                if (is_variadic) {
                    if (!parameter_types.empty()) {
                        out << ", ";
                    }

                    if (variadic_element_type) {
                        out << variadic_element_type->to_string();
                    }

                    out << "...";
                }

                out << ")*";
                break;
            }

            return out.str();
        }

    }
}
