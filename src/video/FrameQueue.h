#pragma once

#include <condition_variable>
#include <deque>
#include <mutex>

#include "video/DecodedFrame.h"

namespace vw::video {

// Bounded frame queue (docs/02 §2.7 / docs/03 §3.6). The decode worker
// (producer) blocks on push() when full — that IS the backpressure; the UI
// thread (consumer) uses tryPop() on the frame timer. close() unblocks any
// waiter so stop/pause never hangs. Fully exercised in M6 (capacity bounds,
// drop-oldest, backpressure).
class FrameQueue {
public:
    explicit FrameQueue(size_t capacity = 3) : capacity_(capacity) {}

    FrameQueue(const FrameQueue&) = delete;
    FrameQueue& operator=(const FrameQueue&) = delete;

    // Producer (decode thread): blocks while full; returns false when closed.
    bool push(DecodedFrame frame);

    // Producer: non-blocking; false when full or closed.
    bool tryPush(DecodedFrame frame);

    // Consumer (UI thread): non-blocking; false when empty or closed.
    bool tryPop(DecodedFrame& out);

    // Drops all frames (pause). Consumers/producers are not unblocked.
    void clear();

    // Permanently closes the queue: unblocks all waiters; push/tryPush/tryPop
    // return false from now on.
    void close();

    size_t size() const;
    size_t capacity() const { return capacity_; }
    void setCapacity(size_t capacity); // M6: bounds tuning

private:
    mutable std::mutex mu_;
    std::condition_variable notFull_;
    std::condition_variable notEmpty_;
    std::deque<DecodedFrame> queue_;
    size_t capacity_;
    bool closed_ = false;
};

} // namespace vw::video
