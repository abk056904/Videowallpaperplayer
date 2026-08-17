#include "playlist/PlaylistManager.h"

#include <algorithm>
#include <utility>

namespace vw::playlist {

namespace {
// Fisher-Yates over [0, n). `avoid` (kNoIndex = none) is excluded from
// position 0 when n > 1 (no immediate repeat of the item just played). n == 0
// returns an empty order (the caller checks .empty() — never dereference).
std::vector<size_t> shuffledPermutation(std::mt19937& rng, size_t n, size_t avoid) {
    if (n == 0) {
        return {};
    }
    std::vector<size_t> order(n);
    for (size_t i = 0; i < n; ++i) {
        order[i] = i;
    }
    for (size_t i = n - 1; i > 0; --i) {
        std::uniform_int_distribution<size_t> dist(0, i);
        std::swap(order[i], order[dist(rng)]);
    }
    if (n > 1 && avoid != PlaylistManager::kNoIndex && order[0] == avoid) {
        std::swap(order[0], order[1 + static_cast<size_t>(
                                  std::uniform_int_distribution<size_t>(0, n - 2)(rng))]);
    }
    return order;
}
} // namespace

PlaylistManager::PlaylistManager(PlaylistData data) : data_(std::move(data)) {
    unavailable_.assign(data_.items.size(), false);
    repairState();
}

// ---- structural ops --------------------------------------------------------

size_t PlaylistManager::add(const std::wstring& path) {
    return add(PlaylistItem{path});
}

size_t PlaylistManager::add(const PlaylistItem& item) {
    data_.items.push_back(item);
    unavailable_.push_back(false);
    rebuildOrder();
    return data_.items.size() - 1;
}

bool PlaylistManager::remove(size_t index) {
    if (!inRange(index)) {
        return false;
    }
    data_.items.erase(data_.items.begin() + static_cast<ptrdiff_t>(index));
    unavailable_.erase(unavailable_.begin() + static_cast<ptrdiff_t>(index));
    if (data_.current == index) {
        data_.current = kNoIndex; // caller re-selects
    } else if (data_.current > index) {
        --data_.current;
    }
    rebuildOrder();
    return true;
}

bool PlaylistManager::move(size_t from, size_t to) {
    if (!inRange(from) || !inRange(to) || from == to) {
        return false;
    }
    const PlaylistItem item = data_.items[from];
    const bool wasUnavailable = unavailable_[from];
    data_.items.erase(data_.items.begin() + static_cast<ptrdiff_t>(from));
    unavailable_.erase(unavailable_.begin() + static_cast<ptrdiff_t>(from));
    data_.items.insert(data_.items.begin() + static_cast<ptrdiff_t>(to), item);
    unavailable_.insert(unavailable_.begin() + static_cast<ptrdiff_t>(to), wasUnavailable);
    // Track the moved index through the shift.
    if (data_.current == from) {
        data_.current = to;
    } else if (from < data_.current && data_.current <= to) {
        --data_.current;
    } else if (to <= data_.current && data_.current < from) {
        ++data_.current;
    }
    rebuildOrder();
    return true;
}

bool PlaylistManager::replace(size_t index, const PlaylistItem& item) {
    if (!inRange(index)) {
        return false;
    }
    data_.items[index] = item;
    // A different file may be behind the same index — clear the cached
    // metadata (stale until the next real open refreshes it, docs §32).
    data_.items[index].duration100ns = 0;
    data_.items[index].width = 0;
    data_.items[index].height = 0;
    data_.items[index].codec.clear();
    return true;
}

void PlaylistManager::clear() {
    data_.items.clear();
    unavailable_.clear();
    data_.current = kNoIndex;
    data_.shuffleOrder.clear();
}

bool PlaylistManager::setEnabled(size_t index, bool enabled) {
    if (!inRange(index)) {
        return false;
    }
    data_.items[index].enabled = enabled;
    return true;
}

bool PlaylistManager::setItemTimes(size_t index, int64_t start100ns, int64_t end100ns) {
    if (!inRange(index)) {
        return false;
    }
    data_.items[index].start100ns = start100ns;
    data_.items[index].end100ns = end100ns;
    return true;
}

bool PlaylistManager::updateCachedMetadata(size_t index, int64_t duration100ns, uint32_t width,
                                           uint32_t height, std::wstring codec) {
    if (!inRange(index)) {
        return false;
    }
    auto& item = data_.items[index];
    item.duration100ns = duration100ns;
    item.width = width;
    item.height = height;
    item.codec = std::move(codec);
    return true;
}

// ---- navigation policy -----------------------------------------------------

size_t PlaylistManager::nextIndex() {
    if (data_.items.empty()) {
        return kNoIndex;
    }
    switch (data_.mode) {
        case Mode::Single:
            return isPlayable(data_.current) ? data_.current : kNoIndex;
        case Mode::Sequential:
        case Mode::Loop: {
            const bool wrap = data_.loop || data_.mode == Mode::Loop;
            return advanceFrom(data_.current, wrap);
        }
        case Mode::Shuffle: {
            // Position of the current item in the shuffled order.
            const auto it =
                std::find(data_.shuffleOrder.begin(), data_.shuffleOrder.end(), data_.current);
            if (it != data_.shuffleOrder.end()) {
                return shuffleForward(static_cast<size_t>(it - data_.shuffleOrder.begin()),
                                      data_.loop);
            }
            // Current not in the (possibly stale) order — regenerate and
            // return the first PLAYABLE item (the front may be disabled /
            // unavailable; same policy as shuffleForward's wrap branch).
            regenerateShuffle();
            for (const size_t i : data_.shuffleOrder) {
                if (isPlayable(i)) {
                    return i;
                }
            }
            return kNoIndex;
        }
    }
    return kNoIndex;
}

size_t PlaylistManager::previousIndex() const {
    if (data_.items.empty()) {
        return kNoIndex;
    }
    switch (data_.mode) {
        case Mode::Single:
            return isPlayable(data_.current) ? data_.current : kNoIndex;
        case Mode::Sequential:
        case Mode::Loop: {
            const bool wrap = data_.loop || data_.mode == Mode::Loop;
            return retreatFrom(data_.current, wrap);
        }
        case Mode::Shuffle: {
            const auto it =
                std::find(data_.shuffleOrder.begin(), data_.shuffleOrder.end(), data_.current);
            if (it == data_.shuffleOrder.end()) {
                return kNoIndex;
            }
            return shuffleBackward(static_cast<size_t>(it - data_.shuffleOrder.begin()),
                                   data_.loop);
        }
    }
    return kNoIndex;
}

bool PlaylistManager::setCurrent(size_t index) {
    if (!inRange(index)) {
        return false;
    }
    data_.current = index;
    return true;
}

const PlaylistItem* PlaylistManager::itemAt(size_t index) const {
    return inRange(index) ? &data_.items[index] : nullptr;
}

void PlaylistManager::setMode(Mode mode) {
    data_.mode = mode;
    if (mode == Mode::Shuffle) {
        regenerateShuffle(); // (re)enter shuffle with a fresh cycle
    }
}

void PlaylistManager::setLoop(bool loop) {
    data_.loop = loop;
}

void PlaylistManager::shuffle() {
    regenerateShuffle();
}

void PlaylistManager::markUnavailable(size_t index) {
    if (inRange(index)) {
        unavailable_[index] = true;
    }
}

bool PlaylistManager::isUnavailable(size_t index) const {
    return inRange(index) && unavailable_[index];
}

void PlaylistManager::adopt(PlaylistData data) {
    data_ = std::move(data);
    unavailable_.assign(data_.items.size(), false);
    repairState();
}

// ---- internals -------------------------------------------------------------

bool PlaylistManager::isPlayable(size_t i) const {
    return inRange(i) && data_.items[i].enabled && !unavailable_[i];
}

size_t PlaylistManager::advanceFrom(size_t from, bool wrap) const {
    const size_t n = data_.items.size();
    if (n == 0) {
        return kNoIndex;
    }
    for (size_t step = 1; step <= n; ++step) {
        if (!wrap && from + step >= n) {
            return kNoIndex; // passed the end without wrapping
        }
        const size_t i = (from + step) % n;
        if (isPlayable(i)) {
            return i;
        }
    }
    return kNoIndex;
}

size_t PlaylistManager::retreatFrom(size_t from, bool wrap) const {
    const size_t n = data_.items.size();
    if (n == 0) {
        return kNoIndex;
    }
    for (size_t step = 1; step <= n; ++step) {
        if (!wrap && from < step) {
            return kNoIndex; // passed the start without wrapping
        }
        const size_t i = (from + n - step) % n;
        if (isPlayable(i)) {
            return i;
        }
    }
    return kNoIndex;
}

size_t PlaylistManager::shuffleForward(size_t pos, bool wrap) {
    const size_t n = data_.shuffleOrder.size();
    for (size_t step = 1; step <= n; ++step) {
        if (pos + step >= n) {
            if (!wrap) {
                return kNoIndex; // cycle ended without loop: stop
            }
            // Looped past the end: start a FRESH cycle (docs/03 §3.9 — the
            // shuffle order is regenerated per cycle; no immediate repeat of
            // the item just played) and play its first playable item.
            regenerateShuffle();
            for (const size_t i : data_.shuffleOrder) {
                if (isPlayable(i)) {
                    return i;
                }
            }
            return kNoIndex;
        }
        const size_t i = data_.shuffleOrder[pos + step];
        if (isPlayable(i)) {
            return i;
        }
    }
    return kNoIndex;
}

size_t PlaylistManager::shuffleBackward(size_t pos, bool wrap) const {
    const size_t n = data_.shuffleOrder.size();
    if (n == 0) {
        return kNoIndex;
    }
    for (size_t step = 1; step <= n; ++step) {
        if (!wrap && pos < step) {
            return kNoIndex;
        }
        const size_t i = data_.shuffleOrder[(pos + n - step) % n];
        if (isPlayable(i)) {
            return i;
        }
    }
    return kNoIndex;
}

void PlaylistManager::regenerateShuffle() {
    const size_t n = data_.items.size();
    data_.shuffleOrder = shuffledPermutation(rng_, n, data_.current);
    ++shuffleGeneration_; // observable via shuffleGeneration() (tests)
}

void PlaylistManager::rebuildOrder() {
    if (data_.mode == Mode::Shuffle) {
        regenerateShuffle();
    } else {
        data_.shuffleOrder.clear();
        data_.shuffleOrder.reserve(data_.items.size());
        for (size_t i = 0; i < data_.items.size(); ++i) {
            data_.shuffleOrder.push_back(i);
        }
    }
}

void PlaylistManager::repairState() {
    if (data_.current >= data_.items.size()) {
        data_.current = data_.items.empty() ? kNoIndex : 0;
    }
    // Sanitize the persisted shuffle order: drop out-of-range/duplicate
    // entries, then append any missing indices so it is a full permutation.
    if (!data_.items.empty() && data_.mode == Mode::Shuffle) {
        std::vector<size_t> seen(data_.items.size(), 0);
        std::vector<size_t> clean;
        clean.reserve(data_.items.size());
        for (const size_t i : data_.shuffleOrder) {
            if (i < data_.items.size() && seen[i] == 0) {
                seen[i] = 1;
                clean.push_back(i);
            }
        }
        for (size_t i = 0; i < data_.items.size(); ++i) {
            if (seen[i] == 0) {
                clean.push_back(i);
            }
        }
        data_.shuffleOrder = std::move(clean);
    }
    if (data_.mode == Mode::Shuffle && data_.shuffleOrder.size() != data_.items.size()) {
        regenerateShuffle();
    } else if (data_.mode != Mode::Shuffle) {
        rebuildOrder();
    }
}

} // namespace vw::playlist
