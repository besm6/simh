#!/usr/bin/env python3
"""
Колода проверки печати: все печатные символы ГОСТ 10859 (коды 000-0137)
выдаются на АЦПУ экстракодом 064 (э64).

    python3 tools/mkgostdeck.py [колода.b6] [ожидаемое.txt]

По умолчанию пишет gostprint.b6 и gostprint.expected.txt (ожидаемый текст
шести строк в UTF-8 по таблице ГОСТ -> Unicode из dispak/encoding.c).
Сценарий ввода и печати - gostprint.ini.

Программа (лист 0, вход 01000):

    01000  э64 01100, э74          печать; э64 возвращает на СЛЕДУЮЩЕЕ слово,
    01001  э74, э74                поэтому конец задачи - отдельным словом
    01100  разр.39-25 - начало текста (01102), разр.15-1 - конец
    01101  формат: разр.48-45 = 0 (текст ГОСТ), разр.24-21 = 010 (последнее
           слово формата) - как разбирает э64 dispak (extra.c, print())
    01102  текст ГОСТ, 6 байт в слове, старший байт первый: 6 строк
           «ННН c c ... c»: ННН - восьмеричный код первого символа строки
           (020k), затем 16 символов 020k..020k+017 через пробел;
           каждая кончается 0214 (новая строка); затем строка наложения
           «WS=1» 0173 0 «VS:1» (не из таблицы ПЕЧСВС), в конце 0172
           (конец информации).
"""
import sys

GOST_SPACE = 0o17
GOST_NEWLINE = 0o214
GOST_END = 0o172
GOST_SET_POS = 0o173

# ГОСТ 10859 -> Unicode, коды 000-0137 (dispak/encoding.c, gost_to_unicode_cyr)
GOST_UNICODE = [
    0x30,   0x31,   0x32,   0x33,   0x34,   0x35,   0x36,   0x37,
    0x38,   0x39,   0x2b,   0x2d,   0x2f,   0x2c,   0x2e,   0x20,
    0x23e8, 0x2191, 0x28,   0x29,   0xd7,   0x3d,   0x3b,   0x5b,
    0x5d,   0x2a,   0x2018, 0x2019, 0x2260, 0x3c,   0x3e,   0x3a,
    0x0410, 0x0411, 0x0412, 0x0413, 0x0414, 0x0415, 0x0416, 0x0417,
    0x0418, 0x0419, 0x041a, 0x041b, 0x041c, 0x041d, 0x041e, 0x041f,
    0x0420, 0x0421, 0x0422, 0x0423, 0x0424, 0x0425, 0x0426, 0x0427,
    0x0428, 0x0429, 0x042b, 0x042c, 0x042d, 0x042e, 0x042f, 0x44,
    0x46,   0x47,   0x49,   0x4a,   0x4c,   0x4e,   0x51,   0x52,
    0x53,   0x55,   0x56,   0x57,   0x5a,   0x203e, 0x2a7d, 0x2a7e,
    0x2228, 0x2227, 0x2283, 0xac,   0xf7,   0x2261, 0x25,   0x25c7,
    0x7c,   0x2015, 0x5f,   0x21,   0x22,   0x042a, 0xb0,   0x2032,
]

CODE_ADDR = 0o1000
INFO_ADDR = 0o1100
TEXT_ADDR = INFO_ADDR + 2


def text_bytes():
    """Текст ГОСТ и ожидаемые строки UTF-8."""
    data, lines = [], []
    for k in range(6):
        start = 0o20 * k
        label = [int(d) for d in "%03o" % start]  # код начала строки, цифры ГОСТ
        codes = list(range(start, start + 0o20))
        data += label + [GOST_SPACE]
        for c in codes:
            data += [c, GOST_SPACE]
        data.append(GOST_NEWLINE)
        lines.append("%03o " % start + "".join(chr(GOST_UNICODE[c]) + " "
                                          for c in codes))
    # Наложение не из таблицы ПЕЧСВС: «WS=1», возврат в позицию 0 (0173 0),
    # «VS:1» поверх. В режиме SET PRN GOST: S и 1 — жирные, W/V и =/:
    # остаются наложением.
    data += [0o113, 0o110, 0o25, 0o1, GOST_SET_POS, 0, 0o112, 0o110, 0o37,
             0o1, GOST_NEWLINE]
    data.append(GOST_END)
    while len(data) % 6:
        data.append(GOST_END)
    return data, lines


def octal_word(w):
    s = "%016o" % w
    return " ".join(s[i:i + 4] for i in range(0, 16, 4))


def main():
    deck = sys.argv[1] if len(sys.argv) > 1 else "gostprint.b6"
    expected = sys.argv[2] if len(sys.argv) > 2 else "gostprint.expected.txt"

    data, lines = text_bytes()
    words = []
    for i in range(0, len(data), 6):
        w = 0
        for b in data[i:i + 6]:
            w = w << 8 | b
        words.append(w)
    text_end = TEXT_ADDR + len(words) - 1

    out = [
        "ШИФР 419900^",
        "ОЗУ 1^",
        "ВРЕ 5^",
        "ВХО %o^" % CODE_ADDR,
        "АЦП 5^",
        "Е",
        "В %o" % CODE_ADDR,
        "К 00 064 %04o 00 074 0000" % INFO_ADDR,
        "К 00 074 0000 00 074 0000",
        "В %o" % INFO_ADDR,
        "С " + octal_word(TEXT_ADDR << 24 | text_end),
        "С " + octal_word(0o10 << 20),
    ]
    out += ["С " + octal_word(w) for w in words]
    out.append("ЕКОНЕЦ")

    with open(deck, "w", encoding="utf-8") as f:
        f.write("\n".join(out) + "\n")
    with open(expected, "w", encoding="utf-8") as f:
        f.write("\n".join(l.rstrip() for l in lines) + "\n")
    print("%s: текст %05o-%05o (%d слов); %s" %
          (deck, TEXT_ADDR, text_end, len(words), expected))


if __name__ == "__main__":
    main()
