/*
 * SVS line printer (АЦПУ ЕС-7033/7036) on the ES channel.
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
 * Два АЦПУ: юнит 0 — АЦПУ0 (ТУС 017, канал Х'4'), юнит 1 — АЦПУ1
 * (ТУС 077, канал Х'5'). Печать идёт в подключённый файл в UTF-8.
 *
 * Диспак печатает через ПЕЧАТЬ.bemsh -> ЕС (ПВВ.bemsh): КОП команды ЕС-канала
 * лежит в разр.31-38 слова СПУ, массив — в ДО (НАМ/РАЗМ, РАЗМ в словах),
 * по 6 байт ДКОИ в 48-разрядном слове, первый байт — в разр.48-41.
 *
 * Коды операций (ПЕЧАТЬ.bemsh, КОППЕЧ и др.) — команды АЦПУ ЕС:
 *   xxxxx001  печать; разр.4-5 — число протяжек ПОСЛЕ печати (0-3),
 *             разр.8 — прогон до канала 1 после печати;
 *   xxxxx011  управление без печати: протяжка на 1-3 строки или прогон
 *             (8В — прогон на начало листа);
 *   01100011  (63) загрузка БУФП, 11111011 (FB) загрузка БУНС —
 *             принимаются, на бумагу не выводятся.
 */
#define NUM_PRINTERS    2

UNIT printer_unit[NUM_PRINTERS] = {
    { UDATA(NULL, UNIT_ATTABLE + UNIT_SEQ, 0) },
    { UDATA(NULL, UNIT_ATTABLE + UNIT_SEQ, 0) },
};

static REG printer_reg[] = {
    { 0 }
};

static MTAB printer_mod[] = {
    { 0 }
};

#define DEB_OPS 000001
#define DEB_DAT 000040

static DEBTAB printer_deb[] = {
    { "OPS",  DEB_OPS, "commands" },
    { "DATA", DEB_DAT, "printed lines" },
    { NULL, 0 }
};

static t_stat printer_reset(DEVICE *dptr);
static t_stat printer_attach(UNIT *u, CONST char *cptr);
static t_stat printer_detach(UNIT *u);

DEVICE printer_dev = {
    "PRN", printer_unit, printer_reg, printer_mod,
    NUM_PRINTERS, 8, 19, 1, 8, 50,
    NULL, NULL, &printer_reset, NULL, &printer_attach, &printer_detach,
    NULL, DEV_DISABLE | DEV_DEBUG, 0, printer_deb
};

/*
 * ДКОИ -> Unicode. Буквы и знаки — по таблице ТДКОИ (УПП -> ДКОИ) из
 * ПЕЧАТЬ.bemsh; коды, общие для латинской и русской буквы одного начертания,
 * ТДКОИ отдаёт русской букве, так они и печатаются. Прочие знаки набора
 * ЕС-7036 (ЗНАКИ) — по стандартной раскладке ДКОИ. Ноль — непечатный код.
 */
static const unsigned short dkoi_to_unicode[256] = {
    [0x40] = ' ',
    [0x4A] = '[',    [0x4B] = '.',    [0x4C] = '<',    [0x4D] = '(',
    [0x4E] = '+',    [0x4F] = '|',
    [0x50] = '&',
    [0x5A] = ']',    [0x5B] = 0x00A4, [0x5C] = '*',    [0x5D] = ')',
    [0x5E] = ';',    [0x5F] = 0x00AC,
    [0x60] = '-',    [0x61] = '/',
    [0x6A] = 0x00A6, [0x6B] = ',',    [0x6C] = '%',    [0x6D] = '_',
    [0x6E] = '>',    [0x6F] = '?',
    [0x7A] = ':',    [0x7B] = '#',    [0x7C] = '@',    [0x7D] = '\'',
    [0x7E] = '=',    [0x7F] = '"',
    [0xB8] = 0x042E, /* Ю */
    [0xBA] = 0x0411, /* Б */
    [0xBB] = 0x0426, /* Ц */
    [0xBC] = 0x0414, /* Д */
    [0xBE] = 0x0424, /* Ф */
    [0xBF] = 0x0413, /* Г */
    [0xC1] = 0x0410, /* А */
    [0xC2] = 0x0412, /* В */
    [0xC3] = 0x0421, /* С */
    [0xC4] = 'D',
    [0xC5] = 0x0415, /* Е */
    [0xC6] = 'F',
    [0xC7] = 'G',
    [0xC8] = 0x041D, /* Н */
    [0xC9] = 'I',
    [0xCB] = 0x0418, /* И */
    [0xCC] = 0x0419, /* Й */
    [0xCE] = 0x041B, /* Л */
    [0xD1] = 'J',
    [0xD2] = 0x041A, /* К */
    [0xD3] = 'L',
    [0xD4] = 0x041C, /* М */
    [0xD5] = 'N',
    [0xD6] = 0x041E, /* О */
    [0xD7] = 0x0420, /* Р */
    [0xD8] = 'Q',
    [0xD9] = 'R',
    [0xDC] = 0x041F, /* П */
    [0xDD] = 0x042F, /* Я */
    [0xE2] = 'S',
    [0xE3] = 0x0422, /* Т */
    [0xE4] = 'U',
    [0xE5] = 'V',
    [0xE6] = 'W',
    [0xE7] = 0x0425, /* Х */
    [0xE8] = 'Y',
    [0xE9] = 'Z',
    [0xEB] = 0x0423, /* У */
    [0xEC] = 0x0416, /* Ж */
    [0xEE] = 0x042C, /* Ь */
    [0xEF] = 0x042B, /* Ы */
    [0xF0] = '0', [0xF1] = '1', [0xF2] = '2', [0xF3] = '3', [0xF4] = '4',
    [0xF5] = '5', [0xF6] = '6', [0xF7] = '7', [0xF8] = '8', [0xF9] = '9',
    [0xFA] = 0x0417, /* З */
    [0xFB] = 0x0428, /* Ш */
    [0xFC] = 0x042D, /* Э */
    [0xFD] = 0x0429, /* Щ */
    [0xFE] = 0x0427, /* Ч */
    [0xFF] = 0x042A, /* Ъ */
};

#define LINE_BYTES  256                 /* с запасом: строка АЦПУ — 132 знака */

static t_stat printer_reset(DEVICE *dptr)
{
    return SCPE_OK;
}

static t_stat printer_attach(UNIT *u, CONST char *cptr)
{
    if (u->flags & UNIT_ATT)
        detach_unit(u);
    return attach_unit(u, cptr);
}

static t_stat printer_detach(UNIT *u)
{
    return detach_unit(u);
}

/*
 * Печать строки из памяти: nwords слов по 6 байт ДКОИ.
 * Хвостовые пробелы отбрасываются.
 */
static void printer_line(UNIT *u, int num, int memaddr, int nwords)
{
    unsigned char line[LINE_BYTES];
    int n = 0, len, i, k;

    for (i = 0; i < nwords && n + 6 <= LINE_BYTES; ++i) {
        t_value w = (memory[mmu_iom_data_pa(memaddr + i)] >> 16) & BITS48;

        for (k = 5; k >= 0; --k)
            line[n++] = (w >> (8 * k)) & 0xFF;
    }
    for (len = n; len > 0 && line[len-1] == 0x40; --len)
        continue;

    for (i = 0; i < len; ++i) {
        unsigned ch = dkoi_to_unicode[line[i]];

        if (ch == 0) {
            sim_debug(DEB_OPS, &printer_dev,
                "АЦПУ%d: непечатный код %02X в позиции %d\n", num, line[i], i);
            ch = 0x00B7;                /* видимая замена: · */
        }
        utf8_putc(ch, u->fileref);
    }
}

/*
 * Обмен с АЦПУ по одной заявке ЕС-канала.
 * Возвращает SCPE_UNATT, если к юниту не подключён файл (АЦПУ не готово).
 */
t_stat svs_printer_io(int num, int kop, int memaddr, int nwords)
{
    UNIT *u;
    int lines;

    if (num < 0 || num >= NUM_PRINTERS)
        return SCPE_NXUN;
    u = &printer_unit[num];
    if (!(u->flags & UNIT_ATT))
        return SCPE_UNATT;

    sim_debug(DEB_OPS, &printer_dev, "АЦПУ%d: КОП=%02X слов=%d адрес=%o\n",
        num, kop, nwords, memaddr);

    switch (kop & 3) {
    case 1:                             /* печать с последующей протяжкой */
        printer_line(u, num, memaddr, nwords);
        break;
    case 3:                             /* управление без печати */
        break;
    default:                            /* загрузка буферов и прочее */
        return SCPE_OK;
    }
    if (kop == 0x63 || kop == 0xFB)     /* загрузка БУФП / БУНС */
        return SCPE_OK;

    if (kop & 0x80) {
        fputc('\f', u->fileref);        /* прогон до канала 1 */
    } else {
        lines = (kop >> 3) & 3;
        if (lines == 0 && (kop & 3) == 1)
            fputc('\r', u->fileref);    /* без протяжки: следующая строка поверх */
        while (lines-- > 0)
            fputc('\n', u->fileref);
    }
    fflush(u->fileref);
    return SCPE_OK;
}
