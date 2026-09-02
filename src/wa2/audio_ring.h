// audio_ring.h — 单生产者/单消费者 PCM 字节环形缓冲
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace wa2 {

// Producer 和 Consumer 各自只能由一个线程调用。计数器单调递增，数组下标
// 才取模，因而满/空状态不会混淆。发布写指针前 PCM 已复制完成；发布读指针
// 后 Producer 才能覆盖已消费区域。
template <size_t Capacity>
class SpscByteRing {
    static_assert(Capacity > 0, "ring capacity must be non-zero");

public:
    size_t Write(const void* source, size_t bytes) {
        if (!source || !bytes) return 0;
        const uint64_t write = write_.load(std::memory_order_relaxed);
        const uint64_t read = read_.load(std::memory_order_acquire);
        const size_t used = (size_t)std::min<uint64_t>(write - read, Capacity);
        const size_t count = std::min(bytes, Capacity - used);
        const size_t offset = (size_t)(write % Capacity);
        const size_t first = std::min(count, Capacity - offset);
        std::memcpy(data_.data() + offset, source, first);
        if (count > first)
            std::memcpy(data_.data(), static_cast<const uint8_t*>(source) + first,
                        count - first);
        write_.store(write + count, std::memory_order_release);
        return count;
    }

    size_t Read(void* destination, size_t bytes) {
        if (!destination || !bytes) return 0;
        const uint64_t read = read_.load(std::memory_order_relaxed);
        const uint64_t write = write_.load(std::memory_order_acquire);
        const size_t available = (size_t)std::min<uint64_t>(write - read, Capacity);
        const size_t count = std::min(bytes, available);
        const size_t offset = (size_t)(read % Capacity);
        const size_t first = std::min(count, Capacity - offset);
        std::memcpy(destination, data_.data() + offset, first);
        if (count > first)
            std::memcpy(static_cast<uint8_t*>(destination) + first, data_.data(),
                        count - first);
        read_.store(read + count, std::memory_order_release);
        return count;
    }

    size_t Available() const {
        const uint64_t read = read_.load(std::memory_order_acquire);
        const uint64_t write = write_.load(std::memory_order_acquire);
        return (size_t)std::min<uint64_t>(write - read, Capacity);
    }

    size_t Free() const { return Capacity - Available(); }
    static constexpr size_t Size() { return Capacity; }

private:
    alignas(64) std::array<uint8_t, Capacity> data_{};
    alignas(64) std::atomic<uint64_t> read_{0};
    alignas(64) std::atomic<uint64_t> write_{0};
};

} // namespace wa2
