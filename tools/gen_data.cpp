// Generates test data for benchmarking.   gen_data <MiB> <out> [text|log|mixed] [seed]
//   text  - Zipf-distributed pseudo-words (compresses like natural-language text)
//   log   - synthetic web-server log lines (highly repetitive structure)
//   mixed - text, logs, random (incompressible) bytes and zero runs interleaved
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 3) { std::fputs("usage: gen_data <MiB> <out> [text|log|mixed] [seed]\n", stderr); return 2; }
    const size_t target = std::strtoull(argv[1], nullptr, 10) << 20;
    const std::string kind = argc >= 4 ? argv[3] : "text";
    std::mt19937_64 rng(argc >= 5 ? std::strtoull(argv[4], nullptr, 10) : 42);

    const char* cons = "bcdfghklmnprstvwz";
    const char* vows = "aeiouy";
    std::vector<std::string> vocab;
    while (vocab.size() < 30000) {
        std::string w;
        for (int s = 1 + rng() % 3; s > 0; --s) { w += cons[rng() % 17]; w += vows[rng() % 6]; if (rng() % 3 == 0) w += cons[rng() % 17]; }
        vocab.push_back(w);
    }
    std::vector<double> cdf(vocab.size());
    double sum = 0;
    for (size_t i = 0; i < vocab.size(); ++i) cdf[i] = (sum += 1.0 / std::pow(i + 1, 1.0));
    std::uniform_real_distribution<double> uni(0, sum);
    auto word = [&]() -> const std::string& {
        size_t i = std::upper_bound(cdf.begin(), cdf.end(), uni(rng)) - cdf.begin();
        return vocab[std::min(i, vocab.size() - 1)];
    };

    auto text = [&](std::string& o, size_t bytes) {
        size_t end = o.size() + bytes;
        int inSentence = 0;
        while (o.size() < end) {
            const std::string& w = word();
            if (inSentence == 0) { o += char(w[0] - 32); o += w.substr(1); }
            else o += w;
            if (++inSentence > 6 + int(rng() % 12)) { o += (rng() % 10 == 0) ? ".\n" : ". "; inSentence = 0; }
            else o += (rng() % 12 == 0) ? ", " : " ";
        }
    };
    auto logs = [&](std::string& o, size_t bytes) {
        static const char* lvl[] = {"INFO", "INFO", "INFO", "WARN", "ERROR", "DEBUG"};
        static const char* path[] = {"/api/items", "/api/users", "/api/orders", "/static/app.js", "/login", "/health"};
        size_t end = o.size() + bytes;
        char buf[256];
        long t = 1700000000;
        while (o.size() < end) {
            t += rng() % 3;
            int n = std::snprintf(buf, sizeof buf, "%ld %s user=%u GET %s/%u status=%d took=%ums ip=10.%u.%u.%u\n", t, lvl[rng() % 6],
                                  unsigned(rng() % 5000), path[rng() % 6], unsigned(rng() % 900), (rng() % 20 == 0) ? 500 : 200,
                                  unsigned(rng() % 300), unsigned(rng() % 4), unsigned(rng() % 255), unsigned(rng() % 255));
            o.append(buf, n);
        }
    };

    FILE* f = std::fopen(argv[2], "wb");
    if (!f) { std::perror("open"); return 1; }
    std::string chunk;
    size_t written = 0;
    while (written < target) {
        chunk.clear();
        size_t want = std::min<size_t>(target - written, 4 << 20);
        if (kind == "text") text(chunk, want);
        else if (kind == "log") logs(chunk, want);
        else {  // mixed: pieces of 64..512 KiB of different kinds
            while (chunk.size() < want) {
                size_t piece = (64 + rng() % 448) << 10;
                switch (rng() % 4) {
                    case 0: text(chunk, piece); break;
                    case 1: logs(chunk, piece); break;
                    case 2: for (size_t i = 0; i < piece; ++i) chunk += char(rng()); break;
                    default: chunk.append(piece / 4, '\0'); break;
                }
            }
        }
        chunk.resize(std::min(chunk.size(), target - written));
        std::fwrite(chunk.data(), 1, chunk.size(), f);
        written += chunk.size();
    }
    std::fclose(f);
    std::printf("wrote %zu MiB of '%s' data to %s\n", written >> 20, kind.c_str(), argv[2]);
    return 0;
}
