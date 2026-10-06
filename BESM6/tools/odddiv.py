#!/usr/bin/env python3
"""
Деление нечётных чисел 1..999 на нечётные 1..999 по модели АУ БЭСМ-6
(b6div.py, тот же алгоритм, что в besm6_arith.c).  Печатает строки

    делимое делитель частное

делимое и делитель - десятичные, частное - 48-битное слово в восьмеричном
виде.  С ключом --old деление прежним nrdiv(); --signs XY задаёт знаки
делимого и делителя (++ по умолчанию, --, -+, +-).

    python3 odddiv.py [--old] [--signs -+] > odddiv.txt
    python3 odddiv_check.py odddiv.txt
"""

import sys

import b6div


def main():
    args = sys.argv[1:]
    old = '--old' in args
    signs = args[args.index('--signs') + 1] if '--signs' in args else '++'
    sa, sb = (-1 if c == '-' else 1 for c in signs)
    words = {n: b6div.int_word(n) for n in range(-999, 1000, 2)}
    out = sys.stdout
    for a in range(sa, sa * 1000, sa * 2):
        for b in range(sb, sb * 1000, sb * 2):
            q = b6div.divide(words[a], words[b], old=old)
            out.write(f'{a} {b} {q:016o}\n')


if __name__ == '__main__':
    main()
