// framebuffer_swizzle.h -- 线性 RGBA8888 到 Tegra 16Bx2 block-linear 的分片转换。
#pragma once

#include <cstdint>
#include <cstring>

namespace wa2 {

// libnx Framebuffer 使用固定的 16 GOB block height。每个 task 覆盖一个
// 64x128 像素块，任务之间没有读写重叠，因此可安全分配给不同 CPU 核。
inline uint32_t BlockLinearTaskCount(uint32_t dstStride, uint32_t height) {
    constexpr uint32_t kBlockHeightPx = 8u << 4; // 128
    const uint32_t widthBlocks = dstStride >> 6;
    const uint32_t heightBlocks =
        (height + kBlockHeightPx - 1u) / kBlockHeightPx;
    return widthBlocks * heightBlocks;
}

inline void SwizzleRgbaToBlockLinearRange(
    void* dstBuffer, const void* srcBuffer,
    uint32_t dstStride, uint32_t srcStride, uint32_t height,
    uint32_t taskBegin, uint32_t taskEnd) {
    constexpr uint32_t kGobBytes = 512;
    constexpr uint32_t kBlockHeightGobs = 1u << 4; // 16
    constexpr uint32_t kBlockHeightPx = 8u << 4;  // 128

    const uint32_t widthBlocks = dstStride >> 6;
    const uint32_t totalTasks = BlockLinearTaskCount(dstStride, height);
    if (!dstBuffer || !srcBuffer || !widthBlocks || taskBegin >= totalTasks)
        return;
    if (taskEnd > totalTasks) taskEnd = totalTasks;

    auto* dst = static_cast<uint8_t*>(dstBuffer);
    const auto* src = static_cast<const uint8_t*>(srcBuffer);
    for (uint32_t task = taskBegin; task < taskEnd; ++task) {
        const uint32_t blockY = task / widthBlocks;
        const uint32_t blockX = task % widthBlocks;
        uint8_t* outGob = dst +
            static_cast<size_t>(task) * kBlockHeightGobs * kGobBytes;

        for (uint32_t gobY = 0; gobY < kBlockHeightGobs; ++gobY) {
            const uint32_t x = blockX * 64u;
            const uint32_t y = blockY * kBlockHeightPx + gobY * 8u;
            if (y < height) {
                const uint8_t* inGob = src +
                    static_cast<size_t>(y) * srcStride + x;
                // 16Bx2 sector ordering, identical to libnx framebuffer.c.
                for (uint32_t i = 0; i < 32u; ++i) {
                    const uint32_t sourceY = ((i >> 1) & 0x06u) | (i & 0x01u);
                    const uint32_t sourceX = ((i << 3) & 0x10u) |
                                             ((i << 1) & 0x20u);
                    std::memcpy(outGob, inGob + sourceY * srcStride + sourceX, 16u);
                    outGob += 16u;
                }
            } else {
                outGob += kGobBytes;
            }
        }
    }
}

} // namespace wa2
