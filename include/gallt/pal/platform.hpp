#ifndef GALLT_PAL_PLATFORM_HPP
#define GALLT_PAL_PLATFORM_HPP

#include <cstdint>
#include <string>
#include <vector>

#if defined(_WIN32)
#define SGC_PLATFORM_WINDOWS 1
#define SGC_PLATFORM_LINUX 0
#else
#define SGC_PLATFORM_WINDOWS 0
#define SGC_PLATFORM_LINUX 1
#endif

namespace gallt {
namespace pal {

    enum class FileError : int {
        None = 0,
        InvalidHandle = 1,
        Permission = 2,
        NotFound = 3,
        AlreadyExists = 4,
        ReadFailed = 5,
        WriteFailed = 6,
        SeekFailed = 7,
        DiskFull = 8,
        Unknown = 9,
    };

    constexpr int kProcessSpawnFailure = 127;

    const char* platform_name() noexcept;
    bool is_windows() noexcept;
    bool is_linux() noexcept;
    const char* target_triple() noexcept;
    const char* future_linux_target_triple() noexcept;
    const char* platform_support_notes() noexcept;

    char path_separator() noexcept;
    const char* preferred_library_extension() noexcept;
    const char* static_library_extension() noexcept;
    std::vector<std::string> library_probe_extensions();

    std::string to_utf8(const std::wstring& text);
    std::wstring to_wide(const std::string& text);

    std::string executable_path();
    std::string environment_variable(const std::string& name);
    bool environment_variable_defined(const std::string& name);
    std::string temporary_directory();
    int process_id() noexcept;
    std::string process_id_text();
    bool enable_utf8_console() noexcept;
    int exit_success_code() noexcept;
    int exit_failure_code() noexcept;

    bool is_absolute_path(const std::string& path);
    bool is_relative_path(const std::string& path);
    std::string join_path(const std::string& base, const std::string& relative);
    std::string parent_path(const std::string& path);
    std::string file_name(const std::string& path);
    std::string extension(const std::string& path);
    std::string replace_extension(const std::string& path, const std::string& ext);
    std::string normalize_path(const std::string& path);
    std::string canonical_path(const std::string& path);
    std::string current_working_directory();

    bool file_exists(const std::string& path);
    bool directory_exists(const std::string& path);
    long long file_size(const std::string& path);
    bool remove_file(const std::string& path);
    bool rename_file(const std::string& from, const std::string& to);
    bool copy_file(const std::string& from, const std::string& to);
    bool create_directory(const std::string& path);
    bool remove_directory(const std::string& path);
    bool read_file(const std::string& path, std::string& out);
    bool write_file(const std::string& path, const std::string& content);

    std::string file_open_mode(const std::string& gallt_mode);
    int file_error_from_errno(int errno_value) noexcept;
    int file_error_from_system_error(int system_error_value) noexcept;
    int last_file_error() noexcept;
    const char* file_error_text(int code) noexcept;

    void* load_library(const std::string& path);
    void close_library(void* handle) noexcept;
    void* find_symbol(void* handle, const std::string& name);

    int run_process(const std::string& executable,
        const std::vector<std::string>& arguments,
        std::string* captured_output);

}
}

#endif
