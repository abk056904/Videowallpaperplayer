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

// Computes the window->texture UV mapping for the given scaling mode.
// winW/winH: window (back buffer) size; vidW/vidH: video frame size.
// Guarded against zero sizes (returns identity, matching the renderer's
// pre-existing defensive behavior).
inline ScaleOffset computeScaleOffset(std::uint32_t winW, std::uint32_t winH,
                                      std::uint32_t vidW, std::uint32_t vidH,
                                      Scaling mode) {
    ScaleOffset out{1.0f, 1.0f, 0.0f, 0.0f};
    if (winW == 0 || winH == 0 || vidW == 0 || vidH == 0) {
        return out;
    }
    const float winAspect = static_cast<float>(winW) / static_cast<float>(winH);
    const float vidAspect = static_cast<float>(vidW) / static_cast<float>(vidH);
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
        case Scaling::Center: // 1:1 native pixels, centered
            out.sx = static_cast<float>(winW) / static_cast<float>(vidW);
            out.sy = static_cast<float>(winH) / static_cast<float>(vidH);
            break;
    }
    out.ox = (1.0f - out.sx) * 0.5f;
    out.oy = (1.0f - out.sy) * 0.5f;
    return out;
}

} // namespace vw::gfx
