#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "app/UiContract.h"
#include "video/VideoMetadata.h"

namespace vw::library {

// Minimal media library (docs/03 §3.14, spec §10.3, M11). Item store with
// lazy metadata + incremental folder watching. The UI drives it ONLY through
// commands (LIBRARY_*) and reads state via UiSnapshot / LibraryChange
// notifications — the library never decodes or plays.
//
// Design (spec §10.3):
//   - Items are path + file stats (size, last-write) + LAZY metadata: probing
//     opens the Source Reader (probeMetadata), so it runs on a background
//     worker and only for items the UI asks for (visible/selected rows).
//   - AddFolder registers a ReadDirectoryChangesW watch (recursive). Watch
//     events update the item set INCREMENTALLY — no full rescan, no repeated
//     enumeration (10k-file folders are safe). refresh() is the manual
//     incremental rescan (re-stat + new-file discovery in watched folders).
//   - Video files are identified by extension (mp4/mkv/mov/avi/webm/m4v/ts);
//     the real media type is validated by the metadata probe (a garbage .mp4
//     is listed but its probe fails -> metadata stays unknown, never crashes).
//
// Thread-safety: mutations happen on the control thread; the watch thread
// only POSTS change events into a queue drained by the control thread
// (pollChangeEvents). The metadata worker only touches the probe queue + a
// completed-results queue, likewise drained by the control thread.
class LibraryManager {
public:
    using ChangeSink = std::function<void(const vw::ui::LibraryChangeNotification&)>;

    LibraryManager();
    ~LibraryManager();

    LibraryManager(const LibraryManager&) = delete;
    LibraryManager& operator=(const LibraryManager&) = delete;

    // Emitter for LibraryChange notifications (set by the app; no-op when the
    // UI is closed). Invoked on the control thread from the command handlers
    // and pollChangeEvents().
    void setChangeSink(ChangeSink sink) { sink_ = std::move(sink); }

    // ---- commands (control thread) ----
    // Adds explicit file paths (already validated to exist). Returns the
    // added ids; duplicates are skipped. Emits Added (batched).
    std::vector<vw::ui::LibraryItemId> addFiles(const std::vector<std::wstring>& paths);
    // Recursively scans `folder` for video files and registers a recursive
    // ReadDirectoryChangesW watch. Emits Added for new items.
    void addFolder(const std::wstring& folder);
    // Removes the given items (and prunes watches that become empty). Emits
    // Removed.
    void remove(const std::vector<vw::ui::LibraryItemId>& ids);
    // Manual incremental rescan: re-stats every item (size/last-write) and
    // discovers new files in watched folders. Emits Updated/Added/Removed
    // deltas + RescanStarted/RescanFinished around the pass.
    void refresh();

    // ---- reads (control thread; snapshot copies) ----
    const std::vector<vw::ui::LibraryItem>& items() const { return items_; }
    const vw::ui::LibraryItem* itemById(vw::ui::LibraryItemId id) const;
    const std::vector<std::wstring>& watchedFolders() const { return watchedFolders_; }

    // ---- lazy metadata ----
    // Enqueues a metadata probe for the item (returns false if already
    // pending/done). The worker probes and the result lands in
    // pollChangeEvents() as an Updated notification (metadataLoaded=true, or
    // remains false with the probe error recorded).
    bool requestMetadata(vw::ui::LibraryItemId id);

    // Number of metadata probes completed (test hook for lazy probing).
    size_t metadataProbesCompleted() const { return probeResultsCount_; }

    // Drains the watch/probe result queues (call from the control thread,
    // e.g. on the app's UI timer); emits the corresponding notifications.
    void pollChangeEvents();

    // Number of items (test hook).
    size_t size() const { return items_.size(); }
    // Number of watches registered (test hook).
    size_t watchCount() const { return watches_.size(); }

private:
    struct Watch;
    static bool isVideoFile(const std::wstring& path);
    static vw::ui::VideoCodec codecFromName(const std::wstring& codec);
    static bool isVideoExtension(const std::wstring& ext);
    static void collectVideoFiles(const std::wstring& folder, std::vector<std::wstring>& out);
    vw::ui::LibraryItemId nextId() { return ++nextId_; }
    void scanFolderInto(const std::wstring& folder, std::vector<std::wstring>& out);
    void addItemInternal(const std::wstring& path, std::vector<vw::ui::LibraryItemId>& added);
    void startWatch(const std::wstring& folder);
    void stopWatch(const std::wstring& folder);
    void probeLoop(); // background worker
    void emit(const vw::ui::LibraryChangeNotification& n);

    std::vector<vw::ui::LibraryItem> items_;           // control thread
    std::map<std::wstring, size_t> itemIndex_;         // path -> items_ index
    std::vector<std::wstring> watchedFolders_;
    std::vector<std::unique_ptr<Watch>> watches_;      // one per watched folder
    ChangeSink sink_;
    vw::ui::LibraryItemId nextId_ = 0;

    // Watch thread: posts raw events here; drained by pollChangeEvents().
    struct PendingEvent {
        enum class Kind { Added, Removed, Updated } kind;
        std::wstring path;
    };
    std::mutex eventMu_;
    std::vector<PendingEvent> pendingEvents_;

    // Metadata probe: PATH queue -> worker -> results queue. The queue holds
    // PATHS, not ids: the worker must never touch items_ (control-thread
    // owned — a read there would race with mutations). The control thread
    // resolves id->path on enqueue and path->item on drain; a probe whose
    // item was removed meanwhile is dropped.
    struct ProbeResult {
        std::wstring path;
        bool ok = false;
        vw::video::VideoMetadata meta; // valid only when ok
    };
    std::mutex probeMu_;
    std::condition_variable probeCv_;
    std::vector<std::wstring> probeQueue_;
    std::vector<ProbeResult> probeResults_;
    size_t probeResultsCount_ = 0; // completed probes (control-thread owned)
    std::thread probeThread_;
    std::atomic<bool> probeStop_{false};

    // Rebuilds itemIndex_ from items_ (erase operations shift positions —
    // the stored indices would point at the wrong item afterwards). O(n);
    // erasure is rare (user action / file deletion), so this is fine.
    void rebuildIndex();
};

} // namespace vw::library
