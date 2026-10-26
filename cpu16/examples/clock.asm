; clock.asm - a clock: the date, the day of week and the time HH:MM:SS in large digits made of blocks.
; H - hour +1, M - minute +1, 0 - zero the seconds, D - next day, Esc - exit.
;
; The example shows the real-time clock ("CPU16 Machine", sect. 8):
;   - reading the clock (rtc_get: the READ command, then the TIME and DATE registers);
;   - detecting a change of second by polling: the 50 Hz timer wakes the program from WFI, and it redraws
;     the screen only when the clock value changed;
;   - setting only the time of day (WRITE_TIME) and only the date (WRITE_DATE): after READ
;     the needed fields are changed in the registers, the rest is as read;
;   - checking ERR: "next day" first tries day + 1, and if there is no such date
;     (the clock did not accept it), sets the 1st of the next month.
; The clock runs by itself, so a second can pass between READ and the write; for the example this does not matter.
; On exit the program prints the clock reading to the console.
;
; Used from stdlib.asm: vectors_init, rtc_get, rtc_hour24, print_date, print_time, scr_mode,
; scr_attr, scr_clear, scr_goto, scr_print_at, scr_box, putc, puts, newline, getkey_nb,
; timer_on, timer_off.

        .equ    VECTORS,    $FF00
        .equ    RAM_START,  $8000
        .equ    DIGITS_COL, 8           ; where to draw the large digits
        .equ    DIGITS_ROW, 7

; ============================================================================
; Prologue
; ============================================================================
        .org    $0000
reset:  LD      SP, #$FF00
        LD      R0, #on_fault           ; ERRCMD goes to on_fault (first of all),
        CALL    vectors_init            ;   the other vectors - to the empty irq_ignore
        LD      R0, #on_tick            ; IRQ0 - the timer
        ST      [VECTORS + 2*0], R0
        CALL    timer_on                ; the timer only wakes the program from WFI
        STI
        CALL    main
halt:   CALL    timer_off               ; stop: the timer off, CLI, WFI
        CLI
@stop:  WFI
        JMP     @stop

; IRQ0: every 20 ms. Does nothing: the exit from WFI in the main program is the event itself.
on_tick:
        IRET

on_fault:
        LD      R0, #MODE_TTY
        CALL    scr_mode
        LD      R0, #msg_fault
        CALL    puts
        JMP     halt

; ============================================================================
; The main program. R4:R5 are the time and date shown on the screen (to redraw on change).
; ============================================================================
main:   PUSH    LR
        PUSH    R4
        PUSH    R5
        LD      R0, #0                  ; coordinate mode, cursor hidden
        CALL    scr_mode
        LD      R0, #BLACK << 3 | CYAN
        CALL    scr_attr
        CALL    scr_clear
        LD      R0, #4
        LD      R1, #4
        LD      R2, #72
        LD      R3, #13
        CALL    scr_box
        LD      R0, #BLACK << 3 | WHITE
        CALL    scr_attr
        LD      R0, #32
        LD      R1, #2
        LD      R2, #msg_title
        CALL    scr_print_at
        LD      R0, #9
        LD      R1, #19
        LD      R2, #msg_keys
        CALL    scr_print_at
        LD      R4, #$FFFF              ; deliberately unequal: the first drawing
        LD      R5, #$FFFF

@loop:  WFI                             ; wait for a timer tick
@keys:  CALL    getkey_nb
        JZ      @draw
        LD      R1, R0
        SHR     R1, #8                  ; scancode
        CMP     R1, #SC_ESC             ; special keys - by scancode
        JZ      @quit
        AND     R0, #$FF                ; printable ones - by character (there is no scancode from stdin)
        CMP     R0, #27                 ; Esc without a scancode
        JZ      @quit
        OR      R0, #$20                ; letters to lower case
        CMP     R0, #'h'
        JNZ     @not_h
        CALL    next_hour
        JMP     @keys
@not_h: CMP     R0, #'m'
        JNZ     @not_m
        CALL    next_minute
        JMP     @keys
@not_m: CMP     R0, #'0'
        JNZ     @not_0
        CALL    zero_seconds
        JMP     @keys
@not_0: CMP     R0, #'d'
        JNZ     @keys
        CALL    next_day
        JMP     @keys
@draw:  CALL    rtc_get                 ; R0 is the time, R1 the date
        CMP     R0, R4
        JNZ     @new
        CMP     R1, R5
        JZ      @loop                   ; the value did not change
@new:   LD      R4, R0
        LD      R5, R1
        CALL    draw_clock
        JMP     @loop

@quit:  LD      R0, #WHITE              ; restore the ordinary console
        CALL    scr_attr
        LD      R0, #MODE_TTY | MODE_CURSOR
        CALL    scr_mode
        CALL    scr_clear
        LD      R0, #msg_bye
        CALL    puts
        CALL    rtc_get
        PUSH    R0
        PUSH    R1
        CALL    print_date
        LD      R0, #' '
        CALL    putc
        POP     R1
        POP     R0
        CALL    print_time
        CALL    newline
        POP     R5
        POP     R4
        POP     IP

; ============================================================================
; Setting the clock
; ============================================================================

; next_hour - hour + 1 around the circle 23 -> 0, the date does not change. The hour of the 24-hour notation lies in two
; places: hour 0..11 in TIME, the PM flag in bit 0 of the DATE register; WRITE_TIME takes both.
next_hour:
        PUSH    LR
        CALL    rtc_get                 ; READ: TIME and DATE hold the current value
        PUSH    R0
        CALL    rtc_hour24              ; R0 = 0..23
        INC     R0
        CMP     R0, #24
        JB      @ok
        LD      R0, #0
@ok:    POP     R2                      ; time
        AND     R2, #$0FFF              ; minute and second as read
        AND     R1, #$FFFE              ; PM = 0
        CMP     R0, #12
        JB      @am
        SUB     R0, #12
        OR      R1, #1                  ; PM = 1
@am:    SHL     R0, #12
        OR      R2, R0
        ST      [RTC_TIME], R2
        ST      [RTC_DATE], R1          ; only the PM bit is needed; the year, month, day do not matter
        LD      R0, #RTC_WRITE_TIME
        STB     [RTC_CMD], R0
        POP     IP

; next_minute - minute + 1 around the circle 59 -> 0, the hour does not change. After READ the DATE register already
; holds the needed PM flag, so only TIME is written.
next_minute:
        PUSH    LR
        CALL    rtc_get
        LD      R1, R0
        SHR     R1, #6
        AND     R1, #63                 ; minute
        INC     R1
        CMP     R1, #60
        JB      @ok
        LD      R1, #0
@ok:    SHL     R1, #6
        AND     R0, #$F03F              ; hour and second as read
        OR      R0, R1
        ST      [RTC_TIME], R0
        LD      R0, #RTC_WRITE_TIME
        STB     [RTC_CMD], R0
        POP     IP

; zero_seconds - seconds = 0. WRITE_TIME also resets the fraction of a second: the next second comes
; exactly 1 s after the key press, as when setting a clock by a time signal.
zero_seconds:
        PUSH    LR
        CALL    rtc_get
        AND     R0, #$FFC0
        ST      [RTC_TIME], R0
        LD      R0, #RTC_WRITE_TIME
        STB     [RTC_CMD], R0
        POP     IP

; next_day - the next day, the time does not change. Day + 1; if there is no such date, the clock does not
; accept it (ERR = 1) - then the 1st of the next month, and after December - January of the next
; year (after year 2088 comes 2025: the year field simply overflows).
next_day:
        PUSH    LR
        CALL    rtc_get                 ; R1 is the date
        LD      R2, R1
        LD      R0, R1
        SHR     R0, #1
        AND     R0, #31
        CMP     R0, #31
        JZ      @month                  ; day + 1 would overflow the day field
        ADD     R1, #1 << 1             ; day + 1
        CALL    write_date
        JZ      @done                   ; the date is accepted
@month: LD      R1, R2
        AND     R1, #$FFC1              ; day = 0 (PM does not matter: WRITE_DATE does not take it)
        OR      R1, #1 << 1             ; day 1
        ADD     R1, #1 << 6             ; month + 1
        LD      R0, R1
        SHR     R0, #6
        AND     R0, #15
        CMP     R0, #13
        JB      @set
        AND     R1, #$FC3F              ; month = 0
        OR      R1, #1 << 6             ; January
        ADD     R1, #1 << 10            ; year + 1
@set:   CALL    write_date
@done:  POP     IP

; write_date - set the date R1 with the WRITE_DATE command. Result: R0 is ERR, ZF = 1 - accepted.
write_date:
        ST      [RTC_DATE], R1
        LD      R0, #RTC_WRITE_DATE
        STB     [RTC_CMD], R0
        LDZX    R0, [RTC_STATUS]
        TEST    R0, #RTC_ERR
        RET

; ============================================================================
; Screen
; ============================================================================

; draw_clock - draw the time R0 and the date R1.
draw_clock:
        PUSH    LR
        PUSH    R4
        PUSH    R5
        PUSH    R6
        LD      R4, R0                  ; time
        LD      R6, R1                  ; date
        LD      R0, #BLACK << 3 | YELLOW
        CALL    scr_attr
        LD      R5, #DIGITS_COL         ; the column of the next character
        LD      R0, R4
        LD      R1, R6
        CALL    rtc_hour24
        CALL    two_digits              ; hours
        LD      R0, #10                 ; ':'
        CALL    glyph
        LD      R0, R4
        SHR     R0, #6
        AND     R0, #63
        CALL    two_digits              ; minutes
        LD      R0, #10
        CALL    glyph
        LD      R0, R4
        AND     R0, #63
        CALL    two_digits              ; seconds
        LD      R0, #BLACK << 3 | WHITE ; the date and the day of week
        CALL    scr_attr
        LD      R0, #26
        LD      R1, #13
        CALL    scr_goto
        LD      R1, R6
        CALL    print_date
        LD      R0, #','
        CALL    putc
        LD      R0, #' '
        CALL    putc
        LD      R1, R6
        CALL    weekday
        ADD     R0, R0
        LD      R0, [R0+day_names]
        CALL    puts                    ; the names have the same length: the previous one is overwritten entirely
        LD      R0, #BLACK << 3 | GRAY  ; exchange registers: this is how the value lies in MTIME
        CALL    scr_attr
        LD      R0, #26
        LD      R1, #15
        LD      R2, #msg_time
        CALL    scr_print_at
        LD      R0, R4
        CALL    print_hex16
        LD      R0, #msg_date
        CALL    puts
        LD      R0, R6
        CALL    print_hex16
        POP     R6
        POP     R5
        POP     R4
        POP     IP

; weekday - the day of week of date R1. Result: R0 = 0..6, 0 is Monday.
; The day number from 1 January 2025 (a Wednesday) = year · 365 + the number of leap years before this year
; + days before the start of the month + day - 1; leap years are every fourth, starting with 2028 (year field 3).
weekday:
        LD      R2, R1
        SHR     R2, #10                 ; year field 0..63
        LD      R0, R2
        MUL     R0, #365
        LD      R3, R2
        SHR     R3, #2                  ; leap years before this year
        ADD     R0, R3
        LD      R3, R1
        SHR     R3, #6
        AND     R3, #15                 ; month 1..12
        CMP     R3, #3
        JB      @nleap                  ; January, February: 29 February of this year has not happened yet
        AND     R2, #3
        CMP     R2, #3
        JNZ     @nleap
        INC     R0                      ; the year is a leap one, 29 February has passed
@nleap: ADD     R3, R3
        LD      R3, [R3+month_start - 2]
        ADD     R0, R3
        LD      R3, R1
        SHR     R3, #1
        AND     R3, #31                 ; day
        ADD     R0, R3
        ADD     R0, #1                  ; - 1 for the day, + 2: 1 January 2025 is a Wednesday
        REMU    R0, #7
        RET

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

; glyph - a large character R0 (0..9, 10 - ':') in column R5, R5 += 8.
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
; days from the start of the year to the start of the month (a non-leap year)
month_start: .word 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334
day_names:   .word d_mon, d_tue, d_wed, d_thu, d_fri, d_sat, d_sun
d_mon:       .asciz  "Monday   "
d_tue:       .asciz  "Tuesday  "
d_wed:       .asciz  "Wednesday"
d_thu:       .asciz  "Thursday "
d_fri:       .asciz  "Friday   "
d_sat:       .asciz  "Saturday "
d_sun:       .asciz  "Sunday   "
msg_title:   .asciz  "REAL-TIME CLOCK"
msg_keys:    .asciz  "H - hour+1   M - minute+1   0 - zero seconds   D - next day   Esc - quit"
msg_time:    .asciz  "TIME $"
msg_date:    .asciz  "   DATE $"
msg_bye:     .asciz  "Clock: "
msg_fault:   .asciz  "\nfault\n"

        .include "../lib/stdlib.asm"
