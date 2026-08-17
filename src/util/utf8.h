#pragma once

#include <string>

#include "util/Result.h"

namespace vw::util {

// Explicit UTF-8 <-> UTF-16 conversion (CP_UTF8) for all file I/O.
//
// Do NOT use std::wfstream/std::wifstream for files: on MSVC the write path
// converts wchar_t to the system ANSI codepage (dropping chars > 0xFF) while
// the read path decodes UTF-8 — an asymmetric pairing that corrupts any
// non-ASCII content (see BUILD_NOTES "M1 gotcha #10"). Files are therefore
// byte streams (std::ofstream/std::ifstream) with these helpers.

// Wide string -> UTF-8 bytes (no error: unpaired surrogates become '?').
std::string wideToUtf8(const std::wstring& s);

// UTF-8 bytes -> wide string. Fails on invalid UTF-8 (callers treat that as
// corrupt-file recovery input, matching the strict-validation policy).
Result<std::wstring> utf8ToWide(const std::string& s);

} // namespace vw::util
