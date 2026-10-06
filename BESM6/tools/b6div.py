#!/usr/bin/env python3
"""
Модель деления АД (016) арифметического устройства БЭСМ-6 - эталон для
besm6_divide() в besm6_arith.c.

Источник: Техническое описание БЭСМ-6, ч. IV "Арифметическое устройство"
(ИЫ1 700 000 ТО-3), п. 4.16, листы 86-102; besm6.github.io/doc/
БЭСМ6-ТО-IV-арифметическое-устройство.pdf.

Алгоритм - деление без восстановления остатка, но остаток хранится в
двухрядном коде (РС1 - суммы, РП1 - переносы), переносы не приводятся.
Приводятся только три старших разряда (41-39) через РСУД/РПУД/РУД (листы
97-99); по ним и по предыдущему действию (ЗУ+ЧВР) таблица 4.4 (лист 92)
выбирает следующее действие: -ЧВР, +ЧВР или только сдвиг.  Частное
набирается двумя составляющими: положительной (РОМ, при -ЧВР) и
отрицательной (ВРУ, при +ЧВР), по 42 разряда (41 + доп. разряд для
округления).  В конце частное = РОМ - ВРУ со специальным округлением по
вентилям листа 102, так что деление нацело всегда точно.

Использование:

    python3 b6div.py WORD1 WORD2      # 48-битные слова (восьм.): WORD1 / WORD2
    python3 b6div.py --old WORD1 WORD2  # прежний nrdiv() из besm6_arith.c
    python3 b6div.py --selftest       # эталоны: литералы Диспака, тест АУ
                                      # (tests/alu.b6), деление нацело
    python3 b6div.py --gen-test [N] > ../tests/div.b6   # тест для SIMH

Эталоны констант (re-dispak avrasp.be/ovd.be Ч10М10, pchkz.be ГРАНЬ):
БЕМШ строит Е'Е-10' десятью делениями 1.0 на 10, а Е'.16666666' - восемью
делениями 16666666 на 10 (снято трассой операций под эмулятором dispak).
На машине (образ 2053) получалось 1755574677547300 и 3712525252434043.
"""

import os
import random
import sys
from fractions import Fraction

BITS40 = (1 << 40) - 1
BITS41 = (1 << 41) - 1
BITS42 = (1 << 42) - 1
BIT41 = 1 << 40
BIT42 = 1 << 41


def bit(x, n):
    """Разряд n (нумерация с 1 справа)."""
    return (x >> (n - 1)) & 1


def split(w):
    """48-битное слово -> (мантисса со знаком в единицах 2^-40, порядок)."""
    m = w & BITS41
    if m & BIT41:
        m -= 1 << 41
    return m, (w >> 41) & 0o177


def join(m, e):
    return ((e & 0o177) << 41) | (m & BITS41)


def au_mantissa_div(x, y):
    """
    Деление мантисс по п. 4.16.  x, y - мантиссы со знаком (единицы 2^-40),
    y нормализован.  Возвращает частное в единицах 2^-40, |q| <= 2 (то есть
    в 42-разрядной сетке СМ с двумя знаковыми разрядами).
    """
    X = x & BITS42                  # 42 разряда: 42 и 41 - знаковые
    Y = y & BITS42
    yneg = y < 0
    LO = (1 << 38) - 1              # двухрядная часть остатка: разряды 38-1

    # Остаток R0 = X/2: старшие разряды 41-39 - на РСУД, младшие 38-1 -
    # однорядным кодом на СМ.  s2/c2 хранят уже сдвинутый влево двухрядный
    # код (разряды 39-1), т.е. на первом такте это сам X: младший разряд
    # делимого при сдвиге вправо не теряется (иначе Е'.16666666' не
    # совпадает с машиной).
    s2 = X & ((1 << 39) - 1)
    c2 = 0
    # РСУД (41,40,39) = разряды 42,41,40 делимого, РПУД = 0
    sud = (bit(X, 42) << 2) | (bit(X, 41) << 1) | bit(X, 40)
    pud = 0
    # РУД: приведённые разряды 40,39 остатка
    rud = (bit(X, 41) << 1) | bit(X, 40)
    zu_plus = (x >= 0) and yneg     # ЗУ+ЧВР перед первым тактом (лист 100)

    pos = neg = 0                   # РОМ, ВРУ
    for _ in range(42):
        # Таблица 4.4: при первом знаке 0 действие повторяет предыдущее,
        # иначе знак остатка - по переносу из приведённых разрядов 40,39;
        # код I,II - знак неизвестен, только сдвиг.
        if not (sud & 4):
            act = '+' if zu_plus else '-'
        else:
            s = (sud & 3) + pud
            if s == 3:
                act = 's'
            else:
                act = '-' if (s >= 4) != yneg else '+'
        zu_plus = act == '+'
        pos = (pos << 1) | (act == '-')
        neg = (neg << 1) | (act == '+')

        if act == 's':
            # Только сдвиг: 41,40 РСУД = 1, разряд 39 двухрядного кода
            # складывается без ВР, перенос - в РПУД40 (лист 98).  Разряд 38
            # (перенос - в РПУД39, как в общей схеме листа 99) лист 98 не
            # упоминает; он добавлен подбором: без него Е'Е-10' получается
            # ...7274 вместо машинного ...7300.  Меняет ~3% частных.
            a, b = bit(s2, 39), bit(c2, 39)
            sud = 0b110 | (a ^ b)
            pud = (a & b) << 1
            a, b = bit(s2, 38), bit(c2, 38)
            pud |= a & b
            lo_s = (s2 & (LO >> 1)) | ((a ^ b) << 37)
            lo_c = c2 & (LO >> 1)
        else:
            # +ЧВР - прямой код делителя, -ЧВР - обратный и 1 в 1-й разряд
            if act == '+':
                yv, inj = Y, 0
            else:
                yv, inj = ~Y & BITS42, 1
            # Трёхрядное поразрядное сложение разрядов 39-1
            ys = yv & ((1 << 39) - 1)
            sm = s2 ^ c2 ^ ys
            cr = ((s2 & c2) | (s2 & ys) | (c2 & ys)) << 1 | inj
            # Лист 99: разряд 39 суммы -> РСУД39, перенос из 39 -> РПУД40,
            # перенос из 38 -> РПУД39; разряды 41,40 - сдвинутый РУД + ВР.
            sud39 = bit(sm, 39)
            pud40 = bit(cr, 40)
            pud39 = bit(cr, 39)
            t39, t40 = rud & 1, rud >> 1
            y40, y41 = bit(yv, 40), bit(yv, 41)
            sud40 = t39 ^ y40
            sud41 = t40 ^ y41 ^ (t39 & y40)
            sud = (sud41 << 2) | (sud40 << 1) | sud39
            pud = (pud40 << 1) | pud39
            lo_s = sm & LO
            lo_c = cr & LO
        # РУД = приведённые разряды 40,39 нового остатка
        rud = ((sud & 3) + pud) & 3
        # Сдвиг влево для следующего такта
        s2 = lo_s << 1
        c2 = lo_c << 1

    # Окончание: частное = РОМ + обр.код(ВРУ) + ДПР, плюс 1рРС2.
    pe, ne = pos & 1, neg & 1
    pm, nm = pos >> 1, neg >> 1
    p1, n1 = pm & 1, nm & 1
    dpr = pe | ((not ne) and (not pe)) | ((not p1) and ne and (not n1))
    r1 = pe & (n1 | p1)
    q = (pm + (~nm & BITS42) + int(dpr) + int(r1)) & BITS42
    if q & BIT42:
        q -= 1 << 42
    return q


def old_nrdiv(nn, dd):
    """Прежний nrdiv() из besm6_arith.c (для сравнения)."""
    res, q = 0, BIT41
    nn *= 2
    dd *= 2
    ex = 0
    if abs(nn) >= abs(dd):
        nn //= 2
        ex = 1
    while q > 1:
        if nn == 0:
            break
        if abs(nn) < (1 << 39):
            nn *= 2
        elif (nn > 0) ^ (dd > 0):
            res -= q
            nn = 2 * nn + dd
        else:
            res += q
            nn = 2 * nn - dd
        q //= 2
    return int(res / 2), ex


def normalize(m, e):
    """Нормализация результата (как normalize_and_round, без округления)."""
    if m == 0:
        return 0, 0
    if m >= BIT41 or m < -BIT41:
        m >>= 1
        e += 1
    while -(BIT41 >> 1) <= m < (BIT41 >> 1):
        m <<= 1
        e -= 1
    return m, e


def divide(w1, w2, old=False):
    """АД: слово w1 / слово w2 -> 48-битное слово."""
    x, ex = split(w1)
    y, ey = split(w2)
    if old:
        q, d = old_nrdiv(x, y)
        e = ex - ey + 64 + d
        m, e = normalize(q, e)
    else:
        q = au_mantissa_div(x, y)
        m, e = normalize(q, ex - ey + 64)
    if m == 0 or e < 0:
        return 0
    return join(m, e)


TEN = 0o4212000000000000


def chain(w, n, old=False):
    """n делений на 10 подряд - так БЕМШ строит десятичные литералы."""
    for _ in range(n):
        w = divide(w, TEN, old)
    return w


# Литералы БЕМШ: (текст, целая мантисса, число делений на 10, слово с машины).
# Е'Е-10' и Е'.16666666' в эмуляторе расходились с машиной (патчи В'176'
# и В'1' в re-dispak), остальные совпадали и раньше.
LITERALS = [
    ("Е'Е-10'", 1, 10, 0o1755574677547300),
    ("Е'.16666666'", 16666666, 8, 0o3712525252434043),
    ("Е'2.4'", 24, 1, 0o4111463146314632),
    ("Е'1.41'", 141, 2, 0o4053217270243656),
    ("Е'0.02'", 2, 2, 0o3552172702436561),
]


def load_b6(path):
    """Слова текстового образа .b6 (строки в/с/к) -> {адрес: слово}."""
    mem, addr = {}, 0
    for line in open(path, encoding='utf-8'):
        line = line.split(';')[0].strip()
        if not line:
            continue
        f = line.split()
        if f[0] == 'в':
            addr = int(f[1], 8)
        elif f[0] == 'с':
            mem[addr] = int(''.join(f[1:]), 8)
            addr += 1
        elif f[0] == 'к':
            addr += 1
    return mem


def alu_cases():
    """Деления из tests/alu.b6: (делимое, делитель, ожидаемое частное)."""
    import re
    here = os.path.dirname(os.path.abspath(__file__))
    path = os.path.join(here, '..', 'tests', 'alu.b6')
    mem = load_b6(path)
    cases = []
    lines = open(path, encoding='utf-8').read().split('\n')
    for i, ln in enumerate(lines):
        m = re.match(r'к сч (\d+)\(1\), дел (\d+)\(1\)', ln)
        if not m:
            continue
        n = re.match(r'к нтж (\d+)\(1\)', lines[i + 1])
        if not n:
            continue
        a, b, r = (0o30000 + int(v, 8) for v in
                   (m.group(1), m.group(2), n.group(1)))
        cases.append((mem[a], mem[b], mem[r]))
    return cases


def selftest():
    ok = True

    def check(name, got, want):
        nonlocal ok
        flag = 'ok' if got == want else 'FAIL'
        if got != want:
            ok = False
        print(f'{flag:4} {name}: {got:016o} (эталон {want:016o})')

    for text, n, k, want in LITERALS:
        check(text, chain(int_word(n), k), want)
    for a, b, r in alu_cases():
        check(f'alu {a:016o}/{b:016o}', divide(a, b), r)
    rnd = random.Random(1)
    # Деление нацело всегда точно
    bad = 0
    for _ in range(20000):
        yi = rnd.randrange(1, 1 << 20) * rnd.choice((1, -1))
        k = rnd.randrange(1, 1 << 19) * rnd.choice((1, -1))
        if divide(int_word(yi * k), int_word(yi)) != int_word(k):
            bad += 1
    print(f'{"ok" if not bad else "FAIL":4} деление нацело: {bad} ошибок')
    ok = ok and not bad
    # Ошибка мантиссы частного меньше единицы младшего разряда
    bad = 0
    for _ in range(20000):
        x = rnd.randrange(-BIT41, BIT41)
        y = rnd.randrange(BIT41 >> 1, BIT41) * rnd.choice((1, -1))
        if abs(Fraction(au_mantissa_div(x, y)) - Fraction(x * BIT41, y)) >= 1:
            bad += 1
    print(f'{"ok" if not bad else "FAIL":4} ошибка < 1 ед.: {bad} нарушений')
    return ok and not bad


def gen_test(n):
    """Самопроверяющий тест для SIMH (tests/div.b6): n случайных делений
    плюс литералы и деления нацело; ожидаемые частные - из этой модели."""
    rnd = random.Random(2)
    rows = []
    for text, m, k, _ in LITERALS:
        w = int_word(m)
        for i in range(k):
            rows.append((w, TEN, divide(w, TEN), f"{text} шаг {i + 1}"))
            w = divide(w, TEN)
    for yi, k in ((3, 1), (3, -7), (-10, 12345), (7, 7), (1, 0)):
        a, b = int_word(yi * k), int_word(yi)
        rows.append((a, b, divide(a, b), f'{yi * k}/{yi}'))
    while len(rows) < n:
        x = rnd.randrange(-BIT41, BIT41)
        y = rnd.randrange(BIT41 >> 1, BIT41) * rnd.choice((1, -1))
        a = join(x, rnd.randrange(48, 80))
        b = join(y, rnd.randrange(48, 80))
        rows.append((a, b, divide(a, b), ''))
    n = len(rows)
    ta, tb, tr = 0o100, 0o100 + n, 0o100 + 2 * n
    out = [
        ';',
        '; Тест деления АД (016): сгенерирован BESM6/tools/b6div.py --gen-test.',
        '; Делит строку за строкой таблицы делимых/делителей и сверяет с',
        '; таблицей частных, посчитанных моделью АУ по ТО (п. 4.16).',
        '; Успех - останов на 014, расхождение - на 015 (номер строки в М1:',
        f'; строка = М1 + {n - 1}).',
        ';',
        'в 10',
        f'к уиа {(-(n - 1)) & 0o77777:o}(1), мода',
        f'к сч {ta + n - 1:o}(1), дел {tb + n - 1:o}(1)   ; 11',
        f'к нтж {tr + n - 1:o}(1), пе 15',
        'к цикл 11(1), мода',
        'к стоп 14, мода                 ; 14: всё совпало',
        'к стоп 15, мода                 ; 15: расхождение',
    ]
    for t, col in ((ta, 0), (tb, 1), (tr, 2)):
        out.append(f'в {t:o}')
        for row in rows:
            c = f'с {row[col]:016o}'
            if col == 2 and row[3]:
                c += f'    ; {row[3]}'
            out.append(c)
    out.append('п 10')
    return '\n'.join(out) + '\n'


def int_word(n):
    """Целое -> нормализованное слово с плавающей запятой."""
    if n == 0:
        return 0
    e = 64 + 40
    m = n
    while m >= BIT41 or m < -BIT41:
        m >>= 1
        e += 1
    m, e = normalize(m, e)
    return join(m, e)


def main():
    args = sys.argv[1:]
    if args and args[0] == '--selftest':
        sys.exit(0 if selftest() else 1)
    if args and args[0] == '--gen-test':
        sys.stdout.write(gen_test(int(args[1]) if len(args) > 1 else 200))
        return
    old = bool(args) and args[0] == '--old'
    if old:
        args = args[1:]
    if len(args) != 2:
        print(__doc__)
        sys.exit(2)
    print(f'{divide(int(args[0], 8), int(args[1], 8), old=old):016o}')


if __name__ == '__main__':
    main()
