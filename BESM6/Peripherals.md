# BESM-6 peripherals and their control registers

This is the **programmer's** view of BESM-6 input/output: the privileged instructions a program
uses to reach the hardware, the address of every device register, the bit layout of every control
word, and the interrupt bits by which devices answer back.

It is the companion to [README.md](README.md), which covers the **operator's** view — which SIMH
`attach` and `set` commands connect a device to a host file. Roughly: README tells you how to
*plug in* a peripheral, this document tells you how a program *drives* it.

Everything here is derived from the simulator sources and describes what this simulator actually
implements. Where the hardware had a feature the simulator does not model, that is said outright
rather than glossed over.

> **A note on the octal radix and bit numbering.** Everything below is **octal** unless marked
> otherwise. BESM-6 numbers bits **right-to-left starting at 1**, so bit 1 is the least
> significant and bit 48 the most significant — the opposite of most machines. The `BBIT(n)`
> macro in [besm6_defs.h](besm6_defs.h) follows this convention.

---

## Table of contents

1. [How BESM-6 I/O works](#how-besm-6-io-works)
2. [Instruction 002 «рег» — special registers](#instruction-002-рег--special-registers)
3. [Instruction 033 «увв» — the peripheral address map](#instruction-033-увв--the-peripheral-address-map)
4. [Interrupt registers ГРП and ПРП](#interrupt-registers-грп-and-прп)
5. [The ready registers](#the-ready-registers)
6. [Magnetic drums (МБ)](#magnetic-drums-мб)
7. [Magnetic disks (МД / КМД)](#magnetic-disks-мд--кмд)
8. [Magnetic tape (МЛ)](#magnetic-tape-мл)
9. [Line printer (АЦПУ)](#line-printer-ацпу)
10. [Punch-tape reader (ФС-1500)](#punch-tape-reader-фс-1500)
11. [Punch-tape punch (ПЛ-80)](#punch-tape-punch-пл-80)
12. [Card punch (ПИ)](#card-punch-пи)
13. [Card reader (ВУ-700)](#card-reader-ву-700)
14. [Terminals, Consul typewriters and the serial multiplexor](#terminals-consul-typewriters-and-the-serial-multiplexor)
15. [Operator panel and the ГПВЦ display board](#operator-panel-and-the-гпвц-display-board)
16. [Error polling](#error-polling)
17. [A worked example: reading a drum page](#a-worked-example-reading-a-drum-page)

---

## How BESM-6 I/O works

The BESM-6 has no separate I/O address space and no channel programs. All device control happens
through **two privileged instructions**, both of which use the *effective address* to name a
register and the *accumulator* (СМ) to carry data:

| Opcode | БЕМШ / MADLEN | Reaches |
|--------|---------------|---------|
| `002` | `рег` / `mod` | CPU-internal special registers: the write cache БРЗ, the page registers РП, the protection register РЗ, the interrupt register ГРП and its mask МГРП, and mode bits in РУУ. |
| `033` | `увв` / `ext` | **The actual peripherals**: drums, disks, tape, printer, punches, card equipment, terminals. |

> **Note.** Despite its name, `002 «рег»` does **not** talk to peripheral hardware. It is included
> here because it is the only way to read and clear ГРП, the main interrupt register — and ГРП is
> how every peripheral signals completion. The two instructions are two halves of one protocol:
> `033` issues the command, ГРП (via `002`) reports the result.

Both instructions share the same shape, in `besm6_cpu.c`:

```c
case 002:                                       /* рег, mod */
    Aex = ADDR (addr + M[reg]);
    if (! IS_SUPERVISOR (RUU))
        longjmp (cpu_halt, STOP_BADCMD);
    cmd_002 ();
    /* Режим АУ - логический, если операция была "чтение" */
    if (Aex & 0200)
        RAU = SET_LOGICAL (RAU);
```

Three properties follow, and they hold for both instructions:

* **Supervisor-only.** Executing either outside supervisor mode raises `STOP_BADCMD` (*запрещённая
  команда*). Ordinary programs reach peripherals only through operating-system extracodes.
* **The register is named by the effective address** `Aex = addr + M[reg]`, not by the
  accumulator. Only the low bits of `Aex` matter: `002` decodes `Aex & 0377`, `033` decodes
  `Aex & 04177`.
* **One bit *of the address* means "read".** For `002` it is bit `0200`; for `033` it is bit
  `04000`. On a read the arithmetic unit is switched to logical mode, because the value delivered
  into the accumulator is a bit pattern, not a number.

So `033 1` *writes* a drum control word, while `033 4031` *reads* the device-ready register. The
conventional way to write these in the sources and below is by their octal address, e.g. "`033
0140`" or just "address `0140` of `033`".

Sources: [besm6_cpu.c:1090](besm6_cpu.c#L1090) (opcode 002),
[besm6_cpu.c:1363](besm6_cpu.c#L1363) (opcode 033).

---

## Instruction 002 «рег» — special registers

Decoded by `cmd_002()` at [besm6_cpu.c:513](besm6_cpu.c#L513), switching on `Aex & 0377`.

| `Aex & 0377` | Dir | Register | What the accumulator means |
|--------------|-----|----------|----------------------------|
| `0`–`7` | write | **БРЗ** — cache (buffer) register, 8 words | The whole 48-bit word. Parity is computed and stored with it. |
| `020`–`027` | write | **РП** — page (приписка) register, 8 of them | Four packed 10-bit page numbers; see below. Also refills 4 TLB entries. |
| `030`–`033` | write | **РЗ** — protection register, 4 slices of 8 bits | Only accumulator bits **21–28** are used; they become one 8-bit slice of the 32-bit РЗ. |
| `036` | write | **МГРП** — mask of the main interrupt register | The full 48-bit mask. |
| `037` | write | **ГРП** — clear the main interrupt register | `GRP &= ACC | GRP_WIRED_BITS` — a **zero** bit in the accumulator clears the corresponding ГРП bit. Wired bits survive; see below. |
| `0100`–`0137` | write | **РУУ** — control-unit mode bits | The data is in the **address**, not the accumulator: address bit 1 = БРО (disable the stop-on-error mode), bit 2 = ПКП, bit 4 = ПКЛ (parity generation for the right and left halfword). |
| `0140`–`0177` | — | watchdog reset | **Not implemented** — raises `STOP_UNIMPLEMENTED`. |
| `0200`–`0207` | read | **БРЗ** | The cache word, with parity stripped. |
| `0237` | read | **ГРП** | The full 48-bit interrupt register. |
| anything else | — | — | Logged as `РЕГ %o - неправильный адрес спец.регистра`; no effect. |

Note the symmetry: address `0200|x` reads back what address `x` writes. Only БРЗ and ГРП have read
addresses — there is no way to read back РП, РЗ or МГРП.

### Wired bits

Some ГРП and ПРП bits are **wired**: they are not flip-flops in the interrupt register but live
wires from a device, so writing to the interrupt register cannot clear them. They go away only
when the device that drives them is itself cleared. The rule is stated in
[besm6_cpu.c:65](besm6_cpu.c#L65):

```c
/* Wired (non-registered) bits of interrupt registers (GRP and PRP)
 * cannot be cleared by writing to the GRP and must be cleared by clearing
 * the registers generating the corresponding interrupts.
 */
#define GRP_WIRED_BITS (GRP_DRUM1_FREE | GRP_DRUM2_FREE |\
                        GRP_CHAN3_DONE | GRP_CHAN4_DONE |\
                        GRP_CHAN5_DONE | GRP_CHAN6_DONE |\
                        GRP_CHAN3_FREE | GRP_CHAN4_FREE |\
                        GRP_CHAN5_FREE | GRP_CHAN6_FREE |\
                        GRP_CHAN7_FREE )

#define PRP_WIRED_BITS (PRP_VU1_END | PRP_VU2_END |\
                        PRP_PCARD1_PUNCH | PRP_PCARD2_PUNCH |\
                        PRP_PTAPE1_PUNCH | PRP_PTAPE2_PUNCH )
```

In practice this means the "device free" and "exchange done" bits of the mass-storage channels
cannot be dismissed with `002 037`; issuing a new command to the device is what lowers them.

### Packing of the page registers (РП)

`mmu_setrp()` at [besm6_mmu.c:694](besm6_mmu.c#L694) unpacks four page numbers from one word. The
layout is unusual — the low 5 bits of the four fields are adjacent, but their high bits are
scattered across the top of the word:

```c
/* Младшие 5 разрядов 4-х регистров приписки упакованы
 * по 5 в 1-20 рр, 6-е разряды - в 29-32 рр, 7-е разряды - в 33-36 рр и т.п.
 */
```

That is: page *i* (i = 0…3) takes its bits 1–5 from accumulator bits `5i+1 … 5i+5`, its bit 6 from
bit `29+i`, its bit 7 from bit `33+i`, its bit 8 from `37+i`, its bit 9 from `41+i` and its bit 10
from `45+i`. Each page number is then masked to `MEMSIZE/1024 - 1` = `777` (512 pages of 1 Kword).

> **Memory mapping is a subject of its own.** РП and РЗ are listed here because `002 «рег»` is how
> they are written, but what they *mean* — address translation, the protection rules, how they
> differ between user and supervisor mode and between instruction fetch and data access — is covered
> in **[Memory_Mapping.md](Memory_Mapping.md)**.

---

## Instruction 033 «увв» — the peripheral address map

This is the real I/O instruction. `cmd_033()` at [besm6_cpu.c:595](besm6_cpu.c#L595) switches on
`Aex & 04177`; bit `04000` selects a read.

### Write addresses (bit `04000` clear)

| Address | Device | Handler | Accumulator |
|---------|--------|---------|-------------|
| `0` | drum printer solenoids | — | Ignored; no effect on the simulation. |
| `1`, `2` | Magnetic drum 1 / 2 | `drum()` | 27-bit exchange control word. |
| `3`, `4` | Disk controller (КМД) 3 / 4 | `disk_io()` | 27-bit exchange control word. |
| `5`, `6` | Tape controller 5 / 6 | `mg_io()` | 27-bit exchange control word. |
| `7` | — | — | **Unimplemented.** |
| `010`, `011` | Punch-tape reader 1 / 2 | `fs_control()` | Bits 1–3: motor/feed command. |
| `012`, `013` | Punch-tape reader, wired program | — | **Unimplemented.** |
| `014`, `015` | Printer (АЦПУ) 1 / 2 | `printer_control()` | Bits 1–4: motor/feed command. |
| `023`, `024` | Disk controller 3 / 4 — commands | `disk_ctl()` | Controller command word; see [disks](#magnetic-disks-мд--кмд). |
| `030` | **ПРП** — clear | — | `PRP &= ACC | PRP_WIRED_BITS`. |
| `031` | **ГРП** — simulate interrupts | — | `GRP |= (ACC & BITS(24)) << 24` — the low 24 accumulator bits are OR-ed into ГРП bits 25–48. |
| `032`, `033` | simulate КМБ→КВУ signals | — | **Unimplemented.** |
| `034` | **МПРП** — mask of ПРП | — | `MPRP = ACC & 077777777` (24 bits). |
| `035` | simulate drum/tape exchange | — | **Unimplemented.** |
| `040`–`047` | Printer 1 hammers | `printer_hammer()` | Bits 1–16: hammer solenoids. |
| `050`–`057` | Printer 2 hammers | `printer_hammer()` | Bits 1–16: hammer solenoids. |
| `070` | ES printer | — | Debug output only. |
| `0100`–`0137` | Tape transport control | `mg_ctl()` | Movement command; unit = `Aex - 0100`. |
| `0140` | Telegraph channel register | `tty_send()` | Bits 1–24: one bit per terminal line. |
| `0141` | Tape formatting | `mg_format()` | Bits 1–2: formatting mode. |
| `0142` | simulate ПРП interrupts | — | **Unimplemented.** |
| `0143` | Serial multiplexor | `mux_send()` | Bits 1–16: a syllable (line number + character). |
| `0147` | power-supply control | — | No observable effect; **falls through** to `0177`. |
| `0150`, `0151` | Card reader 1 / 2 | `vu_control()` | Bits 1–4: command. |
| `0153` | Clear the terminal interface | `mux_clear()` | Ignored. |
| `0154`, `0155` | Card punch 1 / 2 — motor | `pi_control()` | Bits 1 and 4 (`ACC & 011`): run, cull. |
| `0160`–`0167` | Card punch — solenoids | `pi_write()` | Bits 1–20: punch solenoids for group `Aex & 7`. |
| `0170`, `0171` | Punch-tape punch 1 / 2 | `pl_control()` | Bits 1–8: one punched frame. |
| `0172`, `0173` | plotter | — | Debug output only; the plotter is not modelled. |
| `0174`, `0175` | Consul typewriter 1 / 2 | `consul_print()` | Bits 1–8: one character. |
| `0177` | ГПВЦ display board (табло) | — | Bits 1–24: the displayed value. |

### Read addresses (bit `04000` set)

| Address | Reads | Handler |
|---------|-------|---------|
| `04001`, `04002` | syllable in exchange-simulation mode | **Unimplemented.** |
| `04003`, `04004` | Disk controller 3 / 4 status | `disk_state()` |
| `04006`, `04007` | punch-tape wired program | **Unimplemented.** |
| `04014`, `04015` | Punch-tape reader 1 / 2 — one row | `fs_read()` |
| `04016`, `04017`, `04020`–`04023` | punch tape / exchange simulation | **Unimplemented.** |
| `04030` | **ПРП**, high half — `PRP & 077770000` (bits 13–24) | — |
| `04031` | **READY** — device-ready flags (printer etc.) | — |
| `04034` | **ПРП**, low half — `(PRP & 07777) | 037` (bits 1–12; bits 1–5 always read as 1) | — |
| `04035` | Exchange error trigger ОШМ | `drum_errors() \| disk_errors() \| mg_errors()` |
| `04070` | ES printer status | returns the constant `01000`. |
| `04100` | Telegraph channel poll | `tty_query()` |
| `04102` | **READY2** — punch/card ready flags | — |
| `04103`–`04106` | Tape controller 3–6 status | `mg_state()` |
| `04107` | tape write-check circuit | always `0`. |
| `04115` | unknown | Always `0`. DISPAK issues this in groups of eight every few seconds; its purpose is not known. |
| `04140`–`04157` (except `04150`/`04154`) | punchcard row | **Unimplemented.** |
| `04143` | Serial multiplexor | `mux_read()` |
| `04150`, `04154` | Card reader 1 / 2 | `vu_read()` |
| `04160`–`04167` | Card punch — punched row readback, 20 bits | `pi_read()` |
| `04170`–`04173` | punch-tape row check code | **Unimplemented.** |
| `04174`, `04175` | Consul typewriter 1 / 2 — one character | `consul_read()` |
| `04177` | ГПВЦ display board | — |
| anything else | — | Logged as `УВВ %o - неправильный адрес ввода-вывода`; accumulator set to 0. |

> **Note.** "Unimplemented" above means the simulator raises `STOP_UNIMPLEMENTED` (*не
> реализовано*) and halts. These are real hardware features — mostly the "wired program" (запаянная
> программа) bootstrap paths and the exchange-simulation diagnostics — that no supported software
> exercises.

### Service words in low memory

Drums, disks and tapes all store data in **zones** of `8 + 1024` words: 8 service words followed by
1 Kword of user data. The user data goes wherever the control word says, but the 8 service words
always land at a **fixed, hardwired address in low memory**, one buffer per controller:

| Words | Owner |
|-------|-------|
| `010`–`017` | Drum 1 |
| `020`–`027` | Drum 2 |
| `030`–`037` | Disk controller 3 (КМД 0) |
| `040`–`047` | Disk controller 4 (КМД 1) |
| `050`–`057` | Tape channels 3 and 4 |
| `060`–`067` | Tape channels 5 and 6 |

Note that the two tape buffers are each **shared by two channels** — there are four tape channels
but only two buffers.

---

## Interrupt registers ГРП and ПРП

A peripheral never interrupts the CPU directly. It raises a bit in one of two registers, and the
CPU takes an interrupt if the corresponding mask bit is also set.

* **ГРП** (главный регистр прерываний), 48 bits, with mask **МГРП**. Carries both external device
  interrupts and internal CPU faults.
* **ПРП** (регистр прерываний периферии), 24 bits, with mask **МПРП**. Carries the slow character
  devices — punches, Consul typewriters, plotter, multiplexor.

| Register | Read | Write mask | Clear | Simulate |
|----------|------|-----------|-------|----------|
| ГРП | `002 0237` | `002 036` | `002 037` | `033 031` |
| ПРП | `033 4030` (high) + `033 4034` (low) | `033 034` | `033 030` | `033 0142` (unimplemented) |

> **How ПРП reaches the CPU.** ПРП has no interrupt line of its own. Before each instruction the
> CPU tests `PRP & MPRP`, and if any unmasked peripheral interrupt is pending it raises
> **`GRP_SLAVE`** (ГРП bit 37) — [besm6_cpu.c:1962](besm6_cpu.c#L1962). So a ПРП interrupt is
> delivered as a ГРП interrupt, and the handler must then read both halves of ПРП to find out
> which device it was. An external interrupt fires when `GRP & MGRP` is non-zero, interrupts are
> enabled in the PSW, and the CPU is not already in an interrupt.

### ГРП bits — external

From [besm6_defs.h:425](besm6_defs.h#L425). "Raised by" names the simulator function that sets it.

| Bit | Octal value | Name | Meaning |
|-----|-------------|------|---------|
| 48 | `04000000000000000` | `GRP_PRN1_SYNC` | Printer 1 drum sync pulse (`printer_event`). |
| 47 | `02000000000000000` | `GRP_PRN2_SYNC` | Printer 2 drum sync pulse. |
| 46 | `01000000000000000` | `GRP_DRUM1_FREE` | Drum 1 exchange finished *(wired)*. |
| 45 | `00400000000000000` | `GRP_DRUM2_FREE` | Drum 2 exchange finished *(wired)*. |
| 44 | `00200000000000000` | `GRP_VU1_SYNC` | Card reader 1 column sync (`vu_event`). |
| 43 | `00100000000000000` | `GRP_VU2_SYNC` | Card reader 2 column sync. |
| 42 | `00040000000000000` | `GRP_FS1_SYNC` | Punch-tape reader 1 row ready (`fs_event`). |
| 41 | `00020000000000000` | `GRP_FS2_SYNC` | Punch-tape reader 2 row ready. |
| 40 | `00010000000000000` | `GRP_TIMER` | Interval timer tick. |
| 39 | `00004000000000000` | `GRP_PRN1_ZERO` | Printer 1 drum at position zero. |
| 38 | `00002000000000000` | `GRP_PRN2_ZERO` | Printer 2 drum at position zero. |
| 37 | `00001000000000000` | `GRP_SLAVE` | **Raised whenever `PRP & MPRP` is non-zero** — this is how a ПРП interrupt reaches ГРП. See below. |
| 36 | `00000400000000000` | `GRP_CHAN3_DONE` | Channel 3 tape movement finished *(wired)*. |
| 35 | `00000200000000000` | `GRP_CHAN4_DONE` | Channel 4 tape movement finished *(wired)*. |
| 34 | `00000100000000000` | `GRP_CHAN5_DONE` | Channel 5 tape movement finished *(wired)*. |
| 33 | `00000040000000000` | `GRP_CHAN6_DONE` | Channel 6 tape movement finished *(wired)*. |
| 32 | `00000020000000000` | `GRP_PANEL_REQ` | Operator request button (`set cpu req`). |
| 31 | `00000010000000000` | `GRP_TTY_START` | Terminal start bit detected. |
| 30 | `00000004000000000` | `GRP_IMITATION` | Exchange-simulation mode. |
| 29 | `00000002000000000` | `GRP_CHAN3_FREE` | Channel 3 exchange finished *(wired)*. |
| 28 | `00000001000000000` | `GRP_CHAN4_FREE` | Channel 4 exchange finished *(wired)*. |
| 27 | `00000000400000000` | `GRP_CHAN5_FREE` | Channel 5 exchange finished *(wired)*. |
| 26 | `00000000200000000` | `GRP_CHAN6_FREE` | Channel 6 exchange finished *(wired)*. |
| 25 | `00000000100000000` | `GRP_CHAN7_FREE` | Channel 7 exchange finished *(wired)*. |
| 19 | `00000000001000000` | `GRP_SERIAL` | Serial-line poll. **Non-standard** — an addition of this simulator. |
| 11 | `00000000000002000` | `GRP_WATCHDOG` | Watchdog timer. |
| 10 | `00000000000001000` | `GRP_SLOW_CLK` | Slow clock. **Non-standard.** |

### ГРП bits — internal (CPU faults)

| Bits | Octal value | Name | Meaning |
|------|-------------|------|---------|
| 23–21 | `00000000034000000` | `GRP_DIVZERO` | Division by zero. |
| 22–21 | `00000000014000000` | `GRP_OVERFLOW` | Arithmetic overflow. |
| 21 | `00000000004000000` | `GRP_CHECK` | Arithmetic check. |
| 20 | `00000000002000000` | `GRP_OPRND_PROT` | Operand protection violation. |
| 17 | `00000000000200000` | `GRP_WATCHPT_W` | Write watchpoint hit. |
| 16 | `00000000000100000` | `GRP_WATCHPT_R` | Read watchpoint hit. |
| 15 | `00000000000040000` | `GRP_INSN_CHECK` | Instruction check (a number fetched as an instruction). |
| 14 | `00000000000020000` | `GRP_INSN_PROT` | Instruction protection violation. |
| 13 | `00000000000010000` | `GRP_ILL_INSN` | Illegal instruction. |
| 12 | `00000000000004000` | `GRP_BREAKPOINT` | Hardware breakpoint (КРА). |
| 9–5 | `00000000000000760` | `GRP_PAGE_MASK` | Page number that caused the fault. |
| 4 | `00000000000000010` | `GRP_RAM_CHECK` | RAM parity error. |
| 3–1 | `00000000000000007` | `GRP_BLOCK_MASK` | RAM block number that caused the fault. |

The `GRP_SET_PAGE(x,m)` and `GRP_SET_BLOCK(x,m)` macros fill the last two fields.

### ПРП bits

From [besm6_defs.h:473](besm6_defs.h#L473). Bits 14–13 are unused; bits 1–5 always read back as 1.

| Bit | Octal value | Name | Meaning |
|-----|-------------|------|---------|
| 22 | `010000000` | `PRP_VU1_END` | Card reader 1: end of card *(wired)*. |
| 21 | `004000000` | `PRP_VU2_END` | Card reader 2: end of card *(wired)*. |
| 20 | `002000000` | `PRP_PCARD1_CHECK` | Card punch 1: check row ready. |
| 19 | `001000000` | `PRP_PCARD2_CHECK` | Card punch 2: check row ready. |
| 18 | `000400000` | `PRP_PCARD1_PUNCH` | Card punch 1: row punched *(wired)*. |
| 17 | `000200000` | `PRP_PCARD2_PUNCH` | Card punch 2: row punched *(wired)*. |
| 16 | `000100000` | `PRP_PTAPE1_PUNCH` | Tape punch 1: frame punched *(wired)*. |
| 15 | `000040000` | `PRP_PTAPE2_PUNCH` | Tape punch 2: frame punched *(wired)*. |
| 12 | `000004000` | `PRP_CONS1_INPUT` | Consul 1: a character was typed. |
| 11 | `000002000` | `PRP_CONS2_INPUT` | Consul 2: a character was typed. |
| 10 | `000001000` | `PRP_CONS1_DONE` | Consul 1: printing finished. |
| 9 | `000000400` | `PRP_CONS2_DONE` | Consul 2: printing finished. |
| 8 | `000000200` | `PRP_PLOTTER` | Plotter ready. |
| 7 | `000000100` | `PRP_MUX_INPUT` | Multiplexor: input available. |
| 6 | `000000040` | `PRP_MUX_DONE` | Multiplexor: transmission finished. |

---

## The ready registers

Two further status words report *device readiness*, as distinct from *interrupt pending*:

* **`READY`**, read by **`033 4031`** — "опрос сигналов готовности (АЦПУ и пр.)"
* **`READY2`**, read by **`033 4102`** — "опрос сигналов готовности перфокарт и перфолент"

Unlike ГРП and ПРП, these are **not** declared in one place: each device file defines its own bits.
The consolidated layout follows. Note the several bits of **inverted** sense — `NOT_READY` and
`NOTREADY` are set when the device is *un*available.

### `READY` (`033 4031`)

| Bit | Value | Name | Meaning | Source |
|-----|-------|------|---------|--------|
| 24 | `1<<23` | `PRN1_LINEFEED` | Printer 1: **1** = hammers may fire, **0** = paper is advancing. | [besm6_printer.c:59](besm6_printer.c#L59) |
| 23 | `1<<22` | `PRN2_LINEFEED` | Printer 2, same. | |
| 20 | `1<<19` | `PRN1_NOT_READY` | Printer 1 **not** ready (inverted). | [besm6_printer.c:55](besm6_printer.c#L55) |
| 19 | `1<<18` | `PRN2_NOT_READY` | Printer 2 **not** ready (inverted). | |
| 16 | `1<<15` | `FS1_READY` | Punch-tape reader 1 ready. | [besm6_punch.c:41](besm6_punch.c#L41) |
| 15 | `1<<14` | `FS2_READY` | Punch-tape reader 2 ready. | |

### `READY2` (`033 4102`)

| Bit | Value | Name | Meaning | Source |
|-----|-------|------|---------|--------|
| 24 | `1<<23` | `VU1_NOTREADY` | Card reader 1 **not** ready (inverted). | [besm6_vu.c:37](besm6_vu.c#L37) |
| 23 | `1<<22` | `VU1_FEED` | Card reader 1 feeding. | |
| 22 | `1<<21` | `VU1_MAYSTART` | Card reader 1 may be started. | |
| 20 | `1<<19` | `VU2_NOTREADY` | Card reader 2 **not** ready (inverted). | |
| 19 | `1<<18` | `VU2_FEED` | Card reader 2 feeding. | |
| 18 | `1<<17` | `VU2_MAYSTART` | Card reader 2 may be started. | |
| 16 | `1<<15` | `PI1_READY` | Card punch 1 ready. | [besm6_punchcard.c:70](besm6_punchcard.c#L70) |
| 15 | `1<<14` | `PI1_START` | Card punch 1 may be started. | |
| 14 | `1<<13` | `PI2_READY` | Card punch 2 ready. | |
| 13 | `1<<12` | `PI2_START` | Card punch 2 may be started. | |
| 12 | `04000` | `PL1_READY` | Tape punch 1 ready. | [besm6_pl.c:38](besm6_pl.c#L38) |
| 11 | `02000` | `PL2_READY` | Tape punch 2 ready. | |
| 8 | `0200` | `CONS_READY[0]` | Consul 1 ready. | [besm6_tty.c:110](besm6_tty.c#L110) |
| 6 | `040` | `CONS_READY[1]` | Consul 2 ready. | |

> **Note.** The card reader's `FEED`/`MAYSTART` bits carry a comment in the source worth repeating:
> *"Dispak seems to only care about the NOTREADY flag, the proper behavior of FEED and MAYSTART may
> vary."* Their modelling is a best guess.
>
> The Consul ready bits can be **inverted per line** with the `TTY_INVERSE_READY` unit flag — some
> software expects the opposite polarity.

---

## Magnetic drums (МБ)

**SIMH device:** `DRUM`, units `drum0` and `drum1`. **Source:** [besm6_drum.c](besm6_drum.c).
**Addresses:** write control word to `033 1` (drum 1) or `033 2` (drum 2).

The drums are the machine's paging store. Each holds 256 zones of `8 + 1024` words.

### Exchange control word

The accumulator holds a 27-bit control word ([besm6_drum.c:34](besm6_drum.c#L34)):

| Bits | Octal mask | Name | Meaning |
|------|-----------|------|---------|
| 27–24 | `0740000000` | `DRUM_BLOCK` | Memory block number. |
| 23 | `020000000` | `DRUM_READ_OVERLAY` | Read with overlay (считывание с наложением). **Not implemented.** |
| 22 | `010000000` | `DRUM_PARITY_FLAG` | Suppress reading words with bad parity, or write with bad parity. Writing with bad parity is **not implemented**. |
| 21 | `004000000` | `DRUM_READ_SYSDATA` | Read the 8 service words only, not the data. |
| 19 | `001000000` | `DRUM_PAGE_MODE` | **1** = transfer a whole page (1024 words); **0** = transfer one sector (256 words). |
| 18 | `000400000` | `DRUM_READ` | **1** = drum → memory; **0** = memory → drum. |
| 17–13 | `000370000` | `DRUM_PAGE` | Memory page number. |
| 12–11 | `000006000` | `DRUM_PARAGRAF` | Paragraph number within the page (sector mode only). |
| 10–8 | `000001600` | `DRUM_UNIT` | Drum unit number. |
| 7–3 | `000000174` | `DRUM_CYLINDER` | Track number on the drum. |
| 2–1 | `000000003` | `DRUM_SECTOR` | Sector number (sector mode only). |

The zone number is `(cmd & (DRUM_UNIT | DRUM_CYLINDER)) >> 2` — the unit and cylinder fields are
adjacent and together form the zone address. The memory address is assembled from `DRUM_PAGE`,
`DRUM_BLOCK` and (in sector mode) `DRUM_PARAGRAF`.

### Service words

The 8 service words of a zone are read into fixed low memory: addresses `010`–`017` for drum 1,
`020`–`027` for drum 2. In sector mode only 2 of them are used, at `010 + 2*sector`.

### Interrupts

Starting an exchange **clears** `GRP_DRUM1_FREE` (bit 46) or `GRP_DRUM2_FREE` (bit 45); the drum
raises it again when the transfer completes. Because these are *wired* bits, they cannot be cleared
with `002 037` — only by starting a new exchange.

If the unit is not attached, `033 1`/`033 2` set the corresponding bit in the error mask readable
via `033 4035` and return without transferring.

---

## Magnetic disks (МД / КМД)

**SIMH device:** `MD0`–`MD7` (8 controllers × 8 units). **Source:** [besm6_disk.c](besm6_disk.c).
**Addresses:** `033 3` / `033 4` (exchange control word), `033 023` / `033 024` (controller
commands), `033 4003` / `033 4004` (status).

The disk is the only device with a genuinely **two-step protocol**, and it is worth spelling out:

1. **`033 3` or `033 4`** hands the controller (КМД 3 or КМД 4) an *exchange control word*
   describing what to transfer and where in memory. **Nothing happens yet.**
2. **`033 023` or `033 024`** issues *controller commands* — select a group, select a unit, then
   supply the track address. **Supplying the track address is what performs the transfer.**

### Step 1 — the exchange control word (`033 3`, `033 4`)

Decoded by `disk_io()` at [besm6_disk.c:683](besm6_disk.c#L683); bit layout at
[besm6_disk.c:35](besm6_disk.c#L35):

| Bits | Octal mask | Name | Meaning |
|------|-----------|------|---------|
| 27–24 | `0740000000` | `DISK_BLOCK` | Memory block number. |
| 21 | `004000000` | `DISK_READ_SYSDATA` | Read the service words only. |
| 19 | `001000000` | `DISK_PAGE_MODE` | **1** = whole page; **0** = half a page (one track). |
| 18 | `000400000` | `DISK_READ` | **1** = disk → memory; **0** = memory → disk. |
| 17–13 | `000370000` | `DISK_PAGE` | Memory page number. |
| 12 | `000004000` | `DISK_HALFPAGE` | Which half of the page (track mode). |
| 10–8 | `000001600` | `DISK_UNIT` | Unit number. |
| 1 | `000000001` | `DISK_HALFZONE` | Which half of the zone. |

This call also lowers the controller's ГРП "channel free" bit and clears its error flag.

### Step 2 — controller commands (`033 023`, `033 024`)

`disk_ctl()` at [besm6_disk.c:719](besm6_disk.c#L719) decodes the command word by testing bits from
the top down:

| Test | Meaning |
|------|---------|
| bit 12 set | **Supply the track address — and perform the transfer.** For a 29 MB drive the zone is `(cmd & BITS(11)) << 1`; for a 7.25 MB drive the zone is `(cmd >> 1) & BITS(10)` and bit 1 selects the track. |
| bit 11 set | **Select a unit.** Bits 1–8 are a **one-hot** unit mask, but inverted in order: bit 8 → unit 0, bit 7 → unit 1, … bit 1 → unit 7. Bit 10 additionally supplies the low bit of the track number on 29 MB drives. |
| bit 9 set, `(cmd & 01774) == 01400` | **Select a group** (линейка); bits 1–2 give the group number 0–3. |
| `cmd == 011050` | **Release the group** — reset to group 0, no unit selected. |
| otherwise | A **6-bit command** in bits 1–6; see the table below. |

The 6-bit commands (`cmd & 077`):

| Command | Meaning |
|---------|---------|
| `000` | Undocumented; DISPAK issues it once at the start of the boot. |
| `001` | Seek to cylinder 0 (сброс на 0 цилиндр). |
| `002` | Seek (подвод). |
| `003` / `043` | Read (НСМД→МОЗУ) / of the spare track. |
| `004` / `044` | Write (МОЗУ→НСМД) / of the spare track. |
| `005` | Format (разметка) — sets the format flag for the next transfer. |
| `006` | Compare codes (МОЗУ↔НСМД). |
| `007` / `047` | Read the track header / of the spare track. **This one transfers immediately.** |
| `010` | Clear the status register. |
| `011` | Report status bits 1–12. |
| `031` | Report status bits 13–24. |
| `050` | Release the drive (освобождение НМД). |

> **Note.** Seek commands (`001`, `002`) and the plain read/write commands (`003`, `004`) are
> *logged but do not act* in this simulator — the transfer is driven entirely by the bit-12 "supply
> track address" command, using the direction from the exchange control word set in step 1. Only
> `005` (format) and `007` (read header) change what that transfer will do.

### Status register (`033 4003`, `033 4004`)

Returned by `disk_state()`. Most error bits are defined but never set — error conditions are not
simulated. Bit layout at [besm6_disk.c:47](besm6_disk.c#L47):

| Octal | Name | Meaning |
|-------|------|---------|
| `000000377` | `STATUS_SEEK` | "Seek done", one bit per unit. |
| `000000400` | `STATUS_READY` | The selected unit is ready. |
| `000001000` | `STATUS_SEEK_FAIL` | Head location unknown, unit not ready. |
| `000002000` | `STATUS_CHECKSUM` | Bad checksum on read. |
| `000004000` | `STATUS_FAILURE` | Failure — OR of some of the upper bits. |
| `000010000` | `STATUS_MAYDAY` | Unspecified failure. |
| `000020000` | `STATUS_NO_AMRK` | Address marker not found after a revolution. |
| `000040000` | `STATUS_WRONG_CYL` | Wrong address marker. |
| `000100000` | `STATUS_WRONG_ID` | Bad track ID. |
| `000200000` | `STATUS_BAD_ACSUM` | Bad checksum of the address marker. |
| `000400000` | `STATUS_UNFINISHED` | I/O not finished after a revolution. |
| `001000000` | `STATUS_TRK_PARITY` | Track parity error in two-track I/O. |
| `002000000` | `STATUS_READONLY` | The selected unit is read-only. |
| `004000000` | `STATUS_POWERUP` | The unit is powered up. |
| `010000000` | `STATUS_ABSENT` | The unit is not connected. |
| `020000000` | `STATUS_BUF_ERR` | Transfer buffer not ready. |

Of these the simulator actually sets only `STATUS_READY` (in response to command `011`) and
`STATUS_ABSENT` / `STATUS_POWERUP` / `STATUS_READONLY` (in response to `031`, shifted right by 12).

### Interrupts and service words

Controller *n* uses ГРП bit `GRP_CHAN3_FREE >> n` — that is, controller 3 uses bit 29, controller 4
bit 28. The 8 service words of a zone land at memory `030 + 8n`.

---

## Magnetic tape (МЛ)

**SIMH device:** `MG3`–`MG6` (4 channels × 8 units). **Source:** [besm6_mg.c](besm6_mg.c).
**Addresses:** `033 5` / `033 6` (exchange control word), `033 0100`–`0137` (transport control),
`033 0141` (formatting), `033 4103`–`4106` (status), `033 4107` (write-check, always 0).

### Exchange control word (`033 5`, `033 6`)

`mg_io()` at [besm6_mg.c:477](besm6_mg.c#L477). The bit layout mirrors the drum and disk:

| Bits | Octal mask | Name | Meaning |
|------|-----------|------|---------|
| 27–24 | `0740000000` | `MG_BLOCK` | Memory block number. |
| 21 | `004000000` | `MG_READ_SYSDATA` | Control words only. |
| 18 | `000400000` | `MG_READ` | **1** = tape → memory. |
| 17–13 | `000370000` | `MG_PAGE` | Memory page number. |
| 10–8 | `000001600` | `MG_UNIT` | Unit number (extracted as `(op >> 7) & 7`). |

> **Note — two oddities in the controller numbering.** The dispatcher calls `mg_io (Aex - 3, ACC)`,
> so `033 5` and `033 6` address controller indices **2 and 3** — sharing the arithmetic with the
> disks, which use `disk_io (Aex - 3)` for indices 0 and 1.
>
> Further, one exchange control word is **taken by two controllers at once**: index 2 sets up
> controllers 0 and 1, index 3 sets up controllers 2 and 3. But the error flag and the ГРП bit use
> the *given* index, not the pair — deliberately, per the comment in
> [besm6_mg.c:495](besm6_mg.c#L495): *"Error flags and interrupts, however, use the given
> controller number."*

### Transport control (`033 0100`–`0137`)

The address selects the unit: `mg_ctl (Aex - 0100, ACC)`. The accumulator
([besm6_mg.c:42](besm6_mg.c#L42)):

| Bit | Value | Name | Meaning |
|-----|-------|------|---------|
| 24 | `040000000` | `MG_CLEARINTR` | Clear the channel's "movement done" interrupt. Must be issued **alone** — combining it with anything else halts the simulator. |
| 2 | `000000002` | `MG_BACK` | **0** = forward, **1** = backward. |
| 1 | `000000001` | `MG_MOVE` | Start the tape moving. |

If the unit named by the address is the one selected by the last exchange control word, and the
command is *move forward*, the queued read or write is performed. Otherwise `MG_MOVE` just steps
the tape one record forward or backward.

### Status (`033 4103`–`4106`)

Three 8-bit fields, one bit per unit ([besm6_mg.c:48](besm6_mg.c#L48)):

| Bits | Name | Meaning |
|------|------|---------|
| 24–17 | `MG_READONLY` (`1<<16`) | **1** = read-only. |
| 16–9 | `MG_OFFLINE` (`1<<8`) | **1** = offline. |
| 8–1 | `MG_MOVING` (`1`) | **1** = moving. |

### Formatting (`033 0141`)

`mg_format()` takes a 2-bit mode in the accumulator. Only channel `MG6` may format:

| Mode | Meaning |
|------|---------|
| `0` | Formatting off. |
| `1` | Does not exist (the simulator says so). |
| `2` | Erasing. If the tape is already moving, movement stops being self-sustaining and runs off after ~50 ms. |
| `3` | Write the synchrotrack. The tape must already be moving; writing the synchrotrack is self-sustaining. |

### Interrupts

Each channel *n* has two ГРП bits: `GRP_CHAN3_DONE >> n` for "tape movement finished", and one
"exchange finished" bit — `GRP_CHAN6_FREE` for the formatting channel `MG6`, `GRP_CHAN5_FREE` for
all others. Service words go to memory `050` (channels 3–4) or `060` (channels 5–6).

---

## Line printer (АЦПУ)

**SIMH device:** `PRN`, units `prn0`, `prn1` (АЦПУ-128 drum printers).
**Source:** [besm6_printer.c](besm6_printer.c).
**Addresses:** `033 014` / `033 015` (control), `033 040`–`047` and `033 050`–`057` (hammers).

The АЦПУ is a **drum printer**: a spinning drum carries the character set past the paper, and 128
hammers strike the paper when the desired character is under them. So printing a line is not "send
a string" — it is a sequence of hammer firings synchronised to the drum's rotation.

### Control (`033 014`, `033 015`)

`printer_control()` at [besm6_printer.c:131](besm6_printer.c#L131); the accumulator's low 4 bits:

| Command | Meaning |
|---------|---------|
| `1` | Line feed (протяжка). |
| `2` | Ribbon off — the drum keeps spinning; restart is fast. |
| `4` | Start. |
| `8` | Motor off (undocumented). |
| `10` | Motor and ribbon off — restart is slow (ramp-up). |

A command to a printer that is not ready (`PRN1_NOT_READY` in `READY`) is silently ignored.

### Hammers (`033 040`–`057`)

`printer_hammer (num, pos, mask)` at [besm6_printer.c:171](besm6_printer.c#L171). The address
carries both which printer and which hammer group: `num = (Aex >= 050)` and `pos = Aex & 7`. The
accumulator's low 16 bits are the hammer mask; hammer *k* of the mask strikes column
`pos + 8*k`. That covers 128 columns with 8 addresses × 16 bits.

The character struck is whatever is currently under the drum — the simulator tracks it as
`dev->curchar`, advanced by the drum-rotation event. Multiple strikes in one column position are
preserved as overprinting (up to `MAX_STRIKES` = 10).

### Interrupts and ready bits

The drum carries **`0140` (96) character positions**. `printer_event()` at
[besm6_printer.c:196](besm6_printer.c#L196) walks them one at a time, 1400 µs apart:

* `GRP_PRN1_SYNC` / `GRP_PRN2_SYNC` (ГРП bits 48/47) pulse **once per character position** — this
  is the beat the software strikes hammers on.
* `GRP_PRN1_ZERO` / `GRP_PRN2_ZERO` (ГРП bits 39/38) pulse **once per drum revolution**, when the
  drum wraps back to character 0.

Counting sync pulses since the last zero pulse is how the program knows which character is
currently under the hammers. `READY` carries the not-ready and line-feed-in-progress bits — see
[the ready registers](#the-ready-registers).

---

## Punch-tape reader (ФС-1500)

**SIMH device:** `FS`, units `fs0`, `fs1` (photo-reader, фотосчитыватель).
**Source:** [besm6_punch.c](besm6_punch.c).
**Addresses:** `033 010` / `033 011` (control), `033 4014` / `033 4015` (read a row).

### Control

`fs_control()` at [besm6_punch.c:149](besm6_punch.c#L149); the accumulator's low 3 bits:

| Command | Meaning |
|---------|---------|
| `0` | Full power off. |
| `4` | Motor on, no feed. |
| `5` | Feed one row (протяжка). |

Each `5` advances the tape by one frame and schedules the row-ready interrupt. Commands to a
not-ready reader are ignored.

### Reading

`033 4014` / `033 4015` return one 8-bit frame. Text-mode tapes (`attach -t`) are converted from
UTF-8 to GOST-10859 with odd parity (the UPP code) on the fly.

### Interrupts

`GRP_FS1_SYNC` (bit 42) / `GRP_FS2_SYNC` (bit 41) — a row is ready to be read. `READY` bits 16/15
report readiness.

---

## Punch-tape punch (ПЛ-80)

**SIMH device:** `PL`, units `pl0`, `pl1`. **Source:** [besm6_pl.c](besm6_pl.c).
**Address:** `033 0170` / `033 0171`.

The simplest device in the machine. `pl_control()` at [besm6_pl.c:99](besm6_pl.c#L99) takes the
accumulator's low 8 bits and punches them as one frame, straight into the attached file. It then
lowers `PRP_PTAPE1_PUNCH` (bit 16) or `PRP_PTAPE2_PUNCH` (bit 15) and the corresponding `READY2`
bit, raising them again about 12.5 ms later. Writing to a not-ready punch is ignored.

---

## Card punch (ПИ)

**SIMH device:** `PI`, units `pi0`, `pi1`. **Source:** [besm6_punchcard.c](besm6_punchcard.c).
**Addresses:** `033 0154` / `033 0155` (motor), `033 0160`–`0167` (solenoids),
`033 4160`–`4167` (readback).

### Motor control (`033 0154`, `033 0155`)

`pi_control()` at [besm6_punchcard.c:350](besm6_punchcard.c#L350) uses just two accumulator bits
(`ACC & 011`):

| Value | Meaning |
|-------|---------|
| `000` | Stop. |
| `001` | Start, without culling. |
| `010` | Stop, with culling (of limited use). |
| `011` | Start, with culling. |

The **cull** bit is honoured only in the `PI_LAST` state — at the end of a card.

### Punching (`033 0160`–`0167`)

A card row is 80 columns, and the solenoids are driven **20 at a time** — so one row needs four
addresses. The low three bits of the address encode *both* which punch and which group of 20
([besm6_punchcard.c:456](besm6_punchcard.c#L456)):

| Address bits | Meaning |
|--------------|---------|
| bit 3 (`Aex & 4`) | Which card punch: `0` = ПИ-1, `1` = ПИ-2. |
| bits 2–1 (`Aex & 3`) | Which group of 20 columns — **reversed**: the code computes `pos = (num & 3) ^ 3`. |

So `033 0160`–`0163` drive card punch 1 and `033 0164`–`0167` card punch 2. The accumulator's low
20 bits are the solenoids.

`033 4160`–`4167` read the punched row back through the check brushes, with the same address
encoding. Two quirks worth knowing ([besm6_punchcard.c:477](besm6_punchcard.c#L477)):

* the value is returned **inverted** (`^ 0xFFFFF`);
* it reads back the **previous** card, not the current one;
* reading out of turn — outside the `PI_CHECK` phase — returns all ones (`0xFFFFF`).

### Card cycle and interrupts

Each of the 12 rows of a card passes through three phases in order — **strike** (solenoids may
fire), **move**, **check** (the check brushes may be read). Writes are only honoured in the strike
phase and reads only in the check phase; anything else is silently useless.

`PRP_PCARD1_PUNCH` / `PRP_PCARD2_PUNCH` (bits 18/17, *wired*) go high during the strike phase.
`PRP_PCARD1_CHECK` / `PRP_PCARD2_CHECK` (bits 20/19) go high during the check phase. `READY2` bits
16/15 and 14/13 give ready and may-start.

---

## Card reader (ВУ-700)

**SIMH device:** `VU`, units `vu0`, `vu1` (600 cards/minute).
**Source:** [besm6_vu.c](besm6_vu.c).
**Addresses:** `033 0150` / `033 0151` (control), `033 4150` / `033 4154` (read).

### Control

`vu_control()` at [besm6_vu.c:254](besm6_vu.c#L254); the accumulator's low 4 bits:

| Command | Meaning |
|---------|---------|
| `1` | Read a deck (keep going card after card). |
| `2` | Stop. |
| `4` | Read one card. |
| `+010` | Additional bit, OR-able with the above: reset the column buffer. |

### Reading

`033 4150` / `033 4154` return **two 12-bit columns at once**, packed as
`(column[n] << 12) | column[n+1]` — a card is 80 columns, so 40 reads. Interrupts arrive every two
columns, matching the read width.

### Interrupts

`GRP_VU1_SYNC` / `GRP_VU2_SYNC` (bits 44/43) pulse once per **pair** of columns; `PRP_VU1_END` /
`PRP_VU2_END` (bits 22/21, *wired*) mark the end of a card. `READY2` bits 24–22 and 20–18 give
not-ready, feed and may-start.

---

## Terminals, Consul typewriters and the serial multiplexor

**SIMH device:** `TTY` (24 telegraph lines `tty1`–`tty24`, plus 2 parallel Consul lines `tty25`,
`tty26`). **Source:** [besm6_tty.c](besm6_tty.c).

Three distinct interfaces share this device.

### Telegraph channels (`033 0140`, `033 4100`)

The 24 telegraph lines are **bit-serial and polled**. There is no per-line character register:

* **`033 0140`** — `tty_send()`. The accumulator's low 24 bits are written to the channel register:
  **one bit per line**, the current output level of that line.
* **`033 4100`** — `tty_query()`. Returns the 24 input bits, one per line.

Line *n* occupies bit `1 << (24 - n)`, so line 1 is the **most** significant of the 24 bits.

The operating system shifts characters in and out bit by bit, one `033 0140` / `033 4100` pair per
bit time, running the start / 8 data / stop framing itself in software (or 5-bit Baudot MTK-2 for
teletype lines). The bit rate is set with `set tty rate=N`, default 300 Hz.

`GRP_TTY_START` (ГРП bit 31) signals a detected start bit. The simulator also uses the
non-standard `GRP_SERIAL` (bit 19) as the polling clock — `vt_clk()` raises it at the configured
line rate, but *only if it is already enabled in МГРП* (`GRP |= MGRP & GRP_SERIAL`).

### Consul typewriters (`033 0174`/`0175`, `033 4174`/`4175`)

The two Consul-254 typewriters are **parallel** — a whole character at a time:

* **`033 0174` / `033 0175`** — `consul_print()`. The accumulator's low 8 bits are one character
  (GOST-10859). The device goes not-ready until printing completes.
* **`033 4174` / `033 4175`** — `consul_read()`. Returns the last character typed in bits 1–7, with
  an **odd-parity bit in bit 8**.

Interrupts: `PRP_CONS1_INPUT` / `PRP_CONS2_INPUT` (bits 12/11) — a character was typed;
`PRP_CONS1_DONE` / `PRP_CONS2_DONE` (bits 10/9) — printing finished. Ready bits `CONS_READY` in
`READY2` (bits 8 and 6), optionally inverted per line by the `TTY_INVERSE_READY` flag.

### Serial multiplexor (`033 0143`, `033 4143`, `033 0153`)

A later addition for terminal concentrators. `mux_send()` at [besm6_tty.c:1517](besm6_tty.c#L1517)
takes a 16-bit **syllable**:

| Bits | Meaning |
|------|---------|
| 15–9 | Line number. |
| 7–1 | The character — *unless* bit 15 (`0x4000`) marks a control syllable. |

For a control syllable (bit `0x4000` set): if bit 8 (`0x80`) is also set it is a **line status
request**, answered in `MUX_SYLLABLE` with the receive state in bit 4, raising `PRP_MUX_INPUT`
(bit 7). Otherwise bit 4 (`8`) enables or disables reception on that line.

`033 4143` reads `MUX_SYLLABLE` back. `033 0153` clears the whole terminal interface (the
accumulator is ignored) and sets bit 6 of МПРП. `PRP_MUX_DONE` (bit 6) signals transmission
complete.

---

## Operator panel and the ГПВЦ display board

The **request button** is not an `033` address: `set cpu req` raises `GRP_PANEL_REQ` (ГРП bit 32)
directly, and the operating system then reads the front-panel **switch registers** (octal addresses
`1`–`7` in the address space) to learn what the operator asked for. See
[The front panel](README.md#the-front-panel) in the README.

The **табло ГПВЦ СО АН СССР** — the display board at the Novosibirsk computing centre — is a
24-bit lamp panel at **`033 0177`** (write) and **`033 4177`** (read). The simulator keeps the
value but displays nothing. Address `033 0147` (the power-supply control register, which has no
observable effect) deliberately falls through to it.

---

## Error polling

**`033 4035`** — "опрос триггера ОШМi" — returns the OR of the three mass-storage error masks:

```c
ACC = drum_errors() | disk_errors() | mg_errors();
```

Each is a bit mask "по направлениям" (per direction/channel), set when a program issues a command
to a device that is **not attached**, and cleared when a command succeeds:

| Device | Bit set on failure |
|--------|--------------------|
| Drum *n* | `0100 >> n` |
| Disk controller *n* | `020 >> n` |
| Tape channel *n* | `02` for the formatting channel (`MG6`), `04` otherwise |

---

## A worked example: reading a drum page

Read zone `05` of drum 1 into memory page 2 (words `04000`–`05777`).

**1. Build the control word** from the [drum table](#magnetic-drums-мб):

| Field | Bits | Value | Contribution |
|-------|------|-------|--------------|
| `DRUM_PAGE_MODE` | 19 | 1 — a whole 1024-word page | `001000000` |
| `DRUM_READ` | 18 | 1 — drum → memory | `000400000` |
| `DRUM_PAGE` | 17–13 | memory page 2 | `000020000` |
| `DRUM_UNIT` \| `DRUM_CYLINDER` | 10–3 | zone `05`, i.e. `zone << 2` | `000000024` |
| **Total** | — | **the control word** | **`001420024`** |

The memory address is `(cmd & DRUM_PAGE) >> 2`, so a page field of `020000` gives word address
`04000` — the field is scaled such that memory page *P* starts at word *P*·1024.

**2. Issue `033 1`** with that word in the accumulator. The transfer happens, and `GRP_DRUM1_FREE`
(ГРП bit 46) is **cleared**.

**3. Wait for `GRP_DRUM1_FREE` to come back** — read ГРП with `002 0237` and test bit 46, or let
the interrupt fire if bit 46 is enabled in МГРП (`002 036`). Being a *wired* bit, it cannot be
cleared afterwards with `002 037`; it stays raised until the next `033 1` lowers it.

**4. The data has arrived**: the 8 service words of the zone are at `010`–`017`, and the 1024 data
words at `04000`–`05777`.

You can run exactly this against the simulator:

```
sim> set drum debug
sim> attach -n drum0 drum.bin
sim> d 100 001420024              ; the control word built above
sim> d -ml 1 xta 100, ext 1       ; load it into the accumulator, then увв 1
sim> d -ml 2 stop, stop
sim> run 1
```

which prints the decoded exchange:

```
### чтение МБ 10 зона 05 память 04000-05777
```

— "чтение" (read) from drum 1 unit 0, zone `05`, into words `04000`–`05777`, matching the control
word field by field.

---

## See also

* [README.md](README.md) — building and running the simulator, attaching devices, the front panel,
  tracing and debugging.
* [Memory_Mapping.md](Memory_Mapping.md) — the other half of `002 «рег»`: the page registers РП and
  the protection register РЗ, address translation, and the protection rules.
* [besm6_defs.h](besm6_defs.h) — the ГРП and ПРП bit definitions, device prototypes, and the
  `BBIT`/`BITS` bit macros.
* [besm6_cpu.c](besm6_cpu.c) — `cmd_002()` and `cmd_033()`, the two instruction decoders that this
  document describes.
