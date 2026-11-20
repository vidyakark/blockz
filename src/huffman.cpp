#include "blockz/huffman.hpp"

#include <algorithm>
#include <utility>

namespace blockz {

std::vector<uint8_t> huffmanLengths(const std::vector<uint32_t>& freq, int maxLen) {
    const size_t n = freq.size();
    std::vector<uint8_t> lens(n, 0);

    // Used symbols, least frequent first (ties by symbol index so output is deterministic).
    std::vector<std::pair<uint32_t, uint32_t>> sym;  // {freq, index}
    for (size_t i = 0; i < n; ++i)
        if (freq[i]) sym.push_back({freq[i], static_cast<uint32_t>(i)});
    std::sort(sym.begin(), sym.end());
    const size_t m = sym.size();
    if (m == 0) return lens;
    if (m == 1) { lens[sym[0].second] = 1; return lens; }

    // Classic two-queue Huffman construction: leaves are sorted, and internal nodes are created in
    // non-decreasing weight order, so the two smallest nodes are always at the queue fronts.
    std::vector<uint64_t> w(2 * m - 1);
    std::vector<uint32_t> parent(2 * m - 1, 0);
    for (size_t i = 0; i < m; ++i) w[i] = sym[i].first;
    size_t a = 0, b = m;  // next unused leaf / next unused internal node
    for (size_t k = 0; k + 1 < m; ++k) {
        size_t pick[2];
        for (int t = 0; t < 2; ++t) {
            if (a < m && (b >= m + k || w[a] <= w[b])) pick[t] = a++;
            else pick[t] = b++;
        }
        const size_t node = m + k;
        w[node] = w[pick[0]] + w[pick[1]];
        parent[pick[0]] = parent[pick[1]] = static_cast<uint32_t>(node);
    }
    // Depths are clamped at maxLen on the way down *including internal nodes*; every clamped node counts as
    // overflow. (This is the exact bookkeeping zlib uses, which the repair loop below relies on.)
    std::vector<uint32_t> depth(2 * m - 1, 0);
    int overflow = 0;
    for (size_t i = 2 * m - 2; i-- > 0;) {
        uint32_t d = depth[parent[i]] + 1;
        if (d > static_cast<uint32_t>(maxLen)) { d = maxLen; ++overflow; }
        depth[i] = d;
    }
    std::vector<uint32_t> count(maxLen + 2, 0);
    for (size_t i = 0; i < m; ++i) ++count[depth[i]];
    while (overflow > 0) {
        int bits = maxLen - 1;
        while (count[bits] == 0) --bits;
        --count[bits];
        count[bits + 1] += 2;
        --count[maxLen];
        overflow -= 2;
    }

    // Least frequent symbols receive the longest codes.
    size_t next = 0;
    for (int bits = maxLen; bits >= 1; --bits)
        for (uint32_t c = count[bits]; c > 0; --c) lens[sym[next++].second] = static_cast<uint8_t>(bits);
    return lens;
}

std::vector<uint16_t> canonicalCodes(const std::vector<uint8_t>& lens) {
    uint32_t blCount[17] = {0};
    for (uint8_t l : lens) ++blCount[l];
    blCount[0] = 0;
    uint32_t next[17] = {0};
    uint32_t code = 0;
    for (int bits = 1; bits <= 16; ++bits) {
        code = (code + blCount[bits - 1]) << 1;
        next[bits] = code;
    }
    std::vector<uint16_t> codes(lens.size(), 0);
    for (size_t i = 0; i < lens.size(); ++i)
        if (lens[i]) codes[i] = static_cast<uint16_t>(next[lens[i]]++);
    return codes;
}

}  // namespace blockz
