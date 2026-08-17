#include "doctest.h"

#include "graphics/ScaleMath.h"

using vw::gfx::computeScaleOffset;
using vw::gfx::ScaleOffset;
using vw::gfx::Scaling;
using vw::gfx::scalingNeedsBorder;

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
}

TEST_CASE("scale: BORDER sampler needed exactly for the letterbox modes") {
    CHECK(scalingNeedsBorder(Scaling::Fit));
    CHECK(scalingNeedsBorder(Scaling::Center));
    CHECK_FALSE(scalingNeedsBorder(Scaling::Fill));
    CHECK_FALSE(scalingNeedsBorder(Scaling::Stretch));
}
