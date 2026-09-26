/*
 * SVS card reader (ПК) and paper tape reader (ФС) on the ES channel.
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
 * Устройства ввода Диспака (модуль В1К), команды SIMH — как у БЭСМ-6:
 *
 *   VU  vu0 = ПК0 (ТУС 014), vu1 = ПК1 (ТУС 074) — считыватель перфокарт.
 *       attach VU0 колода.txt: текст UTF-8, как в besm6_vu.c: строка —
 *       перфокарта в строчном коде УПП («слойка»: 12 строк по 80 пробивок, до
 *       120 знаков); «картинка» из O и . (12 строк по 80); строка из шести `
 *       — карта «диспетчерский конец»; SET VUn UPDK=a-b — карты a..b в
 *       колонном коде УПДК.
 *   FS  fs0 = ФС0 (ТУС 010), fs1 = ФС1 (ТУС 070) — фотосчитыватель ленты.
 *       attach FS0 лента.bin: восьмидорожечные кадры как есть;
 *       attach -t FS0 лента.txt: текст UTF-8 -> УПП (ГОСТ 10859 с дополнением
 *       до нечётности), как в besm6_punch.c: сплошной текст без переводов
 *       строк; ctrl-] переключает на виртуальные перфокарты по 120 кадров
 *       (строка — карта) и обратно.
 *
 * Обмен — целиком на заявку: карта (КОП 22) или блок ленты (КОП 02).
 * Байты кладутся по 6 в 48-разрядное слово, первый — в разр.48-41.
 * Конец колоды или ленты — «не готово», как у неподключённого устройства.
 */
#define NUM_READERS     2

UNIT vu_unit[NUM_READERS] = {
    { UDATA(NULL, UNIT_ATTABLE + UNIT_SEQ + UNIT_RO, 0) },
    { UDATA(NULL, UNIT_ATTABLE + UNIT_SEQ + UNIT_RO, 0) },
};

UNIT fs_unit[NUM_READERS] = {
    { UDATA(NULL, UNIT_ATTABLE + UNIT_SEQ + UNIT_RO, 0) },
    { UDATA(NULL, UNIT_ATTABLE + UNIT_SEQ + UNIT_RO, 0) },
};

static REG vu_reg[] = { { 0 } };
static REG fs_reg[] = { { 0 } };
static unsigned int vu_updkstart[NUM_READERS], vu_updkend[NUM_READERS];

/*
 * SET VUn UPDK=начало-конец: карты с этими номерами — в колонном коде УПДК
 * (0-0 — выключить, без значения — со второй карты до конца), как в
 * besm6_vu.c.
 */
static t_stat vu_set_updk(UNIT *u, int32 val, CONST char *cptr, void *desc)
{
    unsigned start, end;
    int num = u - vu_unit;

    if (!cptr) {
        sim_printf("Range set to MAX\n");
        vu_updkstart[num] = 1;
        vu_updkend[num] = 0;
        return SCPE_OK;
    }
    if (sscanf(cptr, "%u-%u", &start, &end) != 2 ||
        (start == 0 && end != 0) || (end != 0 && end < start)) {
        sim_printf("Range required, e.g. 10-100, or 0-0 to disable.\n");
        return SCPE_ARG;
    }
    vu_updkstart[num] = start;
    vu_updkend[num] = end;
    return SCPE_OK;
}

static t_stat vu_show_updk(FILE *st, UNIT *u, int32 v, CONST void *dp)
{
    int num = u - vu_unit;

    if (vu_updkstart[num] == 0 && vu_updkend[num] == 0)
        fprintf(st, "UPDK disabled");
    else if (vu_updkend[num] == 0)
        fprintf(st, "UPDK card %d to EOF", vu_updkstart[num]);
    else
        fprintf(st, "UPDK cards %d-%d", vu_updkstart[num], vu_updkend[num]);
    return SCPE_OK;
}

static MTAB vu_mod[] = {
    { MTAB_XTD|MTAB_VUN, 0, "UPDK", "UPDK", &vu_set_updk, &vu_show_updk, NULL,
      "Use the columnwise UPDK code for a range of cards" },
    { 0 }
};
static MTAB fs_mod[] = { { 0 } };

#define DEB_OPS 000001
#define DEB_DAT 000040

static DEBTAB rd_deb[] = {
    { "OPS",  DEB_OPS, "commands" },
    { "DATA", DEB_DAT, "data read" },
    { NULL, 0 }
};

static int fs_textmode[NUM_READERS];    /* лента подключена с -t */

/*
 * Состояние чтения текстовой ленты — как fs_state в besm6_punch.c.
 * Текст идёт «сплошным» (переводы строк пропускаются); знак ctrl-] (GS)
 * переключает на «виртуальные перфокарты» по CARD_LEN кадров, где строка —
 * карта, дополняемая нулями, и обратно (карта ЕНДА3).
 */
#define CARD_LEN 120
enum {
    FS_STARTING,
    FS_BINARY,
    FS_RUNNING,
    FS_IMAGE,
    FS_IMAGE_LAST = FS_IMAGE + CARD_LEN - 1,
    FS_TOOLONG,
    FS_FILLUP,
    FS_FILLUP_LAST = FS_FILLUP + CARD_LEN - 1,
    FS_ENDA3,
    FS_ENDA3_LAST = FS_ENDA3 + CARD_LEN - 1,
    FS_TAIL,
};
static int fs_state[NUM_READERS];
static int vu_cardcnt[NUM_READERS];     /* номер карты в колоде */

static t_stat vu_attach(UNIT *u, CONST char *cptr);
static t_stat fs_attach(UNIT *u, CONST char *cptr);
static t_stat rd_detach(UNIT *u);

DEVICE vu_dev = {
    "VU", vu_unit, vu_reg, vu_mod,
    NUM_READERS, 8, 19, 1, 8, 50,
    NULL, NULL, NULL, NULL, &vu_attach, &rd_detach,
    NULL, DEV_DISABLE | DEV_DEBUG, 0, rd_deb
};

DEVICE fs_dev = {
    "FS", fs_unit, fs_reg, fs_mod,
    NUM_READERS, 8, 19, 1, 8, 50,
    NULL, NULL, NULL, NULL, &fs_attach, &rd_detach,
    NULL, DEV_DISABLE | DEV_DEBUG, 0, rd_deb
};

static t_stat vu_attach(UNIT *u, CONST char *cptr)
{
    if (u->flags & UNIT_ATT)
        detach_unit(u);
    vu_cardcnt[u - vu_unit] = 0;
    return attach_unit(u, cptr);
}

/*
 * С ключом -t подключается текст в UTF-8, иначе — кадры ленты как есть.
 */
static t_stat fs_attach(UNIT *u, CONST char *cptr)
{
    int num = u - fs_unit;

    if (u->flags & UNIT_ATT)
        detach_unit(u);
    fs_textmode[num] = (sim_switches & SWMASK('T')) != 0;
    sim_switches &= ~SWMASK('T');
    fs_state[num] = FS_STARTING;
    return attach_unit(u, cptr);
}

static t_stat rd_detach(UNIT *u)
{
    return detach_unit(u);
}

/*
 * Знак Unicode из файла в UTF-8 (как utf8_getc в besm6_punch.c).
 */
static int utf8_getc(FILE *fin)
{
    int c1, c2, c3;
again:
    c1 = getc(fin);
    if (c1 < 0 || !(c1 & 0x80))
        return c1;
    c2 = getc(fin);
    if (!(c1 & 0x20))
        return (c1 & 0x1f) << 6 | (c2 & 0x3f);
    c3 = getc(fin);
    if (c1 == 0xEF && c2 == 0xBB && c3 == 0xBF)
        goto again;                     /* неразрывный пробел нулевой ширины */
    return (c1 & 0x0f) << 12 | (c2 & 0x3f) << 6 | (c3 & 0x3f);
}

/*
 * Unicode -> ГОСТ 10859 (таблица unicode_to_gost из besm6_punch.c).
 */
static unsigned char unicode_to_gost(unsigned short val)
{
    static const unsigned char tab0[128] = {
        /* 00 - 07 */   017,    017,    017,    017,    017,    017,    017,    017,
        /* 08 - 0f */   017,    017,    0214,   017,    017,    017,    017,    017,
        /* 10 - 17 */   017,    017,    017,    017,    017,    017,    017,    017,
        /* 18 - 1f */   017,    017,    017,    017,    017,    017,    0174,   017,
        /*  !"#$%&' */  0017,   0133,   0134,   0034,   0127,   0126,   0121,   0033,
        /* ()*+,-./ */  0022,   0023,   0031,   0012,   0015,   0013,   0016,   0014,
        /* 01234567 */  0000,   0001,   0002,   0003,   0004,   0005,   0006,   0007,
        /* 89:;<=>? */  0010,   0011,   0037,   0026,   0035,   0025,   0036,   0136,
        /* @ABCDEFG */  0021,   0040,   0042,   0061,   0077,   0045,   0100,   0101,
        /* HIJKLMNO */  0055,   0102,   0103,   0052,   0104,   0054,   0105,   0056,
        /* PQRSTUVW */  0060,   0106,   0107,   0110,   0062,   0111,   0112,   0113,
        /* XYZ[\]^_ */  0065,   0063,   0114,   0027,   017,    0030,   0115,   0132,
        /* `abcdefg */  0032,   0040,   0042,   0061,   0077,   0045,   0100,   0101,
        /* hijklmno */  0055,   0102,   0103,   0052,   0104,   0054,   0105,   0056,
        /* pqrstuvw */  0060,   0106,   0107,   0110,   0062,   0111,   0112,   0113,
        /* xyz{|}~  */  0065,   0063,   0114,   017,    0130,   017,    0123,   017,
    };
    /* Кириллица А-Я (0410-042F) и а-я (0430-044F). */
    static const unsigned char cyr[32] = {
        0040, 0041, 0042, 0043, 0044, 0045, 0046, 0047,
        0050, 0051, 0052, 0053, 0054, 0055, 0056, 0057,
        0060, 0061, 0062, 0063, 0064, 0065, 0066, 0067,
        0070, 0071, 0135, 0072, 0073, 0074, 0075, 0076,
    };

    if (val < 0x80)
        return tab0[val];
    if (val >= 0x410 && val < 0x450)
        return cyr[(val - 0x410) & 037];
    switch (val) {
    case 0x00AC: return 0123;
    case 0x00B0: return 0136;
    case 0x00D7: return 0024;
    case 0x00F7: return 0124;
    case 0x2015: return 0131;
    case 0x2018: return 0032;
    case 0x2019: return 0033;
    case 0x2032: return 0137;
    case 0x203E: return 0115;
    case 0x212F: return 0020;
    case 0x2191: return 0021;
    case 0x2227: return 0121;
    case 0x2228: return 0120;
    case 0x2260: return 0034;
    case 0x2261: return 0125;
    case 0x2264: return 0116;
    case 0x2265: return 0117;
    case 0x2283: return 0122;
    case 0x23E8: return 0020;
    case 0x25C7: return 0127;
    case 0x25CA: return 0127;
    case 0x2A7D: return 0116;
    case 0x2A7E: return 0117;
    }
    return 017;
}

/*
 * УПП — ГОСТ 10859 с дополнением до нечётности (gost_to_upp в besm6_vu.c).
 */
static unsigned char gost_to_upp(unsigned char ch)
{
    unsigned char ret = ch;

    ch = (ch & 0x55) + ((ch >> 1) & 0x55);
    ch = (ch & 0x33) + ((ch >> 2) & 0x33);
    ch = (ch & 0x0F) + ((ch >> 4) & 0x0F);
    return (ch & 1) ? ret : ret | 0x80;
}

/*
 * Положить n байтов в память по 6 в слово, первый — в разр.48-41.
 * Не более nwords слов; хвост последнего слова — нули.
 */
static void put_bytes(int memaddr, int nwords, const unsigned char *buf, int n)
{
    int w, k;

    for (w = 0; w < nwords && 6 * w < n; ++w) {
        t_value v = 0;

        for (k = 0; k < 6; ++k)
            v = (v << 8) | ((6 * w + k < n) ? buf[6 * w + k] : 0);
        memory[mmu_iom_data_pa(memaddr + w)] = v << 16;
        tag[mmu_iom_data_pa(memaddr + w)] = TAG_NUMBER48;
    }
}

/*
 * Перфокарта — как в besm6_vu.c: vu_gost — до 120 знаков ГОСТ строки
 * (0377 — за концом строки), vu_image — 80 колонок по 12 пробивок, разряд i
 * колонки — строка i сверху (12, 11, 0, 1, ..., 9).
 */
#define GOST_DOT 016                    /* непробитая позиция */
#define GOST_O   056                    /* пробитая позиция */
#define DISP_END "\032\032\032\032\032\032\377"   /* карта «диспетчерский конец» */

static unsigned char vu_gost[NUM_READERS][120];
static unsigned short vu_image[NUM_READERS][80];

/*
 * Строка "0-9+-" -> 12 разрядов пробивок (punch в besm6_vu.c).
 */
static int punch(const char *s)
{
    int r = 0;

    while (*s) {
        r |= *s >= '0' ? 4 << (*s - '0') : *s == '+' ? 1 : *s == '-' ? 2 : 0;
        ++s;
    }
    return r;
}

/*
 * ГОСТ -> колонный код УПДК (gost_to_updk в besm6_vu.c).
 */
static unsigned short gost_to_updk(unsigned char ch)
{
    unsigned short ret;
    static const char *upper[4] = { "", "+0", "-0", "+-" };
    static const char *lower[2][16] = {
        { "0", "1", "2",   "3",   "4",   "5",   "6",   "7",
          "8", "9", "082", "083", "084", "085", "086", "087" },
        { "390", "391",   "392",   "39210", "394",   "395",   "396",   "397",
          "398", "39801", "39802", "39821", "39804", "39805", "39806", "39807" }
    };

    if (ch == 0377 /* заполнитель */ || ch == 017 /* пробел */)
        ret = 0;
    else
        ret = punch(upper[(ch >> 4) & 3]) ^ punch(lower[ch >= 0100][ch & 0xF]);
    return ret;
}

/*
 * Строчный образ карты: знак i — строка i/10, колонки 8*(i%10)..+7
 * (reverse_card в besm6_vu.c; raw — байты уже с чётностью).
 */
static void reverse_card(int num, int raw)
{
    int i, j;

    memset(vu_image[num], 0, sizeof(vu_image[num]));
    for (i = 0; i < 120; ++i) {
        unsigned char ch = vu_gost[num][i];
        int mask = 1 << (i / 10);
        int pos = 8 * (i % 10);

        if (!raw) {
            if (ch == 0377)
                break;
            ch = gost_to_upp(ch);
        }
        for (j = 7; j >= 0; --j) {
            if (ch & 1)
                vu_image[num][pos + j] |= mask;
            ch >>= 1;
        }
    }
}

/*
 * Первая строка — 80 знаков O и . (is_prettycard в besm6_vu.c).
 */
static int is_prettycard(const unsigned char *s)
{
    int i;

    for (i = 0; i < 80; ++i)
        if (s[i] != GOST_DOT && s[i] != GOST_O)
            return 0;
    for (i = 80; i < 120; ++i)
        if (s[i] != 0377)
            return 0;
    return 1;
}

static int chad(int num, int bit, int val)
{
    int index = bit / 8;

    switch (val) {
    case GOST_O:
        vu_gost[num][index] <<= 1;
        vu_gost[num][index] |= 1;
        return 0;
    case GOST_DOT:
        vu_gost[num][index] <<= 1;
        return 0;
    default:
        return -1;
    }
}

/*
 * «Картинка» карты: 12 строк по 80 знаков O и . (prettycard в besm6_vu.c).
 */
static int prettycard(UNIT *u)
{
    int bit, ch;
    int num = u - vu_unit;

    for (bit = 0; bit < 80; bit++) {
        /* первая строка уже проверена */
        chad(num, bit, vu_gost[num][bit]);
    }
    for (bit = 80; bit < 12*80; bit++) {
        ch = utf8_getc(u->fileref);
        if (ch == '\n' && bit % 80 == 0)
            ch = utf8_getc(u->fileref);
        ch = unicode_to_gost(ch);
        if (chad(num, bit, ch))
            return -1;
        if (bit % 80 == 79) {
            do
                ch = utf8_getc(u->fileref);
            while (ch == '\r');
            if (ch != '\n')
                return -1;
        }
    }
    /* после карты может быть пустая строка */
    ch = getc(u->fileref);
    if (ch != '\n')
        ungetc(ch, u->fileref);
    return 0;
}

/*
 * Следующая карта колоды — начало vu_event (VU_STARTING) из besm6_vu.c.
 * Возвращает 0 или -1 в конце колоды.
 */
static int vu_next_card(int num)
{
    UNIT *u = &vu_unit[num];
    int ch, endline = 0, i;

    do
        ch = utf8_getc(u->fileref);
    while (ch == '\r');
    if (ch == EOF)
        return -1;

    ++vu_cardcnt[num];
    for (i = 0; i < 120; ++i) {
        if (endline) {
            vu_gost[num][i] = 0377;
        } else {
            int gost;

            if (ch == EOF || ch == '\n') {
                endline = 1;
                gost = 0377;
            } else {
                gost = unicode_to_gost(ch);
            }
            vu_gost[num][i] = gost;
            if (!endline && i != 119)
                do
                    ch = utf8_getc(u->fileref);
                while (ch == '\r');
        }
    }
    if (!endline) {
        /* Строка длиннее 120 знаков: остаток до конца строки отбрасывается. */
        do
            ch = utf8_getc(u->fileref);
        while (ch != '\n' && ch != EOF);
    }
    if (0 == memcmp(vu_gost[num], DISP_END, 7)) {
        /* Карта «диспетчерский конец»: конец ввода образов карт. */
        memset(vu_image[num], 0, sizeof(vu_image[num]));
        vu_image[num][0] = vu_image[num][40] = 0xFFF;
    } else if (is_prettycard(vu_gost[num])) {
        if (prettycard(u) < 0)
            sim_printf("VU-%d: A badly formatted card image at card %d, garbage will follow\n",
                num, vu_cardcnt[num]);
        reverse_card(num, 1);           /* как есть */
    } else if (vu_updkstart[num] != 0 && vu_cardcnt[num] >= vu_updkstart[num] &&
               (vu_updkend[num] == 0 || vu_cardcnt[num] <= vu_updkend[num])) {
        for (i = 0; i < 80; ++i)
            vu_image[num][i] = gost_to_updk(vu_gost[num][i]);
    } else {
        reverse_card(num, 0);           /* с чётностью */
    }
    return 0;
}

/*
 * Чтение карты (КОП 22): образ по колонкам, два байта на колонку, по 6
 * пробивок в младших разрядах байта — 160 байтов, 27 слов. Диспак разбирает
 * слово как три 16-разрядных поля (колонки) и собирает колонку командой СБР
 * по маске 037477: (байт0 & 077) << 6 | (байт1 & 077). Порядок — как в
 * двоичном образе карты ЕС (IBM column binary): байт0 — строки 12, 11, 0-3,
 * байт1 — строки 4-9, верхняя строка в старшем разряде байта. Строка k карты
 * здесь — строка k сверху (в «слойке» В1К она несёт знаки 10k..10k+9).
 */
t_stat svs_card_io(int num, int kop, int memaddr, int nwords)
{
    UNIT *u;
    unsigned char buf[160];
    int i;

    if (num < 0 || num >= NUM_READERS)
        return SCPE_NXUN;
    u = &vu_unit[num];
    sim_debug(DEB_OPS, &vu_dev, "ПК%d: КОП=%02X слов=%d адрес=%o\n",
        num, kop, nwords, memaddr);
    if (!(u->flags & UNIT_ATT))
        return SCPE_UNATT;
    if (vu_next_card(num) < 0) {
        sim_debug(DEB_OPS, &vu_dev, "ПК%d: колода кончилась\n", num);
        detach_unit(u);
        return SCPE_UNATT;
    }
    for (i = 0; i < 80; ++i) {
        int col = 0, k;

        for (k = 0; k < 12; ++k)                /* строка 0 — в старший разряд */
            if (vu_image[num][i] & (1 << k))
                col |= 1 << (11 - k);
        buf[2*i]     = (col >> 6) & 077;
        buf[2*i + 1] = col & 077;
    }
    put_bytes(memaddr, nwords, buf, sizeof(buf));
    sim_debug(DEB_DAT, &vu_dev, "ПК%d: карта %d прочитана\n", num, vu_cardcnt[num]);
    return SCPE_OK;
}

/*
 * Следующий кадр ленты — fs_event из besm6_punch.c без изменений.
 */
static int fs_next_frame(int num)
{
    FILE *f = fs_unit[num].fileref;
    int ch, frame = 0;

again:
    if (fs_state[num] == FS_STARTING) {
        /* Первый кадр после пуска двигателя — пустой, из файла не читается. */
        frame = 0;
        fs_state[num] = fs_textmode[num] ? FS_RUNNING : FS_BINARY;
    } else if (fs_state[num] == FS_BINARY) {
        ch = getc(f);
        if (ch < 0) {
            frame = 0;
            fs_state[num] = FS_TAIL;
        } else {
            frame = ch;
        }
    } else if (fs_state[num] == FS_RUNNING) {
        /* В сплошном тексте переводы строк пропускаются. */
        do
            ch = utf8_getc(f);
        while (ch == '\n' || ch == '\r');
        if (ch < 0) {
            /* хвост ленты без пробивок */
            frame = 0;
            fs_state[num] = FS_TAIL;
        } else if (ch == (']' & 037)) {
            /* ctrl-] (GS): переход к виртуальным перфокартам и обратно. */
            fs_state[num] = FS_IMAGE;
            goto again;
        } else {
            frame = gost_to_upp(unicode_to_gost(ch));
        }
    } else if (fs_state[num] >= FS_IMAGE && fs_state[num] <= FS_IMAGE_LAST) {
        ch = utf8_getc(f);
        if (ch < 0) {
            /* лента кончилась посреди карты */
            frame = 0;
            fs_state[num] = FS_TAIL;
        } else if (ch == '\r') {
            goto again;
        } else if (ch == '\n') {
            /* Остаток виртуальной карты — нули. */
            fs_state[num] = FS_FILLUP + (fs_state[num] - FS_IMAGE);
            goto again;
        } else if (ch == (']' & 037)) {
            if (fs_state[num] != FS_IMAGE)
                sim_printf("ФС%d: ЕНДА3 посреди карты\n", num);
            fs_state[num] = FS_ENDA3;
            goto again;
        } else {
            frame = gost_to_upp(unicode_to_gost(ch));
            /* Строка длиннее CARD_LEN продолжается на следующей карте. */
            if (++fs_state[num] == FS_TOOLONG)
                fs_state[num] = FS_IMAGE;
        }
    } else if (fs_state[num] >= FS_FILLUP && fs_state[num] <= FS_FILLUP_LAST) {
        frame = 0;
        if (++fs_state[num] == FS_ENDA3)
            fs_state[num] = FS_IMAGE;
    } else if (fs_state[num] >= FS_ENDA3 && fs_state[num] <= FS_ENDA3_LAST) {
        /* Карта ЕНДА3: 0200 в каждом пятом кадре; затем снова сплошной текст. */
        frame = ((fs_state[num] - FS_ENDA3) % 5 == 0) ? 0200 : 0;
        if (++fs_state[num] == FS_TAIL)
            fs_state[num] = FS_RUNNING;
    }
    return frame;                       /* в FS_TAIL — нуль */
}

/*
 * Чтение ленты (КОП 02): 6*nwords кадров подряд, кадр за кадром, как их
 * отдаёт fs_event на БЭСМ-6. Протяжка в хвосте ленты (FS_TAIL) снимает
 * ленту, как fs_control (команда 5) на БЭСМ-6: устройство «не готово».
 */
t_stat svs_tape_io(int num, int kop, int memaddr, int nwords)
{
    UNIT *u;
    unsigned char buf[6 * 0400];
    int n, max;

    if (num < 0 || num >= NUM_READERS)
        return SCPE_NXUN;
    u = &fs_unit[num];
    sim_debug(DEB_OPS, &fs_dev, "ФС%d: КОП=%02X слов=%d адрес=%o\n",
        num, kop, nwords, memaddr);
    if (!(u->flags & UNIT_ATT))
        return SCPE_UNATT;
    if (fs_state[num] == FS_TAIL) {
        sim_debug(DEB_OPS, &fs_dev, "ФС%d: лента кончилась\n", num);
        detach_unit(u);
        return SCPE_UNATT;
    }

    max = 6 * nwords;
    if (max > (int) sizeof(buf))
        max = sizeof(buf);
    for (n = 0; n < max; ++n)
        buf[n] = fs_next_frame(num);
    sim_debug(DEB_DAT, &fs_dev, "ФС%d: прочитано %d кадров\n", num, n);
    put_bytes(memaddr, nwords, buf, max);
    return SCPE_OK;
}
