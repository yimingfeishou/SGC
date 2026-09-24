#include "platform.hpp"
#include <filesystem>
#include <system_error>

namespace fs = std::filesystem;

namespace gallt {
namespace pal {

    char path_separator() noexcept {
#if SGC_PLATFORM_WINDOWS
        return '\\';
#else
        return '/';
#endif
    }

    const char* preferred_library_extension() noexcept {
#if SGC_PLATFORM_WINDOWS
        return ".dll";
#else
        return ".so";
#endif
    }

    const char* static_library_extension() noexcept {
#if SGC_PLATFORM_WINDOWS
        return ".lib";
#else
        return ".a";
#endif
    }

    std::vector<std::string> library_probe_extensions() {
        std::vector<std::string> out;
        out.push_back(std::string(static_library_extension()));
        out.push_back(std::string(preferred_library_extension()));
        return out;
    }

    std::string replace_extension(const std::string& path,
        const std::string& ext) {
        fs::path p(to_wide(path));
        std::wstring extension_text = ext.empty()
            ? std::wstring()
            : to_wide(ext.front() == '.' ? ext : ("." + ext));
        p.replace_extension(extension_text);
        return to_utf8(p.wstring());
    }

    bool is_absolute_path(const std::string& path) {
        if (path.empty()) { return false; }
        fs::path p(to_wide(path));
        return p.is_absolute();
    }

    bool is_relative_path(const std::string& path) {
        if (path.empty()) { return true; }
        fs::path p(to_wide(path));
        return p.is_relative();
    }

    std::string join_path(const std::string& base, const std::string& relative) {
        if (base.empty()) { return relative; }
        if (relative.empty()) { return base; }
        fs::path joined(to_wide(base));
        joined /= fs::path(to_wide(relative));
        return to_utf8(joined.wstring());
    }

    std::string parent_path(const std::string& path) {
        fs::path p(to_wide(path));
        return to_utf8(p.parent_path().wstring());
    }

    std::string file_name(const std::string& path) {
        fs::path p(to_wide(path));
        return to_utf8(p.filename().wstring());
    }

    std::string extension(const std::string& path) {
        fs::path p(to_wide(path));
        return to_utf8(p.extension().wstring());
    }

    std::string normalize_path(const std::string& path) {
        std::string result = path;

        for (char& c : result) {
            if (c == '\\') { c = '/'; }
        }

        return result;
    }

    std::string canonical_path(const std::string& path) {
        std::error_code ec;
        fs::path p(to_wide(path));
        fs::path canonical = fs::weakly_canonical(p, ec);
        if (ec) { return path; }
        return to_utf8(canonical.wstring());
    }

    std::string current_working_directory() {
        std::error_code ec;
        fs::path cwd = fs::current_path(ec);
        if (ec) { return std::string(); }
        return to_utf8(cwd.wstring());
    }

    bool file_exists(const std::string& path) {
        if (path.empty()) { return false; }
        std::error_code ec;
        return fs::exists(fs::path(to_wide(path)), ec) && !ec;
    }

    bool directory_exists(const std::string& path) {
        if (path.empty()) { return false; }
        std::error_code ec;
        return fs::is_directory(fs::path(to_wide(path)), ec) && !ec;
    }

    long long file_size(const std::string& path) {
        std::error_code ec;
        std::uintmax_t size = fs::file_size(fs::path(to_wide(path)), ec);
        if (ec) { return -1; }
        return static_cast<long long>(size);
    }

    bool remove_file(const std::string& path) {
        std::error_code ec;
        return fs::remove(fs::path(to_wide(path)), ec) && !ec;
    }

    bool rename_file(const std::string& from, const std::string& to) {
        std::error_code ec;
        fs::rename(fs::path(to_wide(from)), fs::path(to_wide(to)), ec);
        return !ec;
    }

    bool copy_file(const std::string& from, const std::string& to) {
        std::error_code ec;
        fs::copy_file(fs::path(to_wide(from)), fs::path(to_wide(to)),
            fs::copy_options::overwrite_existing, ec);
        return !ec;
    }

    bool create_directory(const std::string& path) {
        std::error_code ec;
        fs::create_directory(fs::path(to_wide(path)), ec);
        return !ec;
    }

    bool remove_directory(const std::string& path) {
        std::error_code ec;
        fs::remove(fs::path(to_wide(path)), ec);
        return !ec;
    }

}
}
