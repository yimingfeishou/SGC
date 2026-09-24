#ifndef GALLT_LEXER_LEXER_HPP
#define GALLT_LEXER_LEXER_HPP

#include "../common/token.hpp"
#include "../common/diagnostics.hpp"
#include <string_view>
#include <vector>
#include <optional>

namespace gallt {

    class Lexer {
    public:
        Lexer(std::string_view source, std::string_view filename, DiagnosticEngine& diag);
        ~Lexer() = default;

        Lexer(const Lexer&) = delete;
        Lexer& operator=(const Lexer&) = delete;

        Token next_token();

        Token peek_token();

        SourceLocation current_location() const;

        std::string_view remaining_source() const;

        bool at_end() const noexcept;

        void validate_file();

    private:
        std::string_view source_;
        std::string_view filename_;
        DiagnosticEngine& diag_;

        std::size_t position_ = 0;
        std::size_t line_ = 1;
        std::size_t column_ = 1;

        bool has_error_ = false;

        std::optional<Token> peeked_token_;

        void skip_whitespace();

        Token scan_token();

        Token read_identifier();
        Token read_number();
        Token read_char_literal();
        Token read_string_literal();
        Token read_comment();
        Token read_operator_or_delimiter();

        char advance();
        char peek() const;
        char peek_next() const;
        bool is_at_end() const;
        bool match(char expected);
        void skip();

        void report_error(ErrorCode code, std::string_view message);
        void report_error_at(SourceLocation loc, ErrorCode code, std::string_view message);

        char parse_escape_sequence(std::size_t& pos, SourceLocation loc, bool is_char);

        bool validate_line_endings();
        bool validate_utf8();
    };

}

#endif
