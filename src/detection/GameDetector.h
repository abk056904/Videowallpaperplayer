#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <windows.h>

// Game detection (docs/02 §2.9 / docs/03 §3.11, M9): inspects ONLY the
// foreground process (GetWindowThreadProcessId -> OpenProcess ->
// QueryFullProcessImageNameW) and matches its executable against the
// configured allow ("always pause") / deny ("never pause") lists. Never
// enumerates all processes; no injection, no game hooks, no admin.
//
// Classification is CACHED per (pid, path, window class) so repeated
// foreground-change events do not re-open the process; the cache invalidates
// on pid change / process exit / explicit reset (config change). The path
// extraction is a thin, injectable function (unit tests feed synthetic paths
// without a real foreground process).

namespace vw::detection {

enum class GameClass { Unknown, Game, NotGame };

struct GameState {
    DWORD pid = 0;
    std::wstring processPath; // full path, "" when inaccessible
    std::wstring exeName;     // basename, lowercased for matching
    GameClass classification = GameClass::Unknown;
    bool listMatched = false;    // true when allow/deny lists decided it
    bool heuristicMatched = false; // true when heuristic (class+path) decided it
};

class GameDetector {
public:
    // Injectable process-path lookup (tests substitute a synthetic path; the
    // default impl uses OpenProcess + QueryFullProcessImageNameW). Returns
    // nullopt when the process cannot be inspected (already exited / access
    // denied).
    using PathLookup = std::function<std::optional<std::wstring>(DWORD pid)>;

    // Allow (always pause) / deny (never pause) executable-name lists
    // (config.detection.alwaysPause / neverPause; basenames, case-insensitive).
    void setLists(std::vector<std::wstring> alwaysPause, std::vector<std::wstring> neverPause);

    // Re-evaluates the foreground process (the app reads the pid from the
    // foreground window with GetWindowThreadProcessId). Cheap: the process
    // path is only looked up when the cache key CHANGED — the pid changed OR
    // the cached process exited (a reused pid must not return stale
    // classification). Window class is passed for heuristic game detection.
    // Returns the current state.
    using AliveCheck = std::function<bool(DWORD pid)>;
    GameState updateForeground(DWORD pid, const std::wstring& windowClass = {},
                               PathLookup lookup = defaultPathLookup,
                               AliveCheck alive = isAlive);

    const GameState& state() const { return state_; }

    // Pure classification (unit-testable): an executable name against the
    // lists. Both sides are normalized (basename, lowercase) before the
    // match. NeverPause (deny) wins over AlwaysPause (allow) — a game listed
    // in both is treated as "never pause".
    static GameClass classify(const std::wstring& exeName,
                              const std::vector<std::wstring>& alwaysPause,
                              const std::vector<std::wstring>& neverPause);

    // Heuristic game detection: checks window class name + process path
    // for known game engine / launcher patterns. Used when the explicit
    // allow/deny lists don't match (Unknown). Window class is optional —
    // when available it catches Unity/Unreal/GLFW/SDL engines.
    static GameClass classifyHeuristic(const std::wstring& exeName,
                                       const std::wstring& processPath,
                                       const std::wstring& windowClass);

    // Clears the cache + state (config change / playlist reset).
    void reset();

    static std::optional<std::wstring> defaultPathLookup(DWORD pid);

    // True when `pid` is (still) a live process. Cheap (OpenProcess +
    // GetExitCodeProcess) — used to invalidate the cache when a process exits
    // so a REUSED pid does not return stale classification.
    static bool isAlive(DWORD pid);

private:
    std::vector<std::wstring> alwaysPause_;
    std::vector<std::wstring> neverPause_;
    GameState state_;
    DWORD cachedPid_ = 0;
};

} // namespace vw::detection
