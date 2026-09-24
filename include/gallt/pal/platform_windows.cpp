#include "platform.hpp"

#if SGC_PLATFORM_WINDOWS

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <cerrno>
#include <fstream>
#include <sstream>
#include <system_error>

namespace gallt {
namespace pal {

    namespace {
        int g_last_file_error = 0;

        void set_last_file_error(int code) noexcept {
            g_last_file_error = code;
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
