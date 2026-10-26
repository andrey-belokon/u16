#!/usr/bin/env python3
# stdlib_test.py - tests of stdlib.asm: the programs are assembled by cpu16asm and executed in cpu16,
# the output is compared with the expected one (for 32-bit math the expected value is computed by Python).
#
# Run: python3 stdlib_test.py path/to/cpu16asm path/to/cpu16

import os
import random
import shutil
import subprocess
import sys
import tempfile

ASM, EMU = sys.argv[1], sys.argv[2]
HERE = os.path.dirname(os.path.abspath(__file__))
LIB = os.path.join(os.path.dirname(HERE), "lib", "stdlib.asm")
passed = failed = 0


def check(cond, what, detail=""):
    global passed, failed
    if cond:
        passed += 1
    else:
        failed += 1
        print("FAIL:", what, detail)


def run(body, stdin=b"", steps=5_000_000, data="", screen=False, args=()):
    """Assemble a program with the library and run it; return the output (bytes).
    screen=True - return the screen contents at the end (--screen) instead of the output;
    args - additional emulator keys (for example, --rtc)."""
    src = ("        .org    0\n        LD      SP, #$FF00\n" + body +
           "        WFI\n" + data + '        .include "stdlib.asm"\n')
    with tempfile.TemporaryDirectory() as tmp:
        shutil.copy(LIB, tmp)
        with open(os.path.join(tmp, "t.asm"), "w") as f:
            f.write(src)
        r = subprocess.run([ASM, "t.asm", "-o", "t.bin"], cwd=tmp, capture_output=True, text=True)
        if r.returncode:
            raise SystemExit("assembly failed:\n" + r.stderr + "\n" + src[:2000])
        r = subprocess.run([EMU, "-n", str(steps)] + (["--screen"] if screen else []) + list(args) + ["t.bin"],
                           cwd=tmp, input=stdin, capture_output=True)
        return r.stdout


def imm(v):
    return f"#${v & 0xFFFF:04X}"


def ld32(rhi, rlo, v):
    return f"        LD      {rlo}, {imm(v)}\n        LD      {rhi}, {imm(v >> 16)}\n"


# ---------------------------------------------------------------- output
out = run('''        LD      R0, #'A'
        CALL    putc
        CALL    newline
        LD      R0, #msg
        CALL    puts
        LD      R0, #empty
        CALL    puts
''', data='msg:    .asciz  "Hello, stdlib!\\n"\nempty:  .asciz  ""\n')
check(out == b"A\nHello, stdlib!\n", "putc, newline, puts", repr(out))

cases = [0, 1, 7, 9, 10, 99, 100, 255, 256, 4095, 32767, 32768, 40000, 65534, 65535]
body = "".join(f"        LD      R0, {imm(v)}\n        CALL    print_u16\n        CALL    newline\n" for v in cases)
out = run(body)
check(out == "".join(f"{v}\n" for v in cases).encode(), "print_u16", repr(out[:200]))

scases = [0, 1, -1, 127, -128, 32767, -32768, -12345, 12345]
body = "".join(f"        LD      R0, {imm(v)}\n        CALL    print_s16\n        CALL    newline\n" for v in scases)
out = run(body)
check(out == "".join(f"{v}\n" for v in scases).encode(), "print_s16", repr(out))

hcases = [0, 0x000F, 0x00A0, 0x1234, 0xBEEF, 0xFFFF]
body = "".join(f"        LD      R0, {imm(v)}\n        CALL    print_hex16\n        LD      R0, {imm(v)}\n"
               f"        CALL    print_hex8\n        CALL    newline\n" for v in hcases)
out = run(body)
check(out == "".join(f"{v:04X}{v & 0xFF:02X}\n" for v in hcases).encode(), "print_hex16, print_hex8", repr(out))

# ---------------------------------------------------------------- 32-bit math
rnd = random.Random(1234)
edge = [0, 1, 2, 9, 10, 0xFFFF, 0x10000, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFE, 0xFFFFFFFF, 4294967295, 1234567890]


def rand32():
    k = rnd.randrange(6)
    if k == 0:
        return rnd.choice(edge)
    if k == 1:
        return rnd.randrange(0x10000)
    return rnd.randrange(1 << 32)


def math_test(name, n, make, expect):
    body, exp = "", ""
    for _ in range(n):
        b, e = make(), None
        body += b[0]
        exp += b[1]
    out = run(body, steps=50_000_000)
    check(out == exp.encode(), name, f"first mismatch near: {out[:120]!r} vs {exp[:120]!r}" if out != exp.encode() else "")


def pu32():   # print R1:R0 and a newline, without touching R4
    return "        CALL    print_u32\n        CALL    newline\n"


def mk_add():
    a, b = rand32(), rand32()
    return (ld32("R1", "R0", a) + ld32("R3", "R2", b) + "        CALL    add32\n" + pu32(), f"{(a + b) & 0xFFFFFFFF}\n")


def mk_sub():
    a, b = rand32(), rand32()
    return (ld32("R1", "R0", a) + ld32("R3", "R2", b) + "        CALL    sub32\n        LD      R4, FLAGS\n" + pu32() +
            "        LD      R0, R4\n        AND     R0, #2\n        CALL    print_u16\n        CALL    newline\n",
            f"{(a - b) & 0xFFFFFFFF}\n{2 if a < b else 0}\n")


def mk_neg():
    a = rand32()
    return (ld32("R1", "R0", a) + "        CALL    neg32\n" + pu32(), f"{(-a) & 0xFFFFFFFF}\n")


def mk_cmp():
    a = rand32()
    b = a if rnd.randrange(4) == 0 else rand32()
    flags = (1 if a == b else 0) | (2 if a < b else 0)
    return (ld32("R1", "R0", a) + ld32("R3", "R2", b) + "        CALL    cmp32u\n        LD      R0, FLAGS\n"
            "        AND     R0, #3\n        CALL    print_u16\n        CALL    newline\n", f"{flags}\n")


def mk_mulu16():
    a, b = rnd.randrange(0x10000), rnd.choice([rnd.randrange(0x10000), 0, 1, 0xFFFF])
    return (f"        LD      R0, {imm(a)}\n        LD      R1, {imm(b)}\n        CALL    mulu16\n" + pu32(), f"{a * b}\n")


def mk_mul32():
    a, b = rand32(), rand32()
    return (ld32("R1", "R0", a) + ld32("R3", "R2", b) + "        CALL    mul32\n" + pu32(), f"{(a * b) & 0xFFFFFFFF}\n")


def mk_div():
    a = rand32()
    d = rnd.choice([rnd.randrange(1, 0x10000), 1, 10, 0xFFFF, 0x8000, 0x8001, 0])
    if d == 0:
        exp = f"{0xFFFFFFFF}\n{a & 0xFFFF}\n16\n"
    else:
        exp = f"{a // d}\n{a % d}\n0\n"
    return (ld32("R1", "R0", a) + f"        LD      R2, {imm(d)}\n        CALL    divu32\n        LD      R4, FLAGS\n"
            "        PUSH    R2\n" + pu32() + "        POP     R0\n        CALL    print_u16\n        CALL    newline\n"
            "        LD      R0, R4\n        AND     R0, #16\n        CALL    print_u16\n        CALL    newline\n", exp)


def mk_s32():
    a = rand32()
    s = a - (1 << 32) if a & 0x80000000 else a
    return (ld32("R1", "R0", a) + "        CALL    print_s32\n        CALL    newline\n", f"{s}\n")


def mk_hex32():
    a = rand32()
    return (ld32("R1", "R0", a) + "        CALL    print_hex32\n        CALL    newline\n", f"{a:08X}\n")


for name, mk in [("add32", mk_add), ("sub32 with borrow", mk_sub), ("neg32", mk_neg), ("cmp32u flags", mk_cmp),
                 ("mulu16", mk_mulu16), ("mul32", mk_mul32), ("divu32 quotient, remainder, OF", mk_div),
                 ("print_s32", mk_s32), ("print_hex32", mk_hex32)]:
    math_test(name, 150, mk, None)

# preservation of R4..R11 by the library
out = run(''.join(f"        LD      R{k}, #{1000 + k}\n" for k in range(4, 12)) + '''        LD      R0, #$1234
        LD      R1, #$5678
        CALL    print_u32
        LD      R2, #7
        CALL    divu32
        CALL    mul32
        CALL    print_hex32
''' + ''.join(f"        LD      R0, R{k}\n        CALL    print_u16\n" for k in range(4, 12)))
check(out.endswith("".join(str(1000 + k) for k in range(4, 12)).encode()), "library preserves R4..R11", repr(out[-60:]))

# ---------------------------------------------------------------- memory and strings
out = run('''        LD      R0, #buf
        LD      R1, #'x'
        LD      R2, #5
        CALL    memset
        LD      R0, #buf+1
        LD      R1, #src
        LD      R2, #3
        CALL    memcpy
        LD      R0, #buf
        CALL    puts
        CALL    newline
        LD      R0, #buf
        CALL    strlen
        CALL    print_u16
        LD      R0, #empty
        CALL    strlen
        CALL    print_u16
        LD      R0, #buf
        LD      R1, #'z'
        LD      R2, #0
        CALL    memset          ; zero length: nothing changes
        LD      R0, #buf
        CALL    puts
''', data='src:    .ascii  "abc"\nempty:  .byte   0\nbuf:    .space  8\n')
check(out == b"xabcx\n50xabcx", "memset, memcpy, strlen", repr(out))

# ---------------------------------------------------------------- input
out = run('''        CALL    getc
        CALL    putc
        CALL    getc_nb
        CALL    putc
        CALL    getc_nb
        JNZ     @bad
        LD      R0, #'.'
        CALL    putc
@bad:
''', stdin=b"QR")
check(out == b"QR.", "getc, getc_nb (0 when empty)", repr(out))

read_body = "".join('''        LD      R0, #0
        CALL    read_dec
        LD      R4, R1
        CALL    print_s16
        LD      R0, #'/'
        CALL    putc
        LD      R0, R4
        CALL    print_u16
        CALL    newline
''' for _ in range(14))
inp = b"123\n  -45 x\n65535\n65536\n-32768\n-32769\n\n7-\n--3\n0\n"
out = run(read_body, stdin=inp)
# "-45 " ends with a blank, so "x" and "\n" after it give two errors;
# 65536 is an overflow; -32769 is an error (the value 32769 prints as -32767);
# "7-" is the number 7, the sign is taken; "--3" is a second minus: an error, then 3
exp = (b"123/0\n-45/0\n0/1\n0/1\n-1/0\n0/1\n-32768/0\n-32767/1\n0/1\n7/0\n"
       b"0/1\n0/1\n3/0\n0/0\n")
check(out == exp, "read_dec values, errors and overflow", repr(out))

out = run('''        LD      R0, #1
        CALL    read_dec
''', stdin=b"  42\n")
check(out == b"  42\n", "read_dec echo", repr(out))

gets_body = '''        LD      R0, #buf
        LD      R1, #8
        LD      R2, #0
        CALL    gets
        CALL    print_u16
        LD      R0, #':'
        CALL    putc
        LD      R0, #buf
        CALL    puts
        CALL    newline
'''
out = run(gets_body * 4, stdin=b"hello\nabcdefghij\nab\x08c\r\n\x08\x08x\n", data="buf:    .space  8\n")
check(out == b"5:hello\n7:abcdefg\n2:ac\n1:x\n", "gets: length, truncation, backspace, CR", repr(out))
out = run('''        LD      R0, #buf
        LD      R1, #8
        LD      R2, #1
        CALL    gets
''', stdin=b"ab\x08c\n", data="buf:    .space  8\n")
check(out == b"ab\x08 \x08c\n", "gets echo with backspace", repr(out))

# ---------------------------------------------------------------- display, keyboard, timer
out = run('''        LD      R0, #0
        CALL    scr_mode                ; coordinate mode
        LD      R0, #BLUE << 3 | WHITE
        CALL    scr_attr
        CALL    scr_clear
        LD      R0, #2
        LD      R1, #1
        LD      R2, #8
        LD      R3, #4
        CALL    scr_box
        LD      R0, #4
        LD      R1, #2
        LD      R2, #greet
        CALL    scr_print_at
        LD      R0, #1
        LD      R1, #6
        CALL    scr_goto
        LD      R0, #'Ж'
        CALL    putc
        LD      R0, #4
        LD      R1, #2
        CALL    scr_goto
        LD      R0, #RED
        LD      R1, #2
        CALL    scr_paint               ; recolor "Да" and check that the text is intact
        LD      R0, #'!'
        CALL    putc
        LD      R0, #MODE_TTY
        CALL    scr_mode
        LDZX    R0, [CON_COLS]
        CALL    print_u16               ; teletype: "80" at the position after "!"
''', data='greet:  .asciz  "Да"\n', screen=True)
lines = out.decode("utf-8").split("\n")
# stdout: "80" from the teletype, then a screen dump (row 0 is empty)
check(lines[0] == "80" and lines[1:7] == ["  ╔══════╗", "  ║ Да!80║", "  ║      ║", "  ╚══════╝", "", " Ж"],
      "scr_box, scr_print_at, scr_goto, scr_paint keeps text",
      repr(lines[:8]))
out = run('''        CALL    getkey
        LD      R4, R0
        CALL    getkey_nb
        JNZ     @bad
        LD      R0, R4
        CALL    print_hex16
        CALL    timer_on
        LDZX    R0, [TIMER]
        CALL    print_u16
        CALL    timer_off
        LDZX    R0, [TIMER]
        CALL    print_u16
@bad:
''', stdin=b"\x1b[A")
check(out == b"480010", "getkey returns the scancode of a terminal arrow key; timer_on/off", repr(out))

# ---------------------------------------------------------------- interrupt vectors
# vectors_init: vector 8 is the ERRCMD handler, the other 15 are irq_ignore; a TRAP on a free vector
# returns without changing registers and flags; TRAP #8 (the same vector as ERRCMD) goes to the handler
out = run('''        LD      R0, #fault
        CALL    vectors_init
        LD      R0, #fault
        CALL    print_hex16
        LD      R0, #' '
        CALL    putc
        LD      R0, #irq_ignore
        CALL    print_hex16
        CALL    newline
        LD      R4, #$FF00
        LD      R5, #16
@v:     LD      R0, [R4++]
        CALL    print_hex16
        LD      R0, #' '
        CALL    putc
        DEC     R5
        JNZ     @v
        CALL    newline
        LD      R6, #1234
        CMP     R6, #1234               ; ZF = 1
        TRAP    #9
        JNZ     @bad
        TRAP    #15
        TRAP    #0                      ; IRQ0 without a timer in the program - also a stub
        CMP     R6, #1234
        JNZ     @bad
        LD      R0, #'T'
        CALL    putc
        CALL    newline
        TRAP    #8
@bad:   LD      R0, #'?'
        CALL    putc
''', data='''fault:  LD      R0, #'F'
        CALL    putc
        CALL    newline
        CLI
@h:     WFI
        JMP     @h
''')
lines = out.decode().split("\n")
ok = len(lines) >= 4
if ok:
    f, ig = lines[0].split()
    vec = lines[1].split()
    ok = vec == [ig] * 8 + [f] + [ig] * 7 and lines[2] == "T" and lines[3] == "F"
check(ok, "vectors_init: ERRCMD - handler, other vectors - irq_ignore; TRAP to a free vector returns", repr(out))

# a real interrupt without a working handler (the timer, IRQ0): irq_ignore returns,
# the program continues; three ticks wake WFI
out = run('''        LD      R0, #fault
        CALL    vectors_init
        CALL    timer_on
        STI
        LD      R4, #3
@w:     WFI
        DEC     R4
        JNZ     @w
        CALL    timer_off
        CLI
        LD      R0, #'W'
        CALL    putc
        CALL    newline
''', data='''fault:  LD      R0, #'F'
        CALL    putc
        CLI
@h:     WFI
        JMP     @h
''')
check(out == b"W\n", "an IRQ without a working handler goes to irq_ignore and the program goes on", repr(out))

# ---------------------------------------------------------------- real-time clock
def rtc_words(y, mo, d, h, mi, se):
    """Date and time (24-hour notation) -> (TIME, DATE) in the FS16 timestamp format."""
    return (h % 12) << 12 | mi << 6 | se, (y - 2025) << 10 | mo << 6 | d << 1 | (h >= 12)


# print_date and print_time: midnight, noon, end of day, one-digit fields, range edges
body = ""
cases = [(2025, 1, 1, 0, 0, 0), (2026, 10, 11, 12, 0, 0), (2028, 2, 29, 23, 59, 59), (2031, 7, 4, 1, 2, 3),
         (2088, 12, 31, 11, 59, 59), (2040, 6, 9, 13, 30, 5)]
for c in cases:
    t, d = rtc_words(*c)
    body += f"        LD      R0, {imm(t)}\n        LD      R1, {imm(d)}\n        CALL    print_date\n        LD      R0, #' '\n        CALL    putc\n"
    body += f"        LD      R0, {imm(t)}\n        LD      R1, {imm(d)}\n        CALL    print_time\n"
    body += f"        LD      R0, {imm(t)}\n        LD      R1, {imm(d)}\n        CALL    rtc_hour24\n        PUSH    R0\n"
    body += "        LD      R0, #' '\n        CALL    putc\n        POP     R0\n        CALL    print_u16\n        CALL    newline\n"
out = run(body)
want = "".join(f"{d:02}.{mo:02}.{y} {h:02}:{mi:02}:{se:02} {h}\n" for y, mo, d, h, mi, se in cases)
check(out.decode() == want, "print_date, print_time, rtc_hour24", repr(out))

# rtc_get by --rtc; rtc_set: a valid value is set, an invalid one - R0 = 1, the clock unchanged
t, d = rtc_words(2030, 4, 30, 18, 45, 0)
out = run(f'''        CALL    rtc_get
        PUSH    R0
        PUSH    R1
        CALL    print_date
        LD      R0, #' '
        CALL    putc
        POP     R1
        POP     R0
        CALL    print_time
        CALL    newline
        LD      R0, {imm(t)}
        LD      R1, {imm(d)}
        CALL    rtc_set
        JNZ     @bad
        CALL    print_u16
        LD      R0, {imm(t)}
        LD      R1, {imm(rtc_words(2027, 2, 28, 0, 0, 0)[1] + (1 << 1))}   ; 29 February 2027
        CALL    rtc_set
        CALL    print_u16
        CALL    rtc_get
        PUSH    R0
        CALL    newline
        CALL    print_date
        LD      R0, #' '
        CALL    putc
        POP     R0
        SHR     R0, #6
        AND     R0, #63
        CALL    print_u16               ; minute: seconds could have passed, a minute hardly
        LDZX    R0, [RTC_STATUS]
        CALL    print_u16
@bad:
''', args=("--rtc", "11.10.2026T23:59:58"))
check(out.decode() in ("11.10.2026 23:59:58\n01\n30.04.2030 451", "11.10.2026 23:59:59\n01\n30.04.2030 451"),
      "rtc_get after --rtc; rtc_set accepts a valid value and rejects 29.02.2027", repr(out))

print(f"{'FAILED' if failed else 'OK'}: {passed} passed, {failed} failed")
sys.exit(1 if failed else 0)
