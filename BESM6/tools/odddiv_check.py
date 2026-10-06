#!/usr/bin/env python3
"""
Проверка выдачи odddiv.py: для каждой строки "делимое делитель частное"
(частное - 48-битное слово БЭСМ-6 в восьмеричном виде) сравнивает частное с
точным значением делимое/делитель и считает точные, с недостатком и с
избытком; для неточных - ещё и величину ошибки в единицах младшего разряда.

    python3 odddiv_check.py odddiv.txt
"""

import sys
from collections import Counter
from fractions import Fraction


def word_value(w):
    """48-битное слово -> точное значение (Fraction)."""
    m = w & ((1 << 41) - 1)
    if m & (1 << 40):
        m -= 1 << 41
    e = (w >> 41) & 0o177
    return Fraction(m, 1 << 40) * Fraction(2) ** (e - 64)


def ulp(w):
    """Единица младшего разряда слова w."""
    return Fraction(2) ** (((w >> 41) & 0o177) - 64 - 40)


def main():
    stat = Counter()
    errs = Counter()
    int_q = Counter()
    mag = Counter()
    for line in open(sys.argv[1]):
        a, b, q = line.split()
        a, b, w = int(a), int(b), int(q, 8)
        exact = Fraction(a, b)
        got = word_value(w)
        if got == exact:
            kind = 'точно'
        elif got < exact:
            kind = 'с недостатком'
        else:
            kind = 'с избытком'
        stat[kind] += 1
        if abs(got) != abs(exact):
            mag['меньше' if abs(got) < abs(exact) else 'больше'] += 1
        if got != exact:
            err = (got - exact) / ulp(w)
            errs[float(abs(err))] += 1
        if a % b == 0:
            int_q[kind] += 1
    total = sum(stat.values())
    print(f'всего делений: {total}')
    for kind in ('точно', 'с недостатком', 'с избытком'):
        print(f'  {kind:14} {stat[kind]:7}  ({100 * stat[kind] / total:.2f}%)')
    print(f'  по модулю: меньше точного {mag["меньше"]}, больше {mag["больше"]}')
    print(f'из них частное целое (делится нацело): {sum(int_q.values())}')
    for kind in ('точно', 'с недостатком', 'с избытком'):
        print(f'  {kind:14} {int_q[kind]:7}')
    worst = max(errs, default=0)
    print(f'наибольшая ошибка: {worst:.6f} ед. младшего разряда')


if __name__ == '__main__':
    main()
