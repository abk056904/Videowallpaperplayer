#include "util/utf8.h"

#include <windows.h>

namespace vw::util {

std::string wideToUtf8(const std::wstring& s) {
    if (s.empty()) return {};
    const int needed = ::WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()),
                                             nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return {};
    std::string out(static_cast<size_t>(needed), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), needed,
                          nullptr, nullptr);
    return out;
}

Result<std::wstring> utf8ToWide(const std::string& s) {
    if (s.empty()) return std::wstring{};
    const int needed = ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(),
                                             static_cast<int>(s.size()), nullptr, 0);
    if (needed <= 0) {
        return std::unexpected(L"invalid UTF-8 input");
    }
    std::wstring out(static_cast<size_t>(needed), L'\0');
    ::MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()),
                          out.data(), needed);
    return out;
}

} // namespace vw::util
