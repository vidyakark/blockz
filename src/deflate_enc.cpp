// DEFLATE encoder: LZ77 (hash chains, optional lazy matching) + per-block choice of
// stored / fixed-Huffman / dynamic-Huffman encoding, whichever is smallest.
#include <algorithm>
#include <cstring>

#include "blockz/deflate.hpp"
#include "blockz/huffman.hpp"

namespace blockz {
namespace {

constexpr uint32_t kWindow = 32768, kMinMatch = 3, kMaxMatch = 258;
constexpr uint32_t kHashBits = 15;
constexpr size_t kMaxTokens = 1 << 15;  // tokens per DEFLATE block

const uint16_t kLenBase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
const uint8_t kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const uint16_t kDistBase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
const uint8_t kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
const uint8_t kClOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

struct Tables {
    uint8_t lenCode[256];  // (length-3) -> index into kLenBase
    uint8_t distCode[512];
    Tables() {
        for (int c = 0; c < 29; ++c)
            for (uint32_t l = kLenBase[c]; l < kLenBase[c] + (1u << kLenExtra[c]) && l <= 258; ++l) lenCode[l - 3] = static_cast<uint8_t>(c);
        lenCode[258 - 3] = 28;  // length 258 has its own code
        int dist = 0;
        int c = 0;
        for (; c < 16; ++c)
            for (int k = 0; k < (1 << kDistExtra[c]); ++k) distCode[dist++] = static_cast<uint8_t>(c);
        dist >>= 7;
        for (; c < 30; ++c)
            for (int k = 0; k < (1 << (kDistExtra[c] - 7)); ++k) distCode[256 + dist++] = static_cast<uint8_t>(c);
    }
};
const Tables& tables() {
    static const Tables t;
    return t;
}
inline uint32_t distSym(uint32_t d) {  // d = distance - 1
    const Tables& t = tables();
    return d < 256 ? t.distCode[d] : t.distCode[256 + (d >> 7)];
}

struct LevelParams { int chain, nice; bool lazy; };
const LevelParams kLevels[10] = {{0, 0, false}, {4, 8, false}, {8, 16, false}, {16, 32, false}, {16, 32, true},
                                 {32, 64, true}, {64, 128, true}, {128, 128, true}, {256, 258, true}, {1024, 258, true}};

struct Token {
    uint16_t a;  // literal byte, or match length
    uint16_t b;  // 0 for a literal, else match distance
};

class BitWriter {
public:
    explicit BitWriter(std::vector<uint8_t>& out) : out_(out) {}
    void put(uint32_t v, int n) {  // LSB-first, n <= 16
        acc_ |= static_cast<uint64_t>(v) << nb_;
        nb_ += n;
        while (nb_ >= 8) { out_.push_back(static_cast<uint8_t>(acc_)); acc_ >>= 8; nb_ -= 8; }
    }
    void align() {
        if (nb_ > 0) { out_.push_back(static_cast<uint8_t>(acc_)); acc_ = 0; nb_ = 0; }
    }
    void bytes(const uint8_t* p, size_t n) { out_.insert(out_.end(), p, p + n); }  // only when aligned

private:
    std::vector<uint8_t>& out_;
    uint64_t acc_ = 0;
    int nb_ = 0;
};

inline uint32_t reverseBits(uint32_t code, int len) {
    uint32_t r = 0;
    for (int i = 0; i < len; ++i) { r = (r << 1) | (code & 1); code >>= 1; }
    return r;
}

// A Huffman code ready for writing: bit-reversed codes + lengths.
struct Code {
    std::vector<uint8_t> len;
    std::vector<uint16_t> rev;
    void init(const std::vector<uint8_t>& lens) {
        len = lens;
        std::vector<uint16_t> c = canonicalCodes(lens);
        rev.resize(lens.size());
        for (size_t i = 0; i < lens.size(); ++i) rev[i] = static_cast<uint16_t>(reverseBits(c[i], lens[i]));
    }
};

// Makes sure at least two symbols have a non-zero count so the code is complete.
void ensureTwo(std::vector<uint32_t>& f) {
    size_t used = 0;
    for (uint32_t x : f) used += x != 0;
    for (size_t i = 0; used < 2 && i < f.size(); ++i)
        if (!f[i]) { f[i] = 1; ++used; }
}

class Encoder {
public:
    Encoder(const uint8_t* in, size_t n, int level, std::vector<uint8_t>& out)
        : d_(in), n_(static_cast<uint32_t>(n)), p_(kLevels[level]), bw_(out), head_(1u << kHashBits, -1), prev_(kWindow, -1) {
        tok_.reserve(kMaxTokens);
        tables();  // build lookup tables once
        // Fixed Huffman code (RFC 1951 3.2.6)
        std::vector<uint8_t> fl(288), fd(30, 5);
        for (int i = 0; i < 288; ++i) fl[i] = i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8;
        fixedLit_.init(fl);
        fixedDist_.init(fd);
    }

    void run() {
        uint32_t i = 0;
        while (i < n_) {
            uint32_t dist = 0;
            uint32_t len = find(i, 0, dist);
            if (len >= kMinMatch && p_.lazy && len < static_cast<uint32_t>(p_.nice) && i + 1 < n_) {
                insert(i);
                uint32_t d2 = 0;
                if (find(i + 1, len, d2) > len) {  // a longer match starts one byte later: emit a literal first
                    emitLiteral(d_[i]);
                    ++i;
                    continue;
                }
                emitMatch(len, dist);
                for (uint32_t k = 1; k < len; ++k) insert(i + k);
                i += len;
            } else if (len >= kMinMatch) {
                emitMatch(len, dist);
                for (uint32_t k = 0; k < len; ++k) insert(i + k);
                i += len;
            } else {
                emitLiteral(d_[i]);
                insert(i);
                ++i;
            }
        }
        flushBlock(true);
        bw_.align();  // pad the last byte with zero bits
    }

private:
    uint32_t hash(uint32_t pos) const {
        uint32_t v = uint32_t(d_[pos]) | uint32_t(d_[pos + 1]) << 8 | uint32_t(d_[pos + 2]) << 16;
        return (v * 0x9E3779B1u) >> (32 - kHashBits);
    }
    void insert(uint32_t pos) {
        if (pos + 3 > n_) return;
        uint32_t h = hash(pos);
        prev_[pos & (kWindow - 1)] = head_[h];
        head_[h] = static_cast<int32_t>(pos);
    }
    // Longest match at pos that is longer than prevLen (0 if none). Distances are limited to the 32 KiB window.
    uint32_t find(uint32_t pos, uint32_t prevLen, uint32_t& distOut) {
        if (pos + 3 > n_) return 0;
        const uint32_t maxLen = std::min<uint32_t>(kMaxMatch, n_ - pos);
        uint32_t best = prevLen < 2 ? 2 : prevLen;
        if (best >= maxLen) return 0;
        const int64_t limit = pos > kWindow ? int64_t(pos) - kWindow : 0;
        int32_t cand = head_[hash(pos)];
        int chain = p_.chain;
        if (prevLen >= 8) chain >>= 2;
        uint32_t found = 0;
        const uint8_t* cur = d_ + pos;
        while (cand >= 0 && cand >= limit && chain-- > 0) {
            const uint8_t* m = d_ + cand;
            if (m[best] == cur[best] && m[0] == cur[0] && m[1] == cur[1]) {
                uint32_t l = 2;
                while (l < maxLen && m[l] == cur[l]) ++l;
                if (l > best) {
                    best = l;
                    found = l;
                    distOut = pos - cand;
                    if (l >= static_cast<uint32_t>(p_.nice) || l >= maxLen) break;
                }
            }
            int32_t next = prev_[cand & (kWindow - 1)];
            if (next >= cand) break;  // stale slot
            cand = next;
        }
        if (found == 3 && distOut > 4096) return 0;  // a 3-byte match this far away costs more than 3 literals
        return found;
    }

    void emitLiteral(uint8_t c) {
        tok_.push_back({c, 0});
        ++covered_;
        if (tok_.size() >= kMaxTokens) flushBlock(false);
    }
    void emitMatch(uint32_t len, uint32_t dist) {
        tok_.push_back({static_cast<uint16_t>(len), static_cast<uint16_t>(dist)});
        covered_ += len;
        if (tok_.size() >= kMaxTokens) flushBlock(false);
    }

    // ---- block output -----------------------------------------------------------------------------
    void flushBlock(bool final) {
        std::vector<uint32_t> lf(286, 0), df(30, 0);
        uint64_t extraBits = 0;
        const Tables& t = tables();
        for (const Token& k : tok_) {
            if (k.b == 0) { ++lf[k.a]; continue; }
            uint32_t lc = t.lenCode[k.a - 3], dc = distSym(k.b - 1u);
            ++lf[257 + lc];
            ++df[dc];
            extraBits += kLenExtra[lc] + kDistExtra[dc];
        }
        lf[256] = 1;  // end-of-block

        // Dynamic code: lengths + header encoding
        std::vector<uint32_t> lf2 = lf, df2 = df;
        ensureTwo(lf2);
        ensureTwo(df2);
        std::vector<uint8_t> ll = huffmanLengths(lf2, 15), dl = huffmanLengths(df2, 15);
        size_t hlit = 286, hdist = 30;
        while (hlit > 257 && ll[hlit - 1] == 0) --hlit;
        while (hdist > 1 && dl[hdist - 1] == 0) --hdist;
        std::vector<std::pair<uint8_t, uint8_t>> rle;  // {code length symbol, extra value}
        {
            std::vector<uint8_t> all(ll.begin(), ll.begin() + hlit);
            all.insert(all.end(), dl.begin(), dl.begin() + hdist);
            size_t i = 0;
            while (i < all.size()) {
                uint8_t v = all[i];
                size_t run = 1;
                while (i + run < all.size() && all[i + run] == v) ++run;
                i += run;
                if (v == 0) {
                    while (run >= 3) {
                        size_t take = run >= 11 ? std::min<size_t>(run, 138) : run;
                        rle.push_back(take >= 11 ? std::make_pair<uint8_t, uint8_t>(18, uint8_t(take - 11)) : std::make_pair<uint8_t, uint8_t>(17, uint8_t(take - 3)));
                        run -= take;
                    }
                    for (; run > 0; --run) rle.push_back({0, 0});
                } else {
                    rle.push_back({v, 0});
                    --run;
                    while (run >= 3) {
                        size_t take = std::min<size_t>(run, 6);
                        rle.push_back({16, uint8_t(take - 3)});
                        run -= take;
                    }
                    for (; run > 0; --run) rle.push_back({v, 0});
                }
            }
        }
        std::vector<uint32_t> cf(19, 0);
        for (auto& r : rle) ++cf[r.first];
        ensureTwo(cf);
        std::vector<uint8_t> cl = huffmanLengths(cf, 7);
        size_t hclen = 19;
        while (hclen > 4 && cl[kClOrder[hclen - 1]] == 0) --hclen;

        auto cost = [&](const std::vector<uint8_t>& L, const std::vector<uint8_t>& D) {
            uint64_t bits = extraBits;
            for (size_t i = 0; i < 286; ++i) bits += uint64_t(lf[i]) * L[i];
            for (size_t i = 0; i < 30; ++i) bits += uint64_t(df[i]) * D[i];
            return bits;
        };
        uint64_t dynBits = 3 + 5 + 5 + 4 + 3 * hclen + cost(ll, dl);
        for (auto& r : rle) dynBits += cl[r.first] + (r.first == 16 ? 2 : r.first == 17 ? 3 : r.first == 18 ? 7 : 0);
        uint64_t fixBits = 3 + cost(fixedLit_.len, fixedDist_.len);
        const uint64_t rawBytes = covered_;
        uint64_t storedBits = 8 * (rawBytes + 5 * ((rawBytes + 65534) / 65535)) + 10;

        if (rawBytes > 0 && storedBits < dynBits && storedBits < fixBits) {
            writeStored(final);
        } else if (fixBits <= dynBits) {
            bw_.put(final ? 1 : 0, 1);
            bw_.put(1, 2);
            writeTokens(fixedLit_, fixedDist_);
        } else {
            bw_.put(final ? 1 : 0, 1);
            bw_.put(2, 2);
            bw_.put(static_cast<uint32_t>(hlit - 257), 5);
            bw_.put(static_cast<uint32_t>(hdist - 1), 5);
            bw_.put(static_cast<uint32_t>(hclen - 4), 4);
            for (size_t i = 0; i < hclen; ++i) bw_.put(cl[kClOrder[i]], 3);
            Code clc;
            clc.init(cl);
            for (auto& r : rle) {
                bw_.put(clc.rev[r.first], clc.len[r.first]);
                if (r.first == 16) bw_.put(r.second, 2);
                else if (r.first == 17) bw_.put(r.second, 3);
                else if (r.first == 18) bw_.put(r.second, 7);
            }
            Code lc, dc;
            lc.init(ll);
            dc.init(dl);
            writeTokens(lc, dc);
        }
        blockStart_ += covered_;
        covered_ = 0;
        tok_.clear();
    }

    void writeTokens(const Code& lit, const Code& dist) {
        const Tables& t = tables();
        for (const Token& k : tok_) {
            if (k.b == 0) { bw_.put(lit.rev[k.a], lit.len[k.a]); continue; }
            uint32_t lc = t.lenCode[k.a - 3], dc = distSym(k.b - 1u);
            bw_.put(lit.rev[257 + lc], lit.len[257 + lc]);
            if (kLenExtra[lc]) bw_.put(k.a - kLenBase[lc], kLenExtra[lc]);
            bw_.put(dist.rev[dc], dist.len[dc]);
            if (kDistExtra[dc]) bw_.put(k.b - kDistBase[dc], kDistExtra[dc]);
        }
        bw_.put(lit.rev[256], lit.len[256]);
    }

    void writeStored(bool final) {
        size_t off = blockStart_, left = covered_;
        while (left > 0) {
            size_t chunk = std::min<size_t>(left, 65535);
            left -= chunk;
            bw_.put((final && left == 0) ? 1 : 0, 1);
            bw_.put(0, 2);
            bw_.align();
            uint8_t hdr[4] = {uint8_t(chunk), uint8_t(chunk >> 8), uint8_t(~chunk), uint8_t((~chunk) >> 8)};
            bw_.bytes(hdr, 4);
            bw_.bytes(d_ + off, chunk);
            off += chunk;
        }
    }

    const uint8_t* d_;
    uint32_t n_;
    LevelParams p_;
    BitWriter bw_;
    std::vector<int32_t> head_, prev_;
    std::vector<Token> tok_;
    uint32_t covered_ = 0;   // input bytes covered by the tokens of the current block
    size_t blockStart_ = 0;  // input offset where the current block starts
    Code fixedLit_, fixedDist_;
};

}  // namespace

void deflateCompress(const uint8_t* in, size_t n, int level, std::vector<uint8_t>& out) {
    level = std::max(1, std::min(9, level));
    Encoder e(in, n, level, out);
    e.run();
}

}  // namespace blockz
