#!/bin/sh
# b16build.sh path/prog.b16 [b16c options] - build a program to run under the monitor: out/<prog>.bin,
# out/<prog>.map. The image parameters (option max_top, option min_free) and assembler modules
# (include asm; sys.asm pulls in sys.b16) are set by the source itself (B16, sect. 3.2, 3.6).
# Include paths: the source directory, user/lib, kernel (abi.b16). Variables: B16C, CPU16ASM,
# B16FLAGS.
set -e
SRC=$1; shift
D=$(cd "$(dirname "$0")/.." && pwd)
S=$(cd "$(dirname "$SRC")" && pwd)
N=$(basename "$SRC" .b16)
O=$D/out
mkdir -p "$O"
"${B16C:-$D/../b16c/b16c}" -Werror -I "$S" -I "$D/user/lib" -I "$D/kernel" ${B16FLAGS:-} "$@" "$SRC" -o "$O/$N.s"
"${CPU16ASM:-$D/../cpu16/build/cpu16asm}" --exec -m "$O/$N.map" -l "$O/$N.lst" -o "$O/$N.bin" "$O/$N.s"
