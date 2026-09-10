// driver/command_line.cpp
// 命令行参数解析实现
// Implementation of command-line argument parsing

#include "command_line.hpp"

#include <windows.h>

namespace gallt {
namespace {
    std::string narrow_utf8(const std::wstring& w) {
        // Windows 命令行通常是 UTF-16，转换为编译器内部使用的 UTF-8
        // Windows command lines are UTF-16; convert to UTF-8 internally
        if (w.empty()) return std::string();
        int len = ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(),
            static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
        std::string out(static_cast<size_t>(len), '\0');
        ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
            out.data(), len, nullptr, nullptr);
        return out;
    }
}

    bool parse_command_line(int argc, const wchar_t* const* argv, CommandOptions& out) {
        out = CommandOptions{};
        if (argv == nullptr || argc <= 0) {
            out.mode = CommandMode::Invalid;
            out.error_message = "no arguments were provided";
            return false;
        }
        for (int i = 1; i < argc; ++i) {
            std::wstring arg = argv[i];
            if (arg == L"--help" || arg == L"-h" || arg == L"/?") {
                out.mode = CommandMode::Help;
                continue;
            }
            if (arg == L"--version" || arg == L"-V") {
                out.mode = CommandMode::Version;
                continue;
            }
            if (arg == L"--compile") {
                out.mode = CommandMode::Compile;
                continue;
            }
            auto require_value = [&](const wchar_t* name) -> std::string* {
                if (i + 1 >= argc) {
                    out.error_message = "missing value after " + narrow_utf8(arg);
                    return nullptr;
                }
                ++i;
                static std::string value;
                value = narrow_utf8(argv[i]);
                return &value;
            };
            if (arg == L"--input" || arg == L"-i") {
                std::string* v = require_value(L"--input");
                if (!v) return false;
                out.input = *v;
            } else if (arg == L"--output" || arg == L"-o") {
                std::string* v = require_value(L"--output");
                if (!v) return false;
                out.output = *v;
            } else if (!arg.empty() && arg[0] == L'-') {
                out.mode = CommandMode::Invalid;
                out.error_message = "unknown option: " + narrow_utf8(arg);
                return false;
            } else {
                out.positional.push_back(narrow_utf8(arg));
            }
        }
        return true;
    }

    std::string help_text() {
        // 与 Doc/编译器参数.txt 对齐
        // Aligned with Doc/compiler-options.txt
        return
            "Standard Gallt Compiler (sgc)\n"
            "Usage:\n"
            "  sgc --compile --input \"file.glt\" --output \"program.exe\"\n"
            "  sgc --help\n"
            "  sgc --version\n";
    }

    std::string version_text() {
        return
            "sgc Standard Gallt Compiler 0.2.0 (LLVM backend, x86-64 Windows)\n"
            "Build date: 2026-09-10\n"
            "This version is an early preview release and may encounter errors or unhandled edge cases.";
    }

} // namespace gallt
