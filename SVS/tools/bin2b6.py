#!/usr/bin/env python3
"""
Перевод сырого образа памяти БЭСМ-6/СВС в текстовый формат .b6,
который понимает `load` симулятора (см. svs_read_line в svs_sys.c).

Образ — подряд идущие 48-разрядные слова по 6 байт, старший байт первым
(big-endian). Так устроен, в частности, vyzpvv.bin: 1024 слова по 6 байт,
грузится с адреса 02000.

Все слова выводятся строками `к` (команда), то есть с тегом TAG_INSN48.
Это не произвол: на настоящей машине такая страница приезжает чтением с
диска, а там у всех слов данных тег 1 (DISK_TAG_INSN), и disk_word_to_mem()
ставит TAG_INSN48 всем словам подряд. Пометь слово числом — и переход на
код даст контроль команды.

  python3 tools/bin2b6.py vyzpvv.bin --base 2000 --start 2000 > vyzpvv.b6
"""
import argparse
import sys

WORD_BYTES = 6


def insn_text(half):
    """24-разрядную половину слова — в текст, как её разбирает
    parse_instruction(): val = reg<<20 | opcode<<12 | addr."""
    reg = (half >> 20) & 0o17
    if half & (1 << 19):
        # длинная команда: код 020..037 (печатается двумя цифрами), адрес 15 разр.
        return "%02o %02o %05o" % (reg, ((half >> 12) & 0o370) >> 3, half & 0o77777)
    # короткая команда: код до 0177 (три цифры), адрес 12 разр.
    return "%02o %03o %04o" % (reg, (half >> 12) & 0o177, half & 0o7777)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("image", help="сырой образ (6 байт на слово, big-endian)")
    ap.add_argument("--base", default="2000", help="адрес загрузки, восьмеричный")
    ap.add_argument("--start", default=None, help="стартовый адрес, восьмеричный")
    ap.add_argument("--comment", default=None, help="строка-заголовок")
    args = ap.parse_args()

    base = int(args.base, 8)
    data = open(args.image, "rb").read()
    if len(data) % WORD_BYTES:
        sys.exit("длина %d не кратна %d байтам на слово" % (len(data), WORD_BYTES))
    words = [int.from_bytes(data[i:i + WORD_BYTES], "big")
             for i in range(0, len(data), WORD_BYTES)]

    out = sys.stdout
    out.write(";\n")
    out.write("; %s\n" % (args.comment or
                          "Сгенерировано tools/bin2b6.py из %s" % args.image))
    out.write("; %d слов с адреса %o.\n" % (len(words), base))
    out.write(";\n")
    out.write("в %o\n" % base)
    for i, w in enumerate(words):
        left, right = (w >> 24) & 0o77777777, w & 0o77777777
        out.write("к %s, %s ; с %016o ; %05o\n"
                  % (insn_text(left), insn_text(right), w, base + i))
    if args.start is not None:
        out.write("п %o\n" % int(args.start, 8))


if __name__ == "__main__":
    main()
