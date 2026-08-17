#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>

#include <windows.h>

#include "video/DecodedFrame.h"

namespace vw::video {

// Bounded frame queue (docs/02 §2.5 / docs/03 §3.8, M4 + M6). The decode
// worker (producer) blocks on push() when full — that IS the backpressure; the
// scheduler (consumer) uses popNewestUpTo() on the frame deadline. close()
// unblocks any waiter so stop/pause never hangs.
//
// M6: the consumer selects the NEWEST frame whose media timestamp is due,
// dropping stale frames (droppedFrames counter) for wallpaper freshness; a
// manual-reset event is signaled on every push so a wait loop can wake on new
// frames without polling (FrameScheduler's "wake on new frame").
class FrameQueue {
public:
    explicit FrameQueue(size_t capacity = 3);
    ~FrameQueue();

    FrameQueue(const FrameQueue&) = delete;
    FrameQueue& operator=(const FrameQueue&) = delete;

    // Producer (decode thread): blocks while full; returns false when closed.
    bool push(DecodedFrame frame);

    // Producer: non-blocking; false when full or closed.
    bool tryPush(DecodedFrame frame);

    // Consumer (scheduler): non-blocking; false when empty or closed.
    bool tryPop(DecodedFrame& out);

    // Consumer (M6): pops the NEWEST frame whose media timestamp <= due100ns,
    // dropping any older (stale) frames — counted in droppedFrames(). Frames
    // with timestamps in the future stay queued. The EOS sentinel is delivered
    // immediately once it reaches the front (never stale-dropped). Returns
    // false when nothing is due.
    bool popNewestUpTo(LONGLONG due100ns, DecodedFrame& out);

    // Drops all frames (pause) and resets the dropped counter + event.
    // Consumers/producers are not unblocked.
    void clear();

    // Permanently closes the queue: unblocks all waiters; push/tryPush/tryPop
    // return false from now on. Resets the event (no stale wakeups).
    void close();

    size_t size() const;
    size_t capacity() const { return capacity_; }
    void setCapacity(size_t capacity); // M6: bounds tuning

    // Manual-reset event signaled on every successful push; the consumer
    // resets it on wake. Lets the message loop wait on {timer, event} with
    // zero polling (docs/02 §2.5).
    HANDLE newFrameEvent() const { return event_; }

    // Cumulative stale frames dropped by popNewestUpTo since the last clear().
    uint64_t droppedFrames() const;

private:
    void signalLocked(); // SetEvent under the lock

    mutable std::mutex mu_;
    std::condition_variable notFull_;
    std::condition_variable notEmpty_;
    std::deque<DecodedFrame> queue_;
    size_t capacity_;
    uint64_t dropped_ = 0;
    HANDLE event_ = nullptr;
    bool closed_ = false;
};

} // namespace vw::video
