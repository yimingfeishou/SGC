// lexer/lexer.cpp
// 词法分析器实现 —— 将 Gallt 源码转换为 Token 流
// Lexer implementation — converts Gallt source code into a token stream

#include "../lexer/lexer.hpp"
#include <cctype>
#include <unordered_map>
#include <optional>
#include <charconv>
#include <system_error>

namespace gallt {

    // ============================================================================
    // 静态辅助数据 (Static helper data)
    // ============================================================================

    namespace {
        // 关键字到 TokenType 的映射表
        // Keyword to TokenType mapping table
        const std::unordered_map<std::string_view, TokenType> KEYWORD_MAP = {
            // 类型 (Types)
            {"int", TokenType::Keyword_Int},
            // 0.4.1 新增整型/无符号类型说明符 (Gallt 0.4.1.txt §2)
            // Added in 0.4.1: long/unsigned type specifiers (Gallt 0.4.1.txt §2)
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
            // 结构体 (Struct)
            {"struct", TokenType::Keyword_Struct},
            // 控制流 (Control flow)
            {"if", TokenType::Keyword_If},
            {"else", TokenType::Keyword_Else},
            {"for", TokenType::Keyword_For},
            {"while", TokenType::Keyword_While},
            {"break", TokenType::Keyword_Break},
            {"return", TokenType::Keyword_Return},
            // 模块与外部 (Modules & extern)
            {"guide", TokenType::Keyword_Guide},
            {"clib", TokenType::Keyword_Clib},
            {"extern", TokenType::Keyword_Extern},
            {"from", TokenType::Keyword_From},
            // 指针与内存 (Pointers & memory)
            {"null", TokenType::Keyword_Null},
            {"heap", TokenType::Keyword_Heap},
            // 0.3 新增：编译期泛型块 (Gallt 0.3.txt §19)
            // Added in 0.3: compile-time generics blocks (Gallt 0.3.txt §19)
            {"generics", TokenType::Keyword_Generics},
            // 0.4 新增：命名空间与编译期代码生成 (Gallt 0.4.txt §19/§21)
            // Added in 0.4: namespaces and compile-time code generation
            {"namespace", TokenType::Keyword_Namespace},
            {"access", TokenType::Keyword_Access},
            {"addition", TokenType::Keyword_Addition},
            {"emit", TokenType::Keyword_Emit},
            // 0.4.1 新增：类型限定符 (Gallt 0.4.1.txt §2)
            // Added in 0.4.1: the const type qualifier (Gallt 0.4.1.txt §2)
            {"const", TokenType::Keyword_Const},
        };

        // 检查字符是否为标识符的合法起始字符 (字母或下划线)
        // Check if character is a valid identifier start (letter or underscore)
        bool is_identifier_start(char c) noexcept {
            return std::isalpha(static_cast<unsigned char>(c)) || c == '_';
        }

        // 检查字符是否为标识符的合法续接字符 (字母、数字或下划线)
        // Check if character is a valid identifier continuation (letter, digit, or underscore)
        bool is_identifier_continuation(char c) noexcept {
            // '$' 只作为续接字符出现：命名空间/泛型实例的命名修饰会生成
            // `ns$member`、`Box$int$Member` 这类内部名，Emit 生成的源码可能
            // 需要引用它们（Gallt 0.4.txt §19）。
            // '$' is accepted only as a continuation character: name mangling for
            // namespaces/generic instances produces internal names such as
            // `ns$member` or `Box$int$Member`, and emitted source may reference them.
            return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '$';
        }

        // 检查字符是否为空白 (空格、制表符、换行符)
        // Check if character is whitespace (space, tab, newline)
        bool is_whitespace(char c) noexcept {
            return c == ' ' || c == '\t' || c == '\n' || c == '\r';
        }

        // 检查字符是否为数字
        // Check if character is a digit
        bool is_digit(char c) noexcept {
            return std::isdigit(static_cast<unsigned char>(c));
        }

        // 检查字符是否为十六进制数字
        // Check if character is a hex digit
        bool is_hex_digit(char c) noexcept {
            return std::isxdigit(static_cast<unsigned char>(c));
        }

        // 检查字符是否为八进制数字
        // Check if character is an octal digit
        bool is_octal_digit(char c) noexcept {
            return c >= '0' && c <= '7';
        }

        // 0.4.1 §7：数值字面量后缀合法性
        // 整数字面量允许 l/L、u/U、lu/Lu/lU/LU；浮点字面量仅允许 f/F。
        // 0.4.1 §7: valid numeric literal suffixes. Integer literals accept
        // l/L, u/U and lu/Lu/lU/LU; floating literals accept only f/F.
        bool is_valid_integer_suffix(std::string_view suffix) noexcept {
            auto lower = [](char c) {
                return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            };
            if (suffix.size() == 1) {
                char c = lower(suffix[0]);
                return c == 'l' || c == 'u';
            }
            if (suffix.size() == 2) {
                char a = lower(suffix[0]);
                char b = lower(suffix[1]);
                return a == 'l' && b == 'u';
            }
            return false;
        }

        bool is_valid_float_suffix(std::string_view suffix) noexcept {
            return suffix.size() == 1 &&
                (suffix[0] == 'f' || suffix[0] == 'F');
        }
    } // anonymous namespace

    // ============================================================================
    // 构造函数与析构函数
    // Constructor & Destructor
    // ============================================================================

    Lexer::Lexer(std::string_view source, std::string_view filename, DiagnosticEngine& diag)
        : source_(source)
        , filename_(filename)
        , diag_(diag) {
        // 容忍 UTF-8 BOM：部分 Windows 编辑器会在文件开头写入 EF BB BF
        // Tolerate a UTF-8 BOM, which some Windows editors prepend
        if (source_.size() >= 3 &&
            static_cast<unsigned char>(source_[0]) == 0xEF &&
            static_cast<unsigned char>(source_[1]) == 0xBB &&
            static_cast<unsigned char>(source_[2]) == 0xBF) {
            position_ = 3;
        }
        // 验证文件编码和换行符 (Validate file encoding and line endings)
        // 参照 Gallt 0.2.txt § 编译文件要求: UTF-8 格式，换行符必须为 LF
        // Reference: Gallt 0.2.txt § Compilation File Requirements: UTF-8, LF line endings
        validate_file();
    }

    // ============================================================================
    // 主接口 (Public Interface)
    // ============================================================================

    Token Lexer::next_token() {
        // 如果存在预读缓存，则返回缓存并清空
        // If peek cache exists, return it and clear
        if (peeked_token_.has_value()) {
            Token result = peeked_token_.value();
            peeked_token_.reset();
            return result;
        }

        // 遇到致命错误时直接返回 EOF
        // Return EOF directly if a fatal error occurred
        if (has_error_) {
            return Token{ TokenType::EndOfFile, current_location(), "" };
        }

        return scan_token();
    }

    Token Lexer::peek_token() {
        if (!peeked_token_.has_value()) {
            if (has_error_) {
                peeked_token_ = Token{ TokenType::EndOfFile, current_location(), "" };
            }
            else {
                peeked_token_ = scan_token();
            }
        }
        return peeked_token_.value();
    }

    SourceLocation Lexer::current_location() const {
        return SourceLocation{ filename_, line_, column_ };
    }

    std::string_view Lexer::remaining_source() const {
        if (position_ >= source_.size()) {
            return "";
        }
        return source_.substr(position_);
    }

    bool Lexer::at_end() const noexcept {
        return position_ >= source_.size();
    }

    // ============================================================================
    // 文件验证 (File Validation)
    // ============================================================================

    void Lexer::validate_file() {
        // 验证换行符 (Validate line endings)
        // 参照 Gallt 0.2.txt § 编译文件要求: 换行符必须为 LF
        // Reference: Gallt 0.2.txt § Compilation File Requirements: LF line endings
        bool line_ending_ok = validate_line_endings();

        // 验证 UTF-8 编码 (Validate UTF-8 encoding)
        bool encoding_ok = validate_utf8();

        if (!line_ending_ok || !encoding_ok) {
            // ER 0034: 输入文件不符合规定，编码为 '[coding]'，换行符为 '[linebreak]'
            // ER 0034: Input file does not meet requirements, encoding is '[coding]', line ending is '[linebreak]'
            std::string msg = "input file does not meet requirements: ";
            if (!encoding_ok) msg += "invalid UTF-8 encoding; ";
            if (!line_ending_ok) msg += "line endings must be LF (found CRLF or standalone CR); ";
            report_error(ErrorCode::InvalidInputFile, msg);
            has_error_ = true;
        }
    }

    bool Lexer::validate_line_endings() {
        std::size_t pos = 0;
        while (pos < source_.size()) {
            char c = source_[pos];
            if (c == '\r') {
                // 如果 CR 后面紧跟 LF，则视为 CRLF（违规）
                // If CR is followed by LF, treat as CRLF (violation)
                if (pos + 1 < source_.size() && source_[pos + 1] == '\n') {
                    return false; // CRLF 不符合要求
                }
                return false; // 单独的 CR 也不符合要求
            }
            pos++;
        }
        return true;
    }

    bool Lexer::validate_utf8() {
        std::size_t pos = 0;
        while (pos < source_.size()) {
            unsigned char c = static_cast<unsigned char>(source_[pos]);

            // ASCII 字符 (0x00-0x7F)
            if (c <= 0x7F) {
                pos++;
                continue;
            }

            // 多字节序列 (Multi-byte sequence)
            int expected_bytes = 0;
            if ((c & 0xE0) == 0xC0) {      // 2字节序列: 110xxxxx
                expected_bytes = 2;
            }
            else if ((c & 0xF0) == 0xE0) { // 3字节序列: 1110xxxx
                expected_bytes = 3;
            }
            else if ((c & 0xF8) == 0xF0) { // 4字节序列: 11110xxx
                expected_bytes = 4;
            }
            else {
                return false; // 无效的前导字节 (Invalid leading byte)
            }

            // 检查后续字节是否有效 (Check continuation bytes)
            for (int i = 1; i < expected_bytes; i++) {
                if (pos + i >= source_.size()) {
                    return false; // 提前结束 (Premature end)
                }
                unsigned char cont = static_cast<unsigned char>(source_[pos + i]);
                if ((cont & 0xC0) != 0x80) {
                    return false; // 后续字节不是 10xxxxxx 格式 (Invalid continuation byte)
                }
            }
            pos += expected_bytes;
        }
        return true;
    }

    // ============================================================================
    // 核心扫描 (Core Scanning)
    // ============================================================================

    void Lexer::skip_whitespace() {
        while (!is_at_end()) {
            char c = peek();
            if (c == ' ' || c == '\t') {
                advance();
                column_++;
            } else {
                break;
            }
        }
    }

    Token Lexer::scan_token() {
        skip_whitespace();

        if (is_at_end()) {
            return Token{ TokenType::EndOfFile, current_location(), "" };
        }

        char c = peek();

        // 处理换行符
        if (c == '\n') {
            SourceLocation loc = current_location();
            advance();
            line_++;
            column_ = 1;
            return Token{ TokenType::Newline, loc, "\n" };
        }
        if (c == '\r') {
            SourceLocation loc = current_location();
            advance();
            if (!is_at_end() && peek() == '\n') {
                advance(); // 跳过 CRLF 中的 LF (虽然验证会阻止)
            }
            line_++;
            column_ = 1;
            return Token{ TokenType::Newline, loc, "\r\n" };
        }

        // 标识符或关键字 (Identifier or keyword)
        if (is_identifier_start(c)) {
            return read_identifier();
        }

        // 数字字面量 (Numeric literal)
        if (is_digit(c) || (c == '.' && is_digit(peek_next()))) {
            return read_number();
        }

        // 字符字面量 (Character literal)
        if (c == '\'') {
            return read_char_literal();
        }

        // 字符串字面量 (String literal)
        if (c == '"') {
            return read_string_literal();
        }

        // 注释 (Comment)
        if (c == '/') {
            char next = peek_next();
            if (next == '/' || next == '*') {
                return read_comment();
            }
        }

        // 运算符或分隔符 (Operator or delimiter)
        return read_operator_or_delimiter();
    }

    // ============================================================================
    // 标识符与关键字识别 (Identifier & Keyword Recognition)
    // ============================================================================

    Token Lexer::read_identifier() {
        SourceLocation start_loc = current_location();
        std::size_t start_pos = position_;

        char c = advance();
        while (!is_at_end() && is_identifier_continuation(peek())) {
            advance();
        }

        std::string_view lexeme = source_.substr(start_pos, position_ - start_pos);

        // 处理布尔字面量 (Handle boolean literals)
        if (lexeme == "true" || lexeme == "false") {
            return Token{ TokenType::BoolLiteral, start_loc, lexeme };
        }

        // 检查关键字映射 (Check keyword map)
        auto it = KEYWORD_MAP.find(lexeme);
        if (it != KEYWORD_MAP.end()) {
            return Token{ it->second, start_loc, lexeme };
        }

        // 普通标识符 (Plain identifier)
        return Token{ TokenType::Identifier, start_loc, lexeme };
    }

    // ============================================================================
    // 数字字面量识别 (Numeric Literal Recognition)
    // ============================================================================

    Token Lexer::read_number() {
        SourceLocation start_loc = current_location();
        std::size_t start_pos = position_;
        bool is_float = false;
        bool has_exponent = false;

        // 后缀扫描 + Token 构造（0.4.1 §7）
        // 整数字面量后缀：l/L → lint，u/U → uint，lu/Lu/lU/LU → luint
        // 浮点字面量后缀：f/F → float
        // Suffix scan and token construction (0.4.1 §7): integer literals accept
        // l/L, u/U and lu/Lu/lU/LU; floating literals accept only f/F.
        auto finish_number = [&](bool as_float) {
            std::size_t suffix_start = position_;
            while (!is_at_end() &&
                (std::isalnum(static_cast<unsigned char>(peek())) || peek() == '_')) {
                advance();
            }
            std::string_view suffix = source_.substr(suffix_start, position_ - suffix_start);
            if (!suffix.empty()) {
                const bool valid = as_float ? is_valid_float_suffix(suffix)
                                            : is_valid_integer_suffix(suffix);
                if (!valid) {
                    // ER 0025：0.4.1 更新为允许 'f'、'l'、'u' 与 'lu'
                    // ER 0025: 0.4.1 allows 'f', 'l', 'u' and 'lu'
                    report_error(ErrorCode::InvalidNumericSuffix,
                        "invalid numeric literal suffix: only 'f', 'l', 'u' and 'lu' "
                        "are allowed, got '" + std::string(suffix) + "'");
                }
            }
            std::string_view lexeme = source_.substr(start_pos, position_ - start_pos);
            return Token{ as_float ? TokenType::FloatLiteral : TokenType::IntegerLiteral,
                start_loc, lexeme };
        };

        // 检查十六进制前缀 (Check hex prefix)
        if (peek() == '0' && (peek_next() == 'x' || peek_next() == 'X')) {
            advance(); // 跳过 '0'
            advance(); // 跳过 'x'/'X'

            if (!is_hex_digit(peek())) {
                // 0x 后没有十六进制数字
                // 将其视为普通 '0' 后跟未知标记，但为了健壮性，我们回退
                // 但更安全的是报错，但标准没有明确禁止，我们简单回退
                // 但词法分析器通常不会在这里报错，因为这不是 ER 列表中的错误
                // 我们只返回一个整数 0
                return Token{ TokenType::IntegerLiteral, start_loc, source_.substr(start_pos, 1) };
            }

            while (!is_at_end() && is_hex_digit(peek())) {
                advance();
            }

            // 十六进制不带小数/指数，但仍是整数字面量，允许整数后缀
            // Hexadecimal literals carry no fraction/exponent but still accept
            // integer suffixes
            return finish_number(false);
        }

        // 八进制前缀 (Octal prefix) — 以 0 开头且后跟八进制数字
        if (peek() == '0' && is_octal_digit(peek_next())) {
            advance(); // 跳过第一个 '0'
            while (!is_at_end() && is_octal_digit(peek())) {
                advance();
            }
            // 八进制同样允许整数后缀（0.4.1 §7 未做限制）
            // Octal literals accept integer suffixes as well (0.4.1 §7)
            return finish_number(false);
        }

        // 整数或浮点数 (Integer or float)
        // 整数部分
        while (!is_at_end() && is_digit(peek())) {
            advance();
        }

        // 小数部分 (Fractional part)
        if (!is_at_end() && peek() == '.') {
            char next = peek_next();
            if (is_digit(next)) {
                is_float = true;
                advance(); // 跳过 '.'
                while (!is_at_end() && is_digit(peek())) {
                    advance();
                }
            }
        }

        // 指数部分 (Exponent part)
        if (!is_at_end() && (peek() == 'e' || peek() == 'E')) {
            char next = peek_next();
            if (is_digit(next) || next == '+' || next == '-') {
                is_float = true;
                has_exponent = true;
                advance(); // 跳过 'e'/'E'

                if (!is_at_end() && (peek() == '+' || peek() == '-')) {
                    advance(); // 跳过符号
                }

                if (!is_at_end() && is_digit(peek())) {
                    while (!is_at_end() && is_digit(peek())) {
                        advance();
                    }
                }
                else {
                    // 指数后没有数字，视为错误，但词法分析器容忍
                    // 但这里我们可以回退，不过标准没说，我们按C语言方式视为错误
                    // 但其实词法分析器最好只生成 token，语义检查处理类型
                    // 我们仍然返回 float token，但后面语义会报错
                }
            }
        }

        (void)has_exponent;
        return finish_number(is_float);
    }

    // ============================================================================
    // 字符字面量识别 (Character Literal Recognition)
    // ============================================================================

    Token Lexer::read_char_literal() {
        SourceLocation start_loc = current_location();
        std::size_t start_pos = position_;

        // 参照 Gallt 0.2.txt §9: 转义符和编号表示字符
        // Reference: Gallt 0.2.txt §9: Escape sequences and numbered characters

        advance(); // 跳过开头的 '

        if (is_at_end() || peek() == '\'') {
            // ER 0023: 字符常量格式错误：空字符
            // ER 0023: Invalid character literal: empty character
            report_error(ErrorCode::InvalidCharLiteral, "empty character literal");
            if (!is_at_end()) advance(); // 跳过闭合引号以避免无限循环
            return Token{ TokenType::CharLiteral, start_loc, source_.substr(start_pos, position_ - start_pos) };
        }

        // 解析字符内容
        char ch = 0;
        if (peek() == '\\') {
            // 转义序列 (Escape sequence)
            ch = parse_escape_sequence(position_, start_loc, true);
        }
        else {
            ch = advance();
        }

        // 检查闭合引号
        if (is_at_end() || peek() != '\'') {
            // ER 0023: 字符常量格式错误：无效转义序列或未闭合
            // ER 0023: Invalid character literal: invalid escape sequence or unclosed
            report_error(ErrorCode::InvalidCharLiteral, "unclosed character literal or invalid escape sequence");
            // 尝试跳过到下一个 ' 或换行
            while (!is_at_end() && peek() != '\'' && peek() != '\n') {
                advance();
            }
            if (!is_at_end() && peek() == '\'') {
                advance(); // 跳过闭合引号
            }
        }
        else {
            advance(); // 跳过闭合引号 '
        }

        std::string_view lexeme = source_.substr(start_pos, position_ - start_pos);
        return Token{ TokenType::CharLiteral, start_loc, lexeme };
    }

    // ============================================================================
    // 字符串字面量识别 (String Literal Recognition)
    // ============================================================================

    Token Lexer::read_string_literal() {
        SourceLocation start_loc = current_location();
        std::size_t start_pos = position_;

        // 参照 Gallt 0.2.txt §9: 字符串支持转义符
        // Reference: Gallt 0.2.txt §9: Strings support escape sequences

        advance(); // 跳过开头的 "

        bool is_closed = false;
        while (!is_at_end()) {
            char c = peek();
            if (c == '"') {
                advance(); // 跳过闭合的 "
                is_closed = true;
                break;
            }
            if (c == '\\') {
                parse_escape_sequence(position_, start_loc, false);
            }
            else {
                // 普通字符
                advance();
            }
        }

        if (!is_closed) {
            // ER 0024: 字符串常量格式错误：未闭合的字符串字面量
            // ER 0024: Invalid string literal: unclosed string literal
            report_error(ErrorCode::UnclosedStringLiteral, "unclosed string literal");
        }

        std::string_view lexeme = source_.substr(start_pos, position_ - start_pos);
        return Token{ TokenType::StringLiteral, start_loc, lexeme };
    }

    // ============================================================================
    // 注释识别 (Comment Recognition)
    // ============================================================================

    Token Lexer::read_comment() {
        SourceLocation start_loc = current_location();
        std::size_t start_pos = position_;

        advance(); // 跳过 '/'

        if (peek() == '/') {
            // 单行注释 (Single-line comment) // ...
            advance(); // 跳过第二个 '/'
            while (!is_at_end() && peek() != '\n') {
                advance();
            }
            // 注释不产生 Token，返回下一个 Token
            return scan_token();
        }
        else if (peek() == '*') {
            // 多行注释 (Multi-line comment) /* ... */
            advance(); // 跳过 '*'

            bool is_closed = false;
            while (!is_at_end()) {
                if (peek() == '*' && peek_next() == '/') {
                    advance(); // 跳过 '*'
                    advance(); // 跳过 '/'
                    is_closed = true;
                    break;
                }
                // 处理换行更新位置
                if (peek() == '\n') {
                    line_++;
                    column_ = 1;
                }
                else {
                    column_++;
                }
                advance();
            }

            if (!is_closed) {
                // 参照 C 标准: 未闭合的注释是未定义行为，但我们会报错（但在 ER 表中没有对应条目）
                // 我们使用 ER 0020 (表达式语法错误) 或 ER 0024 变体，但作为词法错误，我们报 ER 0020 是最接近的
                // 严格来说标准没有定义，我们报一个通用错误
                // 实际上，我们使用 report_error 但不指定特定错误码，或者使用最接近的。
                // 文档没有明确说明未闭合注释的错误码，我们使用 ER 0020 作为语法错误。
                // 但为了更准确，我们实际上应该报错，但继续扫描。
                // 这里我们使用 ER 0020 或 ER 0024？实际上 ER 0024 是字符串，不合适。
                // 我们使用 ER 0020: 表达式语法错误，但 "comment not closed"。
                // 不过我们作为编译器编写者，应该尽量与文档对齐。文档只列了 ER 0020 是通用的表达式语法错误。
                // 所以我们报 ER 0020。
                report_error(ErrorCode::ExpressionSyntaxError, "unclosed multi-line comment");
            }
            // 注释不产生 Token
            return scan_token();
        }

        // 单个 '/' 不是注释，是除法运算符
        // 但这种情况不会发生，因为 scan_token 检查了 '/'
        // 这里是防御性代码
        return Token{ TokenType::Slash, start_loc, "/" };
    }

    // ============================================================================
    // 运算符与分隔符识别 (Operator & Delimiter Recognition)
    // ============================================================================

    Token Lexer::read_operator_or_delimiter() {
        SourceLocation start_loc = current_location();
        char c = advance(); // 消费当前字符

        switch (c) {
            // 赋值与相等 (Assignment & Equality)
        case '=':
            if (!is_at_end() && peek() == '=') {
                advance();
                return Token{ TokenType::Equal, start_loc, "==" };
            }
            return Token{ TokenType::Assign, start_loc, "=" };

            // 加法与自增 (Addition & Increment)
        case '+':
            if (!is_at_end() && peek() == '+') {
                advance();
                return Token{ TokenType::Increment, start_loc, "++" };
            }
            if (!is_at_end() && peek() == '=') {
                advance();
                return Token{ TokenType::PlusAssign, start_loc, "+=" };
            }
            return Token{ TokenType::Plus, start_loc, "+" };

            // 减法、自减、箭头 (Subtraction, Decrement, Arrow)
        case '-':
            if (!is_at_end() && peek() == '-') {
                advance();
                return Token{ TokenType::Decrement, start_loc, "--" };
            }
            if (!is_at_end() && peek() == '=') {
                advance();
                return Token{ TokenType::MinusAssign, start_loc, "-=" };
            }
            if (!is_at_end() && peek() == '>') {
                advance();
                return Token{ TokenType::Arrow, start_loc, "->" };
            }
            return Token{ TokenType::Minus, start_loc, "-" };

            // 乘法、幂运算、解引用 (Multiplication, Power, Dereference)
        case '*':
            if (!is_at_end() && peek() == '*') {
                advance();
                return Token{ TokenType::Power, start_loc, "**" };
            }
            return Token{ TokenType::Star, start_loc, "*" };

            // 除法 (Division)
        case '/':
            return Token{ TokenType::Slash, start_loc, "/" };

            // 取余 (Remainder, Gallt 0.2.txt §3)
        case '%':
            return Token{ TokenType::Percent, start_loc, "%" };

            // 大于、大于等于 (Greater, GreaterEqual)
        case '>':
            if (!is_at_end() && peek() == '=') {
                advance();
                return Token{ TokenType::GreaterEqual, start_loc, ">=" };
            }
            return Token{ TokenType::Greater, start_loc, ">" };

            // 小于、小于等于 (Less, LessEqual)
        case '<':
            if (!is_at_end() && peek() == '=') {
                advance();
                return Token{ TokenType::LessEqual, start_loc, "<=" };
            }
            return Token{ TokenType::Less, start_loc, "<" };

            // 不等于 (NotEqual)
        case '!':
            if (!is_at_end() && peek() == '=') {
                advance();
                return Token{ TokenType::NotEqual, start_loc, "!=" };
            }
            return Token{ TokenType::LogicalNot, start_loc, "!" };

            // 逻辑与 (Logical AND)
        case '&':
            if (!is_at_end() && peek() == '&') {
                advance();
                return Token{ TokenType::LogicalAnd, start_loc, "&&" };
            }
            // 单个 & 是取地址运算符 (Address-of operator)
            return Token{ TokenType::AddressOf, start_loc, "&" };

            // 逻辑或 (Logical OR)
        case '|':
            if (!is_at_end() && peek() == '|') {
                advance();
                return Token{ TokenType::LogicalOr, start_loc, "||" };
            }
            // Gallt 0.3.txt §19：单个 '|' 用于泛型类型参数约束的联合类型
            // Gallt 0.3.txt §19: a single '|' separates union members in generic constraints
            return Token{ TokenType::Pipe, start_loc, "|" };

            // 分隔符 (Delimiters)
        case '(':
            return Token{ TokenType::LeftParen, start_loc, "(" };
        case ')':
            return Token{ TokenType::RightParen, start_loc, ")" };
        case '{':
            return Token{ TokenType::LeftBrace, start_loc, "{" };
        case '}':
            return Token{ TokenType::RightBrace, start_loc, "}" };
        case '[':
            return Token{ TokenType::LeftBracket, start_loc, "[" };
        case ']':
            return Token{ TokenType::RightBracket, start_loc, "]" };
        case ',':
            return Token{ TokenType::Comma, start_loc, "," };
        case ';':
            return Token{ TokenType::Semicolon, start_loc, ";" };
        case ':':
            // Gallt 0.4.txt §3：`::` 是访问运算符，必须先于单字符 ':' 识别
            // Gallt 0.4.txt §3: '::' is an access operator and must win over ':'
            if (!is_at_end() && peek() == ':') {
                advance();
                return Token{ TokenType::ColonColon, start_loc, "::" };
            }
            // Gallt 0.3.txt §19：类型参数约束分隔符 `T: constraint`
            // Gallt 0.3.txt §19: type-parameter constraint separator
            return Token{ TokenType::Colon, start_loc, ":" };
        case '.':
            return Token{ TokenType::Dot, start_loc, "." };

        default: {
            // 未知字符 (Unknown character)
            std::string msg = "unexpected character '";
            msg.push_back(c);
            msg.push_back('\'');
            report_error(ErrorCode::ExpressionSyntaxError, msg);
            // 返回一个未知 Token，让语法分析器处理
            return Token{ TokenType::Unknown, start_loc, std::string_view(&c, 1) };
        }
        }
    }

    // ============================================================================
    // 转义序列解析 (Escape Sequence Parsing)
    // ============================================================================

    char Lexer::parse_escape_sequence(std::size_t& pos, SourceLocation loc, bool is_char) {
        // 参照 Gallt 0.2.txt §9: 转义符
        // Reference: Gallt 0.2.txt §9: Escape sequences
        // \n \t \r \b \f \v \0 \\ \" \' \ddd \xhh

        if (pos >= source_.size() || source_[pos] != '\\') {
            return '\\'; // 不应该发生
        }

        pos++; // 跳过反斜杠 '\'
        if (pos >= source_.size()) {
            report_error(ErrorCode::InvalidCharLiteral, "unexpected end after escape sequence");
            return '\\';
        }

        char c = source_[pos];
        pos++; // 消费转义字符

        switch (c) {
        case 'n':  return '\n';
        case 't':  return '\t';
        case 'r':  return '\r';
        case 'b':  return '\b';
        case 'f':  return '\f';
        case 'v':  return '\v';
        case '0':  return '\0';
        case '\\': return '\\';
        case '"':  return '"';
        case '\'': return '\'';

        case 'x': {
            // 十六进制转义 \xhh (1-2 位)
            // Hexadecimal escape \xhh (1-2 digits)
            if (pos >= source_.size()) {
                report_error(ErrorCode::InvalidCharLiteral, "incomplete hex escape sequence");
                return 'x';
            }

            char hex_digits[3] = { 0, 0, 0 };
            int count = 0;
            while (count < 2 && pos < source_.size() && is_hex_digit(source_[pos])) {
                hex_digits[count] = source_[pos];
                pos++;
                count++;
            }

            if (count == 0) {
                report_error(ErrorCode::InvalidCharLiteral, "hex escape sequence requires at least one hex digit");
                return 'x';
            }

            // 将十六进制字符串转换为数字
            std::string_view hex_str(hex_digits, count);
            int value = 0;
            auto [ptr, ec] = std::from_chars(hex_str.data(), hex_str.data() + hex_str.size(), value, 16);
            if (ec != std::errc()) {
                report_error(ErrorCode::InvalidCharLiteral, "invalid hex escape sequence");
                return 'x';
            }
            return static_cast<char>(value);
        }

        default: {
            // 八进制转义 \ddd (1-3 位)
            // Octal escape \ddd (1-3 digits)
            if (is_octal_digit(c)) {
                // 第一个八进制数字已经在 c 中
                char oct_digits[4] = { c, 0, 0, 0 };
                int count = 1;
                while (count < 3 && pos < source_.size() && is_octal_digit(source_[pos])) {
                    oct_digits[count] = source_[pos];
                    pos++;
                    count++;
                }

                std::string_view oct_str(oct_digits, count);
                int value = 0;
                auto [ptr, ec] = std::from_chars(oct_str.data(), oct_str.data() + oct_str.size(), value, 8);
                if (ec != std::errc()) {
                    report_error(ErrorCode::InvalidCharLiteral, "invalid octal escape sequence");
                    return c;
                }
                if (value > 255) {
                    report_error(ErrorCode::InvalidCharLiteral, "octal escape sequence value out of range (max 377)");
                    return c;
                }
                return static_cast<char>(value);
            }

            // 未知转义序列 (Unknown escape sequence)
            std::string msg = "unknown escape sequence '\\";
            msg.push_back(c);
            msg.push_back('\'');
            report_error(ErrorCode::InvalidCharLiteral, msg);
            return c; // 返回字符本身
        }
        }
    }

    // ============================================================================
    // 辅助函数 (Helper Functions)
    // ============================================================================

    char Lexer::advance() {
        if (is_at_end()) {
            return '\0';
        }
        char c = source_[position_];
        position_++;
        column_++;
        return c;
    }

    char Lexer::peek() const {
        if (is_at_end()) {
            return '\0';
        }
        return source_[position_];
    }

    char Lexer::peek_next() const {
        if (position_ + 1 >= source_.size()) {
            return '\0';
        }
        return source_[position_ + 1];
    }

    bool Lexer::is_at_end() const {
        return position_ >= source_.size();
    }

    bool Lexer::match(char expected) {
        if (is_at_end() || peek() != expected) {
            return false;
        }
        advance();
        return true;
    }

    void Lexer::skip() {
        if (!is_at_end()) {
            position_++;
            column_++;
        }
    }

    // ============================================================================
    // 错误报告辅助 (Error Reporting Helpers)
    // ============================================================================

    void Lexer::report_error(ErrorCode code, std::string_view message) {
        diag_.report_error(current_location(), code, message);
        has_error_ = true;
    }

    void Lexer::report_error_at(SourceLocation loc, ErrorCode code, std::string_view message) {
        diag_.report_error(loc, code, message);
        has_error_ = true;
    }

} // namespace gallt
