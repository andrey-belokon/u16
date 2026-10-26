#!/bin/sh
# Compile, assemble and run:  ./run.sh prog.b16 [cpu16 options]
# For example: ./run.sh examples/bare/screen.b16 --screen
# The examples in examples/monitor are built by the monitor's tools/b16build.sh: they need the monitor, not a bare machine.
# The standard library is pulled in as include "console.b16" (the lib directory is in -I).
# Extra compiler options: B16FLAGS, for example B16FLAGS=-s ./run.sh prog.b16
set -e
D=$(dirname "$0")
SRC=$1; shift
B=${SRC%.b16}
"$D/b16c" $B16FLAGS -I "$D/lib" "$SRC" -o "$B.s"
${CPU16ASM:-cpu16asm} -o "$B.bin" -l "$B.lst" "$B.s" >/dev/null
${CPU16:-cpu16} "$@" "$B.bin"
