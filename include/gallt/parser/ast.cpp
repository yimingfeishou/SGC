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
            case TypeKind::Error:
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

        namespace {
            std::string type_text(const Type& type, bool source_form) {
                std::ostringstream out;

                if (type.is_const) {
                    out << "const ";
                }

                switch (type.kind) {
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
                case TypeKind::Error:  out << "error"; break;
                case TypeKind::Array:
                    if (type.element_type) {
                        out << type_text(*type.element_type, source_form);
                    } else {
                        out << "?";
                    }

                    out << '[';

                    if (type.array_size.has_value()) {
                        out << *type.array_size;
                    }

                    out << ']';
                    break;
                case TypeKind::Pointer:
                    if (type.pointee_type) {
                        out << type_text(*type.pointee_type, source_form);
                    } else {
                        out << "void";
                    }

                    out << '*';
                    break;
                case TypeKind::Struct:
                    if (type.generic_ref) {
                        out << type.generic_ref->to_string();
                    } else if (source_form) {
                        out << type.struct_name;
                    } else {
                        out << "struct " << type.struct_name;
                    }
                    break;
                case TypeKind::Function:
                    if (type.return_type) {
                        out << type_text(*type.return_type, source_form);
                    } else {
                        out << "void";
                    }

                    out << '(';

                    for (size_t i = 0; i < type.parameter_types.size(); ++i) {
                        if (i != 0) {
                            out << ", ";
                        }

                        out << type_text(type.parameter_types[i], source_form);
                    }

                    if (type.is_variadic) {
                        if (!type.parameter_types.empty()) {
                            out << ", ";
                        }

                        if (type.variadic_element_type) {
                            out << type_text(*type.variadic_element_type, source_form);
                        }

                        out << "...";
                    }

                    out << ")*";
                    break;
                }

                return out.str();
            }
        }

        std::string Type::to_string() const {
            return type_text(*this, false);
        }

        std::string Type::to_source_string() const {
            return type_text(*this, true);
        }

    }
}
