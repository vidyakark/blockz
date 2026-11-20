// Thread-scaling benchmark.   bench <file> [maxThreads] [level] [blockKiB]
// Measures pure CPU work: the compressed/decompressed bytes are counted, not written to disk.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "blockz/gzip.hpp"

using namespace blockz;

static double best(int runs, const std::function<double()>& f) {
    double b = 1e30;
    for (int i = 0; i < runs; ++i) b = std::min(b, f());
    return b;
}

int main(int argc, char** argv) {
    if (argc < 2) { std::fputs("usage: bench <file> [maxThreads] [level] [blockKiB]\n", stderr); return 2; }
    unsigned maxT = argc >= 3 ? std::atoi(argv[2]) : defaultThreads();
    int level = argc >= 4 ? std::atoi(argv[3]) : 6;
    size_t blockKiB = argc >= 5 ? std::strtoull(argv[4], nullptr, 10) : 1024;

    FILE* f = std::fopen(argv[1], "rb");
    if (!f) { std::perror(argv[1]); return 1; }
    std::vector<uint8_t> data;
    uint8_t buf[1 << 16];
    for (size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) data.insert(data.end(), buf, buf + n);
    std::fclose(f);
    const double mb = data.size() / 1e6;

    std::vector<unsigned> counts;
    for (unsigned t = 1; t < maxT; t *= 2) counts.push_back(t);
    counts.push_back(maxT);

    // Reference compressed file (made with all threads) for the decompression runs.
    std::vector<uint8_t> packed;
    std::string err;
    CompressOptions o;
    o.level = level;
    o.threads = maxT;
    o.blockSize = blockKiB * 1024;
    if (!compressBuffer(data.data(), data.size(), o, packed, err)) { std::fprintf(stderr, "error: %s\n", err.c_str()); return 1; }

    std::printf("file: %s (%.1f MB)  level %d  block %zu KiB  ->  %.1f MB (%.1f%% of original, %.2f:1)\n\n", argv[1], mb, level, blockKiB,
                packed.size() / 1e6, 100.0 * packed.size() / data.size(), double(data.size()) / packed.size());
    std::printf("%8s | %9s %9s %8s | %9s %9s %8s\n", "threads", "comp s", "comp MB/s", "speedup", "decomp s", "dec MB/s", "speedup");
    double c1 = 0, d1 = 0;
    for (unsigned t : counts) {
        o.threads = t;
        double cs = best(3, [&] {
            Stats st;
            uint64_t sunk = 0;
            if (!compressStream(data.data(), data.size(), o, [&](const uint8_t*, size_t n) { sunk += n; return true; }, st, err)) std::exit(1);
            return st.seconds;
        });
        double ds = best(3, [&] {
            Stats st;
            uint64_t sunk = 0;
            if (!decompressStream(packed.data(), packed.size(), t, [&](const uint8_t*, size_t n) { sunk += n; return sunk != 0 || n == 0; }, st, err)) {
                std::fprintf(stderr, "decompress error: %s\n", err.c_str());
                std::exit(1);
            }
            return st.seconds;
        });
        if (t == counts.front()) { c1 = cs; d1 = ds; }
        std::printf("%8u | %9.3f %9.0f %7.2fx | %9.3f %9.0f %7.2fx\n", t, cs, mb / cs, c1 / cs, ds, mb / ds, d1 / ds);
    }
    return 0;
}
