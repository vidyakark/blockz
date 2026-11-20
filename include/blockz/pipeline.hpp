#pragma once
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace blockz {

// Runs work(i, result, err) for i in [0, count) on `threads` worker threads and hands every result to
// sink(i, result) strictly in index order on the calling thread.
//
// Back-pressure: workers never start a block more than `window` positions ahead of the sink, so memory use
// is bounded by roughly window * blockSize no matter how large the input is. Stops at the first failure.
template <class Work, class Sink>
bool orderedParallel(size_t count, unsigned threads, size_t window, Work work, Sink sink, std::string& err) {
    if (threads == 0) threads = 1;
    if (window < threads) window = threads;
    std::mutex m;
    std::condition_variable cv;
    size_t next = 0, writePos = 0;
    bool failed = false;
    std::vector<std::vector<uint8_t>> results(count);
    std::vector<char> ready(count, 0);

    auto worker = [&] {
        for (;;) {
            size_t i;
            {
                std::unique_lock<std::mutex> lk(m);
                cv.wait(lk, [&] { return failed || next >= count || next < writePos + window; });
                if (failed || next >= count) return;
                i = next++;
            }
            std::vector<uint8_t> r;
            std::string e;
            bool ok = work(i, r, e);
            {
                std::lock_guard<std::mutex> lk(m);
                if (!ok) {
                    if (!failed) err = e;
                    failed = true;
                } else {
                    results[i] = std::move(r);
                    ready[i] = 1;
                }
            }
            cv.notify_all();
        }
    };

    std::vector<std::thread> pool;
    for (unsigned t = 0; t < threads; ++t) pool.emplace_back(worker);

    for (size_t i = 0; i < count; ++i) {
        std::vector<uint8_t> r;
        {
            std::unique_lock<std::mutex> lk(m);
            cv.wait(lk, [&] { return failed || ready[i]; });
            if (failed) break;
            r = std::move(results[i]);
        }
        if (!sink(i, r)) {
            std::lock_guard<std::mutex> lk(m);
            if (!failed) err = "output error (disk full or write failed)";
            failed = true;
            break;
        }
        {
            std::lock_guard<std::mutex> lk(m);
            ++writePos;
        }
        cv.notify_all();
    }
    {
        std::lock_guard<std::mutex> lk(m);
        // If we left the loop early, make sure workers stop.
        if (writePos < count) failed = true;
    }
    cv.notify_all();
    for (auto& t : pool) t.join();
    return err.empty() && writePos == count;
}

}  // namespace blockz
