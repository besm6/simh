#!/usr/bin/env python3
"""
Перебивка номера тома (пакета) в служебных словах образа диска СВС.

Задача архива требует том «СРЧ.К40Б2052» (устройство 0). Настоящего тома 2052
нет, но есть другие тома в /usr/local/share/besm6 (2248, 2148, …) с реальной
разметкой зон. Скрипт делает КОПИЮ такого тома и меняет в ней только НОМЕР
ПАКЕТА в служебных словах СС[1]/СС[5] (разр.24-13, ключ 070707 в разр.39-25) —
то же поле и тем же способом, что и makeSVS2053.py (п.1 его шапки).

Кроме номера пакета правится НОМЕР ЗОНЫ (СС[0], СС[4], разр.48-37). Тома в
/usr/local/share/besm6 хранят его УДВОЕННЫМ (СС[0]=2z, СС[4]=2z+1) — так же,
как дистрибутивный svs2053.bin. СВС-загрузчик и ДИСКИ ждут неудвоенный номер
(СС[0]=СС[4]=z, ПРЗОНЫ, физ.073503), иначе задача архива читает зону и получает
«ДРУГ.ЗОНА» -> ОШЧТ -> «СБОЙ АРХИВА». Скрипт кладёт в разр.48-37 сам z (как
makeSVS2053.py, п.2), сохраняя младшие разряды слова. Данные зон и контрольные
суммы СС[3]/СС[7] (считаются по словам данных) не трогаются.

Номер пакета хранится как ДЕСЯТИЧНОЕ значение имени файла, сдвинутое на 12
(disk_format в svs_disk.c: volume<<12; disk_volume_from_name читает имя по
основанию 10). Поэтому --volume задаётся десятичным числом (по умолчанию 2052).

    python3 tools/mkvol.py <исходный-том> <копия> [--volume N] [--keep-zones]

--keep-zones оставляет номер зоны как в исходнике (удвоенным) — для отладки.

Дистрибутив/исходник открывается только на чтение.
"""
import argparse
import shutil
import struct
import sys

WORD = 8
ZONE_WORDS = 8 + 1024
M48 = (1 << 48) - 1

KEY_MASK  = 0o77777 << 24           # разр.39-25: ключ СС
KEY       = 0o70707 << 24           # его значение в исправных словах
PACK_MASK = 0o7777 << 12            # разр.24-13: номер пакета (тома)
ZONE_MASK = 0o7777 << 36            # разр.48-37: номер зоны


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("src", help="исходный том (только чтение)")
    ap.add_argument("dst", help="куда положить копию с перебитым номером тома")
    ap.add_argument("--volume", type=int, default=2052,
                    help="новый номер тома, десятичный (по умолчанию 2052)")
    ap.add_argument("--keep-zones", action="store_true",
                    help="не трогать номер зоны (оставить удвоенным, для отладки)")
    ap.add_argument("--zero-data", action="store_true",
                    help="обнулить данные всех зон (СС/номер тома оставить, КС в 0) — "
                         "чистый пустой том без файлов исходника")
    args = ap.parse_args()

    if not 2048 <= args.volume <= 4095:
        sys.exit("mkvol: --volume %d вне диапазона 2048..4095" % args.volume)

    shutil.copyfile(args.src, args.dst)

    with open(args.dst, "r+b") as f:
        data = bytearray(f.read())
        if len(data) % (ZONE_WORDS * WORD):
            sys.exit("mkvol: %s не кратен размеру зоны (%d слов)"
                     % (args.src, ZONE_WORDS))
        nzones = len(data) // (ZONE_WORDS * WORD)
        npack = nzone = nzero = 0
        old = oldzone = None

        for z in range(nzones):
            base = z * ZONE_WORDS * WORD

            if args.zero_data:                  # чистим данные, СС оставляем
                for i in range(8, ZONE_WORDS):  # 1024 слова данных -> 0 (тег сохраняем)
                    off = base + i * WORD
                    raw = struct.unpack("<Q", data[off:off + WORD])[0]
                    data[off:off + WORD] = struct.pack("<Q", raw & ~M48)
                for i in (3, 7):                # КС по обнулённым данным == 0
                    off = base + i * WORD
                    raw = struct.unpack("<Q", data[off:off + WORD])[0]
                    data[off:off + WORD] = struct.pack("<Q", raw & ~M48)
                nzero += 1

            for i in (1, 5):                    # СС[1], СС[5]: номер пакета
                off = base + i * WORD
                raw = struct.unpack("<Q", data[off:off + WORD])[0]
                val = raw & M48
                if (val & KEY_MASK) != KEY:     # правим только ключевые слова
                    continue
                if old is None:
                    old = (val & PACK_MASK) >> 12
                new = (val & ~PACK_MASK & M48) | (args.volume << 12)
                if new != val:
                    data[off:off + WORD] = struct.pack("<Q", (raw & ~M48) | new)
                    npack += 1

            if args.keep_zones:                 # СС[0], СС[4]: неудвоенный номер зоны
                continue
            for i in (0, 4):
                off = base + i * WORD
                raw = struct.unpack("<Q", data[off:off + WORD])[0]
                val = raw & M48
                if val == 0:                    # зона не размечена
                    continue
                if oldzone is None and i == 0:
                    oldzone = (val & ZONE_MASK) >> 36
                new = (val & ~ZONE_MASK & M48) | ((z & 0o7777) << 36)
                if new != val:
                    data[off:off + WORD] = struct.pack("<Q", (raw & ~M48) | new)
                    nzone += 1

        f.seek(0)
        f.write(data)

    print("mkvol: %s -> %s" % (args.src, args.dst))
    print("          зон %d; номер тома %s -> %d в %d служебных словах"
          % (nzones, old, args.volume, npack))
    if args.keep_zones:
        print("          номер зоны оставлен как в исходнике (--keep-zones)")
    else:
        print("          номер зоны %s.. -> неудвоенный z, исправлен в %d словах"
              % (oldzone, nzone))
    if args.zero_data:
        print("          данные обнулены во всех %d зонах (СС/номер тома сохранены)"
              % nzero)


if __name__ == "__main__":
    main()
