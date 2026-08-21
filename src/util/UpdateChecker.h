#pragma once

#include <atomic>
#include <string>
#include <thread>

// Queries GitHub releases API on a background thread to check for updates.
// Call start() after paths are initialized; poll result() periodically.
// Non-blocking: the HTTP request runs on a dedicated thread.
namespace vw::util {

class UpdateChecker {
public:
    // GitHub repo owner/name for the releases API.
    static constexpr const wchar_t* kRepoOwner = L"abk056904";
    static constexpr const wchar_t* kRepoName = L"Videowallpaperplayer";

    // Current app version (from app.rc: 1.0.0.0).
    static constexpr const wchar_t* kCurrentVersion = L"1.0.0.0";

    UpdateChecker() = default;
    ~UpdateChecker();

    UpdateChecker(const UpdateChecker&) = delete;
    UpdateChecker& operator=(const UpdateChecker&) = delete;

    // Start the background check. Safe to call multiple times (no-op if running).
    void start();

    // Check result (thread-safe).
    bool isUpdateAvailable() const { return updateAvailable_; }
    const std::wstring& latestVersion() const { return latestVersion_; }
    const std::wstring& downloadUrl() const { return downloadUrl_; }
    bool isCheckComplete() const { return checkComplete_; }

private:
    void checkThreadFunc();

    std::thread thread_;
    std::atomic<bool> checkComplete_{false};
    std::atomic<bool> updateAvailable_{false};
    std::wstring latestVersion_;
    std::wstring downloadUrl_;
};

} // namespace vw::util
