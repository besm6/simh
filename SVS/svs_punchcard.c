/*
 * SVS card punch (ПИ) on the ES channel.
 *
 * Copyright (c) 2026, Leonid Broukhis
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * LEONID BROUKHIS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF
 * OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */
#include "svs_defs.h"

/*
 * Перфоратор карт, как в besm6_punchcard.c: pi0 = ПИ0 (ТУС 015),
 * pi1 = ПИ1 (ТУС 075). Диспак (ЕСПИ80.bemsh) выводит карту командой ЕС-канала
 * КОП 61: 160 байтов — образ по колонкам, два байта на колонку по 6 пробивок
 * в младших разрядах (строки 12, 11, 0-3 и 4-9, верхняя строка в старшем
 * разряде), как читает svs_cards.c.
 *
 * Режим вывода задаётся ключом attach, как на БЭСМ-6:
 *   -b  120 байтов на карту, построчно;
 *   -v  три строки по 40 знаков Брайля (по умолчанию);
 *   -d  12 строк по 80 знаков O и .;
 *   -g, -u  текст ГОСТ/УПП, если карта им является, иначе как -v.
 */
#define NUM_PUNCHES 2

UNIT pi_unit[NUM_PUNCHES] = {
    { UDATA(NULL, UNIT_ATTABLE + UNIT_SEQ, 0) },
    { UDATA(NULL, UNIT_ATTABLE + UNIT_SEQ, 0) },
};

static REG pi_reg[] = { { 0 } };
static MTAB pi_mod[] = { { 0 } };

#define DEB_OPS 000001

static DEBTAB pi_deb[] = {
    { "OPS", DEB_OPS, "commands" },
    { NULL, 0 }
};

/* Образ карты: 12 строк по 80 колонок, колонка 0 — первая. */
typedef unsigned char PI_CARD[12][80];

typedef void (*punch_fn_t)(int unit, PI_CARD card);
static punch_fn_t pi_punch_fn[NUM_PUNCHES];

static t_stat pi_attach(UNIT *u, CONST char *cptr);
static t_stat pi_detach(UNIT *u);

DEVICE pi_dev = {
    "PI", pi_unit, pi_reg, pi_mod,
    NUM_PUNCHES, 8, 19, 1, 8, 50,
    NULL, NULL, NULL, NULL, &pi_attach, &pi_detach,
    NULL, DEV_DISABLE | DEV_DEBUG, 0, pi_deb
};

/*
 * ГОСТ 10859 -> Unicode (gost_to_unicode_cyr в besm6_printer.c).
 */
static const unsigned short gost_to_unicode[0140] = {
    /* 000-007 */   0x30,   0x31,   0x32,   0x33,   0x34,   0x35,   0x36,   0x37,
    /* 010-017 */   0x38,   0x39,   0x2b,   0x2d,   0x2f,   0x2c,   0x2e,   0x2423,
    /* 020-027 */   0x23e8, 0x2191, 0x28,   0x29,   0xd7,   0x3d,   0x3b,   0x5b,
    /* 030-037 */   0x5d,   0x2a,   0x2018, 0x2019, 0x2260, 0x3c,   0x3e,   0x3a,
    /* 040-047 */   0x0410, 0x0411, 0x0412, 0x0413, 0x0414, 0x0415, 0x0416, 0x0417,
    /* 050-057 */   0x0418, 0x0419, 0x041a, 0x041b, 0x041c, 0x041d, 0x041e, 0x041f,
    /* 060-067 */   0x0420, 0x0421, 0x0422, 0x0423, 0x0424, 0x0425, 0x0426, 0x0427,
    /* 070-077 */   0x0428, 0x0429, 0x042b, 0x042c, 0x042d, 0x042e, 0x042f, 0x44,
    /* 100-107 */   0x46,   0x47,   0x49,   0x4a,   0x4c,   0x4e,   0x51,   0x52,
    /* 110-117 */   0x53,   0x55,   0x56,   0x57,   0x5a,   0x203e, 0x2a7d, 0x2a7e,
    /* 120-127 */   0x2228, 0x2227, 0x2283, 0xac,   0xf7,   0x2261, 0x25,   0x25c7,
    /* 130-137 */   0x7c,   0x2015, 0x5f,   0x21,   0x22,   0x042a, 0xb0,   0x2032,
};

void gost_putc(unsigned char ch, FILE *f)
{
    unsigned short u = ch < 0140 ? gost_to_unicode[ch] : 0;

    utf8_putc(u ? u : ' ', f);
}

/* 12 строк по 80 знаков O и . (pi_punch_dots). */
static void pi_punch_dots(int unit, PI_CARD card)
{
    FILE *f = pi_unit[unit].fileref;
    int l, c;

    for (l = 0; l < 12; ++l) {
        for (c = 0; c < 80; ++c)
            putc(card[l][c] ? 'O' : '.', f);
        putc('\n', f);
    }
    putc('\n', f);
}

/* Построчно 120 байтов (pi_to_bytes). */
static void pi_to_bytes(PI_CARD card, unsigned char buf[120])
{
    int l, c, n = 0;

    memset(buf, 0, 120);
    for (l = 0; l < 12; ++l)
        for (c = 0; c < 80; ++c, ++n)
            buf[n / 8] = (buf[n / 8] << 1) | card[l][c];
}

static void pi_punch_binary(int unit, PI_CARD card)
{
    unsigned char buf[120];

    pi_to_bytes(card, buf);
    fwrite(buf, 120, 1, pi_unit[unit].fileref);
}

/* Три строки по 40 знаков Брайля и пустая строка (pi_punch_visual). */
static void pi_punch_visual(int unit, PI_CARD card)
{
    FILE *f = pi_unit[unit].fileref;
    unsigned char bytes[3][40];
    int line, col;

    memset(bytes, 0, sizeof(bytes));
    for (line = 0; line < 12; ++line)
        for (col = 0; col < 80; ++col)
            if (card[line][col])
                /*
                 * Знак Брайля — U+2800 плюс 8-разрядная маска точек:
                 *   0 3
                 *   1 4
                 *   2 5
                 *   6 7
                 */
                bytes[line/4][col/2] |=
                    "\x01\x08\x02\x10\x04\x20\x40\x80"[line%4*2 + col%2];
    for (line = 0; line < 3; ++line) {
        for (col = 0; col < 40; ++col)
            fprintf(f, "\342%c%c", 0240 + (bytes[line][col] >> 6),
                0200 + (bytes[line][col] & 077));
        putc('\n', f);
    }
    putc('\n', f);
}

/*
 * Текст ГОСТ с дополнением до нечётности; если карта им не является —
 * знаками Брайля (pi_punch_gost).
 */
static void pi_punch_gost(int unit, PI_CARD card)
{
    FILE *f = pi_unit[unit].fileref;
    unsigned char buf[120];
    int len, cur, zero_expected = 0;

    pi_to_bytes(card, buf);
    /*
     * Байты — с нечётной чётностью, кроме необязательных нулей в конце строк
     * карты и в конце карты. Хвостовые нули отбрасываются, промежуточные
     * становятся пробелами. Первый знак каждой строки обязан быть знаком.
     */
    for (len = 120; len && !buf[len-1]; --len)
        continue;
    for (cur = 0; cur < len; ++cur) {
        if (cur % 10 == 0)
            zero_expected = 0;      /* новая строка карты */
        if (zero_expected) {
            if (buf[cur])
                break;
        } else if (!buf[cur]) {
            if (cur % 10 == 0)
                break;              /* первый знак строки нулевой */
            zero_expected = 1;
        } else if (!odd_parity(buf[cur]) || (buf[cur] & 0177) >= 0140) {
            break;
        }
    }
    if (cur != len) {
        pi_punch_visual(unit, card);
        return;
    }
    for (cur = 0; cur < len; ++cur) {
        if (buf[cur])
            gost_putc(buf[cur] & 0177, f);
        else
            putc(' ', f);
    }
    putc('\n', f);
}

/*
 * Ключи режима, как в pi_attach (besm6_punchcard.c). По умолчанию -v.
 */
static t_stat pi_attach(UNIT *u, CONST char *cptr)
{
    int unit = u - pi_unit;

    pi_punch_fn[unit] = NULL;
    while (sim_switches &
           (SWMASK('B')|SWMASK('V')|SWMASK('D')|SWMASK('G')|SWMASK('U'))) {
        if (pi_punch_fn[unit])
            return SCPE_ARG;
        if (sim_switches & SWMASK('B')) {
            pi_punch_fn[unit] = pi_punch_binary;
            sim_switches &= ~SWMASK('B');
        } else if (sim_switches & SWMASK('V')) {
            pi_punch_fn[unit] = pi_punch_visual;
            sim_switches &= ~SWMASK('V');
        } else if (sim_switches & SWMASK('D')) {
            pi_punch_fn[unit] = pi_punch_dots;
            sim_switches &= ~SWMASK('D');
        } else if (sim_switches & SWMASK('G')) {
            pi_punch_fn[unit] = pi_punch_gost;
            sim_switches &= ~SWMASK('G');
        } else {
            pi_punch_fn[unit] = pi_punch_gost;
            sim_switches &= ~SWMASK('U');
        }
    }
    if (pi_punch_fn[unit] == NULL)
        pi_punch_fn[unit] = pi_punch_visual;
    if (u->flags & UNIT_ATT)
        detach_unit(u);
    return attach_unit(u, cptr);
}

static t_stat pi_detach(UNIT *u)
{
    return detach_unit(u);
}

/*
 * Вывод карты (КОП 61): nbytes байтов образа по колонкам с адреса memaddr,
 * по 6 в 48-разрядном слове, первый — в разр.48-41.
 * Возвращает SCPE_UNATT, если файл не подключён (перфоратор не готов).
 */
t_stat svs_punch_io(int num, int kop, int memaddr, int nbytes)
{
    UNIT *u;
    unsigned char buf[160];
    PI_CARD card;
    int i, k;

    if (num < 0 || num >= NUM_PUNCHES)
        return SCPE_NXUN;
    u = &pi_unit[num];
    sim_debug(DEB_OPS, &pi_dev, "ПИ%d: КОП=%02X байтов=%d адрес=%o\n",
        num, kop, nbytes, memaddr);
    if (!(u->flags & UNIT_ATT))
        return SCPE_UNATT;

    memset(buf, 0, sizeof(buf));
    for (i = 0; i < nbytes && i < (int) sizeof(buf); ++i) {
        t_value w = (memory[mmu_iom_data_pa(memaddr + i / 6)] >> 16) & BITS48;

        buf[i] = (w >> (8 * (5 - i % 6))) & 0xFF;
    }
    /* Колонка: (байт0 & 077) << 6 | (байт1 & 077), строка 12 — старший разряд. */
    for (i = 0; i < 80; ++i) {
        int col = (buf[2*i] & 077) << 6 | (buf[2*i + 1] & 077);

        for (k = 0; k < 12; ++k)
            card[k][i] = (col >> (11 - k)) & 1;
    }
    (*pi_punch_fn[num])(num, card);
    fflush(u->fileref);
    return SCPE_OK;
}
