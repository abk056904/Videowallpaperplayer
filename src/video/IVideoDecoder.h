#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "util/Result.h"
#include "video/DecodedFrame.h"
#include "video/VideoMetadata.h"

namespace vw::video {

class FrameQueue; // forward (defined in FrameQueue.h as class)

// Abstract video decoder interface. All backends (MF, NVDEC/CUDA/D3D11VA,
// FFmpeg software) implement this. The renderer consumes DecodedFrame without
// knowing which backend produced it. Selected at runtime by CreateBestDecoder()
// in DecoderFactory.h.
class IVideoDecoder {
public:
    virtual ~IVideoDecoder() = default;

    IVideoDecoder(const IVideoDecoder&) = delete;
    IVideoDecoder& operator=(const IVideoDecoder&) = delete;
    IVideoDecoder() = default;

    // Open a video file. Returns error on failure.
    virtual Result<void> open(const std::wstring& path) = 0;

    // Start decoding. queue is owned by the caller.
    virtual Result<void> start(FrameQueue* queue, LONGLONG position100ns = 0) = 0;

    // Stop decoding (may be called multiple times).
    virtual void stop() = 0;

    // Release all resources.
    virtual void close() = 0;

    // Query
    virtual bool isHardwareDecoding() const = 0;
    virtual const std::wstring& decoderName() const = 0;
    virtual const VideoMetadata& metadata() const = 0;
    virtual bool isOpen() const = 0;
    virtual uint64_t decodedFrames() const = 0;
};

} // namespace vw::video
