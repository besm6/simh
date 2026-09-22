# Tracing in the SVS simulator

The SVS processor can log every instruction it executes, together with the
machine state it touches. Tracing uses the standard SIMH debug mechanism:
one switch selects *where* the output goes, another selects *what* is traced.

---

## 1. Quick start

```
sim> set debug out.trace        ; where the trace goes
sim> set cpu0 debug             ; what to trace - everything
sim> dep PC 32000
sim> step 20
```

Two switches, both required. `set cpu0 debug` alone produces nothing if no
debug file is open — see [§7](#7-pitfalls).

---

## 2. Where the output goes

| Command | Effect |
|---|---|
| `set debug <file>` | open `<file>` for debug output (appends) |
| `set debug -n <file>` | same, but truncate first |
| `set debug log` | send the trace to the **console log** opened by `set console log=` |
| `set debug stdout` / `stderr` | send it to the terminal |
| `set nodebug` | close the debug file; tracing stops producing output |

`set debug log` is what the `.ini` scripts in this directory use:

```
set console log=dispak.trace
set debug log
```

Both streams then become one file, so the machine's own console output stays
interleaved with the trace in the order it happened. **Order matters**: the
console log must be opened first, or `set debug log` fails with `SCPE_ARG`.

Do *not* write `set console log=X` followed by `set debug X` — that opens the
same path twice with two buffers and shreds the output.

---

## 3. What to trace — the debug flags

`cpu0`…`cpu3` each carry five independent flags:

| Flag | Traces |
|---|---|
| `INSN` | machine instructions, interrupts, and the РЕГ-instruction explanations |
| `EXTRA` | extracodes only (э50…э77 except э75, plus э20 and э21) |
| `REGS` | changed registers and operand memory reads/writes |
| `FETCH` | instruction fetches |
| `DEV` | channel (ПВВ), disk and МПД exchanges |

```
set cpu0 debug                  ; all five flags - the full trace
set cpu0 debug=insn             ; instructions only
set cpu0 debug=insn;regs        ; ...plus registers and memory
set cpu0 debug=extra            ; only extracodes - a cheap call trace
set cpu0 debug=dev              ; only channel/device traffic
set cpu0 nodebug=fetch          ; drop one flag, keep the rest
set cpu0 nodebug                ; tracing off
show cpu0 debug                 ; => Debug=INSN;EXTRA;REGS;DEV
```

The separator is a semicolon, not a comma. Flag names are case-insensitive on
input and printed uppercase by `show`.

---

## 4. Narrowing by address

The instruction trace can be limited to a range of `PC` values:

```
sim> set cpu0 window=76000:77777    ; octal, inclusive; ":" or "-" as separator
sim> set cpu0 nowindow              ; remove the limit
sim> show cpu0 window               ; => trace window 76000:77777
```

The window applies to everything that is *per-instruction* (`INSN`, `EXTRA`,
`REGS` register dumps, `FETCH`). It does **not** apply to operand memory
accesses or to `DEV` lines, which have no meaningful `PC` of their own.

The window is global, not per-processor.

---

## 5. Line formats

A short fragment with `set cpu0 debug`:

```
cpu0       Write M21 = 02017
cpu0       Write M27 = 02007
cpu0       Write RUU = 044
cpu0       Write POP = 0000 0400 0000 0000
cpu0       Fetch [00100 0000100] = 35:00 000 0200 01 050 0012
cpu0 00100 0000100 L: 00 000 0200  зп 200
cpu0       Memory Write [00200 0000200] = 35:0000 0000 0000 0000
cpu0 00100 0000100 R: 01 050 0012  э50 12(1) = 12
cpu0       Write M16 = 00012
cpu0       Write M32 = 00101
cpu0 ----- 00550L: Контроль команды -----
```

Every line begins with the processor it came from (`cpu0`…`cpu3`, or `iom0`…`iom3`
and `disk` for device lines). Everything is octal unless noted.

### Instruction

```
cpu0 00100 0000100 R: 01 050 0012  э50 12(1) = 12
     |     |       |  |            |          |
     |     |       |  |            |          executive address (extracodes only)
     |     |       |  |            disassembled mnemonic
     |     |       |  octal instruction fields: register, opcode, address
     |     |       L = left / R = right half of the double-command word
     |     physical address of the instruction word
     virtual address (СчАС)
```

Both the virtual and the physical address are shown, so a bad приписка is
visible at a glance. The ` = <addr>` suffix appears only for short-form
extracodes (э50…э77) and gives the executive address after index-register and
`М[МОД]` modification.

### Register change

Only registers that *changed* since the previous instruction are printed:

```
cpu0       Write ACC = 7777 7777 7777 7777
cpu0       Write M1 = 30000
cpu0       Write RAU = 10
```

All processor state is covered: `ACC`, `RMR`, the whole `M0`…`M35` file, `RAU`,
`RUU`, the mapping registers `RP0`…`RP7` (user) and `RPS0`…`RPS7` (supervisor),
the protection register `RZ`, the fault address `EADDR`, the tag register `TAG`,
the interprocessor registers `PP`/`OPP`/`POP`/`OPOP`/`RKP`, the internal
interrupt register `RPR`, and `GRVP`/`GRM`.

48-bit values print as four groups of four octal digits, 32-bit ones as
`%03o %04o %04o`.

The left/right half-word bit of `RUU` is deliberately *not* reported: it flips
on every instruction and the `L`/`R` marker already shows it.

After a `reset`, the first register block dumps the whole non-zero startup
state (the four `Write` lines at the top of the fragment above).

### Memory read/write

```
cpu0       Memory Write [00200 0000200] = 35:0000 0000 0000 0000
cpu0       Memory Read [32011 0032011] = 36:0000 0100 0000 0001
```

Virtual address, physical address, tag, then the value. Addresses 1–7 in user
mode are the console switch registers and print differently:

```
cpu0       Read  TR3 = 0000 0000 0000 0000
```

64-bit words print as five groups, splitting off the low tag field:
`%04o %04o %04o %04o:%02o %04o`.

### Instruction fetch

```
cpu0       Fetch [00100 0000100] = 35:00 000 0200 01 050 0012
```

Printed once per instruction *word* (on the left half only). Tag `35` is a
command word, `36` is a data word — a `36` here is about to raise
«контроль команды».

### Exception or interrupt

```
cpu0 ----- 00550L: Контроль команды -----
cpu0 ----- 33012L: Внутреннее прерывание -----
cpu0 ----- 33012L: Внешнее прерывание: ГРВП=0010 ГРМ=0774 (доставлено 0010) -----
```

Address, half-word marker, and the reason. Internal faults, external
interrupts and simulator stops all use this form.

### Device (`DEV`)

```
iom0 --- Сброс ПВВ
iom0 --- СТБАК: КОП=04 ИБАК СБ=0 НУС=0 адрес=100100 слово=0x4000008040
iom0 --- Команда БАКПВВ@100100: КОП=05 ИТУС СБ=0 НУС=0 адрес=100360 слово=0x50000080f0
disk ---   зона 0462: СС[0]=... — ОС ждёт 0462, будет ОШЗОНЫ
cpu0 --- МПД приём слога 0x0141
```

Device tracing is keyed on **cpu0**'s `DEV` flag, whichever processor or channel
produces the line.

### РЕГ explanations (`INSN`)

The `РЕГ` instruction (`cmd_002`) explains itself in plain Russian:

```
cpu0 --- Чтение ЗЗ (секции памяти, запретов нет)
cpu0 --- Установка часов: 000000000000000
cpu0 --- Прерывание процессорам, маска 0400
```

### Protection faults

Reported through `svs_debug()`, so they reach **stdout as well as** the log —
they stay visible even without a debug file:

```
--- (05412) защита числа
--- (32010) контроль команды: физ.0000010 тег=036 слово=0000000000000000
```

---

## 6. Practical recipes

**Long run, small trace.** A per-instruction trace grows about 3 MB/s, so a
20-million-instruction boot produces roughly a gigabyte. Trace the channel only:

```
set console log=dispak.trace
set debug log
set cpu0 debug=dev
```

This is what `svs.ini`, `dispak.ini`, `int.ini` and `hybrid.ini` do.
`runsim.sh` adds a watchdog that kills the simulator once the trace passes a
size limit.

**Zoom in on one routine.** Start with the cheap trace, then switch on the
expensive one from the `sim>` prompt, limited to the code you care about:

```
sim> set cpu0 debug=insn;dev
sim> set cpu0 window=76000:77777
sim> cont
```

**Bounded full trace.** Combine `set cpu0 debug` with a breakpoint or a
counted `step`, never with an open-ended `go`:

```
sim> set debug -n out.trace
sim> set cpu0 debug
sim> br 33543
sim> go 30036
```

**Who calls whom.** `set cpu0 debug=extra` prints one line per extracode with
its executive address — a compact call trace.

---

## 7. Pitfalls

- **`set cpu0 debug` without `set debug <file>` silently does nothing.** Every
  trace test is gated on `sim_deb`, so with no debug file open the flags are
  set and no output appears. `show cpu0 debug` will still report them.
- **`set debug` needs an argument.** Unlike `set console log`, a bare
  `set debug` is an error.
- **`set debug <file>` appends** and writes a version banner at the top. The
  `.ini` scripts `! rm -f` the file first; `set debug -n <file>` does the same.
- **Flags are separated by `;`.** `debug=insn,dev` fails with
  `Non-existent parameter - DEV`.
- **`set cpu0 debug` includes `FETCH`**, which roughly doubles the line count.
  The BESM-6 tracer omits fetches; `set cpu0 nodebug=fetch` matches it.
- **Device lines follow cpu0 only.** `set cpu1 debug=dev` has no effect.
- **Only cpu0 executes.** `sim_instr()` still runs `cpu_core[0]` alone
  (the other processors are a TODO), so `cpu1`…`cpu3` produce no instruction
  trace even with their flags set. Their flags are already wired, so they will
  work once multiprocessing lands.

---

## 8. Implementation notes

The tracer lives in [svs_trace.c](svs_trace.c); the flags and the gating macros
are in [svs_defs.h](svs_defs.h):

```c
#define CPU_DEB(cpu, bits)       (sim_deb && (cpu_dev[(cpu)->index].dctrl & (bits)))
#define CPU_TRACE(cpu, bits)     (CPU_DEB(cpu, bits) && TRACE_IN_WINDOW((cpu)->PC))
#define SVS_DEV_TRACE()          (sim_deb && (cpu_dev[0].dctrl & DEB_DEV))
```

Use `CPU_TRACE` for anything tied to the current instruction and `CPU_DEB` for
everything else (memory accesses, reset messages).

Hook points:

| Where | What |
|---|---|
| `cpu_one_instr()` | `svs_trace_opcode()` — the instruction line |
| end of `cpu_one_instr()`, entry to `sim_instr()` | `svs_trace_registers()` — the diff |
| `mmu_store/store64/load/load64()` | `svs_trace_memory()` / `svs_trace_memory64()` |
| `mmu_fetch()` | `svs_trace_fetch()` |
| `setjmp` block in `sim_instr()`, interrupt dispatch | `svs_trace_exception()` |
| `cpu_reset()` | `svs_trace_reset()` — clears the register snapshot |

**One rule when adding trace output:** write to `sim_deb` with `fprintf()` and
nothing else. [scp.h](../scp.h) does `#define fprintf Fprintf`, and SIMH's
`Fprintf()` buffers partial lines destined for `sim_deb`. A bare `vfprintf()`,
`fputs()` or `putc()` bypasses that buffer and its text surfaces *ahead* of the
line it belongs to. This is why `svs_trace_exception()` formats through
`vsnprintf()` into a local buffer before printing, and why `svs_log()`,
`svs_log_cont()` and `svs_debug()` in [svs_sys.c](svs_sys.c) do the same — under
`set debug log` the log and the debug stream are the same file.

Registers are diffed against a snapshot kept in `static CORE cpu_state[NUM_CORES]`;
`svs_trace_reset()` zeroes the entry so the first dump after a reset shows the
initial state.
