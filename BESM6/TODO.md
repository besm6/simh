# TODO

## Restore the КРАБ path in ГЕНС2 (needed for ОСА)

With `СОЮЗ ДА` (ТРАКТЫ bit 31), the system disk does not boot. ГЕНС2 checks the bit at `ВОЗВ1`
(`re-dispak/gens2.be:572-574`, «ЕСТЬ СИСТЕМА СВЯЗИ») and jumps to `КРАБ` (`gens2.be:685-696`). In
this build `КРАБ` is `сч 0; стоп КРАБ; пб КРАБ` at 060520–060521, an endless stop.

The original code is still in the source as comments:
`И Е16 / ПО КРАБА / СЧ КАТР / … / СЧ 1 / И Е16 / ПО КРАБА / УИА 3 / УИА (М5)`.
Starting from `сч 0`, `ПО КРАБА` was always taken. `КРАБА` itself (`gens2.be:336`, the check of the
job catalogue `КАТР`) is still assembled.

ОСА cannot run without `СОЮЗ ДА`:
- ДИСП70 sends task 24's alarm to `есацпу` only when bit 31 is set;
- ГЕНС1 reads ОСА's zones 0706–0707 only when bit 31 is set (`gens1.be:931-939`).

Status: done in `tools/makeBESM2053.py`, as part of `СОЮЗ ДА`. Word 060520 of ГЕНС2 (image zone
0475) becomes `пб ВОЗВ2(М6) / мода`, and the zone checksum is updated. Restoring the literal
`ПО КРАБА` path loops forever: `КРАБА` is the ordinary job-catalogue pass, which ends back at
`ВОЗВ1` → `КРАБ`. The original apparently waited for the partner machine and could only be left
through switch register 1, bit 16. With the patch, a `СОЮЗ ДА` image boots to the operator prompt.

## Do not assume КОНФУС is resident (needed to switch off СВЯЗЬ7 / ДКС)

In this К-71 build, КОНФУС (КАДОПАМ/ДКС, DW-21; `konfus.be`, zone 0676) is loaded into page 034
(070000–071777) only when `СЭВМ ДА` is set:
- ГЕНС1 sets `КУСЛСВ := КЛСВ` (`gens1.be:103-110`; КЛСВ = zone 0576 + НОММЛ1, page 034);
- a start-up block (033540–033552 at run time) does `сч КУСЛСВ; по …; слц НОММЛ1; пв ФИЗОБМ`, and
  then clears `КУСЛСВ`.

With `СЭВМ НЕТ`, page 034 still holds ГЕНС. НОМБОБ's К-71 block `диасм7` (`nombob.be:316-345`)
does `сч рсвсм1; по нстдкс` into КОНФУС without a check. That lands on ГЕНС's `встав1`, which
restarts generation at `СТАТИ`, and the boot then traps on `АЦП`, which ГЕНС2 has already used as
a scratch cell (`gens2.be:224`).

НОМБОБ is not the only module that assumes КОНФУС is present. Modules that import from it:

| Module | Imports | Under `∧К71`? |
|---|---|---|
| ПЕЧАТЬ | `начdw` (called on every printer start, `pechat.be:47`), `пркод7`, DW-21 cells | no |
| ДИСП70 | `Э52777` (the Э52 handler) | yes |
| КЗ | `кзк71` (ДКС cold start) | yes |
| НОМБОБ | `нстдкс`, `рсвсм1`, `рсвясм` | yes |
| ПРИК4 | `вых1к7` | yes |
| МОТТ | `тпрер`, `тпроц`, `тобщ`, `тав` (КОНВОЙ entries), `базсв7` | yes |
| ЭК2 | `маспр7` | no |
| ЭК7, ЭКО, ХЛАМ, КЗ1, ПРИК7, ПРИСКВ, ПРИКАЗ | ДКС state cells and entries | partly |
| СВЯЗЬ7 | its whole working state | — |

To do:
- Decide how a call site should detect that КОНФУС is present. `ПРЕДЕЛ` bit 28 (СЭВМ) is one
  option; a check of the КОНФУС page is another.
- Patch the call sites, starting with НОМБОБ `диасм7` and ПЕЧАТЬ `НАЧАЛО`, so that СВЯЗЬ7/ДКС can be
  switched off. Slot 24 is then free for ОСА.

## ОСА: recover the АС-6 protocol and emulate the ES side

Status (details in `doc/ОСА.md` §5):
- `033 0200–0237` / `04200–04237` are decoded to the `OSA` device (`besm6_osa.c`). It is a
  register-level trace model for now.
- With `СОЮЗ ДА` (including the ГЕНС2 patch above), `ТМГУ НЕТ`, `СЭВМ ДА` and ДКС off, ОСА is
  resident at 040000 and runs.
- It polls `04230` on task 24's alarm and writes nothing else while the link is quiet.

Next steps:
1. Trace what ОСА does on request bit 7 (`202`, work Е48 → `G40057`): which block it expects,
   where, and what it checks against `D43154`/«ПАУК»/«ЕСТ0». Raise bit 7 by hand through the
   `REQ` register and follow the code.
2. Trace the output path from an `ЕСАЦПУ 0` job to `201`: who queues work for ОСА, the descriptor
   `D43074` (address, length in `221`, flags Е19–Е22), and completion on bit 4.
3. Write the protocol up in `doc/ОСА.md` §5.
4. Emulate the ES side in `besm6_osa.c`:
   - `sim_activate` events;
   - DMA through the descriptor;
   - the ES printer (file, ДКОИ → UTF-8), ПИ/ПЛ, and ЕСТЕРМ terminals over TMXR.
5. Add a register-level test, `tests/osa.ini`, and an end-to-end check of printing through the ES
   printer.
