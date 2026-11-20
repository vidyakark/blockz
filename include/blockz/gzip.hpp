#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace blockz {

struct CompressOptions {
    int level = 6;               // 1 (fast) .. 9 (small)
    unsigned threads = 0;        // 0 = all hardware threads
    size_t blockSize = 1 << 20;  // input bytes per independently compressed block
};

struct Stats {
    uint64_t inBytes = 0, outBytes = 0, blocks = 0;
    double seconds = 0;
    unsigned threads = 0;
    bool parallelLayout = true;  // false when decoding a .gz made by another tool (sequential path)
};

using Sink = std::function<bool(const uint8_t*, size_t)>;         // return false to abort
using Progress = std::function<void(uint64_t done, uint64_t total)>;

// Output is a standard gzip file: one gzip member per block, each carrying a small extra field ("SX") with
// its compressed size so that decompression can find all member boundaries without decoding and then
// decode every block on its own thread. Any gzip/zlib tool can read the result.
bool compressStream(const uint8_t* in, size_t n, const CompressOptions& opt, const Sink& sink, Stats& st,
                    std::string& err, const Progress& progress = nullptr);

// Decodes any valid gzip data. Files in the layout above decode in parallel; foreign gzip files
// (single stream, no size field) decode sequentially. Verifies CRC-32 and size of every member.
bool decompressStream(const uint8_t* in, size_t n, unsigned threads, const Sink& sink, Stats& st, std::string& err,
                      const Progress& progress = nullptr);

struct MemberInfo {
    uint64_t offset, compSize, rawSize;
};
// Lists the members of a file in the parallel layout; false if the data is not in that layout.
bool listMembers(const uint8_t* in, size_t n, std::vector<MemberInfo>& out);

// Convenience wrappers for in-memory data.
bool compressBuffer(const uint8_t* in, size_t n, const CompressOptions& opt, std::vector<uint8_t>& out, std::string& err);
bool decompressBuffer(const uint8_t* in, size_t n, unsigned threads, std::vector<uint8_t>& out, std::string& err);

unsigned defaultThreads();

}  // namespace blockz
