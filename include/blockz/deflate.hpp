#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace blockz {

// Compresses [in, in+n) into one raw DEFLATE stream (RFC 1951) appended to `out`.
// level 1 (fastest) .. 9 (smallest). The stream ends with a block marked final.
void deflateCompress(const uint8_t* in, size_t n, int level, std::vector<uint8_t>& out);

// Decodes one raw DEFLATE stream. On success `out` holds exactly the decoded bytes and
// *consumed is the number of input bytes the stream occupied. Returns false on any corrupt,
// truncated or over-long (> maxOut) input; it never reads or writes out of bounds.
bool inflateRaw(const uint8_t* in, size_t n, std::vector<uint8_t>& out, size_t sizeHint,
                size_t maxOut, size_t* consumed);

}  // namespace blockz
