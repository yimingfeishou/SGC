#include "platform.hpp"

#if SGC_PLATFORM_WINDOWS

#include "../driver/entry.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <cerrno>
#include <fstream>
#include <malloc.h>
#include <sstream>
#include <system_error>

namespace gallt {
namespace pal {

    namespace {
        int g_last_file_error = 0;

        void set_last_file_error(int code) noexcept {
            g_last_file_error = code;
        }

        std::string find_program_in_path(const std::string& name) {
            std::string path = environment_variable("PATH");
            if (path.empty()) { return std::string(); }

            std::size_t begin = 0;
            while (begin <= path.size()) {
                std::size_t end = path.find(';', begin);
                if (end == std::string::npos) { end = path.size(); }

                std::string directory = path.substr(begin, end - begin);
                if (!directory.empty()) {
                    if (directory.size() >= 2 &&
                        directory.front() == '"' && directory.back() == '"') {
                        directory = directory.substr(1, directory.size() - 2);
                    }

                    std::string candidate = join_path(directory, name);
                    if (file_exists(candidate)) {
                        return candidate;
                    }
                }

                begin = end + 1;
            }

            return std::string();
        }

        std::string find_tool(const std::string& tool_name) {
            for (const char* env : { "SGC_LLVM_BIN", "LLVM_BIN" }) {
                std::string dir = environment_variable(env);
                if (dir.empty()) { continue; }
                std::string candidate = join_path(dir, tool_name);
                if (file_exists(candidate)) { return candidate; }
            }

            std::string module_path = executable_path();

            if (!module_path.empty()) {
                std::string candidate = join_path(parent_path(module_path),
                    tool_name);
                if (file_exists(candidate)) { return candidate; }
            }

            std::string on_path = find_program_in_path(tool_name);
            if (!on_path.empty()) { return on_path; }

            return tool_name;
        }
    }

    const char* platform_name() noexcept { return "windows"; }

    bool is_windows() noexcept { return true; }

    bool is_linux() noexcept { return false; }

    const char* target_triple() noexcept { return "x86_64-pc-windows-msvc"; }

    const char* future_linux_target_triple() noexcept {
        return "x86_64-unknown-linux-gnu";
    }

    const char* platform_support_notes() noexcept {
        return "windows: executable path, environment, temporary directory, "
            "console UTF-8, path and file operations, dynamic library loading, "
            "file mode conversion, file error mapping and process spawning are "
            "implemented";
    }

    std::string to_utf8(const std::wstring& text) {
        if (text.empty()) { return std::string(); }
        const int size = ::WideCharToMultiByte(CP_UTF8, 0, text.c_str(),
            static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
        if (size <= 0) { return std::string(); }
        std::string out(static_cast<size_t>(size), '\0');
        ::WideCharToMultiByte(CP_UTF8, 0, text.c_str(),
            static_cast<int>(text.size()), out.data(), size, nullptr, nullptr);
        return out;
    }

    std::wstring to_wide(const std::string& text) {
        if (text.empty()) { return std::wstring(); }
        const int size = ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(),
            static_cast<int>(text.size()), nullptr, 0);
        if (size <= 0) { return std::wstring(); }
        std::wstring out(static_cast<size_t>(size), L'\0');
        ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(),
            static_cast<int>(text.size()), out.data(), size);
        return out;
    }

    std::string executable_path() {
        std::wstring buffer(MAX_PATH, L'\0');

        for (;;) {
            DWORD written = ::GetModuleFileNameW(nullptr, buffer.data(),
                static_cast<DWORD>(buffer.size()));
            if (written == 0) { return std::string(); }

            if (written < buffer.size()) {
                buffer.resize(written);
                break;
            }

            buffer.resize(buffer.size() * 2);

            if (buffer.size() > 32768) {
                buffer.resize(written);
                break;
            }
        }

        return to_utf8(buffer);
    }

    std::string find_llvm_clang() {
        return find_tool("clang.exe");
    }

    std::string find_llvm_librarian() {
        return find_tool("llvm-lib.exe");
    }

    std::vector<std::string> llvm_librarian_arguments(
        const std::string& output, const std::vector<std::string>& members) {
        std::vector<std::string> out;
        out.push_back("/nologo");
        out.push_back("/out:" + output);
        out.insert(out.end(), members.begin(), members.end());
        return out;
    }

    std::vector<std::string> linker_selection_arguments() {
        return { "-fuse-ld=lld" };
    }

    std::vector<std::string> stack_arguments(
        const std::optional<std::uint64_t>& stack_size,
        const std::optional<std::uint64_t>& commit_size) {
        if (!stack_size.has_value() && !commit_size.has_value()) {
            return std::vector<std::string>();
        }

        std::string value = "/STACK:";
        value += std::to_string(stack_size.has_value() ? *stack_size : 1048576ull);

        if (commit_size.has_value()) {
            value += ",";
            value += std::to_string(*commit_size);
        }

        return { "-Xlinker", value };
    }

    std::vector<std::string> linker_mode_arguments(bool static_link) {
        if (!static_link) { return std::vector<std::string>(); }
        return { "-static" };
    }

    std::vector<std::string> dynamic_library_export_arguments(
        const std::vector<std::string>& symbols) {
        std::vector<std::string> out;

        for (const std::string& symbol : symbols) {
            out.push_back("-Wl,/EXPORT:" + symbol);
        }

        return out;
    }

    std::string environment_variable(const std::string& name) {
        std::wstring wide_name = to_wide(name);
        DWORD size = ::GetEnvironmentVariableW(wide_name.c_str(), nullptr, 0);
        if (size == 0) { return std::string(); }
        std::wstring value(static_cast<size_t>(size - 1), L'\0');
        DWORD written = ::GetEnvironmentVariableW(wide_name.c_str(), value.data(),
            size);
        if (written == 0) { return std::string(); }
        value.resize(written);
        return to_utf8(value);
    }

    bool environment_variable_defined(const std::string& name) {
        std::wstring wide_name = to_wide(name);
        return ::GetEnvironmentVariableW(wide_name.c_str(), nullptr, 0) != 0;
    }

    std::string temporary_directory() {
        std::wstring buffer(MAX_PATH, L'\0');
        DWORD written = ::GetTempPathW(static_cast<DWORD>(buffer.size()),
            buffer.data());

        if (written == 0 || written >= buffer.size()) {
            buffer.resize(written + 1);
            written = ::GetTempPathW(static_cast<DWORD>(buffer.size()),
                buffer.data());
            if (written == 0 || written >= buffer.size()) { return std::string(); }
        }

        buffer.resize(written);
        return to_utf8(buffer);
    }

    int process_id() noexcept {
        return static_cast<int>(::GetCurrentProcessId());
    }

    std::string process_id_text() {
        return std::to_string(static_cast<unsigned long>(::GetCurrentProcessId()));
    }

    bool enable_utf8_console() noexcept {
        bool ok = ::SetConsoleOutputCP(CP_UTF8) != FALSE;
        ::SetConsoleCP(CP_UTF8);
        return ok;
    }

    bool standard_input_is_interactive() noexcept {
        HANDLE handle = ::GetStdHandle(STD_INPUT_HANDLE);

        if (handle == nullptr || handle == INVALID_HANDLE_VALUE) {
            return false;
        }

        DWORD mode = 0;
        return ::GetConsoleMode(handle, &mode) != FALSE;
    }

    bool standard_input_is_readable() noexcept {
        HANDLE handle = ::GetStdHandle(STD_INPUT_HANDLE);

        if (handle == nullptr || handle == INVALID_HANDLE_VALUE) {
            return false;
        }

        switch (::GetFileType(handle)) {
        case FILE_TYPE_DISK: {
            LARGE_INTEGER size = {};
            LARGE_INTEGER position = {};

            if (::GetFileSizeEx(handle, &size) == FALSE) {
                return false;
            }

            position.QuadPart = 0;

            if (::SetFilePointerEx(handle, position, &position, FILE_CURRENT) == FALSE) {
                return false;
            }

            return size.QuadPart > position.QuadPart;
        }
        case FILE_TYPE_PIPE: {
            DWORD available = 0;

            if (::PeekNamedPipe(handle, nullptr, 0, nullptr, &available,
                nullptr) == FALSE) {
                return false;
            }

            return available > 0;
        }
        default:
            return false;
        }
    }

    std::size_t stack_headroom_bytes() noexcept {
        ULONG_PTR low = 0;
        ULONG_PTR high = 0;
        ::GetCurrentThreadStackLimits(&low, &high);

        if (low == 0 || high == 0 || high <= low) {
            return kStackHeadroomUnknown;
        }

        char marker = 0;
        const ULONG_PTR current = reinterpret_cast<ULONG_PTR>(&marker);

        if (current <= low || current >= high) {
            return 0;
        }

        return static_cast<std::size_t>(current - low);
    }

    bool stack_headroom_available(std::size_t required_bytes) noexcept {
        return stack_headroom_bytes() >= required_bytes;
    }

    std::size_t stack_total_bytes() noexcept {
        ULONG_PTR low = 0;
        ULONG_PTR high = 0;
        ::GetCurrentThreadStackLimits(&low, &high);

        if (low == 0 || high == 0 || high <= low) {
            return kStackHeadroomUnknown;
        }

        return static_cast<std::size_t>(high - low);
    }

    namespace {
        LONG WINAPI stack_guard_handler(EXCEPTION_POINTERS* info) {
            if (info == nullptr || info->ExceptionRecord == nullptr ||
                info->ExceptionRecord->ExceptionCode != EXCEPTION_STACK_OVERFLOW) {
                return EXCEPTION_CONTINUE_SEARCH;
            }

            ::_resetstkoflw();

            HANDLE handle = ::GetStdHandle(STD_ERROR_HANDLE);

            if (handle != nullptr && handle != INVALID_HANDLE_VALUE) {
                DWORD written = 0;
                ::WriteFile(handle, kStackOverflowMessage,
                    static_cast<DWORD>(sizeof(kStackOverflowMessage) - 1), &written,
                    nullptr);
                ::WriteFile(handle, "\n", 1, &written, nullptr);
            }

            ::ExitProcess(static_cast<UINT>(exit_failure_code()));
            return EXCEPTION_CONTINUE_SEARCH;
        }
    }

    bool install_stack_guard() noexcept {
        static void* registered =
            ::AddVectoredExceptionHandler(1, stack_guard_handler);
        return registered != nullptr;
    }

    int exit_success_code() noexcept { return 0; }

    int exit_failure_code() noexcept { return 1; }

    bool read_file(const std::string& path, std::string& out) {
        std::ifstream in(to_wide(path), std::ios::binary);

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
        std::ofstream output(to_wide(path),
            std::ios::binary | std::ios::trunc);

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
        return gallt_mode;
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
        const std::error_code code(system_error_value,
            std::system_category());
        if (!code) { return static_cast<int>(FileError::None); }

        if (code == std::errc::no_such_file_or_directory ||
            code == std::errc::no_such_process) {
            return static_cast<int>(FileError::NotFound);
        }

        if (code == std::errc::permission_denied ||
            code == std::errc::operation_not_permitted) {
            return static_cast<int>(FileError::Permission);
        }

        if (code == std::errc::file_exists) {
            return static_cast<int>(FileError::AlreadyExists);
        }

        if (code == std::errc::no_space_on_device ||
            code == std::errc::not_enough_memory) {
            return static_cast<int>(FileError::DiskFull);
        }

        return static_cast<int>(FileError::Unknown);
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

        HMODULE module = ::LoadLibraryW(to_wide(path).c_str());

        if (module == nullptr) {
            const DWORD last_error = ::GetLastError();
            set_last_file_error(last_error == ERROR_FILE_NOT_FOUND ||
                last_error == ERROR_PATH_NOT_FOUND ||
                last_error == ERROR_MOD_NOT_FOUND
                ? static_cast<int>(FileError::NotFound)
                : static_cast<int>(FileError::Unknown));
            return nullptr;
        }

        set_last_file_error(static_cast<int>(FileError::None));
        return reinterpret_cast<void*>(module);
    }

    void close_library(void* handle) noexcept {
        if (handle != nullptr) {
            ::FreeLibrary(reinterpret_cast<HMODULE>(handle));
        }
    }

    void* find_symbol(void* handle, const std::string& name) {
        if (handle == nullptr || name.empty()) { return nullptr; }
        FARPROC symbol = ::GetProcAddress(reinterpret_cast<HMODULE>(handle),
            name.c_str());
        return reinterpret_cast<void*>(symbol);
    }

    int run_process(const std::string& executable,
        const std::vector<std::string>& arguments,
        std::string* captured_output) {
        const std::wstring wide_executable = to_wide(executable);
        std::wstring command = L"\"" + wide_executable + L"\"";

        for (const std::string& argument : arguments) {
            std::wstring wide_argument = to_wide(argument);
            command += L' ';

            if (wide_argument.find(L' ') != std::wstring::npos ||
                wide_argument.find(L'\t') != std::wstring::npos) {
                command += L"\"" + wide_argument + L"\"";
            } else {
                command += wide_argument;
            }
        }

        std::vector<wchar_t> buffer(command.begin(), command.end());
        buffer.push_back(L'\0');

        HANDLE read_pipe = nullptr;
        HANDLE write_pipe = nullptr;
        SECURITY_ATTRIBUTES sa{ sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE };

        if (captured_output != nullptr) {
            if (!::CreatePipe(&read_pipe, &write_pipe, &sa, 0)) {
                return kProcessSpawnFailure;
            }

            ::SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);
        }

        STARTUPINFOW si{ sizeof(STARTUPINFOW) };
        PROCESS_INFORMATION pi{};
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = write_pipe ? write_pipe : ::GetStdHandle(STD_OUTPUT_HANDLE);
        si.hStdError = write_pipe ? write_pipe : ::GetStdHandle(STD_ERROR_HANDLE);
        si.hStdInput = ::GetStdHandle(STD_INPUT_HANDLE);

        BOOL ok = ::CreateProcessW(wide_executable.c_str(), buffer.data(),
            nullptr, nullptr, TRUE,
            CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
        if (write_pipe) { ::CloseHandle(write_pipe); }

        if (!ok) {
            if (read_pipe) { ::CloseHandle(read_pipe); }
            return kProcessSpawnFailure;
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

}
}

#endif
