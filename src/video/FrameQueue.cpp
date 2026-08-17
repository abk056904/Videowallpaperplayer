#include "video/FrameQueue.h"

#include <utility>

namespace vw::video {

FrameQueue::FrameQueue(size_t capacity) : capacity_(capacity == 0 ? 1 : capacity) {
    // Manual-reset, initially unsignaled: the consumer resets it after each
    // wake; a push during the drain re-signals it (no lost wakeups).
    event_ = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
}

FrameQueue::~FrameQueue() {
    if (event_) {
        ::CloseHandle(event_);
    }
}

void FrameQueue::signalLocked() {
    if (event_) {
        ::SetEvent(event_);
    }
}

bool FrameQueue::push(DecodedFrame frame) {
    std::unique_lock lock(mu_);
    notFull_.wait(lock, [&] { return closed_ || queue_.size() < capacity_; });
    if (closed_) {
        return false;
    }
    queue_.push_back(std::move(frame));
    signalLocked();
    notEmpty_.notify_one();
    return true;
}

bool FrameQueue::tryPush(DecodedFrame frame) {
    std::lock_guard lock(mu_);
    if (closed_ || queue_.size() >= capacity_) {
        return false;
    }
    queue_.push_back(std::move(frame));
    signalLocked();
    notEmpty_.notify_one();
    return true;
}

bool FrameQueue::takeSpareBuffer(std::vector<uint8_t>& out) {
    std::lock_guard lock(mu_);
    if (spareBuffers_.empty()) {
        return false;
    }
    out = std::move(spareBuffers_.back()); // most recent (cache-warm)
    spareBuffers_.pop_back();
    return true;
}

void FrameQueue::recycleBuffer(std::vector<uint8_t>& bytes) {
    std::lock_guard lock(mu_);
    recycleLocked(bytes);
}

void FrameQueue::recycleLocked(std::vector<uint8_t>& bytes) {
    // Only buffers worth keeping: a fresh 14 MB block is VirtualAlloc-backed;
    // the recycled pool is bounded at capacity_ so pause clears() the memory.
    if (bytes.capacity() < (1ull << 20)) { // < 1 MB: not worth hoarding
        return;
    }
    if (spareBuffers_.size() >= capacity_) {
        return;
    }
    spareBuffers_.push_back(std::move(bytes));
}

bool FrameQueue::tryPop(DecodedFrame& out) {
    std::lock_guard lock(mu_);
    if (queue_.empty()) {
        return false;
    }
    out = std::move(queue_.front());
    queue_.pop_front();
    // NOTE: the delivered frame's buffer stays with the caller (out). Only
    // buffers that would be DESTROYED (stale-dropped frames) are recycled.
    notFull_.notify_one();
    return true;
}

bool FrameQueue::popNewestUpTo(LONGLONG due100ns, DecodedFrame& out) {
    std::lock_guard lock(mu_);
    if (queue_.empty()) {
        return false;
    }
    // The EOS sentinel is delivered immediately once it reaches the front —
    // never delayed by (or counted against) the staleness policy.
    if (queue_.front().endOfStream) {
        out = std::move(queue_.front());
        queue_.pop_front();
        notFull_.notify_one();
        return true;
    }
    // Frames arrive in decode order, so the due frames are a prefix. Walk it,
    // keeping the newest one; everything older is stale (dropped). Frames with
    // timestamps in the future stay queued for later deadlines.
    DecodedFrame best;
    bool have = false;
    size_t popped = 0;
    while (!queue_.empty() && !queue_.front().endOfStream &&
           queue_.front().timestamp <= due100ns) {
        recycleLocked(best.bytes); // M13: the previous (stale-dropped) frame's buffer
        best = std::move(queue_.front());
        queue_.pop_front();
        have = true;
        ++popped;
    }
    if (!have) {
        return false;
    }
    dropped_ += popped - 1;
    out = std::move(best);
    // The delivered frame keeps its buffer; the stale-dropped ones were
    // recycled inside the loop (they would otherwise be destroyed).
    notFull_.notify_one();
    return true;
}

std::optional<LONGLONG> FrameQueue::peekTimestamp() const {
    std::lock_guard lock(mu_);
    if (queue_.empty() || queue_.front().endOfStream) {
        return std::nullopt;
    }
    return queue_.front().timestamp;
}

void FrameQueue::clear() {
    std::lock_guard lock(mu_);
    queue_.clear();
    spareBuffers_.clear(); // M13: pause releases the recycled memory
    dropped_ = 0;
    if (event_) {
        ::ResetEvent(event_);
    }
    notFull_.notify_all();
}

void FrameQueue::close() {
    std::lock_guard lock(mu_);
    closed_ = true;
    spareBuffers_.clear(); // M13: no reuse after close
    if (event_) {
        ::ResetEvent(event_);
    }
    notFull_.notify_all();
    notEmpty_.notify_all();
}

size_t FrameQueue::size() const {
    std::lock_guard lock(mu_);
    return queue_.size();
}

void FrameQueue::setCapacity(size_t capacity) {
    std::lock_guard lock(mu_);
    capacity_ = capacity == 0 ? 1 : capacity;
    notFull_.notify_all();
}

uint64_t FrameQueue::droppedFrames() const {
    std::lock_guard lock(mu_);
    return dropped_;
}

} // namespace vw::video
