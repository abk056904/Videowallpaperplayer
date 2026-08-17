#include "detection/GameDetector.h"

#include <algorithm>

#include <psapi.h>

namespace vw::detection {

namespace {

std::wstring toLower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(::towlower(c)); });
    return s;
}

std::wstring baseName(const std::wstring& path) {
    const auto pos = path.find_last_of(L"\\/");
    return pos == std::wstring::npos ? path : path.substr(pos + 1);
}

// Normalized list entry: basename, lowercased (matching the exe-name side).
std::wstring normalizeEntry(const std::wstring& s) {
    return toLower(baseName(s));
}

} // namespace

void GameDetector::setLists(std::vector<std::wstring> alwaysPause,
                            std::vector<std::wstring> neverPause) {
    alwaysPause_.clear();
    neverPause_.clear();
    for (auto& s : alwaysPause) {
        alwaysPause_.push_back(normalizeEntry(s));
    }
    for (auto& s : neverPause) {
        neverPause_.push_back(normalizeEntry(s));
    }
    reset();
}

GameClass GameDetector::classify(const std::wstring& exeName,
                                 const std::vector<std::wstring>& alwaysPause,
                                 const std::vector<std::wstring>& neverPause) {
    const std::wstring name = toLower(baseName(exeName));
    if (name.empty()) {
        return GameClass::Unknown;
    }
    // Deny (never pause) wins over allow (always pause). Entries are
    // normalized the same way as the exe name (basename, lowercase).
    const bool denied = std::any_of(neverPause.begin(), neverPause.end(),
                                    [&](const std::wstring& e) { return normalizeEntry(e) == name; });
    if (denied) {
        return GameClass::NotGame;
    }
    const bool allowed = std::any_of(alwaysPause.begin(), alwaysPause.end(),
                                     [&](const std::wstring& e) {
                                         return normalizeEntry(e) == name;
                                     });
    if (allowed) {
        return GameClass::Game;
    }
    return GameClass::Unknown; // no list match — the policy layer decides
}

std::optional<std::wstring> GameDetector::defaultPathLookup(DWORD pid) {
    if (pid == 0) {
        return std::nullopt;
    }
    HANDLE proc = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!proc) {
        return std::nullopt;
    }
    wchar_t path[MAX_PATH]{};
    DWORD size = MAX_PATH;
    const BOOL ok = ::QueryFullProcessImageNameW(proc, 0, path, &size);
    ::CloseHandle(proc);
    if (!ok || size == 0) {
        return std::nullopt;
    }
    return std::wstring(path, size);
}

GameState GameDetector::updateForeground(DWORD pid, PathLookup lookup, AliveCheck alive) {
    state_.pid = pid;

    // Cache hit: same pid as the last classification AND the process is still
    // alive — keep everything (process path, classification), no scan. If the
    // cached process EXITED, the pid may have been reused by a different
    // process: re-lookup (liveness is the cheap check that catches the reuse).
    if (pid != 0 && pid == cachedPid_ && alive && alive(pid)) {
        return state_;
    }

    // New pid (or pid 0 = no foreground): fresh lookup + classification.
    cachedPid_ = pid;
    state_.processPath.clear();
    state_.exeName.clear();
    state_.classification = GameClass::Unknown;
    state_.listMatched = false;
    if (pid == 0) {
        return state_;
    }
    auto path = lookup ? lookup(pid) : std::nullopt;
    if (!path) {
        return state_;
    }
    state_.processPath = std::move(*path);
    state_.exeName = toLower(baseName(state_.processPath));
    state_.classification = classify(state_.exeName, alwaysPause_, neverPause_);
    state_.listMatched = state_.classification != GameClass::Unknown;
    return state_;
}

bool GameDetector::isAlive(DWORD pid) {
    if (pid == 0) {
        return false;
    }
    HANDLE proc = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!proc) {
        return false;
    }
    DWORD code = 0;
    const bool alive = ::GetExitCodeProcess(proc, &code) != FALSE && code == STILL_ACTIVE;
    ::CloseHandle(proc);
    return alive;
}

void GameDetector::reset() {
    state_ = {};
    cachedPid_ = 0;
}

} // namespace vw::detection
