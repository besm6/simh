#!/usr/bin/env python3
"""
Деление нечётных чисел 1..999 на нечётные 1..999 по модели АУ БЭСМ-6
(b6div.py, тот же алгоритм, что в besm6_arith.c).  Печатает строки

    делимое делитель частное

делимое и делитель - десятичные, частное - 48-битное слово в восьмеричном
виде.  С ключом --old деление прежним nrdiv(); --signs XY задаёт знаки
делимого и делителя (++ по умолчанию, --, -+, +-).  С ключом --random N
вместо этого N пар: случайная мантисса делимого (равномерно, в том числе
ненормализованная) и случайная нормализованная мантисса делителя, порядки 0;
делимое и делитель печатаются словами с префиксом 0o.

    python3 odddiv.py [--old] [--signs -+] [--random 250000] > odddiv.txt
    python3 odddiv_check.py odddiv.txt
"""

import random
import sys

import b6div


def main():
    args = sys.argv[1:]
    old = '--old' in args
    signs = args[args.index('--signs') + 1] if '--signs' in args else '++'
    sa, sb = (-1 if c == '-' else 1 for c in signs)
    out = sys.stdout
    if '--random' in args:
        rnd = random.Random(3)
        top = b6div.BIT41
        for _ in range(int(args[args.index('--random') + 1])):
            x = sa * rnd.randrange(1, top)
            y = sb * rnd.randrange(top >> 1, top)
            a = b6div.join(x, 64)
            b = b6div.join(y, 64)
            q = b6div.divide(a, b, old=old)
            out.write(f'0o{a:016o} 0o{b:016o} {q:016o}\n')
        return
    words = {n: b6div.int_word(n) for n in range(-999, 1000, 2)}
    for a in range(sa, sa * 1000, sa * 2):
        for b in range(sb, sb * 1000, sb * 2):
            q = b6div.divide(words[a], words[b], old=old)
            out.write(f'{a} {b} {q:016o}\n')


if __name__ == '__main__':
    main()
