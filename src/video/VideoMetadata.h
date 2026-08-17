#pragma once

#include <cstdint>
#include <string>

// mfapi.h declares the MFVideoFormat_* GUID constants + MF_MT_* attributes
// that the inline helpers below reference — it must come first. (mfuuid.lib
// provides the GUID symbols at link time.)
#include <mfapi.h>
#include <mfobjects.h>
#include <mftransform.h>

#include "util/Result.h"

namespace vw::video {

// Validated media metadata (docs/02 §2.7 / docs/03 §3.6, M4). Read from the
// source reader's native media type + presentation descriptor — never from the
// file extension.
struct VideoMetadata {
    std::wstring path;
    GUID subtype{};          // raw MFVideoFormat_* GUID
    std::wstring codec;      // human-readable, e.g. L"H.264"
    UINT width = 0;
    UINT height = 0;
    double fps = 0.0;        // MF_MT_FRAME_RATE numerator/denominator
    int64_t duration100ns = 0; // MF_PD_DURATION (100 ns units)
    bool hasAudio = false;
    UINT bitDepth = 8;       // MF_MT_VIDEO_BIT_DEPTH, else inferred from subtype
    bool hdr = false;        // primaries/transfer indicate HDR, or bitDepth > 8
    // Sample aspect ratio (MF_MT_PIXEL_ASPECT_RATIO, packed UINT64:
    // HI32 = numerator, LO32 = denominator). 1:1 for square-pixel content.
    // Anamorphic files (e.g. 720x480 DVD, 4:3 SAR) have non-square pixels:
    // the DISPLAY aspect differs from width/height, and crop/scale must use
    // the display aspect or the video distorts (docs/02 §2.4, "fit screen").
    UINT sarNum = 1;
    UINT sarDen = 1;

    // Display aspect ratio = (width * sarNum) / (height * sarDen). 0 when
    // width/height are unknown. This is what the scaling math consumes.
    double displayAspect = 0.0;

    // Parses codec/size/fps/bit-depth/HDR from an MF media type. Separated so
    // unit tests can exercise it with synthetic media types (spec M4 exit).
    static Result<void> fillFromMediaType(IMFMediaType* type, VideoMetadata& out);

    static std::wstring subtypeName(const GUID& subtype);
    static UINT bitDepthFromSubtype(const GUID& subtype);
};

} // namespace vw::video
