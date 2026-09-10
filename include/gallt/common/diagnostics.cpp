// common/diagnostics.cpp
// 诊断引擎实现 —— 收集、格式化并输出诊断
// Diagnostic engine implementation — collects, formats, and prints diagnostics

#include "diagnostics.hpp"

#include <cstdio>
#include <sstream>

namespace gallt {

    // ============================================================================
    // 错误码格式化为 "ER XXXX" / "RTER XXXX"
    // Format an error code as "ER XXXX" / "RTER XXXX"
    // ============================================================================

    std::string Diagnostic::error_code_string(ErrorCode code) {
        std::string s = std::to_string(static_cast<std::uint16_t>(code));
        while (s.size() < 4u) {
            s.insert(s.begin(), '0');
        }
        return "ER " + s;
    }

    std::string Diagnostic::runtime_error_code_string(RuntimeErrorCode code) {
        std::string s = std::to_string(static_cast<std::uint16_t>(code));
        while (s.size() < 4u) {
            s.insert(s.begin(), '0');
        }
        return "RTER " + s;
    }

    // ============================================================================
    // 诊断引擎实现
    // Diagnostic engine implementation
    // ============================================================================

    std::string DiagnosticEngine::severity_prefix(DiagnosticSeverity sev) {
        // 按严重级别返回短标签；Note 不携带错误码
        // Return the short label by severity; notes carry no error code
        switch (sev) {
        case DiagnosticSeverity::Error:   return "error";
        case DiagnosticSeverity::Warning: return "warning";
        case DiagnosticSeverity::Note:    return "note";
        }
        return "note";
    }

    std::string DiagnosticEngine::format_message(ErrorCode code, std::string_view user_msg) {
        // 当前诊断消息已经由调用者完成格式化，这里原样保留
        // Callers already format the message; keep it verbatim
        (void)code;
        return std::string(user_msg);
    }

    void DiagnosticEngine::report(DiagnosticSeverity sev, SourceLocation loc, ErrorCode code, std::string_view msg) {
        Diagnostic d;
        d.severity = sev;
        d.location = loc;
        d.code = code;
        d.message = format_message(code, msg);
        diagnostics_.push_back(std::move(d));
        if (sev == DiagnosticSeverity::Error) {
            ++error_count_;
        } else if (sev == DiagnosticSeverity::Warning) {
            ++warning_count_;
        }
    }

    void DiagnosticEngine::report(DiagnosticSeverity sev, SourceLocation loc, std::string_view msg) {
        Diagnostic d;
        d.severity = sev;
        d.location = loc;
        d.code = ErrorCode::ExpressionSyntaxError; // note 不使用错误码 / notes carry no error code
        d.message = std::string(msg);
        diagnostics_.push_back(std::move(d));
        if (sev == DiagnosticSeverity::Error) {
            ++error_count_;
        } else if (sev == DiagnosticSeverity::Warning) {
            ++warning_count_;
        }
    }

    void DiagnosticEngine::report_error(SourceLocation loc, ErrorCode code, std::string_view message) {
        report(DiagnosticSeverity::Error, loc, code, message);
    }

    void DiagnosticEngine::report_warning(SourceLocation loc, ErrorCode code, std::string_view message) {
        report(DiagnosticSeverity::Warning, loc, code, message);
    }

    void DiagnosticEngine::report_note(SourceLocation loc, std::string_view message) {
        report(DiagnosticSeverity::Note, loc, message);
    }

    void DiagnosticEngine::report_runtime_error(RuntimeErrorCode code, std::string_view message) {
        // 运行时错误直接打印到标准错误；编译器中仅为将来嵌入运行库保留接口
        // Runtime errors go directly to stderr; retained for future runtime embedding
        std::fprintf(stderr, "%s: %.*s\n",
            Diagnostic::runtime_error_code_string(code).c_str(),
            static_cast<int>(message.size()), message.data());
    }

    void DiagnosticEngine::clear() {
        diagnostics_.clear();
        error_count_ = 0;
        warning_count_ = 0;
    }

    void DiagnosticEngine::print_all(std::ostream& os) const {
        for (const Diagnostic& d : diagnostics_) {
            // filename:line:col: severity: ER XXXX: message
            if (d.location.is_valid() && !d.location.filename.empty()) {
                os << d.location.filename << ':'
                   << d.location.line << ':'
                   << d.location.column << ": ";
            } else if (d.location.is_valid()) {
                os << "<input>:" << d.location.line << ':' << d.location.column << ": ";
            }
            os << severity_prefix(d.severity) << ": ";
            if (d.severity != DiagnosticSeverity::Note) {
                os << Diagnostic::error_code_string(d.code) << ": ";
            }
            os << d.message << '\n';
        }
    }

    void DiagnosticEngine::print_all(std::ostream& os, DiagnosticSeverity min_severity) const {
        for (const Diagnostic& d : diagnostics_) {
            auto rank = [](DiagnosticSeverity s) -> int {
                // Error(0) < Warning(1) < Note(2)
                switch (s) {
                case DiagnosticSeverity::Error: return 0;
                case DiagnosticSeverity::Warning: return 1;
                case DiagnosticSeverity::Note: return 2;
                }
                return 2;
            };
            auto min_rank = [&]() -> int {
                switch (min_severity) {
                case DiagnosticSeverity::Error: return 0;
                case DiagnosticSeverity::Warning: return 1;
                case DiagnosticSeverity::Note: return 2;
                }
                return 0;
            };
            if (rank(d.severity) >= min_rank()) {
                if (d.location.is_valid() && !d.location.filename.empty()) {
                    os << d.location.filename << ':'
                       << d.location.line << ':'
                       << d.location.column << ": ";
                } else if (d.location.is_valid()) {
                    os << "<input>:" << d.location.line << ':' << d.location.column << ": ";
                }
                os << severity_prefix(d.severity) << ": ";
                if (d.severity != DiagnosticSeverity::Note) {
                    os << Diagnostic::error_code_string(d.code) << ": ";
                }
                os << d.message << '\n';
            }
        }
    }

} // namespace gallt
