#pragma once

#include <filesystem>
#include <optional>

#include "playlist/PlaylistManager.h"
#include "util/Result.h"

namespace vw::playlist {

// On-disk playlist format (docs/03 §3.9, decision D-06: strict JSON via the
// project's parser — no third-party dependency). Saved ONLY on transition /
// shutdown / meaningful change, never every second.
//
// File shape (version 1):
//   {
//     "version": 1,
//     "mode": "loop",                 // single|sequential|loop|shuffle
//     "loop": true,
//     "current": 2,
//     "shuffleOrder": [1, 0, 2],
//     "items": [{ "path": "...", "start100ns": 0, "end100ns": 0,
//                 "enabled": true, "duration100ns": ..., "width": ...,
//                 "height": ..., "codec": "H.264" }]
//   }
//
// load() returns nullopt when the file is missing OR corrupt (logged inside);
// the caller seeds defaults. Unknown keys are ignored; wrong-typed values fall
// back to field defaults (same tolerance as the config loader). A file with a
// version NEWER than supported is treated as corrupt (never guess).
class PlaylistStore {
public:
    static constexpr int kFormatVersion = 1;

    static std::optional<PlaylistData> load(const std::filesystem::path& path);
    static Result<void> save(const std::filesystem::path& path, const PlaylistData& data);

    static const wchar_t* modeName(Mode mode);          // for JSON + logging
    static std::optional<Mode> modeFromName(const std::wstring& name);
};

} // namespace vw::playlist
