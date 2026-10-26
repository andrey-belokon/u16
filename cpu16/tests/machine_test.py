#!/usr/bin/env python3
# machine_test.py - the examples from "CPU16 Machine: Memory and Devices" in the command-line
# emulator, with disk images (the --disk key), and the example examples/disk_demo.asm.
#
#   - the small-disk driver with polling waits (sect. 6.6): writing and reading sectors,
#     the RANGE and NODISK errors, data in the image file;
#   - waiting on IRQ1 without a race (sect. 6.6): the INFO command, the handler acknowledges with ACK;
#   - enabling the memory manager mmu_init (sect. 7.7) and reading the map with four words (sect. 7.5);
#   - the clock (sect. 8.5): rtc_get after the --rtc key, rtc_set with a valid and an invalid value,
#     rtc_hour24; the --rtc key: startup errors;
#   - the --disk key: startup errors; disk_demo.asm: the launch counter grows in the image file;
#   - clock.asm: setting the clock with keys, the day of week.
# The fragments from the document (docs/cpu16_machine_en.md) are embedded here verbatim; the test checks that they match
# the document whose path is given as the third argument.
#
# Run: python3 machine_test.py path/to/cpu16asm path/to/cpu16 [cpu16_machine_en.md]

import os
import shutil
import subprocess
import sys
import tempfile

ASM, EMU = sys.argv[1], sys.argv[2]
DOC = sys.argv[3] if len(sys.argv) > 3 else ""
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
LIB = os.path.join(ROOT, "lib", "stdlib.asm")
passed = failed = 0


def check(cond, what, detail=""):
    global passed, failed
    if cond:
        passed += 1
    else:
        failed += 1
        print("FAIL:", what, detail)


# ---- fragments of the document (sect. 6.6, 7.5, 7.7, 8.5)
RTC_GETSET = '        .equ    RTC_TIME,       $FF2A\n        .equ    RTC_DATE,       $FF2C\n        .equ    RTC_CMD,        $FF2E\n        .equ    RTC_STATUS,     $FF2E\n        .equ    RTC_READ,       1\n        .equ    RTC_WRITE,      2\n        .equ    RTC_WRITE_TIME, 3\n        .equ    RTC_WRITE_DATE, 4\n        .equ    RTC_ERR,        $01\n\n; Output: R0 = time, R1 = date.\nrtc_get:\n        PUSH    FLAGS                   ; critical section: READ and register reads\n        CLI                             ;   without a handler running between them\n        LD      R0, #RTC_READ\n        STB     [RTC_CMD], R0           ; clock state -> TIME, DATE\n        LD      R0, [RTC_TIME]\n        LD      R1, [RTC_DATE]\n        POP     FLAGS\n        RET\n\n; Input: R0 = time, R1 = date.\n; Output: R0 = 0 and ZF = 1: clock updated;\n;         R0 = 1: invalid value; clock left unchanged.\nrtc_set:\n        PUSH    FLAGS\n        CLI\n        ST      [RTC_TIME], R0\n        ST      [RTC_DATE], R1\n        LD      R0, #RTC_WRITE\n        STB     [RTC_CMD], R0\n        LDZX    R0, [RTC_STATUS]        ; bit 0 = ERR; all other bits 0\n        POP     FLAGS\n        CMP     R0, #0\n        RET\n'
RTC_HOUR24 = '; Input: R0 = time, R1 = date. Output: R0 = hour 0..23.\nrtc_hour24:\n        SHR     R0, #12                 ; hour 0..11\n        TEST    R1, #1                  ; PM?\n        JZ      @am\n        ADD     R0, #12\n@am:    RET\n'
DRIVER = '        .equ    DK_CMD,    $FF30\n        .equ    DK_STATUS, $FF30\n        .equ    DK_ERROR,  $FF31\n        .equ    DK_DISK,   $FF32\n        .equ    DK_COUNT,  $FF33\n        .equ    DK_ADDR,   $FF34\n        .equ    DK_LBA_LO, $FF36\n        .equ    DK_LBA_HI, $FF38\n        .equ    DK_CTRL,   $FF3A\n        .equ    DK_DRIVES, $FF3B\n        .equ    DK_ACK,    0\n        .equ    DK_READ,   1\n        .equ    DK_WRITE,  2\n        .equ    DK_INFO,   3\n        .equ    DK_FLUSH,  4\n        .equ    DK_BUSY,   $01\n\n; Input: R0 = disk, R1 = LBA, R2 = buffer address, R3 = sector count\n; Output: R0 = error code, ZF = 1 on success.\n; R4 is used and preserved on the stack.\ndisk_read:\n        PUSH    R4\n        LD      R4, #DK_READ\n        JMP     disk_io\ndisk_write:\n        PUSH    R4\n        LD      R4, #DK_WRITE\ndisk_io:                                ; R4 = command; saved R4 on the stack\n        STB     [DK_DISK], R0\n        STB     [DK_COUNT], R3\n        ST      [DK_ADDR], R2\n        ST      [DK_LBA_LO], R1\n        LD      R0, #0\n        ST      [DK_LBA_HI], R0          ; small disk: high word is zero\n        STB     [DK_CMD], R4\n@wait:  LDZX    R0, [DK_STATUS]\n        TEST    R0, #DK_BUSY\n        JNZ     @wait\n        LDZX    R0, [DK_ERROR]\n        POP     R4\n        CMP     R0, #0\n        RET\n'
IRQ_WAIT = '; Input: R1 = command code. Clobbers R0.\ndisk_start_wait:\n        CLI\n        LD      R0, #0\n        STB     [dk_done], R0\n        STB     [DK_CMD], R1            ; start command\n@wait:  LDZX    R0, [dk_done]\n        CMP     R0, #0\n        JNZ     @done\n        WFI                             ; IF = 0: wake on request without entering handler\n        STI                             ; request accepted immediately after STI\n        CLI\n        JMP     @wait\n@done:  STI\n        RET\n\ndisk_irq:                               ; IRQ1 vector: ST [$FF00 + 2*1], ...\n        PUSH    R0\n        LD      R0, #DK_ACK\n        STB     [DK_CMD], R0            ; deassert request\n        LD      R0, #1\n        STB     [dk_done], R0\n        POP     R0\n        IRET\n\ndk_done: .byte  0\n'
MAP_SAVE = "        .equ    MMU_PAGE, $FF40\n\n        LD      R0, [MMU_PAGE+0]         ; PAGE0, PAGE1\n        PUSH    R0\n        LD      R0, [MMU_PAGE+2]         ; PAGE2, PAGE3\n        PUSH    R0\n        LD      R0, [MMU_PAGE+4]         ; PAGE4, PAGE5\n        PUSH    R0\n        LD      R0, [MMU_PAGE+6]         ; PAGE6, PAGE7\n        PUSH    R0\n"
MMU_INIT = '        .equ    MMU_PAGE, $FF40\n        .equ    MMU_CTRL, $FF48\n        .equ    MMU_EN,   $01\n\n; Clobbers R0.\nmmu_init:\n        LD      R0, #9 << 8 | 8          ; PAGE0 = 8, PAGE1 = 9\n        ST      [MMU_PAGE+0], R0\n        LD      R0, #11 << 8 | 10        ; PAGE2 = 10, PAGE3 = 11\n        ST      [MMU_PAGE+2], R0\n        LD      R0, #5 << 8 | 4          ; PAGE4 = 4 (bootloader code), PAGE5 = 5\n        ST      [MMU_PAGE+4], R0\n        LD      R0, #7 << 8 | 6          ; PAGE6 = 6, PAGE7 = 7 (vectors, stack)\n        ST      [MMU_PAGE+6], R0\n        LD      R0, #MMU_EN\n        STB     [MMU_CTRL], R0           ; MMU enabled\n        RET                              ; taken branch: synchronization point\n'


def build(tmp, name, src):
    path = os.path.join(tmp, name + ".asm")
    with open(path, "w", encoding="utf-8") as f:
        f.write(src)
    r = subprocess.run([ASM, path, "-o", os.path.join(tmp, name + ".bin")], capture_output=True, text=True)
    check(r.returncode == 0, f"{name}: assembles", r.stderr)
    return os.path.join(tmp, name + ".bin")


def run(binary, *disks, stdin="", rtc=None):
    args = [EMU] + (["--rtc", rtc] if rtc else [])
    for d in disks:
        args += ["--disk", d]
    r = subprocess.run(args + [binary], input=stdin, capture_output=True, text=True, timeout=30)
    return r.returncode, r.stdout, r.stderr


with tempfile.TemporaryDirectory() as tmp:
    shutil.copy(LIB, tmp)
    img = os.path.join(tmp, "data.img")
    with open(img, "wb") as f:
        f.write(bytes(64 * 512))

    # ---------------- a driver with polling
    prog = """        .org    0
        LD      SP, #$FF00
        LD      R0, #buf                ; pattern: byte i = i mod 251
        LD      R1, #0
@fill:  LD      R2, R1
        REMU    R2, #251
        STB     [R0++], R2
        INC     R1
        CMP     R1, #1024
        JB      @fill
        LD      R0, #1                  ; disk 1, sectors 3..4
        LD      R1, #3
        LD      R2, #buf
        LD      R3, #2
        CALL    disk_write
        CALL    report
        LD      R0, #1
        LD      R1, #3
        LD      R2, #buf2
        LD      R3, #2
        CALL    disk_read
        CALL    report
        LD      R0, #buf                ; compare the buffers
        LD      R1, #buf2
        LD      R4, #1024
@cmp:   LDZX    R2, [R0++]
        LDZX    R3, [R1++]
        CMP     R2, R3
        JNZ     @diff
        DEC     R4
        JNZ     @cmp
        LD      R0, #msg_same
        CALL    puts
        JMP     @errs
@diff:  LD      R0, #msg_diff
        CALL    puts
@errs:  LD      R0, #1                  ; past the end of the disk: RANGE
        LD      R1, #63
        LD      R2, #buf2
        LD      R3, #2
        CALL    disk_read
        CALL    report
        LD      R0, #5                  ; no such disk: NODISK
        LD      R1, #0
        LD      R2, #buf2
        LD      R3, #1
        CALL    disk_read
        CALL    report
        WFI
; report - print the error code and ZF
report: PUSH    LR
        PUSH    R4
        LD      R4, FLAGS
        CALL    print_u16
        LD      R0, #' '
        CALL    putc
        LD      R0, R4
        AND     R0, #1
        CALL    print_u16
        CALL    newline
        POP     R4
        POP     IP
msg_same: .asciz "same\\n"
msg_diff: .asciz "different\\n"
""" + DRIVER + """        .include "stdlib.asm"
        .org    $4000
buf:    .space  1024
buf2:   .space  1024
"""
    b = build(tmp, "driver", prog)
    code, out, err = run(b, "1=" + img)
    check(code == 0 and out == "0 1\n0 1\nsame\n3 0\n1 0\n", "6.6 polling driver: write, read back, RANGE, NODISK", repr(out) + err)
    data = open(img, "rb").read()
    check(data[3 * 512:5 * 512] == bytes(i % 251 for i in range(1024)) and data[:3 * 512] == bytes(3 * 512),
          "6.6 polling driver: sectors 3..4 are in the image file")

    # ---------------- waiting on IRQ1
    prog = """        .org    0
        LD      SP, #$FF00
        LD      R0, #disk_irq
        ST      [$FF00 + 2*1], R0       ; IRQ1 vector
        LD      R0, #1
        STB     [DK_CTRL], R0           ; IEN = 1
        LD      R0, #1
        STB     [DK_DISK], R0
        LD      R0, #info
        ST      [DK_ADDR], R0
        LD      R1, #DK_INFO
        CALL    disk_start_wait
        LD      R0, [info]
        CALL    print_u16
        CALL    newline
        LDZX    R0, [DK_STATUS]         ; DONE is cleared by the ACK command
        CALL    print_hex8
        CALL    newline
        LDZX    R0, [dk_done]
        CALL    print_u16
        CALL    newline
        LD      R0, #info + 16
        CALL    puts
        CALL    newline
        CLI
        WFI
""" + DRIVER + IRQ_WAIT + """        .include "stdlib.asm"
        .org    $4000
info:   .space  32
"""
    b = build(tmp, "irqwait", prog)
    code, out, err = run(b, "1=" + img + ":ro")
    check(code == 0 and out == "64\n18\n1\ndata.img\n", "6.6 IRQ1 wait without a race: INFO, handler ACK, label", repr(out) + err)

    # ---------------- the memory manager: mmu_init and reading the map
    map_save = MAP_SAVE.replace("        .equ    MMU_PAGE, $FF40\n\n", "")   # MMU_PAGE is already defined in MMU_INIT
    prog = """        .org    0
        JMP     #boot                   ; the loader code is in page 4, as in the example of sect. 7.7
        .org    $8000
boot:   LD      SP, #$FF00
        CALL    mmu_init
        LD      R0, #$1234
        ST      [$0000], R0             ; page 0 -> frame 8
        LD      R0, #0 << 8 | 8         ; PAGE0 = 8, PAGE1 = 0: frame 0 (the former ROM) in page 1
        ST      [MMU_PAGE+0], R0
        LD      R0, [$0000]
        CALL    print_hex16
        CALL    newline
        LD      R0, [$2000]             ; the word at physical $0000: JMP #boot
        CALL    print_hex16
        CALL    newline
""" + map_save + """        LD      R4, #4
@pop:   POP     R0
        CALL    print_hex16
        CALL    newline
        DEC     R4
        JNZ     @pop
        LD      R0, #0
        STB     [MMU_CTRL], R0          ; switching off: physical address = logical
        LD      R0, [$0000]
        CALL    print_hex16
        CALL    newline
        WFI
""" + MMU_INIT + """        .include "stdlib.asm"
"""
    b = build(tmp, "mmu", prog)
    code, out, err = run(b)
    check(code == 0 and out == "1234\nC873\n0706\n0504\n0B0A\n0008\nC873\n", "7.7 mmu_init and 7.5 reading the map", repr(out) + err)

    # ---------------- the --disk key: startup errors
    bad = os.path.join(tmp, "bad.img")
    with open(bad, "wb") as f:
        f.write(b"x" * 1000)
    code, out, err = run(b, "0=" + bad)
    check(code != 0 and "not a multiple of 512" in err, "--disk: size not a multiple of 512 is a start-up error", err)
    code, out, err = run(b, "8=" + img)
    check(code != 0 and "N = 0..7" in err, "--disk: disk number 0..7", err)
    code, out, err = run(b, "0=" + os.path.join(tmp, "missing.img"))
    check(code != 0 and "cannot open" in err, "--disk: missing file", err)

    # ---------------- the clock: rtc_get, rtc_set, rtc_hour24 (sect. 8.5); without the library - it has the same names
    def rtc_words(y, mo, d, h, mi, se):
        return (h % 12) << 12 | mi << 6 | se, (y - 2025) << 10 | mo << 6 | d << 1 | (h >= 12)
    t_set, d_set = rtc_words(2030, 4, 30, 18, 45, 0)
    d_bad = rtc_words(2027, 2, 28, 0, 0, 0)[1] + (1 << 1)          # 29 February 2027
    prog = f"""        .org    0
        LD      SP, #$FF00
        CALL    rtc_get
        CALL    pair
        LD      R0, #${t_set:04X}
        LD      R1, #${d_set:04X}
        CALL    rtc_set
        LD      R2, FLAGS
        CALL    hex
        LD      R0, R2
        AND     R0, #1                  ; ZF
        CALL    hex
        LD      R0, #${t_set:04X}
        LD      R1, #${d_bad:04X}
        CALL    rtc_set
        CALL    hex
        CALL    rtc_get
        LD      R4, R0
        LD      R0, R1
        CALL    hex                     ; date: the previous setting
        LD      R0, R4
        CALL    rtc_hour24
        CALL    hex
        WFI
; pair - print R0 and R1; hex - print R0 (4 digits and a space). They clobber R0, R1, R3, R5.
pair:   PUSH    LR
        PUSH    R1
        CALL    hex
        POP     R0
        CALL    hex
        POP     IP
hex:    LD      R5, #4
@d:     ROR     R0, #12
        LD      R3, R0
        AND     R3, #15
        CMP     R3, #10
        JB      @n
        ADD     R3, #'A' - '0' - 10
@n:     ADD     R3, #'0'
        STB     [$FF20], R3
        DEC     R5
        JNZ     @d
        LD      R3, #' '
        STB     [$FF20], R3
        RET
""" + RTC_GETSET + RTC_HOUR24
    b = build(tmp, "rtc", prog)
    code, out, err = run(b, rtc="11.10.2026T23:59:58")
    t0, d0 = rtc_words(2026, 10, 11, 23, 59, 58)
    want = f"{d_set:04X} 0012 "
    check(code == 0 and out in (f"{t0:04X} {d0:04X} 0000 0001 0001 " + want, f"{t0 + 1:04X} {d0:04X} 0000 0001 0001 " + want),
          "8.5 rtc_get after --rtc, rtc_set valid and invalid, rtc_hour24", repr(out) + err)
    code, out, err = run(b)
    check(code == 0 and out.endswith(want), "8.5 without --rtc: the clock starts at host time and can be set", repr(out) + err)

    # ---------------- the --rtc key: startup errors
    for bad in ("31.12.2024T23:59:59", "01.01.2089T00:00:00", "30.02.2026T00:00:00", "29.02.2027T00:00:00",
                "01.01.2026T24:00:00", "01.01.2026 00:00:00", "2026-01-01T00:00:00", "1.1.2026T00:00:00", "now"):
        code, out, err = run(b, rtc=bad)
        check(code != 0 and "--rtc" in err and out == "", f"--rtc {bad}: start-up error", err)
    code, out, err = run(b, rtc="31.12.2088T23:59:59")
    check(code == 0, "--rtc: the last second of 2088 is accepted", err)

    # ---------------- examples/disk_demo.asm
    src = open(os.path.join(ROOT, "examples", "disk_demo.asm"), encoding="utf-8").read().replace('"../lib/stdlib.asm"', '"stdlib.asm"')
    b = build(tmp, "disk_demo", src)
    demo = os.path.join(tmp, "demo.img")
    with open(demo, "wb") as f:
        f.write(bytes(64 * 512))
    outs = [run(b, "0=" + demo, "3=" + img + ":ro")[1] for _ in range(2)]
    check("disk 0: 64 sectors, small, label \"demo.img\"" in outs[0] and "disk 3: 64 sectors, read-only, small" in outs[0]
          and outs[0].endswith("runs counted on this disk: 1\n") and outs[1].endswith("runs counted on this disk: 2\n")
          and open(demo, "rb").read(2) == b"\x02\x00", "disk_demo.asm: INFO for each disk, run counter in the image", repr(outs))
    code, out, err = run(b)
    check(out.startswith("No disks."), "disk_demo.asm without disks", repr(out))

    # ---------------- examples/clock.asm: the setting keys, the day of week on the screen
    src = open(os.path.join(ROOT, "examples", "clock.asm"), encoding="utf-8").read().replace('"../lib/stdlib.asm"', '"stdlib.asm"')
    b = build(tmp, "clock", src)
    for start, want in (("31.10.2026T23:58:30", "01.11.2026 00:59:00"), ("31.12.2026T10:00:00", "01.01.2027 11:01:00"),
                        ("31.12.2088T12:30:59", "01.01.2025 13:31:00"), ("28.02.2028T06:00:00", "29.02.2028 07:01:00")):
        code, out, err = run(b, stdin="hm0d\x1b", rtc=start)
        check(code == 0 and out.endswith("Clock: " + want + "\n"), f"clock.asm from {start}: H, M, 0, D", repr(out[-40:]) + err)
    import datetime
    for day in ("2025-01-01", "2028-03-01", "2026-10-11", "2088-12-31"):
        r = subprocess.run([EMU, "--rtc", ".".join(reversed(day.split("-"))) + "T09:08:07", "-n", "8000", "--screen", b], input="", capture_output=True, text=True, timeout=30)
        name = datetime.date.fromisoformat(day).strftime("%A")
        ru = ".".join(reversed(day.split("-")))
        check(f"{ru}, {name}" in r.stdout and "TIME $920" in r.stdout, f"clock.asm: {ru} is {name}", r.stdout[-600:])

# ---- checking the fragments against the document
if DOC:
    if os.path.exists(DOC):
        doc = open(DOC, encoding="utf-8").read()
        for name, frag in (("6.6 driver", DRIVER), ("6.6 IRQ1 wait", IRQ_WAIT), ("7.5 map", MAP_SAVE), ("7.7 mmu_init", MMU_INIT),
                           ("8.5 rtc_get, rtc_set", RTC_GETSET), ("8.5 rtc_hour24", RTC_HOUR24)):
            check("```asm\n" + frag + "```" in doc, f"fragment {name} equals the text in {os.path.basename(DOC)}")
    else:
        check(False, f"document {DOC} not found")

print(f"{'FAILED' if failed else 'OK'}: {passed} passed, {failed} failed")
sys.exit(1 if failed else 0)
