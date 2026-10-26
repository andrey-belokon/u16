; stopwatch.asm - a stopwatch on the 50 Hz timer: the time mm:ss.ss in large digits made of blocks.
; Space - start/stop, R - reset, Esc - exit.
;
; The example shows the real-time timer (IRQ0 every 20 ms), an interrupt handler,
; waiting in WFI, keyboard polling without waiting and drawing with box-drawing characters.
; The handler counts ticks, and the main program only redraws the screen when
; the tick count changed: this way the time does not depend on the emulator's speed.
;
; Used from stdlib.asm: vectors_init, scr_mode, scr_attr, scr_clear, scr_goto, scr_print_at,
; scr_box, putc, puts, getkey_nb, timer_on, timer_off.

        .equ    VECTORS,    $FF00
        .equ    RAM_START,  $8000
        .equ    DIGITS_COL, 8           ; where to draw the large digits
        .equ    DIGITS_ROW, 8

; ============================================================================
; Prologue
; ============================================================================
        .org    $0000
reset:  LD      SP, #$FF00
        LD      R0, #on_fault           ; ERRCMD goes to on_fault (first of all),
        CALL    vectors_init            ;   the other vectors - to the empty irq_ignore
        LD      R0, #on_tick            ; IRQ0 - the timer
        ST      [VECTORS + 2*0], R0
        LD      R0, #0                  ; variables
        ST      [ticks], R0
        ST      [running], R0
        CALL    timer_on                ; the vector is already filled in: it is safe to enable
        STI
        CALL    main
halt:   CALL    timer_off               ; stop: the timer off, CLI, WFI
        CLI
@stop:  WFI
        JMP     @stop

; IRQ0: every 20 ms; counts ticks while the stopwatch runs.
on_tick:
        PUSH    R0
        LD      R0, [running]
        TEST    R0, R0
        JZ      @out
        LD      R0, [ticks]
        INC     R0
        ST      [ticks], R0
@out:   POP     R0
        IRET

on_fault:
        LD      R0, #MODE_TTY
        CALL    scr_mode
        LD      R0, #msg_fault
        CALL    puts
        JMP     halt

; ============================================================================
; The main program. R4 is the number of ticks shown on the screen (to redraw on change).
; ============================================================================
main:   PUSH    LR
        PUSH    R4
        LD      R0, #0                  ; coordinate mode, cursor hidden
        CALL    scr_mode
        LD      R0, #BLACK << 3 | GREEN
        CALL    scr_attr
        CALL    scr_clear
        LD      R0, #4
        LD      R1, #5
        LD      R2, #72
        LD      R3, #10
        CALL    scr_box
        LD      R0, #BLACK << 3 | WHITE
        CALL    scr_attr
        LD      R0, #33
        LD      R1, #2
        LD      R2, #msg_title
        CALL    scr_print_at
        LD      R0, #20
        LD      R1, #18
        LD      R2, #msg_keys
        CALL    scr_print_at
        LD      R4, #$FFFF              ; deliberately unequal: the first drawing
        CALL    show_state

@loop:  WFI                             ; wait for a tick (or a key press: we will check on the next tick)
@keys:  CALL    getkey_nb
        JZ      @draw
        LD      R1, R0
        SHR     R1, #8                  ; scancode
        CMP     R1, #SC_ESC             ; special keys - by scancode
        JZ      @quit
        AND     R0, #$FF                ; printable ones - by character (there is no scancode from stdin)
        CMP     R0, #27                 ; Esc without a scancode
        JZ      @quit
        CMP     R0, #' '
        JNZ     @notspace
        LD      R0, [running]           ; start/stop
        XOR     R0, #1
        ST      [running], R0
        CALL    show_state
        JMP     @keys
@notspace:
        CMP     R0, #'r'
        JZ      @reset
        CMP     R0, #'R'
        JNZ     @keys
@reset: LD      R0, #0
        ST      [ticks], R0
        JMP     @keys
@draw:  LD      R0, [ticks]
        CMP     R0, R4
        JZ      @loop                   ; the time did not change
        LD      R4, R0
        CALL    draw_time
        JMP     @loop

@quit:  LD      R0, #WHITE              ; restore the ordinary console
        CALL    scr_attr
        LD      R0, #MODE_TTY | MODE_CURSOR
        CALL    scr_mode
        CALL    scr_clear
        LD      R0, #msg_bye
        CALL    puts
        POP     R4
        POP     IP

; show_state - the "running" / "stopped" label.
show_state:
        LD      R0, [running]
        LD      R2, #msg_stopped
        LD      R1, #BLACK << 3 | RED
        TEST    R0, R0
        JZ      @show
        LD      R2, #msg_running
        LD      R1, #BLACK << 3 | GREEN
@show:  STB     [CON_ATTR], R1
        LD      R0, #35
        LD      R1, #16
        JMP     scr_print_at            ; a tail call

; draw_time - draw the time R0 (in 20 ms ticks) as mm:ss.ss in large digits.
; Minutes = ticks / 3000, seconds = ticks / 50 mod 60, hundredths = (ticks mod 50) * 2.
draw_time:
        PUSH    LR
        PUSH    R4
        PUSH    R5
        LD      R4, R0
        LD      R0, #BLACK << 3 | YELLOW
        CALL    scr_attr
        LD      R5, #DIGITS_COL         ; the column of the next character
        LD      R0, R4
        DIVU    R0, #3000               ; minutes
        CALL    two_digits
        LD      R0, #10                 ; ':'
        CALL    glyph
        LD      R0, R4
        DIVU    R0, #50
        REMU    R0, #60                 ; seconds
        CALL    two_digits
        LD      R0, #11                 ; '.'
        CALL    glyph
        LD      R0, R4
        REMU    R0, #50
        SHL     R0, #1                  ; hundredths
        CALL    two_digits
        POP     R5
        POP     R4
        POP     IP

; two_digits - the number R0 (0..99) as two large digits from position R5.
two_digits:
        PUSH    LR
        PUSH    R6
        LD      R6, R0
        DIVU    R0, #10
        CALL    glyph
        LD      R0, R6
        REMU    R0, #10
        CALL    glyph
        POP     R6
        POP     IP

; glyph - a large character R0 (0..9, 10 - ':', 11 - '.') in column R5, R5 += 8.
; A character is 3x5 dots, a dot is two character cells: '█' or a blank.
glyph:  PUSH    R6
        PUSH    R7
        ADD     R0, R0
        LD      R6, [R0+font]           ; 15 bits: 5 rows of 3 dots, the high ones are the top row
        LD      R7, #0                  ; a character row
@row:   STB     [CON_COL], R5
        LD      R0, R7
        ADD     R0, #DIGITS_ROW
        STB     [CON_ROW], R0
        LD      R1, #3                  ; dots in a row
@dot:   LD      R2, #' '
        TEST    R6, #$4000              ; the next dot is the highest of the 15 bits
        JZ      @put
        LD      R2, #'█'
@put:   STB     [CONSOLE], R2
        STB     [CONSOLE], R2
        SHL     R6, #1
        DEC     R1
        JNZ     @dot
        INC     R7
        CMP     R7, #5
        JB      @row
        ADD     R5, #8
        POP     R7
        POP     R6
        RET

; ============================================================================
; Constants
; ============================================================================
; the 3x5 font: rows from top to bottom, 3 bits each (bit 14 is the left dot of the top row)
font:   .word   %111101101101111                               ; 0
        .word   %010110010010111                               ; 1
        .word   %111001111100111                               ; 2
        .word   %111001111001111                               ; 3
        .word   %101101111001001                               ; 4
        .word   %111100111001111                               ; 5
        .word   %111100111101111                               ; 6
        .word   %111001001001001                               ; 7
        .word   %111101111101111                               ; 8
        .word   %111101111001111                               ; 9
        .word   %000010000010000                               ; :
        .word   %000000000000010                               ; .
msg_title:   .asciz  "STOPWATCH"
msg_keys:    .asciz  "Space - start/stop    R - reset    Esc - quit"
msg_running: .asciz  " running "
msg_stopped: .asciz  " stopped "
msg_bye:     .asciz  "Stopwatch finished.\n"
msg_fault:   .asciz  "\nfault\n"

        .include "../lib/stdlib.asm"

; ============================================================================
; RAM
; ============================================================================
        .org    RAM_START
ticks:      .space  2                   ; ticks while the stopwatch runs
running:    .space  2                   ; 1 - running
