#include "util/UpdateChecker.h"

#include <windows.h>
#include <winhttp.h>

#pragma comment(lib, "winhttp.lib")

#include "logging/Logger.h"
#include "util/json.h"
#include "util/utf8.h"

namespace vw::util {

UpdateChecker::~UpdateChecker() {
    if (thread_.joinable()) {
        thread_.join();
    }
}

void UpdateChecker::start() {
    if (checkComplete_ || thread_.joinable()) return;
    thread_ = std::thread(&UpdateChecker::checkThreadFunc, this);
}

namespace {

// Compare two version strings like "1.0.0" lexicographically.
// Returns -1, 0, or 1.
int compareVersions(const std::wstring& a, const std::wstring& b) {
    auto split = [](const std::wstring& v) -> std::vector<int> {
        std::vector<int> parts;
        size_t start = 0;
        while (start < v.size()) {
            size_t pos = v.find(L'.', start);
            if (pos == std::wstring::npos) pos = v.size();
            parts.push_back(std::stoi(v.substr(start, pos - start)));
            start = pos + 1;
        }
        return parts;
    };
    auto pa = split(a);
    auto pb = split(b);
    size_t len = pa.size() > pb.size() ? pa.size() : pb.size();
    for (size_t i = 0; i < len; ++i) {
        int ai = (i < pa.size()) ? pa[i] : 0;
        int bi = (i < pb.size()) ? pb[i] : 0;
        if (ai < bi) return -1;
        if (ai > bi) return 1;
    }
    return 0;
}

} // anonymous namespace

void UpdateChecker::checkThreadFunc() {
    auto& log = vw::log::Logger::instance();

    // Build the GitHub API URL.
    const std::wstring host = L"api.github.com";
    const std::wstring path = L"/repos/" + std::wstring(kRepoOwner) + L"/" + std::wstring(kRepoName) + L"/releases/latest";

    HINTERNET hSession = ::WinHttpOpen(L"VideoWallpaper/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) { checkComplete_ = true; return; }

    HINTERNET hConnect = ::WinHttpConnect(hSession, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConnect) { ::WinHttpCloseHandle(hSession); checkComplete_ = true; return; }

    HINTERNET hRequest = ::WinHttpOpenRequest(hConnect, L"GET", path.c_str(),
                                               nullptr, WINHTTP_NO_REFERER,
                                               WINHTTP_DEFAULT_ACCEPT_TYPES,
                                               WINHTTP_FLAG_SECURE);
    if (!hRequest) {
        ::WinHttpCloseHandle(hConnect);
        ::WinHttpCloseHandle(hSession);
        checkComplete_ = true;
        return;
    }

    // Set timeout (5 seconds).
    DWORD timeout = 5000;
    ::WinHttpSetOption(hRequest, WINHTTP_OPTION_RECEIVE_TIMEOUT, &timeout, sizeof(timeout));

    BOOL sent = ::WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                      WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
    if (!sent) {
        ::WinHttpCloseHandle(hRequest);
        ::WinHttpCloseHandle(hConnect);
        ::WinHttpCloseHandle(hSession);
        checkComplete_ = true;
        return;
    }

    BOOL received = ::WinHttpReceiveResponse(hRequest, nullptr);
    if (!received) {
        ::WinHttpCloseHandle(hRequest);
        ::WinHttpCloseHandle(hConnect);
        ::WinHttpCloseHandle(hSession);
        checkComplete_ = true;
        return;
    }

    // Read response.
    std::string responseBody;
    DWORD bytesAvailable = 0;
    while (::WinHttpQueryDataAvailable(hRequest, &bytesAvailable) && bytesAvailable > 0) {
        std::vector<char> buf(bytesAvailable);
        DWORD bytesRead = 0;
        if (::WinHttpReadData(hRequest, buf.data(), bytesAvailable, &bytesRead)) {
            responseBody.append(buf.data(), bytesRead);
        }
        bytesAvailable = 0;
    }

    ::WinHttpCloseHandle(hRequest);
    ::WinHttpCloseHandle(hConnect);
    ::WinHttpCloseHandle(hSession);

    if (responseBody.empty()) {
        checkComplete_ = true;
        return;
    }

    // Parse JSON response (narrow → wide for the project's JSON parser).
    auto wideBody = utf8ToWide(responseBody);
    if (!wideBody) {
        checkComplete_ = true;
        return;
    }

    auto parsed = Json::parse(*wideBody);
    if (!parsed) {
        log.debug(L"update checker: JSON parse failed");
        checkComplete_ = true;
        return;
    }

    // Extract tag_name (e.g. "v1.1.0") and html_url (browser link).
    const auto& tagName = parsed->get(L"tag_name");
    const auto& htmlUrl = parsed->get(L"html_url");
    if (!tagName.isString() || tagName.asString().empty()) {
        checkComplete_ = true;
        return;
    }

    latestVersion_ = tagName.asString();
    // Strip leading 'v' if present (e.g. "v1.1.0" → "1.1.0").
    if (!latestVersion_.empty() && latestVersion_[0] == L'v') {
        latestVersion_ = latestVersion_.substr(1);
    }
    if (htmlUrl.isString()) {
        downloadUrl_ = htmlUrl.asString();
    }

    if (compareVersions(latestVersion_, kCurrentVersion) > 0) {
        updateAvailable_ = true;
        log.info(L"update available: {} (current: {})", latestVersion_, kCurrentVersion);
    } else {
        log.debug(L"no update available (current: {}, latest: {})", kCurrentVersion, latestVersion_);
    }

    checkComplete_ = true;
}

} // namespace vw::util
