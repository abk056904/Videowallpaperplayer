#pragma once

#include <cstddef>

namespace vw::util {

// Cast a size_t index to ptrdiff_t for use with vector iterator arithmetic.
// Prevents the noisy `static_cast<ptrdiff_t>(index)` scattered across
// PlaylistManager and LibraryManager.
inline ptrdiff_t idx(std::size_t i) {
    return static_cast<ptrdiff_t>(i);
}

} // namespace vw::util
