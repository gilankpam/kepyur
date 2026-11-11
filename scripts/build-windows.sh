#!/usr/bin/env bash
# Desktop tools on Windows: kp_replay, kp_live, kp_sim -> build/windows/*.exe
# Needs MSYS2 (C:\msys64) with the UCRT64 toolchain and mipsel binutils in /opt/mipsel (see README).
# Runs from Git Bash or an MSYS2 shell.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
MSYS=${MSYS2_ROOT:-/c/msys64}
[ -x "$MSYS/ucrt64/bin/gcc.exe" ] || { echo "MSYS2 UCRT64 gcc not found under $MSYS (set MSYS2_ROOT)" >&2; exit 1; }
MIPS_BIN="$MSYS/opt/mipsel/bin"
[ -x "$MIPS_BIN/mipsel-linux-gnu-as.exe" ] || { echo "mipsel binutils not found in $MIPS_BIN (see README)" >&2; exit 1; }
export PATH="$MSYS/ucrt64/bin:$MSYS/usr/bin:$PATH"
export KEPYUR_MIPS_PREFIX="$(cygpath -m "$MIPS_BIN")/mipsel-linux-gnu-"
cmake -S "$ROOT/native" -B "$ROOT/build/windows" -G Ninja -DKEPYUR_RADIO=ON >/dev/null
ninja -C "$ROOT/build/windows" kp_replay kp_live kp_sim
# the MinGW runtime + libusb DLLs next to the exes, so they run from PowerShell / cmd too
for exe in "$ROOT"/build/windows/kp_{replay,live,sim}.exe; do
  ldd "$exe" | awk '/\/ucrt64\// { print $1 }'
done | sort -u | while read -r dll; do cp -u "$MSYS/ucrt64/bin/$dll" "$ROOT/build/windows/"; done
echo "built: $(cygpath -w "$ROOT/build/windows")"
