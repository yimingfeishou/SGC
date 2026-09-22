#ifndef GALLT_CODEGEN_CODEGEN_DETAIL_HPP
#define GALLT_CODEGEN_CODEGEN_DETAIL_HPP

#include "codegen.hpp"
#include "../semantic/constant_folding.hpp"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <sstream>
#include <stdexcept>
#include <system_error>

namespace gallt {
    using namespace AST;

    namespace codegen_detail {

    inline std::string decode_escaped_bytes(std::string_view raw, bool is_char);

    inline bool is_file_builtin_name(const std::string& name) {
        static const std::unordered_set<std::string> names = {
            "fileopen", "fileclose", "fileflush", "fileread", "filewrite",
            "filewritebytes", "filegetc", "fileputc", "filereadline",
            "filewriteline", "fileseek", "filetell", "fileeof", "fileerror",
            "fileremove", "filerename", "fileexists", "filesize", "filecopy",
            "filemkdir", "fileremovedir",
        };
        return names.find(name) != names.end();
    }

    inline bool is_string_builtin_name(const std::string& name) {
        static const std::unordered_set<std::string> names = {
            "strlen", "strconcat", "strcopy", "strmove", "strcompare",
            "strcontains", "strsubstr", "strfind", "strreplace", "strupper",
            "strlower", "strtrim", "strcharat", "strsetchar", "strsplitcount",
            "strsplitget", "strread", "strwrite", "strwritefile",
        };
        return names.find(name) != names.end();
    }

    inline bool builtin_type_by_name(const std::string& name, AST::Type& out) {
        if (name == "int") out = AST::Type::make_int();
        else if (name == "lint") out = AST::Type::make_lint();
        else if (name == "uint") out = AST::Type::make_uint();
        else if (name == "luint") out = AST::Type::make_luint();
        else if (name == "float") out = AST::Type::make_float();
        else if (name == "double") out = AST::Type::make_double();
        else if (name == "char") out = AST::Type::make_char();
        else if (name == "uchar") out = AST::Type::make_uchar();
        else if (name == "bool") out = AST::Type::make_bool();
        else if (name == "string") out = AST::Type::make_string();
        else if (name == "file") out = AST::Type::make_file();
        else if (name == "void") out = AST::Type::make_void();
        else return false;
        return true;
    }

    inline int hex_value(char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    }

    inline int octal_value(char c) {
        return (c >= '0' && c <= '7') ? c - '0' : -1;
    }

    inline std::string llvm_escape_bytes(const std::string& bytes) {
        std::string out = "c\"";
        for (unsigned char b : bytes) {
            char nibbles[] = "0123456789abcdef";
            switch (b) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\22"; break;
            case '\n': out += "\\0A"; break;
            case '\r': out += "\\0D"; break;
            case '\t': out += "\\09"; break;
            default:
                if (b >= 0x20 && b <= 0x7E) {
                    out.push_back(static_cast<char>(b));
                } else {
                    out += "\\";
                    out.push_back(nibbles[(b >> 4) & 0xF]);
                    out.push_back(nibbles[b & 0xF]);
                }
            }
        }
        out += '"';
        return out;
    }

    inline std::string llvm_float_constant_text(std::string text) {
        if (text.empty()) {
            return text;
        }
        if (text == "inf" || text == "+inf") {
            return "0x7FF0000000000000";
        }
        if (text == "-inf") {
            return "0xFFF0000000000000";
        }
        if (text == "nan" || text == "+nan" || text == "-nan") {
            return "0x7FF8000000000000";
        }
        if (text.find('.') == std::string::npos) {
            const std::size_t exponent = text.find_first_of("eE");
            if (exponent == std::string::npos) {
                text += ".0";
            }
            else {
                text.insert(exponent, ".0");
            }
        }
        return text;
    }

    inline bool line_accepts_debug_metadata(const std::string& line) {
        if (line.empty()) return false;
        if (line.front() == '@' || line.front() == '!') return false;
        if (line.back() == ':') return false;
        if (line == "}") return false;
        if (line.rfind("define ", 0) == 0) return false;
        if (line.rfind("declare ", 0) == 0) return false;
        if (line.rfind("source_filename", 0) == 0) return false;
        if (line.rfind("target ", 0) == 0) return false;
        if (line.rfind("attributes ", 0) == 0) return false;
        if (line.rfind("phi ", 0) == 0) return false;
        if (line.find(" = phi ") != std::string::npos) return false;
        if (line.find(" = ") != std::string::npos) {
            if (line.find(" = global ") != std::string::npos) return false;
            if (line.find(" = private ") != std::string::npos) return false;
            if (line.find(" = constant ") != std::string::npos) return false;
            if (line.find(" = internal ") != std::string::npos) return false;
            if (line.find(" = type ") != std::string::npos) return false;
            return true;
        }
        static const char* kInstructionPrefixes[] = {
            "ret ", "br ", "switch ", "unreachable", "store ", "call ",
            "invoke ", "resume ", "fence ",
        };
        for (const char* prefix : kInstructionPrefixes) {
            if (line.rfind(prefix, 0) == 0) return true;
        }
        return false;
    }

    inline std::string escape_metadata_string(const std::string& text) {
        std::string out;
        out.reserve(text.size());
        for (char c : text) {
            if (c == '\\' || c == '"') {
                out.push_back('\\');
            }
            out.push_back(c);
        }
        return out;
    }

    inline std::string strip_literal_quotes(std::string_view lexeme) {
        if (lexeme.size() >= 2 &&
            ((lexeme.front() == '"' && lexeme.back() == '"') ||
             (lexeme.front() == '\'' && lexeme.back() == '\''))) {
            lexeme.remove_prefix(1);
            lexeme.remove_suffix(1);
        }
        return std::string(lexeme);
    }

    inline std::string decode_escaped_bytes(std::string_view raw, bool is_char) {
        std::string content = strip_literal_quotes(raw);
        std::string out;
        for (size_t i = 0; i < content.size(); ++i) {
            char c = content[i];
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            if (++i >= content.size()) break;
            char e = content[i];
            switch (e) {
            case 'n': out.push_back('\n'); break;
            case 't': out.push_back('\t'); break;
            case 'r': out.push_back('\r'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'v': out.push_back('\v'); break;
            case '\\': out.push_back('\\'); break;
            case '"': out.push_back('"'); break;
            case '\'': out.push_back('\''); break;
            case 'x': {
                int value = 0;
                int digits = 0;
                while (i + 1 < content.size() && digits < 2) {
                    int d = hex_value(content[i + 1]);
                    if (d < 0) break;
                    ++i;
                    value = value * 16 + d;
                    ++digits;
                }
                out.push_back(static_cast<char>(value));
                break;
            }
            default:
                if (octal_value(e) >= 0) {
                    int value = octal_value(e);
                    int digits = 1;
                    while (i + 1 < content.size() && digits < 3) {
                        int d = octal_value(content[i + 1]);
                        if (d < 0) break;
                        ++i;
                        value = value * 8 + d;
                        ++digits;
                    }
                    out.push_back(static_cast<char>(value));
                } else {
                    out.push_back(e);
                }
                break;
            }
        }
        if (is_char && !out.empty()) {
            return std::string(1, out.front());
        }
        return out;
    }

    }
}

#endif
