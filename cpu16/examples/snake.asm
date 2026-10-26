; snake.asm - the classic "snake" on the 80x25 display.
; Arrows - direction, digits 1..9 - speed, space - pause, Esc - exit;
; after a loss Enter - a new game.
;
; What the example shows:
;   - a real-time timer: the snake moves once every delay ticks of 20 ms, so the game
;     speed does not depend on the emulator's speed; every 5 eaten - a higher speed level;
;   - equal speed across the screen: a character cell is twice as tall as it is wide, so a vertical move
;     takes twice as many ticks as a horizontal one;
;   - the snake body of box-drawing lines: the segment's character is chosen by the neighboring segments;
;   - the keyboard without waiting: special keys (arrows, Esc) by scancode,
;     printable ones (space) by character;
;   - the field map in memory: the screen cannot be read (reading the data port takes a key),
;     so the game itself remembers what lies in each cell;
;   - a ring buffer: the snake body is the positions from tail to head;
;   - a pseudo-random number generator on MUL; the seed is the tick count at the moment a key is pressed
;     on the title screen, so every game is different.
;
; In the web debugger: build (Ctrl+B), open the display with the "Display" button, run (F9),
; click the display and play.

        .equ    VECTORS,    $FF00
        .equ    RAM_START,  $8000
        .equ    COLS,       80          ; screen width: cell position = row * 80 + column
        .equ    TOP,        2           ; the field inside the frame: rows 2..23, columns 1..78
        .equ    BOTTOM,     23
        .equ    LEFT,       1
        .equ    RIGHT,      78
        .equ    BUF_MASK,   2047        ; the body ring buffer: 2048 positions (the field is smaller)
        .equ    START_LEVEL, 3          ; the initial speed level (1..9)
        .equ    HINT_COL,   45          ; where in the status line the hint is (up to column 79)

        .equ    EMPTY,      0           ; cell contents on the map
        .equ    WALL,       1
        .equ    BODY,       2
        .equ    FOOD,       3

        .equ    A_FIELD,    BLACK << 3 | GRAY
        .equ    A_BORDER,   BLACK << 3 | CYAN
        .equ    A_BODY,     BLACK << 3 | GREEN
        .equ    A_HEAD,     BLACK << 3 | YELLOW
        .equ    A_FOOD,     BLACK << 3 | RED
        .equ    A_STATUS,   BLUE << 3 | WHITE
        .equ    A_BOX,      RED << 3 | WHITE

; ============================================================================
; Prologue
; ============================================================================
        .org    $0000
reset:  LD      SP, #$FF00
        LD      R0, #on_fault           ; ERRCMD goes to on_fault (first of all),
        CALL    vectors_init            ;   the other vectors - to the empty irq_ignore
        LD      R0, #on_tick            ; IRQ0 - the timer
        ST      [VECTORS + 2*0], R0
        LD      R0, #0
        ST      [ticks], R0
        ST      [best], R0
        LD      R0, #START_LEVEL        ; the level games start from (changed by the digits)
        ST      [base_level], R0
        CALL    timer_on
        STI
        CALL    main
halt:   CALL    timer_off               ; stop: the timer off, CLI, WFI
        CLI
@stop:  WFI
        JMP     @stop

on_tick:                                ; IRQ0: a counter of 20 ms ticks
        PUSH    R0
        LD      R0, [ticks]
        INC     R0
        ST      [ticks], R0
        POP     R0
        IRET

on_fault:
        LD      R0, #MODE_TTY
        CALL    scr_mode
        LD      R0, #msg_fault
        CALL    puts
        JMP     halt

; ============================================================================
; The main program: the title screen, games until exit
; ============================================================================
main:   PUSH    LR
        LD      R0, #0                  ; coordinate mode, cursor hidden
        CALL    scr_mode
        CALL    title
        TEST    R0, R0
        JNZ     @quit
@game:  CALL    play
        TEST    R0, R0                  ; 1 - the player pressed Esc during the game
        JNZ     @quit
        CALL    game_over
        TEST    R0, R0
        JZ      @game
@quit:  LD      R0, #WHITE              ; restore the ordinary console
        CALL    scr_attr
        LD      R0, #MODE_TTY | MODE_CURSOR
        CALL    scr_mode
        CALL    scr_clear
        LD      R0, #msg_bye
        CALL    puts
        LD      R0, [best]
        CALL    print_u16
        CALL    newline
        POP     IP

; ----------------------------------------------------------------------------
; title - the title screen. Result: R0 = 1 if Esc was pressed. The generator seed is the ticks.
; ----------------------------------------------------------------------------
title:  PUSH    LR
        LD      R0, #A_FIELD
        CALL    scr_attr
        CALL    scr_clear
        LD      R0, #A_BORDER
        CALL    scr_attr
        LD      R0, #20
        LD      R1, #7
        LD      R2, #40
        LD      R3, #9
        CALL    scr_box
        LD      R0, #A_HEAD
        CALL    scr_attr
        LD      R0, #33
        LD      R1, #9
        LD      R2, #msg_name
        CALL    scr_print_at
        LD      R0, #A_FIELD
        CALL    scr_attr
        LD      R0, #24
        LD      R1, #11
        LD      R2, #msg_help1
        CALL    scr_print_at
        LD      R0, #24
        LD      R1, #12
        LD      R2, #msg_help2
        CALL    scr_print_at
        LD      R0, #27
        LD      R1, #14
        LD      R2, #msg_start
        CALL    scr_print_at
        CALL    wait_key                ; R0 is the key code
        LD      R1, [ticks]             ; seed: the ticks and the key code
        XOR     R1, R0
        ST      [seed], R1
        CALL    is_esc
        POP     IP

; ----------------------------------------------------------------------------
; play - one game. Result: R0 = 1 - exit by Esc, 0 - a loss.
; R4 is the tick of the last move, R5 is 1 on pause.
; ----------------------------------------------------------------------------
play:   PUSH    LR
        PUSH    R4
        PUSH    R5
        CALL    new_game
        LD      R4, [ticks]
        LD      R5, #0
@loop:  WFI                             ; wait for a tick
@keys:  CALL    getkey_nb
        JZ      @time
        CALL    is_esc
        TEST    R0, R0
        JNZ     @quit
        LD      R0, R1                  ; is_esc returned the key code in R1
        AND     R0, #$FF
        CMP     R0, #' '                ; space - pause
        JNZ     @digit
        XOR     R5, #1
        CALL    show_pause
        JMP     @keys
@digit: CMP     R0, #'1'                ; digits 1..9 - the speed level
        JB      @dir
        CMP     R0, #'9'
        JA      @dir
        SUB     R0, #'0'
        ST      [base_level], R0        ; and for the following games
        CALL    set_level
        CALL    status
        JMP     @keys
@dir:   SHR     R1, #8                  ; scancode: arrows
        LD      R0, #-COLS
        CMP     R1, #SC_UP
        JZ      @set
        LD      R0, #COLS
        CMP     R1, #SC_DOWN
        JZ      @set
        LD      R0, #-1
        CMP     R1, #SC_LEFT
        JZ      @set
        LD      R0, #1
        CMP     R1, #SC_RIGHT
        JNZ     @keys
@set:   ST      [nextdir], R0
        JMP     @keys
@time:  TEST    R5, R5
        JNZ     @loop                   ; pause
        LD      R1, [nextdir]           ; the direction of the next move: new, if not backwards
        LD      R2, [dir]
        LD      R3, R2
        ADD     R3, R1
        JZ      @eff
        LD      R2, R1
@eff:   LD      R3, [delay]             ; ticks per move
        TEST    R2, #1                  ; +-1 are odd, +-80 are even
        JNZ     @horiz
        ADD     R3, R3                  ; vertically it takes twice as long: a character cell is twice as tall
@horiz: LD      R0, [ticks]
        SUB     R0, R4                  ; how many ticks have passed since the last move
        CMP     R0, R3
        JB      @loop
        LD      R4, [ticks]
        CALL    step
        TEST    R0, R0
        JZ      @loop
        LD      R0, #0                  ; a loss
        JMP     @out
@quit:  LD      R0, #1
@out:   POP     R5
        POP     R4
        POP     IP

; new_game - the field, a 4-cell snake in the center, the first food, the status line.
new_game:
        PUSH    LR
        PUSH    R4
        PUSH    R5
        LD      R0, #A_FIELD
        CALL    scr_attr
        CALL    scr_clear
        LD      R0, #A_BORDER
        CALL    scr_attr
        LD      R0, #0
        LD      R1, #1
        LD      R2, #80
        LD      R3, #24
        CALL    scr_box
        LD      R0, #grid               ; the map: everything is a wall, then the inside of the field is empty
        LD      R1, #WALL
        LD      R2, #COLS * 25
        CALL    memset
        LD      R4, #TOP
@rows:  LD      R0, R4
        MUL     R0, #COLS
        ADD     R0, #grid + LEFT
        LD      R1, #EMPTY
        LD      R2, #RIGHT - LEFT + 1
        CALL    memset
        INC     R4
        CMP     R4, #BOTTOM
        JBE     @rows
        LD      R0, #0                  ; the snake: row 12, columns 20..23, moving right
        ST      [tail_i], R0
        ST      [score], R0
        ST      [eaten], R0
        LD      R0, #3
        ST      [head_i], R0
        LD      R0, #1
        ST      [dir], R0
        ST      [nextdir], R0
        LD      R0, [base_level]
        CALL    set_level
        LD      R4, #0
@body:  LD      R5, R4                  ; position = 12 * 80 + 20 + i
        ADD     R5, #12 * COLS + 20
        LD      R0, R4
        ADD     R0, R0
        ST      [R0+body], R5
        LD      R0, #BODY
        STB     [R5+grid], R0
        INC     R4
        CMP     R4, #4
        JB      @body
        LD      R0, #12 * COLS + 23     ; head
        LD      R1, #'☻'
        LD      R2, #A_HEAD
        CALL    cell
        LD      R4, #0                  ; the body segments - lines by neighbors
@segs:  LD      R0, R4
        CALL    draw_seg
        INC     R4
        CMP     R4, #3
        JB      @segs
        CALL    place_food
        CALL    status
        POP     R5
        POP     R4
        POP     IP

; ----------------------------------------------------------------------------
; step - one move. Result: R0 = 1 - crashed, 0 - alive.
; R4 is the head position, R5 the new head position, R6 is 1 if the food was eaten,
; R7 is 1 if the tail moved.
; ----------------------------------------------------------------------------
step:   PUSH    LR
        PUSH    R4
        PUSH    R5
        PUSH    R6
        PUSH    R7
        LD      R6, #0
        LD      R7, #0
        LD      R0, [nextdir]           ; the new direction, if it is not backwards
        LD      R1, [dir]
        ADD     R1, R0
        JZ      @keepdir                ; opposite directions sum to 0
        ST      [dir], R0
@keepdir:
        LD      R0, [head_i]
        ADD     R0, R0
        LD      R4, [R0+body]
        LD      R5, R4
        ADD     R5, [dir]
        LDZX    R0, [R5+grid]
        CMP     R0, #FOOD
        JZ      @eat
        TEST    R0, R0
        JNZ     @dead                   ; a wall or the body
        LD      R0, [tail_i]            ; the tail leaves the cell
        ADD     R0, R0
        LD      R0, [R0+body]
        LD      R1, #EMPTY
        STB     [R0+grid], R1
        LD      R1, #' '
        LD      R2, #A_FIELD
        CALL    cell
        LD      R0, [tail_i]
        INC     R0
        AND     R0, #BUF_MASK
        ST      [tail_i], R0
        LD      R7, #1
        JMP     @advance
@eat:   LD      R0, [score]
        ADD     R0, #10
        ST      [score], R0
        LD      R0, [eaten]
        INC     R0
        ST      [eaten], R0
        REMU    R0, #5                  ; every 5 pieces - a higher speed level
        JNZ     @food
        LD      R0, [level]
        CMP     R0, #9
        JAE     @food
        INC     R0
        CALL    set_level
@food:  CALL    place_food
        LD      R6, #1                  ; we will update the status line after the move (the length is already new)
@advance:
        LD      R0, [head_i]            ; the new head
        INC     R0
        AND     R0, #BUF_MASK
        ST      [head_i], R0
        ADD     R0, R0
        ST      [R0+body], R5
        LD      R0, #BODY
        STB     [R5+grid], R0
        LD      R0, R5
        LD      R1, #'☻'
        LD      R2, #A_HEAD
        CALL    cell
        LD      R0, [head_i]            ; the former head is a body segment: a line by neighbors
        DEC     R0
        AND     R0, #BUF_MASK
        CALL    draw_seg
        TEST    R7, R7                  ; the tail moved: the new tail is the end of a line
        JZ      @tailok
        LD      R0, [tail_i]
        CALL    draw_seg
@tailok:
        TEST    R6, R6
        JZ      @alive
        CALL    status
@alive: LD      R0, #0
        JMP     @out
@dead:  LD      R0, #1
@out:   POP     R7
        POP     R6
        POP     R5
        POP     R4
        POP     IP

; draw_seg - draw the body segment with number R0 in the ring buffer (not the head).
; The character is a double line toward the neighboring segments: to the previous (if it is not the tail) and to the next.
draw_seg:
        PUSH    LR
        PUSH    R4
        PUSH    R5
        PUSH    R6
        LD      R4, R0                  ; segment number
        ADD     R0, R0
        LD      R5, [R0+body]           ; its position
        LD      R6, #0                  ; direction mask: L=1, R=2, U=4, D=8
        CMP     R4, [tail_i]
        JZ      @next                   ; the tail has no previous segment
        LD      R0, R4
        DEC     R0
        AND     R0, #BUF_MASK
        ADD     R0, R0
        LD      R0, [R0+body]
        SUB     R0, R5                  ; offset to the previous segment
        CALL    dirbit
        OR      R6, R0
@next:  LD      R0, R4
        INC     R0
        AND     R0, #BUF_MASK
        ADD     R0, R0
        LD      R0, [R0+body]
        SUB     R0, R5                  ; offset to the next segment
        CALL    dirbit
        OR      R6, R0
        LDZX    R1, [R6+seg_chars]
        LD      R0, R5
        LD      R2, #A_BODY
        CALL    cell
        POP     R6
        POP     R5
        POP     R4
        POP     IP

; dirbit - offset R0 (-1, +1, -80, +80) -> direction bit R0: left 1, right 2, up 4, down 8.
dirbit: CMP     R0, #-1
        JNZ     @r
        LD      R0, #1
        RET
@r:     CMP     R0, #1
        JNZ     @u
        LD      R0, #2
        RET
@u:     CMP     R0, #-COLS
        JNZ     @d
        LD      R0, #4
        RET
@d:     LD      R0, #8
        RET

; set_level - speed level R0 (1..9): delay = ticks per horizontal move.
set_level:
        ST      [level], R0
        LDZX    R0, [R0+speeds-1]
        ST      [delay], R0
        RET

; place_food - food in a random empty cell (if the field is almost full - in the first empty one).
place_food:
        PUSH    LR
        PUSH    R4
        PUSH    R5
        LD      R5, #200                ; random attempts
@try:   CALL    rand
        REMU    R0, #BOTTOM - TOP + 1
        ADD     R0, #TOP
        LD      R4, R0
        MUL     R4, #COLS               ; row * 80
        CALL    rand
        REMU    R0, #RIGHT - LEFT + 1
        ADD     R0, #LEFT
        ADD     R4, R0                  ; + column
        LDZX    R0, [R4+grid]
        TEST    R0, R0
        JZ      @put
        DEC     R5
        JNZ     @try
        LD      R4, #0                  ; random did not work: the first empty cell
@scan:  LDZX    R0, [R4+grid]
        TEST    R0, R0
        JZ      @put
        INC     R4
        CMP     R4, #COLS * 25
        JB      @scan
        JMP     @out                    ; there are no empty cells
@put:   LD      R0, #FOOD
        STB     [R4+grid], R0
        LD      R0, R4
        LD      R1, #'♥'
        LD      R2, #A_FOOD
        CALL    cell
@out:   POP     R5
        POP     R4
        POP     IP

; rand - a pseudo-random number 0..2047 (a linear congruential generator, high bits).
rand:   LD      R0, [seed]
        MUL     R0, #25173
        ADD     R0, #13849
        ST      [seed], R0
        SHR     R0, #5
        RET

; cell - character R1 with attribute R2 into the cell at position R0 (row * 80 + column).
cell:   STB     [CON_ATTR], R2
        LD      R3, R0
        DIVU    R3, #COLS
        STB     [CON_ROW], R3
        REMU    R0, #COLS
        STB     [CON_COL], R0
        STB     [CONSOLE], R1
        RET

; status - the status line: score, length, speed.
status: PUSH    LR
        LD      R0, #A_STATUS
        CALL    scr_attr
        LD      R0, #0
        LD      R1, #0
        CALL    scr_goto
        LD      R0, #CMD_CLEOL
        STB     [CON_CMD], R0
        LD      R0, #1
        LD      R1, #0
        LD      R2, #msg_status
        CALL    scr_print_at
        LD      R0, #16
        LD      R1, #0
        CALL    scr_goto
        LD      R0, [score]
        CALL    print_u16
        LD      R0, #31
        LD      R1, #0
        CALL    scr_goto
        LD      R0, [head_i]            ; length = head - tail + 1 (modulo the buffer)
        SUB     R0, [tail_i]
        AND     R0, #BUF_MASK
        INC     R0
        CALL    print_u16
        LD      R0, #43
        LD      R1, #0
        CALL    scr_goto
        LD      R0, [level]             ; speed level 1..9
        CALL    print_u16
        LD      R0, #HINT_COL
        LD      R1, #0
        LD      R2, #msg_hint
        CALL    scr_print_at
        POP     IP

; show_pause - the pause hint. R5 is 1 on pause (play's register, only read).
show_pause:
        PUSH    LR
        LD      R0, #A_STATUS | INVERSE
        CALL    scr_attr
        LD      R2, #msg_paused
        TEST    R5, R5
        JNZ     @show
        LD      R0, #A_STATUS
        CALL    scr_attr
        LD      R2, #msg_hint
@show:  LD      R0, #HINT_COL
        LD      R1, #0
        CALL    scr_print_at
        POP     IP

; ----------------------------------------------------------------------------
; game_over - a message and waiting: R0 = 0 - Enter (a new game), 1 - Esc.
; ----------------------------------------------------------------------------
game_over:
        PUSH    LR
        PUSH    R4
        LD      R0, [score]             ; best score
        CMP     R0, [best]
        JBE     @box
        ST      [best], R0
@box:   LD      R0, #A_BOX
        CALL    scr_attr
        LD      R0, #22                 ; the frame: columns 22..57, rows 9..15
        LD      R1, #9
        LD      R2, #36
        LD      R3, #7
        CALL    scr_box
        LD      R4, #10                 ; the inside of the frame - in the message background
@fill:  LD      R0, #23                 ; (the counter is in R4: scr_print_at clobbers R0..R3)
        LD      R1, R4
        LD      R2, #msg_blank
        CALL    scr_print_at
        INC     R4
        CMP     R4, #15
        JB      @fill
        LD      R0, #A_BOX | INVERSE
        CALL    scr_attr
        LD      R0, #34
        LD      R1, #10
        LD      R2, #msg_over
        CALL    scr_print_at
        LD      R0, #A_BOX
        CALL    scr_attr
        LD      R0, #33
        LD      R1, #12
        LD      R2, #msg_score
        CALL    scr_print_at
        LD      R0, [score]
        CALL    print_u16
        LD      R0, #25
        LD      R1, #14
        LD      R2, #msg_again
        CALL    scr_print_at
@wait:  CALL    wait_key
        CALL    is_esc
        TEST    R0, R0
        JNZ     @out                    ; R0 = 1 - exit
        LD      R0, R1
        AND     R0, #$FF
        CMP     R0, #10                 ; Enter
        JNZ     @wait
        LD      R0, #0
@out:   POP     R4
        POP     IP

; wait_key - wait for a key, waiting in WFI (the timer wakes it every 20 ms). R0 is the key code.
wait_key:
        PUSH    LR
@w:     WFI
        CALL    getkey_nb
        JZ      @w
        POP     IP

; is_esc - R0 = 1 if key code R0 is Esc (by scancode or character), otherwise 0.
; The key code stays in R1.
is_esc: LD      R1, R0
        LD      R2, R0
        SHR     R2, #8
        CMP     R2, #SC_ESC
        JZ      @yes
        AND     R0, #$FF
        CMP     R0, #27
        JZ      @yes
        LD      R0, #0
        RET
@yes:   LD      R0, #1
        RET

; ============================================================================
; Constants
; ============================================================================
msg_name:   .asciz  "☻█████ S N A K E"
msg_help1:  .asciz  "Arrows - turn, 1..9 - speed"
msg_help2:  .asciz  "Space - pause; eat ♥ to grow"
msg_start:  .asciz  "Press any key (Esc quits)"
msg_status: .asciz  "SNAKE   Score:        Length:      Speed:"
msg_hint:   .asciz  "←↑→↓ 1-9 speed Space pause Esc quit"
msg_paused: .asciz  "PAUSED: press Space to continue    "
msg_over:   .asciz  " GAME OVER "
msg_score:  .asciz  "Your score: "
msg_again:  .asciz  "Enter - play again, Esc - quit"
msg_blank:  .asciz  "                                  "   ; 34 spaces
msg_bye:    .asciz  "Thanks for playing! Best score: "
msg_fault:  .asciz  "\nfault\n"
; ticks per horizontal move for levels 1..9 (a vertical one is twice as many)
speeds:     .byte   12, 10, 8, 7, 6, 5, 4, 3, 2
; the segment character by the mask of directions to neighbors (L=1, R=2, U=4, D=8)
seg_chars:  .byte   '█', '═', '═', '═', '║', '╝', '╚', '█'     ; 0..7
            .byte   '║', '╗', '╔', '█', '║', '█', '█', '█'     ; 8..15

        .include "../lib/stdlib.asm"

; ============================================================================
; RAM
; ============================================================================
        .org    RAM_START
grid:       .space  COLS * 25           ; the field map: EMPTY, WALL, BODY, FOOD
body:       .space  2 * (BUF_MASK + 1)  ; body positions (the ring buffer)
ticks:      .space  2
best:       .space  2
seed:       .space  2
head_i:     .space  2
tail_i:     .space  2
dir:        .space  2                   ; the moves: +1, -1, +80, -80
nextdir:    .space  2                   ; direction by the last arrow
delay:      .space  2                   ; ticks per horizontal move (by level)
level:      .space  2                   ; speed level 1..9
base_level: .space  2                   ; the level the game starts from
score:      .space  2
eaten:      .space  2
