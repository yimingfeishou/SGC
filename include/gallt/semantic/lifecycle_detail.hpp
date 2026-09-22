#ifndef GALLT_SEMANTIC_LIFECYCLE_DETAIL_HPP
#define GALLT_SEMANTIC_LIFECYCLE_DETAIL_HPP

#include "lifecycle.hpp"
#include <algorithm>
#include <cstdlib>
#include <cstdio>
#include <functional>

namespace gallt {
    using namespace AST;

    namespace lifecycle_detail {
        constexpr int kArgumentRankUnknown = 1 << 20;

    }
}

#endif
