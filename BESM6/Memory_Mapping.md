# BESM-6 memory mapping and protection

This is the **programmer's** view of BESM-6 memory: how a virtual address becomes a physical one,
which registers hold the mapping, how a page is protected, how protection differs between user and
supervisor mode — and, most importantly, how it differs between an *instruction fetch* and a *data
load or store*. Those two are not variations on one mechanism; they are two different mechanisms
that happen to share a page table.

It is the third of the BESM-6 documents. [README.md](README.md) is the **operator's** view — how to
build the simulator and plug things into it. [Peripherals.md](Peripherals.md) is the **I/O**
programmer's view — how a program drives a device. This document is the **memory** programmer's
view, and it is the one you need to write an operating system.

Everything here is derived from the simulator sources and describes what this simulator actually
implements. Where the hardware had a feature the simulator does not model, that is said outright
rather than glossed over.

> **A note on the octal radix and bit numbering.** Everything below is **octal** unless marked
> otherwise. BESM-6 numbers bits **right-to-left starting at 1**, so bit 1 is the least
> significant and bit 48 the most significant — the opposite of most machines. The `BBIT(n)`
> macro in [besm6_defs.h](besm6_defs.h) follows this convention.

---

## Table of contents

1. [The two address spaces](#the-two-address-spaces)
2. [The registers](#the-registers)
3. [Address translation](#address-translation)
4. [Supervisor mode versus user mode](#supervisor-mode-versus-user-mode)
5. [Instruction fetch versus data load and store](#instruction-fetch-versus-data-load-and-store)
6. [Protection violations and how they are reported](#protection-violations-and-how-they-are-reported)
7. [Entering and leaving supervisor mode](#entering-and-leaving-supervisor-mode)
8. [Programming the MMU](#programming-the-mmu)
9. [Свёртка — the word check tags](#свёртка--the-word-check-tags)
10. [The БРЗ write cache and the БРС prefetch buffer](#the-брз-write-cache-and-the-брс-prefetch-buffer)
11. [Notes for an operating-system port](#notes-for-an-operating-system-port)
12. [Reset state](#reset-state)
13. [See also](#see-also)

---

## The two address spaces

An effective address on the BESM-6 is **15 bits**. Every memory entry point begins by truncating to
that width — `addr &= BITS(15)` in [besm6_mmu.c:404](besm6_mmu.c#L404) and
[besm6_mmu.c:504](besm6_mmu.c#L504) — and the `ADDR()` macro
([besm6_defs.h:98](besm6_defs.h#L98)) does the same wherever the CPU computes one. So a program,
kernel or user, sees at most:

| | |
|---|---|
| **Virtual address** | 15 bits = **32 Kwords** |
| **Page size** | 1 Kword = `2000` words (offset = `addr & 01777`) |
| **Virtual pages** | **32** (page index = `addr >> 10`, so 5 bits) |

The page is called a **лист** ("sheet") in the Russian sources and in the simulator's error
messages. There is no `PAGE_SIZE` constant; the 1 Kword page is hard-wired as the shift `>> 10` and
the mask `01777`.

Physical memory is much larger:

| | |
|---|---|
| **Physical memory** | `MEMSIZE = 512 * 1024` words ([besm6_defs.h:44](besm6_defs.h#L44)) = **512 Kwords** |
| **Physical pages** | **512**, so a physical page number is 9 bits |

The consequence is the single most important shape in the machine: **a program addresses 32 pages,
the machine has 512.** The mapping registers are what connect the two, and the 32-page window is
the only thing a program can see at any instant. Everything else has to be mapped in first.

> **The physical page field is 10 bits wide, but only 9 are usable here.** `mmu_setrp()` extracts a
> ten-bit page number from the accumulator and then masks it with `(MEMSIZE >> 10) - 1` = `0777`
> ([besm6_mmu.c:697](besm6_mmu.c#L697)). The hardware field would address 1024 pages (1 Mword); this
> simulator is built for 512 Kwords, so the top bit is discarded.

### The hole at the bottom of physical memory

Physical words **0–7 are not RAM.** They are the front-panel *switch registers* (тумблерные
регистры), `pult[11][8]` at [besm6_mmu.c:71](besm6_mmu.c#L71). Any physical access below `010`
reads the panel — either the operator's switch settings or one of the ten hardwired bootstrap
programs, selected by `set cpu pult=n`. See [besm6_mmu.c:466](besm6_mmu.c#L466) in `mmu_memaccess()`
and [besm6_mmu.c:641](besm6_mmu.c#L641) in `mmu_prefetch()`.

Address **0 is a black hole** in both directions: `mmu_store()` drops the write and `mmu_load()`
returns 0 without touching memory ([besm6_mmu.c:405](besm6_mmu.c#L405),
[besm6_mmu.c:505](besm6_mmu.c#L505)). Transferring control to address 0 is a check stop —
`mmu_fetch()` raises `STOP_INSN_CHECK` with the message *"передача управления на 0"*
([besm6_mmu.c:662](besm6_mmu.c#L662)).

Writes to physical words 1–7 do not store either; they are the side channel that flushes the write
cache. See [the БРЗ section](#the-брз-write-cache-and-the-брс-prefetch-buffer).

---

## The registers

### РП — регистры приписки (the page registers)

*Приписка*, literally "assignment" or "attachment", is the BESM-6 word for address mapping. The
mapping lives in eight registers:

```c
/*
 * 64-битные регистры RP0-RP7 - для отображения регистров приписки,
 * группами по 4 ради компактности, 12 бит на страницу.
 * TLB0-TLB31 - постраничные регистры приписки, копии RPi.
 * Обращение к памяти должно вестись через TLBi.
 */
t_value RP[8];
uint32 TLB[32];
```
— [besm6_mmu.c:50](besm6_mmu.c#L50)

Each **РП** register carries **four** page descriptors, so 8 × 4 = **32 descriptors — exactly one
per virtual page.** There is no page table in memory, no page-table base register, and no page
walk: the entire mapping is those eight registers, and a descriptor is nothing but a physical page
number. There are no permission bits in a descriptor, no dirty bit, no referenced bit.

`TLB[32]` is not a cache in the usual sense — it never misses and it is never refilled from memory.
It is simply the *unpacked* copy of РП, one entry per virtual page, and it is what every memory
access actually reads. Writing РП rewrites the corresponding four TLB entries in the same breath
([besm6_mmu.c:717](besm6_mmu.c#L717)), so the two can never drift apart.

### РЗ — регистр защиты (the protection register)

```c
uint32 BAZ[8], TABST, RZ, OLDEST, FLUSH;
```
— [besm6_mmu.c:44](besm6_mmu.c#L44)

**РЗ is 32 bits: one bit per virtual page.** A **set** bit means the page is *closed* (защищён) to
data access. That is the whole of it — a single bit, with no read/write distinction. A page is
either readable-and-writable or neither.

### ПСВ (PSW) — the two override bits

The mode register `M[021]` holds, among other things, the two bits that switch the whole mechanism
off ([besm6_defs.h:203](besm6_defs.h#L203)):

```c
#define PSW_MMAP_DISABLE        000001  /* БлП - блокировка приписки */
#define PSW_PROT_DISABLE        000002  /* БлЗ - блокировка защиты */
```

* **БлП** (*блокировка приписки*) — disable mapping. Data addresses become physical.
* **БлЗ** (*блокировка защиты*) — disable protection. РЗ is ignored.

They are independent, and — this is the crux of the whole document — **БлП applies to data only.**

### РУУ (RUU) — where "supervisor" lives

There is no kernel-mode bit in the PSW. Supervisor mode is a property of *how you got here*
([besm6_defs.h:179](besm6_defs.h#L179)):

```c
#define RUU_EXTRACODE           000004  /* РежЭ - режим экстракода */
#define RUU_INTERRUPT           000010  /* РежПр - режим прерывания */

#define IS_SUPERVISOR(x)        ((x) & (RUU_EXTRACODE | RUU_INTERRUPT))
```

You are the supervisor **if and only if** you are inside an extracode or inside an interrupt. See
[Supervisor mode versus user mode](#supervisor-mode-versus-user-mode).

### Summary

| Register | Width | Cyrillic | Source | Role |
|----------|-------|----------|--------|------|
| `RP[0..7]` | 8 × 48 bits | **РП0**–**РП7** | [besm6_mmu.c:56](besm6_mmu.c#L56) | The page table. 4 physical page numbers each. **Write-only.** |
| `TLB[0..31]` | 32 × 9 bits | — | [besm6_mmu.c:57](besm6_mmu.c#L57) | Unpacked copy of РП; what translation actually reads. Not program-visible. |
| `RZ` | 32 bits | **РЗ** | [besm6_mmu.c:44](besm6_mmu.c#L44) | One protection bit per virtual page. Set = closed. **Write-only.** |
| `M[021]` | 15 bits | **ПСВ** | [besm6_defs.h:192](besm6_defs.h#L192) | Holds БлП (bit 1) and БлЗ (bit 2). |
| `M[027]` | 15 bits | **СПСВ** | [besm6_defs.h:193](besm6_defs.h#L193) | Saved ПСВ across a trap. |
| `RUU` | 9 bits | **РУУ** | [besm6_defs.h:177](besm6_defs.h#L177) | Holds РежЭ/РежПр, i.e. supervisor mode. |
| `BRZ[0..7]`, `BAZ[0..7]` | 8 × 50 / 8 × 16 | **БРЗ**, **БАЗ** | [besm6_mmu.c:43](besm6_mmu.c#L43) | Write-back store cache and its address tags. |
| `BRS[0..3]`, `BAS[0..3]` | 4 × 50 / 4 × 16 | **БРС**, **БАС** | [besm6_mmu.c:46](besm6_mmu.c#L46) | Instruction prefetch buffer. |

All of these are visible from the SIMH console under their Cyrillic names on the `MMU` device
(`examine mmu РП0`, `examine mmu РЗ`) and under Latin names on the `CPU` device (`RP0`, `RZ`) —
[besm6_mmu.c:180](besm6_mmu.c#L180) and [besm6_cpu.c:233](besm6_cpu.c#L233). That is a debugging
convenience of the simulator, not a facility of the machine: **a program cannot read РП or РЗ back.**
See [Programming the MMU](#programming-the-mmu).

---

## Address translation

The entire translation is one line, and it appears verbatim in three places —
`mmu_memaccess()` ([besm6_mmu.c:464](besm6_mmu.c#L464)), `mmu_flush()`
([besm6_mmu.c:319](besm6_mmu.c#L319)) and, spelled out, `mmu_prefetch()`
([besm6_mmu.c:632](besm6_mmu.c#L632)):

```c
/* Вычисляем физический адрес слова */
addr = (addr > 0100000) ? (addr - 0100000) :
    (addr & 01777) | (TLB[addr >> 10] << 10);
```

So, for a mapped access:

```
  virtual   14 . . . 10 | 9 . . . . . . . 0
            +-----------+-------------------+
            | page (5)  |    offset (10)    |
            +-----------+-------------------+
                  |                |
            TLB[page] = 9 bits     |
                  |                |
            +-----------------+----+---------+
  physical  |   frame (9)     |  offset (10) |
            +-----------------+--------------+
                18 . . . . . 10 | 9 . . . . 0
```

The offset passes through untouched; only the page number is replaced. Nothing else happens — no
permission bits are consulted here, because the descriptor has none.

### Bit 16: the "already physical" tag

`0100000` is bit 16, one above the 15-bit address. It is **not part of the architectural address**;
it is an internal marker meaning *"this address has already been resolved and needs no mapping."*
When it is set, translation is just `addr - 0100000` — the address is used as-is.

Three places set it, and the difference between them is the heart of this document:

| Set by | Condition | Source |
|--------|-----------|--------|
| `mmu_store()` | `M[PSW] & PSW_MMAP_DISABLE` | [besm6_mmu.c:416](besm6_mmu.c#L416) |
| `mmu_load()` | `M[PSW] & PSW_MMAP_DISABLE` | [besm6_mmu.c:511](besm6_mmu.c#L511) |
| `mmu_fetch()` | `IS_SUPERVISOR(RUU)` | [besm6_mmu.c:671](besm6_mmu.c#L671) |

Because bit 16 is only 16 bits wide, an **unmapped access can only reach physical words 0–37777** —
the low 32 Kwords. Unmapped mode does not open up the 512 Kword store; it merely removes the
indirection through РП. The upper 480 Kwords of physical memory are reachable *only* through the
mapping registers, from any mode.

The tag also leaks into the breakpoint and watchpoint registers ИБП and ДВП, which are compared
against the *tagged* address; the CPU therefore stamps bit 16 into them when they are loaded while
mapping is off ([besm6_cpu.c:1408](besm6_cpu.c#L1408)):

```c
/* breakpoint/watchpoint regs will match physical
 * or virtual addresses depending on the current
 * mapping mode.
 */
if ((M[PSW] & PSW_MMAP_DISABLE) &&
    (reg == IBP || reg == DWP))
    M[reg] |= BBIT(16);
```

---

## Supervisor mode versus user mode

Supervisor mode is not a bit you set. It is `IS_SUPERVISOR(RUU)` — *"am I inside an extracode or
inside an interrupt?"* ([besm6_defs.h:185](besm6_defs.h#L185)). You become the supervisor by taking
an extracode or an interrupt, and you stop being the supervisor by executing `выпр`. There is no
other door in either direction.

What supervisor mode buys you:

1. **Instruction fetch becomes unmapped** — automatically and unconditionally. See below.
2. **Instruction protection vanishes** — a supervisor fetch is never checked.
3. **The privileged instructions become legal**: `002 «рег»` (which is how you write РП and РЗ),
   `033 «увв»` (all I/O), `0320 «выпр»`, and `046`/`047`. In user mode each of these raises
   `STOP_BADCMD` → ГРП bit 13 `GRP_ILL_INSN`.
4. **The register file widens from 16 to 32.** `040 «уи»` and its relatives index `M[Aex & 037]` in
   supervisor mode but only `M[Aex & 017]` in user mode ([besm6_cpu.c:1401](besm6_cpu.c#L1401)).
   This is the *only* reason the kernel can reach ПСВ (`M[021]`) at all — and hence the only reason
   it can touch БлП and БлЗ. A user program cannot name those registers, so it cannot ask for the
   mapping to be turned off.

What supervisor mode does **not** buy you: **exemption from РЗ.** Data protection applies to the
supervisor exactly as it applies to a user program, unless БлЗ is set. The kernel is not
automatically trusted with data; it must explicitly say so.

---

## Instruction fetch versus data load and store

This is the section to read twice. The BESM-6 uses **different criteria for mapping and completely
different mechanisms for protection** depending on whether it is fetching an instruction or touching
an operand.

| | **Instruction fetch** | **Data load / store** |
|---|---|---|
| Handler | `mmu_fetch()` [:658](besm6_mmu.c#L658) | `mmu_load()` [:499](besm6_mmu.c#L499), `mmu_store()` [:400](besm6_mmu.c#L400) |
| **Mapped?** | **Never** in supervisor mode; **always** in user mode. Keyed on `IS_SUPERVISOR(RUU)`. | Whenever **БлП is clear**. Keyed on `PSW_MMAP_DISABLE`, independent of mode. |
| Can the mode be overridden? | **No.** БлП has no effect on fetch. | **Yes.** БлП is a normal, writable ПСВ bit. |
| **Protection test** | The РП entry for the page **is zero**. | The **РЗ bit** for the page **is set**. |
| Protection checker | `mmu_fetch_check()` [:580](besm6_mmu.c#L580) | `mmu_protection_check()` [:295](besm6_mmu.c#L295) |
| Applies in supervisor mode? | **No** — never checked. | **Yes**, unless БлЗ is set. |
| Override bit | none | **БлЗ** (`PSW_PROT_DISABLE`) |
| Trap | `STOP_INSN_PROT` | `STOP_OPERAND_PROT` |
| ГРП bit | **14** `GRP_INSN_PROT` | **20** `GRP_OPRND_PROT` |
| Faulting page reported? | No | **Yes** — ГРП bits 5–9 |

### Instruction protection: a zero РП entry

```c
void mmu_fetch_check (int addr)
{
    /* В режиме супервизора защиты нет */
    if (! IS_SUPERVISOR(RUU)) {
        int page = TLB[addr >> 10];
        /*
         * Для команд в режиме пользователя признак защиты -
         * 0 в регистре приписки.
         */
        if (page == 0) {
            iintr_data = addr >> 10;
            ...
            longjmp (cpu_halt, STOP_INSN_PROT);
        }
    }
}
```
— [besm6_mmu.c:580](besm6_mmu.c#L580)

**Physical page 0 is the invalid-page sentinel.** A virtual page whose descriptor is zero is
non-executable in user mode, and that is the *only* way to make a page non-executable. РЗ has
nothing to do with it.

The corollary is a constraint on physical memory layout: **physical page 0 can never hold user
code**, because "maps to page 0" is indistinguishable from "not mapped". Physical page 0 is where
the interrupt vectors and the switch registers live anyway, so this costs nothing — but a kernel
that hands out physical page 0 as an ordinary user text page will find it silently unexecutable.

Note also that a zero descriptor does **not** stop *data* access. Virtual page *n* with `РП = 0` and
its РЗ bit clear is perfectly readable and writable — it just aliases physical page 0. The two
mechanisms are genuinely orthogonal.

### Data protection: the РЗ bit

```c
void mmu_protection_check (int addr)
{
    /* Защита блокируется в режиме супервизора для физических (!) адресов 1-7 (ТО-8) - WTF? */
    int tmp_prot_disabled = (M[PSW] & PSW_PROT_DISABLE) ||
        (IS_SUPERVISOR (RUU) && (M[PSW] & PSW_MMAP_DISABLE) && addr < 010);

    /* Защита не заблокирована, а лист закрыт */
    if (! tmp_prot_disabled && (RZ & (1 << (addr >> 10)))) {
        iintr_data = addr >> 10;
        ...
        longjmp (cpu_halt, STOP_OPERAND_PROT);
    }
}
```
— [besm6_mmu.c:295](besm6_mmu.c#L295)

Three things to notice:

1. **The check is on the *virtual* page**, `addr >> 10`, taken *before* bit 16 is applied. РЗ's 32
   bits line up one-to-one with the 32 virtual pages.
2. **It applies even when mapping is off.** In unmapped supervisor code, `addr >> 10` is a
   *physical* page number in the low 32 K — and РЗ is still consulted against it. Disabling
   mapping does not disable protection; only БлЗ does. A kernel running unmapped with a stale РЗ
   can fault on its own data.
3. The odd second clause: in supervisor mode with mapping off, physical words **1–7** (the switch
   registers) are exempt. The source comment's *"WTF?"* is a fair reaction; it is the hardware's
   ТО-8 erratum, faithfully reproduced.

### Why the asymmetry is the key to the whole machine

Put the two mapping rules side by side:

* Supervisor code **always executes from physical memory**. It cannot be mapped, ever.
* Supervisor **data access is mapped or not according to БлП**, a bit the kernel controls freely.

So the kernel executes at fixed physical addresses while *independently* choosing whose data it is
looking at. Clear БлП and the kernel's loads and stores run through the **user's** page table — the
one still sitting in РП, because taking an interrupt did not disturb it. Set БлП and they address
physical memory directly.

**That is `copyin` and `copyout`, implemented in hardware, with no page-table switching at all.**
It is the single most useful property of this MMU, and it falls straight out of the fetch/data
asymmetry.

---

## Protection violations and how they are reported

Both protection checks `longjmp` to the trap handler in `sim_instr()`, which converts the stop code
into an interrupt ([besm6_cpu.c:1825](besm6_cpu.c#L1825)):

```c
case STOP_INSN_PROT:
    if (M[PSW] & PSW_INTR_HALT)             /* ПоП */
        goto ret;
    if (RUU & RUU_RIGHT_INSTR) {
        ++PC;
    }
    RUU ^= RUU_RIGHT_INSTR;
    op_int_1 (sim_stop_messages[r]);
    // SPSW_NEXT_RK must be 1 for this interrupt
    M[SPSW] |= SPSW_NEXT_RK;
    GRP |= GRP_INSN_PROT;
    break;
case STOP_OPERAND_PROT:
    ...
    op_int_1 (sim_stop_messages[r]);
    M[SPSW] |= SPSW_NEXT_RK;
    // The offending virtual page is in bits 5-9
    GRP |= GRP_OPRND_PROT;
    GRP = GRP_SET_PAGE (GRP, iintr_data);
    break;
```

### What the handler is told

| ГРП bit | Name | Meaning |
|---------|------|---------|
| **20** | `GRP_OPRND_PROT` | Data access to a closed page (*"Число в чужом листе"*) |
| **14** | `GRP_INSN_PROT` | Instruction fetch from a page with a zero descriptor (*"Команда в чужом листе"*) |
| **13** | `GRP_ILL_INSN` | Privileged instruction in user mode |
| **15** | `GRP_INSN_CHECK` | Fetched word is not tagged as an instruction; also a jump to address 0 |
| **5–9** | `GRP_PAGE_MASK` | **The faulting virtual page number** |

— [besm6_defs.h:453](besm6_defs.h#L453)

**ГРП bits 5–9 are the machine's fault-address register**, and they are the reason a demand-paging
kernel is possible at all. `GRP_SET_PAGE(x,m)` shifts the page number left by 4
([besm6_defs.h:468](besm6_defs.h#L468)); a handler recovers it with `(GRP & 0760) >> 4`.

Note the granularity: you learn the faulting **page**, not the faulting word. That is enough for
demand paging and for growing a stack, but a kernel that wants the exact address must decode the
instruction itself. Note also that the page number is filled in **only for a data violation** — an
instruction-protection fault reports no page, because the handler can work it out from the saved PC.

### The restart protocol — read this before writing a fault handler

Look again at the trap code above. Before vectoring, the CPU **advances past the faulting
instruction**: it bumps `PC` and flips `RUU_RIGHT_INSTR`, then sets `SPSW_NEXT_RK` in the saved
mode word.

**The saved PC therefore points at the instruction *after* the one that faulted, and `выпр` will not
re-execute it.** This is the opposite of what a page-fault handler needs, and nothing in the
hardware corrects it. Backing up is the kernel's job.

Two facts make it possible:

* `SPSW_NEXT_RK` (`001000`, [besm6_defs.h:230](besm6_defs.h#L230)) is set to tell you this happened.
  The Russian gloss is *"на регистр РК принята следующая команда"* — "РК has taken delivery of the
  *next* instruction."
* `SPSW_RIGHT_INSTR` (`000400`) records which half of the 48-bit word faulted, since two 24-bit
  instructions share a word.

So a handler that intends to retry must reconstruct the faulting instruction's position from
`SPSW_NEXT_RK` and `SPSW_RIGHT_INSTR` and rewrite `M[IRET]`/`СПСВ` before returning. Get this wrong
and a demand-paged program will skip an instruction every time it faults — which is exactly the kind
of bug that only shows up under memory pressure.

### Halt instead of interrupt

`PSW_INTR_HALT` (**ПоП**, `000004`) turns internal interrupts into simulator halts, which is what
you want while bringing a kernel up: instead of vectoring to a handler you don't trust yet, the
simulator stops and prints *"Команда в чужом листе"* or *"Число в чужом листе"*.

There is one wrinkle, preserved from the real machine
([besm6_cpu.c:1838](besm6_cpu.c#L1838)):

```c
case STOP_OPERAND_PROT:
#if 0
/* ДИСПАК держит признак ПоП установленным.
 * При запуске СЕРП возникает обращение к чужому листу. */
            if (M[PSW] & PSW_INTR_HALT)             /* ПоП */
                goto ret;
#endif
```

**ПоП is deliberately ignored for data-protection faults.** DISPAK runs with ПоП permanently set and
still relies on taking operand-protection interrupts, so the check is compiled out. An
instruction-protection fault honours ПоП; a data-protection fault does not.

---

## Entering and leaving supervisor mode

All three doors lead to physical page 0, which is why the low pages are reserved.

### Interrupt

```c
void op_int_1 (const char *msg)
{
    M[SPSW] = (M[PSW] & (PSW_INTR_DISABLE | PSW_MMAP_DISABLE |
                         PSW_PROT_DISABLE)) | IS_SUPERVISOR (RUU);
    if (RUU & RUU_RIGHT_INSTR)
        M[SPSW] |= SPSW_RIGHT_INSTR;
    M[IRET] = PC;
    M[PSW] |= PSW_INTR_DISABLE | PSW_MMAP_DISABLE | PSW_PROT_DISABLE;
    ...
    PC = 0500;
    RUU &= ~RUU_RIGHT_INSTR;
    RUU = SET_SUPERVISOR (RUU, SPSW_INTERRUPT);
}
```
— [besm6_cpu.c:1728](besm6_cpu.c#L1728)

The old БлП/БлЗ/БлПр and the old supervisor bits go into **СПСВ**; the PC goes into **ИРЕТ**
(`M[033]`). Then **БлП, БлЗ and БлПр are all forced on** — mapping off, protection off, external
interrupts off — and control lands at physical **`0500`** (internal fault) or **`0501`** (external
interrupt, `op_int_2`, [besm6_cpu.c:1750](besm6_cpu.c#L1750)).

Note the `|=`: an interrupt **ORs** those bits in, so ПоП and ПоК survive.

### Extracode — the system-call gate

```c
/* Адрес возврата из экстракода. */
M[ERET] = nextpc;
/* Сохранённые режимы УУ. */
M[SPSW] = (M[PSW] & (PSW_INTR_DISABLE | PSW_MMAP_DISABLE |
                     PSW_PROT_DISABLE)) | IS_SUPERVISOR (RUU);
/* Текущие режимы УУ. */
M[PSW] = PSW_INTR_DISABLE | PSW_MMAP_DISABLE |
    PSW_PROT_DISABLE | /*?*/ PSW_INTR_HALT;
M[14] = Aex;
RUU = SET_SUPERVISOR (RUU, SPSW_EXTRACODE);

if (opcode <= 077)
    PC = 0500 + opcode;             /* э50-э77 */
else
    PC = 0540 + (opcode >> 3);      /* э20, э21 */
```
— [besm6_cpu.c:1512](besm6_cpu.c#L1512)

The differences from an interrupt matter:

* The return address goes to **ЭРЕТ** (`M[032]`), not ИРЕТ.
* **The effective address is passed to the handler in `M[14]`** — that is the system-call argument
  register.
* ПСВ is **overwritten** (`=`, not `|=`). ПоК and the write-watch bit are *lost* across an
  extracode, unlike across an interrupt.

Vectors: extracodes **э50–э77** land at **`0550`–`0577`**; **э20** and **э21** land at **`0560`** and
**`0561`**. Those overlap — **э20 aliases э60, and э21 aliases э61** — so an OS cannot use both
halves of each pair. `стоп` executed in user mode is quietly re-dispatched as extracode **э63**
([besm6_cpu.c:1666](besm6_cpu.c#L1666)), landing at `0563`.

### Return: `0320 «выпр» / iret`

```c
case 0320:                                      /* выпр, iret */
    Aex = addr;
    if (! IS_SUPERVISOR (RUU)) {
        longjmp (cpu_halt, STOP_BADCMD);
    }
    M[PSW] = (M[PSW] & PSW_WRITE_WATCH) |
        (M[SPSW] & (SPSW_INTR_DISABLE |
                    SPSW_MMAP_DISABLE | SPSW_PROT_DISABLE));
    PC = M[(reg & 3) | 030];
    ...
    RUU = SET_SUPERVISOR (RUU,
                          M[SPSW] & (SPSW_EXTRACODE | SPSW_INTERRUPT));
```
— [besm6_cpu.c:1637](besm6_cpu.c#L1637)

Supervisor-only. It restores БлП, БлЗ and БлПр from СПСВ and restores the supervisor bits from СПСВ
— so **returning to user mode means returning with СПСВ's РежЭ/РежПр clear.** The mode you go back
to is whatever was saved, which is why the kernel can edit СПСВ to control where it lands.

The **index-register field selects the return address register**: `PC = M[(reg & 3) | 030]`, so

| `reg & 3` | Register | Used for |
|-----------|----------|----------|
| 2 | `M[032]` = **ЭРЕТ** | return from an extracode |
| 3 | `M[033]` = **ИРЕТ** | return from an interrupt |

### The vector map

| Physical address | Entered by |
|------------------|------------|
| `0500` | internal interrupt (any fault: protection, check, illegal instruction, overflow) |
| `0501` | external interrupt (any device; also ПРП via ГРП bit 37) |
| `0550`–`0577` | extracodes э50–э77 |
| `0560` / `0561` | extracodes э20 / э21 (aliasing э60 / э61) |

Every fault in the machine funnels through the single vector `0500`. The handler must read ГРП to
find out what happened.

---

## Programming the MMU

РП and РЗ are written by the privileged instruction **`002 «рег»`** (`mod` in the MADLEN dialect),
decoded by `cmd_002()` at [besm6_cpu.c:513](besm6_cpu.c#L513). The register is named by the low bits
of the *effective address*; the data comes from the *accumulator*.

| `Aex & 0377` | Register | Accumulator |
|--------------|----------|-------------|
| `020`–`027` | **РП0**–**РП7** | Four packed page numbers — see below |
| `030`–`033` | **РЗ**, byte 0–3 | Bits 21–28 become 8 protection bits |

```c
case 020: case 021: case 022: case 023:
case 024: case 025: case 026: case 027:
    /* Запись в регистры приписки */
    mmu_setrp (Aex & 7, ACC);
    break;
case 030: case 031: case 032: case 033:
    /* Запись в регистры защиты */
    mmu_setprotection (Aex & 3, ACC);
    break;
```
— [besm6_cpu.c:523](besm6_cpu.c#L523)

Executing `рег` outside supervisor mode raises `STOP_BADCMD`
([besm6_cpu.c:1092](besm6_cpu.c#L1092)).

> ### РП and РЗ are write-only
>
> There is **no read address** for either register. `0220`–`0227` and `0230`–`0233` are not decoded;
> they fall into `cmd_002()`'s `default:` and merely log *"РЕГ %o - неправильный адрес
> спец.регистра"* ([besm6_cpu.c:576](besm6_cpu.c#L576)). Only БРЗ (`0200`–`0207`) and ГРП (`0237`)
> can be read back.
>
> **A kernel can never ask the hardware what the current mapping is.** It must keep its own shadow
> copy of every process's page table in memory and treat РП/РЗ as write-only outputs. This is not a
> simulator limitation; it is how the machine is built.

### Packing of the page registers

One accumulator word writes **four** descriptors at once. The layout is irregular — the low 5 bits
of the four fields are adjacent at the bottom of the word, but their upper bits are scattered across
the top ([besm6_mmu.c:699](besm6_mmu.c#L699)):

```c
/* Младшие 5 разрядов 4-х регистров приписки упакованы
 * по 5 в 1-20 рр, 6-е разряды - в 29-32 рр, 7-е разряды - в 33-36 рр и т.п.
 */
```

Writing `рег 020+i` programs virtual pages `4i`, `4i+1`, `4i+2`, `4i+3`. Calling them p0…p3:

| Field | Virtual page | Bits 1–5 from | Bit 6 | Bit 7 | Bit 8 | Bit 9 | Bit 10 |
|-------|--------------|---------------|-------|-------|-------|-------|--------|
| p0 | `4i` | acc 1–5 | acc 29 | acc 33 | acc 37 | acc 41 | acc 45 |
| p1 | `4i+1` | acc 6–10 | acc 30 | acc 34 | acc 38 | acc 42 | acc 46 |
| p2 | `4i+2` | acc 11–15 | acc 31 | acc 35 | acc 39 | acc 43 | acc 47 |
| p3 | `4i+3` | acc 16–20 | acc 32 | acc 36 | acc 40 | acc 44 | acc 48 |

Each field is then masked to 9 bits (512 physical pages). **Accumulator bits 21–28 are unused by
РП** — and they are exactly the bits РЗ consumes.

### Packing of the protection register

```c
void mmu_setprotection (int idx, t_value val)
{
    /* Разряды сумматора, записываемые в регистр защиты - 21-28 */
    int mask = 0xff << (idx * 8);
    val = ((val >> 20) & 0xff) << (idx * 8);
    RZ = (uint32)((RZ & ~mask) | val);
}
```
— [besm6_mmu.c:737](besm6_mmu.c#L737)

`рег 030+j` takes **accumulator bits 21–28** and installs them as РЗ bits `8j … 8j+7`, i.e. the
protection bits for virtual pages `8j … 8j+7`. Four writes (`рег 030` … `рег 033`) cover all 32
pages.

**Accumulator bit 21 is the *least* significant bit of the byte.** So bit 21 is the protection bit
for page `8j`, and bit 28 is the bit for page `8j+7`:

| Accumulator bit | 21 | 22 | 23 | 24 | 25 | 26 | 27 | 28 |
|-----------------|----|----|----|----|----|----|----|----|
| Virtual page | `8j` | `8j+1` | `8j+2` | `8j+3` | `8j+4` | `8j+5` | `8j+6` | `8j+7` |

It is easy to get this backwards. Closing virtual page 1, for instance, needs accumulator bit **22**
— the value `010000000` — written with `рег 030`, not bit 21.

> **The two packings are complementary by design.** РП uses accumulator bits 1–20 and 29–48; РЗ uses
> bits 21–28. One 48-bit word can therefore carry a quartet of page numbers *and* a byte of
> protection bits at once — which is what an OS's page-table entry looks like in memory, ready to be
> loaded with two `рег` instructions and no shifting.

### Setting БлП and БлЗ

The cheapest way is `0240 «уиа» / vtm` (or `0250 «слиа» / utm`) executed in supervisor mode with
**register field 0** ([besm6_cpu.c:1543](besm6_cpu.c#L1543)):

```c
case 0240:                                      /* уиа, vtm */
    Aex = addr;
    M[reg] = addr;
    M[0] = 0;
    if (IS_SUPERVISOR (RUU) && reg == 0) {
        M[PSW] &= ~(PSW_INTR_DISABLE |
                    PSW_MMAP_DISABLE | PSW_PROT_DISABLE);
        M[PSW] |= addr & (PSW_INTR_DISABLE |
                          PSW_MMAP_DISABLE | PSW_PROT_DISABLE);
    }
```

The mode bits come straight from the **address field**, so `уиа <mask>(0)` sets ПСВ's БлП/БлЗ/БлПр
to `mask` in a single instruction. Only those three bits can be set this way — ПоП, ПоК and the
write-watch bit cannot.

The general route is `040 «уи»`, which in supervisor mode writes any of `M[0]`…`M[037]`, ПСВ
(`M[021]`) included. Use `уиа` for the mode bits and `уи` for everything else (ЭРЕТ, ИРЕТ, СПСВ,
ИБП, ДВП).

### Worked examples

These have been run against the simulator; the octal values are the ones that actually work.

**Map virtual page 1 to physical page 3.** Page 1 is `p1` of РП0 (`1 = 4·0 + 1`), so its low 5 bits
live in accumulator bits 6–10. Physical page 3 fits in those 5 bits, so the whole accumulator is
`3 << 5` = **`0140`**, and the instruction is `рег 020`. Virtual `2000` (page 1, offset 0) then
aliases physical `6000`:

```
sim> d 1000 140
sim> d -m 1 xta 1000, mod 20      ; ACC = 0140 ; рег 020 -> РП0
sim> d -m 2 vtm 2002, xta 2000    ; clear БлП ; read virtual 2000
```

For a page number above 31 the upper bits scatter: page *4i+1* takes bit 6 from accumulator bit 30,
bit 7 from 34, bit 8 from 38, bit 9 from 42, bit 10 from 46. Since РП is write-only you must rebuild
the whole quartet from your shadow copy, OR in the new descriptor, and write it back.

**Close virtual page 1 to data access.** Page 1 is bit 1 of РЗ byte 0, which is accumulator bit
**22** — the value **`010000000`** — written with `рег 030`. Touching virtual `2000` afterwards (with
БлЗ clear) faults with ГРП = `02000020`: bit 20 `GRP_OPRND_PROT`, plus the page number `1` in bits
5–9.

**Close virtual pages 8–15.** Their bits are all of РЗ byte 1: set accumulator bits 21–28, i.e.
**`01774000000`**, and execute `рег 031`.

**Open the whole address space (kernel startup).** Write zero to `рег 030`, `031`, `032`, `033`.
РЗ = 0 means every page is open. Reset already does this
([besm6_mmu.c:247](besm6_mmu.c#L247)).

**Reach user data from the kernel.** `уиа 2(0)` — clear БлП, keep БлЗ set — then load and store
normally: addresses now go through the user's РП while the kernel carries on executing from physical
memory. `уиа 3(0)` to go back. (Add `02000` to either if you want БлПр kept set, i.e. external
interrupts left disabled.) Keeping БлЗ *set* is the point: it lets the kernel touch pages the user
cannot. Clear БлЗ as well if you want the user's own protection enforced against the kernel — a
cheap way to validate a user pointer and turn a fault into `EFAULT`.

---

## Свёртка — the word check tags

Every 48-bit word carries a 2-bit tag, held in bits 49–50 of the simulator's 64-bit `t_value`. The
tag is written from the ПКЛ/ПКП mode bits at the moment of the store and checked on every read
([besm6_defs.h:100](besm6_defs.h#L100)):

```c
/*
 * Работа со сверткой. Значение разрядов свертки слова равно значению
 * регистров ПКЛ и ПКП при записи слова.
 * 00 - командная свертка
 * 01 или 10 - контроль числа
 * 11 - числовая свертка
 * В памяти биты свертки имитируют четность полуслов.
 */
#define PARITY_INSN             1
#define PARITY_NUMBER           2
#define SET_PARITY(x, c)        (((x) & BITS48) | (((c) & 3LL) << 48))
#define IS_INSN(x)              (((x) >> 48) == PARITY_INSN)
#define IS_NUMBER(x)            (((x) >> 48) == PARITY_INSN ||  \
                                 ((x) >> 48) == PARITY_NUMBER)
```

`mmu_store()` tags every word it writes with `SET_PARITY (val, RUU ^ PARITY_INSN)`
([besm6_mmu.c:434](besm6_mmu.c#L434)), where the low two bits of РУУ are ПКЛ and ПКП. The four
combinations:

| ПКЛ | ПКП | Свёртка | A fetch of such a word | A load of such a word |
|-----|-----|---------|------------------------|-----------------------|
| 0 | 0 | **командная** (instruction) | OK | OK |
| 0 | 1 | контроль числа | fails | **fails** → `STOP_RAM_CHECK` |
| 1 | 0 | контроль числа | fails | **fails** → `STOP_RAM_CHECK` |
| 1 | 1 | **числовая** (numeric) | **fails** → `STOP_INSN_CHECK` | OK |

Two of these are useful and two are diagnostic:

* **`00` — командная свёртка** is the normal state. Both ПКЛ and ПКП are clear after reset, so
  ordinary stores produce words that are valid as both code and data. This is what you get unless
  you ask for otherwise.
* **`11` — числовая свёртка** marks a word as **data only**. It loads fine, but fetching it raises
  `STOP_INSN_CHECK` → ГРП bit 15. This is a genuine no-execute facility, though a *per-word* one
  rather than a per-page one, and it is not a substitute for the РП-zero mechanism.
* **`01` and `10`** produce a word that fails *every* read. They exist so that diagnostics can
  deliberately plant a bad check word and confirm the checking circuitry notices.

A failed check on a load raises `STOP_RAM_CHECK` (ГРП bit 21 `GRP_CHECK` plus bit 4
`GRP_RAM_CHECK`, with the offending memory block in bits 1–3), or `STOP_CACHE_CHECK` if the bad word
came from the БРЗ cache (`GRP_CHECK` with bit 4 *cleared*, and the БРЗ number in bits 1–3).

> **The two checks are gated differently.** The RAM check in `mmu_memaccess()` is only performed when
> the MMU has `CHECK_ENB` — i.e. under `set mmu check`
> ([besm6_mmu.c:488](besm6_mmu.c#L488)). The **instruction** check in `mmu_fetch()` is
> **always** performed ([besm6_mmu.c:687](besm6_mmu.c#L687)). So `set mmu nocheck` does not make the
> numeric-convolution no-execute effect go away.

---

## The БРЗ write cache and the БРС prefetch buffer

The BESM-6 sits behind two small buffers. They are mostly invisible, but they are *not* invisible to
an operating system, and both can corrupt a process if mishandled.

### БРЗ — the write cache

Eight words (`BRZ[8]`) with 16-bit address tags (`BAZ[8]`), fully associative, **write-back**, with
a pairwise-LRU priority matrix in `TABST` ([besm6_mmu.c:258](besm6_mmu.c#L258)). A store lands in a
БРЗ line; the line is written to memory only when it is evicted, by `mmu_flush()`
([besm6_mmu.c:310](besm6_mmu.c#L310)).

**Flushing is done by writing to the switch-register addresses.** A store to physical 1–7 with
mapping off does not store anything; it advances the `FLUSH` counter and evicts the oldest line
(`mmu_flush_by_age()`, [besm6_mmu.c:360](besm6_mmu.c#L360)). The first such store only *arms* the
counter (`case 0: break`) — eviction begins with the second — so **nine consecutive stores are
needed to drain all eight lines.** Any ordinary store resets the counter to zero
([besm6_mmu.c:445](besm6_mmu.c#L445)), so the nine must not be interleaved with anything else.

> **The coherence hazard.** `mmu_flush()` recomputes the physical address of the line it is about to
> write back *from the current TLB* ([besm6_mmu.c:319](besm6_mmu.c#L319)):
>
> ```c
> waddr = (waddr > 0100000) ? (waddr - 0100000) :
>     (waddr & 01777) | (TLB[waddr >> 10] << 10);
> ```
>
> A dirty line tagged with a *virtual* address is therefore written back through **whatever mapping
> is loaded when it is finally evicted**, not the one that was loaded when the store happened. If a
> kernel reloads РП while dirty lines are outstanding, those stores land in the **new** process's
> memory.
>
> **A context switch must drain БРЗ before reloading РП.**

### БРС — the instruction prefetch buffer

Four words (`BRS[4]`) with tags (`BAS[4]`) and their own LRU, filled by `mmu_prefetch()`
([besm6_mmu.c:601](besm6_mmu.c#L601)). The CPU speculatively prefetches the next instruction word
each cycle ([besm6_cpu.c:1063](besm6_cpu.c#L1063)).

**БРС is not coherent with БРЗ.** A word written through the write cache can still be fetched stale
from the prefetch buffer. Any kernel that writes instructions and then executes them — `exec`,
signal trampolines, loading an overlay, patching a jump — must ensure the writes have reached memory
and the prefetch buffer no longer holds the old copy.

The speculative prefetch performs **no protection check** — `mmu_prefetch(addr, 0)` is called with
`actual == 0` and translates without consulting РЗ or the РП-zero rule. All checking happens in
`mmu_fetch()`/`mmu_fetch_check()` on the real fetch. This is not a protection hole; nothing
observable escapes.

### What the simulator actually models

`set mmu cache` enables true LRU write-cache modelling; it is **off by default** because it costs
about 20 % in speed. With it off, `mmu_store()` still routes through a БРЗ line but flushes it
immediately ([besm6_mmu.c:427](besm6_mmu.c#L427)), so stores are effectively write-through and the
coherence hazards above cannot bite.

**Do not take that as licence to ignore them.** They are real properties of the hardware, they are
faithfully modelled under `set mmu cache`, and a kernel that only works with the cache off is a
kernel that would not have worked on the real machine. Test with `set mmu cache`.

---

## Notes for an operating-system port

Drawing the above together, in the order a kernel will meet it.

**1. The address space is small.** 32 Kwords, 32 pages, per process — that is the hard ceiling on a
BESM-6 process image, text plus data plus stack. Physical memory is 16× larger, so many processes
fit in core at once, but no single one can exceed 32 K without overlays.

**2. The kernel executes from the low 32 K of physical memory, always.** Supervisor instruction
fetch is unconditionally unmapped, and unmapped addresses are 15 bits. There is no way to run kernel
code above physical `0100000`. Kernel text plus any code reachable from an interrupt must fit below
that line; everything else — buffers, the page pool, user images — lives above it and is reached by
mapping it in.

**3. Unmapped kernel *data* access is also capped at the low 32 K.** To touch physical memory above
`0100000`, the kernel must map it through РП like anyone else, i.e. spend some of its 32 virtual
pages on a window. Plan the virtual layout with that in mind: a kernel that maps user pages into
*every* one of its 32 slots has no window left to reach a buffer with.

**4. `copyin`/`copyout` is free.** Bracket the copy with `уиа 2(0)` … `уиа 3(0)` to clear and
restore БлП. While БлП is clear, the kernel's loads and stores go through the user's page table —
which is still loaded, because taking a trap does not disturb РП — while the kernel keeps executing
from physical memory. No page-table switch, no temporary mapping, no window. This is the mechanism
the whole design hands you, and it is why the fetch/data asymmetry exists.

To *validate* a user pointer at the same time, clear БлЗ as well and let the user's own РЗ bits
fault on an out-of-bounds access; catch the resulting `GRP_OPRND_PROT` and turn it into `EFAULT`.

**5. Keep a shadow page table.** РП and РЗ cannot be read back. Every process's mapping must be held
in kernel memory, and the hardware registers treated as a write-only cache of it. Reloading is
cheap: eight `рег 020+i` and four `рег 030+j` — twelve instructions to switch an address space.

**6. Two protection mechanisms, not one.**
* To make a page **unreadable and unwritable**: set its **РЗ** bit.
* To make a page **non-executable**: set its **РП** entry to **0**.
* These are independent. A page can be executable but unreadable (`РП ≠ 0`, `РЗ` set), or readable
  but non-executable (`РП = 0`, `РЗ` clear — though it then aliases physical page 0, so this is a
  trap, not a feature).
* There is **no read/write distinction**. A page cannot be made read-only. Copy-on-write is
  therefore not directly implementable: the best available approximation is to close the page
  entirely with РЗ and let the fault handler distinguish read from write by decoding the faulting
  instruction. Plan for `fork()` to copy, or to be `vfork()`.
* **Never hand physical page 0 to a user process as text.** It is indistinguishable from "unmapped".

**7. The fault handler reads ГРП.** All faults vector to `0500`. Read ГРП with `рег 0237`; bit 20 is
a data violation with the **faulting page in bits 5–9**, bit 14 an instruction violation, bit 13 an
illegal instruction. Clear the handled bits with `рег 037` (an AND-mask: a **zero** in the
accumulator clears the bit).

**8. Fix the PC before returning from a fault.** The saved PC points *past* the faulting
instruction, `SPSW_NEXT_RK` says so, and `SPSW_RIGHT_INSTR` says which 24-bit half faulted. A
demand-paging handler must back it up by hand. See
[the restart protocol](#the-restart-protocol--read-this-before-writing-a-fault-handler).

**9. Drain БРЗ on every context switch, before reloading РП.** Otherwise dirty lines from the old
process are written back through the new process's mapping. Nine consecutive stores to physical 1–7
with mapping off will do it.

**10. Watch the extracode vector aliases.** э20/э60 and э21/э61 share vectors `0560`/`0561`. Pick
one of each pair as the system-call trap and leave the other alone.

**11. Interrupts and extracodes save state differently.** An extracode *overwrites* ПСВ (losing ПоК
and the write-watch bit); an interrupt *ORs* the disable bits in. And an extracode returns via ЭРЕТ
(`выпр` with reg ≡ 2), an interrupt via ИРЕТ (reg ≡ 3). A single `выпр` in a shared trap-exit path
must therefore know which door it came in by.

**12. There is exactly one saved-state slot.** СПСВ, ЭРЕТ and ИРЕТ are single registers, not a
stack. Interrupts are disabled on entry (БлПр is forced on), and a second internal fault while
handling the first is fatal — `STOP_DOUBLE_INTR`. The kernel must save СПСВ/ИРЕТ to its own stack
before it does anything that can fault, and before it re-enables interrupts.

**13. Hardware debug registers exist.** ИБП (`M[034]`, КРА) is an execute breakpoint and ДВП
(`M[035]`, ЗПСЧ) is a data watchpoint, with `PSW_WRITE_WATCH` selecting write- or read-match. They
raise ГРП bits 12, 16 and 17. These are the natural foundation for `ptrace`. Remember that they
match the *tagged* address, so they follow the current mapping mode.

---

## Reset state

```c
RUU = RUU_EXTRACODE | RUU_AVOST_DISABLE;
for (i=0; i<NREGS; ++i)
    M[i] = 0;
...
/* Регистр 17: БлП, БлЗ, ПОП, ПОК, БлПр */
M[PSW] = PSW_MMAP_DISABLE | PSW_PROT_DISABLE | PSW_INTR_HALT |
    PSW_CHECK_HALT | PSW_INTR_DISABLE;
```
— [besm6_cpu.c:393](besm6_cpu.c#L393); and `mmu_reset()` zeroes `RP[]` and `RZ`
([besm6_mmu.c:238](besm6_mmu.c#L238)).

A bootstrap therefore inherits:

| | |
|---|---|
| **Mode** | **Supervisor** (РежЭ set) — the machine comes up in extracode mode |
| **БлП, БлЗ** | set — mapping and protection both **off** |
| **БлПр** | set — external interrupts **off** |
| **ПоП, ПоК** | set — any fault **halts** the simulator rather than vectoring |
| **РП, РЗ** | all **zero** |
| **All `M[]`** | zero |

Two consequences. First, the kernel starts executing at physical addresses with no mapping in
force, which is exactly where it will spend the rest of its life. Second, **every РП entry is zero,
so every virtual page is non-executable** — any attempt to enter user mode before programming РП
faults immediately with `GRP_INSN_PROT`. Programming РП is not optional setup; it is the thing that
makes user mode possible at all.

`sim_instr()` copies РП into the TLB at the start of every run (`mmu_setup()`,
[besm6_cpu.c:1776](besm6_cpu.c#L1776)), so a `deposit mmu РП0 …` from the SIMH console takes effect
on the next `run`.

---

## See also

* [README.md](README.md) — building and running the simulator, the register list, `set mmu` options,
  tracing and debugging.
* [Peripherals.md](Peripherals.md) — the other half of `002 «рег»`: ГРП, МГРП and the БРЗ cache
  addresses, plus the whole of `033 «увв»` and the device interrupt bits.
* [besm6_mmu.c](besm6_mmu.c) — the MMU itself: translation, protection, the caches.
* [besm6_defs.h](besm6_defs.h) — the ПСВ, СПСВ, РУУ and ГРП bit definitions.
* [besm6_cpu.c](besm6_cpu.c) — `cmd_002()`, the trap-to-interrupt handler in `sim_instr()`, and the
  supervisor entry/exit paths.
