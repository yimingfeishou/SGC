// parser/ast.cpp
// AST 辅助函数实现 —— 类型相等比较与文本表示
// AST helper implementation — type equality and text representation

#include "ast.hpp"

#include <sstream>

namespace gallt {
    namespace AST {

        bool Type::operator==(const Type& other) const {
            // 依次比较类型种类与递归负载；struct 用名称、function 比较完整签名
            // Compare kind and recursive payloads; structs compare by name and functions by signature
            if (kind != other.kind) {
                return false;
            }
            switch (kind) {
            case TypeKind::Int:
            case TypeKind::Float:
            case TypeKind::Double:
            case TypeKind::Char:
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
            switch (kind) {
            case TypeKind::Int:    out << "int"; break;
            case TypeKind::Float:  out << "float"; break;
            case TypeKind::Double: out << "double"; break;
            case TypeKind::Char:   out << "char"; break;
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
                out << "struct " << struct_name;
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
