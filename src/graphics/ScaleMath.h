#pragma once

#include <cstdint>

// Pure texture-scaling math shared by D3D11Renderer and the unit tests
// (header-only so tests need no D3D linkage). The renderer maps the
// fullscreen window UV [0,1]^2 onto the video texture UV via
//   texUv = windowUv * (sx, sy) + (ox, oy)
// and this header computes (sx, sy, ox, oy) for each scaling mode. Derived
// and verified at pixel level (docs/02 §2.4, M5 review):
//
//   Fill    (cover): video scaled so the whole window is covered, overflow
//           cropped. One axis is exactly 1.0 (fills), the other < 1.0 (the
//           visible fraction of the video along the crop axis). Never samples
//           outside [0,1] -> the CLAMP sampler is correct.
//   Fit     (contain): whole frame visible, letterboxed. One axis is 1.0, the
//           other > 1.0 (the video occupies a sub-region of the window along
//           that axis) -> the margin samples outside [0,1] and must show
//           black (BORDER sampler).
//   Stretch: full frame, aspect ignored. Identity.
//   Center  (1:1): video at native pixel size, centered. The window shows a
//           window-sized region of the video: sx = winW/vidW, sy = winH/vidH
//           (> 1.0 when the video is smaller than the window -> margins).
//
// Offsets are always (1 - s)/2 (centering); letterbox margins are the
// out-of-range regions of the > 1.0 axis.

namespace vw::gfx {

enum class Scaling { Fill, Fit, Stretch, Center };

struct ScaleOffset {
    float sx; // texture UV scale x
    float sy; // texture UV scale y
    float ox; // texture UV offset x
    float oy; // texture UV offset y
};

// True when the mode can sample outside the texture UV square, in which case
// the renderer must use a BORDER (black) sampler instead of CLAMP so letterbox
// margins render black rather than smearing the video edge.
constexpr bool scalingNeedsBorder(Scaling mode) {
    return mode == Scaling::Fit || mode == Scaling::Center;
}

// The display aspect ratio a frame presents with: width/height corrected by
// the sample aspect ratio (anamorphic content — non-square pixels). `display`
// is the SAR-corrected aspect carried by the frame (0 when unknown); the raw
// pixel dims are the fallback. Returns 0 when nothing is usable. This is what
// the scaling math consumes so crop/scale stays universal (docs/02 §2.4).
inline float videoAspectFor(std::uint32_t vidW, std::uint32_t vidH, float displayAspect) {
    if (displayAspect > 0.0f && displayAspect < 100.0f) {
        return displayAspect;
    }
    if (vidW > 0 && vidH > 0) {
        return static_cast<float>(vidW) / static_cast<float>(vidH);
    }
    return 0.0f;
}

// Computes the window->texture UV mapping for the given scaling mode.
// winW/winH: window (back buffer) size; vidAspect: the video's DISPLAY aspect
// ratio (SAR-corrected, see videoAspectFor). Guarded against zero sizes
// (returns identity, matching the renderer's pre-existing defensive
// behavior).
inline ScaleOffset computeScaleOffset(std::uint32_t winW, std::uint32_t winH, float vidAspect,
                                      Scaling mode) {
    ScaleOffset out{1.0f, 1.0f, 0.0f, 0.0f};
    if (winW == 0 || winH == 0 || vidAspect <= 0.0f) {
        return out;
    }
    const float winAspect = static_cast<float>(winW) / static_cast<float>(winH);
    switch (mode) {
        case Scaling::Stretch: // full frame, aspect ignored
            break;
        case Scaling::Fit: // contain: whole frame visible, letterboxed
            if (winAspect > vidAspect) { // window wider: width-limited
                out.sx = winAspect / vidAspect; // > 1: video occupies a centered band
            } else {
                out.sy = vidAspect / winAspect;
            }
            break;
        case Scaling::Fill: // cover: frame fills the window, overflow cropped
            if (winAspect > vidAspect) { // window wider: height-limited
                out.sy = vidAspect / winAspect; // < 1: visible fraction of the height
            } else {
                out.sx = winAspect / vidAspect;
            }
            break;
        case Scaling::Center: // 1:1 native pixels — needs the pixel dims the
            break;            // width/height overload below provides; the
                              // aspect overload (renderer) never uses Center.
    }
    out.ox = (1.0f - out.sx) * 0.5f;
    out.oy = (1.0f - out.sy) * 0.5f;
    return out;
}

// Width/height overload (square-pixel content, no SAR correction needed —
// used by the unit tests and any caller without a display aspect). Center
// keeps its native-pixel meaning here (vidW/vidH available).
inline ScaleOffset computeScaleOffset(std::uint32_t winW, std::uint32_t winH,
                                      std::uint32_t vidW, std::uint32_t vidH,
                                      Scaling mode) {
    if (mode == Scaling::Center) {
        ScaleOffset out{1.0f, 1.0f, 0.0f, 0.0f};
        if (winW == 0 || winH == 0 || vidW == 0 || vidH == 0) {
            return out;
        }
        out.sx = static_cast<float>(winW) / static_cast<float>(vidW);
        out.sy = static_cast<float>(winH) / static_cast<float>(vidH);
        out.ox = (1.0f - out.sx) * 0.5f;
        out.oy = (1.0f - out.sy) * 0.5f;
        return out;
    }
    return computeScaleOffset(winW, winH, videoAspectFor(vidW, vidH, 0.0f), mode);
}

} // namespace vw::gfx
