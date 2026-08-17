#include "doctest.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>

#include "playlist/PlaylistManager.h"
#include "playlist/PlaylistStore.h"

using vw::playlist::PlaylistData;
using vw::playlist::PlaylistItem;
using vw::playlist::PlaylistManager;
using vw::playlist::PlaylistStore;

namespace {

PlaylistManager threeItemPlaylist() {
    PlaylistManager pm;
    pm.add(L"a.mp4");
    pm.add(L"b.mp4");
    pm.add(L"c.mp4");
    pm.setCurrent(0);
    return pm;
}

} // namespace

// ---- structural ops --------------------------------------------------------

TEST_CASE("playlist: add returns indices and stores items in order") {
    PlaylistManager pm;
    CHECK(pm.empty());
    CHECK(pm.add(L"a.mp4") == 0);
    CHECK(pm.add(L"b.mp4") == 1);
    CHECK(pm.add(L"c.mp4") == 2);
    REQUIRE(pm.size() == 3);
    CHECK(pm.itemAt(0)->path == L"a.mp4");
    CHECK(pm.itemAt(2)->path == L"c.mp4");
    CHECK(pm.itemAt(3) == nullptr);
    CHECK(pm.currentIndex() == PlaylistManager::kNoIndex);
}

TEST_CASE("playlist: remove shifts indices and repairs current") {
    PlaylistManager pm = threeItemPlaylist(); // current = 0
    CHECK_FALSE(pm.remove(5));
    CHECK(pm.remove(0));
    REQUIRE(pm.size() == 2);
    CHECK(pm.itemAt(0)->path == L"b.mp4");
    CHECK(pm.currentIndex() == PlaylistManager::kNoIndex); // removed current

    pm.setCurrent(1);
    pm.remove(0); // removing before current shifts it down
    CHECK(pm.currentIndex() == 0);
    CHECK(pm.itemAt(0)->path == L"c.mp4");
}

TEST_CASE("playlist: move reorders and tracks current") {
    PlaylistManager pm = threeItemPlaylist(); // [a,b,c], current 0
    CHECK_FALSE(pm.move(0, 0));
    CHECK(pm.move(0, 2)); // [b,c,a]
    CHECK(pm.itemAt(0)->path == L"b.mp4");
    CHECK(pm.itemAt(2)->path == L"a.mp4");
    CHECK(pm.currentIndex() == 2); // current followed the moved item
}

TEST_CASE("playlist: replace, clear, enabled, times") {
    PlaylistManager pm = threeItemPlaylist();
    PlaylistItem r;
    r.path = L"replacement.mp4";
    CHECK_FALSE(pm.replace(9, r));
    CHECK(pm.replace(1, r));
    CHECK(pm.itemAt(1)->path == L"replacement.mp4");

    CHECK(pm.setEnabled(1, false));
    CHECK_FALSE(pm.itemAt(1)->enabled);
    CHECK(pm.setItemTimes(0, 100, 5000));
    CHECK(pm.itemAt(0)->start100ns == 100);
    CHECK(pm.itemAt(0)->end100ns == 5000);
    CHECK_FALSE(pm.setItemTimes(9, 0, 0));

    pm.clear();
    CHECK(pm.empty());
    CHECK(pm.currentIndex() == PlaylistManager::kNoIndex);
}

TEST_CASE("playlist: updateCachedMetadata fills the cached fields") {
    PlaylistManager pm = threeItemPlaylist();
    CHECK(pm.updateCachedMetadata(1, 30'000'000, 1920, 1080, L"H.264"));
    CHECK_FALSE(pm.updateCachedMetadata(9, 0, 0, 0, L""));
    CHECK(pm.itemAt(1)->duration100ns == 30'000'000);
    CHECK(pm.itemAt(1)->width == 1920);
    CHECK(pm.itemAt(1)->height == 1080);
    CHECK(pm.itemAt(1)->codec == L"H.264");
}

// ---- navigation policy -----------------------------------------------------

TEST_CASE("playlist: sequential without loop stops at the end") {
    PlaylistManager pm = threeItemPlaylist();
    pm.setMode(vw::playlist::Mode::Sequential);
    pm.setLoop(false);
    CHECK(pm.nextIndex() == 1);
    pm.setCurrent(1);
    CHECK(pm.nextIndex() == 2);
    pm.setCurrent(2);
    CHECK(pm.nextIndex() == PlaylistManager::kNoIndex); // stop, hold last frame
}

TEST_CASE("playlist: sequential with loop wraps") {
    PlaylistManager pm = threeItemPlaylist();
    pm.setMode(vw::playlist::Mode::Sequential);
    pm.setLoop(true);
    pm.setCurrent(2);
    CHECK(pm.nextIndex() == 0);
}

TEST_CASE("playlist: Loop mode always wraps regardless of the loop flag") {
    PlaylistManager pm = threeItemPlaylist();
    pm.setMode(vw::playlist::Mode::Loop);
    pm.setLoop(false); // spec: Loop playlist wraps even with loop off
    pm.setCurrent(2);
    CHECK(pm.nextIndex() == 0);
}

TEST_CASE("playlist: Single mode self-loops") {
    PlaylistManager pm = threeItemPlaylist();
    pm.setMode(vw::playlist::Mode::Single);
    pm.setLoop(false); // spec §5/M7: single-item mode loops even when loop is off
    pm.setCurrent(1);
    CHECK(pm.nextIndex() == 1);
    CHECK(pm.previousIndex() == 1);
}

TEST_CASE("playlist: next/previous skip disabled items") {
    PlaylistManager pm = threeItemPlaylist();
    pm.setMode(vw::playlist::Mode::Sequential);
    pm.setLoop(true);
    pm.setEnabled(1, false); // [a, X, c]
    pm.setCurrent(0);
    CHECK(pm.nextIndex() == 2); // skips disabled b
    pm.setCurrent(2);
    CHECK(pm.nextIndex() == 0); // wraps past a
    CHECK(pm.previousIndex() == 0); // backward from c skips b
}

TEST_CASE("playlist: next skips unavailable items (broken files)") {
    PlaylistManager pm = threeItemPlaylist();
    pm.setMode(vw::playlist::Mode::Sequential);
    pm.setLoop(false);
    pm.setCurrent(0);
    pm.markUnavailable(1);
    CHECK(pm.isUnavailable(1));
    CHECK(pm.nextIndex() == 2); // skips the broken item
}

TEST_CASE("playlist: previous walks backward with wrap") {
    PlaylistManager pm = threeItemPlaylist();
    pm.setMode(vw::playlist::Mode::Loop);
    pm.setCurrent(0);
    CHECK(pm.previousIndex() == 2);
}

TEST_CASE("playlist: all items unavailable -> next is kNoIndex") {
    PlaylistManager pm = threeItemPlaylist();
    pm.setMode(vw::playlist::Mode::Sequential);
    pm.setLoop(true);
    pm.setCurrent(0);
    pm.markUnavailable(0);
    pm.markUnavailable(1);
    pm.markUnavailable(2);
    CHECK(pm.nextIndex() == PlaylistManager::kNoIndex);
}

TEST_CASE("playlist: shuffle is a permutation with no immediate repeat") {
    PlaylistManager pm = threeItemPlaylist();
    pm.setMode(vw::playlist::Mode::Shuffle);
    pm.setLoop(true);
    pm.setCurrent(0);
    const auto order0 = pm.shuffleOrder();
    REQUIRE(order0.size() == 3);
    std::set<size_t> seen(order0.begin(), order0.end());
    CHECK(seen.size() == 3); // full permutation of 0..2
    CHECK(seen.count(0) == 1);
    CHECK(seen.count(1) == 1);
    CHECK(seen.count(2) == 1);

    // Walk until the cycle wraps and a fresh order is generated. Consecutive
    // plays are never the same item; the fresh cycle does not start with the
    // item that was playing when the wrap happened (the old cycle's last item).
    size_t current = 0;
    size_t lastPlayed = current;
    std::vector<size_t> fresh;
    for (int i = 0; i < 6 && fresh.empty(); ++i) {
        const size_t next = pm.nextIndex();
        REQUIRE(next != PlaylistManager::kNoIndex);
        CHECK(next != current);
        lastPlayed = current; // item playing when this advance happened
        current = next;
        pm.setCurrent(current);
        if (pm.shuffleOrder() != order0) {
            fresh = pm.shuffleOrder();
        }
    }
    REQUIRE(fresh.size() == 3); // the wrapped cycle regenerated the order
    // The wrap regenerates the cycle and returns its first playable item, so
    // fresh.front() == current by construction; the guarantee is against the
    // item that was playing when the wrap fired.
    CHECK(fresh.front() != lastPlayed);
}

TEST_CASE("playlist: shuffle without loop stops at the cycle end") {
    PlaylistManager pm = threeItemPlaylist();
    pm.setMode(vw::playlist::Mode::Shuffle);
    pm.setLoop(false);
    pm.setCurrent(0);
    // Item 0 sits somewhere in the cycle; the walk plays every item AFTER it,
    // then stops (kNoIndex) — never wrapping.
    const auto& order = pm.shuffleOrder();
    const size_t pos0 = static_cast<size_t>(
        std::find(order.begin(), order.end(), size_t{0}) - order.begin());
    const size_t expectedSteps = order.size() - 1 - pos0;
    size_t steps = 0;
    size_t next = pm.nextIndex();
    while (next != PlaylistManager::kNoIndex) {
        ++steps;
        pm.setCurrent(next);
        next = pm.nextIndex();
    }
    CHECK(steps == expectedSteps);
}

// ---- adopt / load repair ---------------------------------------------------

// ---- M7 review fixes -------------------------------------------------------

TEST_CASE("playlist: empty playlist in Shuffle mode never crashes navigation") {
    // Regression: shuffledPermutation(n=0) underflowed i to SIZE_MAX and
    // dereferenced order[i] out of bounds (empty playlist + Shuffle mode,
    // reachable via config mode=shuffle with no videoPath).
    PlaylistManager pm;
    pm.setMode(vw::playlist::Mode::Shuffle);
    CHECK(pm.shuffleOrder().empty());
    CHECK(pm.nextIndex() == PlaylistManager::kNoIndex);
    CHECK(pm.previousIndex() == PlaylistManager::kNoIndex);
    pm.add(L"a.mp4"); // adding to an empty Shuffle playlist is safe too
    CHECK(pm.shuffleOrder().size() == 1);
    CHECK(pm.nextIndex() == 0);
}

TEST_CASE("playlist: shuffle navigation never returns a non-playable item") {
    // Every public mutation keeps the shuffle order a full permutation, so the
    // "stale order" branch in nextIndex() is defense-in-depth — but BOTH that
    // branch and shuffleForward's wrap branch must skip disabled/unavailable
    // items and land on a playable one (review fix).
    PlaylistManager pm;
    pm.add(L"a.mp4");
    pm.add(L"b.mp4");
    pm.add(L"c.mp4");
    pm.setMode(vw::playlist::Mode::Shuffle);
    pm.setCurrent(1);
    pm.markUnavailable(0);
    pm.markUnavailable(2);
    // Only b is playable — navigation must land on it from either direction.
    CHECK(pm.nextIndex() == 1);
    CHECK(pm.previousIndex() == 1);
    // A fresh cycle (wrap) with the same constraint still finds the one
    // playable item.
    pm.setCurrent(1);
    CHECK(pm.nextIndex() == 1);
}

TEST_CASE("playlist: kNoIndex current survives the store round-trip") {
    // Regression: size_t(-1) cast to double (1.84e19) then back through
    // int64_t was an out-of-range cast (UB) — kNoIndex came back as garbage.
    const auto path = std::filesystem::temp_directory_path() / L"vw_playlist_noindex.json";
    std::filesystem::remove(path);
    PlaylistData data;
    data.items = {PlaylistItem{L"C:/videos/a.mp4"}, PlaylistItem{L"C:/videos/b.mp4"}};
    data.current = PlaylistManager::kNoIndex; // e.g. after remove() of current
    REQUIRE(PlaylistStore::save(path, data));
    auto loaded = PlaylistStore::load(path);
    REQUIRE(loaded.has_value());
    CHECK(loaded->current == PlaylistManager::kNoIndex);
    std::filesystem::remove(path);
}

TEST_CASE("playlist: replace clears stale cached metadata") {
    PlaylistManager pm = threeItemPlaylist();
    pm.updateCachedMetadata(0, 30'000'000, 1920, 1080, L"H.264");
    CHECK(pm.itemAt(0)->duration100ns == 30'000'000);
    PlaylistItem replacement{L"C:/videos/new.mp4"};
    REQUIRE(pm.replace(0, replacement));
    // A different file may sit behind the same index — cached metadata is
    // stale until the next real open refreshes it.
    CHECK(pm.itemAt(0)->path == L"C:/videos/new.mp4");
    CHECK(pm.itemAt(0)->duration100ns == 0);
    CHECK(pm.itemAt(0)->width == 0);
    CHECK(pm.itemAt(0)->codec.empty());
}

TEST_CASE("playlist: adopt clamps out-of-range current and repairs the order") {
    PlaylistData data;
    data.items = {PlaylistItem{L"a.mp4"}, PlaylistItem{L"b.mp4"}, PlaylistItem{L"c.mp4"}};
    data.mode = vw::playlist::Mode::Shuffle;
    data.current = 99;            // out of range -> clamped
    data.shuffleOrder = {5, 0, 0}; // invalid + duplicate -> sanitized permutation
    PlaylistManager pm(std::move(data));
    CHECK(pm.currentIndex() == 0);
    REQUIRE(pm.shuffleOrder().size() == 3);
    std::set<size_t> seen(pm.shuffleOrder().begin(), pm.shuffleOrder().end());
    CHECK(seen.size() == 3);
    CHECK(seen.count(0) == 1);
    CHECK(seen.count(1) == 1);
    CHECK(seen.count(2) == 1);
}

// ---- persistence -----------------------------------------------------------

TEST_CASE("playlist: store round-trips items, mode, current and shuffle order") {
    const auto path = std::filesystem::temp_directory_path() / L"vw_playlist_roundtrip.json";
    std::filesystem::remove(path);

    PlaylistData data;
    data.items = {PlaylistItem{L"C:/videos/a.mp4", 0, 0, true, 30'000'000, 1920, 1080, L"H.264"},
                  PlaylistItem{L"C:/videos/b.mp4"}};
    data.mode = vw::playlist::Mode::Shuffle;
    data.loop = false;
    data.current = 1;
    data.shuffleOrder = {1, 0};
    auto saved = PlaylistStore::save(path, data);
    REQUIRE(saved);

    auto loaded = PlaylistStore::load(path);
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->items.size() == 2);
    CHECK(loaded->items[0].path == L"C:/videos/a.mp4");
    CHECK(loaded->items[0].start100ns == 0);
    CHECK(loaded->items[0].enabled);
    CHECK(loaded->items[0].duration100ns == 30'000'000);
    CHECK(loaded->items[0].width == 1920);
    CHECK(loaded->items[0].height == 1080);
    CHECK(loaded->items[0].codec == L"H.264");
    CHECK(loaded->items[1].path == L"C:/videos/b.mp4");
    CHECK(loaded->mode == vw::playlist::Mode::Shuffle);
    CHECK_FALSE(loaded->loop);
    CHECK(loaded->current == 1);
    REQUIRE(loaded->shuffleOrder.size() == 2);
    CHECK(loaded->shuffleOrder[0] == 1);
    CHECK(loaded->shuffleOrder[1] == 0);

    std::filesystem::remove(path);
}

TEST_CASE("playlist: non-ASCII paths survive the UTF-8 store") {
    const auto path = std::filesystem::temp_directory_path() / L"vw_playlist_utf8.json";
    std::filesystem::remove(path);
    PlaylistData data;
    data.items = {PlaylistItem{L"C:/videos/壁纸/视频.mp4"}, PlaylistItem{L"C:/éclair.mp4"}};
    data.current = 0;
    REQUIRE(PlaylistStore::save(path, data));
    auto loaded = PlaylistStore::load(path);
    REQUIRE(loaded.has_value());
    REQUIRE(loaded->items.size() == 2);
    CHECK(loaded->items[0].path == L"C:/videos/壁纸/视频.mp4");
    CHECK(loaded->items[1].path == L"C:/éclair.mp4");
    std::filesystem::remove(path);
}

TEST_CASE("playlist: store treats missing and corrupt files as defaults") {
    const auto missing = std::filesystem::temp_directory_path() / L"vw_playlist_missing.json";
    std::filesystem::remove(missing);
    CHECK_FALSE(PlaylistStore::load(missing).has_value());

    const auto corrupt = std::filesystem::temp_directory_path() / L"vw_playlist_corrupt.json";
    {
        std::ofstream out(corrupt, std::ios::binary);
        out << "{ this is not json !!!";
    }
    CHECK_FALSE(PlaylistStore::load(corrupt).has_value());

    // Unsupported future version -> treated as corrupt.
    const auto future = std::filesystem::temp_directory_path() / L"vw_playlist_future.json";
    {
        std::ofstream out(future, std::ios::binary);
        out << "{\"version\": 999, \"items\": []}";
    }
    CHECK_FALSE(PlaylistStore::load(future).has_value());

    std::filesystem::remove(corrupt);
    std::filesystem::remove(future);
}

TEST_CASE("playlist: mode names round-trip through the store") {
    CHECK(std::wstring(PlaylistStore::modeName(vw::playlist::Mode::Single)) == L"single");
    CHECK(std::wstring(PlaylistStore::modeName(vw::playlist::Mode::Sequential)) == L"sequential");
    CHECK(std::wstring(PlaylistStore::modeName(vw::playlist::Mode::Loop)) == L"loop");
    CHECK(std::wstring(PlaylistStore::modeName(vw::playlist::Mode::Shuffle)) == L"shuffle");
    CHECK(PlaylistStore::modeFromName(L"single") == vw::playlist::Mode::Single);
    CHECK(PlaylistStore::modeFromName(L"shuffle") == vw::playlist::Mode::Shuffle);
    CHECK_FALSE(PlaylistStore::modeFromName(L"weird").has_value());
}

TEST_CASE("playlist: 200-item playlist ops stay correct and cheap") {
    PlaylistManager pm;
    for (int i = 0; i < 200; ++i) {
        pm.add(L"clip" + std::to_wstring(i) + L".mp4");
    }
    pm.setMode(vw::playlist::Mode::Sequential);
    pm.setLoop(true);
    pm.setCurrent(0);
    size_t next = pm.nextIndex();
    int steps = 0;
    while (next != 0 && steps < 300) { // stop when the cycle wraps back to 0
        pm.setCurrent(next);
        next = pm.nextIndex();
        ++steps;
    }
    CHECK(steps == 199); // visited every other item, then wrapped to 0
    CHECK(next == 0);
    CHECK(pm.itemAt(199)->path == L"clip199.mp4");
}
