; mod.asm - an assembly module with its own initialized data.
asm_add:
        ADD     R0, R1
        RET

; uint asm_table(uint i): the i-th word of the table
asm_table:
        SHL     R0, #1
        LD      R1, #table
        ADD     R0, R1
        LD      R0, [R0]
        RET

; uint asm_count(): a counter in the module's data, incremented on every call
asm_count:
        LD      R0, [counter]
        ADD     R0, #1
        ST      [counter], R0
        RET

table:  .word   1000, 2000, 3000
counter: .word  41
