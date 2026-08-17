#include "doctest.h"

#include <windows.h>
#include <mfapi.h>

#include <chrono>
#include <filesystem>
#include <fstream>

#include "library/LibraryManager.h"

namespace {

struct MfScope {
    bool ok = false;
    MfScope() { ok = SUCCEEDED(::MFStartup(MF_VERSION)); }
    ~MfScope() {
        if (ok) {
            ::MFShutdown();
        }
    }
};

// Temp sandbox dir: one .mp4 (real clip when available, else a fake), one
// .txt (must be ignored), one subdir with another .mp4 (recursive scan).
struct Sandbox {
    std::filesystem::path root;
    std::filesystem::path video1, video2, txt;

    explicit Sandbox(bool nested = true) {
        root = std::filesystem::temp_directory_path() /
               (L"vw_lib_" + std::to_wstring(::GetTickCount64()));
        std::filesystem::create_directories(root);
        video1 = root / L"a.mp4";
        txt = root / L"notes.txt";
        write(video1, "not a real video (probe fails — listed, unknown metadata)");
        write(txt, "ignored");
        if (nested) {
            std::filesystem::create_directories(root / L"sub");
            video2 = root / L"sub" / L"b.mp4";
            write(video2, "also fake");
        }
    }
    ~Sandbox() { std::filesystem::remove_all(root); }

    static void write(const std::filesystem::path& p, const std::string& bytes) {
        std::ofstream out(p, std::ios::binary);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    }
};

// A real playable clip for the metadata probe (same convention as the other
// real-file tests). Returns empty when unavailable.
std::filesystem::path realClip() {
    for (const auto& dir : {std::filesystem::path(L"C:/Users/mbk43/Videos/bgcmp"),
                            std::filesystem::path(L"C:/Users/mbk43/Downloads/Video")}) {
        std::error_code ec;
        for (std::filesystem::directory_iterator it(dir, ec), end; it != end; it.increment(ec)) {
            if (it->is_regular_file() && it->path().extension() == L".mp4") {
                return it->path();
            }
        }
    }
    return {};
}

// Drains watch/probe events until the predicate holds (bounded — the watch
// thread delivers asynchronously).
template <typename F>
bool waitFor(vw::library::LibraryManager& lib, F&& pred, int ms = 3000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < deadline) {
        lib.pollChangeEvents();
        if (pred()) {
            return true;
        }
        ::Sleep(25);
    }
    return false;
}

} // namespace

using vw::library::LibraryManager;

TEST_CASE("library: addFiles adds, dedups, and emits one batched Added") {
    Sandbox s;
    LibraryManager lib;
    int addedNotifs = 0;
    lib.setChangeSink([&](const vw::ui::LibraryChangeNotification& n) {
        if (n.kind == vw::ui::LibraryChangeKind::Added) {
            ++addedNotifs;
        }
    });
    const auto ids = lib.addFiles({s.video1.wstring(), s.video1.wstring(), s.txt.wstring()});
    CHECK(ids.size() == 1); // duplicate + non-video skipped
    CHECK(lib.size() == 1);
    CHECK(addedNotifs == 1);
    lib.pollChangeEvents();
    CHECK(lib.size() == 1);
}

TEST_CASE("library: addFolder scans recursively and dedups on re-add") {
    Sandbox s;
    LibraryManager lib;
    lib.addFolder(s.root.wstring());
    CHECK(lib.size() == 2); // a.mp4 + sub/b.mp4 (notes.txt ignored)
    CHECK(lib.watchCount() == 1);
    lib.addFolder(s.root.wstring()); // re-adding the same folder: no dupes
    CHECK(lib.size() == 2);
}

TEST_CASE("library: remove rebuilds the index (dedup still works after erase)") {
    Sandbox s;
    LibraryManager lib;
    const auto ids = lib.addFiles({s.video1.wstring(), s.video2.wstring()});
    REQUIRE(ids.size() == 2);
    // Remove the FIRST item: without a rebuild, the stored index for the
    // second item would shift and dedup would break (or worse, out of range).
    lib.remove({ids[0]});
    CHECK(lib.size() == 1);
    const auto again = lib.addFiles({s.video2.wstring()});
    CHECK(again.empty()); // dedup still catches it after the erase
    CHECK(lib.size() == 1);
}

TEST_CASE("library: watch adds new files and removes deleted ones") {
    Sandbox s;
    LibraryManager lib;
    lib.addFolder(s.root.wstring());
    REQUIRE(lib.size() == 2);

    // New file dropped into the watched folder -> appears via the watch.
    const auto newFile = s.root / L"new.mp4";
    Sandbox::write(newFile, "fake");
    CHECK(waitFor(lib, [&] { return lib.size() == 3; }));

    // Delete -> disappears via the watch.
    std::error_code ec;
    std::filesystem::remove(newFile, ec);
    CHECK(waitFor(lib, [&] { return lib.size() == 2; }));
}

TEST_CASE("library: refresh detects changed files and finds new ones") {
    Sandbox s;
    LibraryManager lib;
    lib.addFolder(s.root.wstring());
    REQUIRE(lib.size() == 2);

    const auto newFile = s.root / L"c.mp4";
    Sandbox::write(newFile, "fake");
    int updated = 0, rescan = 0;
    lib.setChangeSink([&](const vw::ui::LibraryChangeNotification& n) {
        if (n.kind == vw::ui::LibraryChangeKind::Updated) {
            updated += static_cast<int>(n.ids.size());
        }
        if (n.kind == vw::ui::LibraryChangeKind::RescanStarted) {
            ++rescan;
        }
    });
    // Grow a.mp4 (content changed) + drop c.mp4, then refresh.
    {
        std::ofstream out(s.video1, std::ios::binary | std::ios::app);
        out << "grow";
    }
    Sandbox::write(newFile, "fake");
    lib.refresh();
    CHECK(lib.size() == 3);       // c.mp4 discovered
    CHECK(updated >= 1);          // a.mp4 marked updated
    CHECK(rescan == 1);           // RescanStarted fired
}

TEST_CASE("library: lazy metadata probe fills a real clip, skips non-video") {
    MfScope mf;
    if (!mf.ok) {
        MESSAGE("MFStartup failed — skipping");
        return;
    }
    const auto clip = realClip();
    if (clip.empty()) {
        MESSAGE("no real clip — skipping");
        return;
    }
    Sandbox s;
    LibraryManager lib;
    lib.addFolder(s.root.wstring());
    lib.addFiles({clip.wstring()});
    REQUIRE(lib.size() == 3);
    CHECK(lib.metadataProbesCompleted() == 0); // lazy: nothing probed yet

    // Probe ONLY the real clip (locate by path, not by id).
    vw::ui::LibraryItemId realId = 0;
    for (const auto& i : lib.items()) {
        if (i.path == clip.wstring()) {
            realId = i.id;
            break;
        }
    }
    REQUIRE(realId != 0);
    CHECK(lib.requestMetadata(realId));
    CHECK_FALSE(lib.requestMetadata(realId)); // already pending

    CHECK(waitFor(lib, [&] { return lib.metadataProbesCompleted() >= 1; }));
    const auto* after = lib.itemById(realId);
    REQUIRE(after != nullptr);
    CHECK(after->metadataLoaded);
    CHECK(after->metadata.width > 0);
    CHECK(after->metadata.height > 0);
}

TEST_CASE("library: non-video probe never crashes (listed, metadata unknown)") {
    MfScope mf;
    if (!mf.ok) {
        MESSAGE("MFStartup failed — skipping");
        return;
    }
    Sandbox s;
    LibraryManager lib;
    lib.addFolder(s.root.wstring());
    CHECK(lib.size() == 2);
    // Probing the fake .mp4s: probe fails -> metadata stays unknown, listed.
    vw::ui::LibraryItemId firstId = 0;
    for (const auto& i : lib.items()) {
        firstId = i.id;
        break;
    }
    REQUIRE(firstId != 0);
    CHECK(lib.requestMetadata(firstId));
    CHECK(waitFor(lib, [&] { return lib.metadataProbesCompleted() >= 1; }));
    const auto* after = lib.itemById(firstId);
    REQUIRE(after != nullptr);
    CHECK_FALSE(after->metadataLoaded); // probe failed, item still listed
}

TEST_CASE("library: probe results for removed items are dropped safely") {
    MfScope mf;
    if (!mf.ok) {
        MESSAGE("MFStartup failed — skipping");
        return;
    }
    const auto clip = realClip();
    if (clip.empty()) {
        MESSAGE("no real clip — skipping");
        return;
    }
    Sandbox s;
    LibraryManager lib;
    lib.addFiles({clip.wstring()});
    REQUIRE(lib.size() == 1);
    vw::ui::LibraryItemId id = 0;
    for (const auto& i : lib.items()) {
        id = i.id;
    }
    REQUIRE(lib.requestMetadata(id));
    // Remove the item BEFORE the probe result drains: pollChangeEvents must
    // resolve path -> item on the control thread and drop the orphan (no
    // crash, no spurious Updated).
    lib.remove({id});
    int updated = 0;
    lib.setChangeSink([&](const vw::ui::LibraryChangeNotification& n) {
        if (n.kind == vw::ui::LibraryChangeKind::Updated) {
            ++updated;
        }
    });
    CHECK(waitFor(lib, [&] { return lib.metadataProbesCompleted() >= 1; }));
    CHECK(updated == 0);
    CHECK(lib.size() == 0);
}
