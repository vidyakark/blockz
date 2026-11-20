#include <algorithm>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>

#include "blockz/crc32.hpp"
#include "blockz/deflate.hpp"
#include "blockz/gzip.hpp"
#include "blockz/huffman.hpp"

using namespace blockz;
using Bytes = std::vector<uint8_t>;

static int g_fail = 0, g_checks = 0;
#define CHECK(cond)                                                                       \
    do {                                                                                  \
        ++g_checks;                                                                       \
        if (!(cond)) { ++g_fail; std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } \
    } while (0)

static std::mt19937_64 rng(12345);

static Bytes randomBytes(size_t n) {
    Bytes b(n);
    for (auto& x : b) x = static_cast<uint8_t>(rng());
    return b;
}
static Bytes textLike(size_t n) {
    static const char* words[] = {"the", "quick", "brown", "fox", "jumps", "over", "lazy", "dog", "compress", "block", "thread", "huffman", "window", "match"};
    Bytes b;
    while (b.size() < n) {
        const char* w = words[rng() % 14];
        b.insert(b.end(), w, w + std::strlen(w));
        b.push_back(rng() % 9 == 0 ? '\n' : ' ');
    }
    b.resize(n);
    return b;
}
static Bytes patterns(const std::string& name, size_t n) {
    if (name == "zeros") return Bytes(n, 0);
    if (name == "random") return randomBytes(n);
    if (name == "text") return textLike(n);
    if (name == "bytes") { Bytes b(n); for (size_t i = 0; i < n; ++i) b[i] = static_cast<uint8_t>(i); return b; }
    if (name == "period3") { Bytes b(n); for (size_t i = 0; i < n; ++i) b[i] = "abc"[i % 3]; return b; }
    Bytes b = textLike(n / 3);  // mixed
    Bytes r = randomBytes(n / 3);
    b.insert(b.end(), r.begin(), r.end());
    b.resize(n, 0);
    return b;
}

static void testCrc() {
    const char* s = "123456789";
    CHECK(crc32(reinterpret_cast<const uint8_t*>(s), 9) == 0xCBF43926u);
    CHECK(crc32(nullptr, 0) == 0);
    Bytes b = randomBytes(10007);
    uint32_t whole = crc32(b.data(), b.size());
    uint32_t part = crc32(b.data(), 3333);
    part = crc32(b.data() + 3333, b.size() - 3333, part);
    CHECK(whole == part);
}

static void testHuffman() {
    auto kraft = [](const std::vector<uint8_t>& l, int maxLen) {
        uint64_t sum = 0;
        for (uint8_t x : l) if (x) sum += uint64_t(1) << (maxLen - x);
        return sum;
    };
    for (int trial = 0; trial < 300; ++trial) {
        size_t n = 2 + rng() % 285;
        std::vector<uint32_t> f(n);
        for (auto& x : f) x = (rng() % 3 == 0) ? 0 : 1 + (rng() % (1u << (rng() % 20)));
        size_t used = 0;
        for (auto x : f) used += x != 0;
        if (used < 2) continue;
        for (int maxLen : {7, 15}) {
            if ((size_t(1) << maxLen) < used) continue;
            auto l = huffmanLengths(f, maxLen);
            bool okLen = true;
            for (size_t i = 0; i < n; ++i) okLen &= (f[i] ? (l[i] >= 1 && l[i] <= maxLen) : l[i] == 0);
            CHECK(okLen);
            CHECK(kraft(l, maxLen) == (uint64_t(1) << maxLen));  // complete prefix code
        }
    }
    // Fibonacci frequencies force very deep trees -> exercises the length limiting.
    std::vector<uint32_t> fib(40);
    fib[0] = fib[1] = 1;
    for (size_t i = 2; i < fib.size(); ++i) fib[i] = fib[i - 1] + fib[i - 2];
    auto l = huffmanLengths(fib, 15);
    CHECK(*std::max_element(l.begin(), l.end()) <= 15);
    uint64_t k = 0;
    for (uint8_t x : l) k += uint64_t(1) << (15 - x);
    CHECK(k == (uint64_t(1) << 15));
    CHECK(huffmanLengths({0, 5, 0}, 15) == (std::vector<uint8_t>{0, 1, 0}));
}

static void testDeflateRoundtrip() {
    const size_t sizes[] = {0, 1, 2, 3, 100, 4095, 65535, 65536, 70000, 300000};
    for (const char* kind : {"zeros", "random", "text", "bytes", "period3", "mixed"})
        for (size_t n : sizes)
            for (int level : {1, 4, 6, 9}) {
                Bytes in = patterns(kind, n), comp, back;
                deflateCompress(in.data(), in.size(), level, comp);
                size_t used = 0;
                bool ok = inflateRaw(comp.data(), comp.size(), back, comp.size() * 3, SIZE_MAX, &used);
                CHECK(ok && used == comp.size() && back == in);
                if (std::string(kind) == "random") CHECK(comp.size() <= in.size() + 5 * (in.size() / 32768 + 2) + 8);  // stored fallback: 5 bytes per block
            }
    Bytes z = patterns("zeros", 1 << 20), c;
    deflateCompress(z.data(), z.size(), 6, c);
    CHECK(c.size() < 2000);
    Bytes t = patterns("text", 1 << 20), tc;
    deflateCompress(t.data(), t.size(), 6, tc);
    CHECK(tc.size() < t.size() / 3);
}

static void testContainer() {
    const size_t bsizes[] = {4096, 10000, 65536};
    for (size_t n : {size_t(0), size_t(1), size_t(4095), size_t(4096), size_t(4097), size_t(50000), size_t(200001)})
        for (size_t bs : bsizes)
            for (unsigned threads : {1u, 3u, 8u}) {
                Bytes in = patterns("mixed", n), comp, back;
                CompressOptions o;
                o.level = 5;
                o.threads = threads;
                o.blockSize = bs;
                std::string err;
                CHECK(compressBuffer(in.data(), in.size(), o, comp, err));
                CHECK(decompressBuffer(comp.data(), comp.size(), threads, back, err));
                CHECK(back == in);
                std::vector<MemberInfo> m;
                CHECK(listMembers(comp.data(), comp.size(), m));
                CHECK(m.size() == (n == 0 ? 1 : (n + bs - 1) / bs));
            }
    // Output must not depend on the number of threads.
    Bytes in = patterns("text", 500000), a, b;
    CompressOptions o;
    o.blockSize = 16384;
    std::string err;
    o.threads = 1;
    CHECK(compressBuffer(in.data(), in.size(), o, a, err));
    o.threads = 8;
    CHECK(compressBuffer(in.data(), in.size(), o, b, err));
    CHECK(a == b);
}

static void testCorruption() {
    Bytes in = patterns("text", 120000), comp, back;
    CompressOptions o;
    o.blockSize = 8192;
    o.threads = 4;
    std::string err;
    CHECK(compressBuffer(in.data(), in.size(), o, comp, err));

    int detected = 0, tried = 0;
    for (int i = 0; i < 400; ++i) {
        Bytes bad = comp;
        size_t pos = 20 + rng() % (bad.size() - 28);
        bad[pos] ^= static_cast<uint8_t>(1u << (rng() % 8));
        // Skip positions that fall in another member's header or trailer-size area (those are exercised below).
        std::vector<MemberInfo> m;
        listMembers(comp.data(), comp.size(), m);
        bool inHeader = false;
        for (auto& x : m) inHeader |= pos >= x.offset && pos < x.offset + 20;
        if (inHeader) continue;
        ++tried;
        Bytes out;
        bool ok = decompressBuffer(bad.data(), bad.size(), 4, out, err);
        if (!ok) ++detected;
        else CHECK(out == in);  // only acceptable if the flip hit unused padding bits
    }
    CHECK(tried > 300 && detected >= tried - 5);

    for (size_t cut : {size_t(1), size_t(7), size_t(8), size_t(9), size_t(30), size_t(1000), comp.size() - 5}) {
        Bytes t(comp.begin(), comp.end() - cut);
        Bytes out;
        CHECK(!decompressBuffer(t.data(), t.size(), 4, out, err));
    }
    Bytes garbage = randomBytes(5000), out;
    CHECK(!decompressBuffer(garbage.data(), garbage.size(), 2, out, err));
    // A corrupted block in the middle must make the parallel pipeline fail cleanly (no hang).
    Bytes bad = comp;
    std::vector<MemberInfo> m;
    listMembers(comp.data(), comp.size(), m);
    bad[m[7].offset + 40] ^= 0x55;
    CHECK(!decompressBuffer(bad.data(), bad.size(), 4, out, err));
    CHECK(err.find("block") != std::string::npos);
}

// gzip members as written by other tools: no size field, optional FNAME, several members back to back.
static void testForeignGzip() {
    auto member = [](const Bytes& raw, bool withName) {
        Bytes m = {0x1f, 0x8b, 8, uint8_t(withName ? 8 : 0), 1, 2, 3, 4, 0, 3};
        if (withName) { const char* nm = "file.txt"; m.insert(m.end(), nm, nm + 9); }
        deflateCompress(raw.data(), raw.size(), 6, m);
        uint32_t c = crc32(raw.data(), raw.size());
        for (int i = 0; i < 4; ++i) m.push_back(uint8_t(c >> (8 * i)));
        for (int i = 0; i < 4; ++i) m.push_back(uint8_t(raw.size() >> (8 * i)));
        return m;
    };
    Bytes a = patterns("text", 30000), b = patterns("mixed", 5000);
    Bytes file = member(a, true), second = member(b, false);
    file.insert(file.end(), second.begin(), second.end());
    Bytes out, expect = a;
    expect.insert(expect.end(), b.begin(), b.end());
    std::string err;
    CHECK(decompressBuffer(file.data(), file.size(), 4, out, err));
    CHECK(out == expect);
    std::vector<MemberInfo> m;
    CHECK(!listMembers(file.data(), file.size(), m));  // not in the parallel layout
    file.insert(file.end(), 16, 0);                    // trailing zero padding is ignored, like gzip does
    CHECK(decompressBuffer(file.data(), file.size(), 4, out, err) && out == expect);
    file.push_back(7);
    CHECK(!decompressBuffer(file.data(), file.size(), 4, out, err));  // other trailing garbage is an error
}

static void testInflateFuzz() {
    // Random and mutated streams: the decoder must reject or decode them, never crash or overrun (run under ASan in CI).
    for (int i = 0; i < 20000; ++i) {
        Bytes junk = randomBytes(rng() % 300), out;
        size_t used;
        inflateRaw(junk.data(), junk.size(), out, 64, 1 << 20, &used);
    }
    Bytes in = patterns("mixed", 20000), comp;
    deflateCompress(in.data(), in.size(), 6, comp);
    for (int i = 0; i < 20000; ++i) {
        Bytes m = comp, out;
        for (int k = 0; k < 1 + int(rng() % 4); ++k) m[rng() % m.size()] ^= static_cast<uint8_t>(1u << (rng() % 8));
        if (rng() % 4 == 0) m.resize(rng() % m.size());
        size_t used;
        inflateRaw(m.data(), m.size(), out, 100, 1 << 20, &used);
    }
    CHECK(true);
}

int main() {
    testCrc();
    testHuffman();
    testDeflateRoundtrip();
    testContainer();
    testCorruption();
    testForeignGzip();
    testInflateFuzz();
    std::printf("%d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
