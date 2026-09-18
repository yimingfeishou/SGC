#include "../lexer/lexer.hpp"
#include <cctype>
#include <unordered_map>
#include <optional>
#include <charconv>
#include <system_error>

namespace gallt {

    namespace {
        const std::unordered_map<std::string_view, TokenType> KEYWORD_MAP = {
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
            {"const", TokenType::Keyword_Const},
        };

        bool is_identifier_start(char c) noexcept {
            return std::isalpha(static_cast<unsigned char>(c)) || c == '_';
        }

        bool is_identifier_continuation(char c) noexcept {
            return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '$';
        }

        bool is_whitespace(char c) noexcept {
            return c == ' ' || c == '\t' || c == '\n' || c == '\r';
        }

        bool is_digit(char c) noexcept {
            return std::isdigit(static_cast<unsigned char>(c));
        }

        bool is_hex_digit(char c) noexcept {
            return std::isxdigit(static_cast<unsigned char>(c));
        }

        bool is_octal_digit(char c) noexcept {
            return c >= '0' && c <= '7';
        }

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
    } 

    Lexer::Lexer(std::string_view source, std::string_view filename, DiagnosticEngine& diag)
        : source_(source)
        , filename_(filename)
        , diag_(diag) {
        if (source_.size() >= 3 &&
            static_cast<unsigned char>(source_[0]) == 0xEF &&
            static_cast<unsigned char>(source_[1]) == 0xBB &&
            static_cast<unsigned char>(source_[2]) == 0xBF) {
            position_ = 3;
        }
        validate_file();
    }

    Token Lexer::next_token() {
        if (peeked_token_.has_value()) {
            Token result = peeked_token_.value();
            peeked_token_.reset();
            return result;
        }

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


    void Lexer::validate_file() {
        bool line_ending_ok = validate_line_endings();

        bool encoding_ok = validate_utf8();

        if (!line_ending_ok || !encoding_ok) {
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
                if (pos + 1 < source_.size() && source_[pos + 1] == '\n') {
                    return false; 
                }
                return false; 
            }
            pos++;
        }
        return true;
    }

    bool Lexer::validate_utf8() {
        std::size_t pos = 0;
        while (pos < source_.size()) {
            unsigned char c = static_cast<unsigned char>(source_[pos]);

            if (c <= 0x7F) {
                pos++;
                continue;
            }

            int expected_bytes = 0;
            if ((c & 0xE0) == 0xC0) {      
                expected_bytes = 2;
            }
            else if ((c & 0xF0) == 0xE0) { 
                expected_bytes = 3;
            }
            else if ((c & 0xF8) == 0xF0) { 
                expected_bytes = 4;
            }
            else {
                return false; 
            }

            for (int i = 1; i < expected_bytes; i++) {
                if (pos + i >= source_.size()) {
                    return false; 
                }
                unsigned char cont = static_cast<unsigned char>(source_[pos + i]);
                if ((cont & 0xC0) != 0x80) {
                    return false; 
                }
            }
            pos += expected_bytes;
        }
        return true;
    }

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
                advance(); 
            }
            line_++;
            column_ = 1;
            return Token{ TokenType::Newline, loc, "\r\n" };
        }

        if (is_identifier_start(c)) {
            return read_identifier();
        }

        if (is_digit(c) || (c == '.' && is_digit(peek_next()))) {
            return read_number();
        }

        if (c == '\'') {
            return read_char_literal();
        }

        if (c == '"') {
            return read_string_literal();
        }

        if (c == '/') {
            char next = peek_next();
            if (next == '/' || next == '*') {
                return read_comment();
            }
        }

        return read_operator_or_delimiter();
    }

    Token Lexer::read_identifier() {
        SourceLocation start_loc = current_location();
        std::size_t start_pos = position_;

        char c = advance();
        while (!is_at_end() && is_identifier_continuation(peek())) {
            advance();
        }

        std::string_view lexeme = source_.substr(start_pos, position_ - start_pos);

        if (lexeme == "true" || lexeme == "false") {
            return Token{ TokenType::BoolLiteral, start_loc, lexeme };
        }

        auto it = KEYWORD_MAP.find(lexeme);
        if (it != KEYWORD_MAP.end()) {
            return Token{ it->second, start_loc, lexeme };
        }

        return Token{ TokenType::Identifier, start_loc, lexeme };
    }

    Token Lexer::read_number() {
        SourceLocation start_loc = current_location();
        std::size_t start_pos = position_;
        bool is_float = false;
        bool has_exponent = false;

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
                    report_error(ErrorCode::InvalidNumericSuffix,
                        "invalid numeric literal suffix: only 'f', 'l', 'u' and 'lu' "
                        "are allowed, got '" + std::string(suffix) + "'");
                }
            }
            std::string_view lexeme = source_.substr(start_pos, position_ - start_pos);
            return Token{ as_float ? TokenType::FloatLiteral : TokenType::IntegerLiteral,
                start_loc, lexeme };
        };

        if (peek() == '0' && (peek_next() == 'x' || peek_next() == 'X')) {
            advance(); 
            advance(); 

            if (!is_hex_digit(peek())) {
                return Token{ TokenType::IntegerLiteral, start_loc, source_.substr(start_pos, 1) };
            }

            while (!is_at_end() && is_hex_digit(peek())) {
                advance();
            }

            return finish_number(false);
        }

        if (peek() == '0' && is_octal_digit(peek_next())) {
            advance(); 
            while (!is_at_end() && is_octal_digit(peek())) {
                advance();
            }
            return finish_number(false);
        }

        while (!is_at_end() && is_digit(peek())) {
            advance();
        }

        if (!is_at_end() && peek() == '.') {
            char next = peek_next();
            if (is_digit(next)) {
                is_float = true;
                advance(); 
                while (!is_at_end() && is_digit(peek())) {
                    advance();
                }
            }
        }

        if (!is_at_end() && (peek() == 'e' || peek() == 'E')) {
            char next = peek_next();
            if (is_digit(next) || next == '+' || next == '-') {
                is_float = true;
                has_exponent = true;
                advance(); 

                if (!is_at_end() && (peek() == '+' || peek() == '-')) {
                    advance(); 
                }

                if (!is_at_end() && is_digit(peek())) {
                    while (!is_at_end() && is_digit(peek())) {
                        advance();
                    }
                }
                else {
                }
            }
        }

        (void)has_exponent;
        return finish_number(is_float);
    }

    Token Lexer::read_char_literal() {
        SourceLocation start_loc = current_location();
        std::size_t start_pos = position_;


        advance(); 

        if (is_at_end() || peek() == '\'') {
            report_error(ErrorCode::InvalidCharLiteral, "empty character literal");
            if (!is_at_end()) advance(); 
            return Token{ TokenType::CharLiteral, start_loc, source_.substr(start_pos, position_ - start_pos) };
        }

        char ch = 0;
        if (peek() == '\\') {
            ch = parse_escape_sequence(position_, start_loc, true);
        }
        else {
            ch = advance();
        }

        if (is_at_end() || peek() != '\'') {
            report_error(ErrorCode::InvalidCharLiteral, "unclosed character literal or invalid escape sequence");
            while (!is_at_end() && peek() != '\'' && peek() != '\n') {
                advance();
            }
            if (!is_at_end() && peek() == '\'') {
                advance(); 
            }
        }
        else {
            advance(); 
        }

        std::string_view lexeme = source_.substr(start_pos, position_ - start_pos);
        return Token{ TokenType::CharLiteral, start_loc, lexeme };
    }

    Token Lexer::read_string_literal() {
        SourceLocation start_loc = current_location();
        std::size_t start_pos = position_;


        advance(); 

        bool is_closed = false;
        while (!is_at_end()) {
            char c = peek();
            if (c == '"') {
                advance(); 
                is_closed = true;
                break;
            }
            if (c == '\\') {
                parse_escape_sequence(position_, start_loc, false);
            }
            else {
                advance();
            }
        }

        if (!is_closed) {
            report_error(ErrorCode::UnclosedStringLiteral, "unclosed string literal");
        }

        std::string_view lexeme = source_.substr(start_pos, position_ - start_pos);
        return Token{ TokenType::StringLiteral, start_loc, lexeme };
    }

    Token Lexer::read_comment() {
        SourceLocation start_loc = current_location();
        std::size_t start_pos = position_;

        advance(); 

        if (peek() == '/') {
            advance(); 
            while (!is_at_end() && peek() != '\n') {
                advance();
            }
            return scan_token();
        }
        else if (peek() == '*') {
            advance(); 

            bool is_closed = false;
            while (!is_at_end()) {
                if (peek() == '*' && peek_next() == '/') {
                    advance(); 
                    advance(); 
                    is_closed = true;
                    break;
                }
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
                report_error(ErrorCode::ExpressionSyntaxError, "unclosed multi-line comment");
            }
            return scan_token();
        }

        return Token{ TokenType::Slash, start_loc, "/" };
    }

    Token Lexer::read_operator_or_delimiter() {
        SourceLocation start_loc = current_location();
        char c = advance(); 

        switch (c) {
        case '=':
            if (!is_at_end() && peek() == '=') {
                advance();
                return Token{ TokenType::Equal, start_loc, "==" };
            }
            return Token{ TokenType::Assign, start_loc, "=" };

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

        case '*':
            if (!is_at_end() && peek() == '*') {
                advance();
                return Token{ TokenType::Power, start_loc, "**" };
            }
            return Token{ TokenType::Star, start_loc, "*" };

        case '/':
            return Token{ TokenType::Slash, start_loc, "/" };

        case '%':
            return Token{ TokenType::Percent, start_loc, "%" };

        case '>':
            if (!is_at_end() && peek() == '=') {
                advance();
                return Token{ TokenType::GreaterEqual, start_loc, ">=" };
            }
            return Token{ TokenType::Greater, start_loc, ">" };

        case '<':
            if (!is_at_end() && peek() == '=') {
                advance();
                return Token{ TokenType::LessEqual, start_loc, "<=" };
            }
            return Token{ TokenType::Less, start_loc, "<" };

        case '!':
            if (!is_at_end() && peek() == '=') {
                advance();
                return Token{ TokenType::NotEqual, start_loc, "!=" };
            }
            return Token{ TokenType::LogicalNot, start_loc, "!" };

        case '&':
            if (!is_at_end() && peek() == '&') {
                advance();
                return Token{ TokenType::LogicalAnd, start_loc, "&&" };
            }
            return Token{ TokenType::AddressOf, start_loc, "&" };

        case '|':
            if (!is_at_end() && peek() == '|') {
                advance();
                return Token{ TokenType::LogicalOr, start_loc, "||" };
            }
            return Token{ TokenType::Pipe, start_loc, "|" };

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
            if (!is_at_end() && peek() == ':') {
                advance();
                return Token{ TokenType::ColonColon, start_loc, "::" };
            }
            return Token{ TokenType::Colon, start_loc, ":" };
        case '.':
            return Token{ TokenType::Dot, start_loc, "." };
        case '@':
            return Token{ TokenType::At, start_loc, "@" };

        default: {
            std::string msg = "unexpected character '";
            msg.push_back(c);
            msg.push_back('\'');
            report_error(ErrorCode::ExpressionSyntaxError, msg);
            return Token{ TokenType::Unknown, start_loc, std::string_view(&c, 1) };
        }
        }
    }

    char Lexer::parse_escape_sequence(std::size_t& pos, SourceLocation loc, bool is_char) {

        if (pos >= source_.size() || source_[pos] != '\\') {
            return '\\'; 
        }

        pos++; 
        if (pos >= source_.size()) {
            report_error(ErrorCode::InvalidCharLiteral, "unexpected end after escape sequence");
            return '\\';
        }

        char c = source_[pos];
        pos++; 

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
            if (is_octal_digit(c)) {
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

            std::string msg = "unknown escape sequence '\\";
            msg.push_back(c);
            msg.push_back('\'');
            report_error(ErrorCode::InvalidCharLiteral, msg);
            return c; 
        }
        }
    }

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

    void Lexer::report_error(ErrorCode code, std::string_view message) {
        diag_.report_error(current_location(), code, message);
        has_error_ = true;
    }

    void Lexer::report_error_at(SourceLocation loc, ErrorCode code, std::string_view message) {
        diag_.report_error(loc, code, message);
        has_error_ = true;
    }

} 
