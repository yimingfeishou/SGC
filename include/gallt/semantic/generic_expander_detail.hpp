#ifndef GALLT_SEMANTIC_GENERIC_EXPANDER_DETAIL_HPP
#define GALLT_SEMANTIC_GENERIC_EXPANDER_DETAIL_HPP

#include "generic_expander.hpp"
#include "constant_folding.hpp"
#include "../lexer/lexer.hpp"
#include "../parser/parser.hpp"
#include <algorithm>
#include <cstdlib>
#include <cstdio>
#include <functional>
#include <map>
#include <set>

namespace gallt {
    using namespace AST;

    namespace generic_expander_detail {
        inline bool is_builtin_type_name(const std::string& name) {
            return name == "int" || name == "lint" || name == "uint" ||
                name == "luint" || name == "float" || name == "double" ||
                name == "char" || name == "uchar" || name == "bool" ||
                name == "string" || name == "file" || name == "void";
        }

        inline std::string arguments_text(const GenericRef& ref) {
            std::string out;

            for (std::size_t i = 0; i < ref.arguments.size(); ++i) {
                if (i != 0) { out += ", "; }
                out += ref.arguments[i].normalize();
            }

            return out;
        }

        inline std::string arguments_kind_signature(const GenericRef& ref) {
            std::string out;

            for (std::size_t i = 0; i < ref.arguments.size(); ++i) {
                const GenericArgument& arg = ref.arguments[i];
                out += "|";

                if (arg.is_type) {
                    out += "T:" + arg.type.to_string();
                } else if (arg.is_string_constant) {
                    out += "CS";
                } else if (arg.float_constant) {
                    out += "CF";
                } else {
                    out += "CI";
                }
            }

            return out;
        }

        inline bool is_free_identifier(const std::string& name,
            const std::unordered_map<std::string, StructDefinition*>& structs) {
            if (is_builtin_type_name(name)) { return false; }
            return structs.find(name) == structs.end();
        }

        inline bool text_contains_compile_time_condition(const std::string& text) {
            std::string name = "<emit>";
            std::string_view source_view(text);
            std::string_view name_view(name);
            DiagnosticEngine scratch;
            Lexer lexer(source_view, name_view, scratch);
            bool previous_is_const = false;

            for (;;) {
                Token token = lexer.next_token();
                if (token.type == TokenType::EndOfFile) { return false; }
                if (previous_is_const && token.type == TokenType::Keyword_If) {
                    return true;
                }
                previous_is_const = token.type == TokenType::Keyword_Const;
            }
        }

    }
}

#endif
