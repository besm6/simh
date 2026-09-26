# Known bugs

## A3 virtual cards from paper tape are corrupted when the passport uses a standard passport (ЗС)

**Status:** open. Not yet known whether this is Dispak's own behaviour or an
emulation difference.

### Symptom

A job read from paper tape (`ДАЙ ФС0`, `ФС8`, `attach -t FS0 …`) whose passport
requests a standard passport (e.g. `ЗСХ^`) and then has an A3 section (virtual
punch cards, `ctrl-]` … `ctrl-]`):

```
ШИФР 419900 ЗСХ^ВРЕ 100^АЦП 1^ЕЕВ1А3
^]*NAME TEST
^]ЕКОНЕЦ
```

The job is formed (`Л001-7 419900000000`), but Monitor-80 rejects its input:
it reports garbage cards (0002, 0008, 0010, 0011) and the job ends with
`ОЗ(001) … M=017 MACCИB HE CУЩ-T.`. The result does not depend on the contents
of the A3 card (a blank card gives the same errors). Shifting the A3 section by
padding the passport changes the outcome (tape jam `ЗMЛ0`, or the ЕНДА3 card is
found but the card contents are lost: `ОТСУТСТВУЕТ П/К *NАМЕ`).

### Mechanism

В1К cuts each 960-frame tape block into 120-character pseudo-cards. It
extracts characters from each 6-byte word (`ВЫБ2`/`ВЫБ3`) and counts them in
`СЧСИМ` (physical 047370 in the traced run). When the count reaches 120 the
card is complete: `СЧСИМ` is cleared and the next word is fetched (code at
046364–046377), **discarding the rest of the current word**. This is only
harmless if every pseudo-card starts on a word boundary.

- **Input pass.** The count starts at the tape's first (dummy) frame, so
  pseudo-cards are frames 0–119, 120–239, … and always word-aligned.
- **Formation pass.** When `ЗСХ` is reached, В3В4 saves the count before
  splicing in the standard passport (`СЧ СЧСИМ / ЗП СЧСИМК`, В3В4.bemsh:388–389),
  and В1К restores it afterwards (`СЧ СЧСИМК / ЗП СЧСИМ`, В1К.bemsh:499–500).
  In this pass the count at that moment is 0, because `ШИФР 419900 ЗС` is
  consumed without passing through the counting code. After the splice the
  count is therefore 1 at `Х` (frame 15) instead of 16.

With the example tape, the formation pass considers a pseudo-card complete at
frame 134, in the middle of buffer word 047156 (frames 132–137). Frames 135–137
are discarded; they are zeros from the A3 card's padding. В6В7 then receives
107 padding zeros instead of 110.

В6В7 recognizes the end of an A3 section (`КОНА3`) as 24 consecutive 5-character
groups equal to `0200 0 0 0 0` (`СЧ МЛРВБЩ / НТЖ Е40`, groups counted from the
start of A3 by `ЦИКЛ (М11)` in `РЕДА3`). With 3 characters missing, every
ЕНДА3 group arrives as `0 0 0200 0 0`, the end is never seen, and whatever
follows is passed to Monitor-80 as data cards.

### What is ruled out

- The tape reader: `svs_cards.c` (`fs_next_frame`) reproduces BESM-6's
  `fs_event` exactly, including the dummy start frame, `ctrl-]` switching,
  zero fill and the ЕНДА3 card. A job without an A3 section (`koan.txt`
  without `ctrl-]`) is read, run and printed correctly.
- Zero-frame dropping: inside A3 `ЯЧВСТ` has the `КОБХОД` bit (A3), so В1К
  delivers every frame, zeros included.

### Open question

Whether `ШИФР … ЗС` really bypasses the character counter on SVS hardware
(making A3 after a standard passport misaligned there too), or whether the
emulation makes the formation pass read the passport differently from the
input pass.

### How it was found

A breakpoint at 051141 (`РЕДА3`, after `ЗП МЛРВБЩ`) printing `ACC,M11` shows
the A3 characters and group phase seen by В6В7. A windowed trace
(`set cpu0 window=46000:47777`, `debug=insn;regs`) from `ФС8` to `001-7`
shows В1К extracting only 3 characters from word 047156, and the value of
`СЧСИМ` for every character.
