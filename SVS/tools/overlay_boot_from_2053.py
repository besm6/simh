#!/usr/bin/env python3
"""
Гибридный образ: база ~/.besm6/2253, недостающие зоны загрузки — из svs2053.bin.

На 2253 среди зон, которые читает boot.sh, шахматки 05252/02525 нет
(она одна на всём пакете — зона 0427, к загрузке не относится). Зато 14
зон загрузки полностью нулевые, а в svs2053.bin там лежит нужное
содержимое. Скрипт:

  1. копирует базу (по умолчанию ~/.besm6/2253);
  2. находит среди BOOT_ZONES зоны-«пустышки» — шахматную заливку
     и/или целиком нулевые (см. --only-checkerboard);
  3. подставляет эти зоны из svs2053.bin;
  4. прогоняет tools/makeSVS2053.py (номер пакета 4005, правка
     удвоенных номеров зон в СС).

    python3 tools/overlay_boot_from_2053.py [опции]

Список BOOT_ZONES — уникальные зоны МД с РАЗМ=784 (и метка 0100),
прочитанные DISK1 в прогоне svs.ini / boot.sh.
"""
from __future__ import annotations

import argparse
import os
import shutil
import struct
import subprocess
import sys
import tempfile

WORD = 8
ZONE_WORDS = 8 + 1024
ZONE_BYTES = ZONE_WORDS * WORD
M48 = (1 << 48) - 1

# Чередование слов данных после росписи пакета (диски / формат).
CB_EVEN = 0o5252525252525252 & M48
CB_ODD = 0o2525252525252525 & M48

# Зоны, которые DISK1 читает при загрузке (octal). Метка 0100 — РАЗМ=8.
BOOT_ZONES = (
    0o511, 0o754, 0o555,
    *range(0o565, 0o634),          # 565…633
    0o556, 0o100,
    0o755, 0o471, 0o472, 0o473,
    0o551, 0o552, 0o553,
    0o544, 0o545, 0o546,
    0o501, 0o502, 0o503,
    0o574, 0o462, 0o464, 0o443, 0o475, 0o554,
    0o34,
)

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_BASE = os.path.expanduser("~/.besm6/2253")
DEFAULT_FILL = "/home/leob/git/besm6.github.io/download/disks/svs2053.bin"
DEFAULT_OUT = os.path.join(os.path.dirname(HERE), "svs2253-bootfix.bin")
MAKE_FIX = os.path.join(HERE, "makeSVS2053.py")


def n_zones(path: str) -> int:
    sz = os.path.getsize(path)
    if sz % ZONE_BYTES:
        sys.exit("%s: размер %d не кратен зоне (%d байт)" % (path, sz, ZONE_BYTES))
    return sz // ZONE_BYTES


def read_zone(data: bytes | bytearray, z: int) -> bytes:
    off = z * ZONE_BYTES
    return bytes(data[off:off + ZONE_BYTES])


def is_all_zero(zone: bytes) -> bool:
    return all(b == 0 for b in zone)


def is_checkerboard(zone: bytes) -> bool:
    """Все 1024 слова данных — чередование 05252… / 02525… (в любом порядке пар)."""
    if len(zone) != ZONE_BYTES:
        return False
    words = [struct.unpack_from("<Q", zone, (8 + i) * WORD)[0] & M48
             for i in range(1024)]
    if not words:
        return False
    # Строгая заливка: каждое слово — один из двух шахматных констант,
    # и оба встречаются (иначе это «все 52» и т.п.).
    s = set(words)
    if s != {CB_EVEN, CB_ODD}:
        return False
    # Типичная роспись — строгое чередование; принимаем и сдвиг на 1.
    alt_even = all(words[i] == (CB_EVEN if i % 2 == 0 else CB_ODD)
                   for i in range(1024))
    alt_odd = all(words[i] == (CB_ODD if i % 2 == 0 else CB_EVEN)
                  for i in range(1024))
    return alt_even or alt_odd


def main() -> None:
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--base", default=DEFAULT_BASE,
                    help="исходный пакет (по умолчанию ~/.besm6/2253)")
    ap.add_argument("--fill", default=DEFAULT_FILL,
                    help="откуда брать зоны (svs2053.bin)")
    ap.add_argument("--out", default=DEFAULT_OUT,
                    help="куда положить исправленный образ")
    ap.add_argument("--only-checkerboard", action="store_true",
                    help="подставлять только шахматные зоны, не трогая нулевые")
    ap.add_argument("--pack", default="4005",
                    help="номер пакета для makeSVS2053.py (восьм., по умолч. 4005)")
    ap.add_argument("--dry-run", action="store_true",
                    help="только перечислить зоны, ничего не писать")
    args = ap.parse_args()

    for p in (args.base, args.fill, MAKE_FIX):
        if not os.path.isfile(p):
            sys.exit("нет файла: %s" % p)

    n_base = n_zones(args.base)
    n_fill = n_zones(args.fill)
    base = open(args.base, "rb").read()
    fill = open(args.fill, "rb").read()

    need: list[int] = []
    skipped_oor: list[int] = []
    for z in BOOT_ZONES:
        if z >= n_base:
            skipped_oor.append(z)
            continue
        if z >= n_fill:
            sys.exit("зона %o есть в базе, но нет в %s (%d зон)"
                     % (z, args.fill, n_fill))
        zone = read_zone(base, z)
        cb = is_checkerboard(zone)
        empty = is_all_zero(zone)
        if cb or (empty and not args.only_checkerboard):
            need.append(z)

    def log(msg: str) -> None:
        print(msg, flush=True)

    log("overlay_boot_from_2053: база %s (%d зон), заливка %s (%d зон)"
        % (args.base, n_base, args.fill, n_fill))
    log("  зон загрузки в списке: %d" % len(BOOT_ZONES))
    if skipped_oor:
        log("  вне образа базы: %s" % " ".join("%o" % z for z in skipped_oor))
    kind = "шахматка" if args.only_checkerboard else "шахматка или нули"
    log("  к подстановке (%s): %d — %s"
        % (kind, len(need),
           " ".join("%o" % z for z in need) if need else "(нет)"))

    if args.dry_run:
        return
    if not need and args.only_checkerboard:
        log("  нечего копировать; --only-checkerboard и шахматных зон загрузки нет")
        need = []

    with tempfile.TemporaryDirectory() as tmp:
        staged = os.path.join(tmp, "staged.bin")
        shutil.copyfile(args.base, staged)
        if need:
            with open(staged, "r+b") as out, open(args.fill, "rb") as src:
                for z in need:
                    src.seek(z * ZONE_BYTES)
                    blob = src.read(ZONE_BYTES)
                    if len(blob) != ZONE_BYTES:
                        sys.exit("короткое чтение зоны %o из %s" % (z, args.fill))
                    out.seek(z * ZONE_BYTES)
                    out.write(blob)
            log("  подставлено зон: %d" % len(need))

        cmd = [sys.executable, MAKE_FIX, staged, args.out, "--pack", args.pack]
        log("  %s" % " ".join(cmd))
        subprocess.check_call(cmd)

    log("overlay_boot_from_2053: готово -> %s" % args.out)


if __name__ == "__main__":
    main()
