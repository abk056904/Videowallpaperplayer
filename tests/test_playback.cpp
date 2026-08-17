#include "doctest.h"

#include <chrono>
#include <filesystem>
#include <limits>
#include <thread>

#include <mfapi.h>
#include <windows.h>

#include "playback/FrameScheduler.h"
#include "playback/PlaybackController.h"
#include "video/FrameQueue.h"

namespace {

// The PlaybackController test needs Media Foundation + a clip on disk (the
// same skip pattern as the real-file tests in test_video.cpp).
struct MfScope {
    bool ok = false;
    MfScope() { ok = SUCCEEDED(::MFStartup(MF_VERSION)); }
    ~MfScope() {
        if (ok) {
            ::MFShutdown();
        }
    }
};

std::filesystem::path findTestClip() {
    const std::filesystem::path dir = L"C:/Users/mbk43/Videos/bgcmp";
    std::error_code ec;
    if (!std::filesystem::exists(dir, ec)) {
        return {};
    }
    for (const auto& e : std::filesystem::directory_iterator(dir, ec)) {
        if (e.is_regular_file(ec) && e.path().extension() == L".mp4") {
            return e.path();
        }
    }
    return {};
}

} // namespace

using vw::playback::FrameScheduler;
using vw::video::DecodedFrame;
using vw::video::FrameQueue;

TEST_CASE("scheduler: frame interval from source FPS") {
    FrameScheduler s;
    CHECK_FALSE(s.pacingEnabled());

    s.setSourceFps(30.0);
    CHECK(s.pacingEnabled());
    CHECK(s.sourceFps() == doctest::Approx(30.0));
    CHECK(s.interval100ns() == 10'000'000LL / 30); // 333333

    s.setSourceFps(60.0);
    CHECK(s.interval100ns() == 10'000'000LL / 60); // 166666

    s.setSourceFps(0.0); // broken/unknown metadata
    CHECK_FALSE(s.pacingEnabled());
    CHECK(s.interval100ns() == 0);
}

TEST_CASE("scheduler: due media time tracks wall time 1:1 from the anchor") {
    FrameScheduler s;
    s.setSourceFps(30.0);
    s.reset(1'000'000LL, 500'000LL); // media 500 ms is due at wall 1000 ms
    CHECK(s.dueMediaAt(1'000'000LL) == 500'000LL);
    CHECK(s.dueMediaAt(1'000'000LL + 333'333LL) == 500'000LL + 333'333LL);
    CHECK(s.dueMediaAt(2'000'000LL) == 1'500'000LL);
}

TEST_CASE("scheduler: pacing disabled means everything is due") {
    FrameScheduler s;
    s.reset(0, 0);
    CHECK(s.dueMediaAt(0) == (std::numeric_limits<LONGLONG>::max)());
    CHECK(s.dueMediaAt(1'000'000LL) == (std::numeric_limits<LONGLONG>::max)());
}

TEST_CASE("scheduler: steady-state cadence (decode keeps up)") {
    FrameScheduler s;
    s.setSourceFps(30.0);
    s.reset(0, 0);

    // The anchor frame (media ts 0) is due immediately.
    CHECK(s.msUntilNextDeadline(0) == 1);

    // Frame 0 presented at the anchor: frame 1 is due one interval out.
    s.advanceAfterPresent(0, 0);
    CHECK(s.msUntilNextDeadline(0) == 34); // 333333 ns -> 34 ms (rounded up)

    // Frame 1 presented exactly at its deadline: cadence holds (33.3 ms).
    s.advanceAfterPresent(333'333LL, 333'333LL);
    CHECK(s.msUntilNextDeadline(333'333LL) == 34);

    // Presenting EARLY (frame 2 at t=500000, its deadline is 666666) must not
    // shift the cadence: frame 3 is still due one media interval later
    // (deadline 999999 -> 50 ms away from t=500000).
    s.advanceAfterPresent(500'000LL, 666'666LL);
    CHECK(s.msUntilNextDeadline(500'000LL) == 50);
}

TEST_CASE("scheduler: slow decode snaps the deadline forward (no busy re-fire)") {
    FrameScheduler s;
    s.setSourceFps(30.0);
    s.reset(0, 0);

    // Frame 2 decoded late and presented 1 s in; the next deadline must be
    // relative to NOW, not the stale cadence (else the timer re-fires at a
    // past deadline in a tight loop).
    s.advanceAfterPresent(1'000'000LL, 666'666LL);
    CHECK(s.msUntilNextDeadline(1'000'000LL) == 34);
}

TEST_CASE("scheduler: idle wake re-arms relative to now") {
    FrameScheduler s;
    s.setSourceFps(30.0);
    s.reset(0, 0);

    // Deadline passed, nothing to present: re-arm one interval from now.
    s.advanceIdle(500'000LL);
    CHECK(s.msUntilNextDeadline(500'000LL) == 34);
    // A past deadline never yields a sub-1 ms arm.
    s.advanceIdle(0);
    CHECK(s.msUntilNextDeadline(0) >= 1);
}

TEST_CASE("queue: popNewestUpTo drops stale frames and counts them") {
    FrameQueue q(4);
    for (int i = 1; i <= 4; ++i) {
        DecodedFrame f;
        f.timestamp = i * 333'333LL;
        f.bytes.resize(4);
        CHECK(q.tryPush(std::move(f)));
    }
    CHECK(q.droppedFrames() == 0);

    // Frames 1 and 2 are due: the newest (2) wins, frame 1 is stale.
    DecodedFrame out;
    CHECK(q.popNewestUpTo(2 * 333'333LL, out));
    CHECK(out.timestamp == 2 * 333'333LL);
    CHECK(q.droppedFrames() == 1);

    // Frame 3 is not due yet: nothing to present, nothing dropped.
    CHECK_FALSE(q.popNewestUpTo(2 * 333'333LL, out));
    CHECK(q.droppedFrames() == 1);

    // Frame 3 becomes due and is presented.
    CHECK(q.popNewestUpTo(3 * 333'333LL, out));
    CHECK(out.timestamp == 3 * 333'333LL);
    CHECK(q.droppedFrames() == 1);

    // Frame 4 was popped as "due-but-future"? No: it is still queued.
    CHECK(q.size() == 1);
    CHECK(q.popNewestUpTo(4 * 333'333LL, out));
    CHECK(out.timestamp == 4 * 333'333LL);
    CHECK(q.size() == 0);
}

TEST_CASE("queue: EOS sentinel is delivered immediately, never stale-dropped") {
    FrameQueue q(2);
    DecodedFrame f;
    f.timestamp = 1'000'000LL;
    CHECK(q.tryPush(f));
    DecodedFrame eos;
    eos.endOfStream = true;
    CHECK(q.tryPush(eos));

    DecodedFrame out;
    // Not due yet: nothing (the real frame waits for its deadline).
    CHECK_FALSE(q.popNewestUpTo(500'000LL, out));
    // Due: the frame is presented.
    CHECK(q.popNewestUpTo(1'000'000LL, out));
    CHECK_FALSE(out.endOfStream);
    // EOS reaches the front and is delivered immediately regardless of due.
    CHECK(q.popNewestUpTo(0, out));
    CHECK(out.endOfStream);
    CHECK(q.droppedFrames() == 0); // EOS never counts against the staleness policy
}

TEST_CASE("queue: peekTimestamp reports the front frame's PTS (M6 review)") {
    FrameQueue q(3);
    CHECK_FALSE(q.peekTimestamp().has_value()); // empty

    DecodedFrame f;
    f.timestamp = 42'000'000LL; // nonzero initial PTS (edit-list / trimmed file)
    CHECK(q.tryPush(f));
    CHECK(q.peekTimestamp().has_value());
    CHECK(*q.peekTimestamp() == 42'000'000LL);

    // Popping the front advances the peek to the next frame.
    DecodedFrame out;
    CHECK(q.popNewestUpTo(42'000'000LL, out));
    CHECK_FALSE(q.peekTimestamp().has_value());

    // The EOS sentinel at the front reports nullopt (nothing to anchor to).
    DecodedFrame eos;
    eos.endOfStream = true;
    CHECK(q.tryPush(eos));
    CHECK_FALSE(q.peekTimestamp().has_value());

    // clear() empties the queue (EOS and all); peek is nullopt again, then
    // reports a newly pushed frame normally.
    q.clear();
    CHECK_FALSE(q.peekTimestamp().has_value());
    DecodedFrame g;
    g.timestamp = 1LL;
    CHECK(q.tryPush(g));
    CHECK(q.peekTimestamp().has_value());
    CHECK(*q.peekTimestamp() == 1LL);
}

TEST_CASE("queue: new-frame event signals on push and resets on clear") {
    FrameQueue q(2);
    CHECK(q.newFrameEvent() != nullptr);
    CHECK(::WaitForSingleObject(q.newFrameEvent(), 0) == WAIT_TIMEOUT);

    DecodedFrame f;
    f.bytes.resize(4);
    CHECK(q.tryPush(f));
    CHECK(::WaitForSingleObject(q.newFrameEvent(), 0) == WAIT_OBJECT_0);

    ::ResetEvent(q.newFrameEvent());
    q.clear();
    CHECK(::WaitForSingleObject(q.newFrameEvent(), 0) == WAIT_TIMEOUT); // reset by clear
    CHECK(q.droppedFrames() == 0);
}

TEST_CASE("queue: producer/consumer with popNewestUpTo across threads") {
    FrameQueue q(3);
    std::thread producer([&] {
        for (int i = 0; i < 100; ++i) {
            DecodedFrame f;
            f.timestamp = i * 1'000LL;
            q.push(std::move(f)); // blocks on the capacity-3 backpressure
        }
    });

    // Consume at a "freshness" cadence: every pop takes the newest due frame
    // and drops whatever it skips — nothing is lost or duplicated. The loop
    // WAITS for the producer (the consumer must never run ahead of it and
    // exit, or the producer blocks on a full queue with no consumer).
    uint64_t presented = 0;
    uint64_t dropped = 0;
    LONGLONG lastTimestamp = -1;
    LONGLONG due = 0;
    while (presented + dropped < 100) {
        due += 1'000LL;
        DecodedFrame out;
        if (q.popNewestUpTo(due, out)) {
            ++presented;
            dropped = q.droppedFrames();
            CHECK(out.timestamp > lastTimestamp); // never out of order
            lastTimestamp = out.timestamp;
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(1)); // let it catch up
        }
    }
    producer.join();
    CHECK(presented + dropped == 100); // every frame accounted for exactly once
    CHECK(lastTimestamp == 99 * 1'000LL); // newest frame eventually presented
}

TEST_CASE("PlaybackController: paces a real clip end to end (M6)") {
    MfScope mf;
    if (!mf.ok) {
        MESSAGE("MFStartup failed — skipping PlaybackController test");
        return;
    }
    const auto clip = findTestClip();
    if (clip.empty()) {
        MESSAGE("no test clips found — skipping PlaybackController test");
        return;
    }

    vw::playback::PlaybackController pc;
    auto opened = pc.open(clip.wstring(), nullptr, 3); // software path
    REQUIRE(opened);
    CHECK(pc.metadata().fps > 0.0);
    CHECK(pc.metadata().width > 0);

    REQUIRE(pc.start());
    CHECK(pc.state() == vw::playback::PlaybackController::State::Playing);

    // onWake() until the first frame is due (decode is slow on this machine,
    // so poll with the same cadence the message loop would wait on).
    std::optional<DecodedFrame> frame;
    for (int i = 0; i < 400 && !frame; ++i) {
        frame = pc.onWake();
        if (!frame) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    REQUIRE(frame);
    CHECK_FALSE(frame->endOfStream);
    CHECK(frame->width == pc.metadata().width);
    CHECK(pc.stats().presentedFrames >= 1);

    // Pause preserves the position; resume re-anchors and plays on.
    pc.pause();
    CHECK(pc.state() == vw::playback::PlaybackController::State::Paused);
    REQUIRE(pc.resume());
    CHECK(pc.state() == vw::playback::PlaybackController::State::Playing);

    pc.stop();
    CHECK(pc.state() == vw::playback::PlaybackController::State::Stopped);
    CHECK(pc.stats().presentedFrames >= 1);
}
