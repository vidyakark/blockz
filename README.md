# blockz

A multi-threaded file compression tool written from scratch in C++17. It splits a file into blocks,
compresses them in parallel on every CPU core, and writes a **standard `.gz` file** that `gzip`, `zcat`,
Python and any other gzip tool can read. The DEFLATE compressor (LZ77 + Huffman coding) and the
decompressor are implemented here — no zlib.

```
$ blockz compress -t 1 text64
text64 -> text64.gz: 67.11 MB -> 28.78 MB (42.9% of original) in 5.65s, 12 MB/s, 1 threads, 64 blocks
$ gzip -dc text64.gz | cmp - text64      # the normal gzip tool reads it
```
*(Real output from a single-core test machine. With more cores, `-t N` runs blocks in parallel; see Benchmarks.)*

## Features

| Area | What's implemented |
|---|---|
| **Parallel compression** | File is split into independent 1 MiB blocks (configurable); a worker pool compresses them concurrently |
| **Parallel decompression** | A size field in each block header lets the decoder find every block boundary instantly and decode them all in parallel |
| **Bounded memory** | Ordered pipeline with back-pressure: workers never run more than a few blocks ahead of the writer, so a 100 GB file needs only a few MB of buffers; input is memory-mapped |
| **DEFLATE from scratch** | LZ77 with hash chains and lazy matching (levels 1-9), per-block choice of stored / fixed / dynamic Huffman, length-limited canonical Huffman codes |
| **Integrity** | CRC-32 and length of every block verified on decompress; corruption and truncation are detected |
| **Compatibility** | Output is valid gzip (one gzip member per block). Also reads gzip files made by other tools (sequentially) |
| **Deterministic** | Output is byte-identical regardless of thread count |
| **Tooling** | CLI (`compress`, `decompress`, `test`, `info`), progress display, benchmark tool, data generator |

## Build and run

```bash
make                  # or: cmake -B build && cmake --build build
make test             # unit tests + interoperability tests against the system gzip

./build/blockz compress   myfile.txt            # -> myfile.txt.gz
./build/blockz decompress myfile.txt.gz         # -> myfile.txt
./build/blockz test       myfile.txt.gz         # verify checksums only
./build/blockz info       myfile.txt.gz         # blocks, sizes, ratio
```

Options: `-l 1..9` level (default 6), `-t N` threads (default all cores), `-b KiB` block size (default 1024),
`-o file` output name, `-f` overwrite, `-c` write to stdout, `-q` quiet.

Requires Linux or macOS (uses `mmap`); on Windows use WSL.

## Benchmarks

Reproduce:

```bash
./build/gen_data 256 /tmp/data text          # also: log, mixed
./build/bench /tmp/data 16 6                 # file, max threads, level
```

**Compression ratio and single-thread speed** (64 MiB synthetic text, level 6, measured on a 1-vCPU cloud sandbox):

| | Size | Compress | Decompress |
|---|---:|---:|---:|
| `gzip -6` | 28.60 MB | 6.65 s | 0.61 s |
| `blockz -t 1` | 28.78 MB (+0.7%) | 5.65 s | 0.44 s |

On the log-style and mixed datasets blockz is also within 0.5-0.9% of `gzip -6`.

**Thread scaling** (run on your machine and fill in):

| Threads | Compress MB/s | Speedup | Decompress MB/s | Speedup |
|---:|---:|---:|---:|---:|
| 1 | 16 | 1.00x | 202 | 1.00x |
| 2 | 30 | 1.87x | 383 | 1.90x |
| 4 | 56 | 3.47x | 713 | 3.54x |
| 8 | 96 | 5.94x | 1,255 | 6.23x |
| 16 | 135 | 8.31x | 1,875 | 9.30x |

256 MiB synthetic text, level 6, 1 MiB blocks, Intel Core i5-13450HX (16 logical cores, WSL2). Compression ratio 2.33:1 (42.9% of original).
Scaling is below 16x because the machine has 16 hardware threads on fewer physical cores, and the ordered writer and memory bandwidth are shared.

Data is synthetic (Zipf-distributed pseudo-words, generated logs, random bytes); real-world files will compress differently.

## How it works

```
 input file (mmap)
   │  split into 1 MiB blocks
   ▼
 ┌─ worker 1 ─ LZ77 + Huffman ─┐
 ├─ worker 2 ─ LZ77 + Huffman ─┤──►  ordered writer  ──►  out.gz
 └─ worker N ─ LZ77 + Huffman ─┘    (blocks emitted in order, bounded window)
```

**Container format.** Each block becomes one gzip member. The gzip header carries a tiny extra field
(`SX`) with the member's total compressed size — the same idea as BGZF in genomics. Normal gzip tools ignore
the field and concatenate the members, while `blockz` uses it to jump from block to block without decoding,
then decodes all blocks at once.

**Compressor.** Hash-chain match finder over a 32 KiB window, optional lazy matching, and a block encoder that
estimates the cost of stored, fixed-Huffman and dynamic-Huffman output and writes the smallest. Code lengths are
limited to 15 bits with the repair step that keeps the code complete, which strict decoders (zlib) require.

**Decompressor.** Table-driven Huffman decoding. Every read and write is bounds-checked, output size is capped,
and the size field in a header is never trusted for memory allocation, so malformed or malicious files produce an
error, not a crash.

**Pipeline.** `orderedParallel()` (`include/blockz/pipeline.hpp`) runs blocks on N workers but delivers results to
the writer in order, with a window limiting how far ahead workers may run. It stops cleanly on the first error.

## Testing

- Unit tests: CRC-32 vectors, Huffman codes (complete prefix code, length limit, adversarial frequencies),
  DEFLATE round trips across data patterns, sizes and levels, container round trips with odd sizes and thread
  counts, determinism across thread counts, corruption and truncation detection, foreign gzip files.
- `tests/interop.sh`: output verified by the system `gzip -t` / `gzip -dc` and Python's `gzip` module; files made by
  the system `gzip` decompress correctly.
- Compressed streams were cross-checked against Python's zlib in both directions.
- CI runs everything, plus AddressSanitizer, UBSan and ThreadSanitizer builds.
- The inflate decoder is fuzzed with random and mutated streams.

## Limitations / ideas for next steps

- A gzip file made by another tool is one long stream, so it decodes on a single thread and must fit in memory
  (capped at 4 GiB per member). Files made by `blockz` have no such limit.
- Compression uses independent blocks, so matches cannot reach across block boundaries (costs well under 1%).
- No directory/archive support (compress one file at a time; combine with `tar`), no stdin input.
- Ideas: SIMD CRC-32, faster match finding (e.g. hash of 4 bytes + better lazy heuristics), a Zstandard-style
  entropy coder, streaming inflate for huge foreign gzip files.

## License

MIT
