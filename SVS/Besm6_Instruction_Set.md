# BESM-6 Instruction Set Reference

## 1. Overview

The BESM-6 is a 48-bit word machine. Memory is addressed in 48-bit words; there are
32,768 (0100000 octal) words of address space. Instructions are 24 bits wide and packed
two per word — the **left** instruction occupies bits 48–25, the **right** instruction
occupies bits 24–1. Branches always target word boundaries (left instruction only).

**Bit-numbering convention**: bits are numbered right-to-left starting from 1. Bit 1 is
the least-significant bit; bit 48 is the most-significant bit.

**Octal** notation is used throughout.

---

## 2. CPU Registers

### Visible to the user

| Name | Width | Description |
|------|-------|-------------|
| A | 48 bits | Accumulator. Holds the current floating-point working value. |
| Y | 48 bits | "Younger bits" register (also called RMR — регистр младших разрядов). Extension of A for double-precision arithmetic and logical operations. |
| R | 6 bits | AU mode register. Controls arithmetic behavior and conditional branch semantics. See [Section 4](#4-the-au-mode-register-r). |
| M[1]–M[017] | 15 bits each | Index (modifier) registers. Used for address modification. M[017] is conventionally used as the stack pointer. |
| C | 15 bits | Address-modification register. Added to EA during address calculation. Reset to 0 after every instruction **except** UTC and WTC. |
| K | 15 bits | Program counter. Points to the current word; a flag inside the processor selects left vs. right half. |

**M[0]** always reads as 0.

### Floating-point format of the accumulator A

```
Bit:  48     42  41 40                       1
     ┌─────────┬───┬──────────────────────────┐
     │Exponent │ S │ Mantissa (2's complement)│
     └─────────┴───┴──────────────────────────┘
```

- **Bits 48–42** (7 bits): Exponent, biased by 64.
- **Bit 41**: Sign. 0 = positive, 1 = negative.
- **Bits 40–1** (40 bits): Mantissa in two's complement.

Numeric value: `(0.Mantissa − Sign) × 2^(Exponent − 64)`

A normalized number has bits 41 and 40 different (i.e., the top bit of the mantissa disagrees
with the sign bit). The mantissa magnitude lies in the range [0.5, 1.0) for positive numbers,
or [-1.0, -0.5) for negative numbers.

The Y register (RMR) stores the lower 40 bits of double-precision results in bits 40–1;
bits 48–41 of Y are preserved by most operations but are not architecturally significant
for normal arithmetic.

### Notation used in instruction descriptions

| Symbol | Meaning |
|--------|---------|
| A | Accumulator |
| Y | Younger-bits register (RMR) |
| Z | Y[40:1] — lower 40 bits of Y |
| R | AU mode register |
| K | Program counter |
| M | Index register addressed by the M field of the current instruction |
| U | Effective address (EA) of the current instruction |
| V | EA minus the M-register contribution: `offset + C` |
| N | U[7:1] — low 7 bits of EA |
| I | U[4:1] — low 4 bits of EA (selects an index register) |
| J | V[4:1] — low 4 bits of V |
| X | Memory word at EA: `mem[U]` |
| W | X[15:1] — low 15 bits of X |

---

## 3. Instruction Formats

### Format 1 — short opcode (bit 20 = 0)

```
Bits: 24  21  20  19 18     13 12           1
     ┌──────┬───┬───┬─────────┬──────────────┐
     │  M   │ 0 │ S │ OPCode1 │    offset    │
     └──────┴───┴───┴─────────┴──────────────┘
```

- **M** (bits 24–21): Selects one of 16 modifier registers (0–017 octal).
- **Bit 20**: Must be 0 for Format 1.
- **S** (bit 19): Segment bit. When set, adds 070000 to offset, effectively addressing
  the upper eighth of the address space.
- **OPCode1** (bits 18–13): 6-bit opcode, range 000–077 octal.
- **offset** (bits 12–1): 12-bit unsigned address offset.

Effective address: `EA = (M[reg] + offset + S×070000 + C) mod 0100000`

Opcodes are traditionally written in octal including the S bit, so values 000–077 assume
S = 0, and 040–077 (with S = 1) would be written 100–177.

### Format 2 — long opcode (bit 20 = 1)

```
Bits: 24  21  20 19      16 15               1
     ┌──────┬───┬──────────┬──────────────────┐
     │  M   │ 1 │ OPCode2  │     offset       │
     └──────┴───┴──────────┴──────────────────┘
```

- **M** (bits 24–21): Selects modifier register.
- **Bit 20**: Must be 1 for Format 2.
- **OPCode2** (bits 19–16): 4-bit opcode, written as 20–37 octal (the leading 1 from
  bit 20 is implicit in the traditional notation).
- **offset** (bits 15–1): 15-bit unsigned address offset (larger range than Format 1).

Effective address: `EA = (M[reg] + offset + C) mod 0100000`

The C register is added to EA by both formats, but **reset to 0 after every instruction
except UTC (022) and WTC (023)**.

### Addressing range and symbolic globals

The address space is 15 bits (32,768 words), but a Format 1 instruction's offset field is
only **12 bits**, so XTA (010), ATX (000), A+X (004) and the other short-format
memory-reference instructions can directly reach only offsets 0–04095 (plus any
index-register / C contribution). A full 15-bit symbolic address — e.g. an arbitrary
module-level global — does **not** fit in their address field.

To reference such an address the compiler must first load it into the C register with
`utc name` (Format 2, 15-bit offset; C survives into the next instruction) or into an index
register, and only then issue a bare `xta` / `atx` / `a+x` that reads or writes `mem[C]`.
There is no "direct symbolic global" short form — this is why the BESM-6 backend emits the
`utc name` + bare-load/store idiom for every global access rather than a single
`xta name`.

---

## 4. The AU Mode Register R

The 6-bit AU mode register R (RAU) controls three independent aspects of arithmetic:

```
Bit:     6     5     4     3      2        1
     ┌───────┬───────────────┬─────────┬────────┐
     │Sup.ovf│     ω mode    │Sup.round│Sup.norm│
     └───────┴───────────────┴─────────┴────────┘
```

| Bit(s) | Mask | Name | Effect when set |
|--------|------|------|-----------------|
| 6 | 040 | RAU_OVF_DISABLE | Suppress floating-point overflow exception |
| 5–3 | 034 | ω mode | Selects branch condition; see below |
| 2 | 002 | RAU_ROUND_DISABLE | Suppress rounding after normalization |
| 1 | 001 | RAU_NORM_DISABLE | Suppress normalization after arithmetic |

### ω (omega) mode — bits 5–3

The ω mode records what kind of operation was performed most recently. It governs the
condition tested by the conditional branch instructions UZA and U1A, and the behavior of
YTA.

| Bits 5–3 | Decimal | Name | Condition tested by UZA/U1A |
|----------|---------|------|-----------------------------|
| 100 | 4 | Additive | ω = (A[41] ≠ 0), i.e., A < 0 |
| 010 | 2 | Multiplicative | ω = (A[48] = 0), i.e., abs(A) < 0.5 |
| 001 | 1 | Logical | ω = (A ≠ 0) |
| 000 | 0 | None | ω = 1 always (branch always taken by U1A) |

When an instruction **sets** a new mode, it rewrites **only** the ω-group (bits 5–3):
it clears those three bits and writes exactly one of the three mode bits. Bits 6, 2, and 1
(overflow/rounding/normalization suppression) are **preserved** — an ω-setting op does not
touch them. In the dubna emulator this is `CoreState::set_logical/set_additive/
set_multiplicative` (`processor.h`), each `RAU &= ~RAU_MODE; RAU |= <group>` with
`RAU_MODE = 034`. Only `XTR`/`NTR` (and a reset) change the suppress bits.

This is what lets the runtime helpers hold an `NTR 3` (suppress norm + round)
configuration across a chain of logical/arithmetic ops while only the ω axis varies, and
why a helper that has dropped to `NTR 0` for an FP step can return to `R = 7` either with an
explicit `NTR 7` or with `NTR 3` followed by any logical op (which restores ω = logical
without disturbing the suppress bits). See
[Besm6_Runtime_Library.md](Besm6_Runtime_Library.md) § *ω mode and the AU mode register R*.

### How each instruction affects R

| Effect on R | Instructions |
|-------------|-------------|
| **Additive** (bits 5–3 = 100) | A+X, A-X, X-A, AMX, AVX |
| **Multiplicative** (bits 5–3 = 010) | A*X, A/X, ARX, E+X, E-X, E+N, E-N |
| **Logical** (bits 5–3 = 001) | XTA, STX, XTS, AAX, AEX, AOX, APX, AUX, ACX, ANX, ASX, ASN, STI, ITA, ITS, all extracodes (050–077, 020, 021) |
| **Kept** (R unchanged) | ATX, ATI, MTJ, J+M, UTC, WTC, VTM, UTM, UJ, VJM, VZM, V1M, VLM, *36, STOP |
| **As set by operand** | XTR sets R = X[47:42]; NTR sets R = EA[6:1] |
| **Used** (reads R, then kept) | UZA, U1A, YTA, RTE |

### Normalization suppression (bit 1)

When bit 1 is set (RAU_NORM_DISABLE), the normalization step is skipped after
arithmetic. The result is stored as-is in A; the mantissa may be denormal (bits 42 and
41 differ). This is useful for packed binary data manipulation.

### Rounding suppression (bit 2)

When bit 2 is set (RAU_ROUND_DISABLE), the low-order remainder bit that would normally
be OR'd into the result's LSB is suppressed.

### Overflow suppression (bit 6)

When bit 6 is set (RAU_OVF_DISABLE), a floating-point overflow does not raise an
exception. The low 7 bits of the exponent and the mantissa are correct; the overflow is
indicated by bit 8 of the exponent being set (but this is internal and not directly
readable by user code).

---

## 5. Stack Operations

### Architecture

M[017] is the conventional **stack pointer**. The stack grows toward **higher** addresses.
The pointer points to the **first available (empty) slot** above the top element. The
accumulator A holds the logical stack top and is not stored in memory until explicitly
pushed.

```
             Higher addresses
                 ├──────┤
  stack top in A │      │  ← M[017] points here (empty)
                 ├──────┤
                 │  s0  │
                 ├──────┤
                 │  s1  │
                 ├──────┤
                 │  s2  │
                 ├──────┤
                 │      │
                 ├──────┤
             Lower addresses
```

### Stack-mode auto-adjustment

Several instructions can operate in **stack mode** when `V = 0` AND `M = 017`
(i.e., the offset including C is zero and the modifier register field selects M[017]).
In stack mode the stack pointer is adjusted automatically:

| Instruction | Timing | Adjustment |
|-------------|--------|------------|
| ATX | After store | M[017] += 1 (push direction) |
| XTA | Before load | M[017] −= 1 (pop direction) |
| A+X | Before load | M[017] −= 1 |
| A-X | Before load | M[017] −= 1 |
| X-A | Before load | M[017] −= 1 |
| AMX | Before load | M[017] −= 1 |
| AVX | Before load | M[017] −= 1 |
| AAX | Before load | M[017] −= 1 |
| AOX | Before load | M[017] −= 1 |
| AEX | Before load | M[017] −= 1 |
| APX | Before load | M[017] −= 1 |
| AUX | Before load | M[017] −= 1 |
| ANX | Before load | M[017] −= 1 |
| ACX | Before load | M[017] −= 1 |
| A*X | Before load | M[017] −= 1 |
| A/X | Before load | M[017] −= 1 |
| ARX | Before load | M[017] −= 1 |
| ASX | Before load | M[017] −= 1 |
| WTC | Before load | M[017] −= 1 |

When M[017] is decremented, the load then reads from the new (decremented) M[017],
effectively popping the top of the stack into A (used as the operand).

### Dedicated stack instructions

These instructions always adjust M[017] regardless of addressing:

**XTS (003) — Push and load**

```
mem[M[017]] = A          ; save A to stack
M[017] += 1              ; advance stack pointer
A = mem[addr + M[reg]]   ; load new value into A
```

Pushes A onto the stack, then loads a new value from memory into A. ω = Logical.

**STX (001) — Store and pop**

```
mem[addr + M[reg]] = A   ; store A to memory
M[017] -= 1              ; decrement stack pointer
A = mem[M[017]]          ; pop top of stack into A
```

Stores A at the target address, then pops the previous stack top into A. ω = Logical.

**ITS (043) — Push A, load index register**

```
mem[M[017]] = A          ; save A to stack
M[017] += 1              ; advance stack pointer
A = M[I]                 ; load index register I into A
```

**STI (041) — Store into index register and pop (conditional)**

```
M[I] = A[15:1]           ; set index register I from A
if I ≠ 017:
    M[017] -= 1          ; pop only when not targeting M[017]
    A = mem[M[017]]      ; load top of stack into A
```

### Summary: push and pop with A as stack top

Because A is the logical stack top:
- **Pop** = decrement M[017], then load `mem[M[017]]` into A (as X operand).
- **Push** = store A to `mem[M[017]]`, then increment M[017].
- ATX in stack mode is a pure push (stores A, increments pointer, A unchanged).
- XTA in stack mode is a pure pop (decrements pointer, loads mem[new M[017]] into A).

---

## 6. Instruction Reference

### Format 1 Instructions (opcodes 000–047)

---

#### 000 — ATX (зп) — Store accumulator

```
X = A
if V == 0 and M == 017: M[017] += 1    ; stack mode: push
```

ω mode: **Kept**

Writes A to the memory word at EA. In stack mode (V=0, M=017) the stack pointer is
incremented **after** the store, making ATX a push operation.

---

#### 001 — STX (зпм) — Store and pop

```
X = A
M[017] -= 1
A = mem[M[017]]
```

ω mode: **Logical**

Always decrements M[017]. Stores A at EA, then loads the newly exposed stack top into A.
This is an unconditional pop regardless of the addressing mode.

---

#### 002 — MOD (рег) — CPU-internal registers

```
A ↔ register named by EA
```

**Kernel mode only.** Throws `Illegal instruction` in user mode.

One of the machine's two supervisor instructions, and the one that reaches everything
*inside* the CPU: the cache БРЗ, the page registers РП (`020`–`027`), the protection
register РЗ, the interrupt register ГРП (`0237`) and its mask МГРП (`036`), the mode bits
РУУ (`0100`–`0137`). The effective address names the register and the accumulator carries
the data **in both directions**; one bit of the address — `0200` — selects a read, and on a
read the AU switches to logical mode, because what arrives in A is a bit pattern, not a
number.

Reachable from C as the `__besm6_mod` intrinsic ([Besm6_Intrinsics.md](Besm6_Intrinsics.md)).

---

#### 003 — XTS (счм) — Push and load

```
mem[M[017]] = A
M[017] += 1
A = mem[EA]
```

ω mode: **Logical**

Always increments M[017]. Saves A onto the stack, then loads the new operand from memory.

---

#### 004 — A+X (сл) — Add

```
if V == 0 and M == 017: M[017] -= 1    ; stack mode: pop operand
[A, Z] = A + X                         ; floating-point add
```

ω mode: **Additive**

Adds the floating-point value at EA to A. In stack mode, pops the operand. The result
occupies A (upper 41 bits) and Z = Y[40:1] (lower 40 bits of remainder). Normalization
and rounding are applied unless suppressed by R.

---

#### 005 — A-X (вч) — Subtract

```
if V == 0 and M == 017: M[017] -= 1
[A, Z] = A − X
```

ω mode: **Additive**

Subtracts X from A. Stack mode pops the operand. Normalization and rounding apply.

---

#### 006 — X-A (вчоб) — Reverse subtract

```
if V == 0 and M == 017: M[017] -= 1
[A, Z] = X − A
```

ω mode: **Additive**

Subtracts A from X (operand minus accumulator). Stack mode pops the operand.

---

#### 007 — AMX (вчаб) — Subtract absolute values

```
if V == 0 and M == 017: M[017] -= 1
[A, Z] = |A| − |X|
```

ω mode: **Additive**

Computes the difference of the absolute values: `abs(A) − abs(X)`. The sign of the result
follows normal floating-point arithmetic. Stack mode pops the operand.

---

#### 010 — XTA (сч) — Load

```
if V == 0 and M == 017: M[017] -= 1    ; stack mode: pop
A = X
```

ω mode: **Logical**

Loads the memory word at EA into A. In stack mode, decrements M[017] before the load,
effectively popping the top of the stack into A.

---

#### 011 — AAX (и) — Bitwise AND

```
if V == 0 and M == 017: M[017] -= 1
A = A & X
Y = 0
```

ω mode: **Logical**

Performs a bitwise AND between A and X. Y (RMR) is cleared.

---

#### 012 — AEX (нтж) — Bitwise XOR

```
if V == 0 and M == 017: M[017] -= 1
Y = A
A = A ^ X
```

ω mode: **Logical**

Saves the old A into Y, then XORs A with X. The old A is thus preserved in Y.

---

#### 013 — ARX (слц) — Cyclical add (end-around carry)

```
if V == 0 and M == 017: M[017] -= 1
A = A + X (48-bit unsigned)
if carry out of bit 48: A += 1 (end-around carry)
Y = 0
```

ω mode: **Multiplicative**

Adds A and X as unsigned 48-bit integers with end-around carry. If the sum overflows bit
48, the carry is added back into bit 1. Y is cleared. This is **not** floating-point; both
operands are treated as raw 48-bit integers.

---

#### 014 — AVX (знак) — Conditionally negate

```
if V == 0 and M == 017: M[017] -= 1
if X[41] ≠ 0: A = −A    ; negate A if X is negative
Y = 0
```

ω mode: **Additive**

Negates A if and only if X is negative (bit 41 of X is set). The sign test uses the
mantissa sign bit (bit 41), not the exponent sign. Y is cleared. Normalization is
applied after negation.

---

#### 015 — AOX (или) — Bitwise OR

```
if V == 0 and M == 017: M[017] -= 1
A = A | X
Y = 0
```

ω mode: **Logical**

Performs a bitwise OR between A and X. Y is cleared.

---

#### 016 — A/X (дел) — Divide

```
if V == 0 and M == 017: M[017] -= 1
A = A / X    ; floating-point division
Y = undefined
```

ω mode: **Multiplicative**

Divides A by X using a non-restoring division algorithm. The divisor must be normalized
(bits 42 and 41 of X must differ); if not, a division-by-zero exception is raised. Y is
left in an undefined state. The result exponent is `E_A − E_X + 64`.

Special case: if X has mantissa exactly 0.5 (only bit 40 set, i.e., `X & BITS41 == BIT40`),
division is performed as a simple exponent subtraction and mantissa copy.

---

#### 017 — A*X (умн) — Multiply

```
if V == 0 and M == 017: M[017] -= 1
[A, Z] = A × X    ; floating-point multiply
```

ω mode: **Multiplicative**

Multiplies A by X. The result exponent is `E_A + E_X − 64`. A receives the upper 41
bits of the 81-bit product mantissa; Z = Y[40:1] receives the lower 40 bits. If either
operand is zero, the result is exactly zero (A = 0, Y[40:1] = 0).

---

#### 020 — APX (сбр) — Pack bits

```
if V == 0 and M == 017: M[017] -= 1
A = pack(A, X)
Y = 0
```

ω mode: **Logical**

Compresses the bits of A selected by mask X into the LSBs of the result:

```
result = 0
for each bit position i from 1 to 48 (right to left):
    if X[i] == 1:
        shift result right by 1
        place A[i] into result bit 48
```

Bits of A where the corresponding mask bit in X is 1 are gathered and packed into the
most-significant positions of the result (right-shifted as they accumulate). The count of
1-bits in X equals the number of significant result bits.

---

#### 021 — AUX (рзб) — Unpack bits

```
if V == 0 and M == 017: M[017] -= 1
A = unpack(A, X)
Y = 0
```

ω mode: **Logical**

Spreads the MSBs of A into positions selected by mask X (inverse of APX):

```
result = 0
src = A (scanned from bit 48 downward)
for each bit position i from 48 to 1 (left to right):
    shift result left by 1
    if X[i] == 1:
        result |= next bit from A (taken from MSB downward)
```

Each 1-bit in X consumes one bit from A (starting at A's MSB) and places it into the
corresponding result position. 0-bits in X produce 0s in the result.

---

#### 022 — ACX (чед) — Population count

```
if V == 0 and M == 017: M[017] -= 1
A = popcount(A) + X    ; end-around carry applied
Y = 0
```

ω mode: **Logical**

Counts the number of 1-bits in A (population count), then adds X to the count using
end-around carry arithmetic (same carry rule as ARX). Y is cleared.

---

#### 023 — ANX (нед) — Find highest set bit

```
if V == 0 and M == 017: M[017] -= 1
if A == 0:
    Y = 0
    A = X    ; cyclical add of 0 + X
else:
    n = position of highest 1-bit in A   ; bit 48 → n=1, bit 47 → n=2, ..., bit 1 → n=48
    Y = (A << (48 − n + 1)) & BITS48     ; remainder: A shifted so next bit is at 48
    A = n + X    ; cyclical add of position + X
```

ω mode: **Logical**

Finds the position of the highest set bit in A (numbered 1 for bit 48 down to 48 for bit
1). The remainder of A (all bits below the found bit) is shifted to fill Y from the MSB.
The position number is then added to X using end-around carry. If A is zero, Y = 0 and
the result is simply X.

---

#### 024 — E+X (слп) — Add exponent from memory

```
if V == 0 and M == 017: M[017] -= 1
E += X[48:42] − 64
Y = 0
```

ω mode: **Multiplicative**

Extracts the 7-bit exponent field of X (bits 48–42) and adds `X_exponent − 64` to the
exponent of A. Effectively multiplies A by `2^(X_exponent − 64)`. Normalization applies.
Y is cleared.

---

#### 025 — E-X (вчп) — Subtract exponent from memory

```
if V == 0 and M == 017: M[017] -= 1
E -= X[48:42] − 64    (equivalently: E += 64 − X[48:42])
Y = 0
```

ω mode: **Multiplicative**

Subtracts `X_exponent − 64` from the exponent of A. Effectively divides A by
`2^(X_exponent − 64)`. Y is cleared.

---

#### 026 — ASX (сд) — Shift by exponent in memory

```
if V == 0 and M == 017: M[017] -= 1
n = X[48:42] − 64
Y = 0
if n >= 0: [A, Y] >>= n     ; right shift A by n, bits fall into Y
if n < 0:  [Y, A] <<= −n   ; left shift A by −n, bits overflow into Y
```

ω mode: **Logical**

Shifts the 48-bit value in A by `n` bits. A right shift (n > 0) moves bits from A's LSB
into Y's MSB. A left shift (n < 0) moves bits from A's MSB into Y's LSB. Y is cleared
to 0 before the shift. (Note: for shifts ≥ 48, A becomes 0 and the shifted value
entirely occupies Y.)

---

#### 027 — XTR (рж) — Set mode register from memory

```
if V == 0 and M == 017: M[017] -= 1
R = X[47:42]    ; bits 47–42 of X become R[6:1]
```

ω mode: **As set** (R is replaced entirely)

Loads R from the exponent/sign field of X. This is the primary way to restore a
previously saved mode register or set all mode bits at once.

---

#### 030 — RTE (счрж) — Read mode register to exponent

```
A = (R & EA & 0177) << 41
```

ω mode: **Logical**

Places R into the exponent field of A. The value is `R AND (EA AND 0177)` shifted to
bits 47–42; bits 48 and 41–1 of A become 0. The EA acts as a mask on R, allowing
partial reads. (When EA = 077 all 6 bits of R are transferred; EA = 0 yields A = 0.)

---

#### 031 — YTA (счмр) — Get younger bits register

```
if R is logical mode:
    A = Y
else:
    A[40:1] = Z           ; load Y[40:1] into A mantissa
    A[41] = 0             ; clear sign
    E += N − 64           ; adjust exponent by low 7 bits of EA
    Y = Y (unchanged)     ; Y is preserved
```

ω mode: **Used** (R unchanged)

In **logical** mode, simply copies Y to A (replaces A entirely with the raw Y word).

In **additive or multiplicative** mode, splices the lower 40 bits of Y into A's mantissa
while clearing A's sign bit, then adjusts A's exponent by `N − 64` (where N = EA[7:1]).
This is used to reconstruct the second word of a double-precision value.

---

#### 032 — (no mnemonic)

**Kernel mode only.** Throws `Illegal instruction` in user mode.

The assemblers name no mnemonic for this opcode: Madlen writes it `*32` and `b6as`
([cmd/as/tables.c](https://github.com/besm6/v7besm/blob/main/cmd/as/tables.c)) has no entry
for it at all. It is *not* `ext` — that is 033, below.

---

#### 033 — EXT (увв) — I/O control

```
A ↔ peripheral register named by EA
```

**Kernel mode only.** Throws `Illegal instruction` in user mode.

The machine's other supervisor instruction, and the only way to reach a peripheral: drums,
disks, tape, printer, punches, card equipment, terminals. The BESM-6 has no I/O address
space, no memory-mapped device registers and no channel programs — every device is driven by
an `ext` whose effective address names a device register, with the accumulator carrying the
control word or the data **in both directions**. One bit of the address — `04000` — selects
a read, and on a read the AU switches to logical mode.

Note that opcode 033 names *two* instructions: this one in the first structure (Format 1,
long address) and [STOP](#033--stop-стоп--stop) in the second (Format 2). The address-field
width is what distinguishes them.

Reachable from C as the `__besm6_ext` intrinsic ([Besm6_Intrinsics.md](Besm6_Intrinsics.md));
the device address map is in
[Besm6_Peripherals.md](https://github.com/besm6/v7besm/blob/main/doc/Besm6_Peripherals.md).

---

#### 034 — E+N (слпа) — Add exponent immediate

```
E += N − 64    ; N = EA[7:1]
Y = 0
```

ω mode: **Multiplicative**

Immediate form of E+X: adds `N − 64` to A's exponent directly from the low 7 bits of
EA. No memory access. Y is cleared.

---

#### 035 — E-N (вчпа) — Subtract exponent immediate

```
E -= N − 64    (equivalently: E += 64 − N)
Y = 0
```

ω mode: **Multiplicative**

Immediate form of E-X: subtracts `N − 64` from A's exponent. Y is cleared.

---

#### 036 — ASN (сда) — Shift immediate

```
n = N − 64    ; N = EA[7:1]; range −64..63
Y = 0
if n >= 0: [A, Y] >>= n
if n < 0:  [Y, A] <<= −n
```

ω mode: **Logical**

Immediate form of ASX: shifts by `N − 64` bits using the low 7 bits of EA.
When N > 64 - shift to the right.
When N < 64 - shift to the left.
No memory access.

---

#### 037 — NTR (ржа) — Set mode register immediate

```
R = EA[6:1]
```

ω mode: **As set**

Immediate form of XTR: loads R from the low 6 bits of EA. Useful for setting
normalization/rounding/overflow suppression bits without a memory access.

---

#### 040 — ATI (уи) — Accumulator to index register

```
M[I] = A[15:1]    ; I = EA[4:1]
```

ω mode: **Kept**

Copies the low 15 bits of A into the index register selected by the low 4 bits of EA. On the real
machine the file extends further — `ati 021` writes the mode word PSW; see `042 ITA` below.

---

#### 041 — STI (уим) — Store to index and pop

```
I = EA[4:1]
M[I] = A[15:1]
if I ≠ 017:
    M[017] -= 1
    A = mem[M[017]]
```

ω mode: **Logical**

Sets M[I] from A[15:1], then if I is not M[017] itself, decrements M[017] and loads the
new stack top into A. If I == 017, no pop is performed (this is a raw M[017] assignment).

---

#### 042 — ITA (счи) — Index register to accumulator

```
A = M[I]    ; I = EA[4:1]; zero-extended to 48 bits
```

ω mode: **Logical**

Loads the 15-bit value of index register M[I] into A as an unsigned integer.

**The register file continues past the index registers.** On the real machine (and in `simh`) the
address field selects from a wider file that includes the machine's own control registers, of
which **PSW, the mode word, is at `021`** — carrying БлПр (`02000`, interrupts blocked), БлП
(`01`) and БлЗ (`02`). So `ita 021` reads the mode word and `ati 021` writes it, and unlike РП and
РЗ, which are write-only, PSW can be read back this way — the only machine register that can. C
reaches the pair as `__besm6_getpsw`/`__besm6_setpsw` ([Besm6_Intrinsics.md](Besm6_Intrinsics.md)
§3.3). Note that our two simulators model only the index registers: `dubna` and `b6sim` both
compute `M[Aex & 017]`, so under them `ita 021` reads M[1] and `ati 021` overwrites it.

---

#### 043 — ITS (счим) — Push A, load index register

```
mem[M[017]] = A
M[017] += 1
A = M[I]    ; I = EA[4:1]
```

ω mode: **Logical**

Saves A to the stack (incrementing M[017]), then loads M[I] into A.

---

#### 044 — MTJ (уии) — Copy index register

```
M[J] = M[reg]    ; J = V[4:1]; reg from instruction field
```

ω mode: **Kept**

Copies M[reg] (the modifier register from the M field of the instruction) into the index
register selected by the low 4 bits of the effective address (without M[reg] contribution).
So the C register does contribute to the target selection.

---

#### 045 — J+M (сли) — Add index registers

```
M[J] += M[reg]    ; J = V[4:1]; reg from instruction field
```

ω mode: **Kept**

Adds M[reg] to M[J] (in-place). J is selected from the effective address
(without M[reg] contribution, same as MTJ).

---

#### 046 — (соп) — Special memory access

**Illegal.** Throws `Illegal instruction` in all modes.

---

#### 047 — (э47)

**Illegal.** Throws `Illegal instruction` in all modes.

---

### Extracodes (opcodes 050–077, 020, 021)

Extracodes are the system call and mathematical library interface. When executed:

1. `M[016] = EA` (the effective address is stored in M[14] for the extracode handler).
2. The extracode handler is invoked.
3. On return, ω mode is set to **Logical**.

All extracodes share this same dispatch mechanism in `Processor::step()`.

They execute in **user** mode: an extracode is how a program asks the operating system for a
privileged operation, and the Unix v7 syscall trap `$77 N` rides on exactly this mechanism —
`N` is the effective address, the accumulator carries the argument, and `M[016]` comes back
holding `errno`. Reachable from C as the `__besm6_extracode` intrinsic
([Besm6_Intrinsics.md](Besm6_Intrinsics.md)); note that because it writes `M[016]`, an
extracode clobbers **r14**.

Each assembler spells an extracode by its opcode rather than by a mnemonic, and each does it
differently: Madlen `,*74,`, Bemsh `э74`, `b6as` `$74`.

| Opcode(s) | Purpose |
|-----------|---------|
| 050 | Square root (√A) |
| 051 | Sine (sin A) |
| 052 | Cosine (cos A) |
| 053 | Arctangent (arctan A) |
| 054 | Arcsin (arcsin A) |
| 055 | Natural logarithm (ln A) |
| 056 | Exponential (e^A) |
| 057 | Tape and file I/O operations |
| 060 | I/O control (input) |
| 061 | I/O control (output) |
| 062 | (reserved) |
| 063 | Text I/O |
| 064 | Formatted print (GOST, octal, real, ITM, hex, instructions) |
| 065 | System operations |
| 066 | (reserved) |
| 067 | System operations |
| 070 | Memory/register dump |
| 071 | Debug output |
| 072 | System operations |
| 073–074 | (reserved) |
| 075 | CPU time query (1/50 second units) |
| 076 | CPU time query with microsecond resolution |
| 077 | System operations |
| 020 (0200) | Additional system call |
| 021 (0210) | Additional system call |

---

### Format 2 Instructions (opcodes 022–037)

---

#### 022 — UTC (мода) — Set C register immediate

```
C = U    ; C = EA; no memory access
```

ω mode: **Kept**
C register: **Set** (not reset after this instruction)

Sets the address-modification register C to the effective address U. The C value will be
added to the EA of the **next** instruction. If U = 0, this instruction is a NOP (C is
set to 0, which has no effect). UTC is one of only two instructions that do not reset C
to 0 at the end of execution (the other is WTC).

---

#### 023 — WTC (мод) — Set C register from memory

```
if V == 0 and M == 017: M[017] -= 1    ; stack mode
C = X[15:1]    ; low 15 bits of memory word
```

ω mode: **Kept**
C register: **Set** (not reset after this instruction)

Loads C from the low 15 bits of the memory word at EA. The C value will be added to the
EA of the next instruction. WTC supports stack mode.

---

#### 024 — VTM (уиа) — Set index register immediate

```
M[reg] = V    ; V = offset + C (the address field, without M[reg] contribution)
```

ω mode: **Kept**

Loads M[reg] with the 15-bit value V (the raw offset plus C, not including the current
M[reg]). This allows loading an absolute value into any modifier register.

**With `reg == 0`, in supervisor mode, this is the mode-word write.** M[0] always reads 0, so the
register half of the instruction is a no-op and the hardware spends it on PSW instead: the БлП
(`01`), БлЗ (`02`) and БлПр (`02000`) bits of V are written into PSW, **all three at once**. It is
a *masked* write — ПоП, ПоК and the write-watch bit are not in the mask and keep their values — and
it disturbs neither the accumulator nor ω. In user mode there is no side effect at all. `025 UTM`
with `reg == 0` does the identical thing, from the identical address field.

This is the only single-instruction way to change the interrupt level or the mapping override, and
a kernel uses it as such: `vtm 3` enables interrupts, `vtm 02003` blocks them, `vtm 02002` turns
mapping on for a `copyin` bracket. It is exposed to C as `__besm6_maskpsw`
([Besm6_Intrinsics.md](Besm6_Intrinsics.md) §3.3). Two caveats when writing it by hand: **Madlen
will not assemble it** — `,vtm,` with a zero modifier, omitted or explicit, is rejected with
*ошибка в модификаторе*, so it must be written as the raw octal `,24, N` — and **neither `dubna`
nor `b6sim` implements it**; both give a register-0 `vtm` a private meaning, the instruction-trace
toggle.

---

#### 025 — UTM (слиа) — Add immediate to index register

```
M[reg] += V    ; V = offset + C
```

ω mode: **Kept**

Adds V (raw offset plus C, without the current M[reg]) to M[reg]. The result is truncated
to 15 bits.

---

#### 026 — UZA (по) — Branch if ω = 0

```
Y = A
if ω == 0: K = U    ; branch to EA
```

ω mode: **Used** (unchanged)

Saves A into Y, then branches to U if the ω condition is false:

| R mode | ω condition | Branches when |
|--------|------------|---------------|
| Additive | A[41] ≠ 0 (A < 0) | A ≥ 0 |
| Multiplicative | A[48] = 0 (abs(A) < 0.5) | abs(A) ≥ 0.5 |
| Logical | A ≠ 0 | A = 0 |
| None (000) | always 1 | never |

When a branch is taken, K is set to U and execution continues from the left instruction
of word U (the right-instruction flag is cleared).

---

#### 027 — U1A (пе) — Branch if ω ≠ 0

```
Y = A
if ω ≠ 0: K = U
```

ω mode: **Used** (unchanged)

Saves A into Y, then branches to U if the ω condition is true (complement of UZA):

| R mode | Branches when |
|--------|---------------|
| Additive | A < 0 (A[41] set) |
| Multiplicative | abs(A) < 0.5 (A[48] not set) |
| Logical | A ≠ 0 |
| None (000) | always (unconditional branch) |

---

#### 030 — UJ (пб) — Unconditional branch

```
K = U
```

ω mode: **Kept**

Jumps to U unconditionally. The right-instruction flag is cleared (always jumps to the
left instruction of word U).

**Special behavior**: if reg ≠ 0 and addr = 0 (i.e., `UJ M[n], 0`), the machine calls
`set_after_return()` for profiling/trace purposes. This encodes the common return-from-
subroutine idiom `UJ M[n]`.

---

#### 031 — VJM (пв) — Jump to subroutine

```
M[reg] = K + 1    ; save return address (word after current instruction's word)
K = V             ; jump to V = offset + C (without M[reg] contribution)
```

ω mode: **Kept**

Calls a subroutine at V, saving the return address (the word boundary after the current
instruction) in M[reg]. Note that the target is **V** (offset + C only), not the full EA
— the M[reg] value is not added to the target. This means the called routine's address is
independent of the current M[reg] value.

Return is normally accomplished with `UJ M[reg]` which branches to the saved return address.

---

#### 032 — IJ (выпр) — Return from interrupt

**Kernel mode only.** Throws `Illegal instruction` in user mode.

---

#### 033 — STOP (стоп) — Stop

```
halt
```

Stops the processor. Accessible in both user and kernel mode.

On real hardware the halt is **resumable**: the machine stops, the operator reads the halt
reason — the instruction's own 15-bit address field — off the console, presses *continue*,
and execution carries on at the next instruction. Our simulators do not model that: `dubna`
and `b6sim` alike treat opcode 0330 as "the run is over", ignore the address field, and end
the simulation.

This is the Format-2 (short-address) 033; the Format-1 033 is a different instruction
entirely, [EXT](#033--ext-увв--io-control). Madlen has no mnemonic for the halt at all — it
is written as the raw octal machine code `,33,` — while Bemsh spells it `стоп` and `b6as`
`stop`. Reachable from C as the `__besm6_stop` intrinsic
([Besm6_Intrinsics.md](Besm6_Intrinsics.md)), which is deliberately *not* `_Noreturn`.

---

#### 034 — VZM (пио) — Branch if index register is zero

```
if M[reg] == 0: K = V
```

ω mode: **Kept**

Branches to V if M[reg] equals zero. Used for loop termination checks. The target is V
(not EA; M[reg] is not added to the target address).

---

#### 035 — V1M (пино) — Branch if index register is not zero

```
if M[reg] ≠ 0: K = V
```

ω mode: **Kept**

Branches to V if M[reg] is non-zero.

---

#### 036 — *36 (э36) — (Undocumented, same as VZM)

```
if M[reg] == 0: K = V
```

ω mode: **Kept**

Behaves identically to VZM (034). Documented in the original manual as having an
additional "flush BRZ" side-effect, but implemented identically in this emulator.

---

#### 037 — VLM (цикл) — Loop

```
if M[reg] ≠ 0:
    M[reg] += 1
    K = V
```

ω mode: **Kept**

The loop instruction: if M[reg] is non-zero, increments M[reg] and branches to V. If
M[reg] is zero, falls through without branching or modifying M[reg].

Typical usage: initialize M[reg] to a negative count, then let VLM increment it toward
zero. When M[reg] reaches 0 the loop exits. Because the increment happens before the
check fails, M[reg] finishes at exactly 0.

---

## 7. Arithmetic Details

### Floating-point addition and subtraction

`arith_add(val, negate_acc, negate_val)` handles A+X, A-X, X-A, and AMX by selecting
sign combinations:

| Instruction | negate_acc | negate_val | Effect |
|-------------|-----------|-----------|--------|
| A+X | false | false | `[A,Z] = A + X` |
| A-X | false | true | `[A,Z] = A − X` |
| X-A | true | false | `[A,Z] = X − A` (ACC negated) |
| AMX | true | true | `[A,Z] = |A| − |X|` (both forced negative, then subtracted) |

The algorithm:
1. Align the smaller-exponent mantissa by right-shifting it by the exponent difference.
2. Add the aligned mantissas.
3. If the result is denormal (bits 42 and 41 differ), normalize right by 1 (increment
   exponent).
4. Call `arith_normalize_and_round` to left-normalize and optionally round.

Bits shifted out during alignment are collected into the RMR (Y); they participate in
rounding.

### Normalization

`arith_normalize_and_round` normalizes by left-shifting the mantissa until bits 42 and
41 agree, decrementing the exponent by the shift count. If the mantissa is zero, A is
set to 0 and Y[40:1] is cleared. Overflow is detected when the exponent's bit 8 is set
(exponent ≥ 128 after the bias); if RAU_OVF_DISABLE is not set, an exception is raised.

Rounding: if RAU_ROUND_DISABLE is clear and the `round_flag` is set (indicating non-zero
discarded bits), the LSB of the mantissa is forced to 1.

### Shift semantics (ASX / ASN)

The shift amount is `n = exponent_field − 64`:
- **n > 0**: right shift. Bits shifted out of A's LSB accumulate in Y from the MSB down.
  For `n ≥ 48`, A becomes 0 and Y holds the shifted-out portion.
- **n < 0**: left shift by `−n`. Bits shifted out of A's MSB accumulate in Y from the
  LSB up. For shifts ≥ 48, A becomes 0.
- **n = 0**: no shift; Y is set to 0.

Y is **always cleared to 0 before the shift begins**.

### End-around carry (ARX, ACX)

After the 48-bit unsigned addition, if bit 49 of the result is set (carry out), 1 is
added back to the 48-bit result (end-around, one's-complement style). This implements
one's-complement addition without a separate borrow step.

### Division by zero

`arith_divide` raises an exception if the divisor X is not normalized. Normalization is
checked by testing that bits 42 and 41 of X differ: `((X ^ (X << 1)) & BIT41) == 0`
means they are equal — both cases (zero or a denormal) are rejected.

---

## 8. Kernel Mode

The BESM-6 has two privilege levels. In user mode, certain instructions trap immediately
with an `Illegal instruction` exception. In kernel (supervisor) mode all instructions
execute normally.

### Instructions restricted to kernel mode

| Opcode | Mnemonic | Description |
|--------|----------|-------------|
| 002 | MOD (рег) | CPU-internal registers: page registers, protection, ГРП and its mask, mode bits |
| 032 | — | No mnemonic in any assembler (Madlen writes it `*32`) |
| 033 | EXT (увв) | I/O control — the peripherals; the machine's only device access |
| 046 | — (соп) | Special memory access with extended privileges |
| 047 | — (э47) | Reserved; always illegal |
| 0320 | IJ (выпр) | Return from interrupt (restores kernel/user mode and PC) |

All other instructions execute identically in both user and kernel mode. In particular:
- **STOP (033, Format 2)** is accessible in user mode — unlike the Format-1 033, `EXT`, which
  is privileged. The two share an opcode and are told apart by the address-field width.
- All extracodes (050–077, 020, 021) execute in user mode; they implement the system call
  interface through which user programs request privileged operations.
- Stack operations, arithmetic, logical, and branch instructions have no privilege check.

### How privilege is enforced

When the emulator encounters opcode 002, 032, 033, 046, 047, or 0320, it throws a C++
`Processor::Exception` with the message `"Illegal instruction ..."`. The Dubna monitor
intercepts this and terminates the user job with an appropriate error.

---

*Copyright © 1997–2018 Leonid A. Broukhis, © 2023-2026 Serge Vakulenko.*
