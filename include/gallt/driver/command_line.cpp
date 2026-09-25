#include "command_line.hpp"
#include "../pal/platform.hpp"

namespace gallt {
namespace {
    std::string narrow_utf8(const std::wstring& w) {
        return pal::to_utf8(w);
    }
}

    OutputKind classify_output_path(const std::string& path) {
        std::string extension = pal::extension(path);

        for (char& c : extension) {
            if (c >= 'A' && c <= 'Z') { c = static_cast<char>(c - 'A' + 'a'); }
        }

        if (extension == ".lib") { return OutputKind::StaticLibrary; }
        if (extension == ".dll") { return OutputKind::DynamicLibrary; }
        return OutputKind::Executable;
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

            if (arg == L"--debug") {
                out.debug_mode = true;
                out.debug_mode_explicit = true;
                continue;
            }

            if (arg == L"--release") {
                out.release_mode = true;
                out.release_mode_explicit = true;
                continue;
            }

            auto require_value = [&](const wchar_t* name) -> std::string* {
                if (i + 1 >= argc) {
                    out.error_message = "missing value after " + narrow_utf8(arg);
                    out.mode = CommandMode::Invalid;
                    return nullptr;
                }

                ++i;
                static std::string value;
                value = narrow_utf8(argv[i]);
                return &value;
            };

            if (arg == L"--input" || arg == L"-i") {
                std::string* v = require_value(L"--input");
                if (!v) { return false; }
                out.input = *v;
            } else if (arg == L"--output" || arg == L"-o") {
                std::string* v = require_value(L"--output");
                if (!v) { return false; }
                out.output = *v;
            } else if (arg == L"--optimization-level" || arg == L"-OL") {
                std::string* v = require_value(L"--optimization-level");
                if (!v) { return false; }

                if (v->size() != 1 || (*v)[0] < '0' || (*v)[0] > '4') {
                    out.mode = CommandMode::Invalid;
                    out.error_message = "invalid optimization level: " + *v +
                        " (expected 0-4)";
                    return false;
                }

                out.optimization_level = static_cast<int>((*v)[0] - '0');
                out.optimization_level_explicit = true;
            } else if (arg == L"--debug-symbols" || arg == L"-DS") {
                std::string* v = require_value(L"--debug-symbols");
                if (!v) { return false; }

                if (v->size() != 1 || (*v)[0] < '0' || (*v)[0] > '2') {
                    out.mode = CommandMode::Invalid;
                    out.error_message = "invalid debug information level: " + *v +
                        " (expected 0-2)";
                    return false;
                }

                out.debug_symbols_level = static_cast<int>((*v)[0] - '0');
                out.debug_symbols_explicit = true;
            } else if (arg == L"--no-runtime" || arg == L"-NR") {
                out.no_runtime = true;
            } else if (arg == L"--gallt-abi" || arg == L"-GA") {
                out.gallt_abi = true;
            } else if (!arg.empty() && arg[0] == L'-') {
                out.mode = CommandMode::Invalid;
                out.error_message = "unknown option: " + narrow_utf8(arg);
                return false;
            } else {
                out.positional.push_back(narrow_utf8(arg));
            }
        }

        if (out.mode == CommandMode::Invalid) {
            return false;
        }

        if (out.debug_mode_explicit && out.release_mode_explicit) {
            out.mode = CommandMode::Invalid;
            out.error_message = "--debug and --release are mutually exclusive";
            return false;
        }

        if (out.optimization_level_explicit && out.debug_symbols_explicit) {
            out.mode = CommandMode::Invalid;
            out.error_message =
                "--optimization-level and --debug-symbols are mutually exclusive";
            return false;
        }

        if (out.debug_mode && out.optimization_level_explicit) {
            out.mode = CommandMode::Invalid;
            out.error_message =
                "--debug mode does not allow --optimization-level";
            return false;
        }

        if (out.release_mode && out.debug_symbols_explicit) {
            out.mode = CommandMode::Invalid;
            out.error_message =
                "--release mode does not allow --debug-symbols";
            return false;
        }

        if (out.debug_mode) {
            if (!out.optimization_level_explicit) { out.optimization_level = 0; }
            if (!out.debug_symbols_explicit) { out.debug_symbols_level = 2; }
        } else if (out.release_mode) {
            if (!out.optimization_level_explicit) { out.optimization_level = 3; }
            if (!out.debug_symbols_explicit) { out.debug_symbols_level = 0; }
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
            "  sgc --compile --input \"file.glt\" --output \"program.exe\" "
            "--debug-symbols <0-2>\n"
            "  sgc --compile --input \"file.glt\" --output \"program.exe\" -DS <0-2>\n"
            "  sgc --compile --input \"file.glt\" --output \"program.exe\" --debug\n"
            "  sgc --compile --input \"file.glt\" --output \"program.exe\" --release\n"
            "  sgc --compile --input \"file.glt\" --output \"library.lib\" --no-runtime\n"
            "  sgc --compile --input \"file.glt\" --output \"library.lib\" -NR\n"
            "  sgc --compile --input \"file.glt\" --output \"program.exe\" --gallt-abi\n"
            "  sgc --compile --input \"file.glt\" --output \"program.exe\" -GA\n"
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
            "                                4 size-oriented\n"
            "  --debug-symbols, -DS <n>      debug information level:\n"
            "                                0 no debug information (default)\n"
            "                                1 line number tables only\n"
            "                                2 type information, symbol information "
            "and line number tables\n"
            "  --debug                       enable debug mode\n"
            "  --release                     enable release mode\n"
            "  --no-runtime, -NR             emit without the C runtime (importable\n"
            "                                library output; only exported functions\n"
            "                                stay externally visible)\n"
            "  --gallt-abi, -GA              call extern declarations with the native\n"
            "                                Gallt ABI (use when importing a library\n"
            "                                produced by sgc; C libraries need the\n"
            "                                default C ABI)\n"
            "Conflicts:\n"
            "  --optimization-level and --debug-symbols cannot be combined\n"
            "  --debug and --release cannot be combined\n"
            "  --debug does not allow --optimization-level\n"
            "  --release does not allow --debug-symbols\n";
    }

    std::string version_text() {
        return
        "sgc Standard Gallt Compiler 0.4.2-0925 Preview (LLVM backend, x86-64 Windows)\n"
        "Gallt Lang Standard Version 26.09 (Preview)\n"
        "Build date: 2026-09-25\n"
            "The compiler is an early preview version, and support for certain syntax and edge cases may not be fully covered. We appreciate your understanding";
    }

}
