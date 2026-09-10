// lexer/lexer.hpp
// 词法分析器 —— 将源文件转换为 Token 流
// Lexer — converts source file into a stream of tokens

#ifndef GALLT_LEXER_LEXER_HPP
#define GALLT_LEXER_LEXER_HPP

#include "../common/token.hpp"
#include "../common/diagnostics.hpp"
#include <string_view>
#include <vector>
#include <optional>

namespace gallt {

    // ============================================================================
    // 词法分析器 (Lexer)
    // 负责将 UTF-8 源码字符串拆分为词法单元 (Token)
    // Responsible for splitting UTF-8 source string into tokens
    // ============================================================================

    class Lexer {
    public:
        // ---- 构造与析构 ----
        // 构造：传入源码内容和文件名
        // Constructor: pass source content and filename
        Lexer(std::string_view source, std::string_view filename, DiagnosticEngine& diag);
        ~Lexer() = default;

        // 禁止拷贝（词法分析器拥有内部状态，不应拷贝）
        Lexer(const Lexer&) = delete;
        Lexer& operator=(const Lexer&) = delete;

        // ---- 主接口 ----
        // 获取下一个 Token（消费）
        // Get the next token (consume)
        Token next_token();

        // 预看下一个 Token（不消费）
        // Peek at the next token (without consuming)
        Token peek_token();

        // 当前位置（行、列）
        // Current location (line, column)
        SourceLocation current_location() const;

        // 当前剩余源码（从当前位置到末尾）
        // Remaining source from current position
        std::string_view remaining_source() const;

        // 是否已到达文件末尾
        // Whether end of file has been reached
        bool at_end() const noexcept;

        // ---- 验证（构造函数中调用） ----
        // 验证文件编码和换行符，不符合则报告 ER 0034 错误
        // Validate encoding and line endings; report ER 0034 if invalid
        void validate_file();

    private:
        // ---- 成员变量 ----
        std::string_view source_;           // 源码内容（不拥有所有权）
        std::string_view filename_;         // 文件名（不拥有所有权）
        DiagnosticEngine& diag_;            // 诊断引擎引用

        std::size_t position_ = 0;          // 当前字符索引（从0开始）
        std::size_t line_ = 1;              // 当前行号（从1开始）
        std::size_t column_ = 1;            // 当前列号（从1开始）

        bool has_error_ = false;            // 是否遇到致命错误（阻止继续）

        // ---- 内部状态缓存 ----
        std::optional<Token> peeked_token_; // 预读的 Token 缓存

        // ---- 核心扫描函数 ----
        // 跳过空白字符（空格、制表符、换行）
        // Skip whitespace (space, tab, newline)
        void skip_whitespace();

        // 读取并返回下一个 Token（不含空白）
        // Read and return the next token (excluding whitespace)
        Token scan_token();

        // ---- 具体 Token 识别 ----
        Token read_identifier();            // 标识符或关键字
        Token read_number();                // 数字字面量（整数/浮点）
        Token read_char_literal();          // 字符常量
        Token read_string_literal();        // 字符串常量
        Token read_comment();               // 注释（返回空 Token，被跳过）
        Token read_operator_or_delimiter(); // 运算符或分隔符

        // ---- 辅助函数 ----
        // 取当前字符，并前进
        char advance();
        // 取当前字符，不前进
        char peek() const;
        // 取下一个字符，不前进
        char peek_next() const;
        // 判断当前位置是否在范围内
        bool is_at_end() const;
        // 匹配当前字符，若匹配则前进并返回 true
        bool match(char expected);
        // 跳过当前字符（不返回）
        void skip();

        // ---- 错误报告辅助 ----
        void report_error(ErrorCode code, std::string_view message);
        void report_error_at(SourceLocation loc, ErrorCode code, std::string_view message);

        // ---- 字面量解析辅助 ----
        // 解析转义序列（支持 \n, \t, \r, \b, \f, \v, \0, \\, \", \', \xhh, \ddd）
        // Parse escape sequences
        char parse_escape_sequence(std::size_t& pos, SourceLocation loc, bool is_char);

        // 解析数字（包含可选的小数部分和指数）
        // Parse number (includes optional fractional part and exponent)
        std::string_view parse_number();

        // 十六进制/八进制数字解析
        // Parse hex/octal digits
        std::optional<int> parse_hex_digit(char c) const;
        std::optional<int> parse_octal_digit(char c) const;

        // 验证换行符 (LF only)
        bool validate_line_endings();
        // 验证 UTF-8 有效性（简单检查）
        bool validate_utf8();
    };

} // namespace gallt

#endif // GALLT_LEXER_LEXER_HPP