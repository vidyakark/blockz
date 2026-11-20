#pragma once
#include <cstddef>
#include <cstdint>

namespace blockz {

// CRC-32 (IEEE 802.3), as used by gzip. Pass the previous result as `crc` to continue a running checksum.
uint32_t crc32(const uint8_t* data, size_t n, uint32_t crc = 0);

}  // namespace blockz
