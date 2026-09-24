#include "token.hpp"
#include <string_view>
#include <unordered_map>

namespace gallt {

    namespace {
        const std::unordered_map<std::string_view, TokenType>& keyword_table() {
            static const std::unordered_map<std::string_view, TokenType> table = {
                {"int", TokenType::Keyword_Int},
                {"lint", TokenType::Keyword_Lint},
                {"uint", TokenType::Keyword_Uint},
                {"luint", TokenType::Keyword_Luint},
                {"float", TokenType::Keyword_Float},
                {"double", TokenType::Keyword_Double},
                {"char", TokenType::Keyword_Char},
                {"uchar", TokenType::Keyword_Uchar},
                {"bool", TokenType::Keyword_Bool},
                {"string", TokenType::Keyword_String},
                {"file", TokenType::Keyword_File},
                {"void", TokenType::Keyword_Void},
                {"struct", TokenType::Keyword_Struct},
                {"if", TokenType::Keyword_If},
                {"else", TokenType::Keyword_Else},
                {"for", TokenType::Keyword_For},
                {"while", TokenType::Keyword_While},
                {"break", TokenType::Keyword_Break},
                {"return", TokenType::Keyword_Return},
                {"guide", TokenType::Keyword_Guide},
                {"clib", TokenType::Keyword_Clib},
                {"extern", TokenType::Keyword_Extern},
                {"from", TokenType::Keyword_From},
                {"null", TokenType::Keyword_Null},
                {"heap", TokenType::Keyword_Heap},
                {"generics", TokenType::Keyword_Generics},
                {"namespace", TokenType::Keyword_Namespace},
                {"access", TokenType::Keyword_Access},
                {"addition", TokenType::Keyword_Addition},
                {"emit", TokenType::Keyword_Emit},
                {"export", TokenType::Keyword_Export},
                {"const", TokenType::Keyword_Const},
            };
            return table;
        }
    }

    const char* token_type_to_string(TokenType type) noexcept {
        switch (type) {
        case TokenType::Keyword_Int: return "int";
        case TokenType::Keyword_Lint: return "lint";
        case TokenType::Keyword_Uint: return "uint";
        case TokenType::Keyword_Luint: return "luint";
        case TokenType::Keyword_Float: return "float";
        case TokenType::Keyword_Double: return "double";
        case TokenType::Keyword_Char: return "char";
        case TokenType::Keyword_Uchar: return "uchar";
        case TokenType::Keyword_Bool: return "bool";
        case TokenType::Keyword_String: return "string";
        case TokenType::Keyword_File: return "file";
        case TokenType::Keyword_Void: return "void";
        case TokenType::Keyword_Struct: return "struct";
        case TokenType::Keyword_If: return "if";
        case TokenType::Keyword_Else: return "else";
        case TokenType::Keyword_For: return "for";
        case TokenType::Keyword_While: return "while";
        case TokenType::Keyword_Break: return "break";
        case TokenType::Keyword_Return: return "return";
        case TokenType::Keyword_Guide: return "guide";
        case TokenType::Keyword_Clib: return "clib";
        case TokenType::Keyword_Extern: return "extern";
        case TokenType::Keyword_From: return "from";
        case TokenType::Keyword_Null: return "null";
        case TokenType::Keyword_Heap: return "heap";
        case TokenType::Keyword_Generics: return "generics";
        case TokenType::Keyword_Namespace: return "namespace";
        case TokenType::Keyword_Access: return "access";
        case TokenType::Keyword_Addition: return "addition";
        case TokenType::Keyword_Emit: return "emit";
        case TokenType::Keyword_Export: return "export";
        case TokenType::Keyword_Const: return "const";
        case TokenType::Identifier: return "identifier";
        case TokenType::IntegerLiteral: return "integer literal";
        case TokenType::FloatLiteral: return "float literal";
        case TokenType::CharLiteral: return "char literal";
        case TokenType::StringLiteral: return "string literal";
        case TokenType::BoolLiteral: return "boolean literal";
        case TokenType::Assign: return "=";
        case TokenType::PlusAssign: return "+=";
        case TokenType::MinusAssign: return "-=";
        case TokenType::AndAssign: return "&=";
        case TokenType::OrAssign: return "|=";
        case TokenType::XorAssign: return "^=";
        case TokenType::Equal: return "==";
        case TokenType::NotEqual: return "!=";
        case TokenType::Greater: return ">";
        case TokenType::Less: return "<";
        case TokenType::GreaterEqual: return ">=";
        case TokenType::LessEqual: return "<=";
        case TokenType::Plus: return "+";
        case TokenType::Minus: return "-";
        case TokenType::Star: return "*";
        case TokenType::Slash: return "/";
        case TokenType::Percent: return "%";
        case TokenType::Pipe: return "|";
        case TokenType::Caret: return "^";
        case TokenType::Tilde: return "~";
        case TokenType::Power: return "**";
        case TokenType::Increment: return "++";
        case TokenType::Decrement: return "--";
        case TokenType::AddressOf: return "&";
        case TokenType::LogicalAnd: return "&&";
        case TokenType::LogicalOr: return "||";
        case TokenType::LogicalNot: return "!";
        case TokenType::Dot: return ".";
        case TokenType::Arrow: return "->";
        case TokenType::Question: return "?";
        case TokenType::ColonColon: return "::";
        case TokenType::LeftParen: return "(";
        case TokenType::RightParen: return ")";
        case TokenType::LeftBrace: return "{";
        case TokenType::RightBrace: return "}";
        case TokenType::LeftBracket: return "[";
        case TokenType::RightBracket: return "]";
        case TokenType::Comma: return ",";
        case TokenType::Semicolon: return ";";
        case TokenType::Colon: return ":";
        case TokenType::Newline: return "newline";
        case TokenType::EndOfFile: return "end of file";
        case TokenType::At: return "@";
        case TokenType::Ellipsis: return "...";
        case TokenType::Unknown: return "unknown token";
        }

        return "token";
    }

    bool is_bool_literal(std::string_view lexeme) noexcept {
        return lexeme == "true" || lexeme == "false";
    }

    bool is_keyword_string(std::string_view lexeme) noexcept {
        return keyword_table().count(lexeme) != 0;
    }

    TokenType keyword_to_token_type(std::string_view lexeme) noexcept {
        auto it = keyword_table().find(lexeme);
        return it == keyword_table().end() ? TokenType::Identifier : it->second;
    }

}
