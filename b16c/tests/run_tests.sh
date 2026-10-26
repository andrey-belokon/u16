#!/bin/sh
# Regression tests: compilation, assembly (checking the --exec header and the memory
# map), running and comparing the output with tests/expected/*.out. To update the references: --update.
# Paths: B16C, CPU16ASM, CPU16, MONRUN (by default ./b16c, cpu16asm, cpu16 from PATH,
# tests/monrun). If MONRUN exists, each program is additionally run
#   - on a bare machine with memory filled with garbage (the startup code zeroes bss itself);
#   - with a simulated monitor (IF = 1, IRQ0 every 7 instructions, exit via LR, code in R0),
# except for programs marked "// test: bare-only" (own vectors, screen, WFI wait).
# A line "// b16c-flags: ..." at the start of a source sets the compiler options, and
# "// test: args A B" gives the command-line arguments under the monitor.
# A line "// test: steps N" marks a program that never terminates (game, animation): it is run on a bare machine
# for exactly N instructions and the screen snapshot (--screen) is compared with the reference; the instruction
# limit is not an error for such programs, and the garbage and monitor runs are skipped.
# Examples: examples/bare and examples/universal are run the same way; examples/monitor use
# monitor calls that monrun does not execute, so they are only built (--exec) - the monitor
# test bench runs them.
B16C=${B16C:-./b16c}
CPU16ASM=${CPU16ASM:-cpu16asm}
CPU16=${CPU16:-cpu16}
MONRUN=${MONRUN:-tests/monrun}
TMP=${TMPDIR:-/tmp}/b16c_tests.$$
mkdir -p "$TMP"
update=0; [ "$1" = "--update" ] && update=1
fail=0
have_mon=0; [ -x "$MONRUN" ] && have_mon=1

check() {   # check name output-file reference-file
    if [ $update = 1 ]; then cp "$2" "$3"; echo "updated $3"; return 0; fi
    if cmp -s "$2" "$3"; then return 0; fi
    echo "FAIL $1: output differs"; diff "$3" "$2" | head -5; fail=1; return 1
}

for src in tests/t*.b16 examples/bare/*.b16 examples/universal/*.b16; do
    name=$(basename "$src" .b16)
    exp=tests/expected/$name.out
    flags=$(sed -n 's|^// b16c-flags: *||p' "$src" | head -1)
    args=$(sed -n 's|^// test: args *||p' "$src" | head -1)
    bareonly=$(grep -c '^// test: bare-only' "$src")
    steps=$(sed -n 's|^// test: steps *||p' "$src" | head -1 | tr -d ' \r')
    if ! "$B16C" $flags -I lib "$src" -o "$TMP/$name.s" ||
       ! "$CPU16ASM" --exec -m "$TMP/$name.map" -o "$TMP/$name.bin" "$TMP/$name.s" >"$TMP/asm.log" 2>&1; then
        cat "$TMP/asm.log"
        echo "FAIL $name: build"; fail=1; continue
    fi
    if ! grep -q "  $src\$" "$TMP/$name.map"; then echo "FAIL $name: no lines $src in the memory map"; fail=1; fi
    opts=--strict-align; input=/dev/null
    case $name in screen) opts="$opts --screen";; esac
    nsteps=
    if [ -n "$steps" ]; then opts="$opts --screen"; nsteps="-n $steps"; fi
    [ -f "$(dirname "$src")/$name.txt" ] && input=$(dirname "$src")/$name.txt
    "$CPU16" $opts $nsteps "$TMP/$name.bin" < "$input" > "$TMP/$name.out" 2> "$TMP/$name.err"
    if [ -z "$steps" ] && grep -q "limit reached" "$TMP/$name.err"; then echo "FAIL $name: the emulator instruction limit was reached"; fail=1; continue; fi
    if grep -q "violations: [1-9]" "$TMP/$name.err"; then echo "FAIL $name: alignment"; fail=1; continue; fi
    check "$name" "$TMP/$name.out" "$exp" || continue
    ok="ok  "
    if [ -n "$steps" ]; then echo "ok s $name"; continue; fi
    if [ $have_mon = 1 ] && [ $update = 0 ]; then
        "$MONRUN" --garbage $opts "$TMP/$name.bin" < "$input" > "$TMP/$name.g.out" 2> "$TMP/$name.err"
        if grep -q "violations: [1-9]" "$TMP/$name.err" || ! cmp -s "$TMP/$name.g.out" "$exp"; then
            echo "FAIL $name: bare machine with garbage in memory"; diff "$exp" "$TMP/$name.g.out" | head -5; fail=1; continue
        fi
        ok="ok g"
        if [ "$bareonly" = 0 ]; then
            mexp=tests/expected/$name.mon; [ -f "$mexp" ] || mexp=$exp
            set --; for a in $args; do set -- "$@" --arg "$a"; done
            "$MONRUN" --monitor --strict-align "$@" "$TMP/$name.bin" < "$input" > "$TMP/$name.m.out" 2> "$TMP/$name.err"
            code=$(sed -n 's/^exit //p' "$TMP/$name.err")
            want=0; [ -f "tests/expected/$name.exit" ] && want=$(cat "tests/expected/$name.exit")
            if [ "$code" != "$want" ] || ! cmp -s "$TMP/$name.m.out" "$mexp" || grep -q "violations: [1-9]" "$TMP/$name.err"; then
                echo "FAIL $name: under the monitor (code $code, expected $want)"; diff "$mexp" "$TMP/$name.m.out" | head -5; fail=1; continue
            fi
            ok="${ok}m"
        fi
    fi
    # lines "// test: also OPTIONS": the same program built with other compiler options gives the same output
    for extra in $(sed -n 's|^// test: also *||p' "$src" | tr ' ' '_'); do
        keys=$(echo "$extra" | tr '_' ' ')
        if "$B16C" $flags $keys -I lib "$src" -o "$TMP/$name.k.s" 2>/dev/null &&
           "$CPU16ASM" --exec -o "$TMP/$name.k.bin" "$TMP/$name.k.s" >/dev/null 2>&1 &&
           "$CPU16" --strict-align "$TMP/$name.k.bin" < "$input" 2>/dev/null | cmp -s - "$exp"; then
            ok="${ok}k"
        else
            echo "FAIL $name: with keys $keys"; fail=1
        fi
    done
    echo "$ok $name"
done

# Linking with an assembler module (include asm): examples/universal/asmlink and tests/asmmod. The image is checked
# with --exec, run on a bare machine and under the monitor (monrun loads only the bytes up to image_end);
# the address of each function of the module must be less than __image_end.
asmlink_test() {   # name source module reference
    name=$1; src=$2; mod=$3; exp=$4
    if "$B16C" -Werror -I lib "$src" -o "$TMP/$name.s" &&
       "$CPU16ASM" --exec -m "$TMP/$name.map" -o "$TMP/$name.bin" "$TMP/$name.s" >"$TMP/asm.log" 2>&1; then
        ok="ok  "
        "$CPU16" --strict-align "$TMP/$name.bin" < /dev/null > "$TMP/$name.out" 2> "$TMP/$name.err"
        if ! check "$name" "$TMP/$name.out" "$exp"; then return; fi
        if [ $have_mon = 1 ]; then
            "$MONRUN" --monitor --strict-align "$TMP/$name.bin" < /dev/null > "$TMP/$name.m.out" 2> "$TMP/$name.err"
            if ! cmp -s "$TMP/$name.m.out" "$exp"; then
                echo "FAIL $name: under the monitor (code after image_end not loaded?)"; diff "$exp" "$TMP/$name.m.out" | head -5; fail=1; return
            fi
            ok="ok m "
        fi
        ie=$(awk 'NF > 2 && $(NF-1) == "__image_end" { print $1; exit }' "$TMP/$name.map")
        for fn in $(grep -o '^[a-z_0-9]*:' "$mod" | tr -d ':'); do
            a=$(awk -v n="$fn" 'NF > 2 && $(NF-1) == n { print $1; exit }' "$TMP/$name.map")
            if [ -z "$a" ] || [ $((0x${a#\$})) -ge $((0x${ie#\$})) ]; then
                echo "FAIL $name: $fn ($a) is not before __image_end ($ie)"; fail=1; return
            fi
        done
        echo "$ok $name"
    else
        cat "$TMP/asm.log"; echo "FAIL $name: build"; fail=1
    fi
}
asmlink_test asmlink examples/universal/asmlink/main.b16 examples/universal/asmlink/util.asm tests/expected/asmlink.out

# Examples for the monitor: built with the monitor interface (../monitor/user/lib, ../monitor/kernel) and
# the header is checked; running them is done by the monitor test bench scenarios.
for src in examples/monitor/*.b16; do
    name=$(basename "$src" .b16)
    [ "$name" = mon ] && continue           # common example functions, not a program
    if "$B16C" -Werror -I examples/monitor -I ../monitor/user/lib -I ../monitor/kernel "$src" -o "$TMP/$name.s" &&
       "$CPU16ASM" --exec -o "$TMP/$name.bin" "$TMP/$name.s" >"$TMP/asm.log" 2>&1; then
        echo "ok b $name"
    else
        cat "$TMP/asm.log"; echo "FAIL $name: build"; fail=1
    fi
done
asmlink_test asmmod tests/asmmod/prog.b16 tests/asmmod/mod.asm tests/expected/asmmod.out

# The old way: the module after the compiler output lies past __bss_end, --exec must reject this.
# A source without an include asm line - otherwise the module would get into the image twice.
name=asmmod_after
grep -v '^include asm' tests/asmmod/prog.b16 > "$TMP/$name.b16"
"$B16C" -I lib "$TMP/$name.b16" -o "$TMP/$name.s" 2>/dev/null
printf '        .include "%s"\n        .include "%s/tests/asmmod/mod.asm"\n' "$TMP/$name.s" "$PWD" > "$TMP/$name.top.asm"
if "$CPU16ASM" --exec -o "$TMP/$name.bin" "$TMP/$name.top.asm" > "$TMP/asm.log" 2>&1; then
    echo "FAIL $name: --exec accepted a module after image_end"; fail=1
elif grep -q "above image_end" "$TMP/asm.log"; then
    echo "ok   $name"
else
    cat "$TMP/asm.log"; echo "FAIL $name: no image_end message"; fail=1
fi

# Stop in __halt with a pending timer request: the program enables the timer, with IF = 0 (and with IF = 1)
# spins for varying times (the request may hang at any moment) and returns from main. Without STI/CLI
# in __halt the WFI wakes up on the request again and again, and the emulator reaches the instruction limit.
halt_ok=1
for di in "di()" ""; do
    for outer in 0 3 40 100 130 200; do
        sed "s/@OUTER@/$outer/; s/@DI@/$di/" tests/halt.tmpl > "$TMP/halt.b16"
        if "$B16C" "$TMP/halt.b16" -o "$TMP/halt.s" 2>/dev/null &&
           "$CPU16ASM" --exec -o "$TMP/halt.bin" "$TMP/halt.s" >/dev/null 2>&1; then
            "$CPU16" -n 200000000 "$TMP/halt.bin" < /dev/null > "$TMP/halt.out" 2> "$TMP/halt.err"
            if [ "$(cat "$TMP/halt.out")" != "done" ] || grep -q "limit reached" "$TMP/halt.err"; then
                echo "FAIL halt: did not stop (IF: '$di', passes $outer)"; halt_ok=0; fail=1
            fi
        else
            echo "FAIL halt: build"; halt_ok=0; fail=1
        fi
    done
done
[ $halt_ok = 1 ] && echo "ok   halt"

# Target kernel: a module without startup code, code from $1000, data from $8000 (copied by __init_data),
# an assembler prologue (tests/kernel/boot.asm) calls the exported function.
name=kernel
exp=tests/expected/$name.out
if "$B16C" -I lib tests/kernel/kmod.b16 -o tests/kernel/kmod.s &&
   "$CPU16ASM" -o "$TMP/$name.bin" -m "$TMP/$name.map" tests/kernel/boot.asm >"$TMP/asm.log" 2>&1; then
    if [ $have_mon = 1 ]; then "$MONRUN" --garbage --strict-align "$TMP/$name.bin" > "$TMP/$name.out" 2> "$TMP/$name.err"
    else "$CPU16" --strict-align "$TMP/$name.bin" > "$TMP/$name.out" 2> "$TMP/$name.err"; fi
    rm -f tests/kernel/kmod.s
    if grep -q ' kmod_unused ' "$TMP/$name.map"; then echo "FAIL $name: an unreachable function in the image"; fail=1
    else check "$name" "$TMP/$name.out" "$exp" && echo "ok   $name"; fi
else
    cat "$TMP/asm.log"; echo "FAIL $name: build"; fail=1
fi

# Diagnostics: the compiler messages are compared with tests/expected/diag/*.err
for src in tests/diag/*.b16; do
    name=$(basename "$src" .b16)
    flags=$(sed -n 's|^// b16c-flags: *||p' "$src" | head -1)
    "$B16C" $flags -I lib "$src" -o "$TMP/diag.s" 2> "$TMP/$name.err" >/dev/null
    echo "rc $?" >> "$TMP/$name.err"
    check "diag/$name" "$TMP/$name.err" "tests/expected/diag/$name.err" && echo "ok   diag/$name"
done

rm -rf "$TMP"
exit $fail
