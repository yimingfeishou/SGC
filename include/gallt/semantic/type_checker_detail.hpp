#ifndef GALLT_SEMANTIC_TYPE_CHECKER_DETAIL_HPP
#define GALLT_SEMANTIC_TYPE_CHECKER_DETAIL_HPP

#include "../semantic/type_checker.hpp"
#include "../parser/ast.hpp"
#include "../semantic/constant_folding.hpp"
#include <algorithm>
#include <cctype>
#include <charconv>
#include <system_error>
#include <unordered_set>
#include <functional>

namespace gallt {
    using namespace AST;

    namespace type_checker_detail {
        inline bool is_null_literal_expr(const AST::Expression* expr) {
            auto* prim = dynamic_cast<const AST::PrimaryExpression*>(expr);
            return prim != nullptr && prim->kind == AST::PrimaryExpression::Kind::Null;
        }

        inline bool is_legal_export_identifier(const std::string& name) {
            if (name.empty()) return false;
            const unsigned char first = static_cast<unsigned char>(name.front());
            if (!(std::isalpha(first) || first == '_')) return false;
            for (char c : name) {
                const unsigned char value = static_cast<unsigned char>(c);
                if (!(std::isalnum(value) || value == '_')) return false;
            }
            return true;
        }

        inline const char* export_conflict_kind_name(SymbolKind kind) {
            switch (kind) {
            case SymbolKind::Variable: return "variable";
            case SymbolKind::Parameter: return "variable";
            case SymbolKind::Struct: return "type";
            case SymbolKind::FunctionPointer: return "function pointer declaration";
            default: return "declaration";
            }
        }

        enum class FileParamKind {
            FileHandle,
            Buffer,
            IntValue,
            SizeValue,
            StringValue,
            TextValue,
            SeekOrigin
        };

        enum class FileResultKind {
            FileHandlePtr,
            Bool,
            Int,
            String
        };

        struct FileBuiltinInfo {
            std::vector<FileParamKind> params;
            FileResultKind result;
        };

        inline const std::unordered_map<std::string, FileBuiltinInfo>& file_builtin_table() {
            static const std::unordered_map<std::string, FileBuiltinInfo> table = {
                { "fileopen",       { { FileParamKind::StringValue, FileParamKind::StringValue }, FileResultKind::FileHandlePtr } },
                { "fileclose",      { { FileParamKind::FileHandle }, FileResultKind::Bool } },
                { "fileflush",      { { FileParamKind::FileHandle }, FileResultKind::Bool } },
                { "fileread",       { { FileParamKind::FileHandle, FileParamKind::Buffer, FileParamKind::SizeValue }, FileResultKind::Int } },
                { "filewrite",      { { FileParamKind::FileHandle, FileParamKind::TextValue }, FileResultKind::Int } },
                { "filewritebytes", { { FileParamKind::FileHandle, FileParamKind::Buffer, FileParamKind::SizeValue }, FileResultKind::Int } },
                { "filegetc",       { { FileParamKind::FileHandle }, FileResultKind::Int } },
                { "fileputc",       { { FileParamKind::FileHandle, FileParamKind::IntValue }, FileResultKind::Int } },
                { "filereadline",   { { FileParamKind::FileHandle }, FileResultKind::String } },
                { "filewriteline",  { { FileParamKind::FileHandle, FileParamKind::TextValue }, FileResultKind::Int } },
                { "fileseek",       { { FileParamKind::FileHandle, FileParamKind::IntValue, FileParamKind::SeekOrigin }, FileResultKind::Bool } },
                { "filetell",       { { FileParamKind::FileHandle }, FileResultKind::Int } },
                { "fileeof",        { { FileParamKind::FileHandle }, FileResultKind::Bool } },
                { "fileerror",      { { FileParamKind::FileHandle }, FileResultKind::Int } },
                { "fileremove",     { { FileParamKind::StringValue }, FileResultKind::Bool } },
                { "filerename",     { { FileParamKind::StringValue, FileParamKind::StringValue }, FileResultKind::Bool } },
                { "fileexists",     { { FileParamKind::StringValue }, FileResultKind::Bool } },
                { "filesize",       { { FileParamKind::StringValue }, FileResultKind::Int } },
                { "filecopy",       { { FileParamKind::StringValue, FileParamKind::StringValue }, FileResultKind::Bool } },
                { "filemkdir",      { { FileParamKind::StringValue }, FileResultKind::Bool } },
                { "fileremovedir",  { { FileParamKind::StringValue }, FileResultKind::Bool } },
            };
            return table;
        }

        inline bool is_file_builtin_name(const std::string& name) {
            return file_builtin_table().find(name) != file_builtin_table().end();
        }

        enum class StringParamKind {
            StringValue,
            StringRef,
            IntValue,
            CharValue,
            FileHandle
        };

        enum class StringResultKind {
            Int,
            Bool,
            Char,
            String,
            Void
        };

        struct StringBuiltinInfo {
            std::vector<StringParamKind> params;
            StringResultKind result;
        };

        inline const std::unordered_map<std::string, StringBuiltinInfo>& string_builtin_table() {
            static const std::unordered_map<std::string, StringBuiltinInfo> table = {
                { "strlen",       { { StringParamKind::StringValue }, StringResultKind::Int } },
                { "strconcat",    { { StringParamKind::StringValue, StringParamKind::StringValue }, StringResultKind::String } },
                { "strcopy",      { { StringParamKind::StringValue }, StringResultKind::String } },
                { "strmove",      { { StringParamKind::StringRef }, StringResultKind::String } },
                { "strcompare",   { { StringParamKind::StringValue, StringParamKind::StringValue }, StringResultKind::Int } },
                { "strcontains",  { { StringParamKind::StringValue, StringParamKind::StringValue }, StringResultKind::Bool } },
                { "strsubstr",    { { StringParamKind::StringValue, StringParamKind::IntValue, StringParamKind::IntValue }, StringResultKind::String } },
                { "strfind",      { { StringParamKind::StringValue, StringParamKind::StringValue }, StringResultKind::Int } },
                { "strreplace",   { { StringParamKind::StringValue, StringParamKind::StringValue, StringParamKind::StringValue }, StringResultKind::String } },
                { "strupper",     { { StringParamKind::StringValue }, StringResultKind::String } },
                { "strlower",     { { StringParamKind::StringValue }, StringResultKind::String } },
                { "strtrim",      { { StringParamKind::StringValue }, StringResultKind::String } },
                { "strcharat",    { { StringParamKind::StringValue, StringParamKind::IntValue }, StringResultKind::Char } },
                { "strsetchar",   { { StringParamKind::StringRef, StringParamKind::IntValue, StringParamKind::CharValue }, StringResultKind::Bool } },
                { "strsplitcount",{ { StringParamKind::StringValue, StringParamKind::StringValue }, StringResultKind::Int } },
                { "strsplitget",  { { StringParamKind::StringValue, StringParamKind::StringValue, StringParamKind::IntValue }, StringResultKind::String } },
                { "strread",      { {}, StringResultKind::String } },
                { "strwrite",     { { StringParamKind::StringValue }, StringResultKind::Void } },
                { "strwritefile", { { StringParamKind::FileHandle, StringParamKind::StringValue }, StringResultKind::Int } },
            };
            return table;
        }

        inline bool is_string_builtin_name(const std::string& name) {
            return string_builtin_table().find(name) != string_builtin_table().end();
        }

        inline bool is_valid_file_open_mode(std::string_view mode) {
            static const char* kModes[] = {
                "r", "w", "a", "r+", "w+", "a+",
                "rb", "wb", "ab", "r+b", "w+b", "a+b",
            };
            for (const char* candidate : kModes) {
                if (mode == candidate) return true;
            }
            return false;
        }

        inline bool string_literal_content(std::string_view lexeme, std::string& out) {
            if (lexeme.size() < 2 || lexeme.front() != '"' || lexeme.back() != '"') {
                return false;
            }
            std::string_view inner = lexeme.substr(1, lexeme.size() - 2);
            if (inner.find('\\') != std::string_view::npos) {
                return false;
            }
            out.assign(inner);
            return true;
        }

        inline bool types_equal_modulo_const(const AST::Type& a, const AST::Type& b) {
            if (a.kind != b.kind) return false;
            switch (a.kind) {
            case TypeKind::Array:
                if (a.array_size.has_value() != b.array_size.has_value()) return false;
                if (a.array_size.has_value() && *a.array_size != *b.array_size) return false;
                if ((a.element_type == nullptr) != (b.element_type == nullptr)) return false;
                return a.element_type
                    ? types_equal_modulo_const(*a.element_type, *b.element_type)
                    : true;
            case TypeKind::Pointer:
                if ((a.pointee_type == nullptr) != (b.pointee_type == nullptr)) return false;
                return a.pointee_type
                    ? types_equal_modulo_const(*a.pointee_type, *b.pointee_type)
                    : true;
            case TypeKind::Struct:
                return a.struct_name == b.struct_name;
            case TypeKind::Function:
                if (a.parameter_types.size() != b.parameter_types.size()) return false;
                for (std::size_t i = 0; i < a.parameter_types.size(); ++i) {
                    if (!types_equal_modulo_const(a.parameter_types[i],
                        b.parameter_types[i])) {
                        return false;
                    }
                }
                if ((a.return_type == nullptr) != (b.return_type == nullptr)) return false;
                return a.return_type
                    ? types_equal_modulo_const(*a.return_type, *b.return_type)
                    : true;
            default:
                return true;
            }
        }

        inline bool const_qualification_ok(const AST::Type& from, const AST::Type& to) {
            if (from.kind != to.kind) return false;
            if (from.is_const && !to.is_const) return false;
            switch (from.kind) {
            case TypeKind::Array:
                if (from.element_type && to.element_type) {
                    return const_qualification_ok(*from.element_type, *to.element_type);
                }
                return true;
            case TypeKind::Pointer:
                if (from.pointee_type && to.pointee_type) {
                    return const_qualification_ok(*from.pointee_type, *to.pointee_type);
                }
                return true;
            case TypeKind::Function:
                if ((from.return_type == nullptr) != (to.return_type == nullptr)) {
                    return false;
                }
                if (from.return_type && !const_qualification_ok(*from.return_type,
                    *to.return_type)) {
                    return false;
                }
                if (from.parameter_types.size() != to.parameter_types.size()) {
                    return false;
                }
                for (std::size_t i = 0; i < from.parameter_types.size(); ++i) {
                    if (!const_qualification_ok(from.parameter_types[i],
                        to.parameter_types[i])) {
                        return false;
                    }
                }
                return true;
            default:
                return true;
            }
        }

        inline void collect_struct_names_from_type(const AST::Type& type,
            std::unordered_set<std::string>& out) {
            switch (type.kind) {
            case TypeKind::Struct:
                if (!type.struct_name.empty()) out.insert(type.struct_name);
                break;
            case TypeKind::Pointer:
                if (type.pointee_type) {
                    collect_struct_names_from_type(*type.pointee_type, out);
                }
                break;
            case TypeKind::Array:
                if (type.element_type) {
                    collect_struct_names_from_type(*type.element_type, out);
                }
                break;
            default:
                break;
            }
        }

        inline std::string sanitize_identifier(std::string_view text) {
            std::string out;
            out.reserve(text.size());
            for (char c : text) {
                switch (c) {
                case '<': out += 'L'; break;
                case '>': out += 'G'; break;
                case '*': out += 'P'; break;
                case '[': out += 'A'; break;
                case ']': out += 'Z'; break;
                case ',': out += 'C'; break;
                case '.': out += 'D'; break;
                case ' ': break;
                default: out += c; break;
                }
            }
            return out;
        }

    }
}

#endif
