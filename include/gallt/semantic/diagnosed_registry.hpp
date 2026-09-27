#ifndef GALLT_SEMANTIC_DIAGNOSED_REGISTRY_HPP
#define GALLT_SEMANTIC_DIAGNOSED_REGISTRY_HPP

#include "../common/diagnostics.hpp"
#include <cstddef>
#include <tuple>

namespace gallt {

    // Compiler-internal stage identifiers used by the cross-stage diagnosed
    // registry below. Values follow the order in which the compiler runs the
    // stages, so "earlier" is a strict comparison of these values.
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

        inline std::tuple<std::string_view, std::size_t, std::size_t>
            location_key(SourceLocation loc) {
            return std::make_tuple(loc.filename, loc.line, loc.column);
        }

        // Records that this stage diagnosed this location (keeping the earliest
        // stage if several stages report at the same location).
        inline void record(DiagnosticEngine& diag, SourceLocation loc, SemanticStage stage) {
            if (!usable_location(loc)) { return; }

            const int value = static_cast<int>(stage);
            const auto key = location_key(loc);
            auto found = diag.diagnosed_stages.find(key);

            if (found == diag.diagnosed_stages.end() || found->second > value) {
                diag.diagnosed_stages[key] = value;
            }
        }

        // True when a stage strictly earlier than this one already diagnosed the
        // location, i.e. the current stage would only repeat a derived diagnostic.
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
