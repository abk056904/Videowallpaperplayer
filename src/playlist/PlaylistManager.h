#pragma once

#include <cstddef>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

namespace vw::playlist {

// Playback order policy (docs/03 §3.9, spec §18 + M7 adjustment). Mirrors
// config::PlaybackMode:
//   Single     — one item, always self-loops (the loop setting is effectively
//                forced on for single-item mode per spec §5/M7 note).
//   Sequential — play in list order; wrap only when loop() is set.
//   Loop       — sequential, always wraps.
//   Shuffle    — randomized order (persisted); no immediate repeat when >1
//                item; wraps only when loop() is set.
enum class Mode { Single, Sequential, Loop, Shuffle };

// One playlist entry — lightweight by design (docs §32: path + cached
// metadata only, NEVER decoded frames). start/end are optional clip bounds;
// 0 = unset.
struct PlaylistItem {
    std::wstring path;
    int64_t start100ns = 0;  // optional start trim (0 = from the beginning)
    int64_t end100ns = 0;    // optional end trim (0 = natural end)
    bool enabled = true;
    // Cached metadata from a REAL open (never the file extension), persisted
    // so a 10,000-item playlist starts instantly on later runs. 0 = not yet
    // read.
    int64_t duration100ns = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    std::wstring codec;      // human-readable, e.g. L"H.264"
};

// Persisted playlist state (PlaylistStore's on-disk shape).
struct PlaylistData {
    std::vector<PlaylistItem> items;
    Mode mode = Mode::Loop;
    bool loop = true;
    size_t current = static_cast<size_t>(-1);   // kNoIndex semantics
    std::vector<size_t> shuffleOrder;           // permutation over items (Shuffle)
};

// Playlist engine (docs/03 §3.9, M7). Pure list + navigation logic — it never
// touches playback (docs §78: playlist ops must not affect active frame
// playback). The app drives transitions: on EOS it asks nextIndex() and plays
// the result; broken items are marked unavailable (runtime-only; recovery =
// next run tries again, M12 hardens) and skipped by navigation.
class PlaylistManager {
public:
    static constexpr size_t kNoIndex = static_cast<size_t>(-1);

    PlaylistManager() = default;
    explicit PlaylistManager(PlaylistData data);

    // ---- structural ops (never affect active playback) ----
    size_t add(const std::wstring& path);              // returns the new index
    size_t add(const PlaylistItem& item);
    bool remove(size_t index);
    bool move(size_t from, size_t to);
    bool replace(size_t index, const PlaylistItem& item);
    void clear();
    bool setEnabled(size_t index, bool enabled);
    bool setItemTimes(size_t index, int64_t start100ns, int64_t end100ns);
    // Writes real opened metadata back into the cached fields (persisted).
    bool updateCachedMetadata(size_t index, int64_t duration100ns, uint32_t width,
                              uint32_t height, std::wstring codec);

    // ---- navigation policy ----
    // Index to play after the current item ends (kNoIndex = stop and hold the
    // last frame). Skips disabled and unavailable items. Shuffle mode may
    // reshuffle on a looped wrap (avoiding an immediate repeat of the item
    // just played) — the new order is reflected in data(). Non-const: the
    // looped shuffle wrap regenerates the persisted order.
    size_t nextIndex();
    size_t previousIndex() const;
    bool setCurrent(size_t index);
    size_t currentIndex() const { return data_.current; }
    const PlaylistItem* currentItem() const { return itemAt(data_.current); }
    const PlaylistItem* itemAt(size_t index) const;

    void setMode(Mode mode);
    void setLoop(bool loop);
    void shuffle(); // fresh random order (meaningful in Shuffle mode)

    // Runtime broken-item marking (docs/03 §3.9: log, mark unavailable,
    // advance). NOT persisted — the next run retries the item (recovery).
    void markUnavailable(size_t index);
    bool isUnavailable(size_t index) const;

    // ---- state ----
    size_t size() const { return data_.items.size(); }
    bool empty() const { return data_.items.empty(); }
    Mode mode() const { return data_.mode; }
    bool loop() const { return data_.loop; }
    const std::vector<PlaylistItem>& items() const { return data_.items; }
    const std::vector<size_t>& shuffleOrder() const { return data_.shuffleOrder; }

    // ---- persistence ----
    PlaylistData data() const { return data_; }
    void adopt(PlaylistData data); // validates/clamps on load

private:
    bool inRange(size_t i) const { return i < data_.items.size(); }
    bool isPlayable(size_t i) const; // in range + enabled + not unavailable
    // Next playable index strictly after `from` (cyclic when wrap). kNoIndex
    // when none (including "past the end" without wrap).
    size_t advanceFrom(size_t from, bool wrap) const;
    // Prev playable index strictly before `from` (cyclic when wrap).
    size_t retreatFrom(size_t from, bool wrap) const;
    size_t shuffleForward(size_t pos, bool wrap);  // from pos in order_ (may reshuffle)
    size_t shuffleBackward(size_t pos, bool wrap) const;
    void regenerateShuffle();  // Fisher-Yates; no immediate repeat of current
    void rebuildOrder();       // shuffle for Shuffle mode, else sequential
    void repairState();        // clamp current + sanitize order_ (load/struct change)

    PlaylistData data_;
    std::vector<bool> unavailable_; // runtime only, parallel to items
    std::mt19937 rng_{std::random_device{}()};
};

} // namespace vw::playlist
