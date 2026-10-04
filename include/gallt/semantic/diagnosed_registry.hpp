#ifndef GALLT_SEMANTIC_DIAGNOSED_REGISTRY_HPP
#define GALLT_SEMANTIC_DIAGNOSED_REGISTRY_HPP

#include "../common/diagnostics.hpp"
#include <cstddef>
#include <tuple>

namespace gallt {

    enum class SemanticStage : int {
        Conditions = 1,
        Namespaces = 2,
        Generics = 3,
        Lifecycle = 4,
        TypeCheck = 5,
    };

    namespace diagnosed_registry {

        inline bool usable_location(SourceLocation loc) {
            return !loc.filename.empty();
        }

        inline std::tuple<std::string, std::size_t, std::size_t>
            location_key(SourceLocation loc) {
            return std::make_tuple(loc.filename, loc.line, loc.column);
        }

        inline void record(DiagnosticEngine& diag, SourceLocation loc, SemanticStage stage) {
            if (!usable_location(loc)) { return; }

            const int value = static_cast<int>(stage);
            const auto key = location_key(loc);
            auto found = diag.diagnosed_stages.find(key);

            if (found == diag.diagnosed_stages.end() || found->second > value) {
                diag.diagnosed_stages[key] = value;
            }
        }

        inline bool reported_by_earlier_stage(const DiagnosticEngine& diag,
            SourceLocation loc, SemanticStage stage) {
            if (!usable_location(loc)) { return false; }

            auto found = diag.diagnosed_stages.find(location_key(loc));
            return found != diag.diagnosed_stages.end() &&
                found->second < static_cast<int>(stage);
        }

    }

}

#endif
