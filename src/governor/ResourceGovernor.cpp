#include "governor/ResourceGovernor.h"

#include <string>

#include "logging/Logger.h"
#include "playback/PlaybackController.h"

namespace vw::governor {

namespace {

std::chrono::steady_clock::time_point realNow() {
    return std::chrono::steady_clock::now();
}

const char* stateName(State s) {
    switch (s) {
        case State::Active: return "ACTIVE";
        case State::Paused: return "PAUSED";
        case State::Suspended: return "SUSPENDED";
    }
    return "?";
}

} // namespace

ResourceGovernor::ResourceGovernor(playback::PlaybackController& playback,
                                   PausePolicy::Config cfg)
    : playback_(playback), policy_(std::move(cfg)) {}

std::chrono::steady_clock::time_point ResourceGovernor::now() {
    return clock_ ? clock_() : realNow();
}

uint32_t ResourceGovernor::setReason(Reason reason, bool on) {
    const uint32_t oldMask = reasons_;
    if (on) {
        reasons_ |= reason;
    } else {
        reasons_ &= ~reason;
    }
    // Pass the OLD mask so the transition logic sees the pre-update state
    // (setReason ORs the bit in before calling setReasons).
    return setReasonsWithTransition(oldMask, reasons_);
}

uint32_t ResourceGovernor::setReasons(uint32_t reasons) {
    const uint32_t oldMask = reasons_;
    reasons_ = reasons;
    return setReasonsWithTransition(oldMask, reasons_);
}

uint32_t ResourceGovernor::setReasonsWithTransition(uint32_t oldMask, uint32_t newMask) {
    const bool wasPaused = oldMask != 0;
    const bool nowPaused = newMask != 0;

    // Any reason appearing while ACTIVE -> PAUSED; all cleared -> ACTIVE.
    // The long-pause clock starts when the session first pauses.
    if (!wasPaused && nowPaused && state_ == State::Active) {
        pausedSince_ = now();
        transitionTo(State::Paused);
    } else if (wasPaused && !nowPaused && state_ != State::Active) {
        pausedSince_ = {};
        transitionTo(State::Active);
    }
    return reasons_;
}

void ResourceGovernor::onTick() {
    if (state_ != State::Paused) {
        return; // nothing to release; ACTIVE/SUSPENDED handle their own timers
    }
    const auto t = now();
    const auto elapsed = t - pausedSince_;
    const auto limit = std::chrono::seconds(policy_.config().longPauseReleaseSeconds);
    if (elapsed >= limit) {
        transitionTo(State::Suspended);
    }
}

void ResourceGovernor::transitionTo(State next) {
    if (next == state_) {
        return;
    }
    const State from = state_;
    state_ = next;
    auto& log = log::Logger::instance();

    switch (next) {
        case State::Paused: // stop advancement, keep position + decoder
            playback_.pause();
            log.info(L"governor: ACTIVE -> PAUSED (reasons: {})", describeReasons(reasons_));
            break;
        case State::Suspended: // release the decoder entirely
            // stop() logs the session summary + releases the reader/decoder;
            // the position was kept by pause() (playback retains it across
            // stop? NO — stop() drops the session; the resume path reopens
            // and seeks). Long-pause release keeps config/playlist state.
            playback_.stop();
            log.info(L"governor: PAUSED -> SUSPENDED (released decoder, {} s paused)",
                     policy_.config().longPauseReleaseSeconds);
            break;
        case State::Active: // recreate + seek to the saved position
            if (from == State::Suspended) {
                // SUSPENDED dropped the session (stop()); the app reopens the
                // current playlist item on resume (recreate decoder + seek).
                log.info(L"governor: SUSPENDED -> ACTIVE (resume)");
                if (resumeHandler_) {
                    resumeHandler_();
                }
            } else {
                // PAUSED -> ACTIVE: the session is still open — plain resume.
                if (auto r = playback_.resume(); !r) {
                    log.warn(L"governor: resume failed: {}", r.error());
                }
                log.info(L"governor: PAUSED -> ACTIVE (reasons cleared)");
            }
            break;
    }

    if (action_) {
        action_(from, next, reasons_);
    }
}

std::wstring ResourceGovernor::describeReasons(uint32_t reasons) {
    std::wstring out;
    for (uint32_t bit = 1; bit <= kAllReasons && bit != 0; bit <<= 1) {
        if ((reasons & bit) != 0) {
            if (!out.empty()) {
                out += L", ";
            }
            const char* name = reasonName(bit);
            for (const char* p = name; *p; ++p) {
                out += static_cast<wchar_t>(*p);
            }
        }
    }
    if (out.empty()) {
        out = L"none";
    }
    return out;
}

} // namespace vw::governor
