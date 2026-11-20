// DEFLATE decoder (RFC 1951). Table-driven Huffman decoding; every read and write is bounds-checked
// so corrupt or malicious input can only produce an error, never out-of-bounds memory access.
#include <algorithm>
#include <cstring>

#include "blockz/deflate.hpp"

namespace blockz {
namespace {

const uint16_t kLenBase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
const uint8_t kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const uint16_t kDistBase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
const uint8_t kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
const uint8_t kClOrder[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};

class BitReader {
public:
    BitReader(const uint8_t* p, size_t n) : start_(p), p_(p), end_(p + n) {}

    void refill() {
        while (nb_ <= 56) {
            if (p_ < end_) acc_ |= static_cast<uint64_t>(*p_++) << nb_;
            else pad_ += 8;  // virtual zero bytes past the end; consuming them is detected by overrun()
            nb_ += 8;
        }
    }
    uint32_t peek(int n) {
        if (nb_ < n) refill();
        return static_cast<uint32_t>(acc_ & ((1ull << n) - 1));
    }
    void drop(int n) { acc_ >>= n; nb_ -= n; }
    uint32_t bits(int n) {
        if (n == 0) return 0;
        uint32_t v = peek(n);
        drop(n);
        return v;
    }
    bool overrun() const { return nb_ < pad_; }

    // Discard bits up to the next byte boundary.
    void alignByte() { drop(nb_ & 7); }

    // Copies n raw bytes (stored block). Returns false if the input is too short.
    bool copyBytes(uint8_t* dst, size_t n) {
        while (n > 0 && nb_ - pad_ >= 8) {  // bytes already sitting in the bit buffer
            *dst++ = static_cast<uint8_t>(acc_);
            drop(8);
            --n;
        }
        if (n == 0) return true;
        acc_ = 0; nb_ = 0; pad_ = 0;
        if (static_cast<size_t>(end_ - p_) < n) return false;
        std::memcpy(dst, p_, n);
        p_ += n;
        return true;
    }
    // Bytes of input fully consumed so far (call after alignByte()).
    size_t consumed() const { return static_cast<size_t>(p_ - start_) - static_cast<size_t>(nb_ - pad_) / 8; }

private:
    const uint8_t* start_;
    const uint8_t* p_;
    const uint8_t* end_;
    uint64_t acc_ = 0;
    int nb_ = 0;
    int pad_ = 0;
};

// Single-level lookup table: index = next `bits` input bits; entry = symbol | length << 16.
struct HuffTable {
    std::vector<uint32_t> t;
    int bits = 0;

    bool build(const uint8_t* lens, int n) {
        uint32_t count[16] = {0};
        for (int i = 0; i < n; ++i) ++count[lens[i]];
        count[0] = 0;
        int maxLen = 0;
        for (int l = 1; l <= 15; ++l) if (count[l]) maxLen = l;
        int left = 1;
        for (int l = 1; l <= 15; ++l) {  // reject over-subscribed codes
            left <<= 1;
            left -= static_cast<int>(count[l]);
            if (left < 0) return false;
        }
        bits = maxLen == 0 ? 1 : maxLen;
        t.assign(size_t(1) << bits, 0);  // 0 = invalid
        uint32_t next[16];
        uint32_t code = 0;
        for (int l = 1; l <= 15; ++l) { code = (code + count[l - 1]) << 1; next[l] = code; }
        for (int s = 0; s < n; ++s) {
            int l = lens[s];
            if (!l) continue;
            uint32_t c = next[l]++, r = 0;
            for (int i = 0; i < l; ++i) { r = (r << 1) | (c & 1); c >>= 1; }
            for (size_t k = r; k < t.size(); k += size_t(1) << l) t[k] = static_cast<uint32_t>(s) | (static_cast<uint32_t>(l) << 16);
        }
        return true;
    }
    // Returns symbol, or -1 on an invalid code.
    inline int decode(BitReader& br) const {
        uint32_t e = t[br.peek(bits)];
        int l = static_cast<int>(e >> 16);
        if (l == 0) return -1;
        br.drop(l);
        return static_cast<int>(e & 0xFFFF);
    }
};

const HuffTable& fixedLit() {
    static const HuffTable h = [] {
        uint8_t l[288];
        for (int i = 0; i < 288; ++i) l[i] = i < 144 ? 8 : i < 256 ? 9 : i < 280 ? 7 : 8;
        HuffTable x;
        x.build(l, 288);
        return x;
    }();
    return h;
}
const HuffTable& fixedDist() {
    static const HuffTable h = [] {
        uint8_t l[30];
        std::fill(l, l + 30, 5);
        HuffTable x;
        x.build(l, 30);
        return x;
    }();
    return h;
}

}  // namespace

bool inflateRaw(const uint8_t* in, size_t n, std::vector<uint8_t>& out, size_t sizeHint, size_t maxOut, size_t* consumed) {
    constexpr size_t kSlack = 258;
    size_t cap = std::max<size_t>(std::min(sizeHint, maxOut), 1024) + kSlack;
    const size_t hardCap = maxOut > SIZE_MAX - kSlack ? SIZE_MAX : maxOut + kSlack;
    cap = std::min(cap, hardCap);
    out.assign(cap, 0);
    size_t pos = 0;

    BitReader br(in, n);
    // Makes room for at least `need` more bytes; false if that would exceed maxOut.
    auto ensure = [&](size_t need) {
        if (pos + need <= out.size()) return true;
        if (pos + need > hardCap) return false;
        size_t ncap = std::min(hardCap, std::max(out.size() * 2, pos + need));
        out.resize(ncap);
        return true;
    };

    HuffTable lit, dist;
    for (bool last = false; !last;) {
        last = br.bits(1) != 0;
        uint32_t type = br.bits(2);
        if (br.overrun()) return false;

        if (type == 0) {  // stored
            br.alignByte();
            uint32_t len = br.bits(16), nlen = br.bits(16);
            if (br.overrun() || (len ^ 0xFFFF) != nlen) return false;
            if (pos + len > maxOut || !ensure(len)) return false;
            if (!br.copyBytes(out.data() + pos, len)) return false;
            pos += len;
            continue;
        }
        const HuffTable *L, *D;
        if (type == 1) {
            L = &fixedLit();
            D = &fixedDist();
        } else if (type == 2) {
            uint32_t hlit = br.bits(5) + 257, hdist = br.bits(5) + 1, hclen = br.bits(4) + 4;
            if (hlit > 286 || hdist > 30) return false;
            uint8_t cl[19] = {0};
            for (uint32_t i = 0; i < hclen; ++i) cl[kClOrder[i]] = static_cast<uint8_t>(br.bits(3));
            if (br.overrun()) return false;
            HuffTable ct;
            if (!ct.build(cl, 19)) return false;
            uint8_t lens[286 + 30] = {0};
            uint32_t i = 0;
            while (i < hlit + hdist) {
                int s = ct.decode(br);
                if (s < 0 || br.overrun()) return false;
                if (s < 16) { lens[i++] = static_cast<uint8_t>(s); continue; }
                uint32_t rep;
                uint8_t v = 0;
                if (s == 16) {
                    if (i == 0) return false;
                    v = lens[i - 1];
                    rep = 3 + br.bits(2);
                } else if (s == 17) rep = 3 + br.bits(3);
                else rep = 11 + br.bits(7);
                if (i + rep > hlit + hdist) return false;
                while (rep--) lens[i++] = v;
            }
            if (lens[256] == 0) return false;  // end-of-block must be codable
            if (!lit.build(lens, static_cast<int>(hlit)) || !dist.build(lens + hlit, static_cast<int>(hdist))) return false;
            L = &lit;
            D = &dist;
        } else {
            return false;
        }

        for (;;) {
            int s = L->decode(br);
            if (s < 0 || br.overrun()) return false;
            if (!ensure(kSlack)) return false;
            if (s < 256) { out[pos++] = static_cast<uint8_t>(s); continue; }
            if (s == 256) break;
            s -= 257;
            if (s >= 29) return false;
            size_t len = kLenBase[s] + br.bits(kLenExtra[s]);
            int ds = D->decode(br);
            if (ds < 0 || ds >= 30) return false;
            size_t d = kDistBase[ds] + br.bits(kDistExtra[ds]);
            if (br.overrun() || d > pos) return false;
            if (pos + len > maxOut) return false;
            uint8_t* o = out.data() + pos;
            const uint8_t* src = o - d;
            if (d >= len) std::memcpy(o, src, len);
            else for (size_t k = 0; k < len; ++k) o[k] = src[k];  // overlapping copy repeats the pattern
            pos += len;
        }
    }
    br.alignByte();
    if (br.overrun()) return false;
    *consumed = br.consumed();
    out.resize(pos);
    return true;
}

}  // namespace blockz
