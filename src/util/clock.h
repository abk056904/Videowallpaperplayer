#pragma once

#include <cstdint>
#include <windows.h>

namespace vw::util {

// Monotonic high-resolution clock based on QueryPerformanceCounter.
// Used for all playback/frame timing (docs/02 §2.8).
class Clock {
public:
    static const Clock& instance() {
        static const Clock c;
        return c;
    }

    int64_t nowTicks() const {
        LARGE_INTEGER c;
        QueryPerformanceCounter(&c);
        return c.QuadPart;
    }

    double seconds() const { return ticksToSeconds(nowTicks()); }

    double ticksToSeconds(int64_t ticks) const {
        return static_cast<double>(ticks) / freq_.QuadPart;
    }

    // Monotonic wall time in 100 ns units — the same units as media timestamps
    // (DecodedFrame::timestamp) so frame deadlines and media time share one
    // clock (M6 FrameScheduler).
    int64_t now100ns() const {
        LARGE_INTEGER c;
        QueryPerformanceCounter(&c);
        return static_cast<int64_t>(static_cast<double>(c.QuadPart) / freq_.QuadPart * 1e7);
    }

private:
    Clock() { QueryPerformanceFrequency(&freq_); }
    LARGE_INTEGER freq_{};
};

} // namespace vw::util
