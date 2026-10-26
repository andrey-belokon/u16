; tetris.asm - "Tetris" on the 80x25 display.
; ← → - move, ↑ - rotate, ↓ - drop faster, space - hard drop, P - pause, Esc - exit;
; after the game ends, Enter - a new game.
; Demo: on the title screen D - or automatically if nothing is pressed for 30 seconds. In the demo the program plays;
; any key - back to the title screen.
;
; What the example shows:
;   - pieces as 4x4-bit masks in a word: the "does it fit" check is a loop over 16 bits;
;   - the field in memory (the screen cannot be read), removal of full rows;
;   - a field cell is two character cells, so it looks square (a character cell is twice as tall as wide);
;   - falling by ticks of the 50 Hz timer; the level rises every 10 lines, falling speeds up;
;   - a "ghost" - where the piece will land after a hard drop;
;   - a "bag" of 7 shuffled pieces (a random number generator on MUL);
;   - a 32-bit score: ADD/ADC and print_u32;
;   - a player program (demo): trying every rotation and position of the piece and evaluating the field -
;     full lines, height, holes, bumpiness (ai_plan, evaluate).
;
; In the web debugger: build (Ctrl+B), open the display with the "Display" button, run (F9),
; click the display and play.

        .equ    VECTORS,    $FF00
        .equ    RAM_START,  $8000
        .equ    W,          10          ; field: 10 x 20 cells
        .equ    H,          20
        .equ    FIELD_COL,  30          ; screen column of the field's left cell (a cell is 2 character cells)
        .equ    FIELD_ROW,  2           ; screen row of the field's top cell
        .equ    PREV_COL,   56          ; the "next piece" window
        .equ    PREV_ROW,   4
        .equ    STAT_COL,   55          ; score, lines, level

        .equ    A_TEXT,     BLACK << 3 | WHITE
        .equ    A_DIM,      BLACK << 3 | GRAY
        .equ    A_FRAME,    BLACK << 3 | CYAN
        .equ    A_TITLE,    BLACK << 3 | YELLOW
        .equ    A_MSG,      RED << 3 | WHITE

        .equ    DEMO_IDLE,  1500        ; the demo starts by itself after 30 s (20 ms ticks) without key presses
        .equ    AI_DELAY,   16           ; demo: one action every 16 ticks (320 ms), like a human
        .equ    AI_MAX_MOVES, 16        ; demo: this many actions, then drop in any case

        .equ    SHOW_ERASE, 0           ; draw_cur modes
        .equ    SHOW_PIECE, 1
        .equ    SHOW_GHOST, 2

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
        ST      [best + 2], R0
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
; Main program
; ============================================================================
main:   PUSH    LR
        LD      R0, #0                  ; coordinate mode, cursor hidden
        CALL    scr_mode
@title: CALL    title                   ; R0: 0 - game, 1 - demo, 2 - exit
        CMP     R0, #2
        JZ      @quit
        CMP     R0, #1
        JNZ     @game
        CALL    play                    ; demo (R0 = 1)
        CMP     R0, #2
        JZ      @title                  ; a key was pressed - to the title screen
        CALL    demo_over
        JMP     @title
@game:  LD      R0, #0
        CALL    play
        TEST    R0, R0                  ; 1 - exit by Esc during the game
        JZ      @over
        CALL    update_best             ; the score of an interrupted game also counts toward the record
        JMP     @quit
@over:
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
        LD      R1, [best + 2]
        CALL    print_u32
        CALL    newline
        POP     IP

; title - the title screen. Result: R0 = 0 - an ordinary game, 1 - the demo (the D key or 30 s without key presses),
; 2 - exit (Esc). The generator seed is the moment of the key press.
title:  PUSH    LR
        PUSH    R4
        CALL    draw_frame
        LD      R0, #msg_press
        CALL    field_message
        LD      R0, #FIELD_COL
        LD      R1, #FIELD_ROW + 11
        LD      R2, #msg_demo_key
        CALL    scr_print_at
        LD      R4, [ticks]             ; start of waiting
@wait:  WFI
        LD      R0, [ticks]
        SUB     R0, R4
        CMP     R0, #DEMO_IDLE
        JAE     @demo                   ; 30 s without key presses
        CALL    getkey_nb
        JZ      @wait
        LD      R1, [seed]              ; seed: the moment of the key press and the key code
        ADD     R1, [ticks]
        XOR     R1, R0
        ST      [seed], R1
        CALL    is_esc
        TEST    R0, R0
        JNZ     @quit
        LD      R0, R1
        AND     R0, #$FF
        CMP     R0, #'d'
        JZ      @demo
        CMP     R0, #'D'
        JZ      @demo
        LD      R0, #0                  ; an ordinary game
        JMP     @out
@demo:  LD      R0, [seed]              ; each demo gets its own game
        ADD     R0, [ticks]
        ST      [seed], R0
        LD      R0, #1
        JMP     @out
@quit:  LD      R0, #2
@out:   POP     R4
        POP     IP

; demo_over - the end of a demo game: a message for 2 seconds (or until a key is pressed).
demo_over:
        PUSH    LR
        PUSH    R4
        LD      R0, #msg_demo_over
        CALL    field_message
        LD      R4, [ticks]
@wait:  WFI
        CALL    getkey_nb
        JNZ     @out
        LD      R0, [ticks]
        SUB     R0, R4
        CMP     R0, #100
        JB      @wait
@out:   POP     R4
        POP     IP

; draw_frame - the screen: the field frame, the next-piece window, labels, a hint.
draw_frame:
        PUSH    LR
        PUSH    R4
        LD      R0, #A_TEXT
        CALL    scr_attr
        CALL    scr_clear
        LD      R0, #A_FRAME
        CALL    scr_attr
        LD      R0, #FIELD_COL - 1      ; field: inside - 20 columns and 20 rows
        LD      R1, #FIELD_ROW - 1
        LD      R2, #2 * W + 2
        LD      R3, #H + 2
        CALL    scr_box
        LD      R0, #PREV_COL - 2
        LD      R1, #PREV_ROW - 2
        LD      R2, #12
        LD      R3, #7
        CALL    scr_box
        LD      R0, #A_TITLE
        CALL    scr_attr
        LD      R0, #PREV_COL + 2
        LD      R1, #PREV_ROW - 2
        LD      R2, #msg_next
        CALL    scr_print_at
        LD      R0, #6
        LD      R1, #2
        LD      R2, #msg_title
        CALL    scr_print_at
        LD      R0, #A_DIM              ; labels on the right and a hint on the left
        CALL    scr_attr
        LD      R0, #STAT_COL
        LD      R1, #11
        LD      R2, #msg_score
        CALL    scr_print_at
        LD      R0, #STAT_COL
        LD      R1, #13
        LD      R2, #msg_lines
        CALL    scr_print_at
        LD      R0, #STAT_COL
        LD      R1, #15
        LD      R2, #msg_level
        CALL    scr_print_at
        LD      R0, #STAT_COL
        LD      R1, #17
        LD      R2, #msg_best
        CALL    scr_print_at
        LD      R4, #0
@help:  LD      R0, #3
        LD      R1, R4
        ADD     R1, #5
        LD      R2, R4
        ADD     R2, R2
        LD      R2, [R2+help_lines]
        CALL    scr_print_at
        INC     R4
        CMP     R4, #HELP_COUNT
        JB      @help
        POP     R4
        POP     IP

; ============================================================================
; play - one game. R0 = 1 - demo (the program plays), 0 - a human plays.
; Result: R0 = 0 - game over, 1 - exit by Esc, 2 - the demo was interrupted by a key press.
; R4 is the tick of the last fall step, R5 is 1 on pause, R6 the tick of the last action in the demo.
; ============================================================================
play:   PUSH    LR
        PUSH    R4
        PUSH    R5
        PUSH    R6
        ST      [demo], R0
        CALL    demo_label
        LD      R0, #board              ; an empty field
        LD      R1, #0
        LD      R2, #W * H
        CALL    memset
        LD      R0, #0
        ST      [score], R0
        ST      [score + 2], R0
        ST      [lines], R0
        LD      R0, #7                  ; the bag is empty: it will be filled at the first piece
        ST      [bag_pos], R0
        LD      R0, #1
        ST      [level], R0
        LDZX    R0, [drop_ticks]        ; the fall delay of level 1
        ST      [drop_delay], R0
        CALL    redraw_board
        CALL    bag_next
        ST      [next], R0
        CALL    spawn
        CALL    stats
        LD      R4, [ticks]
        LD      R5, #0
        LD      R6, R4
@loop:  WFI
@keys:  CALL    getkey_nb
        JZ      @ai
        LD      R1, [demo]              ; in the demo any key goes back to the title screen
        TEST    R1, R1
        JNZ     @stopdemo
        CALL    is_esc
        TEST    R0, R0
        JNZ     @quit
        LD      R0, R1                  ; key code
        AND     R0, #$FF
        CMP     R0, #'p'                ; P - pause
        JZ      @pause
        CMP     R0, #'P'
        JZ      @pause
        TEST    R5, R5
        JNZ     @keys                   ; on pause the other keys do nothing
        CMP     R0, #' '                ; space - hard drop
        JZ      @hard
        SHR     R1, #8                  ; scancode: arrows
        CMP     R1, #SC_LEFT
        JZ      @left
        CMP     R1, #SC_RIGHT
        JZ      @right
        CMP     R1, #SC_UP
        JZ      @rot
        CMP     R1, #SC_DOWN
        JNZ     @keys
        LD      R0, #0                  ; down one row, +1 point
        LD      R1, #1
        CALL    try_move
        TEST    R0, R0
        JZ      @land
        LD      R0, #1
        CALL    add_score
        LD      R4, [ticks]             ; the fall countdown starts over
        JMP     @keys
@left:  LD      R0, #-1
        LD      R1, #0
        CALL    try_move
        JMP     @keys
@right: LD      R0, #1
        LD      R1, #0
        CALL    try_move
        JMP     @keys
@rot:   CALL    rotate
        JMP     @keys
@hard:  CALL    ghost_y                 ; straight to the ghost's place: +2 points per row
        SUB     R0, [cur_y]             ; how many rows it will fly
        JZ      @land                   ; already at the bottom
        PUSH    R0                      ; try_move clobbers R0..R3
        LD      R1, R0
        LD      R0, #0
        CALL    try_move                ; the ghost is a valid place, the shift always succeeds
        POP     R0
        ADD     R0, R0
        CALL    add_score
        JMP     @land
@pause: XOR     R5, #1
        CALL    show_pause
        JMP     @keys
@ai:    LD      R0, [demo]              ; demo: the program moves once every AI_DELAY ticks
        TEST    R0, R0
        JZ      @fall
        LD      R0, [ticks]
        SUB     R0, R6
        CMP     R0, #AI_DELAY
        JB      @fall
        LD      R6, [ticks]
        CALL    ai_step                 ; R0 = 1 - time to drop
        TEST    R0, R0
        JNZ     @hard
@fall:  TEST    R5, R5
        JNZ     @loop
        LD      R0, [ticks]             ; time to fall?
        SUB     R0, R4
        CMP     R0, [drop_delay]
        JB      @loop
        LD      R4, [ticks]
        LD      R0, #0
        LD      R1, #1
        CALL    try_move
        TEST    R0, R0
        JNZ     @loop
@land:  CALL    lock_piece              ; it does not fall further: lock it, remove rows, next piece
        TEST    R0, R0
        JNZ     @over
        LD      R0, #0                  ; demo: a new plan for a new piece
        ST      [ai_ready], R0
        CALL    spawn
        TEST    R0, R0
        JZ      @over
        LD      R4, [ticks]
        CALL    stats
        JMP     @loop
@over:  LD      R0, #0
        JMP     @out
@stopdemo:
        LD      R0, #2
        JMP     @out
@quit:  LD      R0, #1
@out:   POP     R6
        POP     R5
        POP     R4
        POP     IP

; demo_label - the "DEMO" label on the left (in the demo) or blank.
demo_label:
        PUSH    LR
        LD      R0, #A_TITLE | INVERSE
        CALL    scr_attr
        LD      R2, #msg_demo_on
        LD      R0, [demo]
        TEST    R0, R0
        JNZ     @show
        LD      R0, #A_TEXT
        CALL    scr_attr
        LD      R2, #msg_demo_off
@show:  LD      R0, #3
        LD      R1, #15
        CALL    scr_print_at
        LD      R0, #0                  ; the plan for the first piece has not been made yet
        ST      [ai_ready], R0
        POP     IP

; show_pause - the pause message in the field (R5 is 1 on pause, play's register) or the field back.
show_pause:
        PUSH    LR
        TEST    R5, R5
        JZ      @resume
        LD      R0, #msg_paused
        CALL    field_message
        POP     IP
@resume:
        CALL    redraw_board
        LD      R0, #SHOW_GHOST
        CALL    show_cur
        POP     IP

; ============================================================================
; The piece
; ============================================================================

; spawn - the next piece at the top of the field. Result: R0 = 1 - it fit, 0 - game over.
spawn:  PUSH    LR
        LD      R0, [next]
        ST      [cur_type], R0
        CALL    bag_next
        ST      [next], R0
        LD      R0, #0
        ST      [cur_rot], R0
        ST      [cur_y], R0
        LD      R0, #3
        ST      [cur_x], R0
        CALL    draw_next
        LD      R0, [cur_type]
        LD      R1, [cur_rot]
        LD      R2, [cur_x]
        LD      R3, [cur_y]
        CALL    fits
        TEST    R0, R0
        JZ      @out                    ; it did not fit - game over (R0 = 0)
        LD      R0, #SHOW_GHOST
        CALL    show_cur
        LD      R0, #1
@out:   POP     IP

; try_move - shift the piece by R0 horizontally and R1 vertically. Result: R0 = 1 - it moved.
try_move:
        PUSH    LR
        PUSH    R4
        PUSH    R5
        LD      R4, [cur_x]
        ADD     R4, R0
        LD      R5, [cur_y]
        ADD     R5, R1
        LD      R0, [cur_type]
        LD      R1, [cur_rot]
        LD      R2, R4
        LD      R3, R5
        CALL    fits
        TEST    R0, R0
        JZ      @out
        LD      R0, #SHOW_ERASE
        CALL    show_cur
        ST      [cur_x], R4
        ST      [cur_y], R5
        LD      R0, #SHOW_GHOST
        CALL    show_cur
        LD      R0, #1
@out:   POP     R5
        POP     R4
        POP     IP

; rotate - rotation clockwise; at a wall it tries to shift the piece by 1-2 cells.
rotate: PUSH    LR
        PUSH    R4
        PUSH    R5
        LD      R4, [cur_rot]
        INC     R4
        AND     R4, #3                  ; the new rotation
        LD      R5, #0                  ; the shift attempt number
@try:   LD      R0, [cur_type]
        LD      R1, R4
        LDSX    R2, [R5+kicks]          ; shift: 0, -1, +1, -2, +2
        ADD     R2, [cur_x]
        LD      R3, [cur_y]
        CALL    fits
        TEST    R0, R0
        JNZ     @ok
        INC     R5
        CMP     R5, #5
        JB      @try
        JMP     @out                    ; it does not fit anywhere
@ok:    LD      R0, #SHOW_ERASE
        CALL    show_cur
        ST      [cur_rot], R4
        LDSX    R0, [R5+kicks]
        ADD     R0, [cur_x]
        ST      [cur_x], R0
        LD      R0, #SHOW_GHOST
        CALL    show_cur
@out:   POP     R5
        POP     R4
        POP     IP

; fits - does piece R0 in rotation R1 fit with the top left corner of the 4x4 frame at (R2, R3)?
; Coordinates are signed; above the field (row < 0) is allowed. Result: R0 = 1 - yes, 0 - no.
fits:   PUSH    R4
        PUSH    R5
        PUSH    R6
        PUSH    R7
        LD      R4, R0                  ; mask: shapes[type * 4 + rotation]
        SHL     R4, #2
        ADD     R4, R1
        ADD     R4, R4
        LD      R4, [R4+shapes]
        LD      R5, #0                  ; bit number 0..15: row = R5 / 4, column = R5 mod 4
@bit:   TEST    R4, #$8000
        JZ      @next
        LD      R6, R5                  ; field column
        AND     R6, #3
        ADD     R6, R2
        CMP     R6, #0
        JL      @no
        CMP     R6, #W
        JGE     @no
        LD      R7, R5                  ; field row
        SHR     R7, #2
        ADD     R7, R3
        CMP     R7, #H
        JGE     @no
        CMP     R7, #0
        JL      @next                   ; above the field it does not interfere
        MUL     R7, #W
        ADD     R7, R6
        LDZX    R7, [R7+board]
        TEST    R7, R7
        JNZ     @no
@next:  SHL     R4, #1
        INC     R5
        CMP     R5, #16
        JB      @bit
        LD      R0, #1
        JMP     @out
@no:    LD      R0, #0
@out:   POP     R7
        POP     R6
        POP     R5
        POP     R4
        RET

; show_cur - draw or erase the current piece and its ghost.
; R0 = SHOW_ERASE - erase the piece and the ghost; SHOW_GHOST - draw the ghost, then the piece.
show_cur:
        PUSH    LR
        PUSH    R4
        LD      R4, R0
        CALL    ghost_y                 ; R0 is the ghost row
        LD      R1, R0
        LD      R0, #SHOW_ERASE
        CMP     R4, #SHOW_ERASE
        JZ      @ghost
        LD      R0, #SHOW_GHOST
@ghost: CALL    draw_cur                ; ghost
        LD      R0, #SHOW_ERASE
        CMP     R4, #SHOW_ERASE
        JZ      @piece
        LD      R0, #SHOW_PIECE
@piece: LD      R1, [cur_y]
        CALL    draw_cur                ; the piece over the ghost
        POP     R4
        POP     IP

; ghost_y - the row where the piece will stop on a hard drop. Result: R0.
ghost_y:
        PUSH    LR
        PUSH    R4
        LD      R4, [cur_y]
@down:  LD      R0, [cur_type]
        LD      R1, [cur_rot]
        LD      R2, [cur_x]
        LD      R3, R4
        INC     R3
        CALL    fits
        TEST    R0, R0
        JZ      @done
        INC     R4
        JMP     @down
@done:  LD      R0, R4
        POP     R4
        POP     IP

; draw_cur - the cells of the current piece in row R1 (column - cur_x).
; R0 is the mode: SHOW_ERASE (empty), SHOW_PIECE (the piece's color), SHOW_GHOST (ghost).
draw_cur:
        PUSH    LR
        PUSH    R4
        PUSH    R5
        PUSH    R6
        PUSH    R7
        LD      R6, R0                  ; mode
        LD      R7, R1                  ; row of the 4x4 frame
        LD      R4, [cur_type]
        LD      R0, R4
        SHL     R4, #2
        ADD     R4, [cur_rot]
        ADD     R4, R4
        LD      R4, [R4+shapes]         ; mask
        LDZX    R0, [R0+colors]         ; piece color
        ST      [draw_color], R0
        LD      R5, #0
@bit:   TEST    R4, #$8000
        JZ      @next
        LD      R1, R5                  ; cell row
        SHR     R1, #2
        ADD     R1, R7
        CMP     R1, #0
        JL      @next                   ; we do not draw above the field
        LD      R0, R5                  ; cell column
        AND     R0, #3
        ADD     R0, [cur_x]
        LD      R2, #0                  ; empty
        CMP     R6, #SHOW_ERASE
        JZ      @put
        LD      R2, #GHOST              ; ghost
        CMP     R6, #SHOW_GHOST
        JZ      @put
        LD      R2, [draw_color]
@put:   CALL    cell
@next:  SHL     R4, #1
        INC     R5
        CMP     R5, #16
        JB      @bit
        POP     R7
        POP     R6
        POP     R5
        POP     R4
        POP     IP

; cell - a field cell: R0 is the column 0..9, R1 the row 0..19, R2 the contents:
; 0 - empty, GHOST - ghost, otherwise the block color.
        .equ    GHOST,  $FF
cell:   ADD     R0, R0                  ; a cell is 2 character cells
        ADD     R0, #FIELD_COL
        STB     [CON_COL], R0
        ADD     R1, #FIELD_ROW
        STB     [CON_ROW], R1
        TEST    R2, R2
        JZ      @empty
        CMP     R2, #GHOST
        JZ      @ghost
        STB     [CON_ATTR], R2          ; color on a black background
        LD      R0, #'█'
        JMP     @two
@ghost: LD      R0, #A_DIM
        STB     [CON_ATTR], R0
        LD      R0, #'░'
        JMP     @two
@empty: LD      R0, #A_DIM
        STB     [CON_ATTR], R0
        LD      R0, #' '
@two:   STB     [CONSOLE], R0
        STB     [CONSOLE], R0
        RET

; ============================================================================
; Demo: the player program
; ============================================================================

; ai_step - one action of the program with the current piece: make a plan, rotate, shift.
; Result: R0 = 1 - the piece is in place, time to drop; 0 - continue.
ai_step:
        PUSH    LR
        LD      R0, [ai_ready]
        TEST    R0, R0
        JNZ     @act
        CALL    ai_plan                 ; a new piece: choose the rotation and column
        LD      R0, #1
        ST      [ai_ready], R0
        LD      R0, #0
        ST      [ai_moves], R0
        JMP     @wait
@act:   LD      R0, [ai_moves]          ; just in case: no more than AI_MAX_MOVES actions
        INC     R0
        ST      [ai_moves], R0
        CMP     R0, #AI_MAX_MOVES
        JA      @drop
        LD      R0, [cur_rot]
        CMP     R0, [ai_rot]
        JZ      @move
        CALL    rotate
        JMP     @wait
@move:  LD      R1, [cur_x]             ; signed comparison: the column can be negative
        CMP     R1, [ai_x]
        JZ      @drop                   ; in place
        LD      R0, #1                  ; right (CMP flags, LD does not change them)
        JL      @go
        LD      R0, #-1                 ; left
@go:    LD      R1, #0
        CALL    try_move
        TEST    R0, R0
        JZ      @drop                   ; the field interferes - drop as is
@wait:  LD      R0, #0
        POP     IP
@drop:  LD      R0, #1
        POP     IP

; ai_plan - for the current piece, try the rotations and columns: drop, evaluate the field (evaluate),
; remember the best variant in ai_rot, ai_x. R4 is the rotation, R5 the column, R6 the fall row.
ai_plan:
        PUSH    LR
        PUSH    R4
        PUSH    R5
        PUSH    R6
        LD      R0, #-32768             ; the best score so far is the worst possible
        ST      [ai_best], R0
        LD      R0, [cur_rot]
        ST      [ai_rot], R0
        LD      R0, [cur_x]
        ST      [ai_x], R0
        LD      R4, #0
@rot:   LD      R5, #-2
@col:   LD      R0, [cur_type]          ; does it fit at the current height?
        LD      R1, R4
        LD      R2, R5
        LD      R3, [cur_y]
        CALL    fits
        TEST    R0, R0
        JZ      @nextcol
        LD      R6, [cur_y]             ; drop: lower it while it fits
@fall:  LD      R0, [cur_type]
        LD      R1, R4
        LD      R2, R5
        LD      R3, R6
        INC     R3
        CALL    fits
        TEST    R0, R0
        JZ      @land
        INC     R6
        JMP     @fall
@land:  LD      R0, #8                  ; temporarily put it on the field (8 - "occupied")
        CALL    ai_mark
        CALL    evaluate                ; R0 is the field score
        PUSH    R0
        LD      R0, #0                  ; and remove it
        CALL    ai_mark
        POP     R0
        CMP     R0, [ai_best]           ; better? (signed)
        JLE     @nextcol
        ST      [ai_best], R0
        ST      [ai_rot], R4
        ST      [ai_x], R5
@nextcol:
        INC     R5
        CMP     R5, #W
        JL      @col
        INC     R4
        CMP     R4, #4
        JB      @rot
        POP     R6
        POP     R5
        POP     R4
        POP     IP

; ai_mark - write R0 into the field cells under piece cur_type in rotation R4 at column R5
; and row R6 (cells above the field are skipped). R4..R6 are from ai_plan, unchanged.
ai_mark:
        PUSH    R7
        PUSH    R8
        LD      R7, [cur_type]
        SHL     R7, #2
        ADD     R7, R4
        ADD     R7, R7
        LD      R7, [R7+shapes]         ; mask
        LD      R8, #0                  ; bit number
@bit:   TEST    R7, #$8000
        JZ      @next
        LD      R1, R8                  ; row
        SHR     R1, #2
        ADD     R1, R6
        CMP     R1, #0
        JL      @next
        MUL     R1, #W
        LD      R2, R8                  ; column
        AND     R2, #3
        ADD     R2, R5
        ADD     R1, R2
        STB     [R1+board], R0
@next:  SHL     R7, #1
        INC     R8
        CMP     R8, #16
        JB      @bit
        POP     R8
        POP     R7
        RET

; evaluate - the field score for the demo (more is better):
;   114 * full rows - 26 * sum of column heights - 36 * holes - 9 * bumpiness.
; A hole is an empty cell under an occupied one; bumpiness is the sum of height differences of adjacent columns.
; All quantities are small, the score fits in a signed 16 bits. Result: R0.
evaluate:
        PUSH    R4
        PUSH    R5
        PUSH    R6
        PUSH    R7
        PUSH    R8
        PUSH    R9
        PUSH    R10
        PUSH    R11
        LD      R9, #0                  ; sum of heights
        LD      R10, #0                 ; holes
        LD      R11, #0                 ; bumpiness
        LD      R4, #0                  ; column
@colm:  LD      R6, R4                  ; cell (row 0, column R4)
        LD      R5, #0                  ; row
        LD      R7, #0                  ; column height
@cell:  LDZX    R0, [R6+board]
        TEST    R0, R0
        JZ      @empty
        TEST    R7, R7
        JNZ     @down
        LD      R7, #H                  ; the first occupied from the top: height = H - row
        SUB     R7, R5
        JMP     @down
@empty: TEST    R7, R7
        JZ      @down
        INC     R10                     ; empty under an occupied one - a hole
@down:  ADD     R6, #W
        INC     R5
        CMP     R5, #H
        JB      @cell
        ADD     R9, R7
        TEST    R4, R4                  ; bumpiness: |height - height on the left|
        JZ      @first
        LD      R0, R7
        SUB     R0, R8
        JGE     @abs
        NEG     R0
@abs:   ADD     R11, R0
@first: LD      R8, R7                  ; this column's height - for the next one
        INC     R4
        CMP     R4, #W
        JB      @colm
        LD      R1, #0                  ; full rows
        LD      R6, #0                  ; cell
        LD      R5, #H
@row:   LD      R2, #W                  ; R2 is how many cells of the row are still unoccupied
        LD      R3, #W
@rcell: LDZX    R0, [R6+board]
        TEST    R0, R0
        JZ      @hole
        DEC     R2
@hole:  INC     R6
        DEC     R3
        JNZ     @rcell
        TEST    R2, R2
        JNZ     @notfull
        INC     R1
@notfull:
        DEC     R5
        JNZ     @row
        LD      R0, R1                  ; score
        MUL     R0, #114
        MUL     R9, #26
        SUB     R0, R9
        MUL     R10, #36
        SUB     R0, R10
        MUL     R11, #9
        SUB     R0, R11
        POP     R11
        POP     R10
        POP     R9
        POP     R8
        POP     R7
        POP     R6
        POP     R5
        POP     R4
        RET

; ============================================================================
; Field
; ============================================================================

; lock_piece - put the piece on the field, remove full rows, award points.
; Result: R0 = 1 - the piece landed above the field (game over), 0 - continue.
lock_piece:
        PUSH    LR
        PUSH    R4
        PUSH    R5
        PUSH    R6
        PUSH    R7
        LD      R4, [cur_type]
        LDZX    R6, [R4+colors]         ; color
        SHL     R4, #2
        ADD     R4, [cur_rot]
        ADD     R4, R4
        LD      R4, [R4+shapes]         ; mask
        LD      R5, #0
        LD      R7, #0                  ; 1 - part of the piece is above the field
@bit:   TEST    R4, #$8000
        JZ      @next
        LD      R1, R5
        SHR     R1, #2
        ADD     R1, [cur_y]
        CMP     R1, #0
        JGE     @in
        LD      R7, #1
        JMP     @next
@in:    MUL     R1, #W
        LD      R0, R5
        AND     R0, #3
        ADD     R0, [cur_x]
        ADD     R1, R0
        STB     [R1+board], R6
@next:  SHL     R4, #1
        INC     R5
        CMP     R5, #16
        JB      @bit
        LD      R0, R7
        TEST    R0, R0
        JNZ     @out
        CALL    clear_lines             ; R0 is how many rows were removed
        TEST    R0, R0
        JZ      @done
        LD      R4, R0
        ADD     R0, [lines]             ; lines and level: every 10 lines
        ST      [lines], R0
        DIVU    R0, #10
        INC     R0
        CMP     R0, #LEVELS
        JBE     @lvl
        LD      R0, #LEVELS
@lvl:   ST      [level], R0
        LD      R0, R4                  ; points: line_points[n] * level
        ADD     R0, R0
        LD      R0, [R0+line_points]
        MUL     R0, [level]             ; no more than 800 * 10 - fits in a word
        CALL    add_score
        CALL    redraw_board
@done:  LD      R0, [level]             ; fall delay by level
        LDZX    R0, [R0+drop_ticks-1]
        ST      [drop_delay], R0
        LD      R0, #0
@out:   POP     R7
        POP     R6
        POP     R5
        POP     R4
        POP     IP

; clear_lines - remove full rows, shifting the upper ones down. Result: R0 is how many were removed.
clear_lines:
        PUSH    R4
        PUSH    R5
        PUSH    R6
        LD      R6, #0                  ; rows removed
        LD      R4, #H - 1              ; row, from the bottom up
@row:   LD      R5, R4                  ; is row R4 full?
        MUL     R5, #W
        LD      R0, #W
@cell:  LDZX    R1, [R5+board]
        TEST    R1, R1
        JZ      @notfull
        INC     R5
        DEC     R0
        JNZ     @cell
        INC     R6                      ; full: everything above goes one row down
        LD      R5, R4
        MUL     R5, #W
        ADD     R5, #W - 1              ; the last cell of the row
@shift: CMP     R5, #W
        JB      @top
        LDZX    R1, [R5+board-W]
        STB     [R5+board], R1
        DEC     R5
        JMP     @shift
@top:   LD      R1, #0                  ; the top row is empty
@clear: STB     [R5+board], R1
        DEC     R5
        CMP     R5, #$FFFF
        JNZ     @clear
        JMP     @row                    ; the same row once more (the upper one shifted into it)
@notfull:
        DEC     R4
        CMP     R4, #$FFFF
        JNZ     @row
        LD      R0, R6
        POP     R6
        POP     R5
        POP     R4
        RET

; redraw_board - redraw the whole field.
redraw_board:
        PUSH    LR
        PUSH    R4
        LD      R4, #0                  ; cell number 0..199
@cell:  LD      R1, R4
        DIVU    R1, #W
        LD      R0, R4
        REMU    R0, #W
        LDZX    R2, [R4+board]
        CALL    cell
        INC     R4
        CMP     R4, #W * H
        JB      @cell
        POP     R4
        POP     IP

; field_message - string R0 in the middle of the field (on the message frame).
field_message:
        PUSH    LR
        PUSH    R4
        LD      R4, R0
        LD      R0, #A_MSG
        CALL    scr_attr
        LD      R0, #FIELD_COL
        LD      R1, #FIELD_ROW + 8
        LD      R2, #msg_blank
        CALL    scr_print_at
        LD      R0, #FIELD_COL
        LD      R1, #FIELD_ROW + 9
        LD      R2, R4
        CALL    scr_print_at
        LD      R0, #FIELD_COL
        LD      R1, #FIELD_ROW + 10
        LD      R2, #msg_blank
        CALL    scr_print_at
        POP     R4
        POP     IP

; ============================================================================
; Score, the next piece, game over
; ============================================================================

; add_score - score += R0 (32 bits).
add_score:
        PUSH    LR
        LD      R1, [score]
        ADD     R1, R0
        ST      [score], R1
        LD      R1, [score + 2]
        ADC     R1, #0
        ST      [score + 2], R1
        CALL    stats
        POP     IP

; stats - score, lines, level, record.
stats:  PUSH    LR
        LD      R0, #A_TEXT
        CALL    scr_attr
        LD      R0, #STAT_COL
        LD      R1, #12
        CALL    scr_goto
        LD      R0, [score]
        LD      R1, [score + 2]
        CALL    print_u32
        LD      R0, #msg_pad            ; erase the remainder of the previous, longer number
        CALL    puts
        LD      R0, #STAT_COL
        LD      R1, #14
        CALL    scr_goto
        LD      R0, [lines]
        CALL    print_u16
        LD      R0, #msg_pad
        CALL    puts
        LD      R0, #STAT_COL
        LD      R1, #16
        CALL    scr_goto
        LD      R0, [level]
        CALL    print_u16
        LD      R0, #msg_pad
        CALL    puts
        LD      R0, #STAT_COL
        LD      R1, #18
        CALL    scr_goto
        LD      R0, [best]
        LD      R1, [best + 2]
        CALL    print_u32
        POP     IP

; draw_next - the next piece in the window on the right.
draw_next:
        PUSH    LR
        PUSH    R4
        PUSH    R5
        PUSH    R6
        LD      R4, [next]
        LDZX    R6, [R4+colors]
        SHL     R4, #3                  ; shapes[type * 4 + 0], words
        LD      R4, [R4+shapes]
        LD      R5, #0
@bit:   LD      R0, R5                  ; screen column: PREV_COL + 2 * (bit mod 4)
        AND     R0, #3
        ADD     R0, R0
        ADD     R0, #PREV_COL
        LD      R1, R5                  ; row: PREV_ROW + bit / 4
        SHR     R1, #2
        ADD     R1, #PREV_ROW
        CALL    scr_goto
        LD      R0, #A_DIM
        LD      R1, #' '
        TEST    R4, #$8000
        JZ      @put
        LD      R0, R6
        LD      R1, #'█'
@put:   STB     [CON_ATTR], R0
        STB     [CONSOLE], R1
        STB     [CONSOLE], R1
        SHL     R4, #1
        INC     R5
        CMP     R5, #16
        JB      @bit
        POP     R6
        POP     R5
        POP     R4
        POP     IP

; game_over - a message, the record; waiting: R0 = 0 - Enter (a new game), 1 - Esc.
game_over:
        PUSH    LR
        CALL    update_best
        CALL    stats
        LD      R0, #msg_over
        CALL    field_message
        LD      R0, #A_MSG
        CALL    scr_attr
        LD      R0, #FIELD_COL
        LD      R1, #FIELD_ROW + 11
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
@out:   POP     IP

; update_best - record = max(record, score); both are 32-bit.
update_best:
        PUSH    LR
        LD      R0, [score]
        LD      R1, [score + 2]
        LD      R2, [best]
        LD      R3, [best + 2]
        CALL    cmp32u
        JBE     @out
        LD      R0, [score]
        ST      [best], R0
        LD      R0, [score + 2]
        ST      [best + 2], R0
@out:   POP     IP

; ============================================================================
; Random pieces: a "bag" of 7 pieces in random order
; ============================================================================

; bag_next - the next piece from the bag (0..6). An empty bag is filled and shuffled.
bag_next:
        PUSH    LR
        PUSH    R4
        PUSH    R5
        LD      R0, [bag_pos]
        CMP     R0, #7
        JB      @take
        LD      R4, #0                  ; fill: 0, 1, ..., 6
@fill:  STB     [R4+bag], R4
        INC     R4
        CMP     R4, #7
        JB      @fill
        LD      R4, #6                  ; shuffle (Fisher-Yates): i = 6..1
@mix:   CALL    rand
        LD      R5, R4
        INC     R5
        REMU    R0, R5                  ; j = random 0..i
        LDZX    R1, [R4+bag]            ; swap bag[i] and bag[j]
        LD      R5, R0
        LDZX    R2, [R5+bag]
        STB     [R4+bag], R2
        STB     [R5+bag], R1
        DEC     R4
        JNZ     @mix
        LD      R0, #0
@take:  LDZX    R1, [R0+bag]
        INC     R0
        ST      [bag_pos], R0
        LD      R0, R1
        POP     R5
        POP     R4
        POP     IP

; rand - a pseudo-random number 0..2047 (a linear congruential generator, high bits).
rand:   LD      R0, [seed]
        MUL     R0, #25173
        ADD     R0, #13849
        ST      [seed], R0
        SHR     R0, #5
        RET

; ============================================================================
; Keys
; ============================================================================

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
; Data
; ============================================================================
; Pieces: 4 rotations clockwise, a 4x4 mask - rows from top to bottom, 4 bits each,
; the high bit is the top left corner. Order: I, O, T, S, Z, J, L.
shapes: .word   $0F00, $2222, $00F0, $4444      ; I
        .word   $6600, $6600, $6600, $6600      ; O
        .word   $4E00, $4640, $0E40, $4C40      ; T
        .word   $6C00, $4620, $06C0, $8C40      ; S
        .word   $C600, $2640, $0C60, $4C80      ; Z
        .word   $8E00, $6440, $0E20, $44C0      ; J
        .word   $2E00, $4460, $0E80, $C440      ; L
colors: .byte   CYAN, YELLOW, WHITE, GREEN, RED, BLUE, GRAY
kicks:  .byte   0, -1, 1, -2, 2                 ; shifts when rotating next to a wall
        .equ    LEVELS, 10
drop_ticks: .byte  40, 34, 28, 23, 19, 15, 12, 9, 7, 5   ; ticks per fall step, levels 1..10
        .align  2
line_points: .word 0, 100, 300, 500, 800        ; points for 1..4 lines (multiplied by the level)

msg_title:  .asciz  "T E T R I S"
msg_next:   .asciz  " NEXT "
msg_score:  .asciz  "Score"
msg_lines:  .asciz  "Lines"
msg_level:  .asciz  "Level"
msg_best:   .asciz  "Best"
msg_pad:    .asciz  "      "
msg_press:  .asciz  "  Press any key...  "
msg_demo_key: .asciz "   D - watch demo   "
msg_demo_over: .asciz " DEMO: GAME OVER    "
msg_demo_on:  .asciz " DEMO - press a key  "
msg_demo_off: .asciz "                     "
msg_paused: .asciz  "  PAUSED: press P   "
msg_over:   .asciz  "     GAME OVER      "
msg_again:  .asciz  "Enter again,Esc quit"
msg_blank:  .asciz  "                    "
msg_bye:    .asciz  "Thanks for playing! Best score: "
msg_fault:  .asciz  "\nfault\n"
help_lines: .word   help0, help1, help2, help3, help4, help5, help6
        .equ    HELP_COUNT, 7
help0:      .asciz  "← →     move"
help1:      .asciz  "↑       rotate"
help2:      .asciz  "↓       soft drop"
help3:      .asciz  "Space   hard drop"
help4:      .asciz  "P       pause"
help5:      .asciz  "Esc     quit"
help6:      .asciz  "Lines: 100-800 x level"

        .include "../lib/stdlib.asm"

; ============================================================================
; RAM
; ============================================================================
        .org    RAM_START
board:      .space  W * H               ; field: 0 - empty, otherwise the block color
bag:        .space  8                   ; piece bag
ticks:      .space  2
seed:       .space  2
score:      .space  4                   ; 32 bits: the low word, the high word
best:       .space  4
lines:      .space  2
level:      .space  2
drop_delay: .space  2
bag_pos:    .space  2
next:       .space  2
cur_type:   .space  2
cur_rot:    .space  2
cur_x:      .space  2                   ; signed: at the left wall the I piece can be at -2
cur_y:      .space  2
draw_color: .space  2
demo:       .space  2                   ; 1 - the program is playing
ai_ready:   .space  2                   ; demo: the plan for the current piece has been made
ai_rot:     .space  2                   ; demo: the chosen rotation
ai_x:       .space  2                   ; demo: the chosen column (signed)
ai_best:    .space  2                   ; demo: the best score (signed)
ai_moves:   .space  2                   ; demo: actions with the current piece
