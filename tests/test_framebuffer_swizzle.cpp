// test_framebuffer_swizzle.cpp -- 验证完整转换与四分片转换逐字节一致。
#include "wa2/framebuffer_swizzle.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

namespace {

// 独立按 libnx framebuffer.c 的三层 block/gob 循环实现参考结果。
// 被测代码按扁平 task 定位输出，两种结构不同，能捕获 task 偏移或遍历顺序错误。
void ConvertReference(std::vector<uint8_t>& output,
                      const std::vector<uint8_t>& source,
                      uint32_t stride, uint32_t height) {
    constexpr uint32_t kBlockHeightGobs = 16;
    constexpr uint32_t kBlockHeightPx = 128;
    const uint32_t widthBlocks = stride >> 6;
    const uint32_t heightBlocks =
        (height + kBlockHeightPx - 1u) / kBlockHeightPx;
    uint8_t* outGob = output.data();
    for (uint32_t blockY = 0; blockY < heightBlocks; ++blockY) {
        for (uint32_t blockX = 0; blockX < widthBlocks; ++blockX) {
            for (uint32_t gobY = 0; gobY < kBlockHeightGobs; ++gobY) {
                const uint32_t x = blockX * 64u;
                const uint32_t y = blockY * kBlockHeightPx + gobY * 8u;
                if (y < height) {
                    const uint8_t* inGob = source.data() +
                        static_cast<size_t>(y) * stride + x;
                    for (uint32_t i = 0; i < 32u; ++i) {
                        const uint32_t sourceY = ((i >> 1) & 0x06u) |
                                                 (i & 0x01u);
                        const uint32_t sourceX = ((i << 3) & 0x10u) |
                                                 ((i << 1) & 0x20u);
                        std::memcpy(outGob,
                                    inGob + sourceY * stride + sourceX, 16u);
                        outGob += 16u;
                    }
                } else {
                    outGob += 512u;
                }
            }
        }
    }
}

} // namespace

int main() {
    constexpr uint32_t kWidth = 1280;
    constexpr uint32_t kHeight = 720;
    constexpr uint32_t kStride = kWidth * 4;
    constexpr uint32_t kAlignedHeight = 768;

    std::vector<uint8_t> source(static_cast<size_t>(kStride) * kHeight);
    for (size_t i = 0; i < source.size(); ++i)
        source[i] = static_cast<uint8_t>((i * 131u + (i >> 7)) & 0xffu);

    std::vector<uint8_t> serial(static_cast<size_t>(kStride) * kAlignedHeight, 0xa5);
    std::vector<uint8_t> partitioned(serial.size(), 0xa5);
    std::vector<uint8_t> concurrent(serial.size(), 0xa5);
    std::vector<uint8_t> reference(serial.size(), 0xa5);
    ConvertReference(reference, source, kStride, kHeight);
    const uint32_t tasks = wa2::BlockLinearTaskCount(kStride, kHeight);
    wa2::SwizzleRgbaToBlockLinearRange(serial.data(), source.data(),
                                      kStride, kStride, kHeight, 0, tasks);
    for (uint32_t part = 0; part < 4; ++part) {
        const uint32_t begin = tasks * part / 4u;
        const uint32_t end = tasks * (part + 1u) / 4u;
        wa2::SwizzleRgbaToBlockLinearRange(partitioned.data(), source.data(),
                                          kStride, kStride, kHeight, begin, end);
    }
    std::vector<std::thread> workers;
    for (uint32_t part = 0; part < 4; ++part) {
        const uint32_t begin = tasks * part / 4u;
        const uint32_t end = tasks * (part + 1u) / 4u;
        workers.emplace_back([&, begin, end] {
            wa2::SwizzleRgbaToBlockLinearRange(
                concurrent.data(), source.data(), kStride, kStride, kHeight,
                begin, end);
        });
    }
    for (auto& worker : workers) worker.join();

    if (serial != reference) {
        std::fprintf(stderr, "framebuffer swizzle differs from libnx reference\n");
        return 1;
    }
    if (serial != partitioned) {
        std::fprintf(stderr, "framebuffer swizzle partition mismatch\n");
        return 2;
    }
    if (serial != concurrent) {
        std::fprintf(stderr, "framebuffer swizzle concurrent mismatch\n");
        return 3;
    }
    if (std::all_of(serial.begin(), serial.end(), [](uint8_t v) { return v == 0xa5; })) {
        std::fprintf(stderr, "framebuffer swizzle produced no output\n");
        return 4;
    }
    std::printf("FRAMEBUFFER SWIZZLE PASSED (libnx-ref=match tasks=%u bytes=%zu parts=4 concurrent=match)\n",
                tasks, serial.size());
    return 0;
}
