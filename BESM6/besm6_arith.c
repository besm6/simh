/*
 * BESM-6 arithmetic instructions.
 *
 * Copyright (c) 1997-2009, Leonid Broukhis
 * Copyright (c) 2009, Serge Vakulenko
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:

 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.

 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * SERGE VAKULENKO OR LEONID BROUKHIS BE LIABLE FOR ANY CLAIM, DAMAGES
 * OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE
 * OR OTHER DEALINGS IN THE SOFTWARE.

 * Except as contained in this notice, the name of Leonid Broukhis or
 * Serge Vakulenko shall not be used in advertising or otherwise to promote
 * the sale, use or other dealings in this Software without prior written
 * authorization from Leonid Broukhis and Serge Vakulenko.
 */
#include "besm6_defs.h"

typedef struct {
    t_uint64 mantissa;
    unsigned exponent;                  /* offset by 64 */
} alureg_t;                             /* ALU register type */

static alureg_t toalu (t_value val)
{
    alureg_t ret;

    ret.mantissa = val & BITS41;
    ret.exponent = (val >> 41) & BITS(7);
    if (ret.mantissa & BIT41)
        ret.mantissa |= BIT42;
    return ret;
}

static int SIM_INLINE is_negative (alureg_t *word)
{
    return (word->mantissa & BIT41) != 0;
}

static void negate (alureg_t *val)
{
    if (is_negative (val))
        val->mantissa |= BIT42;
    val->mantissa = (~val->mantissa + 1) & BITS42;
    if (((val->mantissa >> 1) ^ val->mantissa) & BIT41) {
        val->mantissa >>= 1;
        ++val->exponent;
    }
    if (is_negative (val))
        val->mantissa |= BIT42;
}

/*
 * 48-й разряд -> 1, 47-й -> 2 и т.п.
 * Единица 1-го разряда и нулевое слово -> 48,
 * как в первоначальном варианте системы команд.
 */
int besm6_highest_bit (t_value val)
{
    int n = 32, cnt = 0;
    do {
        t_value tmp = val;
        if (tmp >>= n) {
            cnt += n;
            val = tmp;
        }
    } while (n >>= 1);
    return 48 - cnt;
}

/*
 * Нормализация и округление.
 * Результат помещается в регистры ACC и 40-1 разряды RMR.
 * 48-41 разряды RMR сохраняются.
 */
static void normalize_and_round (alureg_t acc, t_uint64 mr, int rnd_rq)
{
    t_uint64 rr = 0;
    int i;
    t_uint64 r;

    if (RAU & RAU_NORM_DISABLE)
        goto chk_rnd;
    i = (acc.mantissa >> 39) & 3;
    if (i == 0) {
        r = acc.mantissa & BITS40;
        if (r) {
            int cnt = besm6_highest_bit (r) - 9;
            r <<= cnt;
            rr = mr >> (40 - cnt);
            acc.mantissa = r | rr;
            mr <<= cnt;
            acc.exponent -= cnt;
            goto chk_zero;
        }
        r = mr & BITS40;
        if (r) {
            int cnt = besm6_highest_bit (r) - 9;
            rr = mr;
            r <<= cnt;
            acc.mantissa = r;
            mr = 0;
            acc.exponent -= 40 + cnt;
            goto chk_zero;
        }
        goto zero;
    } else if (i == 3) {
        r = ~acc.mantissa & BITS40;
        if (r) {
            int cnt = besm6_highest_bit (r) - 9;
            r = (r << cnt) | ((1LL << cnt) - 1);
            rr = mr >> (40 - cnt);
            acc.mantissa = BIT41 | (~r & BITS40) | rr;
            mr <<= cnt;
            acc.exponent -= cnt;
            goto chk_zero;
        }
        r = ~mr & BITS40;
        if (r) {
            int cnt = besm6_highest_bit (r) - 9;
            rr = mr;
            r = (r << cnt) | ((1LL << cnt) - 1);
            acc.mantissa = BIT41 | (~r & BITS40);
            mr = 0;
            acc.exponent -= 40 + cnt;
            goto chk_zero;
        } else {
            rr = 1;
            acc.mantissa = BIT41;
            mr = 0;
            acc.exponent -= 80;
            goto chk_zero;
        }
    }
  chk_zero:
    if (rr)
        rnd_rq = 0;
  chk_rnd:
    if (acc.exponent & 0x8000)
        goto zero;
    if (! (RAU & RAU_ROUND_DISABLE) && rnd_rq)
        acc.mantissa |= 1;

    if (! acc.mantissa && ! (RAU & RAU_NORM_DISABLE)) {
      zero:           ACC = 0;
        RMR &= ~BITS40;
        return;
    }

    ACC = (t_value) (acc.exponent & BITS(7)) << 41 |
        (acc.mantissa & BITS41);
    RMR = (RMR & ~BITS40) | (mr & BITS40);
    /* При переполнении мантисса и младшие разряды порядка верны */
    if (acc.exponent & 0x80) {
        if (! (RAU & RAU_OVF_DISABLE))
            longjmp (cpu_halt, STOP_OVFL);
    }
}

/*
 * Сложение и все варианты вычитаний.
 * Исходные значения: регистр ACC и аргумент 'val'.
 * Результат помещается в регистр ACC и 40-1 разряды RMR.
 */
void besm6_add (t_value val, int negate_acc, int negate_val)
{
    t_uint64 mr;
    alureg_t acc, word, a1, a2;
    int diff, neg, rnd_rq = 0;

    acc = toalu (ACC);
    word = toalu (val);
    if (! negate_acc) {
        if (! negate_val) {
            /* Сложение */
        } else {
            /* Вычитание */
            negate (&word);
        }
    } else {
        if (! negate_val) {
            /* Обратное вычитание */
            negate (&acc);
        } else {
            /* Вычитание модулей */
            if (is_negative (&acc))
                negate (&acc);
            if (! is_negative (&word))
                negate (&word);
        }
    }

    diff = acc.exponent - word.exponent;
    if (diff < 0) {
        diff = -diff;
        a1 = acc;
        a2 = word;
    } else {
        a1 = word;
        a2 = acc;
    }
    mr = 0;
    neg = is_negative (&a1);
    if (diff == 0) {
        /* Nothing to do. */
    } else if (diff <= 40) {
        rnd_rq = (mr = (a1.mantissa << (40 - diff)) & BITS40) != 0;
        a1.mantissa = ((a1.mantissa >> diff) |
                       (neg ? (~0ll << (40 - diff)) : 0)) & BITS42;
    } else if (diff <= 80) {
        diff -= 40;
        rnd_rq = a1.mantissa != 0;
        mr = ((a1.mantissa >> diff) |
              (neg ? (~0ll << (40 - diff)) : 0)) & BITS40;
        if (neg) {
            a1.mantissa = BITS42;
        } else
            a1.mantissa = 0;
    } else {
        rnd_rq = a1.mantissa != 0;
        if (neg) {
            mr = BITS40;
            a1.mantissa = BITS42;
        } else
            mr = a1.mantissa = 0;
    }
    acc.exponent = a2.exponent;
    acc.mantissa = a1.mantissa + a2.mantissa;

    /* Если требуется нормализация вправо, биты 42:41
     * принимают значение 01 или 10. */
    switch ((acc.mantissa >> 40) & 3) {
    case 2:
    case 1:
        rnd_rq |= acc.mantissa & 1;
        mr = (mr >> 1) | ((acc.mantissa & 1) << 39);
        acc.mantissa >>= 1;
        ++acc.exponent;
    }
    normalize_and_round (acc, mr, rnd_rq);
}

/*
 * Деление мантисс так, как его выполняет АУ (ТО ИЫ1 700 000 ТО-3, ч. IV,
 * п. 4.16, листы 86-102).  Деление без восстановления остатка, но остаток
 * хранится в двухрядном коде (суммы и переносы), переносы не приводятся;
 * приводятся только старшие разряды 41-39 (РСУД, РПУД, РУД).  По ним и по
 * предыдущему действию (ЗУ+ЧВР) выбирается следующее действие: вычитание
 * делителя, прибавление или только сдвиг (таблица 4.4).  Частное набирается
 * двумя составляющими по 42 разряда (41 + доп. разряд): положительной (РОМ)
 * и отрицательной (ВРУ); в конце частное = РОМ - ВРУ со специальным
 * округлением (лист 102), при котором деление нацело всегда точно.
 *
 * Мантиссы - в 42-разрядном дополнительном коде (два знаковых разряда),
 * делитель нормализован.  Результат: мантисса частного в той же сетке,
 * |q| <= 2, нормализация - за вызывающим.  Эталонная модель:
 * tools/b6div.py.
 */
#define DBIT(x,n) ((int) ((x) >> ((n)-1)) & 1)
static t_uint64 au_divide (t_uint64 x, t_uint64 y)
{
    const t_uint64 lo = BITS40 >> 2;            /* двухрядная часть: 38..1 */
    const t_uint64 lo39 = BITS40 >> 1;          /* разряды 39..1 */
    int yneg = DBIT(y, 41);
    t_uint64 s2, c2, yv, sm, cr, pos, neg, lo_s, lo_c;
    int sud, pud, rud, zu_plus, act, i, a, b, t39, t40;
    int pe, ne, p1, n1, dpr, r1;

    /* Остаток R0 = X/2: старшие разряды 41-39 - на РСУД, младшие -
     * однорядным кодом на СМ.  s2/c2 - двухрядный код остатка, уже
     * сдвинутый влево (разряды 39-1), на первом такте это сам X. */
    s2 = x & lo39;
    c2 = 0;
    sud = DBIT(x, 42) << 2 | DBIT(x, 41) << 1 | DBIT(x, 40);
    pud = 0;
    rud = DBIT(x, 41) << 1 | DBIT(x, 40);   /* разряды 40,39 остатка */
    zu_plus = !DBIT(x, 41) && yneg;         /* лист 100 */
    pos = neg = 0;

    for (i = 0; i < 42; i++) {
        /* Таблица 4.4: при первом знаке 0 действие повторяет предыдущее;
         * иначе знак остатка - по переносу из приведённых разрядов 40,39;
         * код I,II - знак неизвестен, только сдвиг. */
        if (! (sud & 4))
            act = zu_plus ? '+' : '-';
        else if ((sud & 3) + pud == 3)
            act = 's';
        else
            act = (((sud & 3) + pud >= 4) != yneg) ? '-' : '+';
        zu_plus = (act == '+');
        pos = pos << 1 | (act == '-');
        neg = neg << 1 | (act == '+');

        if (act == 's') {
            /* Только сдвиг: 41,40 РСУД = 1, разряд 39 двухрядного кода
             * складывается без делителя, перенос - в РПУД40 (лист 98).
             * Разряд 38 (перенос - в РПУД39, как в схеме листа 99) лист 98
             * не упоминает: он подобран по константе Е'Е-10' Диспака. */
            a = DBIT(s2, 39);
            b = DBIT(c2, 39);
            sud = 6 | (a ^ b);
            pud = (a & b) << 1;
            a = DBIT(s2, 38);
            b = DBIT(c2, 38);
            pud |= a & b;
            lo_s = (s2 & (lo >> 1)) | (t_uint64) (a ^ b) << 37;
            lo_c = c2 & (lo >> 1);
        } else {
            /* +ЧВР - прямой код делителя, -ЧВР - обратный и 1 в 1-й разряд */
            yv = (act == '+') ? y : ~y & BITS42;
            sm = s2 ^ c2 ^ (yv & lo39);
            cr = ((s2 & c2) | (s2 & yv & lo39) | (c2 & yv & lo39)) << 1;
            if (act == '-')
                cr |= 1;
            /* Разряд 39 суммы - в РСУД, переносы из 39 и 38 - в РПУД;
             * разряды 41,40 - сдвинутый РУД плюс делитель. */
            t39 = rud & 1;
            t40 = rud >> 1;
            sud = (t40 ^ DBIT(yv, 41) ^ (t39 & DBIT(yv, 40))) << 2 |
                (t39 ^ DBIT(yv, 40)) << 1 | DBIT(sm, 39);
            pud = DBIT(cr, 40) << 1 | DBIT(cr, 39);
            lo_s = sm & lo;
            lo_c = cr & lo;
        }
        /* РУД - приведённые разряды 40,39 нового остатка */
        rud = ((sud & 3) + pud) & 3;
        s2 = lo_s << 1;
        c2 = lo_c << 1;
    }

    /* Частное = РОМ + обратный код ВРУ + ДПР, плюс 1 округления в 1рРС2 */
    pe = pos & 1;
    ne = neg & 1;
    pos >>= 1;
    neg >>= 1;
    p1 = pos & 1;
    n1 = neg & 1;
    dpr = pe || (!ne && !pe) || (!p1 && ne && !n1);
    r1 = pe && (n1 || p1);
    return (pos + (~neg & BITS42) + dpr + r1) & BITS42;
}

/*
 * Деление.
 * Исходные значения: регистр ACC и аргумент 'val'.
 * Результат помещается в регистр ACC, содержимое RMR не определено.
 */
void besm6_divide (t_value val)
{
    alureg_t acc;
    alureg_t dividend, divisor;

    if (((val ^ (val << 1)) & BIT41) == 0) {
        /* Ненормализованный делитель: деление на ноль. */
        longjmp (cpu_halt, STOP_DIVZERO);
    }
    dividend = toalu(ACC);
    divisor = toalu(val);

    acc.mantissa = au_divide (dividend.mantissa, divisor.mantissa);
    acc.exponent = dividend.exponent - divisor.exponent + 64;
    if (((acc.mantissa >> 1) ^ acc.mantissa) & BIT41) {
        /* Нормализация вправо */
        acc.mantissa = acc.mantissa >> 1 | (acc.mantissa & BIT42);
        ++acc.exponent;
    }

    normalize_and_round (acc, 0, 0);
}

/*
 * Умножение.
 * Исходные значения: регистр ACC и аргумент 'val'.
 * Результат помещается в регистр ACC и 40-1 разряды RMR.
 */
void besm6_multiply (t_value val)
{
    uint8           neg = 0;
    alureg_t        acc, word, a, b;
    t_uint64        mr, alo, blo, ahi, bhi;

    register t_uint64 l;

    if (! ACC || ! val) {
        /* multiplication by zero is zero */
        ACC = 0;
        RMR &= ~BITS40;
        return;
    }
    acc = toalu (ACC);
    word = toalu (val);

    a = acc;
    b = word;
    mr = 0;

    if (is_negative (&a)) {
        neg = 1;
        negate (&a);
    }
    if (is_negative (&b)) {
        neg ^= 1;
        negate (&b);
    }
    acc.exponent = a.exponent + b.exponent - 64;

    alo = a.mantissa & BITS(20);
    ahi = a.mantissa >> 20;

    blo = b.mantissa & BITS(20);
    bhi = b.mantissa >> 20;

    l = alo * blo + ((alo * bhi + ahi * blo) << 20);

    mr = l & BITS40;
    l >>= 40;

    acc.mantissa = l + ahi * bhi;

    if (neg && (acc.mantissa || mr)) {
        mr = (~mr & BITS40) + 1;
        acc.mantissa = ((~acc.mantissa & BITS40) + (mr >> 40))
            | BIT41 | BIT42;
        mr &= BITS40;
    }

    normalize_and_round (acc, mr, mr != 0);
}

/*
 * Изменение знака числа на сумматоре ACC.
 * Результат помещается в регистр ACC, RMR гасится.
 */
void besm6_change_sign (int negate_acc)
{
    alureg_t acc;

    acc = toalu (ACC);
    if (negate_acc)
        negate (&acc);
    RMR = 0;
    normalize_and_round (acc, 0, 0);
}

/*
 * Изменение порядка числа на сумматоре ACC.
 * Результат помещается в регистр ACC, RMR гасится.
 */
void besm6_add_exponent (int val)
{
    alureg_t acc;

    acc = toalu (ACC);
    acc.exponent += val;
    RMR = 0;
    normalize_and_round (acc, 0, 0);
}

/*
 * Сборка значения по маске.
 */
t_value besm6_pack (t_value val, t_value mask)
{
    t_value result;

    result = 0;
    for (; mask; mask>>=1, val>>=1)
        if (mask & 1) {
            result >>= 1;
            if (val & 1)
                result |= BIT48;
        }
    return result;
}

/*
 * Разборка значения по маске.
 */
t_value besm6_unpack (t_value val, t_value mask)
{
    t_value result;
    int i;

    result = 0;
    for (i=0; i<48; ++i) {
        result <<= 1;
        if (mask & BIT48) {
            if (val & BIT48)
                result |= 1;
            val <<= 1;
        }
        mask <<= 1;
    }
    return result;
}

/*
 * Подсчёт количества единиц в слове.
 */
int besm6_count_ones (t_value word)
{
    int c;

    for (c=0; word; ++c)
        word &= word-1;
    return c;
}

/*
 * Сдвиг сумматора ACC с выдвижением в регистр младших разрядов RMR.
 * Величина сдвига находится в диапазоне -64..63.
 */
void besm6_shift (int i)
{
    RMR = 0;
    if (i > 0) {
        /* Сдвиг вправо. */
        if (i < 48) {
            RMR = (ACC << (48-i)) & BITS48;
            ACC >>= i;
        } else {
            RMR = ACC >> (i-48);
            ACC = 0;
        }
    } else if (i < 0) {
        /* Сдвиг влево. */
        i = -i;
        if (i < 48) {
            RMR = ACC >> (48-i);
            ACC = (ACC << i) & BITS48;
        } else {
            RMR = (ACC << (i-48)) & BITS48;
            ACC = 0;
        }
    }
}
