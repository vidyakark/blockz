#!/bin/sh
# Interoperability tests: blockz output must be readable by gzip (and Python), and blockz must read gzip output.
set -e
B=./build/blockz
G=./build/gen_data
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
fail() { echo "FAIL: $*"; exit 1; }

$G 6 "$T/text" text >/dev/null
$G 6 "$T/log" log >/dev/null
$G 6 "$T/mixed" mixed >/dev/null
: > "$T/empty"
printf 'x' > "$T/one"

for f in text log mixed empty one; do
  for lvl in 1 6 9; do
    $B compress -q -f -l $lvl -t 4 -b 256 -o "$T/$f.gz" "$T/$f"
    gzip -t "$T/$f.gz"                                   || fail "gzip -t rejects $f level $lvl"
    gzip -dc "$T/$f.gz" | cmp - "$T/$f"                  || fail "gzip -dc differs for $f level $lvl"
    $B decompress -q -f -t 4 -o "$T/$f.out" "$T/$f.gz"
    cmp "$T/$f.out" "$T/$f"                              || fail "blockz roundtrip differs for $f level $lvl"
  done
  # a gzip made by the real gzip tool (single stream, different Huffman choices) must decode too
  gzip -c "$T/$f" > "$T/$f.sys.gz"
  $B decompress -q -f -o "$T/$f.out2" "$T/$f.sys.gz"
  cmp "$T/$f.out2" "$T/$f"                               || fail "cannot read system gzip output for $f"
  $B test -q "$T/$f.sys.gz"
done
echo "gzip interoperability: OK"

if command -v python3 >/dev/null 2>&1; then
  python3 - "$T" <<'PY'
import gzip, sys
t = sys.argv[1]
for f in ("text", "log", "mixed", "empty", "one"):
    assert gzip.decompress(open(f"{t}/{f}.gz", "rb").read()) == open(f"{t}/{f}", "rb").read(), f
print("python gzip module interoperability: OK")
PY
fi

# Corrupted input must be detected and reported with a non-zero exit status.
cp "$T/text.gz" "$T/bad.gz"
printf '\377' | dd of="$T/bad.gz" bs=1 seek=100000 conv=notrunc 2>/dev/null
if $B test -q "$T/bad.gz" 2>/dev/null; then fail "corruption not detected"; fi
head -c 50000 "$T/text.gz" > "$T/trunc.gz"
if $B test -q "$T/trunc.gz" 2>/dev/null; then fail "truncation not detected"; fi
echo "corruption detection: OK"

# CLI safety: refuse to overwrite, refuse unknown files
if $B compress -q "$T/text" -o "$T/text.gz" 2>/dev/null; then fail "overwrote existing file without -f"; fi
if $B decompress -q "$T/nonexistent.gz" 2>/dev/null; then fail "accepted missing input"; fi
$B info "$T/text.gz" | grep -q "blocks" || fail "info output"
echo "cli checks: OK"
