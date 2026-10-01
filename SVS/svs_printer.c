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
 *
 * Режим SET PRN GOST (по умолчанию выключен) восстанавливает символы ГОСТ,
 * которых нет в наборе АЦПУ. ПЕЧСВС (ЭТОСИМ, ТДКОИ, ЗАМЕНА/ЗАМЕН1 в
 * печсвс.bemsh) печатает их одинаково для всех АЦПУ:
 *   - наложением: сначала проход без протяжки (КОП 01), потом основная
 *     строка в том же месте. Проходы без протяжки копятся, и при печати с
 *     протяжкой позиция, где сошлись ровно два разных байта из таблицы
 *     ЗАМЕНА/ЗАМЕН1, печатается составным символом (◇ ≡ ÷ ⊃ ⩾ ⩽ Ъ _);
 *     один и тот же знак, наложенный сам на себя, — жирным (Unicode
 *     Mathematical Bold, есть только для латиницы и цифр; подчерк _ — ▁
 *     U+2581 Lower One Eighth Block; прочие — один раз).
 *     Любые другие сочетания остаются наложением через '\r';
 *   - заменой другим знаком: однозначные (байт ДКОИ получается только из
 *     одного кода ГОСТ) возвращаются: @ ↑, # ≠, & ∧, ? °, _ ‾.
 *     Неоднозначные (Ю/⏨, Х/×, V/∨, кавычки, -/―) не трогаются.
 */
#define NUM_PRINTERS    2

#define UNIT_V_GOST     (UNIT_V_UF + 0)
#define UNIT_GOST       (1 << UNIT_V_GOST)  /* восстанавливать символы ГОСТ */

UNIT printer_unit[NUM_PRINTERS] = {
    { UDATA(NULL, UNIT_ATTABLE + UNIT_SEQ, 0) },
    { UDATA(NULL, UNIT_ATTABLE + UNIT_SEQ, 0) },
};

static REG printer_reg[] = {
    { 0 }
};

static t_stat printer_set_gost(UNIT *u, int32 val, CONST char *cptr, void *desc);
static t_stat printer_show_gost(FILE *st, UNIT *u, int32 val, CONST void *desc);

static MTAB printer_mod[] = {
    { MTAB_XTD|MTAB_VDV, 1, NULL, "GOST", &printer_set_gost, NULL, NULL,
        "Restore GOST characters: overprint pairs, unambiguous substitutions, bold" },
    { MTAB_XTD|MTAB_VDV, 0, NULL, "NOGOST", &printer_set_gost, NULL, NULL,
        "Print DKOI codes as is, overprints as separate passes" },
    { MTAB_XTD|MTAB_VDV, 0, "MODE", NULL, NULL, &printer_show_gost, NULL,
        "Show character restoration mode" },
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
    [0x4E] = '+',    [0x4F] = '!',    /* ТДКОИ: ГОСТ 133 ! */
    [0x50] = '&',
    [0x5A] = ']',    [0x5B] = '$',    [0x5C] = '*',    [0x5D] = ')',
    [0x5E] = ';',    [0x5F] = 0x00AC, /* ¬: ТДКОИ «ОТРИЦ», ГОСТ 123 */
    [0x60] = '-',    [0x61] = '/',
    [0x6A] = '|',    [0x6B] = ',',    [0x6C] = '%',    [0x6D] = '_',   /* 6A: ТДКОИ, ГОСТ 130 | */
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
    [0xE0] = '\\',   /* ЕС-7934: ГОСТ 130 | (ТАБКОД) */
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

/*
 * ДКОИ -> Unicode для других устройств (ЕС-7934 в svs_display.c); 0 — кода
 * нет в таблице (печатается как \XX, svs_put_unknown).
 */
unsigned svs_dkoi_unicode(unsigned char b)
{
    return dkoi_to_unicode[b];
}

/*
 * Код ДКОИ, которого нет в таблице: «\XX» (шестнадцатеричный), чтобы в
 * распечатке было видно, какой байт пришёл.
 */
void svs_put_unknown(unsigned char b, FILE *f)
{
    fprintf(f, "\\%02X", b);
}

#define UNKNOWN_CH  0x80000000u         /* | байт: печатать как \XX */

#define LINE_BYTES  256                 /* с запасом: строка АЦПУ — 132 знака */
#define MAX_PASSES  8                   /* проходов без протяжки подряд */

/*
 * Режим GOST: однозначные замены ТДКОИ (код ГОСТ -> байт ДКОИ) обратно.
 */
static const unsigned short gost_single[256] = {
    [0x7C] = 0x2191,    /* @  <- 021 ↑ */
    [0x7B] = 0x2260,    /* #  <- 034 ≠ */
    [0x50] = 0x2227,    /* &  <- 121 ∧ */
    [0x6F] = 0x00B0,    /* ?  <- 136 ° */
    [0x6D] = 0x203E,    /* _  <- 115 ‾ */
};

/*
 * Режим GOST: пары наложения ЗАМЕНА/ЗАМЕН1 (печсвс.bemsh), порядок
 * проходов не важен.
 */
static const struct {
    unsigned char a, b;
    unsigned short ch;
} gost_overprint[] = {
    { 0xE2, 0x6A, 0x25C7 },     /* S + ¦  -> 127 ◇ */
    { 0x7E, 0x6D, 0x2261 },     /* = + _  -> 125 ≡ */
    { 0x7A, 0x60, 0x00F7 },     /* : + -  -> 124 ÷ */
    { 0x5D, 0x60, 0x2283 },     /* ) + -  -> 122 ⊃ */
    { 0x6E, 0x6D, 0x2A7E },     /* > + _  -> 117 ⩾ */
    { 0x4C, 0x6D, 0x2A7D },     /* < + _  -> 116 ⩽ */
    { 0xEE, 0x7D, 0x042A },     /* Ь + '  -> 135 Ъ */
    { 0x4B, 0x60, '_' },        /* . + -  -> 132 _ */
};

/* Проходы без протяжки, ждущие строки с протяжкой (режим GOST). */
static unsigned char pend[NUM_PRINTERS][MAX_PASSES][LINE_BYTES];
static int npend[NUM_PRINTERS];

/*
 * Байт ДКОИ -> Unicode; кода нет в таблице — UNKNOWN_CH | байт («\XX»).
 */
static unsigned printer_char(UNIT *u, int num, unsigned char b, int pos)
{
    unsigned ch = 0;

    if ((u->flags & UNIT_GOST) && gost_single[b])
        ch = gost_single[b];
    if (ch == 0)
        ch = dkoi_to_unicode[b];
    if (ch == 0) {
        sim_debug(DEB_OPS, &printer_dev,
            "АЦПУ%d: непечатный код %02X в позиции %d\n", num, b, pos);
        ch = UNKNOWN_CH | b;
    }
    return ch;
}

/*
 * Жирный вариант знака (Unicode Mathematical Bold): только латиница и
 * цифры; для прочих — сам знак (подчерк — отдельно, в printer_compose).
 */
static unsigned printer_bold(unsigned ch)
{
    if (ch >= 'A' && ch <= 'Z')
        return 0x1D400 + (ch - 'A');
    if (ch >= 'a' && ch <= 'z')
        return 0x1D41A + (ch - 'a');
    if (ch >= '0' && ch <= '9')
        return 0x1D7CE + (ch - '0');
    return ch;
}

/*
 * Строка из памяти: nwords слов по 6 байт ДКОИ; остаток — пробелы (0x40).
 */
static void printer_fetch(int memaddr, int nwords, unsigned char *line)
{
    int n = 0, i, k;

    memset(line, 0x40, LINE_BYTES);
    for (i = 0; i < nwords && n + 6 <= LINE_BYTES; ++i) {
        t_value w = (memory[mmu_iom_data_pa(memaddr + i)] >> 16) & BITS48;

        for (k = 5; k >= 0; --k)
            line[n++] = (w >> (8 * k)) & 0xFF;
    }
}

/*
 * Вывести строку знаков Unicode без хвостовых пробелов.
 */
static void printer_emit(UNIT *u, const unsigned *ch, int n)
{
    int i;

    while (n > 0 && ch[n-1] == ' ')
        --n;
    for (i = 0; i < n; ++i)
        if (ch[i] & UNKNOWN_CH)
            svs_put_unknown(ch[i] & 0xFF, u->fileref);
        else
            utf8_putc(ch[i], u->fileref);
}

/*
 * Вывести строку байтов ДКОИ как есть.
 */
static void printer_emit_bytes(UNIT *u, int num, const unsigned char *line)
{
    unsigned ch[LINE_BYTES];
    int i;

    for (i = 0; i < LINE_BYTES; ++i)
        ch[i] = printer_char(u, num, line[i], i);
    printer_emit(u, ch, LINE_BYTES);
}

/*
 * Вывести накопленные проходы без протяжки как есть — каждый через '\r'.
 */
static void printer_flush_passes(UNIT *u, int num)
{
    int p;

    for (p = 0; p < npend[num]; ++p) {
        printer_emit_bytes(u, num, pend[num][p]);
        fputc('\r', u->fileref);
    }
    npend[num] = 0;
}

/*
 * Режим GOST: свести накопленные проходы с последней строкой и вывести.
 * В каждой позиции: один и тот же знак во всех проходах — в итоговую
 * строку (наложенный сам на себя — жирным); ровно два разных байта из
 * таблицы наложений — составной символ; прочее остаётся наложением.
 */
static void printer_compose(UNIT *u, int num, unsigned char *line)
{
    unsigned out[LINE_BYTES], ch[LINE_BYTES];
    int n = npend[num], c, p, i, j;

    for (c = 0; c < LINE_BYTES; ++c) {
        unsigned char v[MAX_PASSES + 1];
        int nv = 0, same = 1;

        for (p = 0; p < n; ++p)
            if (pend[num][p][c] != 0x40)
                v[nv++] = pend[num][p][c];
        if (line[c] != 0x40)
            v[nv++] = line[c];
        for (i = 1; i < nv; ++i)
            if (v[i] != v[0])
                same = 0;

        out[c] = printer_char(u, num, line[c], c);
        if (nv == 0)
            continue;
        if (same) {
            out[c] = printer_char(u, num, v[0], c);
            if (nv > 1)                 /* жирная черта снизу — ▁ */
                out[c] = (v[0] == 0x6D) ? 0x2581 : printer_bold(out[c]);
        } else if (nv == 2) {
            for (j = 0; j < (int)(sizeof(gost_overprint) / sizeof(gost_overprint[0])); ++j)
                if ((v[0] == gost_overprint[j].a && v[1] == gost_overprint[j].b) ||
                    (v[0] == gost_overprint[j].b && v[1] == gost_overprint[j].a))
                    break;
            if (j == (int)(sizeof(gost_overprint) / sizeof(gost_overprint[0])))
                continue;               /* не из таблицы: остаётся наложением */
            out[c] = gost_overprint[j].ch;
        } else {
            continue;
        }
        for (p = 0; p < n; ++p)         /* сведено в итоговую строку */
            pend[num][p][c] = 0x40;
    }

    for (p = 0; p < n; ++p) {           /* оставшиеся наложения */
        int any = 0;

        for (c = 0; c < LINE_BYTES; ++c) {
            ch[c] = printer_char(u, num, pend[num][p][c], c);
            if (pend[num][p][c] != 0x40)
                any = 1;
        }
        if (any) {
            printer_emit(u, ch, LINE_BYTES);
            fputc('\r', u->fileref);
        }
    }
    npend[num] = 0;
    printer_emit(u, out, LINE_BYTES);
}

static t_stat printer_reset(DEVICE *dptr)
{
    int i;

    for (i = 0; i < NUM_PRINTERS; ++i)
        if ((printer_unit[i].flags & UNIT_ATT) && npend[i]) {
            printer_flush_passes(&printer_unit[i], i);
            fflush(printer_unit[i].fileref);
            printer_unit[i].pos = (t_addr)ftell(printer_unit[i].fileref);
        }
    return SCPE_OK;
}

static t_stat printer_attach(UNIT *u, CONST char *cptr)
{
    if (u->flags & UNIT_ATT)
        printer_detach(u);
    return attach_unit(u, cptr);
}

static t_stat printer_detach(UNIT *u)
{
    int num = (int)(u - printer_unit);

    if ((u->flags & UNIT_ATT) && npend[num])
        printer_flush_passes(u, num);
    npend[num] = 0;
    return detach_unit(u);
}

/*
 * SET PRN GOST / NOGOST — на оба АЦПУ.
 */
static t_stat printer_set_gost(UNIT *u, int32 val, CONST char *cptr, void *desc)
{
    int i;

    if (cptr)
        return SCPE_ARG;
    for (i = 0; i < NUM_PRINTERS; ++i) {
        UNIT *pu = &printer_unit[i];

        if (!val && (pu->flags & UNIT_ATT) && npend[i]) {
            printer_flush_passes(pu, i);
            fflush(pu->fileref);
            pu->pos = (t_addr)ftell(pu->fileref);
        }
        if (val)
            pu->flags |= UNIT_GOST;
        else
            pu->flags &= ~UNIT_GOST;
    }
    return SCPE_OK;
}

static t_stat printer_show_gost(FILE *st, UNIT *u, int32 val, CONST void *desc)
{
    fprintf(st, (printer_unit[0].flags & UNIT_GOST) ? "GOST" : "NOGOST");
    return SCPE_OK;
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

    lines = (kop >> 3) & 3;
    switch (kop & 3) {
    case 1: {                           /* печать с последующей протяжкой */
        unsigned char line[LINE_BYTES];

        printer_fetch(memaddr, nwords, line);
        if (!(u->flags & UNIT_GOST)) {
            printer_emit_bytes(u, num, line);
            break;
        }
        if (!(kop & 0x80) && lines == 0) {
            /* проход без протяжки: ждать строки, которая ляжет поверх */
            if (npend[num] == MAX_PASSES)
                printer_flush_passes(u, num);
            memcpy(pend[num][npend[num]++], line, LINE_BYTES);
            return SCPE_OK;
        }
        printer_compose(u, num, line);
        break;
    }
    case 3:                             /* управление без печати */
        if (kop == 0x63 || kop == 0xFB) /* загрузка БУФП / БУНС */
            return SCPE_OK;
        if (npend[num])                 /* бумага двинется: наложения как есть */
            printer_flush_passes(u, num);
        break;
    default:                            /* загрузка буферов и прочее */
        return SCPE_OK;
    }

    if (kop & 0x80) {
        fputc('\f', u->fileref);        /* прогон до канала 1 */
    } else {
        if (lines == 0 && (kop & 3) == 1)
            fputc('\r', u->fileref);    /* без протяжки: следующая строка поверх */
        while (lines-- > 0)
            fputc('\n', u->fileref);
    }
    fflush(u->fileref);
    /* go/cont ставят файлы последовательных юнитов на u->pos (scp.c,
       run_cmd): без сдвига pos каждое продолжение затирало печать с начала. */
    u->pos = (t_addr)ftell(u->fileref);
    return SCPE_OK;
}
