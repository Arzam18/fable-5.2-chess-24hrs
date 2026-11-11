#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SOURCE="$ROOT/source"
SRC="$SOURCE/src"
NET="${FABLE_NET_PATH:-$SOURCE/nets/k1_256.nnue}"
CXX="${CXX:-aarch64-linux-android24-clang++}"
OUT="$SOURCE/build/fable-5.2-android-arm64-neon"

if [[ ! -s "$NET" ]]; then
    echo "ERROR: required NNUE network is missing or empty: $NET" >&2
    echo "The uploaded source archive did not include source/nets/k1_256.nnue." >&2
    exit 1
fi
if ! command -v "$CXX" >/dev/null 2>&1; then
    echo "ERROR: Android ARM64 compiler not found: $CXX" >&2
    exit 1
fi

python3 - "$NET" <<'PY'
import pathlib, struct, sys
p = pathlib.Path(sys.argv[1])
data = p.read_bytes()
if len(data) < 16:
    raise SystemExit(f"ERROR: NNUE is too small: {len(data)} bytes")
magic, hidden, buckets, king_buckets = struct.unpack_from("<4i", data)
expected = 16 + (768 * 5 * 256 * 2) + (256 * 2) + (8 * 2 * 256 * 2) + (8 * 4)
if (magic, hidden, buckets, king_buckets) != (0x4E4E5548, 256, 8, 5):
    raise SystemExit(
        "ERROR: NNUE header mismatch; expected magic=NNUEH, hidden=256, "
        f"buckets=8, king-buckets=5; got {magic:#x}, {hidden}, {buckets}, {king_buckets}"
    )
if len(data) != expected:
    raise SystemExit(f"ERROR: NNUE size mismatch: got {len(data)}, expected {expected}")
print(f"PASS: validated 5-king-bucket NNUE ({len(data)} bytes): {p.name}")
PY

mkdir -p "$SOURCE/build"
cp "$NET" "$SRC/net.bin"
cleanup() { rm -f "$SRC/net.bin"; }
trap cleanup EXIT

MACROS="$($CXX -dM -E -x c++ /dev/null)"
grep -q '__aarch64__' <<<"$MACROS" || { echo "ERROR: compiler is not targeting AArch64" >&2; exit 1; }
grep -Eq '__ARM_NEON|__ARM_NEON__' <<<"$MACROS" || { echo "ERROR: compiler does not enable NEON" >&2; exit 1; }

cd "$SRC"
"$CXX" \
    -std=c++20 -O3 -DNDEBUG -pthread -flto \
    -march=armv8-a+simd -mtune=cortex-a76 \
    -DNN_H=256 -DNN_KB=5 \
    -ffunction-sections -fdata-sections \
    -Wall -Wno-unused-but-set-variable \
    attacks.cpp position.cpp eval.cpp nnue.cpp search.cpp uci.cpp datagen.cpp \
    -Wl,--gc-sections -Wl,--build-id=none \
    -static-libstdc++ \
    -o "$OUT"

file "$OUT"
readelf -h "$OUT" | grep -E 'Class:|Machine:'
echo "Built: $OUT"
