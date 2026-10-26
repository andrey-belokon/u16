; keys.asm - a key viewer: for each key press shows the character, its code,
; the scancode and the key name. Esc exits.
;
; The example shows reading a word from port $FF20 (the character and scancode of one key press),
; decoding special keys by scancode and a "scancode -> name" table.
;
; In the web debugger: open the display with the "Display" button, run (F9), click the display
; and press keys - with the display focused all keys except Pause go to the program.
;
; Used from stdlib.asm: vectors_init, scr_mode, scr_attr, scr_clear, scr_goto, scr_print_at,
; scr_box, putc, puts, print_hex8, getkey.

        .equ    VECTORS,    $FF00
        .equ    FIRST_ROW,  6           ; log rows
        .equ    LOG_ROWS,   16

; ============================================================================
; Prologue
; ============================================================================
        .org    $0000
reset:  LD      SP, #$FF00
        LD      R0, #on_fault           ; ERRCMD goes to on_fault (first of all),
        CALL    vectors_init            ;   the other vectors - to the empty irq_ignore
        CALL    main
halt:   CLI
@stop:  WFI
        JMP     @stop

on_fault:
        LD      R0, #MODE_TTY
        CALL    scr_mode
        LD      R0, #msg_fault
        CALL    puts
        JMP     halt

; ============================================================================
; Main program
; R4 is the key press code (character | scancode << 8), R5 the log row number (0..LOG_ROWS-1)
; ============================================================================
main:   PUSH    LR
        PUSH    R4
        PUSH    R5
        LD      R0, #0                  ; coordinate mode, cursor hidden
        CALL    scr_mode
        LD      R0, #BLACK << 3 | WHITE
        CALL    scr_attr
        CALL    scr_clear
        LD      R0, #BLACK << 3 | CYAN
        CALL    scr_attr
        LD      R0, #10
        LD      R1, #0
        LD      R2, #60
        LD      R3, #3
        CALL    scr_box
        LD      R0, #13
        LD      R1, #1
        LD      R2, #msg_title
        CALL    scr_print_at
        LD      R0, #BLACK << 3 | YELLOW | UNDERLINE
        CALL    scr_attr
        LD      R0, #12
        LD      R1, #4
        LD      R2, #msg_header
        CALL    scr_print_at
        LD      R5, #0

@key:   CALL    getkey                  ; R0 = character | scancode << 8
        LD      R4, R0
        CALL    show                    ; a log row
        INC     R5
        CMP     R5, #LOG_ROWS
        JB      @next
        LD      R5, #0
@next:  LD      R0, R4
        SHR     R0, #8
        CMP     R0, #SC_ESC
        JNZ     @key

        LD      R0, #WHITE              ; restore the ordinary console
        CALL    scr_attr
        LD      R0, #MODE_TTY | MODE_CURSOR
        CALL    scr_mode
        CALL    scr_clear
        LD      R0, #msg_bye
        CALL    puts
        POP     R5
        POP     R4
        POP     IP

; show - the log row for key press R4 in row FIRST_ROW + R5; the ► marker on the last row.
show:   PUSH    LR
        LD      R0, #BLACK << 3 | WHITE ; erase the marker on the previous row
        CALL    scr_attr
        LD      R1, R5
        ADD     R1, #FIRST_ROW + LOG_ROWS - 1
        CMP     R5, #0
        JZ      @prev
        LD      R1, R5
        ADD     R1, #FIRST_ROW - 1
@prev:  LD      R0, #10
        CALL    scr_goto
        LD      R0, #' '
        CALL    putc
        LD      R0, #10                 ; a new row: clear it
        LD      R1, R5
        ADD     R1, #FIRST_ROW
        CALL    scr_goto
        LD      R0, #CMD_CLEOL
        STB     [CON_CMD], R0
        LD      R0, #BLACK << 3 | YELLOW
        CALL    scr_attr
        LD      R0, #'►'
        CALL    putc
        LD      R0, #BLACK << 3 | WHITE
        CALL    scr_attr
        LD      R0, #' '
        CALL    putc
        ; the character: printable - in quotes, otherwise a dash
        LD      R0, R4
        AND     R0, #$FF
        CMP     R0, #' '
        JB      @nochar
        LD      R0, #'"'
        CALL    putc
        LD      R0, R4
        CALL    putc                    ; the low byte is the character
        LD      R0, #'"'
        CALL    putc
        JMP     @code
@nochar:
        LD      R0, #msg_none
        CALL    puts
@code:  LD      R0, #msg_sep
        CALL    puts
        LD      R0, R4                  ; the character code
        CALL    print_hex8
        LD      R0, #msg_sep2
        CALL    puts
        LD      R0, R4                  ; scancode
        SHR     R0, #8
        CALL    print_hex8
        LD      R0, #msg_sep3
        CALL    puts
        LD      R0, R4                  ; the name
        CALL    key_name
        CALL    puts
        POP     IP

; key_name - the key name. R0 is the key press code. Result: R0 is the string address.
key_name:
        LD      R1, R0
        AND     R1, #$FF                ; character
        SHR     R0, #8                  ; scancode
        LD      R2, #names
@find:  LDZX    R3, [R2]
        TEST    R3, R3
        JZ      @other
        CMP     R3, R0
        JZ      @found
        ADD     R2, #4                  ; an entry: the scancode (a word), the name address (a word)
        JMP     @find
@found: LD      R0, [R2+2]
        RET
@other: CMP     R1, #10                 ; Enter without a scancode (input from stdin on the command line)
        JNZ     @ctrl
        LD      R0, #name_enter
        RET
@ctrl:  CMP     R1, #27                 ; Ctrl + a letter
        JAE     @plain
        TEST    R1, R1
        JZ      @plain
        LD      R0, #name_ctrl
        RET
@plain: LD      R0, #name_char
        RET

; ============================================================================
; Constants
; ============================================================================
msg_title:  .asciz  "Keyboard: press keys (Esc quits)"
msg_header: .asciz  "  char   code   scancode   key            "
msg_none:   .asciz  " - "
msg_sep:    .asciz  "    $"
msg_sep2:   .asciz  "    $"
msg_sep3:   .asciz  "        "
msg_bye:    .asciz  "Keyboard viewer finished.\n"
msg_fault:  .asciz  "\nfault\n"

; the table: scancode (a word), name address (a word); the end is scancode 0
names:  .word   SC_ESC, name_esc,  SC_ENTER, name_enter,  SC_SPACE, name_space
        .word   SC_BACKSPACE, name_bs,  SC_TAB, name_tab
        .word   SC_UP, name_up,  SC_DOWN, name_down,  SC_LEFT, name_left,  SC_RIGHT, name_right
        .word   SC_HOME, name_home,  SC_END, name_end,  SC_PGUP, name_pgup,  SC_PGDN, name_pgdn
        .word   SC_INS, name_ins,  SC_DEL, name_del
        .word   SC_F1, name_f1,  SC_F1+1, name_f2,  SC_F1+2, name_f3,  SC_F1+3, name_f4
        .word   SC_F1+4, name_f5,  SC_F1+5, name_f6,  SC_F1+6, name_f7,  SC_F1+7, name_f8
        .word   SC_F1+8, name_f9,  SC_F1+9, name_f10,  SC_F11, name_f11,  SC_F12, name_f12
        .word   0
name_esc:   .asciz  "Esc"
name_enter: .asciz  "Enter"
name_space: .asciz  "Space"
name_bs:    .asciz  "Backspace"
name_tab:   .asciz  "Tab"
name_up:    .asciz  "↑ Up"
name_down:  .asciz  "↓ Down"
name_left:  .asciz  "← Left"
name_right: .asciz  "→ Right"
name_home:  .asciz  "Home"
name_end:   .asciz  "End"
name_pgup:  .asciz  "PgUp"
name_pgdn:  .asciz  "PgDn"
name_ins:   .asciz  "Insert"
name_del:   .asciz  "Delete"
name_f1:    .asciz  "F1"
name_f2:    .asciz  "F2"
name_f3:    .asciz  "F3"
name_f4:    .asciz  "F4"
name_f5:    .asciz  "F5"
name_f6:    .asciz  "F6"
name_f7:    .asciz  "F7"
name_f8:    .asciz  "F8"
name_f9:    .asciz  "F9"
name_f10:   .asciz  "F10"
name_f11:   .asciz  "F11"
name_f12:   .asciz  "F12"
name_ctrl:  .asciz  "Ctrl + letter"
name_char:  .asciz  "character key"

        .include "../lib/stdlib.asm"
