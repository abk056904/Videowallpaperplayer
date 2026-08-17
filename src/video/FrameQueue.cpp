#include "video/FrameQueue.h"

#include <utility>

namespace vw::video {

bool FrameQueue::push(DecodedFrame frame) {
    std::unique_lock lock(mu_);
    notFull_.wait(lock, [&] { return closed_ || queue_.size() < capacity_; });
    if (closed_) {
        return false;
    }
    queue_.push_back(std::move(frame));
    notEmpty_.notify_one();
    return true;
}

bool FrameQueue::tryPush(DecodedFrame frame) {
    std::lock_guard lock(mu_);
    if (closed_ || queue_.size() >= capacity_) {
        return false;
    }
    queue_.push_back(std::move(frame));
    notEmpty_.notify_one();
    return true;
}

bool FrameQueue::tryPop(DecodedFrame& out) {
    std::lock_guard lock(mu_);
    if (queue_.empty()) {
        return false;
    }
    out = std::move(queue_.front());
    queue_.pop_front();
    notFull_.notify_one();
    return true;
}

void FrameQueue::clear() {
    std::lock_guard lock(mu_);
    queue_.clear();
    notFull_.notify_all();
}

void FrameQueue::close() {
    std::lock_guard lock(mu_);
    closed_ = true;
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

} // namespace vw::video
