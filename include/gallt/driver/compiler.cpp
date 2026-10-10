
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
#include <algorithm>
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

    bool contains_error_type(
        const std::unordered_map<const AST::Expression*, AST::Type>& types) {
        for (const auto& entry : types) {
            if (entry.second.is_error()) { return true; }
        }

        return false;
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

        std::vector<LineSplice> splices;
        auto spliced = std::make_shared<std::string>(
            splice_line_continuations(*source, splices));

        std::string_view source_view(*spliced);
        std::string_view name_view(*name);
        Lexer lexer(source_view, name_view, diag,
            splices.empty() ? nullptr : &splices);
        Parser parser(lexer, diag);

        if (generic_names != nullptr) {
            parser.seed_generic_names(*generic_names);
        }
        out.source = spliced;
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
            unique + "_runtime" + pal::object_file_extension());
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
        const std::vector<std::string> debug_arguments =
            pal::compile_debug_arguments(debug_symbols_level);
        args.insert(args.end(), debug_arguments.begin(), debug_arguments.end());
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
        std::vector<LineSplice> splices;
        std::string spliced = splice_line_continuations(*bytes, splices);
        Lexer lexer(spliced, *name_pool.back(), scratch,
            splices.empty() ? nullptr : &splices);

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
            ? pal::replace_extension(options.input,
                pal::default_executable_extension())
            : options.output;

        if (pal::extension(output_path).empty() &&
            pal::default_executable_extension()[0] != '\0') {
            output_path += pal::default_executable_extension();
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
            diag.report_note(SourceLocation{},
                "the lexical and syntactic phases completed with " +
                std::to_string(diag.error_count()) +
                " error(s); all of their diagnostics were listed above and the "
                "remaining phases (semantic passes, type checking and code "
                "generation) were skipped because the parser could not provide a "
                "healthy AST");
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
        NamespaceLowering namespaces(diag);
        GenericExpander expander(diag, namespaces, options.instantiation_depth);
        LifecycleLowering lifecycle(diag);

        conditions.run(&combined);
        namespaces.run(&combined);
        expander.expand(&combined);

        if (expander.stack_probe_triggered()) {
            return pal::exit_failure_code();
        }

        lifecycle.run(&combined);

        TypeChecker checker(diag, expander.expression_free_identifiers(),
            expander.expression_argument_casts(), emit_entry_point,
            options.extended_semantics);

        {
            std::unordered_map<const AST::Expression*, AST::Type> call_sites;

            for (const auto& entry : expander.expression_call_sites()) {
                call_sites[entry.first] = entry.second.declared_return_type;
            }

            checker.set_expression_call_sites(call_sites);
        }

        checker.check_program(&combined);

        checker.apply_constexpr_folding(combined);

        if (diag.has_errors()) {
            diag.report_note(SourceLocation{},
                "the semantic passes and type checking completed with " +
                std::to_string(diag.error_count()) +
                " error(s); all of their diagnostics were listed above and "
                "downstream code generation was skipped because the semantic "
                "layer could not provide a healthy AST");
            diag.print_all(std::cerr);
            return pal::exit_failure_code();
        }

        if (contains_error_type(checker.expression_types())) {
            diag.report_note(SourceLocation{},
                "the type checker left an unresolved error type without a matching "
                "diagnostic; downstream code generation was skipped because the "
                "semantic layer could not provide a healthy AST");
            diag.print_all(std::cerr);
            return pal::exit_failure_code();
        }

        CodeGenerator generator(&combined, checker.expression_types(),
            checker.resolved_functions(), checker.resolved_externs(),
            checker.resolved_operators(), &diag, options.debug_symbols_level,
            emit_entry_point, options.no_runtime, options.gallt_abi);
        generator.generate();

        if (diag.has_errors()) {
            diag.report_note(SourceLocation{},
                "code generation completed with " +
                std::to_string(diag.error_count()) +
                " error(s); all of its diagnostics were listed above and the "
                "remaining steps (artifact emission and linking) were skipped, so "
                "no output artifact was produced");
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
        const bool attach_runtime = !options.no_runtime;

        if (!write_utf8_file(ir_path, generator.ir()) ||
            (attach_runtime && !write_utf8_file(c_path, runtime_source))) {
            std::cerr << "sgc: cannot create temporary backend files ("
                << pal::file_error_text(pal::last_file_error()) << ")\n";
            return pal::exit_failure_code();
        }

        const std::string clang_path = pal::find_llvm_clang();

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

        if (attach_runtime &&
            !pal::environment_variable_defined("SGC_KEEP_TEMP")) {
            runtime_object = acquire_runtime_object(clang_path, temp_dir,
                c_path_text, runtime_source, optimization_flag,
                options.debug_symbols_level, unique);
        }

        std::vector<std::string> runtime_arguments;

        if (!attach_runtime) {
            runtime_arguments.clear();
        } else if (!runtime_object.empty()) {
            runtime_arguments = { "-x", "none", runtime_object };
        } else {
            runtime_arguments = { "-x", "c", c_path_text };
        }

        std::vector<std::string> tool_args = {
            std::string("--target=") + pal::target_triple(),
            optimization_flag,
            "-Wno-override-module",
            "-Wno-deprecated-declarations",
            "-x", "ir",
            pal::to_utf8(ir_path.wstring()),
        };
        const std::vector<std::string> linker_arguments =
            pal::linker_selection_arguments();
        tool_args.insert(tool_args.begin() + 1, linker_arguments.begin(),
            linker_arguments.end());
        tool_args.insert(tool_args.end(), runtime_arguments.begin(),
            runtime_arguments.end());
        tool_args.push_back("-o");
        tool_args.push_back(output_path);

        if (output_kind == OutputKind::Executable) {
            const std::vector<std::string> stack_arguments =
                pal::stack_arguments(options.stack_size, options.commit_size);
            tool_args.insert(tool_args.end(), stack_arguments.begin(),
                stack_arguments.end());
            const std::vector<std::string> linker_mode_arguments =
                pal::linker_mode_arguments(
                    options.linker_mode == LinkerMode::Static);
            tool_args.insert(tool_args.end(), linker_mode_arguments.begin(),
                linker_mode_arguments.end());
        }

        if (output_kind == OutputKind::DynamicLibrary) {
            tool_args.insert(tool_args.begin() + 2, "-shared");

            const std::vector<std::string> export_arguments =
                pal::dynamic_library_export_arguments(
                    generator.exported_functions());
            tool_args.insert(tool_args.end(), export_arguments.begin(),
                export_arguments.end());
        }

        const std::vector<std::string> compile_debug_arguments =
            pal::compile_debug_arguments(options.debug_symbols_level);
        tool_args.insert(tool_args.end(), compile_debug_arguments.begin(),
            compile_debug_arguments.end());
        const std::vector<std::string> link_debug_arguments =
            pal::link_debug_arguments(options.debug_symbols_level);
        tool_args.insert(tool_args.end(), link_debug_arguments.begin(),
            link_debug_arguments.end());

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
            const std::string librarian = pal::find_llvm_librarian();
            if (!pal::file_exists(librarian)) {
                std::cerr << "sgc: LLVM librarian not found: " << librarian
                    << "\n";
                return pal::exit_failure_code();
            }
            object_paths.push_back(fs::path(pal::to_wide(
                pal::join_path(temp_dir,
                    unique + "_gallt" + pal::object_file_extension()))));
            const std::string gallt_object = pal::to_utf8(
                object_paths[0].wstring());
            fs::path runtime_object_path;
            std::string runtime_object_file;

            if (attach_runtime) {
                runtime_object_path = fs::path(pal::to_wide(
                    pal::join_path(temp_dir,
                        unique + "_runtime" + pal::object_file_extension())));
                runtime_object_file = pal::to_utf8(runtime_object_path.wstring());
                object_paths.push_back(runtime_object_path);
            }

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
            const std::vector<std::string> ir_debug_arguments =
                pal::compile_debug_arguments(options.debug_symbols_level);
            ir_compile.insert(ir_compile.end(), ir_debug_arguments.begin(),
                ir_debug_arguments.end());
            link_result = pal::run_process(clang_path, ir_compile, &tool_output);

            if (link_result != 0) {
                if (!tool_output.empty()) { std::cerr << tool_output; }
                std::cerr << "sgc: LLVM backend failed with exit code "
                    << link_result << '\n';
                return link_result;
            }

            std::string runtime_archive_member = runtime_object;

            if (attach_runtime && runtime_archive_member.empty()) {
                std::vector<std::string> c_compile = compile_common;
                c_compile.push_back("-x");
                c_compile.push_back("c");
                c_compile.push_back(c_path_text);
                c_compile.push_back("-o");
                c_compile.push_back(runtime_object_file);
                const std::vector<std::string> c_debug_arguments =
                    pal::compile_debug_arguments(options.debug_symbols_level);
                c_compile.insert(c_compile.end(), c_debug_arguments.begin(),
                    c_debug_arguments.end());
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

            std::vector<std::string> archive_members;
            archive_members.push_back(gallt_object);

            if (attach_runtime) {
                archive_members.push_back(runtime_archive_member);
            }

            const std::vector<std::string> archive_args =
                pal::llvm_librarian_arguments(output_path, archive_members);

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
            std::cout << "compilation successful\n";
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
