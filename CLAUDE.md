# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Scope: BESM-6 only

**Work in this project is focused exclusively on the BESM-6 simulator in `BESM6/`.** Ignore every other machine directory. This repo is a fork of the [Open SIMH](https://github.com/open-simh/simh) suite, so it also carries dozens of unrelated upstream simulators (VAX, PDP11, PDP10, HP2100, `SVS/`, …) plus a huge shared `makefile`/`descrip.mms` and the SIMH framework at the repo root (`scp.c`, `sim_*.c/.h`, `sim_defs.h`). Treat all of that as read-only infrastructure — don't touch it unless a change genuinely belongs there. In particular, do not spend effort on `SVS/` or any other architecture.

The **BESM-6** is a Soviet mainframe: 48-bit word, octal, single CPU.

## Build

Binaries are produced into `BIN/`; object files go under `BIN/<platform>-build/`.

```sh
make besm6      # builds BIN/besm6 (needs SDL2 video + TTF font support; pulled in automatically)
```

Build options come from the makefile: `BESM6_OPT = -I BESM6 -DUSE_INT64 …` (the 48-bit word requires `-DUSE_INT64`). The link step runs an internal register sanity check on a healthy build.

## Run and test

Simulator control scripts are `.ini` files, run from inside `BESM6/` so relative `attach` paths resolve:

```sh
cd BESM6 && ../BIN/besm6 dispak.ini        # boot the DISPAK OS from disk images
cd BESM6 && ../BIN/besm6 test_alu.ini      # ALU regression (uses SIMH breakpoints)
```

Tests rely on hitting expected breakpoint addresses (`br <addr>`, then `go`). Tracing: `set cpu debug` / `set mmu debug` / `set drum debug`, with console output redirected via `set console log=<file>`. Disk/drum images (`*.bin`, `boot_dispak.b6`) are attached by the `.ini` scripts; `.b6` files are loadable BESM-6 memory images.

## Architecture notes (`BESM6/`)

- **Word model.** 48-bit words held in `t_value`. Everything is octal. Floating point is sign-magnitude with a base-2 exponent. Bit macros in `besm6_defs.h` number bits **right-to-left starting at 1** (`BBIT(n)`, `BIT40`, `BIT41`, `BIT48`, …) — the opposite of most SIMH machines, so read the header before touching arithmetic.
- **Registers.** `M[]` holds the index/modifier registers (М1–М17) plus special registers (PSW, SPSW, PC, …), with indices defined in `besm6_defs.h`. Cyrillic register names (М1–М17, СМ/ACC) show up in traces.
- **Memory.** `512*1024` words. Drums/disks are organized in zones of `ZONE_SIZE = 8 + 1024` words (8 system words + 1 Kword of user data), each word stored as an 8-byte little-endian record.
- **File layout.** `besm6_cpu.c` = fetch/execute + `DEVICE` tables + `sim_devices[]`; `besm6_arith.c` = ALU/FPU; `besm6_mmu.c` = memory mapping & protection; `besm6_sys.c` = load/dump, symbolic assembler/disassembler, examine/deposit; `besm6_disk.c`/`besm6_drum.c` = mass storage. Peripherals: `besm6_tty.c`, `besm6_printer.c`, `besm6_punch.c`, `besm6_punchcard.c`, `besm6_pl.c` (plotter), `besm6_mg.c` (mag tape), `besm6_vu.c`, `besm6_panel.c` (front panel with Cyrillic font rendering).
- **PC name clash.** `besm6_defs.h` does `#define PC PC_Global` to dodge a namespace conflict — keep that in mind when grepping.

## Conventions

- Comments and identifiers in `BESM6/` are frequently in Russian (Cyrillic). Match the surrounding language when editing a file rather than converting it.
- `.editorconfig` is authoritative for whitespace.
