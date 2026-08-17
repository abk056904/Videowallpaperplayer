#include "video/VideoMetadata.h"

#include <mfapi.h>

namespace vw::video {

std::wstring VideoMetadata::subtypeName(const GUID& g) {
    if (g == MFVideoFormat_H264) return L"H.264";
    if (g == MFVideoFormat_HEVC || g == MFVideoFormat_HEVC_ES) return L"HEVC";
    if (g == MFVideoFormat_MP4V) return L"MPEG-4 Part 2";
    if (g == MFVideoFormat_MP43) return L"MPEG-4 v3";
    if (g == MFVideoFormat_MJPG) return L"MJPEG";
    if (g == MFVideoFormat_VP80) return L"VP8";
    if (g == MFVideoFormat_VP90) return L"VP9";
    if (g == MFVideoFormat_AV1) return L"AV1";
    if (g == MFVideoFormat_WMV3) return L"WMV3";
    if (g == MFVideoFormat_NV12) return L"NV12";
    if (g == MFVideoFormat_P010) return L"P010";
    if (g == MFVideoFormat_RGB32) return L"RGB32";
    return L"unknown";
}

UINT VideoMetadata::bitDepthFromSubtype(const GUID& g) {
    // 10-bit YUV family (P010, P016, Y210, Y216, v210, v216, Y410).
    if (g == MFVideoFormat_P010 || g == MFVideoFormat_P016 || g == MFVideoFormat_Y210 ||
        g == MFVideoFormat_Y216 || g == MFVideoFormat_v210 || g == MFVideoFormat_v216 ||
        g == MFVideoFormat_Y410) {
        return 10;
    }
    return 8;
}

Result<void> VideoMetadata::fillFromMediaType(IMFMediaType* type, VideoMetadata& out) {
    if (!type) {
        return std::unexpected(L"fillFromMediaType: null media type");
    }

    GUID subtype{};
    if (FAILED(type->GetGUID(MF_MT_SUBTYPE, &subtype))) {
        return std::unexpected(L"media type has no subtype");
    }
    out.subtype = subtype;
    out.codec = subtypeName(subtype);

    UINT32 w = 0, h = 0;
    if (SUCCEEDED(::MFGetAttributeSize(type, MF_MT_FRAME_SIZE, &w, &h))) {
        out.width = w;
        out.height = h;
    }

    UINT32 num = 0, den = 0;
    if (SUCCEEDED(::MFGetAttributeRatio(type, MF_MT_FRAME_RATE, &num, &den)) && num > 0 &&
        den > 0) {
        out.fps = static_cast<double>(num) / static_cast<double>(den);
    }

    // Sample aspect ratio (anamorphic correction). MF packs it as a UINT64:
    // HI32 = numerator, LO32 = denominator; MFGetAttributeRatio unpacks it.
    // Missing = square pixels (1:1). The display aspect drives the scaling
    // math so crop/scale is universal across square and non-square content.
    UINT32 sarNum = 0, sarDen = 0;
    if (SUCCEEDED(::MFGetAttributeRatio(type, MF_MT_PIXEL_ASPECT_RATIO, &sarNum, &sarDen)) &&
        sarNum > 0 && sarDen > 0) {
        out.sarNum = sarNum;
        out.sarDen = sarDen;
    }
    if (out.width > 0 && out.height > 0) {
        out.displayAspect =
            (static_cast<double>(out.width) * out.sarNum) / (static_cast<double>(out.height) * out.sarDen);
    }

    // Bit depth comes from the subtype family (P010/P016/... = 10-bit, else
    // 8). There is no MF_MT_VIDEO_BIT_DEPTH attribute in the platform headers
    // (checked against the 26100 SDK) — never read a nonexistent attribute.
    out.bitDepth = bitDepthFromSubtype(subtype);

    // HDR signal: 10+ bit, BT.2020 primaries, or PQ/HLG transfer functions.
    UINT32 primaries = 0, transfer = 0;
    type->GetUINT32(MF_MT_VIDEO_PRIMARIES, &primaries);
    type->GetUINT32(MF_MT_TRANSFER_FUNCTION, &transfer);
    out.hdr = out.bitDepth > 8 || primaries == MFVideoPrimaries_BT2020 ||
              transfer == MFVideoTransFunc_2084 || transfer == MFVideoTransFunc_HLG;
    return {};
}

} // namespace vw::video
