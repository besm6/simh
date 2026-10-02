#!/usr/bin/env python3
"""
Собрать .b6 из зон образа диска.

Распакованная страница зоны — это слова образа buf[8..1031] в исходном
порядке (проверено: циклическая сумма этих 1024 слов совпадает с СС[3]
у зон 0511/0555/0460/0462/0463). Поэтому зона переводится в 1024 слова
памяти один к одному, без свёртки.

  python3 tools/zones2b6.py образ 30000 747 750 751 752 > adap747.b6
"""
import struct
import sys

WORD = 8
ZONE_WORDS = 8 + 1024
M48 = (1 << 48) - 1


def insn_text(half):
    reg = (half >> 20) & 0o17
    if half & (1 << 19):
        return "%02o %02o %05o" % (reg, ((half >> 12) & 0o370) >> 3, half & 0o77777)
    return "%02o %03o %04o" % (reg, (half >> 12) & 0o177, half & 0o7777)


def main():
    if len(sys.argv) < 4:
        sys.exit(__doc__)
    img, base = sys.argv[1], int(sys.argv[2], 8)
    zones = [int(z, 8) for z in sys.argv[3:]]
    data = open(img, "rb").read()

    out = sys.stdout
    out.write(";\n; Собрано tools/zones2b6.py из %s, зоны %s.\n;\n"
              % (img, " ".join("%o" % z for z in zones)))
    out.write("в %o\n" % base)
    addr = base
    for z in zones:
        off = z * ZONE_WORDS * WORD
        for i in range(8, ZONE_WORDS):
            w = struct.unpack("<Q", data[off + i*WORD: off + i*WORD + WORD])[0] & M48
            left, right = (w >> 24) & 0o77777777, w & 0o77777777
            out.write("к %s, %s ; с %016o ; %05o\n"
                      % (insn_text(left), insn_text(right), w, addr))
            addr += 1


if __name__ == "__main__":
    main()
