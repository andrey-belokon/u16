# Minimalist 16-bit Processor Architecture — Revision 6

The instruction-word format is unchanged. For JR (introduced in revision 5), the R2 and R2MOD fields together form the branch displacement. For the combined MISC opcode (revision 6), the R2 field selects the operation. Revision 4 changed the flag layout: IF and OF exchanged positions (Section 2). All behavior left unspecified in the original description is defined below. Section 12 summarizes changes and additions.

## 1. General Rules

- Word size is 16 bits. The byte-addressed memory space is 64 KiB, with address arithmetic modulo 2^16.
- There are 16 word-sized registers, R0..R15. R0..R11 are equivalent general-purpose registers (R0 is not hardwired to zero).
- Unless otherwise stated, operations are word-sized.
- Instructions occupy one or two words. Instructions and data are word-aligned.
- For word accesses, address bit 0 is ignored. Byte operations may access any address.
- Byte order is little-endian: the low byte of a word is stored at even address A, and the high byte at A+1.
- Bit 0 of IP is always zero; writes to IP clear that bit.

### Special Registers

| Register | Name | Purpose |
|---|---|---|
| R12 | LR | Link register (written by CALL) |
| R13 | SP | Stack pointer; the stack grows downward |
| R14 | IP | Instruction pointer |
| R15 | FLAGS | Flags register |

Only IP (automatic instruction fetching), FLAGS (ALU and interrupt flags), and SP/LR/IP in the Section 6 restrictions and CALL/TRAP/IRET instructions receive special hardware treatment. In all other respects they behave as ordinary registers.

## 2. Flags

ZF, CF, SF, and OF are the standard arithmetic flags. IF controls interrupts (0 = disabled, 1 = enabled).

```
  0    1    2    3    4    5   6   7   8   9  10  11  12  13  14  15
| ZF | CF | SF | IF | OF | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
```

- Bits 5..15 always read as zero; writes to them are ignored.
- FLAGS can be read and written like any other register (LD, ST, PUSH, POP, ALU). Individual flags can be set, cleared, or toggled using bitwise operations (Section 2.2).
- Flag masks: ZF = $01, CF = $02, SF = $04, IF = $08, OF = $10.
- If an instruction writes its result to R15 (R1=R15), the ALU flags are **not** updated automatically: R15 receives exactly the written value. CMP and TEST do not write a destination register, so they update flags normally even when R1=R15.

### 2.1. ALU Flag Rules

| Instructions | ZF | SF | CF | OF |
|---|---|---|---|---|
| ADD, ADC | Result is zero | Result bit 15 | Carry out of bit 15 | Signed overflow |
| SUB, SBC, CMP | Result is zero | Result bit 15 | **Borrow:** 1 if unsigned A < B (including incoming CF for SBC) | Signed overflow |
| NEG | Result is zero | Bit 15 | 1 if R1≠0 (borrow) | 1 if R1=$8000 |
| AND, OR, XOR, TEST | Result is zero | Bit 15 | 0 | 0 |
| SHL, SHR, SAR, ROR (count ≠ 0) | Result is zero | Bit 15 | Last bit shifted out (for ROR, result bit 15) | 0 |
| SHL, SHR, SAR, ROR (count = 0) | Unchanged | Unchanged | Unchanged | Unchanged |
| MUL | Result is zero | Bit 15 | 1 if the high product word is nonzero | Same as CF |
| MULHU | Result is zero | Bit 15 | 0 | 0 |
| DIVU, REMU | Result is zero | Bit 15 | 0 | 1 on division by zero |

ADC computes A+B+CF; SBC computes A−B−CF. Multiword ADD/ADC and SUB/SBC chains work without inverting CF.

The following instructions do not change flags: LD, LDSX, LDZX, ST, STB, XCHB, NOT, BSX, Jxx, CALL, WFI. TRAP clears IF; IRET loads flags from the stack.

### 2.2. Modifying Flags in Software

Flags are modified with OR, AND, or XOR using R1=R15. Because writing to R15 suppresses the automatic ALU flag update, R15 receives the exact result of the operation.

A short immediate (MOD=1, 0..15) covers bits 0..3, namely ZF, CF, SF, and IF. This yields the following rules:

| Instruction | Operation | Side effect |
|---|---|---|
| `OR FLAGS, #k` (short) | Sets bits specified by k | None |
| `XOR FLAGS, #k` (short) | Toggles bits specified by k | None |
| `AND FLAGS, #k` (short) | Keeps only the selected bits among bits 0..3, clearing the others | **Always clears OF** (bit 4), since bits 4..15 of the short immediate are zero |
| `AND FLAGS, #m` (long, two words) | Preserves only bits specified by m | None if bit 4 of m is 1 |
| `TEST FLAGS, #k` | Tests bits specified by k | **Overwrites ZF, CF, SF, and OF:** ZF=1 if (FLAGS & k)=0; CF=0, SF=0, OF=0. IF is unchanged |

Bits 5..15 remain zero for all these operations.

Standard operations (all single-word instructions are atomic: an interrupt cannot occur between reading and writing FLAGS):

| Operation | Instruction | Words | Encoding | Result | Side effect |
|---|---|---|---|---|---|
| STI | `OR FLAGS, #$08` | 1 | `6FC1` | IF=1 | None |
| CLI | `AND FLAGS, #$07` | 1 | `67B9` | IF=0 | **OF=0** |
| CLI without side effects | `AND FLAGS, #$FFF7` | 2 | `67F3 FFF7` | IF=0 | None |
| STC | `OR FLAGS, #$02` | 1 | `6F91` | CF=1 | None |
| CLC | `AND FLAGS, #$0D` | 1 | `67E9` | CF=0 | **OF=0** |
| CLC without side effects | `AND FLAGS, #$FFFD` | 2 | `67F3 FFFD` | CF=0 | None |
| CMC | `XOR FLAGS, #$02` | 1 | `7791` | CF toggled | None |
| Test IF | `TEST FLAGS, #$08` | 1 | `8FC1` | ZF=1 if IF=0 | **CF=0, SF=0, OF=0; previous ZF lost** |

Do not clear IF by executing OR followed by XOR: IF would temporarily become 1 between those instructions, allowing an interrupt to be accepted.

For critical sections, use `PUSH FLAGS` / `CLI` / ... / `POP FLAGS`. Restoring the complete FLAGS register restores IF and OF, making CLI's side effect irrelevant.

## 3. Instruction Format

Instruction word (most significant bit on the left):

```
 15      11 10    7 6     3 2     0
| OPCODE:5 | R1:4  | R2:4  | R2MOD:3 |
```

If an instruction needs a `disp` word, it immediately follows the instruction word. Instruction length is:

> **Two words if R2MOD ∈ {5, 6, 7} or (R2MOD = 3 and R2 = IP); otherwise one word.**

This rule applies only to instructions that use the R2/MOD operand. Combined-opcode MISC instructions (NEG, NOT, BSX, IRET, WFI) and JR always occupy one word.

**Unused fields must be zero.** Any field that is not used by an instruction must contain zero. A nonzero value is a reserved encoding (Section 6.2, ERRCMD):

| Instructions | Fields required to be zero |
|---|---|
| NEG, NOT, BSX (MISC) | R2MOD (R2 is the operation subcode) |
| IRET, WFI (MISC) | R1, R2MOD |
| CALL, TRAP | R1 |
| Any operand-using instruction with MOD=5 | R2 |

These encodings are reserved for future instructions (for example, further single-operand instruction subcodes); they must not be treated as “don't care” fields.

Instruction validity and length are determined **solely from the first word**.

### R2MOD Addressing Modes

| MOD | Syntax | Operand value |
|---|---|---|
| 0 | reg | Value of register R2 |
| 1 | #imm4 | R2 field as an unsigned immediate in 0..15 (not a register number) |
| 2 | [reg] | Word (or byte) at address R2 |
| 3 | [reg++] | Same, then R2 += access size. For R2=IP: word following the instruction, then IP += 2 (immediate operand) |
| 4 | [--reg] | First R2 −= access size, then access memory at R2 |
| 5 | [disp] | Word (or byte) at absolute address disp; the R2 field must be 0 |
| 6 | [reg+disp] | Word (or byte) at address R2+disp |
| 7 | reg+disp | The sum R2+disp itself (no memory access) |

- The access size is 2 for word instructions and 1 for byte instructions (STB, LDSX, LDZX, XCHB).
- “R2+MOD value” below means the result defined by this table.
- For Jxx and CALL with MOD=1, see Section 7. MOD=7 with R2=IP gives an address relative to the next instruction.
- `LD R1, R2+disp` (MOD=7) serves as LEA.

## 4. Instruction Execution Order (Microprogram)

1. Fetch the instruction word; IP += 2.
2. Validate the encoding (Section 6). A reserved encoding—or a constrained-unpredictable one for which the implementation selects ERRCMD—raises ERRCMD without instruction side effects (Section 8.4).
3. For a two-word instruction, fetch the second word; IP += 2. **IP now points to the next instruction. This is the value seen by any instruction that reads IP**, including R2=IP with MOD=6 or 7, and IP as an operand in ALU, ST, or PUSH instructions.
4. Read R1 if the instruction uses it. **The R1 source value is captured before any modification of R2.**
5. Evaluate the operand according to MOD; perform any R2 auto-increment or auto-decrement at this step.
6. Perform the memory access (read, write, or atomic exchange).
7. Execute the operation, write the result to R1, and update flags.

Consequences:

- If R1 and R2 are the same register, **the R1 result write takes precedence over auto-increment/decrement**: `LD R1,[R1++]` leaves the loaded value in R1; `POP SP` (`LD SP,[SP++]`) loads SP from the stack.
- `PUSH SP` stores the old SP. `ST R1,[R1++]` and `ST R1,[--R1]` store the old R1 value.
- `LD IP,[IP++]` (`JMP #imm`) jumps to the immediate value.
- Auto-increment would also take place for a Jxx whose condition is false, but these addressing modes are reserved or constrained-unpredictable (Sections 6.2 and 6.3).
- Instructions are atomic with respect to interrupts: IRQs are accepted only between instructions.

## 5. Opcode Table

The opcode occupies the five most significant bits of the instruction word. The decoder is microprogrammed; instruction properties (byte access, R1 writeback, memory reads, valid MOD modes) are taken from ROM using the five-bit opcode.

| Code | Mnemonic | | Code | Mnemonic |
|---|---|---|---|---|
| 00000 | LD | | 10000 | CMP |
| 00001 | LDZX | | 10001 | TEST |
| 00010 | LDSX | | 10010 | SHL |
| 00011 | XCHB | | 10011 | SHR |
| 00100 | ST | | 10100 | SAR |
| 00101 | STB | | 10101 | ROR |
| 00110 | JR | | 10110 | MISC |
| 00111 | reserved | | 10111 | reserved |
| 01000 | ADD | | 11000 | reserved |
| 01001 | ADC | | 11001 | Jxx |
| 01010 | SUB | | 11010 | CALL |
| 01011 | SBC | | 11011 | TRAP |
| 01100 | AND | | 11100 | MUL |
| 01101 | OR | | 11101 | MULHU |
| 01110 | XOR | | 11110 | DIVU |
| 01111 | reserved | | 11111 | REMU |

Reserved opcodes raise ERRCMD. For JR (revision 5 onward), R2 and R2MOD form a seven-bit displacement (Section 7.3).

The combined MISC opcode is `10110` (revision 6): the R2 field selects the operation, and R2MOD must be zero.

| R2 | Instruction | R1 |
|---|---|---|
| 0 | NEG | Register |
| 1 | NOT | Register |
| 2 | BSX | Register |
| 3 | IRET | Must be 0 |
| 4 | WFI | Must be 0 |
| 5..15 | Reserved (ERRCMD) | — |

The NEG encoding is unchanged from revision 5. To the decoder, MISC looks like an ordinary instruction with MOD=0 (the R2 register operand is read but unused); the microprogram chooses the operation by branching on the R2 field.

Multiplication and division occupy the `111xx` opcode block (the low two opcode bits select the operation) and belong to the ALU group.

## 6. Valid Instruction Encodings

Every encoding belongs to one of three classes:

| Class | Behavior | May programs use it? |
|---|---|---|
| **Valid** | Executes as defined in Sections 4 and 7 | Yes |
| **Reserved** (Section 6.2) | Always raises ERRCMD on every implementation | No |
| **Constrained-unpredictable** (Section 6.3) | Implementation chooses either ERRCMD or natural execution (Section 6.4) | No |

An encoding belonging to both prohibited classes is treated as reserved.

**Why two classes?** Reserved encodings provide room for architectural extensions. Because existing implementations do not execute them, later revisions can assign new instructions or modes without breaking conforming programs. Constrained-unpredictable encodings are not useful and are unlikely to be needed; implementations need not spend extra decoding logic to reject them.

### 6.1. Permitted MOD Values by Instruction Group

| Group | Opcodes | Permitted MOD | Reserved MOD |
|---|---|---|---|
| Word load / operand value | LD, ADD, ADC, SUB, SBC, AND, OR, XOR, CMP, TEST, SHL, SHR, SAR, ROR, MUL, MULHU, DIVU, REMU | 0..7 | — |
| Byte load | LDZX, LDSX | 0..6 | 7 |
| Store / exchange | ST, STB, XCHB | 2..6 | 0, 1, 7 |
| Control transfer | Jxx, CALL, TRAP | 0, 1, 2, 3 (only with R2=IP), 5, 6, 7 | 4 |
| Combined MISC opcode | NEG, NOT, BSX, IRET, WFI | MOD=0 only (R2 holds a subcode) | 1..7 |
| Short branch | JR | R2/MOD fields hold a displacement, not an addressing mode | — |

Permitted MOD values are also subject to the restrictions in Section 6.3.

### 6.2. Reserved Encodings (ERRCMD Required)

1. Reserved opcodes `00111`, `01111`, `10111`, `11000`.
2. Jxx and JR with condition code `1111`.
3. MOD values in the “Reserved MOD” column of Section 6.1: ST, STB, XCHB with MOD 0, 1, or 7; LDZX and LDSX with MOD 7; Jxx, CALL, TRAP with MOD 4.
4. Nonzero values in fields that must be zero (Section 3): MOD for MISC; R1 for IRET or WFI; R1 for CALL or TRAP; R2 for any instruction using MOD=5.
5. MISC subcodes 5..15 (the R2 field of the combined opcode).

### 6.3. Constrained-Unpredictable Combinations

For instructions with an operand (all except MISC and JR):

1. R2=R15 with MOD ∈ {2, 3, 4, 6, 7}. (With MOD=0, R15 is read as a value; with MOD=1 and 5, the R2 field does not select a register.)
2. R2=IP with MOD=4.
3. R2=IP with MOD=3 for LDZX, LDSX, ST, STB, and XCHB.
4. R2=IP with MOD=2 for ST, STB, and XCHB.
5. Byte instructions (LDZX, LDSX, STB, XCHB) with MOD=3 or 4 and R2=SP or R2=IP.
6. Jxx, CALL, TRAP with MOD=3 and R2≠IP.

### 6.4. Behavior of Constrained-Unpredictable Combinations

An implementation selects and documents one of the following behaviors for each such encoding:

- **(a) ERRCMD** as defined in Section 8.4, just as for a reserved encoding.
- **(b) Natural execution:** the instruction follows the execution order in Section 4 and the generic MOD formulas in Section 3, as if the restriction did not exist. Instruction length follows the normal rule in Section 3.

Restrictions on option (b):

- Only state that the general rules would affect may change: R1, R2 (auto-increment/decrement), IP, flags as defined for the instruction, and memory at the computed address. IF may change only through a write to R15 following the general rules.
- The instruction must complete in finite time; it must not hang or halt the processor.
- A given encoding must behave consistently on a given implementation.

The choice may differ between encodings, but must remain fixed and documented. Applications must not use these encodings, and the assembler rejects them. Future architecture revisions may define their behavior more precisely or assign new meanings. A program that violates the prohibition has no compatibility guarantee.

The `cpu16.cpp` emulator selects option (a), ERRCMD, for every constrained-unpredictable encoding. This helps identify programming errors.

For byte loads, MOD=0 takes the low byte of register R2 and extends it (`LDZX R1,R2` means `R1=R2&$FF`; `LDSX R1,R2` is equivalent to a copy followed by BSX); MOD=1 extends an immediate constant; the other permitted modes read a byte from memory.

## 7. Instruction Semantics

### 7.1. Loads, Stores, and Exchange

- **LD R1, op:** R1 = operand value (word). Flags unchanged.
- **LDZX / LDSX R1, op:** R1 = operand byte, zero-extended / sign-extended to a word. Flags unchanged.
- **ST op, R1:** Store the word in R1 at the operand address. Flags unchanged.
- **STB op, R1:** Store the low byte of R1 at the operand address. Flags unchanged.
- **XCHB op, R1:** Atomic exchange: R1 = zero-extended previous memory byte; memory = previous low byte of R1. The bus remains locked throughout the read and write, and no interrupts are accepted. Flags unchanged.

Byte stores and XCHB on a 16-bit bus require byte enables or a read-modify-write operation.

### 7.2. Arithmetic and Logic

- **ADD, ADC, SUB, SBC, AND, OR, XOR:** R1 = R1 op operand; flags as defined in Section 2.
- **CMP:** As SUB, but R1 is not modified. **TEST:** As AND, but R1 is not modified.
- **SHL, SHR** (logical), **SAR** (arithmetic right shift), **ROR** (rotate right): source and destination are R1; the shift count is the low four bits of the operand (0..15). A single instruction cannot shift by 16.
- **NEG:** R1 = 0 − R1; updates all flags. **NOT:** R1 = ~R1; does not update flags (use TEST to check the result). **BSX:** Sign-extends the low byte of R1 to a word without changing flags. These three instructions are MISC subcodes.

### 7.2a. Multiplication and Division (Revision 6)

These are unsigned ALU operations of the form R1 = R1 op operand. All addressing modes are permitted, including immediate operands. Every implementation must support them.

| Instruction | Result in R1 | ZF, SF | CF, OF |
|---|---|---|---|
| **MUL** | Low word of R1 × operand (identical for signed and unsigned multiplication) | From result | 1 if high word is nonzero (product exceeds 16 bits) |
| **MULHU** | High word of the unsigned product | From result | 0 |
| **DIVU** | Unsigned 16-by-16 quotient R1 / operand | From result | CF = 0; OF = 1 on division by zero |
| **REMU** | Remainder of R1 mod operand | From result | CF = 0; OF = 1 on division by zero |

- **Division by zero** does not raise an exception. DIVU returns $FFFF; REMU returns the dividend (R1 is unchanged); OF = 1. These values arise naturally from restoring division with a zero divisor, so the microprogram needs only a zero detector to set the flag, not a special branch. Test with `JO` after the instruction.
- **Full 32-bit product:** Use two instructions on copies of the multiplicand: `LD R2, R0` / `MUL R0, R1` / `MULHU R2, R1`.
- **Signed variants** are implemented in software. Derive the high word of a signed product from MULHU, subtracting the multiplier if the multiplicand is negative and vice versa. Signed division uses operand magnitudes and adjusts the result sign.
- When R1=R15, the general rule applies: the result is written to FLAGS without updating ALU flags automatically.

### 7.3. Control Transfer

The target is the operand value: Jxx conditionally loads IP, while CALL loads IP and saves the return address in LR.

| MOD | Branch target |
|---|---|
| 0 | Value of register R2 |
| 1 | **Short signed displacement:** IP + 2·sext4(R2), range −8..+7 words; IP is the address of the following one-word instruction. JR (±64 words) is usually preferable for conditional branches; for CALL this is the only one-word relative form |
| 2 | Word at address R2 |
| 3 (R2=IP) | Word following the instruction (absolute address) |
| 5 | Word at address disp |
| 6 | Word at address R2+disp |
| 7 | R2+disp; with R2=IP, an address relative to the next instruction |

- **Jxx:** R1 contains the condition code; flags are unchanged. If the condition is false, no action is taken and IP points to the next instruction.
- **CALL:** R1 must be 0. Compute the target, then LR = IP (address of the next instruction) and IP = target. The stack is not used and flags do not change.
- **TRAP:** R1 must be 0. Vector number n is the low four bits of the R2+MOD operand value. **For TRAP with MOD=1, this is a plain immediate in 0..15**, not a displacement. Action: PUSH IP; PUSH FLAGS; IF=0; IP = word at `$FF00 + 2·n`. The saved IP is the address of the next instruction. All vector numbers 0..15 are permitted.
- **IRET** (MISC subcode 3): POP FLAGS; POP IP. FLAGS is restored completely, including IF. R1 and MOD must both be 0.
- **JR** (opcode `00110`, revision 5): short conditional relative branch.
  ```
   15      11 10    7 6           0
  | 00110    | cond  | displacement:7 |
  ```
  The `cond` field uses the same encoding as Jxx (table below; code `1111` is reserved). Target = IP + 2·sext7(bits 6..0), where IP is the address of the next instruction. Range: −64..+63 words. JR always occupies one word; it has no displacement word, and the Section 3 instruction-length rule does not apply. Flags are unchanged. If the condition is false, no action is taken.
- **WFI** (MISC subcode 4): wait for interrupt (R1 and MOD must be 0). The processor waits for an IRQ on any line 0..7 or for reset. With IF=1, the IRQ is serviced normally and control returns to the instruction following WFI. With IF=0, execution simply resumes at the next instruction without entering a handler. Flags remain unchanged.

### Jxx and JR Condition Codes (R1)

| Code | Mnemonic | Condition |
|---|---|---|
| 0000 | JMP | Always |
| 0001 | JZ / JE | ZF=1 |
| 0010 | JNZ / JNE | ZF=0 |
| 0011 | JC / JB | CF=1 (unsigned below) |
| 0100 | JNC / JAE | CF=0 |
| 0101 | JA | CF=0 and ZF=0 |
| 0110 | JBE | CF=1 or ZF=1 |
| 0111 | JS | SF=1 |
| 1000 | JNS | SF=0 |
| 1001 | JO | OF=1 |
| 1010 | JNO | OF=0 |
| 1011 | JL | SF≠OF |
| 1100 | JGE | SF=OF |
| 1101 | JG | ZF=0 and SF=OF |
| 1110 | JLE | ZF=1 or SF≠OF |
| 1111 | — | Reserved, ERRCMD |

## 8. Interrupts and Exceptions

### 8.1. Vectors

The vector table occupies `$FF00..$FF1F`: 16 words, with vector n at address `$FF00 + 2n` containing the handler's address.

| Vector | Assignment |
|---|---|
| 0..7 | External interrupt requests IRQ0..IRQ7 |
| 8 | ERRCMD (invalid instruction) |
| 9..15 | Not assigned by hardware; available for software TRAP calls |

(TRAP may also invoke vectors 0..8.)

### 8.2. External Interrupts

- Requests are accepted only between instructions and only when IF=1. The check occurs before fetching each instruction, so enabling IF has no interrupt “shadow”: a pending request can be accepted immediately after the instruction that sets IF=1.
- If several requests are pending, the one with the lowest number is serviced first (IRQ0 has highest priority).
- There are no separate per-line interrupt masks in the CPU core; masking is handled by an external controller. Level-triggered requests held until the device is serviced are recommended.
- The saved IP is the address of the instruction that would have executed next.

### 8.3. Common Entry Sequence (IRQ, TRAP, ERRCMD)

1. SP −= 2; [SP] = IP (the IP to be saved).
2. SP −= 2; [SP] = FLAGS (value before modification).
3. FLAGS.IF = 0 (all other flags remain unchanged).
4. IP = [`$FF00 + 2·n`].

IRET returns by popping FLAGS (from the top of the stack), then IP. The stack layout is the same as for TRAP.

The interrupt mechanism does not save LR, other registers, or other flags separately. A handler must preserve everything it uses, including LR.

### 8.4. ERRCMD (Vector 8)

- Causes: always for reserved encodings (Section 6.2); for constrained-unpredictable encodings (Section 6.3) when the implementation selects ERRCMD.
- ERRCMD is not masked by IF. As usual, IF is cleared on entry.
- All validation uses only the first instruction word and occurs before any side effects: registers (including IP and auto-increments), memory, and flags retain their pre-instruction values.
- **The saved IP points to the first word of the invalid instruction**, allowing the handler to read, decode, or emulate it. To skip the instruction, the handler calculates its length using Section 3 and adjusts the saved IP.
- The exception-entry sequence itself cannot raise ERRCMD. An invalid instruction inside an ERRCMD handler raises vector 8 again; prevention of infinite recursion is the program's responsibility.

## 9. Reset

- All registers, **including SP**, and FLAGS are zero: IF=0 and interrupts are disabled. IP=0, so execution starts at address zero.
- The first instruction must initialize SP (for example, to `$FF00`, placing the downward-growing stack below the vector table). The stack must not be used and interrupts must not be enabled before that.
- Recommended memory layout: boot ROM starting at `$0000`, RAM, the vector table at `$FF00..$FF1F` (which may reside in RAM or ROM), and memory-mapped I/O at `$FF20..$FFFF`.
- **Vector table placement.** Because ERRCMD is unmaskable, it can occur even on the first instruction. Therefore:
  - Prefer placing the vector table in ROM so it is valid immediately after reset. A single ROM can be mapped to both `$0000` (startup code) and `$FF00` (vectors).
  - If the table is in RAM, startup code must initialize vector 8 (ERRCMD) first, immediately after setting SP, then initialize the other vectors before STI.

## 10. Pseudoinstructions and Common Idioms

| Syntax | Equivalent |
|---|---|
| MOV Rd, Rs | LD Rd, Rs |
| MOV Rd, #imm | LD Rd, [IP++] (two words) |
| LEA Rd, Rs+disp | LD Rd, Rs+disp (MOD=7) |
| NOP | LD R0, R0 |
| RET | LD IP, LR |
| PUSH R | ST [--SP], R |
| POP R | LD R, [SP++] |
| JMP addr | Jxx 0000, address (MOD=3 with R2=IP) |
| Short branch | JR cond, displacement (±64 words, one word) |
| Short call | CALL with MOD=1 (±8 words, one word) |
| Relative branch | Jxx cond, IP+disp (MOD=7, R2=IP) |
| STI | OR R15, #$08 (one word, no side effects) |
| CLI | AND R15, #$07 (one word; **also clears OF**, see Section 2.2) |
| STC / CMC | OR R15, #$02 / XOR R15, #$02 (one word, no side effects) |
| CLC | AND R15, #$0D (one word; **also clears OF**) |
| LDZX/LDSX Rd, Rs | Extend the low byte of a register (MOD=0) |

## 11. Self-Modifying Code

Code and data share the same memory. Instructions that write memory (ST, STB, XCHB), as well as interrupt-entry stack writes, may modify executable code. The restrictions on R2=IP in Section 6.3 concern addressing modes only, not writes to code memory: `ST [IP+disp], R1`, `ST [address], R1`, and writes through any other register are permitted.

### 11.1. Guarantees

1. **Current instruction.** Writing to either word of the instruction currently executing (its first word or its `disp` word) does not affect that instruction's behavior: both words have already been fetched before the memory write (Section 4, steps 1 and 3).
2. **Synchronization point.** A modified instruction is guaranteed to execute in its new form if at least one of the following occurs between the write and the fetch of the modified word:
   - A taken Jxx branch (a branch whose condition is false is not a synchronization point), or CALL.
   - Any other write to IP: `LD IP, ...`, `RET`, `POP IP`, or an ALU instruction with R1=IP.
   - TRAP, IRET, or entry into an IRQ or ERRCMD handler.
3. **Without a synchronization point,** behavior is constrained-unpredictable: each instruction word is independently fetched either in its old form or its new form. No other effects are permitted.
4. **Data read by an instruction**—including a vector word during TRAP or interrupt entry, an indirect branch target at `[Rn]`, `[disp]`, or `[Rn+disp]`, and memory operands—follows ordinary data-read semantics. Changes to these values become visible immediately, without a synchronization point.

Revision 3 implementations without instruction prefetch, including the `cpu16.cpp` emulator, always execute the new contents (so case 3 does not occur there). Programs must not rely on this: implementations with prefetch or an instruction cache are required to provide only the guarantee in item 2. Flushing the prefetch queue on every taken branch is one possible implementation.

### 11.2. Example

```
        LD      R0, #$01A9       ; encoding of LD R3, #5
        ST      [patch], R0      ; replace NOP with that instruction
        JMP     patch            ; taken branch: synchronization point
patch:  NOP                      ; executes as LD R3, #5
```

## 12. Changes and Additions

### Revision 6

- **Combined MISC opcode** (`10110`, Section 5): NEG, NOT, BSX, IRET, and WFI share one opcode, with the operation selected by the R2 field (subcodes 5..15 are reserved). MOD must be 0; for IRET and WFI, R1 must also be 0. The NEG encoding is unchanged. Four opcodes have been freed.
- **Unsigned multiplication and division** (Section 7.2a): MUL, MULHU, DIVU, REMU occupy the `111xx` block, are ordinary ALU instructions supporting all addressing modes, and are mandatory for every implementation. Errors such as division by zero do not raise exceptions; OF reports them instead.
- Unassigned opcodes: `00111`, `01111`, `10111`, `11000`.
- **Binary incompatibility with revision 5:** the encodings of NOT, BSX, IRET, and WFI changed. Assembly source needs no changes; simply reassemble it.
- The emulator, assembler, shared ISA header, and web debugger were updated for revision 6.

### Revision 5

- **Fields required to be zero** (Sections 3 and 6.2, item 4): formerly ignored fields (R2/MOD for NEG, NOT, BSX; R1/R2/MOD for IRET, WFI; R1 for CALL, TRAP; R2 when MOD=5) must now be zero or ERRCMD is raised. This reserves them for future subcodes without using additional opcodes: each of the five operand-free instructions retains 127 unused encodings.
- **JR** (opcode `00110`): a one-word conditional relative branch with a ±64-word range instead of ±8 words for Jxx with MOD=1. Jxx with MOD=1 remains valid.
- **Vector table placement** (Section 9): recommendations for handling an ERRCMD before initialization is complete.
- Odd word addresses: processor behavior is unchanged (bit 0 is ignored). The emulator gained a `--strict-align` option that reports such accesses and jumps to odd addresses.
- **Incompatibility with revision 4:** encodings using nonzero formerly “ignored” fields now cause ERRCMD (the assembler already encoded those fields as zero); opcode `00110` no longer causes ERRCMD. Unassigned opcodes remain `00111`, `01111`, `11110`, `11111`.
- The `cpu16.cpp` emulator and assembler reference were updated for revision 5.

### Revision 4

- **Flag layout:** IF moved from bit 4 to bit 3; OF moved from bit 3 to bit 4. ZF, CF, SF, and bits 5..15 are unchanged.
- **Goal:** atomic, one-word STI, CLI, STC, CLC, CMC, and IF tests. Previously STI and CLI required two words because the IF mask could not fit in a short immediate.
- **Side effects** are defined in Section 2.2:
  - `AND FLAGS` with a short immediate always clears OF, so the one-word CLI and CLC operations also clear OF.
  - The long forms `AND FLAGS, #$FFF7` and `AND FLAGS, #$FFFD` preserve OF.
  - `TEST FLAGS, #k` overwrites ZF, CF, SF, and OF.
- **Incompatibility with revision 3:** programs using numeric flag masks ($10 for IF, $08 for OF), or inspecting saved FLAGS bit by bit (for example, from an interrupt-handler stack frame), must be updated. Saving and restoring FLAGS as a whole (PUSH/POP FLAGS, interrupt entry, IRET) is unaffected.
- The `cpu16.cpp` emulator and assembler reference were updated for revision 4.

### Revision 3

- Invalid encodings were divided into two classes (Section 6). **Reserved** encodings always raise ERRCMD and are available for future extensions: unused opcodes, Jxx with condition code 1111, ST/STB/XCHB with MOD 0, 1, or 7, LDZX/LDSX with MOD 7, and branches with MOD 4. **Constrained-unpredictable** encodings (R2=R15 in memory addressing modes, special IP/SP combinations, and branches using MOD 3 with R2≠IP) are forbidden in programs, while implementations may choose ERRCMD or natural execution subject to Section 6.4.
- Self-modifying code is explicitly permitted; guarantees and synchronization points are defined (Section 11).
- No emulator or assembler changes were required. The emulator raises ERRCMD for every constrained-unpredictable encoding and immediately executes modified code. The assembler rejects both prohibited classes.

### Revision 2 (Relative to the Original Description)

Unchanged: instruction-word format, flag order, registers, MOD modes 0..7, original opcode set, interrupt mechanics, and reset behavior.

**Clarifications**

- Explicit bit-field layout (most significant bit on the left), and an instruction-length rule determined by the first word.
- IP value during execution: address of the next instruction after fetching `disp`. PUSH IP, ST IP, CALL, and TRAP are defined.
- Micro-operation ordering and R1 writeback priority over auto-increment when R1 and R2 are the same register.
- Flags: borrow semantics for SUB/SBC/CMP; behavior of NEG and shifts (CF = last bit shifted out; OF=0; a zero count preserves flags); behavior of CMP/TEST when R1=R15.
- XCHB: zero-extended result in R1 and atomicity.
- FLAGS bits 5..15 and IP bit 0 are defined to remain zero.
- ERRCMD: saved IP points to the invalid instruction; decoding from the first word has no side effects; causes are enumerated; vectors 9..15 are available for software use.
- SP is zero on reset, resolving the contradiction between “all registers except SP” and “the stack pointer is zero.”
- Priority and acceptance timing of external interrupts, including the absence of an interrupt “shadow” after enabling IF.
- Recommended memory map, and LEA explicitly written as LD with MOD=7.

**Behavioral changes (review for compatibility)**

- Byte instructions consistently disallow MOD=3 or 4 with SP or IP. Previously STB/XCHB restricted only IP, while allowing SP could break alignment.
- MOD=4 with IP is prohibited for every instruction; MOD=3 with IP is allowed only for word loads and control transfers; MOD=2 with IP is prohibited for stores.
- Jxx and CALL disallow MOD=3 except with IP, and disallow MOD=4, since side effects during conditional branching are not acceptable.
- LDSX/LDZX permit MOD=0 and 1 (register or immediate extension); MOD=7 is prohibited.
- For Jxx and CALL, MOD=1 is a short signed IP-relative displacement. Previously this branch mode was undefined, and absolute addresses 0..15 were not useful. For TRAP, MOD=1 remains an immediate constant.

**Additions**

- Opcode encoding table (Section 5) and summary of permitted modes (Section 6).
- WFI instruction (opcode 11101): wait for interrupts without polling.
