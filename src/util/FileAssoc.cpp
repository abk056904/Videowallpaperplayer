#include "util/FileAssoc.h"

#include <windows.h>

#include "logging/Logger.h"

namespace vw::util {

const std::vector<std::wstring>& FileAssoc::extensions() {
    static const std::vector<std::wstring> kExtensions = {
        L".mp4", L".mkv", L".avi", L".mov", L".webm",
        L".m4v", L".ts", L".mpg", L".mpeg", L".wmv",
    };
    return kExtensions;
}

namespace {

const wchar_t* contentTypeForExt(const std::wstring& ext) {
    if (ext == L".mp4" || ext == L".m4v") return L"video/mp4";
    if (ext == L".mkv") return L"video/x-matroska";
    if (ext == L".avi") return L"video/x-msvideo";
    if (ext == L".mov") return L"video/quicktime";
    if (ext == L".webm") return L"video/webm";
    if (ext == L".ts") return L"video/mp2t";
    if (ext == L".mpg" || ext == L".mpeg") return L"video/mpeg";
    if (ext == L".wmv") return L"video/x-ms-wmv";
    return L"video/mp4";
}

bool setRegString(HKEY root, const std::wstring& subKey,
                  const std::wstring& valueName, const std::wstring& value) {
    HKEY hKey = nullptr;
    LONG res = ::RegCreateKeyExW(root, subKey.c_str(), 0, nullptr,
                                  REG_OPTION_NON_VOLATILE, KEY_SET_VALUE,
                                  nullptr, &hKey, nullptr);
    if (res != ERROR_SUCCESS) return false;
    res = ::RegSetValueExW(hKey, valueName.c_str(), 0, REG_SZ,
                            reinterpret_cast<const BYTE*>(value.c_str()),
                            static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    ::RegCloseKey(hKey);
    return res == ERROR_SUCCESS;
}

std::wstring getRegString(HKEY root, const std::wstring& subKey,
                          const std::wstring& valueName) {
    HKEY hKey = nullptr;
    if (::RegOpenKeyExW(root, subKey.c_str(), 0, KEY_READ, &hKey) != ERROR_SUCCESS) return {};
    wchar_t buf[512]{};
    DWORD bufSize = sizeof(buf);
    DWORD type = 0;
    LONG res = ::RegQueryValueExW(hKey, valueName.c_str(), nullptr, &type,
                                   reinterpret_cast<LPBYTE>(buf), &bufSize);
    ::RegCloseKey(hKey);
    if (res != ERROR_SUCCESS || type != REG_SZ) return {};
    return buf;
}

bool deleteRegKeyRecursive(HKEY root, const std::wstring& subKey) {
    HKEY hKey = nullptr;
    if (::RegOpenKeyExW(root, subKey.c_str(), 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
        wchar_t name[256];
        DWORD nameLen;
        while (true) {
            nameLen = 256;
            if (::RegEnumKeyExW(hKey, 0, name, &nameLen, nullptr,
                                nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
            std::wstring child = subKey + L"\\" + name;
            ::RegCloseKey(hKey);
            deleteRegKeyRecursive(root, child);
            hKey = nullptr;
            if (::RegOpenKeyExW(root, subKey.c_str(), 0, KEY_READ, &hKey) != ERROR_SUCCESS) break;
        }
        if (hKey) ::RegCloseKey(hKey);
    }
    return ::RegDeleteKeyW(root, subKey.c_str()) == ERROR_SUCCESS;
}

} // anonymous namespace

bool FileAssoc::registerAll(const std::wstring& exePath) {
    auto& log = vw::log::Logger::instance();
    const std::wstring classesRoot = L"Software\\Classes";
    bool allOk = true;

    for (const auto& ext : extensions()) {
        const std::wstring progId = L"VideoWallpaper" + ext;

        // .ext -> progID
        std::wstring extKey = classesRoot + ext;
        if (!setRegString(HKEY_CURRENT_USER, extKey, L"", progId)) {
            log.warn(L"file assoc: failed to set progID for {}", ext);
            allOk = false;
            continue;
        }
        const wchar_t* ct = contentTypeForExt(ext);
        if (ct) setRegString(HKEY_CURRENT_USER, extKey, L"Content Type", ct);

        // ProgID description.
        std::wstring progKey = classesRoot + L"\\" + progId;
        setRegString(HKEY_CURRENT_USER, progKey, L"", L"Video Wallpaper Video");

        // shell\\open\\command
        std::wstring cmdKey = progKey + L"\\shell\\open\\command";
        std::wstring cmd = L"\"" + exePath + L"\" \"%1\"";
        if (!setRegString(HKEY_CURRENT_USER, cmdKey, L"", cmd)) {
            log.warn(L"file assoc: failed to set command for {}", ext);
            allOk = false;
        }

        // Default icon.
        std::wstring iconKey = progKey + L"\\DefaultIcon";
        setRegString(HKEY_CURRENT_USER, iconKey, L"", L"\"" + exePath + L"\",0");
    }

    // Register in the "Open with" list.
    for (const auto& ext : extensions()) {
        std::wstring owKey = classesRoot + ext + L"\\OpenWithProgids";
        std::wstring progId = L"VideoWallpaper" + ext;
        HKEY hKey = nullptr;
        if (::RegCreateKeyExW(HKEY_CURRENT_USER, owKey.c_str(), 0, nullptr,
                               REG_OPTION_NON_VOLATILE, KEY_SET_VALUE,
                               nullptr, &hKey, nullptr) == ERROR_SUCCESS) {
            ::RegSetValueExW(hKey, progId.c_str(), 0, REG_NONE, nullptr, 0);
            ::RegCloseKey(hKey);
        }
    }

    log.info(L"file assoc: registered {} extensions", extensions().size());
    return allOk;
}

bool FileAssoc::unregisterAll() {
    auto& log = vw::log::Logger::instance();
    const std::wstring classesRoot = L"Software\\Classes";

    for (const auto& ext : extensions()) {
        std::wstring extKey = classesRoot + ext;
        std::wstring progId = L"VideoWallpaper" + ext;

        // Remove OpenWithProgids entry.
        std::wstring owPath = classesRoot + ext + L"\\OpenWithProgids";
        HKEY owKey = nullptr;
        if (::RegOpenKeyExW(HKEY_CURRENT_USER, owPath.c_str(), 0, KEY_SET_VALUE, &owKey) == ERROR_SUCCESS) {
            ::RegDeleteValueW(owKey, progId.c_str());
            ::RegCloseKey(owKey);
        }

        // Delete the progID key tree.
        std::wstring progKey = classesRoot + L"\\" + progId;
        deleteRegKeyRecursive(HKEY_CURRENT_USER, progKey);
    }

    log.info(L"file assoc: unregistered {} extensions", extensions().size());
    return true;
}

bool FileAssoc::isRegistered() {
    const std::wstring classesRoot = L"Software\\Classes";
    for (const auto& ext : extensions()) {
        std::wstring extKey = classesRoot + ext;
        std::wstring progId = getRegString(HKEY_CURRENT_USER, extKey, L"");
        if (progId == L"VideoWallpaper" + ext) return true;
    }
    return false;
}

} // namespace vw::util
