# BESM-6 simulator

This directory contains a [SIMH](http://simh.trailing-edge.com/)-based simulator of the
**BESM-6** (БЭСМ-6), the most widely used Soviet mainframe of the 1960s–80s. The BESM-6 is a
48-bit machine with sign-magnitude floating-point arithmetic. This guide explains how to
build the simulator, load and run BESM-6 software, attach peripherals, drive the machine from its
front panel, and debug what happens inside.

The simulator ships with everything needed to boot the classic **DISPAK** (ДИСПАК) batch
operating system and run job decks through it — you only need to supply the system disk images
(see [Booting DISPAK](#booting-dispak)).

> **A note on Cyrillic.** The real machine, its operating system, and its assembler mnemonics are
> all in Russian. The simulator exposes register names in Cyrillic *and* Latin, accepts both the
> BEMSH (БЕМШ) and MADLEN mnemonic dialects, and lets terminals type Cyrillic from a Latin
> keyboard. You can drive the whole thing from a plain ASCII terminal.

---

## Table of contents

1. [Building](#building)
2. [Running the simulator](#running-the-simulator)
3. [Machine model and word format](#machine-model-and-word-format)
4. [Registers](#registers)
5. [Loading and dumping programs](#loading-and-dumping-programs)
6. [The symbolic assembler and disassembler](#the-symbolic-assembler-and-disassembler)
7. [CPU and MMU options](#cpu-and-mmu-options)
8. [The front panel](#the-front-panel)
9. [Peripherals](#peripherals)
10. [Terminals (TTY)](#terminals-tty)
11. [Debugging and tracing](#debugging-and-tracing)
12. [Worked examples](#worked-examples)
13. [Output files](#output-files)
14. [SIMH command quick reference](#simh-command-quick-reference)

---

## Building

Build from the **repository root** (one level up from this directory):

```sh
make besm6
```

This produces the binary `BIN/besm6`. The build uses `-DUSE_INT64` (the 48-bit word needs a
64-bit integer type) and pulls in SDL2 video plus SDL_ttf font support, which are required only
for the optional graphical [front panel](#the-front-panel). The link step runs an internal
register sanity check on success.

See the repository `CLAUDE.md` for more build details.

---

## Running the simulator

The general form of invocation is:

```sh
besm6 [-flags] [scriptfile [arguments...]]
```

Run each script **from the directory that holds its data files**, because scripts refer to their
data files by bare relative names (`drum1x.bin`, `boot_dispak.b6`, `input.txt`, …). The
interactive demos live in `BESM6/demo/`; the regression tests live in `BESM6/tests/`:

```sh
cd BESM6/demo  && ../../BIN/besm6 dispak.ini        # boot the DISPAK operating system
cd BESM6/tests && ../../BIN/besm6 besm6_test.ini    # run the whole regression suite
```

`make besm6` runs the regression suite automatically after a successful build.

With no script file, the simulator starts interactively at the `sim>` prompt. Type `help` for
the command list and `q` (or `quit`) to leave:

```
$ ../BIN/besm6
БЭСМ-6 simulator V4.0-0 Beta
sim> help
sim> quit
Goodbye
```

If a script file is given, it is executed (with any arguments) and then the simulator drops into
interactive mode — unless the script ends with `quit`.

### Command-line flags

| Flag | Meaning |
|------|---------|
| `-v` | Echo each script line before executing it (verbose). |
| `-e` | Abort the script on the first command that returns an error. |
| `-q` | Quiet: less chatter, and no startup banner. |
| `-o` | The invoked script inherits the `on` (error-handler) state of its caller. |

### The `simh.ini` startup script

At startup the simulator looks for an initialization script and runs it before anything else:
first `$HOME/simh.ini`, then `./simh.ini` in the current directory. Use it for settings you want
applied to every session.

---

## Machine model and word format

* **Word size.** 48 data bits plus a 2-bit instruction/number tag, so SIMH reports a **50-bit**
  register width (the default octal view shows the 48 data bits as four 12-bit groups). The tag
  marks each word as either an *instruction* or a *number*; loading a data-tagged word as an
  instruction raises a machine check.
* **Radix.** Everything is **octal** by default — addresses, data, opcodes. This document (and
  the simulator's output) uses octal throughout unless noted.
* **Memory.** 512 K words (`512 * 1024` = 262 144 octal `1000000` words).
* **Addresses.** 15 bits.
* **Switch registers.** Octal addresses `1`–`7` are not main memory; they are the front-panel
  *switch registers* (see [The front panel](#the-front-panel)). A word loaded or deposited at an
  address below `10` (octal) goes there instead of RAM.
* **Floating point.** Sign-magnitude, base-2 exponent. Use the `-f` switch to view a word as a
  real number (see below).

---

## Registers

BESM-6 registers have Cyrillic names, and those names appear in disassembly, traces, and the
graphical panel. For **typing** commands, however, use the **Latin** synonyms: each register is
also exposed through a pseudo-device called `REG`, and the command parser matches the Latin names
reliably while the Cyrillic ones may not parse. So `examine ACC` (or `examine REG ACC`) reads the
accumulator; `examine СМ` may report "Non existant register".

| Cyrillic | Latin | Width | Meaning |
|----------|-------|-------|---------|
| `СчАС` | `PC`  | 15 | Program counter (this is `sim`'s PC; `RUN addr`/`GO addr` set it). |
| `РК`   | `RK`  | 24 | Current instruction register. |
| `Аисп` | `Aex` | 15 | Effective (executed) address. |
| `СМ`   | `ACC` | 48 | Accumulator / adder. |
| `РМР`  | `RMR` | 48 | Low-order-bits register. |
| `РАУ`  | `RAU` | 6  | ALU mode bits (binary radix). |
| `М1`…`М17` | `M1`…`M17` | 15 | Index / modifier registers (М17 is also the stack pointer). |
| `М20`  | `M20` | 15 | Address modifier (MOD). |
| `М21`  | `M21` | 15 | Program-status / control-unit modes. |
| `М27`  | `M27` | 15 | Saved modes (SPSW). |
| `М32`–`М33` | `M32`–`M33` | 15 | Extracode / interrupt return addresses. |
| `М34`  | `M34` | 16 | Instruction breakpoint register (hardware KRA). |
| `М35`  | `M35` | 16 | Data watchpoint register. |
| `РУУ`  | `RUU` | 9  | Execution-mode bits (binary radix). |
| `ГРП` / `МГРП` | `GRP` / `MGRP` | 48 | Main interrupt register and its mask. |
| `ПРП` / `МПРП` | `PRP` / `MPRP` | 24 | Peripheral interrupt register and its mask. |

The `REG` pseudo-device additionally exposes internal machine state for the curious: the
write-cache/TLB registers `BRZ0`–`BRZ7` and `BAZ0`–`BAZ7`, page table `TABST`/`RP0`–`RP7`/`RZ`,
and the front-panel words `FP1`–`FP7`.

Registers are read and written with the standard SIMH `EXAMINE` / `DEPOSIT` / `SET` commands and
can be used in `ASSERT` conditions (e.g. `assert ACC==0`).

---

## Loading and dumping programs

The simulator loads programs from a **text** file in the same format used by the DISPAK
operating system. This is the `.b6` format used by `demo/boot_dispak.b6`, `tests/alu.b6`, and the
other bundled programs. (`load` also auto-detects binary `a.out` executables; see `tests/aout/`.)

Load and start:

```
sim> load file.b6
sim> run 2000            ; start execution at octal address 2000
```

Each line of the file begins with a one-letter type code (Cyrillic or its Latin equivalent,
case-insensitive), followed by octal operands. Lines beginning with `;` are comments; blank
lines are ignored.

| Code (Cyrillic / Latin) | Meaning |
|-------------------------|---------|
| `в` / `b` | Set the current load **address** (octal). Loading starts at address 1. |
| `п` / `p` | Set the **start address** (stored into the PC). |
| `ч` / `f` | A **floating-point** number, converted to BESM-6 format. |
| `с` / `c` | An octal **data word** (up to 16 octal digits; groups may be space-separated). |
| `к` / `k` | One or two **machine instructions** (two half-word commands, comma-separated). |

Each stored word advances the load address by one. Words at addresses below `10` (octal) are
placed in the switch registers rather than RAM.

Example (`file.b6`):

```
в 1
к сл  7,  зп   11
к вчп 11, зп   10
к умн 10, дел  10
к вч  10, слпа 145
к пе  6,  стоп
в 6
к сч  11, пб   1
в 7
ч 1.0
с 0
п 1
```

To write all of memory back out in the same text format:

```
sim> dump output.b6
```

---

## The symbolic assembler and disassembler

`EXAMINE` and `DEPOSIT` understand BESM-6 instructions and number formats through command-line
switches.

### Address ranges

The first argument to `examine`/`deposit` selects what to show:

* `addr` — a single word,
* `addr-addr` — an inclusive range,
* `addr/count` — `count` words starting at `addr`.

### Display formats

| Switch | Format |
|--------|--------|
| *(none)* | Four 12-bit octal groups (the default). |
| `-m`  | Disassemble as **БЕМШ** (Cyrillic) mnemonics. |
| `-ml` | Disassemble as **MADLEN** (Latin) mnemonics. |
| `-i`  | Octal instruction fields (register / opcode / address). |
| `-f`  | Interpret the word as a BESM-6 floating-point number. |
| `-b`  | Six octal bytes. |
| `-x`  | 13 hexadecimal digits. |

For example, the same four words shown four ways:

```
sim> ex 32020/4
32020:  0000 2000 0000 0210
...
sim> ex -m 32020/4          ; БЕМШ mnemonics
32020:  сч,
        сч 4412(1)
...
sim> ex -ml 32020/4         ; MADLEN mnemonics
32020:  xta,
        xta 4412(1)
...
sim> ex -f 32020/4          ; as floating-point numbers
32020:  2.7e-20
...
```

### Instruction syntax (for `DEPOSIT`)

Each machine word holds two half-word commands, written left-command `,` right-command. A
command may be written either as raw octal fields or as a mnemonic:

* **Octal:** `register opcode address`, e.g. `01 012 4412`.
* **Mnemonic:** `mnemonic [-]address [(register)]`. A leading `-` gives a negative offset; an
  index/modifier register is written in parentheses. Examples: `vtm 1777(1)`, `atx (1)`,
  `utm -1(1)`.

`DEPOSIT` auto-detects the format: plain octal (up to 16 digits), octal instruction fields,
BEMSH mnemonics, or MADLEN mnemonics. These four lines all store the same word:

```
sim> d 1 1234567                       ; octal data word
sim> d 2 01 012 4412, 01 26 02023      ; octal instruction fields
sim> d 3 нтж 4412(1), по 2023(1)       ; БЕМШ mnemonics
sim> d 4 aex 4412(1), uza 2023(1)      ; MADLEN mnemonics
```

---

## CPU and MMU options

Configure the processor with `SET CPU`:

| Command | Effect |
|---------|--------|
| `set cpu idle` / `set cpu noidle` | Enable/disable idle detection, so the host sleeps instead of busy-spinning when the guest is idle. |
| `set cpu autotime=on` / `=off` | Automatically perform the front-panel date/time setup DISPAK expects at boot, so you don't have to key it in. |
| `set cpu req` | Raise a front-panel *request* interrupt (sets `GRP` bit 32) — the software equivalent of pressing the operator "request" button. |
| `set cpu pult=n` | Select the boot source: `0` = switch registers; `1`–`10` = one of the hardwired bootstrap programs; a value above 10 is treated as a filename to dump "touched" instructions to. |
| `set cpu panel[=fontfile]` / `set cpu nopanel` | Open/close the graphical [front panel](#the-front-panel). |

The memory-management unit is a separate device, `MMU`:

| Command | Effect |
|---------|--------|
| `set mmu cache` / `set mmu nocache` | Enable/disable true LRU write-cache modelling. Accurate but ~20 % slower; off by default. |
| `set mmu check` / `set mmu nocheck` | Enable/disable parity checking. |

> **Programming the MMU.** This section covers how to *configure* the simulated MMU. For how a
> program *drives* it — the page registers РП and the protection register РЗ, virtual-to-physical
> translation, how protection differs between user and supervisor mode and between instruction fetch
> and data access, and how a protection fault is reported — see
> **[Memory_Mapping.md](Memory_Mapping.md)**.

---

## The front panel

The operator interacts with the real machine through **switch registers** — octal addresses
`1`–`7` — and a **request** button. In the simulator you deposit into the switch registers and
then raise the request:

```
sim> d 6 1          ; switch register 6 := 1
sim> d 5 10         ; switch register 5 := octal 10
sim> set cpu req    ; press the "request" button
```

The operating system reads registers 5 and 6 as a console command code (which device to read,
which unit to bring online, etc.). This is exactly how `punchtape.ini` feeds requests to DISPAK.

### The graphical panel

`set cpu panel` opens an 800×450 SDL window showing the machine's blinkenlights: the 16 index
registers, the interrupt registers `ГРП`/`МГРП` and `ПРП`/`МПРП`, the program counter, and the
eight cache registers, with three lamp-brightness levels. `set cpu nopanel` closes it.

The panel needs SDL and SDL_ttf; if they are unavailable the command reports an error telling you
to install them. A TrueType font is located automatically from the usual system font directories,
or you can name one explicitly: `set cpu panel=/path/to/DejaVuSans.ttf`.

---

## Peripherals

Devices are connected to host files (or TCP ports) with the SIMH `ATTACH` command. Common attach
switches: `-n` creates a new/empty image, `-e` requires an existing file, `-t` selects text mode,
`-r` attaches read-only. Most peripheral devices are **disabled by default** and are enabled
implicitly when you attach or `set` them.

> **Programming the peripherals.** This section covers how to *connect* a device. For how a program
> *drives* one — the `033` «увв» I/O instruction and its full address map, each device's control
> word and bit fields, and the ГРП/ПРП interrupt bits — see **[Peripherals.md](Peripherals.md)**.

Disk and drum images share the same on-disk geometry: storage is divided into *zones* of
`8 + 1024` words (8 service words followed by 1024 user words), and each word is written as an
8-byte little-endian record.

### Magnetic disks — `MD0`…`MD7`

Eight controllers, each with 8 units, addressed `md`⟨controller⟩⟨unit⟩ — e.g. `md00`…`md07` are
the units of controller 0.

```
sim> attach -n md06 2052.bin        ; create and format a fresh scratch disk
sim> attach -e md07 sbor2053.bin    ; mount an existing system disk
```

When you attach with `-n`, the image is **formatted**, and the volume number is taken from the
digits in the filename — it **must be in the range 2048–4095**, otherwise the attach is rejected
and the file removed. So `2052.bin` becomes volume 2052.

| Command | Effect |
|---------|--------|
| `set mdN ec-5052` | 7.25 MB drive type (1000 blocks). |
| `set mdN ec-5061` | 29 MB drive type (4000 blocks). |
| `set mdN syslog=file` / `=off` | Log accesses to the system volume (2053) to a file. |

(The drive type can only be changed while no unit of that controller is attached.)

### Magnetic drums — `DRUM`

One device with two units, `drum0` and `drum1`. Used for fast paging/swap storage.

```
sim> attach -n drum0 drum1x.bin
sim> attach -n drum1 drum2x.bin
```

### Line printer — `PRN`

Two units, `prn0` and `prn1` (АЦПУ drum printers). Attach to an output file:

```
sim> attach -n prn0 output.txt
```

The output file is **UTF-8 text**. GOST-10859 character codes are converted to Unicode, so
special mathematical glyphs appear directly: `⏨` (decimal exponent), `⩽`, `⩾`, `≠`, `≡`, `÷`,
`°`, and so on. Over-printing (multiple hammer strikes in one column position) is rendered as
several sub-lines joined by carriage returns, so a viewer shows the overstruck result.

### Punch-tape reader (input) — `FS`

Two units, `fs0` and `fs1` (photo-reader, фотосчитыватель). This is where job decks enter the
machine.

```
sim> attach -t fs0 input.txt        ; text-mode tape
```

With `-t` the file is read as **UTF-8 text** and converted to GOST-10859 with odd parity (UPP
code); without `-t`, it is read as a raw binary tape image. FIFO/pipe inputs are supported, so
you can stream a tape into a running machine. See `input.txt` for a complete DISPAK job deck
(FORTRAN + assembler, compile-link-execute).

### Punch-tape punch (output) — `PL`

Two units, `pl0` and `pl1` (PL-80 punch). The attached file receives one raw byte per punched
frame — a binary tape image.

### Card punch (output) — `PI`

Two units, `pi0` and `pi1`. The output representation is chosen by an attach switch:

| Switch | Output form |
|--------|-------------|
| `-v` | Unicode Braille visualisation (**default**). |
| `-b` | Raw binary, 120 bytes per card. |
| `-d` | ASCII "dots and O's" (80 columns × 12 rows). |
| `-g` / `-u` | Interpret each card as GOST/UPP text; fall back to Braille if it fails the parity check. |

### Card reader (input) — `VU`

Two units, `vu0` and `vu1` (600 cards/min). Attach a **UTF-8 text file**; each line becomes a
card (up to 120 characters). "Pretty-card" ASCII art (`.`/`O`) is also recognised.

| Command | Effect |
|---------|--------|
| `set vu coldly=N` | Inter-column interrupt delay. |
| `set vu enddly=N` | End-of-card signal duration. |
| `set vu carddly=N` | Delay before the next card. |
| `set vuN updk=start-end` | Convert the given card range using the column-wise UPDK code. |

### Magnetic tape — `MG3`…`MG6`

Four controllers (channels 3–6), 8 units each, addressed `mg3`⟨unit⟩ … `mg6`⟨unit⟩. Tapes use
the standard SIMH `.tap` format.

```
sim> attach -n mg30 tape1.tap       ; create and format a new tape
sim> attach -r  mg31 archive.tap    ; mount read-only
```

With `-n` the tape is formatted and its volume number (from the filename) **must be 1–2047**.

---

## Terminals (TTY)

The `TTY` device is a terminal multiplexer with **24 serial lines** (`tty1`…`tty24`) plus **two
parallel "Consul" typewriter lines** (`tty25`, `tty26`). Line 1 is normally the operator console.

### Attaching a line

```
sim> attach tty1 4199         ; listen for telnet connections on TCP port 4199
sim> attach tty1 console      ; bind line 1 to the local SIMH console (Unix hosts)
sim> attach tty Line=1,4198   ; TMXR per-line form (e.g. a UTF-8 console over telnet)
sim> attach tty1 none         ; mark the line permanently unusable
```

A new telnet connection is greeted with an encoding banner and defaults to a Videoton-340
terminal. The operator console may itself be a remote telnet terminal.

### Line modes

A line's mode is a combination of a *character set*, a *terminal type*, and a *backspace* style,
set together, e.g. `set tty1 unicode,authbs` or `set tty1 qwerty,authbs`.

**Character set** — how bytes map to/from the machine's KOI-7 code:

| Mode | Meaning |
|------|---------|
| `unicode` | UTF-8 in and out. |
| `jcuken`  | Type Russian using the standard ЙЦУКЕН keyboard layout mapped onto Latin keys. |
| `qwerty`  | Type Russian as transliterated Latin letters: `Q`=я, `W`=в, `Y`=ы, `J`=й, `X`=ь, `C`=ц, `V`=ж, `` ` ``=ю, `~`=ч, `{`=ш, `}`=щ, `|`=э. |
| `raw`     | No conversion; bytes pass through unchanged. |

**Terminal type:**

| Mode | Meaning |
|------|---------|
| `vt`     | Videoton-340; the machine's control codes are translated to ANSI/VT100 escape sequences (cursor keys, clear screen, home). Default for new telnet connections. |
| `tt`     | MTK-2 (Baudot, 5-bit) teletype. Serial lines only. |
| `consul` | Consul-254 typewriter. Only valid on the two parallel lines (25/26). |
| `off`    | Take the line offline. |

**Backspace:**

| Mode | Meaning |
|------|---------|
| `destrbs` | Destructive (erasing) backspace. Default. |
| `authbs`  | "Authentic" backspace — moves the cursor left only, as on the real hardware. |

**Device-wide knobs:**

| Command | Effect |
|---------|--------|
| `set tty rate=N` | Serial rate in Hz (300, 600, 1200, 2400, 4800, 9600, 19200). Default 300. |
| `set tty turbo=on` / `=off` | TTY interrupt timing follows model time (on) or feels like wall-clock (off). |
| `set tty disconnect=N` | Force-drop the telnet connection on line N. |
| `set -n ttyN log=file` | Log everything typed/echoed on line N to a file. |

> Over a telnet line you can reach a small in-band CLI (its own `set`/`show`/`help`/`exit`) by
> pressing the simulator's interrupt character.

---

## Debugging and tracing

### Enabling debug output

Most devices have a single debug toggle:

```
sim> set cpu debug
sim> set mmu debug
sim> set drum debug
sim> set md0 debug
```

The disk device is the exception — it has named debug categories you can enable selectively:
`OPS` (transactions), `RRD`/`RWR` (register reads/writes), `INTERRUPT`, `TRACE`, `DATA`
(transferred data), and `STATUS`.

Route the output to a file with the console log:

```
sim> set -n console log=log.txt     ; -n truncates/creates the file
sim> set console debug=log          ; send debug output to the console log too
```

### Breakpoints and watchpoints

```
sim> br 32013           ; execution breakpoint at octal 32013
sim> go 32000           ; start at 32000, run to the next breakpoint
sim> go                 ; continue
sim> step 10000         ; execute a bounded number of instructions
```

Breakpoints come in three types — execution (`E`, the default), read (`R`), and write (`W`).
There are also hardware-register breakpoints/watchpoints via `М34`/`М35`.

### Stop codes

When the machine halts, SIMH prints a reason. The messages are in Russian; here is what they
mean:

| Message | Meaning |
|---------|---------|
| Останов | `STOP` instruction executed. |
| Точка останова | SIMH breakpoint. |
| Точка останова по считыванию / записи | Read / write watchpoint. |
| Выход за пределы памяти | Ran past the end of memory. |
| Запрещенная команда | Illegal instruction. |
| Контроль команды | A data-tagged word was fetched as an instruction. |
| Команда в чужом листе | Paging fault on an instruction fetch. |
| Число в чужом листе | Paging fault on a data access. |
| Контроль числа МОЗУ / БРЗ | RAM / write-cache parity error. |
| Переполнение АУ | Arithmetic overflow. |
| Деление на нуль | Division by zero (or a denormal). |
| Двойное внутреннее прерывание | Double internal interrupt. |
| Чтение неформатированного барабана / диска | Read from an unformatted drum / disk. |
| Останов по КРА / считыванию / записи | Hardware instruction breakpoint / load / store match. |
| Не реализовано | Unimplemented I/O or special-register feature. |

---

## Worked examples

### Booting DISPAK

`dispak.ini` boots the DISPAK batch operating system. Its stages are:

1. **Console/log setup** — open `log.txt`, route debug there.
2. **CPU setup** — `set cpu idle`, `set cpu autotime=on` (so the boot-time date/time setup
   happens automatically).
3. **Attach storage** — create the drums (`drum0`, `drum1`) and a scratch disk (`md06`) with
   `-n`; mount the existing system disks (`md07`, `md05`, `md00`, `md01`, `md02`) with `-e`.
4. **Attach the printer** — `attach -n prn0 output.txt`.
5. **Attach terminals** — telnet port 4199 and the local console on `tty1`; put all lines in
   `authbs` mode.
6. **Clear the first RAM page** — deposit a three-word zeroing loop symbolically and run it,
   exactly as an operator would key it from the switch registers after power-up:
   ```
   d -ml 1 xta, vtm 1777(1)
   d -ml 2 atx (1), utm -1(1)
   d -ml 3 v1m 2(1), stop
   run 1
   ```
7. **Boot** — `load boot_dispak.b6` then `run 2000`.

> `dispak.ini` lives in `BESM6/demo/`; run it from there. The system disk images
> (`sbor2053.bin`, `krab2063.bin`, `sbor2048.bin`, `svs2048.bin`, `alt2048.bin`) are **not**
> included in the repository — you must obtain them and place them in `BESM6/demo/` before
> `dispak.ini` will boot. The drum, scratch-disk, and printer files are created for you.

### Running a batch job from paper tape

`demo/punchtape.ini` demonstrates automated operation (run it from `BESM6/demo/`). It attaches
the job deck `input.txt` to the tape reader, boots DISPAK by `do`-ing `dispak.ini`, then drives
the machine from the front panel and by scripting the operator console:

* Deposit a request code into switch registers 5/6 and `set cpu req` to ask DISPAK to read from
  the tape reader.
* Use `expect` to watch the console output and `send` to type operator commands (poll status,
  bring the printer online, wait for printing to finish).
* Results are written to `output.txt`.

`expect` supports `-r` (regex), `-p` (persistent — stays armed after firing), and `-c` (exact
substring); `send after=N "..."` injects characters after N simulated cycles.

### Regression tests

The self-checking test suite lives in `BESM6/tests/` and is run automatically by
`make besm6` (SIMH discovers `tests/besm6_test.ini` after a successful build). Each test loads a
program, runs it, and asserts the resulting machine state with `if (...) echof "FAIL…"; exit 1`,
so a mismatch fails the build. For example `alu.ini`:

```
load alu.b6
br 32013
go -q 32000
if (PC != 032013) echof "FAIL: alu stop 1"; ex PC; exit 1
...
echof "PASS: alu"
```

`pprog05.ini` compares the accumulator against the raw octal words for 1.0, 2.0, 3.0, 4.0, and
`aout.ini` exercises the binary `a.out` loader. Note that numbers in `if` expressions are
decimal unless written with a leading `0` (octal). See
[tests/README.md](tests/README.md) for how to run and add tests.

---

## Output files

Everything is written relative to the working directory (normally `BESM6/`):

| File | Contents |
|------|----------|
| `log.txt` | Console + debug transcript. |
| `output.txt` | Line-printer output — the printed results of a job. |
| `tty1.txt` | Operator-console transcript. |
| `*.bin` | Drum/disk images, created and updated in place. |

---

## SIMH command quick reference

These are the SCP commands the BESM-6 scripts rely on. All simulators in the SIMH family share
this command language; the [SIMH PDF manual](http://simh.trailing-edge.com/pdf/simh_doc.pdf) is
the full reference.

| Command | BESM-6-relevant use |
|---------|---------------------|
| `attach` / `at` | Connect a device unit to a host file or TCP port. Switches `-n` (new), `-e` (must exist), `-t` (text), `-r` (read-only). |
| `set` | Configure the CPU/MMU/devices and terminal modes. |
| `show` | Display device settings and status. |
| `d` / `deposit` | Write a word (or symbolic instruction) to memory or a register. |
| `e` / `examine` | Read memory/registers; supports `-m`/`-ml`/`-i`/`-f`/`-b`/`-x` formats and `addr/count` ranges. |
| `load` | Load a `.b6` text memory image. |
| `dump` | Write memory out in `.b6` text format. |
| `br` / `break`, `nobreak` | Set / clear breakpoints. |
| `go [addr]` | Resume (or start at `addr`) and run to the next breakpoint. |
| `run [addr]` | Reset, then start at `addr`. |
| `step [n]` | Execute one or `n` instructions. |
| `expect`, `send` | React to and inject simulated console text (script automation). |
| `do file` | Execute a nested command/`.ini` file. |
| `echo` | Print a message. |
| `!` | Run a host-OS shell command. |
| `assert`, `on` | Test a condition / install an error handler (used by the tests). |
| `quit` / `q` | Exit the simulator. |
