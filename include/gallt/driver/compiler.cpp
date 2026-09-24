
#include "compiler.hpp"
#include "../common/diagnostics.hpp"
#include "../common/token.hpp"
#include "../codegen/codegen.hpp"
#include "../lexer/lexer.hpp"
#include "../pal/platform.hpp"
#include "../parser/ast.hpp"
#include "../parser/parser.hpp"
#include "../semantic/generic_expander.hpp"
#include "../semantic/condition_compiler.hpp"
#include "../semantic/lifecycle.hpp"
#include "../semantic/namespace_lowering.hpp"
#include "../semantic/type_checker.hpp"
#include <filesystem>
#include <functional>
#include <fstream>
#include <iostream>
#include <cstdint>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace fs = std::filesystem;

namespace gallt {
namespace {

    std::optional<std::string> read_entire_file(const fs::path& path) {
        std::string content;
        if (!pal::read_file(pal::to_utf8(path.wstring()), content)) {
            return std::nullopt;
        }
        return content;
    }

    bool write_utf8_file(const fs::path& path, const std::string& content) {
        return pal::write_file(pal::to_utf8(path.wstring()), content);
    }

    std::string find_llvm_executable(const std::string& tool_name) {
        for (const char* env : { "SGC_LLVM_BIN", "LLVM_BIN" }) {
            std::string dir = pal::environment_variable(env);
            if (dir.empty()) { continue; }
            std::string candidate = pal::join_path(dir, tool_name);
            if (pal::file_exists(candidate)) { return candidate; }
        }

        std::string module_path = pal::executable_path();

        if (!module_path.empty()) {
            std::string candidate = pal::join_path(pal::parent_path(module_path),
                tool_name);
            if (pal::file_exists(candidate)) { return candidate; }
        }

        const char* candidates[] = {
            "C:/LLVM/build/Release/bin",
            "D:/LLVM/build/Release/bin",
            "E:/LLVM/build/Release/bin",
        };

        for (const char* dir : candidates) {
            std::string candidate = pal::join_path(dir, tool_name);
            if (pal::file_exists(candidate)) { return candidate; }
        }

        return tool_name;
    }

    struct LoadedSource {
        std::shared_ptr<std::string> source;
        std::shared_ptr<std::string> name;
        std::unique_ptr<AST::Program> program;
        fs::path canonical_path;
    };

    bool load_one_file(const fs::path& path, DiagnosticEngine& diag, LoadedSource& out,
        std::unordered_set<std::string>* generic_names) {
        auto bytes = read_entire_file(path);
        if (!bytes.has_value()) {
            diag.report_error(SourceLocation{}, ErrorCode::LibraryNotFound,
                "cannot open source file");
            return false;
        }

        auto source = std::make_shared<std::string>(std::move(*bytes));
        auto name = std::make_shared<std::string>(pal::to_utf8(path.wstring()));
        std::string_view source_view(*source);
        std::string_view name_view(*name);
        Lexer lexer(source_view, name_view, diag);
        Parser parser(lexer, diag);

        if (generic_names != nullptr) {
            parser.seed_generic_names(*generic_names);
        }
        out.source = source;
        out.name = name;
        out.program = parser.parse();

        if (generic_names != nullptr && out.program != nullptr) {
            const std::unordered_set<std::string>& names = parser.generic_names();
            generic_names->insert(names.begin(), names.end());
        }

        return out.program != nullptr;
    }

    std::string normalize_library_reference(const std::string& library) {
        return pal::normalize_path(library);
    }

    std::string runtime_object_cache_key(const std::string& runtime_source,
        const std::string& optimization_flag, int debug_symbols_level) {
        std::uint64_t hash = 1469598103934665603ull;
        const std::string parts[] = {
            runtime_source,
            optimization_flag,
            std::to_string(debug_symbols_level),
            pal::target_triple(),
        };

        for (const std::string& part : parts) {
            for (unsigned char byte : part) {
                hash ^= byte;
                hash *= 1099511628211ull;
            }

            hash ^= 0xFF;
            hash *= 1099511628211ull;
        }

        static const char digits[] = "0123456789abcdef";
        std::string text;
        text.reserve(16);

        for (int shift = 60; shift >= 0; shift -= 4) {
            text.push_back(digits[(hash >> shift) & 0xFull]);
        }

        return text;
    }

    void append_runtime_debug_flags(std::vector<std::string>& args,
        int debug_symbols_level) {
        if (debug_symbols_level == 1) {
            args.push_back("-gline-tables-only");
        } else if (debug_symbols_level >= 2) {
            args.push_back("-gcodeview");
            args.push_back("-g");
        }
    }

    std::string acquire_runtime_object(const std::string& clang_path,
        const std::string& temp_dir, const std::string& runtime_c_path,
        const std::string& runtime_source, const std::string& optimization_flag,
        int debug_symbols_level, const std::string& unique) {
        const std::string cache_dir = pal::join_path(temp_dir,
            "sgc_runtime_cache");

        if (!pal::directory_exists(cache_dir) &&
            !pal::create_directory(cache_dir)) {
            return std::string();
        }

        const std::string cached = pal::join_path(cache_dir,
            "crt_" + runtime_object_cache_key(runtime_source, optimization_flag,
                debug_symbols_level) + ".obj");

        if (pal::file_exists(cached) && pal::file_size(cached) > 0) {
            return cached;
        }

        const std::string staging = pal::join_path(temp_dir,
            unique + "_runtime.obj");
        std::vector<std::string> args = {
            std::string("--target=") + pal::target_triple(),
            optimization_flag,
            "-Wno-override-module",
            "-Wno-deprecated-declarations",
            "-c",
            "-x", "c",
            runtime_c_path,
            "-o", staging,
        };
        append_runtime_debug_flags(args, debug_symbols_level);
        std::string output;

        if (pal::run_process(clang_path, args, &output) != 0) {
            pal::remove_file(staging);
            return std::string();
        }

        if (!pal::rename_file(staging, cached)) {
            pal::remove_file(staging);
            return pal::file_exists(cached) ? cached : std::string();
        }

        return cached;
    }

    std::vector<std::pair<std::string, SourceLocation>> scan_guide_references(
        const fs::path& path, std::vector<std::shared_ptr<std::string>>& name_pool) {
        std::vector<std::pair<std::string, SourceLocation>> references;
        auto bytes = read_entire_file(path);
        if (!bytes.has_value()) { return references; }
        name_pool.push_back(std::make_shared<std::string>(
            pal::to_utf8(path.wstring())));
        DiagnosticEngine scratch;
        Lexer lexer(*bytes, *name_pool.back(), scratch);

        for (;;) {
            Token token = lexer.next_token();
            if (token.type == TokenType::EndOfFile) { break; }
            if (token.type != TokenType::Keyword_Guide) { continue; }
            Token literal = lexer.next_token();
            if (literal.type != TokenType::StringLiteral) { continue; }
            std::string text(literal.lexeme);

            if (text.size() >= 2 && text.front() == '"' && text.back() == '"') {
                text = text.substr(1, text.size() - 2);
            }

            references.emplace_back(std::move(text), token.location);
        }

        return references;
    }

    const std::vector<fs::path>& standard_library_dirs() {
        static const std::vector<fs::path> dirs = [] {
            std::vector<fs::path> out;

            for (const char* env : { "SGC_STDLIB", "SGC_SL" }) {
                std::string value = pal::environment_variable(env);
                if (!value.empty()) {
                    out.emplace_back(pal::to_wide(value));
                }
            }

            const std::string module_path = pal::executable_path();

            if (!module_path.empty()) {
                fs::path exe_dir(pal::to_wide(pal::parent_path(module_path)));
                out.push_back(exe_dir.parent_path() / L"sl");
                out.push_back(exe_dir / L"sl");
                out.push_back(exe_dir.parent_path() / L"SGC" / L"sl");
            }

            out.push_back(fs::path(L".") / L"sl");
            return out;
        }();

        return dirs;
    }

    std::optional<fs::path> resolve_guide_path(const std::string& guide,
        const fs::path& current_dir) {
        std::string normalized = normalize_library_reference(guide);
        fs::path referenced(pal::to_wide(normalized));

        if (referenced.is_absolute()) {
            if (pal::file_exists(normalized)) {
                return fs::path(pal::to_wide(pal::canonical_path(normalized)));
            }
            return std::nullopt;
        }

        fs::path local = current_dir / referenced;
        if (pal::file_exists(pal::to_utf8(local.wstring()))) {
            return fs::path(pal::to_wide(
                pal::canonical_path(pal::to_utf8(local.wstring()))));
        }

        for (const fs::path& dir : standard_library_dirs()) {
            fs::path candidate = dir / referenced;
            if (pal::file_exists(pal::to_utf8(candidate.wstring()))) {
                return fs::path(pal::to_wide(pal::canonical_path(
                    pal::to_utf8(candidate.wstring()))));
            }

            if (!referenced.has_parent_path()) {
                fs::path by_name = dir / referenced.filename();
                if (pal::file_exists(pal::to_utf8(by_name.wstring()))) {
                    return fs::path(pal::to_wide(pal::canonical_path(
                        pal::to_utf8(by_name.wstring()))));
                }
            }
        }

        return std::nullopt;
    }

}

    int run_compiler(const CommandOptions& options) {
        if (!options.positional.empty()) {
            std::cerr << "sgc: unexpected positional argument '" <<
                options.positional.front() <<
                "'; use --input <file.glt> and --output <file.exe>\n";
            return 2;
        }

        if (options.input.empty()) {
            std::cerr << "sgc: no input file; use --input <file.glt>\n";
            return pal::exit_failure_code();
        }

        std::string output_path = options.output.empty()
            ? pal::replace_extension(options.input, "exe")
            : options.output;

        if (pal::extension(output_path).empty()) {
            output_path += ".exe";
        }

        const OutputKind output_kind = classify_output_path(output_path);
        const bool emit_entry_point = output_kind == OutputKind::Executable;
        fs::path main_path(pal::to_wide(options.input));
        fs::path canonical_main(pal::to_wide(pal::canonical_path(options.input)));

        DiagnosticEngine diag;
        std::vector<LoadedSource> sources;
        std::vector<fs::path> visited;
        std::unordered_set<std::string> merged_generic_names;
        std::vector<std::shared_ptr<std::string>> guide_name_pool;
        std::size_t main_index = 0;
        bool main_registered = false;

        std::function<bool(const fs::path&)> load_with_guides =
            [&](const fs::path& path) -> bool {
            fs::path canonical(pal::to_wide(
                pal::canonical_path(pal::to_utf8(path.wstring()))));
            if (std::find(visited.begin(), visited.end(), canonical) != visited.end()) {
                return true;
            }

            visited.push_back(canonical);

            for (const auto& reference : scan_guide_references(canonical,
                guide_name_pool)) {
                std::string gpath = normalize_library_reference(reference.first);
                std::optional<fs::path> resolved =
                    resolve_guide_path(gpath, canonical.parent_path());
                if (!resolved.has_value()) {
                    diag.report_error(reference.second, ErrorCode::LibraryNotFound,
                        "guide file not found: " + gpath);
                    return false;
                }
                if (!load_with_guides(*resolved)) {
                    return false;
                }
            }

            LoadedSource ls;
            ls.canonical_path = canonical;

            if (!load_one_file(canonical, diag, ls, &merged_generic_names)) {
                return false;
            }

            sources.push_back(std::move(ls));

            if (!main_registered && canonical == canonical_main) {
                main_index = sources.size() - 1;
                main_registered = true;
            }
            return true;
        };

        if (!load_with_guides(canonical_main)) {
            diag.print_all(std::cerr);
            return pal::exit_failure_code();
        }

        if (diag.has_errors()) {
            diag.print_all(std::cerr);
            return pal::exit_failure_code();
        }

        std::vector<std::unique_ptr<AST::TopLevel>> all_nodes;
        std::unordered_map<fs::path, LoadedSource*> by_path;

        for (LoadedSource& ls : sources) {
            by_path[ls.canonical_path] = &ls;
        }

        std::unordered_set<fs::path> expanded;
        std::function<void(LoadedSource*)> append_in_guide_order =
            [&](LoadedSource* current) {
            if (current == nullptr) { return; }
            if (!expanded.insert(current->canonical_path).second) { return; }

            for (auto& node : current->program->top_levels) {
                if (auto* guide = dynamic_cast<AST::GuideStatement*>(node.get())) {
                    std::string gpath = normalize_library_reference(guide->path);
                    std::optional<fs::path> resolved =
                        resolve_guide_path(gpath, current->canonical_path.parent_path());

                    if (resolved.has_value()) {
                        auto found = by_path.find(*resolved);

                        if (found != by_path.end()) {
                            append_in_guide_order(found->second);
                        }
                    }
                } else {
                    all_nodes.push_back(std::move(node));
                }
            }
        };

        if (!sources.empty()) {
            append_in_guide_order(&sources[main_index]);
        }

        SourceLocation fake_start;
        AST::Program combined(fake_start, std::move(all_nodes));

        ConditionCompiler conditions(diag);

        if (!conditions.run(&combined) || diag.has_errors()) {
            diag.print_all(std::cerr);
            return pal::exit_failure_code();
        }

        NamespaceLowering namespaces(diag);

        if (!namespaces.run(&combined) || diag.has_errors()) {
            diag.print_all(std::cerr);
            return pal::exit_failure_code();
        }

        GenericExpander expander(diag);

        if (!expander.expand(&combined) || diag.has_errors()) {
            diag.print_all(std::cerr);
            return pal::exit_failure_code();
        }

        LifecycleLowering lifecycle(diag);

        if (!lifecycle.run(&combined) || diag.has_errors()) {
            diag.print_all(std::cerr);
            return pal::exit_failure_code();
        }

        TypeChecker checker(diag, expander.expression_free_identifiers(),
            expander.expression_argument_casts(), emit_entry_point);

        {
            std::unordered_map<const AST::Expression*, AST::Type> call_sites;

            for (const auto& entry : expander.expression_call_sites()) {
                call_sites[entry.first] = entry.second.declared_return_type;
            }

            checker.set_expression_call_sites(call_sites);
        }

        if (!checker.check_program(&combined)) {
            diag.print_all(std::cerr);
            return pal::exit_failure_code();
        }

        CodeGenerator generator(&combined, checker.expression_types(),
            checker.resolved_functions(), checker.resolved_externs(),
            checker.resolved_operators(), &diag, options.debug_symbols_level,
            emit_entry_point);
        generator.generate();

        if (diag.has_errors()) {
            diag.print_all(std::cerr);
            return pal::exit_failure_code();
        }

        const std::string temp_dir = pal::temporary_directory();
        const std::string unique = "sgc_" + pal::process_id_text();
        fs::path ir_path(pal::to_wide(
            pal::join_path(temp_dir, unique + ".ll")));
        fs::path c_path(pal::to_wide(
            pal::join_path(temp_dir, unique + ".c")));
        const std::string runtime_source = CodeGenerator::runtime_c_source();
        const std::string c_path_text = pal::to_utf8(c_path.wstring());

        if (!write_utf8_file(ir_path, generator.ir()) ||
            !write_utf8_file(c_path, runtime_source)) {
            std::cerr << "sgc: cannot create temporary backend files ("
                << pal::file_error_text(pal::last_file_error()) << ")\n";
            return pal::exit_failure_code();
        }

        const std::string clang_path = find_llvm_executable("clang.exe");

        std::string optimization_flag;

        switch (options.optimization_level) {
        case 0: optimization_flag = "-O0"; break;
        case 1: optimization_flag = "-O1"; break;
        case 3: optimization_flag = "-O3"; break;
        case 4: optimization_flag = "-Os"; break;
        case 2:
            default: optimization_flag = "-O2"; break;
        }

        std::string runtime_object;

        if (!pal::environment_variable_defined("SGC_KEEP_TEMP")) {
            runtime_object = acquire_runtime_object(clang_path, temp_dir,
                c_path_text, runtime_source, optimization_flag,
                options.debug_symbols_level, unique);
        }

        std::vector<std::string> runtime_arguments;

        if (!runtime_object.empty()) {
            runtime_arguments = { "-x", "none", runtime_object };
        } else {
            runtime_arguments = { "-x", "c", c_path_text };
        }

        std::vector<std::string> tool_args = {
            std::string("--target=") + pal::target_triple(),
            "-fuse-ld=lld",
            optimization_flag,
            "-Wno-override-module",
            "-Wno-deprecated-declarations",
            "-x", "ir",
            pal::to_utf8(ir_path.wstring()),
        };
        tool_args.insert(tool_args.end(), runtime_arguments.begin(),
            runtime_arguments.end());
        tool_args.push_back("-o");
        tool_args.push_back(output_path);

        if (output_kind == OutputKind::DynamicLibrary) {
            tool_args.insert(tool_args.begin() + 2, "-shared");

            for (const std::string& exported : generator.exported_functions()) {
                tool_args.push_back("-Wl,/EXPORT:" + exported);
            }
        }

        if (options.debug_symbols_level == 1) {
            tool_args.push_back("-gline-tables-only");
        } else if (options.debug_symbols_level >= 2) {
            tool_args.push_back("-gcodeview");
            tool_args.push_back("-g");
            tool_args.push_back("-Wl,/DEBUG");
        }

        bool reset_language = false;
        bool link_library_missing = false;
        std::vector<std::string> resolved_libraries;

        for (const std::string& lib : generator.link_libraries()) {
            std::string ref = normalize_library_reference(lib);
            std::string lib_path = ref;
            std::vector<std::string> bases;

            if (pal::is_absolute_path(lib_path)) {
                bases.push_back(std::string());
            } else {
                bases.push_back(pal::to_utf8(canonical_main.parent_path().wstring()));
                bases.push_back(pal::current_working_directory());

                for (const fs::path& dir : standard_library_dirs()) {
                    bases.push_back(pal::to_utf8(dir.wstring()));
                }

                const std::string module_path = pal::executable_path();

                if (!module_path.empty()) {
                    bases.push_back(pal::parent_path(module_path));
                }
            }

            std::vector<std::string> probes;

            if (pal::extension(lib_path).empty()) {
                probes.push_back(lib_path);
                probes.push_back(lib_path + pal::static_library_extension());
            } else {
                probes.push_back(lib_path);
            }

            bool resolved = false;

            for (const std::string& base : bases) {
                for (const std::string& probe : probes) {
                    std::string candidate = base.empty()
                        ? probe
                        : pal::join_path(base, probe);
                    if (pal::file_exists(candidate)) {
                        resolved_libraries.push_back(candidate);
                        resolved = true;
                        break;
                    }
                }

                if (resolved) { break; }
            }

            if (!resolved) {
                link_library_missing = true;
                diag.report_error(SourceLocation{}, ErrorCode::LibraryNotFound,
                    "clib library not found: " + lib);
            }
        }

        if (link_library_missing) {
            diag.print_all(std::cerr);
            return pal::exit_failure_code();
        }

        std::string tool_output;
        int link_result = 0;
        std::vector<fs::path> object_paths;

        if (output_kind == OutputKind::StaticLibrary) {
            const std::string librarian = find_llvm_executable("llvm-lib.exe");
            if (!pal::file_exists(librarian)) {
                std::cerr << "sgc: LLVM librarian not found: " << librarian
                    << "\n";
                return pal::exit_failure_code();
            }
            object_paths.push_back(fs::path(pal::to_wide(
                pal::join_path(temp_dir, unique + "_gallt.obj"))));
            object_paths.push_back(fs::path(pal::to_wide(
                pal::join_path(temp_dir, unique + "_runtime.obj"))));
            const std::string gallt_object = pal::to_utf8(
                object_paths[0].wstring());
            const std::string runtime_object_file = pal::to_utf8(
                object_paths[1].wstring());

            std::vector<std::string> compile_common = {
                std::string("--target=") + pal::target_triple(),
                optimization_flag,
                "-Wno-override-module",
                "-Wno-deprecated-declarations",
                "-c",
            };
            std::vector<std::string> ir_compile = compile_common;
            ir_compile.push_back("-x");
            ir_compile.push_back("ir");
            ir_compile.push_back(pal::to_utf8(ir_path.wstring()));
            ir_compile.push_back("-o");
            ir_compile.push_back(gallt_object);
            if (options.debug_symbols_level == 1) {
                ir_compile.push_back("-gline-tables-only");
            } else if (options.debug_symbols_level >= 2) {
                ir_compile.push_back("-gcodeview");
                ir_compile.push_back("-g");
            }
            link_result = pal::run_process(clang_path, ir_compile, &tool_output);

            if (link_result != 0) {
                if (!tool_output.empty()) { std::cerr << tool_output; }
                std::cerr << "sgc: LLVM backend failed with exit code "
                    << link_result << '\n';
                return link_result;
            }

            std::string runtime_archive_member = runtime_object;

            if (runtime_archive_member.empty()) {
                std::vector<std::string> c_compile = compile_common;
                c_compile.push_back("-x");
                c_compile.push_back("c");
                c_compile.push_back(c_path_text);
                c_compile.push_back("-o");
                c_compile.push_back(runtime_object_file);
                append_runtime_debug_flags(c_compile,
                    options.debug_symbols_level);
                link_result = pal::run_process(clang_path, c_compile,
                    &tool_output);

                if (link_result != 0) {
                    if (!tool_output.empty()) { std::cerr << tool_output; }
                    std::cerr << "sgc: LLVM backend failed with exit code "
                        << link_result << '\n';
                    return link_result;
                }
                runtime_archive_member = runtime_object_file;
            }

            std::vector<std::string> archive_args = {
                "/nologo",
                "/out:" + output_path,
                gallt_object,
                runtime_archive_member,
            };
            link_result = pal::run_process(librarian, archive_args, &tool_output);
        } else {
            if (!resolved_libraries.empty()) {
                tool_args.push_back("-x");
                tool_args.push_back("none");

                for (const std::string& library : resolved_libraries) {
                    tool_args.push_back(library);
                }
            }

            link_result = pal::run_process(clang_path, tool_args, &tool_output);
        }

        if (!tool_output.empty()) {
            std::cerr << tool_output;
        }
        if (link_result != 0) {
            std::cerr << "sgc: LLVM backend failed with exit code "
                << link_result << '\n';
        } else {
            std::cout << "Compilation successful\n";
        }

        if (!pal::environment_variable_defined("SGC_KEEP_TEMP")) {
            pal::remove_file(pal::to_utf8(ir_path.wstring()));
            pal::remove_file(pal::to_utf8(c_path.wstring()));

            for (const fs::path& object : object_paths) {
                pal::remove_file(pal::to_utf8(object.wstring()));
            }
        } else {
            std::cerr << "sgc: backend files retained: "
                << pal::to_utf8(ir_path.wstring()) << "\n";

            for (const fs::path& object : object_paths) {
                std::cerr << "sgc: backend files retained: "
                    << pal::to_utf8(object.wstring()) << "\n";
            }
        }
        return link_result;
    }

}
