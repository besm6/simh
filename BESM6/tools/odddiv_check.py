#!/usr/bin/env python3
"""
Проверка выдачи odddiv.py: для каждой строки "делимое делитель частное"
(частное - 48-битное слово БЭСМ-6 в восьмеричном виде; делимое и
делитель - десятичные целые или, с префиксом 0o, слова) сравнивает частное с
точным значением делимое/делитель и считает точные, с недостатком и с
избытком, отдельно - для ненормализованного делимого и для деления нацело;
наибольшую ошибку - в единицах сетки, в которой АУ считает частное.

    python3 odddiv_check.py odddiv.txt
"""

import sys
from collections import Counter
from fractions import Fraction

import b6div


def word_value(w):
    """48-битное слово -> точное значение (Fraction)."""
    m = w & ((1 << 41) - 1)
    if m & (1 << 40):
        m -= 1 << 41
    e = (w >> 41) & 0o177
    return Fraction(m, 1 << 40) * Fraction(2) ** (e - 64)


def grid(wa, wb, exact):
    """Сетка частного в АУ до нормализации: 2^(pX-pY-40), при |q| >= 1 -
    вдвое крупнее (нормализация вправо).  Для нормализованного делимого
    совпадает с единицей младшего разряда результата."""
    e = ((wa >> 41) & 0o177) - ((wb >> 41) & 0o177)
    g = Fraction(2) ** (e - 40)
    if abs(exact) >= Fraction(2) ** e:
        g *= 2
    return g


def normalized(w):
    return ((w >> 40) ^ (w >> 39)) & 1


def main():
    stat = Counter()
    errs = []
    int_q = Counter()
    mag = Counter()
    unnorm = Counter()
    for line in open(sys.argv[1]):
        a, b, q = line.split()
        w = int(q, 8)
        if a.startswith('0o'):
            # слова (odddiv.py --random)
            wa, wb = int(a, 8), int(b, 8)
        else:
            wa, wb = b6div.int_word(int(a)), b6div.int_word(int(b))
        exact = word_value(wa) / word_value(wb)
        got = word_value(w)
        if got == exact:
            kind = 'точно'
        elif got < exact:
            kind = 'с недостатком'
        else:
            kind = 'с избытком'
        stat[kind] += 1
        if not normalized(wa):
            unnorm[kind] += 1
        if abs(got) != abs(exact):
            mag['меньше' if abs(got) < abs(exact) else 'больше'] += 1
        if got != exact:
            errs.append(float(abs(got - exact) / grid(wa, wb, exact)))
        if exact.denominator == 1:
            int_q[kind] += 1
    total = sum(stat.values())
    print(f'всего делений: {total}')
    for kind in ('точно', 'с недостатком', 'с избытком'):
        print(f'  {kind:14} {stat[kind]:7}  ({100 * stat[kind] / total:.2f}%)')
    print(f'  по модулю: меньше точного {mag["меньше"]}, больше {mag["больше"]}')
    n = sum(unnorm.values())
    print(f'делимое ненормализовано: {n}')
    for kind in ('точно', 'с недостатком', 'с избытком'):
        print(f'  {kind:14} {unnorm[kind]:7}')
    print(f'частное целое (делится нацело): {sum(int_q.values())}')
    for kind in ('точно', 'с недостатком', 'с избытком'):
        print(f'  {kind:14} {int_q[kind]:7}')
    print(f'наибольшая ошибка: {max(errs, default=0):.6f} ед. сетки частного'
          f' (2^(pX-pY-40), при |q| >= 1 - 2^(pX-pY-39))')


if __name__ == '__main__':
    main()
