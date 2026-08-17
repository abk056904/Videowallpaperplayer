#include "library/LibraryManager.h"

#include <windows.h>

#include <algorithm>
#include <cwctype>
#include <filesystem>

#include "logging/Logger.h"
#include "video/DecoderManager.h"

namespace vw::library {

namespace {

// Video extensions the library recognizes. The REAL media type is validated
// by the metadata probe (a renamed .txt becomes a listed item whose probe
// fails — metadata stays unknown, never a crash).
const wchar_t* kVideoExtensions[] = {L".mp4", L".mkv", L".mov", L".avi",
                                     L".webm", L".m4v", L".ts", L".mpg", L".mpeg"};

std::wstring lower(std::wstring s) {
    for (wchar_t& c : s) {
        c = static_cast<wchar_t>(::towlower(c));
    }
    return s;
}

} // namespace

vw::ui::VideoCodec LibraryManager::codecFromName(const std::wstring& codec) {
    const std::wstring c = lower(codec);
    if (c.find(L"h.264") != std::wstring::npos || c == L"h264" || c == L"avc") {
        return vw::ui::VideoCodec::H264;
    }
    if (c.find(L"h.265") != std::wstring::npos || c.find(L"hevc") != std::wstring::npos) {
        return vw::ui::VideoCodec::HEVC;
    }
    if (c.find(L"av1") != std::wstring::npos) {
        return vw::ui::VideoCodec::AV1;
    }
    if (c.find(L"vp9") != std::wstring::npos) {
        return vw::ui::VideoCodec::VP9;
    }
    return vw::ui::VideoCodec::Unknown;
}

// ---- watch (ReadDirectoryChangesW, recursive) ----
struct LibraryManager::Watch {
    std::wstring folder;
    HANDLE dir = INVALID_HANDLE_VALUE;
    std::thread thread;
    std::atomic<bool> stop{false};
    std::vector<BYTE> buffer;
    LibraryManager* owner = nullptr; // for posting events
};

LibraryManager::LibraryManager() {
    probeThread_ = std::thread([this] { probeLoop(); });
}

LibraryManager::~LibraryManager() {
    {
        std::lock_guard<std::mutex> lk(probeMu_);
        probeStop_ = true;
    }
    probeCv_.notify_all();
    if (probeThread_.joinable()) {
        probeThread_.join();
    }
    for (auto& w : watches_) {
        w->stop = true;
        if (w->dir != INVALID_HANDLE_VALUE) {
            ::CancelSynchronousIo(w->thread.native_handle());
            ::CloseHandle(w->dir); // unblocks the pending ReadDirectoryChangesW
            w->dir = INVALID_HANDLE_VALUE;
        }
        if (w->thread.joinable()) {
            w->thread.join();
        }
    }
}

bool LibraryManager::isVideoExtension(const std::wstring& ext) {
    const std::wstring e = lower(ext);
    for (const wchar_t* k : kVideoExtensions) {
        if (e == k) {
            return true;
        }
    }
    return false;
}

bool LibraryManager::isVideoFile(const std::wstring& path) {
    const std::wstring ext = std::filesystem::path(path).extension().wstring();
    return isVideoExtension(ext);
}

void LibraryManager::collectVideoFiles(const std::wstring& folder,
                                       std::vector<std::wstring>& out) {
    std::error_code ec;
    for (std::filesystem::recursive_directory_iterator it(folder, ec), end; it != end;
         it.increment(ec)) {
        if (ec) {
            break; // unreadable subtree — skip
        }
        if (it->is_regular_file(ec) && isVideoFile(it->path().wstring())) {
            out.push_back(it->path().wstring());
        }
    }
}

void LibraryManager::emit(const vw::ui::LibraryChangeNotification& n) {
    if (sink_) {
        sink_(n);
    }
}

void LibraryManager::rebuildIndex() {
    itemIndex_.clear();
    for (size_t i = 0; i < items_.size(); ++i) {
        std::error_code ec;
        const auto canon = std::filesystem::weakly_canonical(items_[i].path, ec).wstring();
        itemIndex_[canon] = i;
    }
}

// Exact file size + last-write FILETIME via Win32 (no filesystem-clock
// conversion — MSVC's file clock lacks a portable to_sys; GetFileAttributesExW
// returns the 100 ns since 1601 ticks directly).
bool fileStat(const std::wstring& path, uint64_t& size, uint64_t& lastWriteTicks) {
    WIN32_FILE_ATTRIBUTE_DATA wfad{};
    if (!::GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &wfad)) {
        return false;
    }
    size = (static_cast<uint64_t>(wfad.nFileSizeHigh) << 32) | wfad.nFileSizeLow;
    lastWriteTicks = (static_cast<uint64_t>(wfad.ftLastWriteTime.dwHighDateTime) << 32) |
                     wfad.ftLastWriteTime.dwLowDateTime;
    return true;
}

void LibraryManager::addItemInternal(const std::wstring& path,
                                     std::vector<vw::ui::LibraryItemId>& added) {
    if (!isVideoFile(path)) {
        return; // only video extensions enter the library (probe validates content)
    }
    std::error_code ec;
    const auto status = std::filesystem::status(path, ec);
    if (ec || !std::filesystem::is_regular_file(status)) {
        return; // stale/deleted — skip silently (watch events race renames)
    }
    const auto canon = std::filesystem::weakly_canonical(path, ec).wstring();
    if (itemIndex_.count(canon) != 0) {
        return; // duplicate
    }
    vw::ui::LibraryItem item;
    item.id = nextId();
    item.path = path;
    if (!fileStat(path, item.fileSize, item.lastWriteTicks)) {
        return;
    }
    items_.push_back(std::move(item));
    itemIndex_[canon] = items_.size() - 1;
    added.push_back(items_.back().id);
}

std::vector<vw::ui::LibraryItemId> LibraryManager::addFiles(
    const std::vector<std::wstring>& paths) {
    std::vector<vw::ui::LibraryItemId> added;
    for (const auto& p : paths) {
        addItemInternal(p, added);
    }
    if (!added.empty()) {
        vw::ui::LibraryChangeNotification n;
        n.kind = vw::ui::LibraryChangeKind::Added;
        n.ids = added;
        emit(n);
    }
    return added;
}

void LibraryManager::scanFolderInto(const std::wstring& folder,
                                    std::vector<std::wstring>& out) {
    collectVideoFiles(folder, out);
}

void LibraryManager::addFolder(const std::wstring& folder) {
    std::error_code ec;
    if (!std::filesystem::is_directory(folder, ec)) {
        log::Logger::instance().warn(L"library: add folder failed (not a directory): {}",
                                     folder);
        return;
    }
    std::vector<std::wstring> files;
    collectVideoFiles(folder, files);
    std::vector<vw::ui::LibraryItemId> added;
    for (const auto& f : files) {
        addItemInternal(f, added);
    }
    startWatch(folder);
    if (!added.empty()) {
        vw::ui::LibraryChangeNotification n;
        n.kind = vw::ui::LibraryChangeKind::Added;
        n.ids = added;
        emit(n);
    }
}

void LibraryManager::startWatch(const std::wstring& folder) {
    if (std::find(watchedFolders_.begin(), watchedFolders_.end(), folder) !=
        watchedFolders_.end()) {
        return; // already watched
    }
    auto watch = std::make_unique<Watch>();
    watch->folder = folder;
    watch->owner = this;
    watch->buffer.resize(64 * 1024);
    watch->dir = ::CreateFileW(folder.c_str(), FILE_LIST_DIRECTORY,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                               OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (watch->dir == INVALID_HANDLE_VALUE) {
        log::Logger::instance().warn(L"library: cannot watch folder ({}): {}", folder,
                                     ::GetLastError());
        return;
    }
    Watch* raw = watch.get();
    watch->thread = std::thread([raw] {
        DWORD bytes = 0;
        while (!raw->stop) {
            const BOOL ok = ::ReadDirectoryChangesW(
                raw->dir, raw->buffer.data(), static_cast<DWORD>(raw->buffer.size()), TRUE,
                FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE |
                    FILE_NOTIFY_CHANGE_SIZE,
                &bytes, nullptr, nullptr);
            if (!ok || bytes == 0) {
                break; // handle closed (stop) or watch failed
            }
            // Parse the notify records into pending events.
            size_t offset = 0;
            while (offset + sizeof(FILE_NOTIFY_INFORMATION) <= bytes) {
                const auto* info =
                    reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(raw->buffer.data() + offset);
                const size_t nameLen =
                    std::min<size_t>(info->FileNameLength, bytes - offset -
                                                              FIELD_OFFSET(FILE_NOTIFY_INFORMATION,
                                                                           FileName));
                std::wstring name(reinterpret_cast<const wchar_t*>(info->FileName),
                                  nameLen / sizeof(wchar_t));
                const std::wstring full = raw->folder + L"\\" + name;
                PendingEvent e;
                switch (info->Action) {
                    case FILE_ACTION_ADDED:
                    case FILE_ACTION_RENAMED_NEW_NAME:
                        e.kind = PendingEvent::Kind::Added;
                        break;
                    case FILE_ACTION_REMOVED:
                    case FILE_ACTION_RENAMED_OLD_NAME:
                        e.kind = PendingEvent::Kind::Removed;
                        break;
                    default:
                        e.kind = PendingEvent::Kind::Updated;
                        break;
                }
                e.path = full;
                std::lock_guard<std::mutex> lk(raw->owner->eventMu_);
                raw->owner->pendingEvents_.push_back(std::move(e));
                if (info->NextEntryOffset == 0) {
                    break;
                }
                offset += info->NextEntryOffset;
            }
        }
    });
    watchedFolders_.push_back(folder);
    watches_.push_back(std::move(watch));
}

void LibraryManager::remove(const std::vector<vw::ui::LibraryItemId>& ids) {
    std::vector<vw::ui::LibraryItemId> removed;
    for (const vw::ui::LibraryItemId id : ids) {
        const auto it = std::find_if(items_.begin(), items_.end(),
                                     [&](const vw::ui::LibraryItem& i) { return i.id == id; });
        if (it == items_.end()) {
            continue;
        }
        std::error_code ec;
        const auto canon = std::filesystem::weakly_canonical(it->path, ec).wstring();
        itemIndex_.erase(canon);
        removed.push_back(id);
        items_.erase(it);
    }
    // The erasures shifted positions — rebuild the path->index map (the old
    // indices would point at the wrong item / out of bounds).
    rebuildIndex();

    // Prune watches whose folder no longer holds any library item (the watch
    // thread would otherwise linger on a folder the user removed everything
    // from — a handle + thread leak).
    for (size_t i = watches_.size(); i-- > 0;) {
        const auto& w = watches_[i];
        const bool stillUsed = std::any_of(
            items_.begin(), items_.end(), [&](const vw::ui::LibraryItem& item) {
                std::error_code ec;
                const std::wstring p = std::filesystem::weakly_canonical(item.path, ec).wstring();
                // Folder prefix at a path-separator boundary ("C:\\Videos"
                // must not match "C:\\Videos2\\x.mp4").
                if (p.rfind(w->folder, 0) != 0) {
                    return false;
                }
                return p.size() == w->folder.size() ||
                       p[w->folder.size()] == L'\\' || p[w->folder.size()] == L'/';
            });
        if (!stillUsed) {
            stopWatch(w->folder);
        }
    }
    if (!removed.empty()) {
        vw::ui::LibraryChangeNotification n;
        n.kind = vw::ui::LibraryChangeKind::Removed;
        n.ids = removed;
        emit(n);
    }
}

void LibraryManager::stopWatch(const std::wstring& folder) {
    const auto it = std::find(watchedFolders_.begin(), watchedFolders_.end(), folder);
    if (it == watchedFolders_.end()) {
        return;
    }
    watchedFolders_.erase(it);
    const auto w = std::find_if(watches_.begin(), watches_.end(),
                                [&](const auto& w) { return w->folder == folder; });
    if (w != watches_.end()) {
        (*w)->stop = true;
        if ((*w)->dir != INVALID_HANDLE_VALUE) {
            ::CancelSynchronousIo((*w)->thread.native_handle());
            ::CloseHandle((*w)->dir); // unblocks the pending ReadDirectoryChangesW
            (*w)->dir = INVALID_HANDLE_VALUE;
        }
        if ((*w)->thread.joinable()) {
            (*w)->thread.join();
        }
        watches_.erase(w);
    }
}

void LibraryManager::refresh() {
    vw::ui::LibraryChangeNotification start;
    start.kind = vw::ui::LibraryChangeKind::RescanStarted;
    emit(start);

    // 1) Re-stat every item: mark size/lastWrite changes.
    std::vector<vw::ui::LibraryItemId> updated;
    for (auto& item : items_) {
        std::error_code ec;
        if (!std::filesystem::is_regular_file(std::filesystem::status(item.path, ec))) {
            continue; // deleted — the watch (or the next refresh) removes it
        }
        uint64_t fsize = 0, ticks = 0;
        if (!fileStat(item.path, fsize, ticks)) {
            continue;
        }
        if (fsize != item.fileSize || ticks != item.lastWriteTicks) {
            item.fileSize = fsize;
            item.lastWriteTicks = ticks;
            item.metadataLoaded = false; // content changed — metadata is stale
            updated.push_back(item.id);
        }
    }
    if (!updated.empty()) {
        vw::ui::LibraryChangeNotification n;
        n.kind = vw::ui::LibraryChangeKind::Updated;
        n.ids = updated;
        emit(n);
    }

    // 2) Discover NEW files in watched folders (incremental — only watched
    //    folders are enumerated; never a timer-driven full rescan).
    std::vector<vw::ui::LibraryItemId> added;
    for (const auto& folder : watchedFolders_) {
        std::vector<std::wstring> files;
        collectVideoFiles(folder, files);
        for (const auto& f : files) {
            addItemInternal(f, added);
        }
    }
    if (!added.empty()) {
        vw::ui::LibraryChangeNotification n;
        n.kind = vw::ui::LibraryChangeKind::Added;
        n.ids = added;
        emit(n);
    }

    vw::ui::LibraryChangeNotification finish;
    finish.kind = vw::ui::LibraryChangeKind::RescanFinished;
    emit(finish);
}

const vw::ui::LibraryItem* LibraryManager::itemById(vw::ui::LibraryItemId id) const {
    const auto it = std::find_if(items_.begin(), items_.end(),
                                 [&](const vw::ui::LibraryItem& i) { return i.id == id; });
    return it == items_.end() ? nullptr : &*it;
}

bool LibraryManager::requestMetadata(vw::ui::LibraryItemId id) {
    // Resolve id -> path on the CONTROL thread (the worker never sees items_).
    const auto* item = itemById(id);
    if (!item || item->metadataLoaded) {
        return false;
    }
    std::lock_guard<std::mutex> lk(probeMu_);
    if (std::find(probeQueue_.begin(), probeQueue_.end(), item->path) != probeQueue_.end() ||
        std::find(probeInFlight_.begin(), probeInFlight_.end(), item->path) != probeInFlight_.end()) {
        return false; // already pending (queued or currently being probed)
    }
    probeQueue_.push_back(item->path);
    probeCv_.notify_one();
    return true;
}

void LibraryManager::probeLoop() {
    for (;;) {
        std::wstring path;
        {
            std::unique_lock<std::mutex> lk(probeMu_);
            probeCv_.wait(lk, [this] { return probeStop_ || !probeQueue_.empty(); });
            if (probeStop_ && probeQueue_.empty()) {
                return;
            }
            path = probeQueue_.front();
            probeQueue_.erase(probeQueue_.begin());
            probeInFlight_.push_back(path);
        }
        // Probe OUTSIDE the lock (Source Reader open is slow) and WITHOUT
        // touching items_ (control-thread owned).
        ProbeResult result;
        result.path = path;
        auto meta = video::DecoderManager::probeMetadata(path);
        if (meta) {
            result.ok = true;
            result.meta = std::move(*meta);
        }
        std::lock_guard<std::mutex> lk(probeMu_);
        const auto it = std::find(probeInFlight_.begin(), probeInFlight_.end(), path);
        if (it != probeInFlight_.end()) {
            probeInFlight_.erase(it);
        }
        probeResults_.push_back(std::move(result));
    }
}

void LibraryManager::pollChangeEvents() {
    // Watch events -> item mutations (control thread).
    {
        std::vector<PendingEvent> events;
        {
            std::lock_guard<std::mutex> lk(eventMu_);
            events.swap(pendingEvents_);
        }
        if (!events.empty()) {
            std::vector<vw::ui::LibraryItemId> added, removed, updated;
            for (const auto& e : events) {
                std::error_code ec;
                if (e.kind == PendingEvent::Kind::Added) {
                    if (isVideoFile(e.path) && std::filesystem::is_regular_file(e.path, ec)) {
                        const auto canon = std::filesystem::weakly_canonical(e.path, ec).wstring();
                        if (itemIndex_.count(canon) == 0) {
                            addItemInternal(e.path, added);
                        }
                    }
                } else if (e.kind == PendingEvent::Kind::Removed) {
                    const auto canon = std::filesystem::weakly_canonical(e.path, ec).wstring();
                    const auto it = itemIndex_.find(canon);
                    if (it != itemIndex_.end()) {
                        removed.push_back(items_[it->second].id);
                        items_.erase(items_.begin() + static_cast<ptrdiff_t>(it->second));
                        // itemIndex_ rebuilt below (positions shifted).
                    }
                } else { // Updated / modified
                    const auto canon = std::filesystem::weakly_canonical(e.path, ec).wstring();
                    const auto it = itemIndex_.find(canon);
                    if (it != itemIndex_.end()) {
                        auto& item = items_[it->second];
                        uint64_t fsize = 0, ticks = 0;
                        if (fileStat(item.path, fsize, ticks) && fsize != item.fileSize) {
                            item.fileSize = fsize;
                            item.lastWriteTicks = ticks;
                            item.metadataLoaded = false;
                            updated.push_back(item.id);
                        }
                    }
                }
            }
            if (!removed.empty()) {
                rebuildIndex(); // the watch removed items — positions shifted
            }
            if (!added.empty()) {
                vw::ui::LibraryChangeNotification n;
                n.kind = vw::ui::LibraryChangeKind::Added;
                n.ids = added;
                emit(n);
            }
            if (!removed.empty()) {
                vw::ui::LibraryChangeNotification n;
                n.kind = vw::ui::LibraryChangeKind::Removed;
                n.ids = removed;
                emit(n);
            }
            if (!updated.empty()) {
                vw::ui::LibraryChangeNotification n;
                n.kind = vw::ui::LibraryChangeKind::Updated;
                n.ids = updated;
                emit(n);
            }
        }
    }

    // Metadata probe results -> item metadata + Updated. The worker carried
    // the probed metadata back — the control thread never re-probes (a Source
    // Reader open on the UI thread would stutter the window).
    {
        std::vector<ProbeResult> results;
        {
            std::lock_guard<std::mutex> lk(probeMu_);
            results.swap(probeResults_);
        }
        std::vector<vw::ui::LibraryItemId> updated;
        for (auto& r : results) {
            // Resolve path -> item on the CONTROL thread. The item may have
            // been removed while the probe ran — drop the result then.
            std::error_code ec;
            const auto canon = std::filesystem::weakly_canonical(r.path, ec).wstring();
            const auto it = itemIndex_.find(canon);
            if (it != itemIndex_.end() && r.ok) {
                auto& item = items_[it->second];
                item.metadata.width = r.meta.width;
                item.metadata.height = r.meta.height;
                item.metadata.frameRate = r.meta.fps;
                item.metadata.durationSeconds =
                    static_cast<double>(r.meta.duration100ns) / 10'000'000.0;
                item.metadata.codec = codecFromName(r.meta.codec);
                item.metadata.hdr = r.meta.hdr;
                item.metadata.hasAudio = r.meta.hasAudio;
                item.metadataLoaded = true;
                updated.push_back(item.id);
            }
            // Probe failed: metadata stays unknown (listed, unreadable).
            ++probeResultsCount_;
        }
        if (!updated.empty()) {
            vw::ui::LibraryChangeNotification n;
            n.kind = vw::ui::LibraryChangeKind::Updated;
            n.ids = updated;
            emit(n);
        }
    }
}

} // namespace vw::library
