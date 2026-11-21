#pragma once
#include <cstdint>

namespace gateway {
// Read only headers from a booted ESP application. The bootloader already verified the image.
template<class Read>
bool app_image_size(uint32_t capacity, Read read, uint32_t &length) {
    uint8_t header[24];
    if (capacity < sizeof(header) || !read(0, header, sizeof(header))) return false;
    if (header[0] != 0xE9 || header[1] == 0 || header[1] > 16 || header[23] > 1) return false;
    uint32_t offset = sizeof(header);
    for (unsigned i = 0; i < header[1]; ++i) {
        uint8_t segment[8];
        if (capacity - offset < sizeof(segment) || !read(offset, segment, sizeof(segment))) return false;
        offset += sizeof(segment);
        const uint32_t bytes = uint32_t(segment[4]) | (uint32_t(segment[5]) << 8)
                               | (uint32_t(segment[6]) << 16) | (uint32_t(segment[7]) << 24);
        if ((bytes & 3U) || bytes > capacity - offset) return false;
        offset += bytes;
    }
    const uint32_t tail = 16 - (offset & 15U) + (header[23] ? 32 : 0);
    if (tail > capacity - offset) return false;
    length = offset + tail;
    return true;
}
}
