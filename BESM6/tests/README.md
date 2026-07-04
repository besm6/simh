# BESM-6 regression tests

Self-checking tests for the BESM-6 simulator. Each test loads a small program,
runs it, and verifies the resulting machine state; on any mismatch it prints a
`FAIL:` line and exits with a non-zero status.

## Running

The suite is wired into the build. After a successful compile, `make besm6`
automatically runs it — SIMH looks for `<simdir>/tests/<simname>_test.ini`, i.e.
`BESM6/tests/besm6_test.ini`, and executes it (right after the internal
`RegisterSanityCheck`). A failing test therefore fails the build.

Run the whole suite by hand:

```sh
cd BESM6/tests
../../BIN/besm6 besm6_test.ini
```

Run a single test by hand:

```sh
cd BESM6/tests
../../BIN/besm6 alu.ini        # or pprog05.ini, aout.ini
```

`besm6_test.ini` is only the runner: it `cd`s into this directory, sets a
`runlimit` so a runaway test can't hang the build, and `do`s each sub-test in
turn. A sub-test's `exit 1` terminates the simulator immediately, so the run
stops at the first failure.

## The tests

| Test          | Fixture             | Checks |
|---------------|---------------------|--------|
| `alu.ini`     | `alu.b6`            | Arithmetic-unit program halts three times at address 032013. |
| `pprog05.ini` | `pprog05.b6`        | Accumulator holds 1.0, 2.0, 3.0, 4.0 across four passes. |
| `aout.ini`    | `aout/hello`        | The binary a.out loader loads a linked executable that prints `Hello!` to the operator console and starts it at its `a_entry` (010). |

## Fixture formats

- **`.b6`** — a textual BESM-6 memory image (address lines `в`, octal words `с`,
  instructions `к`, start address `п`). Loaded by `load`. See the main
  [BESM6/README.md](../README.md) for the format and `dump` command.
- **`aout/hello`** — a binary `a.out` executable produced by the BESM-6 Unix
  cross toolchain (`b6as`/`b6ld`); `load` auto-detects the `BESM` magic. The
  `aout/` directory keeps the source (`hello.s`), a disassembly (`hello.dis`)
  and a `Makefile` that rebuilds it (`make -C aout`) when the toolchain is
  installed.

## Writing a new test

1. Add the fixture (a `.b6` image, or a binary a.out image under `aout/`).
2. Write `mytest.ini`: `load` the fixture, drive it with `go`/`br`, then assert
   with `if`:

   ```
   load mytest.b6
   go -q
   if (PC != 012345) echof "FAIL: mytest, expected PC 12345, got:"; ex PC; exit 1
   echof "PASS: mytest"
   ```

3. Add `do mytest.ini` to `besm6_test.ini`.

### `if` gotchas (learned the hard way)

- **Numbers are decimal by default.** Write octal with a leading zero:
  `032013`, `010`, `04050000000000000`. A bare `32013` is decimal; a `0o`
  prefix is *not* accepted and raises `Invalid Expression Element`.
- **`if (cond) A; B; C`** runs the whole rest of the line conditionally, so the
  usual pattern is `if (bad) echof "FAIL: ..."; ex REG; exit 1`.
- **No `printf`-style args in `echof`.** `echof "x=%s" PC` does not work; print
  the value with a following `ex REG` instead.
- **Register names.** Latin `PC`, `ACC`, `M1`… work (Cyrillic `СчАС`, `СМ`… too).
  `ex -f ACC` prints the accumulator as floating point; compare the raw 48-bit
  octal word in `if`.
