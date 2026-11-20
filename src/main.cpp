// blockz - multi-threaded file compressor producing standard .gz files.
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "blockz/gzip.hpp"

using namespace blockz;

namespace {

int usage() {
    std::fputs(
        "blockz - multi-threaded compression tool (writes standard .gz files)\n\n"
        "  blockz compress   [-l 1-9] [-t threads] [-b blockKiB] [-o out] [-f] [-c] [-q] <file>\n"
        "  blockz decompress [-t threads] [-o out] [-f] [-c] [-q] <file.gz>\n"
        "  blockz test       [-t threads] <file.gz>      verify checksums without writing anything\n"
        "  blockz info       <file.gz>                   show block layout and compression ratio\n\n"
        "  -l  level 1 (fastest) .. 9 (smallest), default 6      -t  worker threads, default: all cores\n"
        "  -b  block size in KiB, default 1024                   -o  output file       -f  overwrite output\n"
        "  -c  write to stdout                                   -q  no statistics\n",
        stderr);
    return 2;
}

// Read-only memory-mapped view of a file.
struct MappedFile {
    const uint8_t* data = nullptr;
    size_t size = 0;
    void* map = nullptr;
    bool open(const std::string& path, std::string& err) {
        int fd = ::open(path.c_str(), O_RDONLY);
        if (fd < 0) { err = "cannot open " + path + ": " + std::strerror(errno); return false; }
        struct stat st;
        if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) { ::close(fd); err = path + " is not a regular file"; return false; }
        size = static_cast<size_t>(st.st_size);
        if (size == 0) { static const uint8_t empty = 0; data = &empty; ::close(fd); return true; }
        map = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
        ::close(fd);
        if (map == MAP_FAILED) { map = nullptr; err = "cannot read " + path; return false; }
        madvise(map, size, MADV_SEQUENTIAL);
        data = static_cast<const uint8_t*>(map);
        return true;
    }
    ~MappedFile() { if (map) munmap(map, size); }
};

bool fileExists(const std::string& p) { struct stat st; return stat(p.c_str(), &st) == 0; }

std::string human(double bytes) {
    char b[32];
    if (bytes >= 1e9) std::snprintf(b, sizeof b, "%.2f GB", bytes / 1e9);
    else if (bytes >= 1e6) std::snprintf(b, sizeof b, "%.2f MB", bytes / 1e6);
    else if (bytes >= 1e3) std::snprintf(b, sizeof b, "%.1f KB", bytes / 1e3);
    else std::snprintf(b, sizeof b, "%.0f B", bytes);
    return b;
}

struct Args {
    std::string cmd, input, output;
    int level = 6;
    unsigned threads = 0;
    size_t blockKiB = 1024;
    bool force = false, toStdout = false, quiet = false;
};

bool parseArgs(int argc, char** argv, Args& a) {
    a.cmd = argv[1];
    for (int i = 2; i < argc; ++i) {
        std::string s = argv[i];
        auto val = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
        if (s == "-l" || s == "-t" || s == "-b" || s == "-o") {
            const char* v = val();
            if (!v) { std::fprintf(stderr, "error: %s needs a value\n", s.c_str()); return false; }
            if (s == "-o") a.output = v;
            else {
                char* end;
                long x = std::strtol(v, &end, 10);
                if (*end || x < 0) { std::fprintf(stderr, "error: bad number for %s: %s\n", s.c_str(), v); return false; }
                if (s == "-l") a.level = static_cast<int>(x);
                else if (s == "-t") a.threads = static_cast<unsigned>(x);
                else a.blockKiB = static_cast<size_t>(x);
            }
        } else if (s == "-f") a.force = true;
        else if (s == "-c") a.toStdout = true;
        else if (s == "-q") a.quiet = true;
        else if (!s.empty() && s[0] == '-') { std::fprintf(stderr, "error: unknown option %s\n", s.c_str()); return false; }
        else if (a.input.empty()) a.input = s;
        else { std::fprintf(stderr, "error: only one input file is supported\n"); return false; }
    }
    if (a.input.empty()) { std::fputs("error: no input file\n", stderr); return false; }
    if (a.level < 1 || a.level > 9) { std::fputs("error: level must be 1..9\n", stderr); return false; }
    if (a.blockKiB < 4 || a.blockKiB > 131072) { std::fputs("error: block size must be 4..131072 KiB\n", stderr); return false; }
    return true;
}

Progress makeProgress(bool enabled) {
    if (!enabled || !isatty(2)) return nullptr;
    return [](uint64_t done, uint64_t total) {
        int pct = total ? static_cast<int>(100.0 * double(done) / double(total)) : 100;
        std::fprintf(stderr, "\r  %3d%%", pct);
        std::fflush(stderr);
    };
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) return usage();
    Args a;
    if (!parseArgs(argc, argv, a)) return 2;
    const bool isC = a.cmd == "compress", isD = a.cmd == "decompress", isT = a.cmd == "test", isI = a.cmd == "info";
    if (!isC && !isD && !isT && !isI) return usage();

    std::string err;
    MappedFile in;
    if (!in.open(a.input, err)) { std::fprintf(stderr, "error: %s\n", err.c_str()); return 1; }

    if (isI) {
        std::vector<MemberInfo> m;
        if (!listMembers(in.data, in.size, m)) {
            std::printf("%s: valid gzip layout is not blockz-style (no block table); `decompress` still works, single-threaded\n", a.input.c_str());
            return 0;
        }
        uint64_t raw = 0;
        for (auto& x : m) raw += x.rawSize;
        std::printf("file:         %s\nblocks:       %zu\ncompressed:   %s\noriginal:     %s\nratio:        %.2f:1  (%.1f%% of original)\n",
                    a.input.c_str(), m.size(), human(double(in.size)).c_str(), human(double(raw)).c_str(),
                    in.size ? double(raw) / double(in.size) : 0.0, raw ? 100.0 * double(in.size) / double(raw) : 0.0);
        return 0;
    }

    // Output destination
    std::string outPath = a.output;
    if (!isT && !a.toStdout && outPath.empty()) {
        if (isC) outPath = a.input + ".gz";
        else if (a.input.size() > 3 && a.input.compare(a.input.size() - 3, 3, ".gz") == 0) outPath = a.input.substr(0, a.input.size() - 3);
        else { std::fputs("error: input does not end in .gz; choose the output name with -o\n", stderr); return 2; }
    }
    FILE* out = nullptr;
    if (!isT) {
        if (a.toStdout) out = stdout;
        else {
            if (!a.force && fileExists(outPath)) { std::fprintf(stderr, "error: %s already exists (use -f to overwrite)\n", outPath.c_str()); return 1; }
            out = std::fopen(outPath.c_str(), "wb");
            if (!out) { std::fprintf(stderr, "error: cannot create %s: %s\n", outPath.c_str(), std::strerror(errno)); return 1; }
        }
    }
    Sink sink = [&](const uint8_t* p, size_t n) { return isT || n == 0 || std::fwrite(p, 1, n, out) == n; };

    Stats st;
    bool ok;
    Progress prog = makeProgress(!a.quiet && !a.toStdout);
    if (isC) {
        CompressOptions o;
        o.level = a.level;
        o.threads = a.threads;
        o.blockSize = a.blockKiB * 1024;
        ok = compressStream(in.data, in.size, o, sink, st, err, prog);
    } else {
        ok = decompressStream(in.data, in.size, a.threads, sink, st, err, prog);
    }
    if (prog) std::fputs("\r      \r", stderr);
    if (out && out != stdout && std::fclose(out) != 0) ok = false;
    if (!ok) {
        std::fprintf(stderr, "error: %s\n", err.empty() ? "write failed" : err.c_str());
        if (!isT && !a.toStdout) std::remove(outPath.c_str());
        return 1;
    }
    if (!a.quiet) {
        FILE* msg = a.toStdout ? stderr : stdout;
        double mb = double(isC ? st.inBytes : st.outBytes) / 1e6;
        if (isT) std::fprintf(msg, "%s: OK (%llu blocks, %s)\n", a.input.c_str(), (unsigned long long)st.blocks, human(double(st.outBytes)).c_str());
        else if (isC)
            std::fprintf(msg, "%s -> %s: %s -> %s (%.1f%% of original) in %.2fs, %.0f MB/s, %u threads, %llu blocks\n", a.input.c_str(),
                         a.toStdout ? "stdout" : outPath.c_str(), human(double(st.inBytes)).c_str(), human(double(st.outBytes)).c_str(),
                         st.inBytes ? 100.0 * double(st.outBytes) / double(st.inBytes) : 0.0, st.seconds, st.seconds > 0 ? mb / st.seconds : 0.0,
                         st.threads, (unsigned long long)st.blocks);
        else
            std::fprintf(msg, "%s -> %s: %s in %.2fs, %.0f MB/s, %u thread%s%s\n", a.input.c_str(), a.toStdout ? "stdout" : outPath.c_str(),
                         human(double(st.outBytes)).c_str(), st.seconds, st.seconds > 0 ? mb / st.seconds : 0.0, st.threads, st.threads == 1 ? "" : "s",
                         st.parallelLayout ? "" : " (gzip from another tool: sequential)");
    }
    return 0;
}
