#include "blockz/crc32.hpp"

namespace blockz {
namespace {

// Slicing-by-8: processes 8 bytes per step using 8 lookup tables.
struct Tables {
    uint32_t t[8][256];
    Tables() {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            t[0][i] = c;
        }
        for (uint32_t i = 0; i < 256; ++i)
            for (int s = 1; s < 8; ++s) t[s][i] = (t[s - 1][i] >> 8) ^ t[0][t[s - 1][i] & 0xFF];
    }
};

}  // namespace

uint32_t crc32(const uint8_t* p, size_t n, uint32_t crc) {
    static const Tables T;
    crc = ~crc;
    while (n >= 8) {
        uint32_t a = crc ^ (uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24);
        crc = T.t[7][a & 0xFF] ^ T.t[6][(a >> 8) & 0xFF] ^ T.t[5][(a >> 16) & 0xFF] ^ T.t[4][a >> 24] ^
              T.t[3][p[4]] ^ T.t[2][p[5]] ^ T.t[1][p[6]] ^ T.t[0][p[7]];
        p += 8;
        n -= 8;
    }
    while (n--) crc = T.t[0][(crc ^ *p++) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

}  // namespace blockz
