#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>

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

    // M13: buffer recycling — hands the producer a recycled frame buffer (a
    // 14 MB RGB32 block costs VirtualAlloc + demand-zero page faults per
    // frame if reallocated fresh; reusing the buffer makes resize() a no-op
    // and removes the ~4.5 ms/frame zero-init). Non-blocking; false when no
    // spare is available (the producer then allocates fresh, as before). The
    // pool is drained on clear()/close() (pause releases the memory).
    bool takeSpareBuffer(std::vector<uint8_t>& out);

    // M13: the CONSUMER returns a frame's buffer after it is done with it
    // (e.g. after the GPU upload) so the producer can reuse it instead of
    // allocating a fresh VirtualAlloc-backed block. Thread-safe. The buffer
    // is moved out of `bytes` (which becomes empty).
    void recycleBuffer(std::vector<uint8_t>& bytes);

    // Consumer (scheduler): non-blocking; false when empty or closed.
    bool tryPop(DecodedFrame& out);

    // Consumer (M6): pops the NEWEST frame whose media timestamp <= due100ns,
    // dropping any older (stale) frames — counted in droppedFrames(). Frames
    // with timestamps in the future stay queued. The EOS sentinel is delivered
    // immediately once it reaches the front (never stale-dropped). Returns
    // false when nothing is due.
    bool popNewestUpTo(LONGLONG due100ns, DecodedFrame& out);

    // Media timestamp of the front frame (nullopt when empty or the front is
    // the EOS sentinel). Used to anchor the scheduler to the first frame's PTS.
    std::optional<LONGLONG> peekTimestamp() const;

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
    // M13: stashes a consumed frame's buffer for reuse (bounded at capacity_,
    // only buffers worth keeping — >= 1 MB). Caller holds mu_.
    void recycleLocked(std::vector<uint8_t>& bytes);
    void signalLocked(); // SetEvent under the lock

    mutable std::mutex mu_;
    std::condition_variable notFull_;
    std::condition_variable notEmpty_;
    std::deque<DecodedFrame> queue_;
    std::vector<std::vector<uint8_t>> spareBuffers_; // M13: recycled frame buffers
    size_t capacity_;
    uint64_t dropped_ = 0;
    HANDLE event_ = nullptr;
    bool closed_ = false;
};

} // namespace vw::video
