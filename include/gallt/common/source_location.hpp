#ifndef GALLT_COMMON_SOURCE_LOCATION_HPP
#define GALLT_COMMON_SOURCE_LOCATION_HPP

#include <string_view>
#include <cstddef>

namespace gallt {

    struct SourceLocation {
        std::string_view filename;
        std::size_t line = 1;
        std::size_t column = 1;

        static constexpr SourceLocation empty() noexcept {
            return SourceLocation{};
        }

        constexpr bool is_valid() const noexcept {
            return line > 0 || column > 0;
        }
    };

}

#endif
