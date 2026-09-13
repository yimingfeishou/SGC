// driver/compiler.cpp
// 编译器驱动实现
// Compiler driver implementation

#include "compiler.hpp"

#include "../common/diagnostics.hpp"
#include "../common/token.hpp"
#include "../codegen/codegen.hpp"
#include "../lexer/lexer.hpp"
#include "../parser/ast.hpp"
#include "../parser/parser.hpp"
#include "../semantic/generic_expander.hpp"
#include "../semantic/lifecycle.hpp"
#include "../semantic/namespace_lowering.hpp"
#include "../semantic/type_checker.hpp"

#include <windows.h>

#include <filesystem>
#include <functional>
#include <fstream>
#include <iostream>
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

    std::string wide_to_utf8(const std::wstring& w) {
        if (w.empty()) return std::string();
        int n = ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
            nullptr, 0, nullptr, nullptr);
        std::string out(static_cast<size_t>(n), '\0');
        ::WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()),
            out.data(), n, nullptr, nullptr);
        return out;
    }

    std::wstring utf8_to_wide(const std::string& s) {
        if (s.empty()) return std::wstring();
        int n = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
            nullptr, 0);
        std::wstring out(static_cast<size_t>(n), L'\0');
        ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
            out.data(), n);
        return out;
    }

    std::optional<std::string> read_entire_file(const fs::path& path) {
        // 以二进制读取源码文件；编码检查由 Lexer 完成
        // Read source bytes; the lexer validates the encoding
        std::ifstream in(path, std::ios::binary);
        if (!in) return std::nullopt;
        std::ostringstream ss;
        ss << in.rdbuf();
        if (in.bad()) return std::nullopt;
        return ss.str();
    }

    bool write_utf8_file(const fs::path& path, const std::string& content) {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out.write(content.data(), static_cast<std::streamsize>(content.size()));
        return out.good();
    }

    std::wstring quote_for_process(const std::wstring& value) {
        // CreateProcessW 不需要 shell 转义，只处理引号
        // CreateProcessW does not need shell escaping; handle quotes only
        return L"\"" + value + L"\"";
    }

    int run_process(const std::wstring& executable,
        const std::vector<std::wstring>& args,
        std::string* captured_output) {
        // 使用 CreateProcessW 启动 LLVM 工具并等待结束
        // Launch an LLVM tool through CreateProcessW and wait for completion
        std::wstring command = quote_for_process(executable);
        for (const std::wstring& arg : args) {
            command += L' ';
            std::wstring quoted;
            bool has_space = arg.find(L' ') != std::wstring::npos ||
                arg.find(L'\t') != std::wstring::npos;
            if (has_space) quoted = quote_for_process(arg);
            else quoted = arg;
            command += quoted;
        }

        std::vector<wchar_t> buffer(command.begin(), command.end());
        buffer.push_back(L'\0');

        HANDLE read_pipe = nullptr;
        HANDLE write_pipe = nullptr;
        SECURITY_ATTRIBUTES sa{ sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE };
        if (captured_output != nullptr) {
            if (!::CreatePipe(&read_pipe, &write_pipe, &sa, 0)) {
                return 1;
            }
            ::SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);
        }

        STARTUPINFOW si{ sizeof(STARTUPINFOW) };
        PROCESS_INFORMATION pi{};
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = write_pipe ? write_pipe : ::GetStdHandle(STD_OUTPUT_HANDLE);
        si.hStdError = write_pipe ? write_pipe : ::GetStdHandle(STD_ERROR_HANDLE);
        si.hStdInput = ::GetStdHandle(STD_INPUT_HANDLE);

        BOOL ok = ::CreateProcessW(executable.c_str(), buffer.data(),
            nullptr, nullptr, TRUE,
            CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
        if (write_pipe) ::CloseHandle(write_pipe);
        if (!ok) {
            if (read_pipe) ::CloseHandle(read_pipe);
            return 127;
        }

        if (captured_output != nullptr) {
            char chunk[4096];
            DWORD read_count = 0;
            while (::ReadFile(read_pipe, chunk, sizeof(chunk), &read_count, nullptr) &&
                read_count > 0) {
                captured_output->append(chunk, read_count);
            }
            ::CloseHandle(read_pipe);
        }

        ::WaitForSingleObject(pi.hProcess, INFINITE);
        DWORD code = 1;
        ::GetExitCodeProcess(pi.hProcess, &code);
        ::CloseHandle(pi.hThread);
        ::CloseHandle(pi.hProcess);
        return static_cast<int>(code);
    }

    fs::path find_llvm_executable(const std::wstring& tool_name) {
        // 查找 clang 等 LLVM 工具：先环境变量，再使用仓库中常见构建目录
        // Locate LLVM tools: environment variables first, then common build tree
        const wchar_t* envs[] = { L"SGC_LLVM_BIN", L"LLVM_BIN" };
        for (const wchar_t* env : envs) {
            DWORD size = ::GetEnvironmentVariableW(env, nullptr, 0);
            if (size > 0) {
                std::wstring dir(static_cast<size_t>(size - 1), L'\0');
                ::GetEnvironmentVariableW(env, dir.data(), size);
                fs::path p(dir);
                p /= tool_name;
                if (fs::exists(p)) return p;
            }
        }

        wchar_t module_path[MAX_PATH];
        if (::GetModuleFileNameW(nullptr, module_path, MAX_PATH) > 0) {
            fs::path sgc_dir = fs::path(module_path).parent_path();
            fs::path p = sgc_dir / tool_name;
            if (fs::exists(p)) return p;
        }

        std::vector<fs::path> candidates = {
            L"C:/LLVM/build/Release/bin",
            L"D:/LLVM/build/Release/bin",
			L"E:/LLVM/build/Release/bin",
        };
        for (const fs::path& dir : candidates) {
            fs::path p = dir / tool_name;
            if (fs::exists(p)) return p;
        }
        return tool_name;
    }

    // 展开 guide 导入，将所有顶层节点合并到一个 Program
    // Expand guide imports and merge all top-level nodes into one Program
    struct LoadedSource {
        std::shared_ptr<std::string> source;
        std::shared_ptr<std::string> name;
        std::unique_ptr<AST::Program> program;
        fs::path canonical_path;
    };

    bool load_one_file(const fs::path& path, DiagnosticEngine& diag, LoadedSource& out) {
        auto bytes = read_entire_file(path);
        if (!bytes.has_value()) {
            diag.report_error(SourceLocation{}, ErrorCode::LibraryNotFound,
                "cannot open source file");
            return false;
        }
        auto source = std::make_shared<std::string>(std::move(*bytes));
        auto name = std::make_shared<std::string>(wide_to_utf8(path.wstring()));
        std::string_view source_view(*source);
        std::string_view name_view(*name);
        Lexer lexer(source_view, name_view, diag);
        Parser parser(lexer, diag);
        out.source = source;
        out.name = name;
        out.program = parser.parse();
        return out.program != nullptr;
    }

    std::string normalize_library_reference(const std::string& library) {
        std::string result = library;
        for (char& c : result) {
            if (c == '\\') c = '/';
        }
        return result;
    }

    // 标准库搜索目录列表。
    // Gallt 0.3（最新修正）规定：
    //   “标准库文件夹必须和编译器文件夹在同级目录，否则编译器无法找到标准库”
    // 即：以编译器可执行文件所在目录为“编译器文件夹”，其**同级目录**下的
    // `sl/` 就是标准库文件夹：<编译器文件夹>/../sl/。
    //
    // 下列顺序中，第 1 项是显式环境变量覆盖（非文档路径，仅用于部署自定义），
    // 第 2 项是文档规定的权威路径，其余为兼容回退（例如源码树内把 exe 与 sl
    // 放在同一目录的历史布局），不影响文档路径的优先级。
    // Standard-library search directories. The latest Gallt 0.3 revision states that the
    // standard-library folder must be a sibling of the compiler folder, i.e.
    // <compiler folder>/../sl/. Item 1 is an explicit environment override (not a
    // documented path); item 2 is the documented authoritative path; the rest are
    // compatibility fallbacks only.
    const std::vector<fs::path>& standard_library_dirs() {
        static const std::vector<fs::path> dirs = [] {
            std::vector<fs::path> out;
            // 显式覆盖：SGC_STDLIB / SGC_SL 优先
            // Explicit overrides: SGC_STDLIB / SGC_SL take priority
            for (const wchar_t* env : { L"SGC_STDLIB", L"SGC_SL" }) {
                DWORD size = ::GetEnvironmentVariableW(env, nullptr, 0);
                if (size > 0) {
                    std::wstring value(static_cast<size_t>(size - 1), L'\0');
                    ::GetEnvironmentVariableW(env, value.data(), size);
                    out.emplace_back(value);
                }
            }
            fs::path exe_dir;
            wchar_t module_path[MAX_PATH];
            if (::GetModuleFileNameW(nullptr, module_path, MAX_PATH) > 0) {
                fs::path self(module_path);
                exe_dir = self.parent_path();
                // 【文档规定】标准库文件夹与编译器文件夹同级：<编译器文件夹>/../sl/
                // Documented: the stdlib folder is a sibling of the compiler folder
                out.push_back(exe_dir.parent_path() / L"sl");
                // 兼容回退：编译器与 sl 位于同一目录（源码树 in-tree 布局）
                // Compatibility fallback: compiler and sl in the same folder
                out.push_back(exe_dir / L"sl");
                // 兼容回退：源码树布局 <仓库>/SGC/sl
                out.push_back(exe_dir.parent_path() / L"SGC" / L"sl");
            }
            // 兼容回退：当前工作目录下的 sl/
            // Compatibility fallback: ./sl
            out.push_back(fs::path(L".") / L"sl");
            return out;
        }();
        return dirs;
    }

    // 解析 guide 引用：先相对当前文件，再相对标准库目录（支持短名导入）
    // Resolve a guide reference: relative to the current file first, then the
    // standard-library directories (this is what makes short names work)
    std::optional<fs::path> resolve_guide_path(const std::string& guide,
        const fs::path& current_dir) {
        std::string normalized = normalize_library_reference(guide);
        fs::path referenced(utf8_to_wide(normalized));
        std::error_code ec;
        if (referenced.is_absolute()) {
            if (fs::exists(referenced)) {
                return fs::weakly_canonical(referenced, ec);
            }
            return std::nullopt;
        }
        // 1) 相对当前源文件所在目录 / relative to the importing file
        fs::path local = current_dir / referenced;
        if (fs::exists(local)) {
            return fs::weakly_canonical(local, ec);
        }
        // 2) 标准库目录（短名导入）/ standard-library directories (short names)
        for (const fs::path& dir : standard_library_dirs()) {
            fs::path candidate = dir / referenced;
            if (fs::exists(candidate)) {
                return fs::weakly_canonical(candidate, ec);
            }
            // 短名：只按文件名在标准库目录下查找
            // Short name: look up by file name inside the standard-library directory
            if (!referenced.has_parent_path()) {
                fs::path by_name = dir / referenced.filename();
                if (fs::exists(by_name)) {
                    return fs::weakly_canonical(by_name, ec);
                }
            }
        }
        return std::nullopt;
    }

} // anonymous namespace

    int run_compiler(const CommandOptions& options) {
        if (options.input.empty()) {
            std::cerr << "sgc: no input file; use --input <file.glt>\n";
            return 1;
        }
        fs::path main_path(utf8_to_wide(options.input));
        std::error_code ec;
        fs::path canonical_main = fs::weakly_canonical(main_path, ec);
        if (ec) canonical_main = main_path;

        DiagnosticEngine diag;
        std::vector<LoadedSource> sources;
        std::vector<fs::path> visited;

        // 使用递归函数按 guide 出现顺序加载
        // Recursively load files in guide order
        std::function<bool(const fs::path&)> load_with_guides =
            [&](const fs::path& path) -> bool {
            std::error_code ce;
            fs::path canonical = fs::weakly_canonical(path, ce);
            if (ce) canonical = path;
            if (std::find(visited.begin(), visited.end(), canonical) != visited.end()) {
                return true;
            }
            visited.push_back(canonical);
            LoadedSource ls;
            ls.canonical_path = canonical;
            if (!load_one_file(canonical, diag, ls)) {
                return false;
            }
            // 记录 source 后递归 guide；guide 节点保留在原始 Program 中
            // Keep buffers alive, then recurse into guide nodes of the parsed program
            size_t current_size = sources.size();
            sources.push_back(std::move(ls));
            // 注意：递归调用会向 sources 追加元素，可能使先前取得的引用失效；
            // 因此只取一次 Program 指针（Program 本身在堆上，不随 vector 搬移），
            // 不在循环中继续使用指向 vector 元素的引用。
            // Recursion pushes into `sources`, which may reallocate the vector, so we
            // only keep the heap-allocated Program pointer and never a reference into
            // the vector that stays alive across the recursive call.
            AST::Program* loaded_program = sources.back().program.get();
            for (auto& top : loaded_program->top_levels) {
                if (auto* guide = dynamic_cast<AST::GuideStatement*>(top.get())) {
                    std::string gpath = normalize_library_reference(guide->path);
                    // 解析顺序：当前文件目录 → 标准库目录（短名导入）
                    // Resolution order: importing file directory -> standard library
                    std::optional<fs::path> resolved =
                        resolve_guide_path(gpath, canonical.parent_path());
                    if (!resolved.has_value()) {
                        diag.report_error(guide->location, ErrorCode::LibraryNotFound,
                            "guide file not found: " + gpath);
                        return false;
                    }
                    if (!load_with_guides(*resolved)) {
                        return false;
                    }
                }
            }
            (void)current_size;
            return true;
        };

        if (!load_with_guides(canonical_main)) {
            diag.print_all(std::cerr);
            return 1;
        }
        if (diag.has_errors()) {
            diag.print_all(std::cerr);
            return 1;
        }

        // 将所有顶层节点移入组合 Program
        // Move every top-level node into the combined Program
        std::vector<std::unique_ptr<AST::TopLevel>> all_nodes;
        std::unordered_map<fs::path, LoadedSource*> by_path;
        for (LoadedSource& ls : sources) {
            by_path[ls.canonical_path] = &ls;
        }
        std::unordered_set<fs::path> expanded;
        std::function<void(LoadedSource*)> append_in_guide_order =
            [&](LoadedSource* current) {
            if (current == nullptr) return;
            if (!expanded.insert(current->canonical_path).second) return;
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
            append_in_guide_order(&sources.front());
        }
        SourceLocation fake_start;
        AST::Program combined(fake_start, std::move(all_nodes));

        // Gallt 0.3.txt §19：编译期泛型展开（单态化），必须先于类型检查执行
        // Gallt 0.3.txt §19: expand compile-time generics before type checking
        // Gallt 0.4.txt §21：命名空间降低（在泛型展开之前完成名字归属与限定名解析）
        // Gallt 0.4.txt §21: lower namespaces before generic expansion so that name
        // ownership and qualified-name resolution are already settled
        NamespaceLowering namespaces(diag);
        if (!namespaces.run(&combined) || diag.has_errors()) {
            diag.print_all(std::cerr);
            return 1;
        }

        GenericExpander expander(diag);
        if (!expander.expand(&combined) || diag.has_errors()) {
            diag.print_all(std::cerr);
            return 1;
        }

        // Gallt 0.3.txt §20：对象生命周期降低（构造/析构/拷贝/移动）
        // Gallt 0.3.txt §20: lower object lifetime (ctor/dtor/copy/move)
        LifecycleLowering lifecycle(diag);
        if (!lifecycle.run(&combined) || diag.has_errors()) {
            diag.print_all(std::cerr);
            return 1;
        }

        TypeChecker checker(diag);
        if (!checker.check_program(&combined)) {
            diag.print_all(std::cerr);
            return 1;
        }

        CodeGenerator generator(&combined, checker.expression_types(),
            checker.resolved_functions(), checker.resolved_externs());
        generator.generate();
        if (diag.has_errors()) {
            diag.print_all(std::cerr);
            return 1;
        }

        // 写临时 IR 与运行库 C 文件
        // Write temporary IR and runtime C files
        wchar_t temp_dir[MAX_PATH];
        ::GetTempPathW(MAX_PATH, temp_dir);
        std::wstring unique = L"sgc_" + std::to_wstring(::GetCurrentProcessId());
        fs::path ir_path = fs::path(temp_dir) / (unique + L".ll");
        fs::path c_path = fs::path(temp_dir) / (unique + L".c");
        if (!write_utf8_file(ir_path, generator.ir()) ||
            !write_utf8_file(c_path, CodeGenerator::runtime_c_source())) {
            std::cerr << "sgc: cannot create temporary backend files\n";
            return 1;
        }

        fs::path clang_path = find_llvm_executable(L"clang.exe");
        std::wstring output_path = utf8_to_wide(options.output.empty()
            ? fs::path(options.input).replace_extension(".exe").string()
            : options.output);
        if (!fs::path(output_path).has_extension()) {
            output_path += L".exe";
        }

        // 优化等级（Doc/编译器参数.txt）直接转发给 clang：0→-O0、1→-O1、2→-O2、
        // 3→-O3、4→-Os。编译器自身不实现优化，全部交由 LLVM 后端。
        // The optimization level (Doc/compiler-options.txt) is forwarded verbatim to clang:
        // 0->-O0, 1->-O1, 2->-O2, 3->-O3, 4->-Os. Optimization itself is left to LLVM.
        std::wstring optimization_flag;
        switch (options.optimization_level) {
        case 0: optimization_flag = L"-O0"; break;
        case 1: optimization_flag = L"-O1"; break;
        case 3: optimization_flag = L"-O3"; break;
        case 4: optimization_flag = L"-Os"; break;
        case 2:
        default: optimization_flag = L"-O2"; break;
        }

        std::vector<std::wstring> tool_args = {
            L"--target=x86_64-pc-windows-msvc",
            L"-fuse-ld=lld",
            optimization_flag,
            L"-Wno-override-module",
            L"-Wno-deprecated-declarations",
            L"-x", L"ir",
            ir_path.wstring(),
            L"-x", L"c",
            c_path.wstring(),
            L"-o", output_path,
        };

        // clib 指定的库：优先直接传 .lib/.a，其次尝试 .dll 对应的导入库
        // clib libraries are passed directly when a .lib/.a is found
        // 重置语言选项，使后续 clib 库作为链接器输入，而不是被 clang 当作 C 源文件解析
        // Reset the language mode so clib libraries are linker inputs, not C source files
        bool reset_language = false;
        for (const std::string& lib : generator.link_libraries()) {
            std::string ref = normalize_library_reference(lib);
            fs::path lib_path(utf8_to_wide(ref));
            std::vector<fs::path> probes = { lib_path };
            if (lib_path.extension().empty()) {
                probes.push_back(fs::path(lib_path.wstring() + L".lib"));
            }
            for (const fs::path& probe : probes) {
                if (fs::exists(probe)) {
                    if (!reset_language) {
                        tool_args.push_back(L"-x");
                        tool_args.push_back(L"none");
                        reset_language = true;
                    }
                    tool_args.push_back(probe.wstring());
                    break;
                }
            }
        }

        std::string tool_output;
        int link_result = run_process(clang_path, tool_args, &tool_output);
        if (!tool_output.empty()) {
            std::cerr << tool_output;
        }
        if (link_result != 0) {
            std::cerr << "sgc: LLVM backend failed with exit code "
                << link_result << '\n';
        }

        if (::GetEnvironmentVariableW(L"SGC_KEEP_TEMP", nullptr, 0) == 0) {
            std::error_code remove_ec;
            fs::remove(ir_path, remove_ec);
            fs::remove(c_path, remove_ec);
        } else {
            std::cerr << "sgc: backend files retained: "
                << wide_to_utf8(ir_path.wstring()) << "\n";
        }
        return link_result;
    }

} // namespace gallt
