#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <vector>

namespace SharkSDR::dsp {

// Lock-free single-producer / single-consumer ring buffer.
template <typename T>
class RingBuffer {
public:
    explicit RingBuffer(size_t capacity = 0) { resize(capacity); }

    // Not thread-safe: call only while neither side is active.
    void resize(size_t capacity) {
        size_t c = 1;
        while (c < capacity + 1) c <<= 1;
        buffer_.assign(c, T{});
        mask_ = c - 1;
        head_.store(0);
        tail_.store(0);
    }

    size_t capacity() const { return mask_; }

    size_t readAvailable() const {
        return (head_.load(std::memory_order_acquire) - tail_.load(std::memory_order_acquire)) & mask_;
    }
    size_t writeAvailable() const { return capacity() - readAvailable(); }

    // Producer side. Writes as much as fits, returns the number written.
    size_t write(const T* data, size_t count) {
        size_t head = head_.load(std::memory_order_relaxed);
        size_t tail = tail_.load(std::memory_order_acquire);
        size_t free = capacity() - ((head - tail) & mask_);
        count = std::min(count, free);
        size_t first = std::min(count, buffer_.size() - (head & mask_));
        std::copy(data, data + first, &buffer_[head & mask_]);
        std::copy(data + first, data + count, &buffer_[0]);
        head_.store((head + count) & mask_, std::memory_order_release);
        return count;
    }

    // Consumer side. Reads up to `count` items, returns the number read.
    size_t read(T* data, size_t count) {
        size_t tail = tail_.load(std::memory_order_relaxed);
        size_t head = head_.load(std::memory_order_acquire);
        size_t avail = (head - tail) & mask_;
        count = std::min(count, avail);
        size_t first = std::min(count, buffer_.size() - (tail & mask_));
        std::copy(&buffer_[tail & mask_], &buffer_[tail & mask_] + first, data);
        std::copy(&buffer_[0], &buffer_[0] + (count - first), data + first);
        tail_.store((tail + count) & mask_, std::memory_order_release);
        return count;
    }

    // Consumer side. Drops up to `count` items.
    size_t skip(size_t count) {
        size_t tail = tail_.load(std::memory_order_relaxed);
        size_t head = head_.load(std::memory_order_acquire);
        count = std::min(count, (head - tail) & mask_);
        tail_.store((tail + count) & mask_, std::memory_order_release);
        return count;
    }

private:
    std::vector<T> buffer_;
    size_t mask_ = 0;
    std::atomic<size_t> head_{0};
    std::atomic<size_t> tail_{0};
};

}  // namespace SharkSDR::dsp
