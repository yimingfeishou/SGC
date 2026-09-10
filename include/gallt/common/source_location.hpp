// common/source_location.hpp
// 源码位置追踪 —— 文件名、行号、列号
// Source location tracking — filename, line, column

#ifndef GALLT_COMMON_SOURCE_LOCATION_HPP
#define GALLT_COMMON_SOURCE_LOCATION_HPP

#include <string_view>
#include <cstddef>

namespace gallt {

    // ============================================================================
    // SourceLocation 结构体
    // 用于追踪源码中的精确位置（文件名、行号、列号）
    // Used to track precise locations in source code (filename, line, column)
    // ============================================================================

    struct SourceLocation {
        std::string_view filename;   // 文件名（不拥有所有权，指向输入文件名）
        // filename (non-owning, points to input filename)
        std::size_t line = 1;        // 行号（从1开始）
        // line number (1-based)
        std::size_t column = 1;      // 列号（从1开始）
        // column number (1-based)

// 返回一个空位置（用于无上下文的错误）
// Returns an empty location (for context-less errors)
        static constexpr SourceLocation empty() noexcept {
            return SourceLocation{};
        }

        // 检查位置是否有效（至少行号或列号大于0）
        // Checks if location is valid (at least line or column > 0)
        constexpr bool is_valid() const noexcept {
            return line > 0 || column > 0;
        }
    };

} // namespace gallt

#endif // GALLT_COMMON_SOURCE_LOCATION_HPP