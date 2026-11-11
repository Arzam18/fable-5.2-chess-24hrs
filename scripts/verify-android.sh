#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BIN="$ROOT/source/build/fable-5.2-android-arm64-neon"
NDK_BIN="${ANDROID_NDK_HOME:-${ANDROID_NDK_ROOT:-}}/toolchains/llvm/prebuilt/linux-x86_64/bin"
OBJDUMP="${OBJDUMP:-$NDK_BIN/llvm-objdump}"

[[ -s "$BIN" ]] || { echo "ERROR: missing Android engine: $BIN" >&2; exit 1; }
readelf -h "$BIN" | grep -q 'AArch64' || { echo "ERROR: output is not AArch64 ELF" >&2; exit 1; }

if [[ ! -x "$OBJDUMP" ]]; then
    OBJDUMP="$(command -v llvm-objdump || true)"
fi
[[ -n "$OBJDUMP" && -x "$OBJDUMP" ]] || { echo "ERROR: llvm-objdump not found" >&2; exit 1; }

ASM="$(mktemp)"
trap 'rm -f "$ASM"' EXIT
"$OBJDUMP" -d "$BIN" > "$ASM"
if ! grep -Eiq '\b(smull|smull2)\s+v[0-9]+' "$ASM"; then
    echo "ERROR: no NEON widening multiply found in linked executable" >&2
    exit 1
fi
if grep -Eiq '\b(sdot|udot)\b' "$ASM"; then
    echo "ERROR: unexpected Dotprod instruction in baseline NEON artifact" >&2
    exit 1
fi

echo "PASS: AArch64 ELF verified."
echo "PASS: NEON widening multiply is present in linked NNUE code."
echo "INFO: no SDOT/UDOT; this network's signed 16-bit output math is not Dotprod-compatible."
if readelf -d "$BIN" | grep -q 'libc++_shared.so'; then
    echo "ERROR: engine unexpectedly depends on libc++_shared.so" >&2
    exit 1
fi
echo "PASS: no libc++_shared.so dependency."
