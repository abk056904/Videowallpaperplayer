#pragma once

#include <expected>
#include <string>

namespace vw {

// Lightweight error propagation for fallible operations (C++23 std::expected).
// `Result<T>` carries a value or a wide-string error message. Never silently
// swallow errors at call sites.
template <typename T>
using Result = std::expected<T, std::wstring>;

} // namespace vw
