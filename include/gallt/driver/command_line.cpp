#include <iostream>
#include "command_line.hpp"
#include "../pal/platform.hpp"

namespace gallt {
namespace {
    std::string narrow_utf8(const std::wstring& w) {
        return pal::to_utf8(w);
    }

    std::optional<std::uint64_t> parse_positive_size(const std::string& text) {
        if (text.empty()) { return std::nullopt; }

        std::uint64_t value = 0;

        for (char c : text) {
            if (c < '0' || c > '9') { return std::nullopt; }

            const std::uint64_t digit = static_cast<std::uint64_t>(c - '0');

            if (value > (UINT64_MAX - digit) / 10) { return std::nullopt; }

            value = value * 10 + digit;
        }

        if (value == 0) { return std::nullopt; }
        return value;
    }
}

    OutputKind classify_output_path(const std::string& path) {
        std::string extension = pal::extension(path);

        for (char& c : extension) {
            if (c >= 'A' && c <= 'Z') { c = static_cast<char>(c - 'A' + 'a'); }
        }

        if (extension == pal::static_library_extension()) {
            return OutputKind::StaticLibrary;
        }

        if (extension == pal::preferred_library_extension()) {
            return OutputKind::DynamicLibrary;
        }

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
            } else if (arg == L"--extended-semantics") {
                std::string* v = require_value(L"--extended-semantics");
                if (!v) { return false; }

                if (*v == "true") {
                    out.extended_semantics = true;
                } else if (*v == "false") {
                    out.extended_semantics = false;
                } else {
                    out.mode = CommandMode::Invalid;
                    out.error_message = "invalid extended semantics value: " + *v +
                        " (expected true or false)";
                    return false;
                }
            } else if (arg == L"--stack") {
                std::string* v = require_value(L"--stack");
                if (!v) { return false; }

                std::optional<std::uint64_t> parsed = parse_positive_size(*v);

                if (parsed.has_value()) {
                    out.stack_size = parsed;
                } else {
                    std::cerr << "--stack: invalid value, will revert to the system default value\n";
                }
            } else if (arg == L"--commit") {
                std::string* v = require_value(L"--commit");
                if (!v) { return false; }

                std::optional<std::uint64_t> parsed = parse_positive_size(*v);

                if (parsed.has_value()) {
                    out.commit_size = parsed;
                } else {
                    std::cerr << "--commit: invalid value, will revert to the system default value\n";
                }
            } else if (arg == L"--instantiation-depth") {
                std::string* v = require_value(L"--instantiation-depth");
                if (!v) { return false; }

                std::optional<std::uint64_t> parsed = parse_positive_size(*v);

                if (parsed.has_value()) {
                    out.instantiation_depth = parsed;
                } else {
                    std::cerr << "--instantiation-depth: invalid value, will revert to the default value\n";
                }
            } else if (arg == L"--linker") {
                std::string* v = require_value(L"--linker");
                if (!v) { return false; }

                if (*v == "dynamic") {
                    out.linker_mode = LinkerMode::Dynamic;
                } else if (*v == "static") {
                    out.linker_mode = LinkerMode::Static;
                } else {
                    out.mode = CommandMode::Invalid;
                    out.error_message = "invalid linker mode: " + *v +
                        " (expected dynamic or static)";
                    return false;
                }
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
            "  sgc --compile --input \"file.glt\" --output \"program.exe\" "
            "--stack <bytes> --commit <bytes>\n"
            "  sgc --compile --input \"file.glt\" --output \"program.exe\" "
            "--linker dynamic\n"
            "  sgc --compile --input \"file.glt\" --output \"program.exe\" "
            "--linker static\n"
            "  sgc --compile --input \"file.glt\" --output \"program.exe\" "
            "--instantiation-depth <n>\n"
            "  sgc --compile --input \"file.glt\" --output \"program.exe\" "
            "--extended-semantics true\n"
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
            "  --no-runtime, -NR             emit without the Gallt runtime (importable\n"
            "                                library output; only exported functions\n"
            "                                stay externally visible)\n"
            "  --gallt-abi, -GA              call extern declarations with the native\n"
            "                                Gallt ABI (use when importing a library\n"
            "                                produced by sgc; C libraries need the\n"
            "                                default C ABI)\n"
            "  --stack <bytes>               main-thread stack reserve size in bytes\n"
            "                                (non-zero positive integer; zero or\n"
            "                                negative values are silently ignored)\n"
            "  --commit <bytes>              main-thread stack commit size in bytes\n"
            "                                (non-zero positive integer; zero or\n"
            "                                negative values are silently ignored)\n"
            "  --linker <dynamic|static>     system C library linking mode:\n"
            "                                dynamic (default) or static\n"
            "  --instantiation-depth <n>     generic instantiation recursion depth limit\n"
            "                                (non-zero positive integer; default 2048;\n"
            "                                values above 2048 are allowed but\n"
            "                                compile time is not guaranteed and\n"
            "                                compilation may terminate)\n"
            "  --extended-semantics <bool>   enable extended semantics: some constructs\n"
            "                                forbidden by the Gallt standard document\n"
            "                                are relaxed (true or false; default false)\n"
            "Conflicts:\n"
            "  --optimization-level and --debug-symbols cannot be combined\n"
            "  --debug and --release cannot be combined\n"
            "  --debug does not allow --optimization-level\n"
            "  --release does not allow --debug-symbols\n"
            "Notes:\n"
            "  --stack, --commit and --linker only affect executable output;\n"
            "  they are silently ignored for library output\n"
            "  --linker does not affect the Gallt runtime, which is controlled\n"
            "  by --no-runtime\n";
    }

    std::string version_text() {
        return
        "Standard Gallt Compiler (sgc) 0.5.0-1010 Preview (LLVM backend, x86-64 Windows and Linux)\n"
        "Build date: 2026-10-10\n"
        "This version is experimental";
    }

}
