/*
 * besm6_printer.c: BESM-6 line printer device
 *
 * Copyright (c) 2009, Leonid Broukhis
 *
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

t_stat printer_event (UNIT *u);
void offset_gost_write (int num, FILE *fout);

/*
 * Printer data structures
 *
 * printer_dev  PRINTER device descriptor
 * printer_unit PRINTER unit descriptor
 * printer_reg  PRINTER register list
 */
UNIT printer_unit [] = {
    { UDATA (printer_event, UNIT_ATTABLE+UNIT_SEQ, 0) },
    { UDATA (printer_event, UNIT_ATTABLE+UNIT_SEQ, 0) },
};


#define MAX_STRIKES 10
struct acpu_t {
    int curchar, feed, rampup;
    int strikes;
    int length;
    unsigned char line[128][MAX_STRIKES];
} acpu[2];

#define PRN1_NOT_READY  (1<<19)
#define PRN2_NOT_READY  (1<<18)

/* 1 = можно пользоваться молоточками, 0 - бумага в процессе протяжки */
#define PRN1_LINEFEED   (1<<23)
#define PRN2_LINEFEED   (1<<22)

#define SLOW_START      100*MSEC
#define FAST_START      1*MSEC
#define LINEFEED_SYNC   1       /* Чтобы быстрее печатало; в жизни 20-25 мс/1.4 мс ~= 17 */

static uint32 es_drp, es_drm;   /* ДРП и маска ДРМ АЦПУ ЕС */
static uint32 es_drm_used;      /* ДРМ записывалась: ГРП 18 не выставлять */

REG printer_reg[] = {
    { REGDATA ( "Готов",  READY, 2,  2, 18, 1, NULL, NULL, 0, 0, 0) },
    { REGDATA ( "Прогон", READY, 2,  2, 22, 1, NULL, NULL, 0, 0, 0) },
    { ORDATA  ( ДРП, es_drp, 4) },
    { ORDATA  ( ДРМ, es_drm, 4) },
    { FLDATA  ( ДРМ_БЫЛА, es_drm_used, 0) },
    { 0 }
};

/*
 * АЦПУ ЕС-7032/7033, подключённое напрямую (разделы А0ЕС/А1ЕС ВЫДИНСа):
 * set prn0 es, set prn1 es. Обмен идёт по одному КУС через 033 070/071
 * и 033 4070/4071, окончание операции - ГРП 18, общий для обоих АЦПУ
 * (см. doc/ОСА.md, §4). Драйвер ДД (ОС Дубна, модуль PRINTL) ждёт
 * окончания по ГРП 11: ДРП (033 4027, разряд 4 - АЦПУ 0, 3 - АЦПУ 1)
 * под маской ДРМ (033 026).
 */
#define UNIT_V_ES       (UNIT_V_UF + 0)
#define UNIT_ES         (1 << UNIT_V_ES)

#define ES_KUS_BYTE     0377
#define ES_KUS_ACK      01000   /* 10 р.: информация из АЦПУ принята */
#define ES_KUS_CMD      02000   /* 11 р.: команда */
#define ES_KUS_DATA     04000   /* 12 р.: символ печати */
#define ES_KUS_RESET    010000  /* 13 р.: сброс УУ АЦПУ */

#define ES_ST_NOT_READY 01000   /* 10 р.: АЦПУ выключено / не готово */
#define ES_ST_VALID     02000   /* 11 р.: принят байт состояния, до квитанции */
#define ES_ST_DATA_REQ  04000   /* 12 р.: требуются данные для печати */
#define ES_DRP(num)     (010 >> (num))  /* разряд ДРП: окончание операции */
#define ES_ST_DONE      014     /* 3 и 4 рр. БС: АЦПУ закончило, конец передачи */

#define ES_LINE         128     /* символов в строке */
#define ES_LINE_MAX     160     /* ДД: 7 пробелов поля + 128, конец - командой */
#define ES_DELAY        (1*MSEC)

struct es_prn_t {
    int writing;                /* идёт приём строки по команде "писать" */
    int skip;                   /* пропуск после печати (байт команды >> 3) */
    int length;
    int overprint;              /* строка выдана без пропуска */
    int status;                 /* байт состояния */
    int pending;                /* байт состояния выдан, квитанции не было */
    unsigned char line[ES_LINE_MAX];
} es_prn[2];

MTAB printer_mod[] = {
    { UNIT_ES, 0,       "ACPU128", "ACPU128", NULL, NULL, NULL,
      "АЦПУ-128 (барабанное)" },
    { UNIT_ES, UNIT_ES, "ES",      "ES",      NULL, NULL, NULL,
      "АЦПУ ЕС-7032/7033 (А0ЕС)" },
    { 0 }
};

t_stat printer_reset (DEVICE *dptr);
t_stat printer_attach (UNIT *uptr, CONST char *cptr);
t_stat printer_detach (UNIT *uptr);

DEVICE printer_dev = {
    "PRN", printer_unit, printer_reg, printer_mod,
    2, 8, 19, 1, 8, 50,
    NULL, NULL, &printer_reset, NULL, &printer_attach, &printer_detach,
    NULL, DEV_DISABLE | DEV_DEBUG
};

/*
 * Reset routine
 */
t_stat printer_reset (DEVICE *dptr)
{
    memset(acpu, 0, sizeof (acpu));
    memset(es_prn, 0, sizeof (es_prn));
    es_drp = es_drm = es_drm_used = 0;
    acpu[0].rampup = acpu[1].rampup = SLOW_START;
    sim_cancel (&printer_unit[0]);
    sim_cancel (&printer_unit[1]);
    READY |= PRN1_NOT_READY | PRN2_NOT_READY;
    if (printer_unit[0].flags & UNIT_ATT)
        READY &= ~PRN1_NOT_READY;
    if (printer_unit[1].flags & UNIT_ATT)
        READY &= ~PRN2_NOT_READY;
    return SCPE_OK;
}

t_stat printer_attach (UNIT *u, CONST char *cptr)
{
    t_stat s;
    int num = u - printer_unit;

    if (u->flags & UNIT_ATT) {
        /* Switching files cleanly */
        detach_unit (u);
    }
    s = attach_unit (u, cptr);
    if (s != SCPE_OK)
        return s;

    READY &= ~(PRN1_NOT_READY >> num);
    return SCPE_OK;
}

t_stat printer_detach (UNIT *u)
{
    int num = u - printer_unit;
    READY |= PRN1_NOT_READY >> num;
    return detach_unit (u);
}

/*
 * Управление двигателями, прогон
 */
void printer_control (int num, uint32 cmd)
{
    UNIT *u = &printer_unit[num];
    struct acpu_t * dev = acpu + num;

    if (printer_dev.dctrl)
        besm6_debug(">>> printer%d command %o", num, cmd);
    if (READY & (PRN1_NOT_READY >> num)) {
        if (printer_dev.dctrl)
            besm6_debug(">>> printer%d not ready", num);
        return;
    }
    if (u->flags & UNIT_ES) {
        /* У АЦПУ ЕС нет двигателя и синхроимпульсов АЦПУ-128 */
        fflush (u->fileref);
        return;
    }
    switch (cmd) {
    case 1:         /* linefeed */
        READY &= ~(PRN1_LINEFEED >> num);
        offset_gost_write (num, u->fileref);
        u->pos = ftell(u->fileref);
        dev->feed = LINEFEED_SYNC;
        break;
    case 4:         /* start */
        /* стартуем из состояния прогона для надежности */
        dev->feed = LINEFEED_SYNC;
        READY &= ~(PRN1_LINEFEED >> num);
        if (dev->rampup)
            sim_activate (u, dev->rampup);
        dev->rampup = 0;
        break;
    case 10:        /* motor and ribbon off */
    case 8:         /* motor off? (undocumented) */
    case 2:         /* ribbon off */
        dev->rampup = cmd == 2 ? FAST_START : SLOW_START;
        sim_cancel (u);
        fflush (u->fileref);
        break;
    }
}

/*
 * Управление молоточками
 */
void printer_hammer (int num, int pos, uint32 mask)
{
    struct acpu_t * dev = acpu + num;
    while (mask) {
        if (mask & 1) {
            int strike = 0;
            while (dev->line[pos][strike] && strike < MAX_STRIKES)
                ++strike;
            if (strike < MAX_STRIKES) {
                dev->line[pos][strike] = dev->curchar;
                if (pos + 1 > dev->length)
                    dev->length = pos + 1;
                if (strike + 1 > dev->strikes)
                    dev->strikes = strike + 1;
            }
        }
        mask >>= 1;
        pos += 8;
    }
}

/*
 * Событие: вращение барабана АЦПУ.
 * Устанавливаем флаг прерывания.
 */
t_stat printer_event (UNIT *u)
{
    int num = u - printer_unit;
    struct acpu_t * dev = acpu + num;

    if (u->flags & UNIT_ES) {
        /* АЦПУ ЕС закончило операцию: байт состояния и ТРБ-А */
        es_prn[num].status = ES_ST_DONE;
        es_prn[num].pending = 1;
        if (! es_drm_used)
            GRP |= GRP_ES_PRN;
        es_drp |= ES_DRP (num);
        if (es_drp & es_drm)
            GRP |= GRP_ES_SLOW;
        if (printer_dev.dctrl)
            besm6_debug (">>> ES printer%d: done, ДРП %o, ДРМ %o", num, es_drp, es_drm);
        return SCPE_OK;
    }
    if (dev->curchar < 0140) {
        GRP |= GRP_PRN1_SYNC >> num;
        ++dev->curchar;
        /* For next char */
        sim_activate (u, 1400*USEC);
        if (dev->feed && --dev->feed == 0) {
            READY |= PRN1_LINEFEED >> num;
        }
    } else {
        /* For "zero" */
        dev->curchar = 0;
        GRP |= GRP_PRN1_ZERO >> num;
        if (printer_dev.dctrl)
            besm6_debug(">>> printer%d 'zero'", num);
        /* For first sync after "zero" */
        sim_activate (u, 1000*USEC);
    }
    return SCPE_OK;
}

int gost_latin = 0; /* default cyrillics */

/*
 * GOST-10859 encoding.
 * Documentation: http://en.wikipedia.org/wiki/GOST_10859
 */
static const unsigned short gost_to_unicode_cyr [256] = {
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

static const unsigned short gost_to_unicode_lat [256] = {
    /* 000-007 */   0x30,   0x31,   0x32,   0x33,   0x34,   0x35,   0x36,   0x37,
    /* 010-017 */   0x38,   0x39,   0x2b,   0x2d,   0x2f,   0x2c,   0x2e,   0x2423,
    /* 020-027 */   0x23e8, 0x2191, 0x28,   0x29,   0xd7,   0x3d,   0x3b,   0x5b,
    /* 030-037 */   0x5d,   0x2a,   0x2018, 0x2019, 0x2260, 0x3c,   0x3e,   0x3a,
    /* 040-047 */   0x41,   0x0411, 0x42,   0x0413, 0x0414, 0x45,   0x0416, 0x0417,
    /* 050-057 */   0x0418, 0x0419, 0x4b,   0x041b, 0x4d,   0x48,   0x4f,   0x041f,
    /* 060-067 */   0x50,   0x43,   0x54,   0x59,   0x0424, 0x58,   0x0426, 0x0427,
    /* 070-077 */   0x0428, 0x0429, 0x042b, 0x042c, 0x042d, 0x042e, 0x042f, 0x44,
    /* 100-107 */   0x46,   0x47,   0x49,   0x4a,   0x4c,   0x4e,   0x51,   0x52,
    /* 110-117 */   0x53,   0x55,   0x56,   0x57,   0x5a,   0x203e, 0x2a7d, 0x2a7e,
    /* 120-127 */   0x2228, 0x2227, 0x2283, 0xac,   0xf7,   0x2261, 0x25,   0x25c7,
    /* 130-137 */   0x7c,   0x2015, 0x5f,   0x21,   0x22,   0x042a, 0xb0,   0x2032,
};
/*
 * Write Unicode symbol to file.
 * Convert to UTF-8 encoding:
 * 00000000.0xxxxxxx -> 0xxxxxxx
 * 00000xxx.xxyyyyyy -> 110xxxxx, 10yyyyyy
 * xxxxyyyy.yyzzzzzz -> 1110xxxx, 10yyyyyy, 10zzzzzz
 */
static void
utf8_putc (unsigned short ch, FILE *fout)
{
    if (ch < 0x80) {
        putc (ch, fout);
        return;
    }
    if (ch < 0x800) {
        putc (ch >> 6 | 0xc0, fout);
        putc ((ch & 0x3f) | 0x80, fout);
        return;
    }
    putc (ch >> 12 | 0xe0, fout);
    putc (((ch >> 6) & 0x3f) | 0x80, fout);
    putc ((ch & 0x3f) | 0x80, fout);
}

unsigned short
gost_to_unicode (unsigned char ch)
{
    return gost_latin ? gost_to_unicode_lat [ch] :
        gost_to_unicode_cyr [ch];
}

/*
 * Write GOST-10859 symbol to file.
 * Convert to local encoding (UTF-8, KOI8-R, CP-1251, CP-866).
 */
void
gost_putc (unsigned char ch, FILE *fout)
{
    unsigned short u;

    u = gost_to_unicode (ch);
    if (! u)
        u = ' ';
    utf8_putc (u, fout);
}

/*
 * Write GOST-10859 string with overprint to file in UTF-8.
 */
void
offset_gost_write (int num, FILE *fout)
{
    struct acpu_t * dev = acpu + num;
    int s, p;
    for (s = 0; s < dev->strikes; ++s) {
        if (s)
            fputc ('\r', fout);
        for (p = 0; p < dev->length; ++p) {
            gost_putc (dev->line[p][s] - 1, fout);
        }
    }

    fputc ('\n', fout);
    memset(dev->line, 0, sizeof (dev->line));
    dev->length = dev->strikes = 0;
}

/*
 * ДКОИ (набор АЦПУ ЕС) -> Unicode. Буквы, общие по начертанию для
 * латиницы и кириллицы, выдаются русскими, как и для АЦПУ-128.
 */
static unsigned short
dkoi_to_unicode (unsigned char ch)
{
    static const unsigned short letters [0x100 - 0xB8] = {
        /* B8 */ 0x042e, 0,      0x0411, 0x0426, 0x0414, 0,      0x0424, 0x0413,
        /* C0 */ 0,      0x0410, 0x0412, 0x0421, 'D',    0x0415, 'F',    'G',
        /* C8 */ 0x041d, 'I',    0,      0x0418, 0x0419, 0,      0x041b, 0,
        /* D0 */ 0,      'J',    0x041a, 'L',    0x041c, 'N',    0x041e, 0x0420,
        /* D8 */ 'Q',    'R',    0,      0,      0x041f, 0x042f, 0,      0,
        /* E0 */ 0,      0,      'S',    0x0422, 'U',    'V',    'W',    0x0425,
        /* E8 */ 'Y',    'Z',    0,      0x0423, 0x0416, 0,      0x042c, 0x042b,
        /* F0 */ '0',    '1',    '2',    '3',    '4',    '5',    '6',    '7',
        /* F8 */ '8',    '9',    0x0417, 0x0428, 0x042d, 0x0429, 0x0427, 0,
    };
    if (ch >= 0xB8)
        return letters [ch - 0xB8];
    switch (ch) {
    case 0x40: return ' ';
    case 0x4A: return '[';
    case 0x4B: return '.';
    case 0x4C: return '<';
    case 0x4D: return '(';
    case 0x4E: return '+';
    case 0x4F: return '!';
    case 0x50: return '&';
    case 0x5A: return ']';
    case 0x5B: return 0x00a4;   /* ¤ */
    case 0x5C: return '*';
    case 0x5D: return ')';
    case 0x5E: return ';';
    case 0x5F: return 0x00ac;   /* ¬ */
    case 0x60: return '-';
    case 0x61: return '/';
    case 0x6A: return '|';
    case 0x6B: return ',';
    case 0x6C: return '%';
    case 0x6D: return '_';
    case 0x6E: return '>';
    case 0x6F: return '?';
    case 0x7A: return ':';
    case 0x7B: return '#';
    case 0x7C: return '@';
    case 0x7D: return '\'';
    case 0x7E: return '=';
    case 0x7F: return '"';
    }
    return 0;
}

/*
 * Строка АЦПУ ЕС принята целиком: печатаем, выполняем пропуск.
 */
static void
es_prn_feed (struct es_prn_t *dev, int skip, FILE *fout)
{
    if (skip & 020) {
        /* прогон до отверстия на дорожке перфоленты */
        fputc ((skip & 017) == 1 ? '\f' : '\n', fout);
    } else {
        for (skip &= 3; skip > 0; --skip)
            fputc ('\n', fout);
    }
    dev->overprint = 0;
}

static void
es_prn_print (int num)
{
    UNIT *u = &printer_unit[num];
    struct es_prn_t *dev = es_prn + num;
    int p;

    while (dev->length > 0 && dev->line[dev->length - 1] == 0x40)
        --dev->length;
    if (dev->overprint)
        fputc ('\r', u->fileref);
    for (p = 0; p < dev->length; ++p) {
        unsigned short c = dkoi_to_unicode (dev->line[p]);
        utf8_putc (c ? c : ' ', u->fileref);
    }
    dev->overprint = 1;
    if (dev->skip)
        es_prn_feed (dev, dev->skip, u->fileref);
    u->pos = ftell (u->fileref);
    dev->writing = 0;
    dev->length = 0;
}

static void
es_prn_done (int num)
{
    es_prn[num].status = 0;
    sim_cancel (&printer_unit[num]);
    sim_activate (&printer_unit[num], ES_DELAY);
}

/*
 * Выдача КУС в АЦПУ ЕС (033 070/071).
 */
void es_prn_write (int num, uint32 kus)
{
    UNIT *u = &printer_unit[num];
    struct es_prn_t *dev = es_prn + num;
    int byte = kus & ES_KUS_BYTE;

    if (! (u->flags & UNIT_ES) || ! (u->flags & UNIT_ATT))
        return;
    if (printer_dev.dctrl && ! (kus & ES_KUS_DATA))
        besm6_debug (">>> ES printer%d: КУС %o", num, kus);
    if (kus & ES_KUS_RESET) {
        memset (dev, 0, sizeof (*dev));
        es_drp &= ~ES_DRP (num);
        sim_cancel (u);
        return;
    }
    if (kus & ES_KUS_DATA) {
        if (! dev->writing)
            return;
        if (dev->length < ES_LINE_MAX)
            dev->line[dev->length++] = byte;
        /* Диспак кончает строку 128-м символом, ДД - командой */
        if (! es_drm_used && dev->length == ES_LINE) {
            es_prn_print (num);
            es_prn_done (num);
        }
        return;
    }
    if (! (kus & ES_KUS_CMD)) {
        /* Квитанция (10 р.): байт состояния принят. ГРП 18 общий
         * для обоих АЦПУ ЕС: если второе ждёт, прерывание повторяется. */
        if (kus & ES_KUS_ACK) {
            dev->status = 0;
            dev->pending = 0;
            es_drp &= ~ES_DRP (num);
            if (es_prn[num ^ 1].status && ! es_drm_used)
                GRP |= GRP_ES_PRN;
        }
        return;
    }
    /* Команда принята: выдаётся байт состояния */
    dev->pending = 1;
    es_drp &= ~ES_DRP (num);
    if (dev->writing) {
        /* новая команда обрывает передачу данных и печатает строку
         * (так ДД кончает строку короче 128 символов) */
        es_prn_print (num);
        if (byte == 0) {
            es_prn_done (num);
            return;
        }
    }
    if (byte == 0)
        return;                 /* проверить ввод-вывод: выдать БС */
    switch (byte & 7) {
    case 1:                     /* писать, пропуск после печати */
        dev->writing = 1;
        dev->skip = byte >> 3;
        dev->length = 0;
        dev->status = 0;
        break;
    case 3:                     /* только пропуск или прогон */
        if ((byte >> 3) != 0 && (byte & 0300) != 0100 && byte != 0203) {
            es_prn_feed (dev, byte >> 3, u->fileref);
            u->pos = ftell (u->fileref);
        }
        es_prn_done (num);
        break;
    default:                    /* уточнить состояние и пр. */
        es_prn_done (num);
        break;
    }
}

/*
 * Приём из АЦПУ ЕС (033 4070/4071): байт состояния. После окончания
 * операции (вместе с ГРП 18) он равен 014 до квитанции (10 р. КУС),
 * затем 0. Пока АЦПУ принимает строку, стоит 12 р. - требуются данные.
 */
int es_prn_read (int num)
{
    UNIT *u = &printer_unit[num];

    if ((u->flags & UNIT_ES) && (u->flags & UNIT_ATT)) {
        struct es_prn_t *dev = es_prn + num;
        if (dev->pending)
            return ES_ST_VALID | dev->status;
        return dev->writing ? ES_ST_DATA_REQ : 0;
    }
    return ES_ST_NOT_READY;
}

/*
 * Запись маски ДРМ (033 026).
 */
void es_prn_mask (uint32 mask)
{
    es_drm = mask & (ES_DRP (0) | ES_DRP (1));
    if (es_drm)
        es_drm_used = 1;        /* драйвер ДД: у него ГРП 18 - останов */
    if (printer_dev.dctrl)
        besm6_debug (">>> ES printer: ДРМ %o, ДРП %o", es_drm, es_drp);
    if (es_drp & es_drm)
        GRP |= GRP_ES_SLOW;
}

/*
 * Чтение ДРП (033 4027).
 */
uint32 es_prn_drp (void)
{
    if (printer_dev.dctrl)
        besm6_debug (">>> ES printer: read ДРП %o", es_drp);
    return es_drp;
}
