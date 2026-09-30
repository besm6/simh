#!/usr/bin/env python3
"""
Таблицы перекодировки для дисплеев ЕС-7920 (svs_display.c):
ДКОИ <-> кодовая страница клиента tn3270 (IBM1025, IBM880).

ДКОИ -> Unicode берётся из таблицы dkoi_to_unicode в svs_printer.c (ТДКОИ:
код, общий для латинской и русской буквы одного начертания, отдан русской).
Unicode <-> IBM1025/IBM880 — по iconv хоста.

Обратно (клавиатура клиента -> ДКОИ) латинские буквы, совпадающие по
начертанию с русскими, и строчные буквы сводятся к кодам ДКОИ, как их набрал
бы оператор ЕС-7927: у ОС только прописные, а латинская A и русская А — один
код 0xC1.

    tools/mkdispcp.py > svs_display_cp.h
"""
import re
import subprocess
import sys

HERE = __file__.rsplit('/', 2)[0] if '/' in __file__ else '.'
PRINTER = HERE + '/svs_printer.c'

CODEPAGES = [('cp1025', 'IBM1025'), ('cp880', 'IBM880')]

# Латиница, совпадающая по начертанию с русской буквой ДКОИ.
HOMOGLYPH = dict(zip('ABCEHKMOPTXY', 'АВСЕНКМОРТХУ'))

# Знак, которого нет в кодовой странице клиента -> близкий, если он есть.
SUBSTITUTE = {0x00A6: 0x007C}           # ¦ -> |


def dkoi_table():
    src = open(PRINTER, encoding='utf-8').read()
    body = src[src.index('dkoi_to_unicode[256]'):]
    body = body[:body.index('};')]
    t = [0] * 256
    for code, val in re.findall(r"\[0x([0-9A-Fa-f]{2})\]\s*=\s*(0x[0-9A-Fa-f]+|'[^']*'|'\\'')", body):
        if val.startswith('0x'):
            u = int(val, 16)
        else:
            u = ord(val[1:-1].replace("\\'", "'"))
        t[int(code, 16)] = u
    return t


def cp_to_unicode(iconv_name):
    m = [None] * 256
    for b in range(256):
        r = subprocess.run(['iconv', '-f', iconv_name, '-t', 'UTF-32BE'],
                           input=bytes([b]), capture_output=True)
        if r.returncode == 0 and len(r.stdout) == 4:
            m[b] = int.from_bytes(r.stdout, 'big')
    return m


def fold(u):
    """Символ с клавиатуры -> символ набора ДКОИ."""
    c = chr(u).upper()
    return ord(HOMOGLYPH.get(c, c))


def emit(name, table):
    print('static const unsigned char %s[256] = {' % name)
    for i in range(0, 256, 16):
        print('    ' + ' '.join('0x%02X,' % v for v in table[i:i + 16]))
    print('};')


def main():
    dkoi = dkoi_table()
    uni_to_dkoi = {}
    for code, u in enumerate(dkoi):
        if u:
            uni_to_dkoi.setdefault(u, code)

    print('/*')
    print(' * Сгенерировано tools/mkdispcp.py — не править руками.')
    print(' * ДКОИ <-> кодовые страницы клиента tn3270 для svs_display.c.')
    print(' * Ноль остаётся нулём (пустая позиция экрана), непредставимый')
    print(' * знак — пробел 0x40 (так поступает и ОС, НЕДСЕС).')
    print(' */')
    for short, iconv_name in CODEPAGES:
        c2u = cp_to_unicode(iconv_name)
        u2c = {}
        for b, u in enumerate(c2u):
            if u is not None and b >= 0x40:
                u2c.setdefault(u, b)
        to_cp = [0x40] * 256
        to_dkoi = [0x40] * 256
        to_cp[0] = to_dkoi[0] = 0
        for code, u in enumerate(dkoi):
            if u and u not in u2c and SUBSTITUTE.get(u) in u2c:
                u = SUBSTITUTE[u]
            if u and u in u2c:
                to_cp[code] = u2c[u]
        for b, u in enumerate(c2u):
            if b == 0 or u is None:
                continue
            f = fold(u)
            if f in uni_to_dkoi:
                to_dkoi[b] = uni_to_dkoi[f]
        missing = [hex(c) for c, u in enumerate(dkoi)
                   if u and u not in u2c and SUBSTITUTE.get(u) not in u2c]
        if missing:
            print('/* %s: нет в кодовой странице: %s */' % (short, ' '.join(missing)))
        emit('dkoi_to_%s' % short, to_cp)
        emit('%s_to_dkoi' % short, to_dkoi)


if __name__ == '__main__':
    sys.exit(main())
