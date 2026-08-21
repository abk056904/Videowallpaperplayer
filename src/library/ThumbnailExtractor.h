#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "util/Result.h"

namespace vw::library {

// Extracts a single frame from a video file as a thumbnail image (BMP).
// Uses FFmpeg's software decoder (no GPU required) and runs on a background
// worker thread. Thumbnails are cached as BMP files in AppData/VideoWallpaper/thumbnails/.
//
// Usage:
//   1. ThumbnailExtractor extractor;
//   2. extractor.setCacheDir(L"C:\\Users\\...\\AppData\\Roaming\\VideoWallpaper\\thumbnails");
//   3. extractor.request(videoPath, callback);  // async, callback(path, bmp) on completion
//   4. extractor.poll();  // drain completed results on the UI thread
class ThumbnailExtractor {
public:
    // Callback: (videoPath, success, thumbnailBmpPath)
    // On success, thumbnailBmpPath is the cached BMP file path.
    using Callback = std::function<void(const std::wstring& videoPath, bool success,
                                        const std::wstring& thumbnailPath)>;

    ThumbnailExtractor();
    ~ThumbnailExtractor();

    ThumbnailExtractor(const ThumbnailExtractor&) = delete;
    ThumbnailExtractor& operator=(const ThumbnailExtractor&) = delete;

    // Set the cache directory for thumbnail BMP files.
    void setCacheDir(const std::wstring& dir);

    // Request a thumbnail for the given video file.
    // If already cached, calls back immediately (on calling thread).
    // If pending, deduplicates (one request per path).
    // If not cached, queues for background extraction.
    void request(const std::wstring& videoPath, Callback cb);

    // Drain completed results — call from the UI thread to fire callbacks.
    void poll();

    // Number of pending extractions (test hook).
    size_t pendingCount() const;

    // Check if a thumbnail is already cached for this path.
    bool isCached(const std::wstring& videoPath) const;

    // Get the cache path for a video file's thumbnail.
    std::wstring cachePathFor(const std::wstring& videoPath) const;

    // Synchronous extraction (for tests / single-file use).
    static Result<std::wstring> extractSync(const std::wstring& videoPath,
                                            const std::wstring& outputPath);

private:
    void workerLoop();

    std::wstring cacheDir_;
    std::thread worker_;
    std::atomic<bool> stop_{false};

    struct Request {
        std::wstring videoPath;
        std::vector<Callback> callbacks;
    };
    struct Completed {
        std::wstring videoPath;
        bool success = false;
        std::wstring thumbnailPath;
        std::vector<Callback> callbacks;
    };

    mutable std::mutex mu_;
    std::vector<Request> queue_;
    std::vector<Completed> results_;
    std::vector<std::wstring> inFlight_; // paths being extracted (dedup)
};

} // namespace vw::library
