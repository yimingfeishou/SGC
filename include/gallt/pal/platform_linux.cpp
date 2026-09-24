#include "platform.hpp"

#if SGC_PLATFORM_LINUX

#include <cerrno>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <thread>
#include <unistd.h>

namespace gallt {
namespace pal {

    namespace {
        int g_last_file_error = 0;

        void set_last_file_error(int code) noexcept {
            g_last_file_error = code;
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
        return "linux: path and file operations use standard C++ facilities and "
            "are complete; process spawning and dynamic library loading are "
            "placeholders that report failure instead of loading or running "
            "anything, so this preview cannot link Gallt programs on linux";
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
        (void)path;
        set_last_file_error(static_cast<int>(FileError::Unknown));
        return nullptr;
    }

    void close_library(void* handle) noexcept {
        (void)handle;
    }

    void* find_symbol(void* handle, const std::string& name) {
        (void)handle;
        (void)name;
        return nullptr;
    }

    int run_process(const std::string& executable,
        const std::vector<std::string>& arguments,
        std::string* captured_output) {
        (void)executable;
        (void)arguments;

        if (captured_output != nullptr) {
            *captured_output += platform_support_notes();
            *captured_output += "\n";
        }

        return kProcessSpawnFailure;
    }

}
}

#endif
