// common/token.hpp
// 词法单元定义 —— Token 类型、结构体及辅助函数
// Token definitions — Token types, structure, and helper functions

#ifndef GALLT_COMMON_TOKEN_HPP
#define GALLT_COMMON_TOKEN_HPP

#include "source_location.hpp"
#include <string_view>
#include <cstdint>

namespace gallt {

    // ============================================================================
    // 词法单元类型枚举
    // Token type enumeration
    // ============================================================================

    enum class TokenType : std::uint16_t {
        // ---- 关键字 (Keywords) ----
        // 参照 Gallt 0.2.txt §2 (类型) 和 §8, §9, §11, §12, §14, §15, §16, §17
        // Reference: Gallt 0.2.txt §2 (Types) and §8, §9, §11, §12, §14, §15, §16, §17
        Keyword_Int,        // int
        Keyword_Float,      // float
        Keyword_Double,     // double
        Keyword_Char,       // char
        Keyword_Bool,       // bool
        Keyword_String,     // string
        Keyword_File,       // file (Gallt 0.2.txt §2：文件类型)
        Keyword_Void,       // void
        Keyword_Struct,     // struct
        Keyword_If,         // if
        Keyword_Else,       // else
        Keyword_For,        // for
        Keyword_While,      // while
        Keyword_Break,      // break
        Keyword_Return,     // return
        Keyword_Guide,      // guide
        Keyword_Clib,       // clib
        Keyword_Extern,     // extern
        Keyword_From,       // from
        Keyword_Null,       // null
        Keyword_Heap,       // heap
        Keyword_Free,       // free
        Keyword_Input,      // input
        Keyword_Output,     // output
        Keyword_Size,       // size
        Keyword_Align,      // align

        // ---- 标识符 (Identifier) ----
        Identifier,

        // ---- 字面量 (Literals) ----
        // 参照 Gallt 0.2.txt §4 (表达式) 和 §9 (转义符)
        // Reference: Gallt 0.2.txt §4 (Expressions) and §9 (Escape sequences)
        IntegerLiteral,     // 整数常量，如 42, 0xff, 077
        FloatLiteral,       // 浮点常量，如 3.14, 1.0f
        CharLiteral,        // 字符常量，如 'A', '\n'
        StringLiteral,      // 字符串常量，如 "Hello"
        BoolLiteral,        // 布尔字面量，true 或 false

        // ---- 运算符 (Operators) ----
        // 参照 Gallt 0.2.txt §3 (运算符)
        // Reference: Gallt 0.2.txt §3 (Operators)
        Assign,             // =
        PlusAssign,         // +=
        MinusAssign,        // -=
        Equal,              // ==
        NotEqual,           // !=
        Greater,            // >
        Less,               // <
        GreaterEqual,       // >=
        LessEqual,          // <=
        Plus,               // +
        Minus,              // -
        Star,               // * (乘号 / 解引用)
        Slash,              // /
        Percent,            // % (取余，优先级与 *、/ 相同)
        Power,              // **
        Increment,          // ++
        Decrement,          // --
        AddressOf,          // & (取地址)
        LogicalAnd,         // &&
        LogicalOr,          // ||
        LogicalNot,         // !
        Dot,                // .
        Arrow,              // ->

        // ---- 分隔符 (Delimiters) ----
        // 参照 Gallt 0.2 EBNF.txt 中的语法规则
        // Reference: Gallt 0.2 EBNF.txt for syntax rules
        LeftParen,          // (
        RightParen,         // )
        LeftBrace,          // {
        RightBrace,         // }
        LeftBracket,        // [
        RightBracket,       // ]
        Comma,              // ,
        Semicolon,          // ;
        Newline,            // 换行符

        // ---- 特殊 (Special) ----
        EndOfFile,          // 文件结束
        Unknown,            // 无法识别的字符
    };

    // ============================================================================
    // 词法单元结构体
    // Token structure
    // ============================================================================

    struct Token {
        TokenType type;                     // 词法单元类型
        SourceLocation location;            // 源码位置
        std::string_view lexeme;            // 实际文本（不拥有所有权，指向源文件缓冲区）

        // 默认构造函数
        Token() = default;

        // 便捷构造函数
        Token(TokenType t, SourceLocation loc, std::string_view lex)
            : type(t), location(loc), lexeme(lex) {
        }

        // 判断是否为特定类型
        bool is(TokenType t) const noexcept { return type == t; }

        // 判断是否属于关键字类别
        bool is_keyword() const noexcept {
            return type >= TokenType::Keyword_Int && type <= TokenType::Keyword_Align;
        }

        // 判断是否为字面量
        bool is_literal() const noexcept {
            return type >= TokenType::IntegerLiteral && type <= TokenType::BoolLiteral;
        }

        // 判断是否为运算符
        bool is_operator() const noexcept {
            return type >= TokenType::Assign && type <= TokenType::Arrow;
        }

        // 判断是否为分隔符
        bool is_delimiter() const noexcept {
            return type >= TokenType::LeftParen && type <= TokenType::Semicolon;
        }
    };

    // ============================================================================
    // Token 辅助函数
    // Helper functions for tokens
    // ============================================================================

    // 将 TokenType 转换为可读字符串（用于错误消息）
    // Convert TokenType to a human-readable string (for error messages)
    const char* token_type_to_string(TokenType type) noexcept;

    // 检查给定 lexeme 是否为布尔字面量（true/false）
    // Check if a given lexeme is a boolean literal (true/false)
    bool is_bool_literal(std::string_view lexeme) noexcept;

    // 检查给定 lexeme 是否为关键字（不包括 true/false 和 null，它们单独处理）
    // Check if a given lexeme is a keyword (excluding true/false and null, which are handled separately)
    bool is_keyword_string(std::string_view lexeme) noexcept;

    // 将关键字字符串转换为对应的 TokenType，如果不是关键字则返回 TokenType::Identifier
    // Convert a keyword string to the corresponding TokenType, if not a keyword returns TokenType::Identifier
    TokenType keyword_to_token_type(std::string_view lexeme) noexcept;

} // namespace gallt

#endif // GALLT_COMMON_TOKEN_HPP
