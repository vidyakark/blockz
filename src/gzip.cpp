#include "blockz/gzip.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <thread>

#include "blockz/crc32.hpp"
#include "blockz/deflate.hpp"
#include "blockz/pipeline.hpp"

namespace blockz {
namespace {

constexpr size_t kHeaderBytes = 20;   // 10 fixed + 2 (XLEN) + 8 (extra field)
constexpr size_t kTrailerBytes = 8;   // CRC-32 + ISIZE
constexpr size_t kMaxForeignMember = size_t(4) << 30;  // decoded-size cap for members without a size field

inline uint32_t rd32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
inline void wr32(uint8_t* p, uint32_t v) { p[0] = uint8_t(v); p[1] = uint8_t(v >> 8); p[2] = uint8_t(v >> 16); p[3] = uint8_t(v >> 24); }

double now() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void makeMember(const uint8_t* p, size_t len, int level, std::vector<uint8_t>& r) {
    const uint8_t hdr[kHeaderBytes] = {0x1f, 0x8b, 8, 4,        // magic, deflate, FLG.FEXTRA
                                       0, 0, 0, 0,              // mtime (unset, keeps output reproducible)
                                       uint8_t(level >= 9 ? 2 : level == 1 ? 4 : 0), 3,  // XFL, OS = Unix
                                       8, 0,                    // XLEN
                                       'S', 'X', 4, 0,          // subfield "SX", 4 bytes: total member size
                                       0, 0, 0, 0};
    r.assign(hdr, hdr + kHeaderBytes);
    r.reserve(len / 2 + 128);
    deflateCompress(p, len, level, r);
    uint8_t tr[kTrailerBytes];
    wr32(tr, crc32(p, len));
    wr32(tr + 4, static_cast<uint32_t>(len));
    r.insert(r.end(), tr, tr + kTrailerBytes);
    wr32(r.data() + 16, static_cast<uint32_t>(r.size()));
}

// Strict check for the parallel layout; fills compSize if the member at `off` matches.
bool parseFastMember(const uint8_t* in, size_t n, size_t off, uint64_t& compSize) {
    if (n - off < kHeaderBytes + kTrailerBytes) return false;
    const uint8_t* p = in + off;
    static const uint8_t pre[16] = {0x1f, 0x8b, 8, 4, 0, 0, 0, 0, 0, 0, 8, 0, 'S', 'X', 4, 0};
    if (std::memcmp(p, pre, 4) != 0 || p[10] != 8 || p[11] != 0 || std::memcmp(p + 12, pre + 12, 4) != 0) return false;
    compSize = rd32(p + 16);
    return compSize >= kHeaderBytes + kTrailerBytes && compSize <= n - off;
}

}  // namespace

unsigned defaultThreads() {
    unsigned t = std::thread::hardware_concurrency();
    return t ? t : 1;
}

bool listMembers(const uint8_t* in, size_t n, std::vector<MemberInfo>& out) {
    out.clear();
    size_t off = 0;
    while (off < n) {
        uint64_t cs;
        if (!parseFastMember(in, n, off, cs)) return false;
        out.push_back({off, cs, rd32(in + off + cs - 4)});
        off += cs;
    }
    return !out.empty();
}

bool compressStream(const uint8_t* in, size_t n, const CompressOptions& opt, const Sink& sink, Stats& st,
                    std::string& err, const Progress& progress) {
    err.clear();
    const int level = std::max(1, std::min(9, opt.level));
    const size_t bs = std::max<size_t>(4096, std::min<size_t>(opt.blockSize, size_t(128) << 20));
    const unsigned threads = opt.threads ? opt.threads : defaultThreads();
    const size_t count = n == 0 ? 1 : (n + bs - 1) / bs;
    const double t0 = now();

    st = Stats();
    st.inBytes = n;
    st.threads = threads;
    st.blocks = count;
    uint64_t done = 0;

    bool ok = orderedParallel(
        count, threads, size_t(threads) * 2 + 2,
        [&](size_t i, std::vector<uint8_t>& r, std::string&) {
            size_t off = i * bs, len = std::min(bs, n - off);
            makeMember(in + off, len, level, r);
            return true;
        },
        [&](size_t i, const std::vector<uint8_t>& r) {
            if (!sink(r.data(), r.size())) return false;
            st.outBytes += r.size();
            done += std::min(bs, n - i * bs);
            if (progress) progress(done, n);
            return true;
        },
        err);
    st.seconds = now() - t0;
    return ok;
}

namespace {

// Decodes one gzip member that is NOT in the parallel layout (any valid gzip header). Returns bytes used or 0 on error.
size_t decodeForeignMember(const uint8_t* in, size_t n, std::vector<uint8_t>& out, std::string& err) {
    if (n < 18 || in[0] != 0x1f || in[1] != 0x8b) { err = "not in gzip format (bad magic)"; return 0; }
    if (in[2] != 8) { err = "unsupported compression method"; return 0; }
    const uint8_t flg = in[3];
    if (flg & 0xE0) { err = "unsupported gzip flags"; return 0; }
    size_t p = 10;
    if (flg & 4) {
        if (p + 2 > n) { err = "truncated gzip header"; return 0; }
        p += 2 + (size_t(in[p]) | size_t(in[p + 1]) << 8);
    }
    for (int bit : {8, 16})  // FNAME, FCOMMENT: zero-terminated strings
        if (flg & bit) {
            while (p < n && in[p]) ++p;
            ++p;
        }
    if (flg & 2) p += 2;
    if (p >= n) { err = "truncated gzip header"; return 0; }
    size_t used = 0;
    if (!inflateRaw(in + p, n - p, out, std::min<size_t>((n - p) * 4, size_t(256) << 20), kMaxForeignMember, &used)) {
        err = "corrupt or truncated compressed data";
        return 0;
    }
    p += used;
    if (p + kTrailerBytes > n) { err = "truncated gzip trailer"; return 0; }
    if (rd32(in + p) != crc32(out.data(), out.size())) { err = "CRC-32 mismatch: data is corrupted"; return 0; }
    if (rd32(in + p + 4) != static_cast<uint32_t>(out.size())) { err = "length mismatch: data is corrupted"; return 0; }
    return p + kTrailerBytes;
}

}  // namespace

bool decompressStream(const uint8_t* in, size_t n, unsigned threads, const Sink& sink, Stats& st, std::string& err,
                      const Progress& progress) {
    err.clear();
    if (threads == 0) threads = defaultThreads();
    const double t0 = now();
    st = Stats();
    st.inBytes = n;
    st.threads = threads;
    bool ok = false;

    std::vector<MemberInfo> mem;
    if (listMembers(in, n, mem)) {
        st.blocks = mem.size();
        uint64_t doneIn = 0;
        ok = orderedParallel(
            mem.size(), threads, size_t(threads) * 2 + 2,
            [&](size_t i, std::vector<uint8_t>& r, std::string& e) {
                const MemberInfo& m = mem[i];
                const uint8_t* body = in + m.offset + kHeaderBytes;
                const size_t bodyLen = m.compSize - kHeaderBytes - kTrailerBytes;
                size_t used = 0;
                // The size field is untrusted: DEFLATE cannot expand more than ~1032x, so cap the up-front allocation.
                const size_t hint = static_cast<size_t>(std::min<uint64_t>(m.rawSize, uint64_t(bodyLen) * 1100 + 1024));
                if (!inflateRaw(body, bodyLen, r, hint, m.rawSize, &used) || used != bodyLen || r.size() != m.rawSize) {
                    e = "corrupt data in block " + std::to_string(i + 1);
                    return false;
                }
                if (rd32(in + m.offset + m.compSize - 8) != crc32(r.data(), r.size())) {
                    e = "CRC-32 mismatch in block " + std::to_string(i + 1) + ": data is corrupted";
                    return false;
                }
                return true;
            },
            [&](size_t i, const std::vector<uint8_t>& r) {
                if (!sink(r.data(), r.size())) return false;
                st.outBytes += r.size();
                doneIn += mem[i].compSize;
                if (progress) progress(doneIn, n);
                return true;
            },
            err);
    } else {  // gzip from another tool: members must be decoded one after another
        st.parallelLayout = false;
        st.threads = 1;
        size_t off = 0;
        ok = true;
        std::vector<uint8_t> buf;
        while (off < n) {
            if (std::all_of(in + off, in + n, [](uint8_t b) { return b == 0; })) break;  // trailing zero padding
            size_t used = decodeForeignMember(in + off, n - off, buf, err);
            if (!used) { ok = false; break; }
            if (!sink(buf.data(), buf.size())) { err = "output error (disk full or write failed)"; ok = false; break; }
            st.outBytes += buf.size();
            ++st.blocks;
            off += used;
            if (progress) progress(off, n);
        }
    }
    st.seconds = now() - t0;
    return ok;
}

bool compressBuffer(const uint8_t* in, size_t n, const CompressOptions& opt, std::vector<uint8_t>& out, std::string& err) {
    out.clear();
    Stats st;
    return compressStream(in, n, opt, [&](const uint8_t* p, size_t k) { out.insert(out.end(), p, p + k); return true; }, st, err);
}

bool decompressBuffer(const uint8_t* in, size_t n, unsigned threads, std::vector<uint8_t>& out, std::string& err) {
    out.clear();
    Stats st;
    return decompressStream(in, n, threads, [&](const uint8_t* p, size_t k) { out.insert(out.end(), p, p + k); return true; }, st, err);
}

}  // namespace blockz
