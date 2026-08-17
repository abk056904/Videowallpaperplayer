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
    if (on) {
        reasons_ |= reason;
    } else {
        reasons_ &= ~reason;
    }
    return setReasonsWithTransition(reasons_);
}

uint32_t ResourceGovernor::setReasons(uint32_t reasons) {
    reasons_ = reasons;
    return setReasonsWithTransition(reasons_);
}

uint32_t ResourceGovernor::setReasonsWithTransition(uint32_t newMask) {
    // The long-pause clock starts the moment the session first pauses (an
    // ACTIVE session acquiring its first reason). Reasons piling on or
    // clearing while already paused do NOT restart it — the total paused
    // stretch is what triggers the decoder release.
    if (state_ == State::Active && newMask != 0) {
        pausedSince_ = now();
    }
    // The policy is the single source of truth for the transition table;
    // mask changes never know about the elapsed clock (a fresh reason
    // appearing keeps the session paused, not suspended).
    const State next = policy_.nextState(state_, newMask, false);
    if (next != state_) {
        if (next == State::Active) {
            pausedSince_ = {};
        }
        transitionTo(next);
    }
    return reasons_;
}

void ResourceGovernor::onTick() {
    if (state_ != State::Paused) {
        return; // nothing to release; ACTIVE/SUSPENDED handle their own timers
    }
    const auto elapsed = now() - pausedSince_;
    const auto limit = std::chrono::seconds(policy_.config().longPauseReleaseSeconds);
    const State next = policy_.nextState(state_, reasons_, elapsed >= limit);
    if (next != state_) {
        transitionTo(next);
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
            // stop() logs the session summary + releases the reader/decoder
            // (the resume path recreates it). Playlist/config state survives.
            playback_.stop();
            log.info(L"governor: PAUSED -> SUSPENDED (released decoder, {} s paused)",
                     policy_.config().longPauseReleaseSeconds);
            break;
        case State::Active:
            // SUSPENDED dropped the session, and a user stop does too (the app
            // calls playback_->stop() after feeding the User reason) — both
            // need the app to reopen the current playlist item. Only a plain
            // PAUSED session (decoder kept by pause()) can resume in place.
            if (from == State::Suspended || !playback_.isOpen()) {
                log.info(L"governor: {} -> ACTIVE (reopen)",
                         from == State::Suspended ? L"SUSPENDED" : L"PAUSED");
                if (resumeHandler_) {
                    resumeHandler_();
                } else {
                    log.warn(L"governor: no resume handler — session left stopped");
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
