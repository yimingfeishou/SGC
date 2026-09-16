#include "command_line.hpp"
#include <windows.h>

namespace gallt {
namespace {
    std::string narrow_utf8(const std::wstring& w) {
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
            } else if (arg == L"--optimization-level" || arg == L"-OL") {
                std::string* v = require_value(L"--optimization-level");
                if (!v) return false;
                if (v->size() != 1 || (*v)[0] < '0' || (*v)[0] > '4') {
                    out.mode = CommandMode::Invalid;
                    out.error_message = "invalid optimization level: " + *v +
                        " (expected 0-4)";
                    return false;
                }
                out.optimization_level = static_cast<int>((*v)[0] - '0');
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
        return
            "Standard Gallt Compiler (sgc)\n"
            "Usage:\n"
            "  sgc --compile --input \"file.glt\" --output \"program.exe\"\n"
            "  sgc --compile --input \"file.glt\" --output \"program.exe\" "
            "--optimization-level <0-4>\n"
            "  sgc --compile --input \"file.glt\" --output \"program.exe\" -OL <0-4>\n"
            "  sgc --help\n"
            "  sgc --version\n"
            "Options:\n"
            "  --compile                     compile a Gallt source file\n"
            "  --input,    -i <path>         input .glt file\n"
            "  --output,   -o <path>         output executable\n"
            "  --optimization-level, -OL <n> optimization level:\n"
            "                                0 no optimization\n"
            "                                1 basic\n"
            "                                2 medium (default)\n"
            "                                3 aggressive\n"
            "                                4 size-oriented\n";
    }

    std::string version_text() {
        return
            "sgc Standard Gallt Compiler 0.4.1 Preview (LLVM backend, x86-64 Windows)\n"
            "Gallt Lang Standard Version 26.09 (Preview)\n"
            "Build date: 2026-09-16\n"
            "The compiler is an early preview version, and support for certain syntax and edge cases may not be fully covered. We appreciate your understanding.";
    }

} 
