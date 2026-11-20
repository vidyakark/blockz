#pragma once
#include <cstdint>
#include <vector>

namespace blockz {

// Length-limited Huffman code lengths (every length <= maxLen).
// With >= 2 used symbols the result is a *complete* prefix code (Kraft sum == 1), which strict
// DEFLATE decoders such as zlib require. With exactly 1 used symbol it gets length 1.
std::vector<uint8_t> huffmanLengths(const std::vector<uint32_t>& freq, int maxLen);

// Canonical code values (MSB-first) for the given lengths, as defined in RFC 1951 section 3.2.2.
std::vector<uint16_t> canonicalCodes(const std::vector<uint8_t>& lens);

}  // namespace blockz
