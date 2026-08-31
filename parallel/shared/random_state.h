#ifndef KAHIP_PARALLEL_SHARED_RANDOM_STATE_H
#define KAHIP_PARALLEL_SHARED_RANDOM_STATE_H

#include <random>

namespace kahip::random_compat {
using engine_type = std::mt19937;

// The pinned upstream ParHIP and modified KaHIP objects resolve to one global
// random_functions state.  Keep that behavioral contract while retaining the
// branch's component namespaces.
inline int seed = 0;
inline engine_type engine;
}  // namespace kahip::random_compat

#endif
