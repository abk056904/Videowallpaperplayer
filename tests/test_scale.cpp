#include "doctest.h"

#include "graphics/ScaleMath.h"

using vw::gfx::computeScaleOffset;
using vw::gfx::ScaleOffset;
using vw::gfx::Scaling;
using vw::gfx::scalingNeedsBorder;
using vw::gfx::videoAspectFor;

// Expected values are derived at pixel level (M5 review): e.g. window
// 1920x1080 + video 640x480 under Fill = cover scales video to 1920x1440 and
// shows rows 60..420 of 480 -> sy = 0.75, oy = 0.125; the equivalent Fit
// letterboxes to 1440x1080 -> sx = 1.333.., ox = -0.166.., with the margins
// sampled outside [0,1] (black bars via the BORDER sampler).

TEST_CASE("scale: Fill covers the window, cropping the overflow") {
    // Window wider than the video: fills width exactly, crops height.
    ScaleOffset so = computeScaleOffset(1920, 1080, 640, 480, Scaling::Fill);
    CHECK(so.sx == doctest::Approx(1.0f));
    CHECK(so.ox == doctest::Approx(0.0f));
    CHECK(so.sy == doctest::Approx(0.75f)); // visible 75% of the video height
    CHECK(so.oy == doctest::Approx(0.125f));

    // Video wider than the window: fills height exactly, crops width.
    so = computeScaleOffset(640, 480, 1920, 1080, Scaling::Fill);
    CHECK(so.sx == doctest::Approx(0.75f));
    CHECK(so.ox == doctest::Approx(0.125f));
    CHECK(so.sy == doctest::Approx(1.0f));
    CHECK(so.oy == doctest::Approx(0.0f));
}

TEST_CASE("scale: Fit letterboxes, showing the whole frame") {
    // Window wider: width is the limited axis (scale > 1 -> centered band).
    ScaleOffset so = computeScaleOffset(1920, 1080, 640, 480, Scaling::Fit);
    CHECK(so.sx == doctest::Approx(4.0f / 3.0f));
    CHECK(so.ox == doctest::Approx(-1.0f / 6.0f));
    CHECK(so.sy == doctest::Approx(1.0f));
    CHECK(so.oy == doctest::Approx(0.0f));

    // Video wider: height is the limited axis.
    so = computeScaleOffset(640, 480, 1920, 1080, Scaling::Fit);
    CHECK(so.sx == doctest::Approx(1.0f));
    CHECK(so.ox == doctest::Approx(0.0f));
    CHECK(so.sy == doctest::Approx(4.0f / 3.0f));
    CHECK(so.oy == doctest::Approx(-1.0f / 6.0f));
}

TEST_CASE("scale: Stretch is the identity mapping") {
    const ScaleOffset so = computeScaleOffset(1920, 1080, 640, 480, Scaling::Stretch);
    CHECK(so.sx == doctest::Approx(1.0f));
    CHECK(so.sy == doctest::Approx(1.0f));
    CHECK(so.ox == doctest::Approx(0.0f));
    CHECK(so.oy == doctest::Approx(0.0f));
}

TEST_CASE("scale: Center maps 1:1 by pixel dimensions") {
    // Video smaller than the window: shown at native size with margins.
    ScaleOffset so = computeScaleOffset(1920, 1080, 640, 480, Scaling::Center);
    CHECK(so.sx == doctest::Approx(1920.0f / 640.0f)); // 3
    CHECK(so.ox == doctest::Approx(-1.0f));
    CHECK(so.sy == doctest::Approx(1080.0f / 480.0f)); // 2.25
    CHECK(so.oy == doctest::Approx(-0.625f));

    // Video bigger than the window: the window shows the center crop.
    so = computeScaleOffset(1920, 1080, 3840, 2160, Scaling::Center);
    CHECK(so.sx == doctest::Approx(0.5f));
    CHECK(so.ox == doctest::Approx(0.25f));
    CHECK(so.sy == doctest::Approx(0.5f));
    CHECK(so.oy == doctest::Approx(0.25f));

    // Same aspect, bigger video: center crop at 1:1 (not Fit!).
    so = computeScaleOffset(1920, 1080, 2560, 1440, Scaling::Center);
    CHECK(so.sx == doctest::Approx(0.75f));
    CHECK(so.ox == doctest::Approx(0.125f));
    CHECK(so.sy == doctest::Approx(0.75f));
    CHECK(so.oy == doctest::Approx(0.125f));
}

TEST_CASE("scale: matching aspects give the identity mapping") {
    // 1920x1080 window, 3840x2160 video (both 16:9): Fill and Fit are identity.
    const ScaleOffset fill = computeScaleOffset(1920, 1080, 3840, 2160, Scaling::Fill);
    CHECK(fill.sx == doctest::Approx(1.0f));
    CHECK(fill.sy == doctest::Approx(1.0f));
    CHECK(fill.ox == doctest::Approx(0.0f));
    CHECK(fill.oy == doctest::Approx(0.0f));

    const ScaleOffset fit = computeScaleOffset(1920, 1080, 3840, 2160, Scaling::Fit);
    CHECK(fit.sx == doctest::Approx(1.0f));
    CHECK(fit.sy == doctest::Approx(1.0f));
    CHECK(fit.ox == doctest::Approx(0.0f));
    CHECK(fit.oy == doctest::Approx(0.0f));
}

TEST_CASE("scale: zero sizes are guarded (identity, no division by zero)") {
    const ScaleOffset so = computeScaleOffset(0, 0, 0, 0, Scaling::Fit);
    CHECK(so.sx == doctest::Approx(1.0f));
    CHECK(so.sy == doctest::Approx(1.0f));
    CHECK(so.ox == doctest::Approx(0.0f));
    CHECK(so.oy == doctest::Approx(0.0f));

    // Zero display aspect is also guarded (identity).
    const ScaleOffset zeroAspect = computeScaleOffset(1920, 1080, 0.0f, Scaling::Fill);
    CHECK(zeroAspect.sx == doctest::Approx(1.0f));
    CHECK(zeroAspect.sy == doctest::Approx(1.0f));
}

TEST_CASE("scale: videoAspectFor prefers the SAR-corrected display aspect") {
    // 720x480 anamorphic DVD: raw pixel aspect is 3:2, but the SAR (10:11)
    // makes the display 15:11 = 1.3636. The scaling math must consume the
    // SAR-corrected aspect, not the raw 1.5.
    const float dvd = videoAspectFor(720, 480, 720.0f * 10.0f / 11.0f / 480.0f);
    CHECK(dvd == doctest::Approx(15.0f / 11.0f)); // 1.3636…

    // Fallback: display aspect unknown (0) -> raw pixel aspect.
    const float raw = videoAspectFor(720, 480, 0.0f);
    CHECK(raw == doctest::Approx(1.5f));

    // Degenerate display aspect is rejected -> raw pixel aspect.
    const float garbage = videoAspectFor(720, 480, -5.0f);
    CHECK(garbage == doctest::Approx(1.5f));

    // Nothing usable at all -> 0 (renderer then keeps the identity mapping).
    CHECK(videoAspectFor(0, 0, 0.0f) == doctest::Approx(0.0f));
}

TEST_CASE("scale: anamorphic Fill crops to the DISPLAY aspect, not the pixels") {
    // 1920x1080 window (16:9) + 720x480 anamorphic video whose display is 4:3
    // (SAR 10:11). Fill: the window is WIDER than the 4:3 video, so the video
    // fills the width exactly and its HEIGHT is cropped evenly top/bottom —
    // the "crop evenly both sides, fill the screen, no bars" rule. With the
    // raw 3:2 pixel aspect the same call would crop far less height (0.5625
    // visible) — the wrong (distorting) geometry.
    const float displayAspect = 4.0f / 3.0f;
    const ScaleOffset fill = computeScaleOffset(1920, 1080, displayAspect, Scaling::Fill);
    CHECK(fill.sx == doctest::Approx(1.0f));  // fills the width exactly
    CHECK(fill.ox == doctest::Approx(0.0f));
    CHECK(fill.sy == doctest::Approx(displayAspect / (16.0f / 9.0f))); // < 1: height cropped
    CHECK(fill.sy == doctest::Approx(0.75f)); // 16:9 shows 75% of the 4:3 height
    CHECK(fill.oy == doctest::Approx(0.125f)); // cropped evenly top/bottom

    // Sanity: the same video WITHOUT SAR correction (old behavior) computes
    // the crop from the raw 3:2 pixels — visible 0.5625 of the height, wrong.
    const ScaleOffset raw = computeScaleOffset(1920, 1080, 720.0f / 480.0f, Scaling::Fill);
    CHECK(raw.sy == doctest::Approx(0.84375f));
    CHECK(raw.oy == doctest::Approx(0.078125f));

    // Fit with the same display aspect letterboxes the 4:3 into the 16:9
    // window (sides): sx > 1, centered band.
    const ScaleOffset fit = computeScaleOffset(1920, 1080, displayAspect, Scaling::Fit);
    CHECK(fit.sx == doctest::Approx((16.0f / 9.0f) / displayAspect)); // 4/3
    CHECK(fit.ox == doctest::Approx((1.0f - fit.sx) * 0.5f));
    CHECK(fit.sy == doctest::Approx(1.0f));
    CHECK(fit.oy == doctest::Approx(0.0f));
}

TEST_CASE("scale: BORDER sampler needed exactly for the letterbox modes") {
    CHECK(scalingNeedsBorder(Scaling::Fit));
    CHECK(scalingNeedsBorder(Scaling::Center));
    CHECK_FALSE(scalingNeedsBorder(Scaling::Fill));
    CHECK_FALSE(scalingNeedsBorder(Scaling::Stretch));
}
