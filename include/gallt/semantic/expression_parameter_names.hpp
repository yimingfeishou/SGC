#ifndef GALLT_SEMANTIC_EXPRESSION_PARAMETER_NAMES_HPP
#define GALLT_SEMANTIC_EXPRESSION_PARAMETER_NAMES_HPP

#include <string_view>

namespace gallt {

    namespace expression_parameter_names {

        inline constexpr std::string_view kTemporaryPrefix = "__glt_expr";
        inline constexpr std::string_view kLocalRenameSuffix = "$expr";

        inline bool has_temporary_prefix(std::string_view name) noexcept {
            return !name.empty() && name.rfind(kTemporaryPrefix, 0) == 0;
        }

        inline bool has_local_rename_suffix(std::string_view name) noexcept {
            return !name.empty() && name.find(kLocalRenameSuffix) != std::string_view::npos;
        }

        inline bool is_generated_name(std::string_view name) noexcept {
            return has_temporary_prefix(name) || has_local_rename_suffix(name);
        }

    }

}

#endif
