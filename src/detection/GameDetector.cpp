#include "detection/GameDetector.h"

#include <algorithm>


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

GameClass GameDetector::classifyHeuristic(const std::wstring& exeName,
                                           const std::wstring& processPath,
                                           const std::wstring& windowClass) {
    // 1. Window class heuristic: known game engine window classes.
    //    These are definitive — Unity/Unreal/GLFW/SDL are ONLY used by games.
    static const wchar_t* kGameWindowClasses[] = {
        // Unity
        L"UnityWndClass", L"UnityContainerWndClass",
        // Unreal Engine
        L"UnrealWindow",
        // GLFW
        L"GLFW30", L"GLFW31", L"GLFW32", L"GLFW33", L"GLFW34",
        L"GLFWw30", L"GLFWw31", L"GLFWw32", L"GLFWw33",
        // SDL
        L"SDL_app",
        // Godot
        L"Engine",
        // Source Engine (Valve)
        L"Valve001",
        // CryEngine
        L"CryENGINE",
        // Custom game window classes (DX/Vulkan exclusive fullscreen)
        L"GameWindow",
    };
    if (!windowClass.empty()) {
        const std::wstring clsLower = toLower(windowClass);
        for (const auto* gc : kGameWindowClasses) {
            if (clsLower == gc) {
                return GameClass::Game;
            }
        }
    }

    // 2. Process path heuristic: check for known game store directories.
    //    A process under Steam/EPIC/GOG/Origin is very likely a game.
    const std::wstring pathLower = toLower(processPath);
    static const wchar_t* kGamePathPatterns[] = {
        L"\\steam\\",
        L"\\steamapps\\common\\",
        L"\\epic games\\",
        L"\\epicgames\\",
        L"\\gog games\\",
        L"\\origin\\",
        L"\\ea games\\",
        L"\\ubisoft\\",
        L"\\battle.net\\",
        L"\\blizzard\\",
        L"\\riot games\\",
        L"\\miHoYo\\",
        L"\\hoYoverse\\",
        L"\\rockstar games\\",
        L"\\bethesda\\",
    };
    for (const auto* pat : kGamePathPatterns) {
        if (pathLower.find(pat) != std::wstring::npos) {
            return GameClass::Game;
        }
    }

    // 3. Process name heuristic: common game executables / launchers.
    //    Only match if the process is a KNOWN game executable pattern,
    //    not a generic name like "engine" (which could be Godot or not).
    const std::wstring name = toLower(baseName(exeName));
    static const wchar_t* kGameExePatterns[] = {
        L"game",        L"client",       L"launcher",
        L"unity",       L"unreal",       L"godot",
    };
    for (const auto* pat : kGameExePatterns) {
        if (name.find(pat) != std::wstring::npos) {
            // Extra safety: only match if the path contains a game store dir
            // to avoid false positives (e.g. "client" in a browser).
            for (const auto* pp : kGamePathPatterns) {
                if (pathLower.find(pp) != std::wstring::npos) {
                    return GameClass::Game;
                }
            }
        }
    }

    return GameClass::Unknown;
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

GameState GameDetector::updateForeground(DWORD pid, const std::wstring& windowClass,
                                           PathLookup lookup, AliveCheck alive) {
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
    state_.heuristicMatched = false;
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

    // If the lists didn't match, try heuristic detection (window class + path).
    if (state_.classification == GameClass::Unknown) {
        state_.classification = classifyHeuristic(state_.exeName, state_.processPath, windowClass);
        state_.heuristicMatched = state_.classification != GameClass::Unknown;
    }

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
