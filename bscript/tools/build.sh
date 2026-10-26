#!/bin/sh
# build.sh src.b16 out.bin [b16c options] - build a BScript program to run under the monitor. max_top is set by the
# option in the source; the call wrappers (sys.asm) are pulled in by sys.b16 with an include asm line.
set -e
R=$(cd "$(dirname "$0")/../.." && pwd)
SRC=$1; OUT=$2; shift 2
S=$(cd "$(dirname "$SRC")" && pwd)
D=$(dirname "$OUT")
N=$(basename "$OUT" .bin)
mkdir -p "$D"
"$R/b16c/b16c" -I "$S" -I "$R/bscript/common" -I "$R/monitor/user/lib" -I "$R/monitor/kernel" \
   ${B16FLAGS:-} "$@" "$SRC" -o "$D/$N.s"
"$R/cpu16/build/cpu16asm" --exec -m "$D/$N.map" -l "$D/$N.lst" -o "$OUT" "$D/$N.s"
