; display_demo.asm - an overview of the 80x25 display's features: colors, the palette, frames,
; box-drawing characters, Cyrillic, recoloring text without redrawing.
;
; In the web debugger: build (Ctrl+B), open the display with the "Display" button, run (F9).
; Any key (with the display focused) ends the program.
; On the command line: cpu16asm display_demo.asm && cpu16 --screen display_demo.bin < /dev/null
;
; Used from stdlib.asm: vectors_init, scr_mode, scr_attr, scr_clear, scr_goto, scr_print_at,
; scr_box, scr_paint, putc, puts, getkey.

        .equ    VECTORS,    $FF00

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
; ============================================================================
main:   PUSH    LR
        PUSH    R4
        PUSH    R5
        LD      R0, #0                  ; coordinate mode, cursor hidden
        CALL    scr_mode
        LD      R0, #BLUE << 3 | WHITE
        CALL    scr_attr
        CALL    scr_clear

        ; ---- a title in a frame
        LD      R0, #BLUE << 3 | YELLOW
        CALL    scr_attr
        LD      R0, #16
        LD      R1, #0
        LD      R2, #48
        LD      R3, #3
        CALL    scr_box
        LD      R0, #BLUE << 3 | WHITE
        CALL    scr_attr
        LD      R0, #19
        LD      R1, #1
        LD      R2, #msg_title
        CALL    scr_print_at

        ; ---- the palette: a row is the background color, a column is the text color
        LD      R0, #4
        LD      R1, #4
        LD      R2, #msg_palette
        CALL    scr_print_at
        LD      R4, #0                  ; background
@bg:    LD      R5, #0                  ; text
@fg:    LD      R0, R5                  ; column = 4 + text * 5
        MUL     R0, #5
        ADD     R0, #4
        LD      R1, R4                  ; row = 5 + background
        ADD     R1, #5
        CALL    scr_goto
        LD      R0, R4                  ; attribute = background << 3 | text
        SHL     R0, #3
        OR      R0, R5
        CALL    scr_attr
        LD      R0, #msg_sample
        CALL    puts
        INC     R5
        CMP     R5, #8
        JB      @fg
        INC     R4
        CMP     R4, #8
        JB      @bg

        ; ---- styling: underline and inversion
        LD      R0, #BLUE << 3 | WHITE | UNDERLINE
        CALL    scr_attr
        LD      R0, #48
        LD      R1, #5
        LD      R2, #msg_underline
        CALL    scr_print_at
        LD      R0, #BLUE << 3 | WHITE | INVERSE
        CALL    scr_attr
        LD      R0, #48
        LD      R1, #7
        LD      R2, #msg_inverse
        CALL    scr_print_at
        LD      R0, #BLUE << 3 | YELLOW | INVERSE | UNDERLINE
        CALL    scr_attr
        LD      R0, #48
        LD      R1, #9
        LD      R2, #msg_both
        CALL    scr_print_at

        ; ---- encoding and box-drawing characters
        LD      R0, #BLUE << 3 | WHITE
        CALL    scr_attr
        LD      R0, #4
        LD      R1, #15
        LD      R2, #msg_cyr
        CALL    scr_print_at
        LD      R0, #4
        LD      R1, #16
        LD      R2, #msg_icons
        CALL    scr_print_at
        LD      R0, #4
        LD      R1, #17
        LD      R2, #msg_blocks
        CALL    scr_print_at
        LD      R4, #0                  ; a table with single lines
@table: LD      R0, #58
        LD      R1, R4
        ADD     R1, #12
        LD      R2, R4
        ADD     R2, R2                  ; table row * 2 = the offset in the address list
        LD      R2, [R2+table_rows]
        CALL    scr_print_at
        INC     R4
        CMP     R4, #5
        JB      @table

        ; ---- recoloring: highlight a word without redrawing the text
        LD      R0, #4
        LD      R1, #19
        LD      R2, #msg_paint
        CALL    scr_print_at
        LD      R0, #14                 ; the column of the word "repaints" in this row
        LD      R1, #19
        CALL    scr_goto
        LD      R0, #RED << 3 | YELLOW
        LD      R1, #8
        CALL    scr_paint

        ; ---- the bottom row
        LD      R0, #CYAN << 3 | BLACK
        CALL    scr_attr
        LD      R0, #0
        LD      R1, #24
        CALL    scr_goto
        LD      R0, #CMD_CLEOL          ; the whole row - in the hint background
        STB     [CON_CMD], R0
        LD      R0, #2
        LD      R1, #24
        LD      R2, #msg_key
        CALL    scr_print_at

        CALL    getkey                  ; wait for any key

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

; ============================================================================
; Constants
; ============================================================================
msg_title:     .asciz  "CPU16 display: 80 x 25, 8 colours"
msg_palette:   .asciz  "Palette: rows - background 0..7, columns - text 0..7"
msg_sample:    .asciz  " Ab  "
msg_underline: .asciz  " underlined "
msg_inverse:   .asciz  " inverse "
msg_both:      .asciz  " inverse + underlined "
msg_cyr:       .asciz  "Cyrillic: Съешь же ещё этих мягких булок, Ёжик!"
msg_icons:     .asciz  "Icons: ☺ ☻ ♥ ♦ ♣ ♠ ♪ ♫ ☼ ► ◄ ▲ ▼ ↑ ↓ → ←"
msg_blocks:    .asciz  "Blocks: █ ▓ ▒ ░ ▀ ▄ ▌ ▐   ╔═╦═╗ ║ ║ ║"
msg_paint:     .asciz  "scr_paint repaints text without redrawing it"
msg_key:       .asciz  "Press any key to finish"
msg_bye:       .asciz  "Display demo finished.\n"
msg_fault:     .asciz  "\nfault\n"
table_rows:    .word   row0, row1, row2, row3, row4
row0:          .asciz  "┌──────┬──────┐"
row1:          .asciz  "│ cell │ cell │"
row2:          .asciz  "├──────┼──────┤"
row3:          .asciz  "│ cell │ cell │"
row4:          .asciz  "└──────┴──────┘"

        .include "../lib/stdlib.asm"
