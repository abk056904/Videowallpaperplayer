#pragma once

#include <filesystem>
#include <string>

// Crash reporting via Windows Minidump.
// Call install() once at process startup (after paths are initialized).
// On unhandled exception, a .dmp file is written to crashesDir and the
// process terminates. Old dumps (>maxAge) are auto-cleaned on install().
namespace vw::util {

class CrashReport {
public:
    // Install the unhandled-exception filter. Writes minidumps to crashesDir.
    // Automatically cleans dumps older than maxAgeDays on install.
    static void install(const std::filesystem::path& crashesDir, int maxAgeDays = 7);
};

} // namespace vw::util
