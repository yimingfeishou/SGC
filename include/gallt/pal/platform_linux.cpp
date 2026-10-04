#include "platform.hpp"

#if SGC_PLATFORM_LINUX

#include "../driver/entry.hpp"

#include <cerrno>
#include <cstdlib>
#include <dlfcn.h>
#include <fstream>
#include <poll.h>
#include <signal.h>
#include <sstream>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace gallt {
namespace pal {

    namespace {
        int g_last_file_error = 0;

        void set_last_file_error(int code) noexcept {
            g_last_file_error = code;
        }

        bool is_executable_file(const std::string& path) {
            if (path.empty()) { return false; }
            struct stat info = {};
            if (::stat(path.c_str(), &info) != 0) { return false; }
            if (!S_ISREG(info.st_mode)) { return false; }
            return ::access(path.c_str(), X_OK) == 0;
        }

        std::string find_program_in_path(const std::string& name) {
            const char* path = ::getenv("PATH");
            if (path == nullptr) { return std::string(); }

            std::string directories(path);
            std::size_t begin = 0;

            while (begin <= directories.size()) {
                std::size_t end = directories.find(':', begin);
                if (end == std::string::npos) { end = directories.size(); }

                std::string directory = directories.substr(begin, end - begin);
                if (directory.empty()) { directory = "."; }

                std::string candidate = directory + "/" + name;
                if (is_executable_file(candidate)) { return candidate; }

                begin = end + 1;
            }

            return std::string();
        }

        std::string find_tool(const std::string& tool_name) {
            for (const char* env : { "SGC_LLVM_BIN", "LLVM_BIN" }) {
                std::string dir = environment_variable(env);
                if (dir.empty()) { continue; }
                std::string candidate = join_path(dir, tool_name);
                if (is_executable_file(candidate)) { return candidate; }
            }

            std::string module_path = executable_path();

            if (!module_path.empty()) {
                std::string candidate = join_path(parent_path(module_path),
                    tool_name);
                if (is_executable_file(candidate)) { return candidate; }
            }

            std::string on_path = find_program_in_path(tool_name);
            if (!on_path.empty()) { return on_path; }

            return tool_name;
        }
    }

    const char* platform_name() noexcept { return "linux"; }

    bool is_windows() noexcept { return false; }

    bool is_linux() noexcept { return true; }

    const char* target_triple() noexcept { return "x86_64-unknown-linux-gnu"; }

    const char* future_linux_target_triple() noexcept {
        return "x86_64-unknown-linux-gnu";
    }

    const char* platform_support_notes() noexcept {
        return "linux: path and file operations, console handling, dynamic "
            "library loading, file mode conversion, file error mapping and "
            "process spawning via the system linker toolchain are implemented";
    }

    std::string to_utf8(const std::wstring& text) {
        std::string out;
        out.reserve(text.size());

        for (wchar_t code_point : text) {
            const unsigned int value = static_cast<unsigned int>(code_point);

            if (value < 0x80u) {
                out.push_back(static_cast<char>(value));
            } else if (value < 0x800u) {
                out.push_back(static_cast<char>(0xC0u | (value >> 6)));
                out.push_back(static_cast<char>(0x80u | (value & 0x3Fu)));
            } else {
                out.push_back(static_cast<char>(0xE0u | (value >> 12)));
                out.push_back(static_cast<char>(0x80u | ((value >> 6) & 0x3Fu)));
                out.push_back(static_cast<char>(0x80u | (value & 0x3Fu)));
            }
        }

        return out;
    }

    std::wstring to_wide(const std::string& text) {
        std::wstring out;
        out.reserve(text.size());

        for (size_t i = 0; i < text.size();) {
            const unsigned char lead = static_cast<unsigned char>(text[i]);
            unsigned int value = 0;
            size_t extra = 0;

            if (lead < 0x80u) {
                value = lead;
            } else if ((lead & 0xE0u) == 0xC0u) {
                value = lead & 0x1Fu;
                extra = 1;
            } else if ((lead & 0xF0u) == 0xE0u) {
                value = lead & 0x0Fu;
                extra = 2;
            } else {
                out.push_back(static_cast<wchar_t>(lead));
                ++i;
                continue;
            }

            if (i + extra >= text.size()) {
                out.push_back(static_cast<wchar_t>(lead));
                ++i;
                continue;
            }

            for (size_t k = 1; k <= extra; ++k) {
                value = (value << 6) |
                    (static_cast<unsigned char>(text[i + k]) & 0x3Fu);
            }
            out.push_back(static_cast<wchar_t>(value));
            i += extra + 1;
        }

        return out;
    }

    std::string executable_path() {
        std::string link(4096, '\0');
        const ssize_t written = ::readlink("/proc/self/exe", link.data(),
            link.size());

        if (written <= 0) { return std::string(); }
        link.resize(static_cast<size_t>(written));
        return link;
    }

    std::string find_llvm_clang() {
        return find_tool("clang");
    }

    std::string find_llvm_librarian() {
        return find_tool("llvm-ar");
    }

    std::vector<std::string> llvm_librarian_arguments(
        const std::string& output, const std::vector<std::string>& members) {
        std::vector<std::string> out;
        out.push_back("rcs");
        out.push_back(output);
        out.insert(out.end(), members.begin(), members.end());
        return out;
    }

    std::vector<std::string> linker_selection_arguments() {
        if (!find_program_in_path("ld.lld").empty() ||
            !find_program_in_path("lld").empty()) {
            return { "-fuse-ld=lld" };
        }

        return std::vector<std::string>();
    }

    std::vector<std::string> stack_arguments(
        const std::optional<std::uint64_t>& stack_size,
        const std::optional<std::uint64_t>& commit_size) {
        if (!stack_size.has_value()) { return std::vector<std::string>(); }
        return { "-Wl,-z,stack-size=" + std::to_string(*stack_size) };
    }

    std::vector<std::string> linker_mode_arguments(bool static_link) {
        if (!static_link) { return std::vector<std::string>(); }
        return { "-static" };
    }

    std::vector<std::string> dynamic_library_export_arguments(
        const std::vector<std::string>& symbols) {
        std::string text = "{\n  global:\n";

        for (const std::string& symbol : symbols) {
            text += "    " + symbol + ";\n";
        }

        text += "  local:\n    *;\n};\n";

        const std::string path = join_path(temporary_directory(),
            "sgc_exports_" + process_id_text() + ".map");

        if (!write_file(path, text)) {
            return std::vector<std::string>();
        }

        return { "-Wl,--version-script=" + path };
    }

    std::string environment_variable(const std::string& name) {
        const char* value = ::getenv(name.c_str());
        return value == nullptr ? std::string() : std::string(value);
    }

    bool environment_variable_defined(const std::string& name) {
        return ::getenv(name.c_str()) != nullptr;
    }

    std::string temporary_directory() {
        for (const char* candidate : { "TMPDIR", "TMP", "TEMP" }) {
            const char* value = ::getenv(candidate);
            if (value != nullptr && value[0] != '\0') { return std::string(value); }
        }

        return std::string("/tmp/");
    }

    int process_id() noexcept {
        return static_cast<int>(::getpid());
    }

    std::string process_id_text() {
        return std::to_string(static_cast<unsigned long>(::getpid()));
    }

    bool enable_utf8_console() noexcept { return true; }

    bool standard_input_is_interactive() noexcept {
        return ::isatty(STDIN_FILENO) == 1;
    }

    bool standard_input_is_readable() noexcept {
        struct stat info = {};

        if (::fstat(STDIN_FILENO, &info) != 0) {
            return false;
        }

        if (S_ISREG(info.st_mode)) {
            const off_t position = ::lseek(STDIN_FILENO, 0, SEEK_CUR);

            if (position < 0) {
                return false;
            }

            return info.st_size > position;
        }

        struct pollfd descriptor = {};
        descriptor.fd = STDIN_FILENO;
        descriptor.events = POLLIN;
        return ::poll(&descriptor, 1, 0) == 1;
    }

    namespace {
        struct StackBounds {
            const char* low;
            const char* high;
        };

        StackBounds detect_stack_bounds() noexcept {
            StackBounds bounds{ nullptr, nullptr };
            char marker = 0;
            const std::uintptr_t current =
                reinterpret_cast<std::uintptr_t>(&marker);
            std::ifstream maps("/proc/self/maps");
            std::string line;

            while (std::getline(maps, line)) {
                const std::size_t dash = line.find('-');

                if (dash == std::string::npos) { continue; }

                const std::size_t blank = line.find(' ', dash);

                if (blank == std::string::npos) { continue; }

                const std::uintptr_t begin = static_cast<std::uintptr_t>(
                    ::strtoull(line.substr(0, dash).c_str(), nullptr, 16));
                const std::uintptr_t end = static_cast<std::uintptr_t>(
                    ::strtoull(line.substr(dash + 1, blank - dash - 1).c_str(),
                        nullptr, 16));

                if (current >= begin && current < end) {
                    bounds.low = reinterpret_cast<const char*>(begin);
                    bounds.high = reinterpret_cast<const char*>(end);
                    break;
                }
            }

            return bounds;
        }

        StackBounds current_stack_bounds() noexcept {
            static thread_local const StackBounds bounds = detect_stack_bounds();
            return bounds;
        }
    }

    std::size_t stack_total_bytes() noexcept {
        const StackBounds bounds = current_stack_bounds();

        if (bounds.low == nullptr || bounds.high == nullptr) {
            return kStackHeadroomUnknown;
        }

        return static_cast<std::size_t>(bounds.high - bounds.low);
    }

    std::size_t stack_headroom_bytes() noexcept {
        const StackBounds bounds = current_stack_bounds();

        if (bounds.low == nullptr || bounds.high == nullptr) {
            return kStackHeadroomUnknown;
        }

        char marker = 0;
        const char* current = &marker;

        if (current <= bounds.low || current >= bounds.high) {
            return 0;
        }

        return static_cast<std::size_t>(current - bounds.low);
    }

    bool stack_headroom_available(std::size_t required_bytes) noexcept {
        return stack_headroom_bytes() >= required_bytes;
    }

    namespace {
        alignas(16) char g_stack_guard_region[64 * 1024];

        void stack_guard_handler(int signal_number, siginfo_t* info, void*) {
            const std::uintptr_t fault = reinterpret_cast<std::uintptr_t>(
                info == nullptr ? nullptr : info->si_addr);
            const StackBounds bounds = current_stack_bounds();
            const std::uintptr_t low =
                reinterpret_cast<std::uintptr_t>(bounds.low);

            if (low != 0 && fault >= low && fault < low + 64 * 1024) {
                ssize_t ignored = ::write(STDERR_FILENO, kStackOverflowMessage,
                    sizeof(kStackOverflowMessage) - 1);
                (void)ignored;
                ignored = ::write(STDERR_FILENO, "\n", 1);
                (void)ignored;
                ::_exit(exit_failure_code());
            }

            ::signal(signal_number, SIG_DFL);
            ::raise(signal_number);
        }
    }

    bool install_stack_guard() noexcept {
        static thread_local const StackBounds primed = current_stack_bounds();
        (void)primed;

        stack_t alt = {};
        alt.ss_sp = g_stack_guard_region;
        alt.ss_size = sizeof(g_stack_guard_region);
        alt.ss_flags = 0;

        if (::sigaltstack(&alt, nullptr) != 0) { return false; }

        struct sigaction action = {};
        action.sa_sigaction = stack_guard_handler;
        action.sa_flags = SA_SIGINFO | SA_ONSTACK;
        ::sigemptyset(&action.sa_mask);

        if (::sigaction(SIGSEGV, &action, nullptr) != 0) { return false; }
        ::sigaction(SIGBUS, &action, nullptr);
        return true;
    }

    int exit_success_code() noexcept { return 0; }

    int exit_failure_code() noexcept { return 1; }

    bool read_file(const std::string& path, std::string& out) {
        std::ifstream in(path, std::ios::binary);

        if (!in) {
            set_last_file_error(file_error_from_errno(errno));
            return false;
        }

        std::ostringstream buffer;
        buffer << in.rdbuf();

        if (in.bad()) {
            set_last_file_error(static_cast<int>(FileError::ReadFailed));
            return false;
        }

        set_last_file_error(static_cast<int>(FileError::None));
        out = buffer.str();
        return true;
    }

    bool write_file(const std::string& path, const std::string& content) {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);

        if (!output) {
            set_last_file_error(file_error_from_errno(errno));
            return false;
        }

        output.write(content.data(),
            static_cast<std::streamsize>(content.size()));

        if (!output.good()) {
            set_last_file_error(static_cast<int>(FileError::WriteFailed));
            return false;
        }

        set_last_file_error(static_cast<int>(FileError::None));
        return true;
    }

    std::string file_open_mode(const std::string& gallt_mode) {
        std::string out;
        out.reserve(gallt_mode.size());

        for (char c : gallt_mode) {
            if (c == 'b') { continue; }
            out.push_back(c);
        }
        return out;
    }

    int file_error_from_errno(int errno_value) noexcept {
        switch (errno_value) {
        case 0: return static_cast<int>(FileError::None);
        case ENOENT: return static_cast<int>(FileError::NotFound);
        case EACCES:
        case EPERM: return static_cast<int>(FileError::Permission);
        case EEXIST: return static_cast<int>(FileError::AlreadyExists);
        case ENOSPC: return static_cast<int>(FileError::DiskFull);
        default: return static_cast<int>(FileError::Unknown);
        }
    }

    int file_error_from_system_error(int system_error_value) noexcept {
        return file_error_from_errno(system_error_value);
    }

    int last_file_error() noexcept { return g_last_file_error; }

    const char* file_error_text(int code) noexcept {
        switch (static_cast<FileError>(code)) {
        case FileError::None: return "no error";
        case FileError::InvalidHandle: return "invalid or closed handle";
        case FileError::Permission: return "permission denied";
        case FileError::NotFound: return "path not found";
        case FileError::AlreadyExists: return "path already exists";
        case FileError::ReadFailed: return "read failure";
        case FileError::WriteFailed: return "write failure";
        case FileError::SeekFailed: return "seek failure";
        case FileError::DiskFull: return "disk full";
        default: return "unknown error";
        }
    }

    void* load_library(const std::string& path) {
        if (path.empty()) {
            set_last_file_error(static_cast<int>(FileError::InvalidHandle));
            return nullptr;
        }

        void* handle = ::dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);

        if (handle == nullptr) {
            set_last_file_error(file_exists(path)
                ? static_cast<int>(FileError::Unknown)
                : static_cast<int>(FileError::NotFound));
            return nullptr;
        }

        set_last_file_error(static_cast<int>(FileError::None));
        return handle;
    }

    void close_library(void* handle) noexcept {
        if (handle != nullptr) {
            ::dlclose(handle);
        }
    }

    void* find_symbol(void* handle, const std::string& name) {
        if (handle == nullptr || name.empty()) { return nullptr; }
        return ::dlsym(handle, name.c_str());
    }

    int run_process(const std::string& executable,
        const std::vector<std::string>& arguments,
        std::string* captured_output) {
        std::vector<std::string> storage;
        storage.reserve(arguments.size() + 1);
        storage.push_back(executable);

        for (const std::string& argument : arguments) {
            storage.push_back(argument);
        }

        std::vector<char*> argv;
        argv.reserve(storage.size() + 1);

        for (std::string& entry : storage) {
            argv.push_back(const_cast<char*>(entry.c_str()));
        }

        argv.push_back(nullptr);

        int pipe_descriptors[2] = { -1, -1 };

        if (captured_output != nullptr && ::pipe(pipe_descriptors) != 0) {
            return kProcessSpawnFailure;
        }

        const pid_t child = ::fork();

        if (child < 0) {
            if (pipe_descriptors[0] >= 0) {
                ::close(pipe_descriptors[0]);
                ::close(pipe_descriptors[1]);
            }

            return kProcessSpawnFailure;
        }

        if (child == 0) {
            if (captured_output != nullptr) {
                ::dup2(pipe_descriptors[1], STDOUT_FILENO);
                ::dup2(pipe_descriptors[1], STDERR_FILENO);
                ::close(pipe_descriptors[0]);
                ::close(pipe_descriptors[1]);
            }

            ::execvp(executable.c_str(), argv.data());
            ::_exit(kProcessSpawnFailure);
        }

        if (captured_output != nullptr) {
            ::close(pipe_descriptors[1]);

            char chunk[4096];

            for (;;) {
                const ssize_t count = ::read(pipe_descriptors[0], chunk,
                    sizeof(chunk));

                if (count > 0) {
                    captured_output->append(chunk, static_cast<size_t>(count));
                    continue;
                }

                if (count < 0 && errno == EINTR) { continue; }
                break;
            }

            ::close(pipe_descriptors[0]);
        }

        int status = 0;

        while (::waitpid(child, &status, 0) < 0) {
            if (errno != EINTR) { return kProcessSpawnFailure; }
        }

        if (WIFEXITED(status)) { return WEXITSTATUS(status); }
        if (WIFSIGNALED(status)) { return 128 + WTERMSIG(status); }
        return kProcessSpawnFailure;
    }

}
}

#endif
