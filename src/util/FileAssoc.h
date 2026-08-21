#pragma once

#include <string>
#include <vector>

// File association registration using HKCU (no admin required).
// Registers video file types (.mp4, .mkv, etc.) to open with this app.
namespace vw::util {

class FileAssoc {
public:
    // Register file associations for the given exe path.
    // Returns true on success. Safe to call multiple times (idempotent).
    static bool registerAll(const std::wstring& exePath);

    // Unregister all file associations.
    static bool unregisterAll();

    // Check if file associations are currently registered.
    static bool isRegistered();

    // Video file extensions we support.
    static const std::vector<std::wstring>& extensions();
};

} // namespace vw::util
