/*
 * SVS magnetic disk device (МД), ported from BESM6/besm6_disk.c.
 *
 * Copyright (c) 2009, Serge Vakulenko
 * Copyright (c) 2009, Leonid Broukhis
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
 * SERGE VAKULENKO OR LEONID BROUKHIS BE LIABLE FOR ANY CLAIM, DAMAGES
 * OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE
 * OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * Except as contained in this notice, the name of Leonid Broukhis or
 * Serge Vakulenko shall not be used in advertising or otherwise to promote
 * the sale, use or other dealings in this Software without prior written
 * authorization from Leonid Broukhis and Serge Vakulenko.
 */
#include "svs_defs.h"
#include <string.h>

/*
 * Размер зоны на диске: 8 служебных слов + 1024 слова данных = 1 лист.
 */
#define ZONE_SIZE   (8 + 1024)              /* слов в зоне на диске (физический формат) */
#define DISK_SIZE   (1024 * ZONE_SIZE)      /* слов на устройстве (1024 зоны) */

/*
 * Число слов данных зоны В ПАМЯТИ после свёртки (см. ниже) — 768, а не 1024.
 */
#define ZONE_DATA_WORDS   768
/*
 * Смещение области данных от НАМ (базы заявки): ПВВ вызывает нас как
 * svs_disk_io(dev, zone, memaddr, memaddr + 16, ...). Служебные слова лежат
 * по смещениям 0..7, 8..15 не используются, данные — с 16. Именно эту
 * раскладку покрывает РАЗМ=784 (=0o1420) полной заявки к МД.
 */
#define DISK_DATA_OFFSET  16

/*
 * Формат слова в образе диска (svs2053.bin и т.п.):
 *
 *   8 байт на слово, little-endian. 48-разрядное слово БЭСМ-6 лежит в МЛАДШИХ
 *   6 байтах; 7-й байт — тип слова: 1 = команда, 2 = число (данные); 8-й байт
 *   не используется.
 *
 * Служебные слова зоны (8 шт.) переносятся в память 1:1: значение — в
 * старших разрядах 17..64, младшие 16 (РМР) = 0, тип слова — в tag[]
 * (035 = команда, 036 = число). РАСПАК читает их плоским СЧ (см. цикл ПСС
 * в адап.bemsh), поэтому тег обязан остаться 035/036.
 *
 * Слова данных зоны (1024 шт. на диске) реальное железо СВС в память 1:1 НЕ
 * переносит — канал их сворачивает (см. dispak-svs/tosvs.c, сверено с
 * процедурой РАСПАК в адап.bemsh/adap.txt): 1024 слова = 768 D-слов
 * (data[0..767]) + 256 S-слов (data[768..1023]); каждое 48-битное S-слово
 * режется на три 16-разрядных фрагмента, которые ложатся в РМР (младшие 16
 * разрядов) трёх последовательных D-слов:
 *
 *   out[3j+0] = D[3j+0]<<16 | S[j] разр.48..33
 *   out[3j+1] = D[3j+1]<<16 | S[j] разр.32..17
 *   out[3j+2] = D[3j+2]<<16 | S[j] разр.16..1
 *
 * В памяти зона данных занимает поэтому не 1024, а 768 слов (ZONE_DATA_WORDS).
 * РАСПАК читает их командой СЧП (64-битное "чтение полное с тегом БЭСМ"), а
 * не плоским СЧ — соответственно эти слова тегируются TAG_BITSET, а не
 * 035/036: тег 035/036 на таком слове означает, что оно ещё не свёрнуто (или
 * свёртка не выполнена), и провоцирует контроль числа при 64-битном чтении
 * (см. mmu_load64 в svs_mmu.c).
 */
#define DISK_TAG_INSN   1                   /* 7-й байт: команда */
#define DISK_TAG_DATA   2                   /* 7-й байт: число   */

/*
 * Параметры обмена с устройством МД.
 */
typedef struct {
    int dev;                    /* выбранное устройство 0..7 */
    int zone;                   /* номер зоны на диске */
    int memory;                 /* начальный физ. адрес данных в ОЗУ */
    int sysarea;                /* физ. адрес 8 служебных слов в ОЗУ */
    int status;                 /* регистр состояния */
} DISKCTL;

static DISKCTL controller;      /* один контроллер КМД */

t_stat disk_event(UNIT *u);

/*
 * Нумерация накопителей МД совпадает с адресацией ВЫЗПВВ и АДАП-а.
 *
 * В заявке номер устройства лежит в слове СПУ, разр.39-42 — ЧЕТЫРЕ разряда
 * (эмулятор: (spu>>38)&017; сам АДАП проверяет так же, ВЫППВВ: СДА 64+38 …
 * И =В'17'). Значит адресуются устройства 0..15, и столько же UNIT-ов.
 *
 * ВЫЗПВВ хранит этот же номер в НАПРУС (030115), разбирая его командой
 * РЗБ по маске D02363 = 0070000000001600 на две тройки разрядов:
 *      разр.40-42 — НАПРАВЛЕНИЕ = номер/8
 *      разр.8-10  — УСТРОЙСТВО  = номер%8
 * Измерено: DISK1 -> НАПРУС 0000…0200 (напр 0, устр 1), DISK5 -> 0000…1200
 * (напр 0, устр 5), DISK9 -> 0010…0200 (напр 1, устр 1), DISK13 -> 0010…1200
 * (напр 1, устр 5).
 *
 * Поэтому `attach DISKn` — это ровно устройство n в терминах ВЫЗПВВ/АДАП-а,
 * а его направление равно n/8. Отдельной настройки направления не нужно и
 * быть не может: направление — это старшая тройка того же номера.
 *
 * (Табл.2 инструкции на УБД, ИБЗ.057.008 ИЭ, даёт 4 ВУ на направление — это
 * про другой контроллер; по прогонам ВЫЗПВВ здесь выходит 8.)
 */
#define NUM_DISK_UNITS 16
#define DISK_NAPR(dev)  ((dev) / 8)
#define DISK_UNIT_IN_NAPR(dev)  ((dev) % 8)

UNIT disk_unit[NUM_DISK_UNITS] = {
    { UDATA(disk_event, UNIT_FIX+UNIT_ATTABLE+UNIT_ROABLE+UNIT_DISABLE, DISK_SIZE) },
    { UDATA(disk_event, UNIT_FIX+UNIT_ATTABLE+UNIT_ROABLE+UNIT_DISABLE, DISK_SIZE) },
    { UDATA(disk_event, UNIT_FIX+UNIT_ATTABLE+UNIT_ROABLE+UNIT_DISABLE, DISK_SIZE) },
    { UDATA(disk_event, UNIT_FIX+UNIT_ATTABLE+UNIT_ROABLE+UNIT_DISABLE, DISK_SIZE) },
    { UDATA(disk_event, UNIT_FIX+UNIT_ATTABLE+UNIT_ROABLE+UNIT_DISABLE, DISK_SIZE) },
    { UDATA(disk_event, UNIT_FIX+UNIT_ATTABLE+UNIT_ROABLE+UNIT_DISABLE, DISK_SIZE) },
    { UDATA(disk_event, UNIT_FIX+UNIT_ATTABLE+UNIT_ROABLE+UNIT_DISABLE, DISK_SIZE) },
    { UDATA(disk_event, UNIT_FIX+UNIT_ATTABLE+UNIT_ROABLE+UNIT_DISABLE, DISK_SIZE) },
    { UDATA(disk_event, UNIT_FIX+UNIT_ATTABLE+UNIT_ROABLE+UNIT_DISABLE, DISK_SIZE) },
    { UDATA(disk_event, UNIT_FIX+UNIT_ATTABLE+UNIT_ROABLE+UNIT_DISABLE, DISK_SIZE) },
    { UDATA(disk_event, UNIT_FIX+UNIT_ATTABLE+UNIT_ROABLE+UNIT_DISABLE, DISK_SIZE) },
    { UDATA(disk_event, UNIT_FIX+UNIT_ATTABLE+UNIT_ROABLE+UNIT_DISABLE, DISK_SIZE) },
    { UDATA(disk_event, UNIT_FIX+UNIT_ATTABLE+UNIT_ROABLE+UNIT_DISABLE, DISK_SIZE) },
    { UDATA(disk_event, UNIT_FIX+UNIT_ATTABLE+UNIT_ROABLE+UNIT_DISABLE, DISK_SIZE) },
    { UDATA(disk_event, UNIT_FIX+UNIT_ATTABLE+UNIT_ROABLE+UNIT_DISABLE, DISK_SIZE) },
    { UDATA(disk_event, UNIT_FIX+UNIT_ATTABLE+UNIT_ROABLE+UNIT_DISABLE, DISK_SIZE) },
};

/*
 * Адреса ячеек ГОД / ЗАНЯТА / МГРП для autotime — из таблицы имён
 * резидента на томе 2053 (зоны 0504 и 0742). Заполняются при attach.
 */
int autotime_year;              /* ГОД */
int autotime_taken;             /* ЗАНЯТА */
int autotime_mgrp;              /* МГРП */
int ipzzt;                      /* ИПЗЖТ — база паспорта задачи «жду» */
static UNIT *autotime_sym_unit; /* какой UNIT их выставил */

static REG disk_reg[] = {
    { ORDATA(УСТР,  controller.dev,     3) },
    { ORDATA(ЗОНА,  controller.zone,   10) },
    { ORDATA(МОЗУ,  controller.memory, 20) },
    { ORDATA(СЛУЖ,  controller.sysarea,20) },
    { ORDATA(РС,    controller.status, 24) },
    { ORDATA(YEAR,  autotime_year,     15) },
    { ORDATA(TAKEN, autotime_taken,    15) },
    { ORDATA(MGRP,  autotime_mgrp,     15) },
    { ORDATA(IPZZT, ipzzt,             15) },
    { 0 }
};

/*
 * Тип ёмкости МД. От него зависит, удвоен ли номер зоны в слове СПУ:
 * АДАП читает тип из ТУСЗ и при «не 29 МГБ» делает СДА 64-1 (ветка ДИСК),
 * то есть удваивает номер. Первичный загрузчик ВЫЗПВВ таблиц не имеет и
 * шлёт НАСТОЯЩИЙ номер зоны (например 0747 — зона АДАП-а), поэтому делить
 * его пополам нельзя.
 *
 * По умолчанию 7,25 МБ (зона удвоена) — так ведёт себя конфигурация
 * dispak.ini, где ТУСЗ не настроена.
 */
#define UNIT_V_29MB     (UNIT_V_UF + 0)
#define UNIT_29MB       (1u << UNIT_V_29MB)

static MTAB disk_mod[] = {
    { UNIT_29MB, 0,         "7MB",  "7MB",  NULL, NULL, NULL,
      "тип ёмкости 7,25 МБ: номер зоны в СПУ удвоен" },
    { UNIT_29MB, UNIT_29MB, "29MB", "29MB", NULL, NULL, NULL,
      "тип ёмкости 29 МБ: номер зоны в СПУ настоящий" },
    { 0 }
};

#define DEB_OPS 000001
#define DEB_DAT 000040

static DEBTAB disk_deb[] = {
    { "OPS",  DEB_OPS, "transactions" },
    { "DATA", DEB_DAT, "transfer data" },
    { NULL, 0 }
};

static t_stat disk_reset(DEVICE *dptr);
static t_stat disk_attach(UNIT *u, CONST char *cptr);
static t_stat disk_detach(UNIT *u);
static void disk_clear_autotime_syms(void);
static void disk_load_autotime_syms(UNIT *u);

DEVICE disk_dev = {
    "DISK", disk_unit, disk_reg, disk_mod,
    NUM_DISK_UNITS, 8, 21, 1, 8, 50,
    NULL, NULL, &disk_reset, NULL, &disk_attach, &disk_detach,
    NULL, DEV_DISABLE | DEV_DEBUG, 0, disk_deb
};

/* Имена в ГОСТ-10859, по 6 байт MSB-first (проверено на svs2053). */
#define SYM_GOD     0x232e240f0f0fLL         /* ГОД···· */
#define SYM_MGRP    0x2c23302f0f0fLL         /* МГРП·· */
#define SYM_TAKEN   0x27202d3e3220LL         /* ЗАНЯТА */
#define SYM_IPZZT   0x282f2726320fLL         /* ИПЗЖТ· */

#define ZONE_NAMES  0504
#define ZONE_ADDRS  0742

/*
 * Reset routine.
 */
static t_stat disk_reset(DEVICE *dptr)
{
    int i;

    memset(&controller, 0, sizeof(controller));
    for (i = 0; i < NUM_DISK_UNITS; ++i) {
        disk_unit[i].dptr = dptr;
        sim_cancel(&disk_unit[i]);
    }
    return SCPE_OK;
}

static void disk_clear_autotime_syms(void)
{
    autotime_year = 0;
    autotime_taken = 0;
    autotime_mgrp = 0;
    ipzzt = 0;
    autotime_sym_unit = NULL;
}

/*
 * Плоское чтение 1024 слов данных зоны (без свёртки 4:3) — таблицы
 * имён/адресов на носителе лежат 1:1, не через канал РАСПАК.
 */
static t_stat disk_read_zone_data(UNIT *u, int zone, t_value *data)
{
    t_value buf[ZONE_SIZE];
    int i;

    if (fseek(u->fileref, (long)ZONE_SIZE * zone * 8, SEEK_SET) != 0 ||
        sim_fread(buf, 8, ZONE_SIZE, u->fileref) != ZONE_SIZE)
        return SCPE_IOERR;
    for (i = 0; i < 1024; ++i)
        data[i] = buf[8 + i] & BITS48;
    return SCPE_OK;
}

static int disk_sym_halfword(const t_value *addrs, int idx)
{
    t_value w = addrs[idx / 2];

    if (idx & 1)
        return (int)(w & 077777777);
    return (int)((w >> 24) & 077777777);
}

/*
 * На томе 2053: зона 0504 — имена ячеек (ГОСТ, слово на имя),
 * зона 0742 — адреса (полуслово на имя, тот же индекс).
 */
static void disk_load_autotime_syms(UNIT *u)
{
    t_value names[1024], addrs[1024];
    int i, year = 0, taken = 0, mgrp = 0, ipz = 0;

    if (disk_read_zone_data(u, ZONE_NAMES, names) != SCPE_OK ||
        disk_read_zone_data(u, ZONE_ADDRS, addrs) != SCPE_OK) {
        sim_printf("%s: cannot read symbol table (zones %o/%o)\n",
                   sim_uname(u), ZONE_NAMES, ZONE_ADDRS);
        return;
    }
    for (i = 0; i < 1024; ++i) {
        if (names[i] == SYM_GOD)
            year = disk_sym_halfword(addrs, i);
        else if (names[i] == SYM_TAKEN)
            taken = disk_sym_halfword(addrs, i);
        else if (names[i] == SYM_MGRP)
            mgrp = disk_sym_halfword(addrs, i);
        else if (names[i] == SYM_IPZZT)
            ipz = disk_sym_halfword(addrs, i);
    }
    if (!year || !taken || !mgrp || !ipz) {
        sim_printf("%s: symbol table missing ГОД/ЗАНЯТА/МГРП/ИПЗЖТ\n",
                   sim_uname(u));
        return;
    }
    autotime_year = year;
    autotime_taken = taken;
    autotime_mgrp = mgrp;
    ipzzt = ipz;
    autotime_sym_unit = u;
}

static t_stat disk_attach(UNIT *u, CONST char *cptr)
{
    t_stat r;
    char *basename;

    /* Образ диска существует заранее; принудительно требуем '-e'. */
    sim_switches |= SWMASK('E');
    r = attach_unit(u, cptr);
    if (r != SCPE_OK)
        return r;

    /* Том 2053: имя файла содержит «2053» (svs2053.bin, 2053, …). */
    basename = sim_filepath_parts(u->filename, "n");
    if (basename && strstr(basename, "2053"))
        disk_load_autotime_syms(u);
    free(basename);
    return SCPE_OK;
}

static t_stat disk_detach(UNIT *u)
{
    if (u == autotime_sym_unit)
        disk_clear_autotime_syms();
    return detach_unit(u);
}

/*
 * Событие (завершение обмена). Пока не используется.
 */
t_stat disk_event(UNIT *u)
{
    //TODO: отложенное завершение обмена + прерывание ПРПВВ.
    return SCPE_OK;
}

/*
 * Преобразование одного слова образа диска (8 байт LE) в слово ОЗУ + тег.
 */
static void disk_word_to_mem(t_value w, int addr)
{
    t_value value48 = w & BITS48;           /* младшие 6 байт — значение */
    int pa = mmu_iom_data_pa(addr);              /* адрес из заявки — физический */

    memory[pa] = value48 << 16;             /* значение в разрядах 17..64, РМР=0 */
    /*
     * Прочитанное с диска всегда метится как КОМАНДА (035).
     * Тег переносится дальше сам: РАСПАК читает слово через СОП
     * (mmu_load_with_tag кладёт тег в TagR) и пишет через ЗП/ЗПП, так что
     * распакованная "вызывалка" остаётся исполнимой. Пометь её числом (036) —
     * и переход на неё (032245: ПБ 02000) даст контроль команды.
     */
    tag[pa] = TAG_INSN48;
}

/*
 * Обратное преобразование: слово ОЗУ + тег → слово образа диска.
 */
static t_value mem_to_disk_word(int addr)
{
    int pa = mmu_iom_data_pa(addr);              /* адрес из заявки — физический */
    t_value value48  = (memory[pa] >> 16) & BITS48;
    unsigned tagbyte = (tag[pa] == TAG_INSN48) ? DISK_TAG_INSN : DISK_TAG_DATA;

    return value48 | ((t_value)tagbyte << 48);
}

/*
 * Чтение одной зоны (8 служебных + 1024 слова данных) с диска в ОЗУ.
 * Служебные слова кладутся по адресу sysaddr, данные — по адресу memaddr.
 * Возвращает SCPE_OK либо SCPE_IOERR.
 */
t_stat svs_disk_read(UNIT *u, int zone, int sysaddr, int memaddr, int nwords)
{
    t_value buf[ZONE_SIZE];
    int i, j, k;

    if (!(u->flags & UNIT_ATT))
        return SCPE_UNATT;

    if (fseek(u->fileref, (long)ZONE_SIZE * zone * 8, SEEK_SET) != 0 ||
        sim_fread(buf, 8, ZONE_SIZE, u->fileref) != ZONE_SIZE) {
        return SCPE_IOERR;
    }

    if (u->dptr->dctrl & DEB_DAT)
        sim_debug(DEB_DAT, u->dptr, "::: чтение МД зона %04o СС@%05o данные@%05o\n",
                  zone, sysaddr, memaddr);

    /*
     * Служебные слова: 1:1, тег из файла (035/036).
     * nwords — поле РАЗМ слова ДО, отсчитывается ОТ НАМ (= sysaddr): у МД
     * полная заявка РАЗМ=784 покрывает 8 служ. + 8 пропуск + 768 данных.
     */
    for (i = 0; i < 8 && i < nwords; ++i)
        disk_word_to_mem(buf[i], sysaddr + i);

    /*
     * Что ОС увидит в служебных словах. ДИСКИ сверяет N зоны из СС[0] со
     * своим КУС (ПРЗОНЫ, физ.073503; в листинге diski.txt 73477) и при
     * несовпадении уходит в ПЛОХО с кодом ОШЗОНЫ. Сходится, когда СС[0]
     * равно номеру зоны образа: ОС сама так его и пишет (диски.bemsh, ОБ4А).
     * В исходном svs2053.bin там лежит удвоенный номер — отсюда зацикливание
     * на зоне 0462; лечит tools/makeSVS2053.py (svs2053-fixed.bin).
     */
    if (SVS_DEV_TRACE()) {
        t_value ss0 = (memory[mmu_iom_data_pa(sysaddr)] >> 16) & BITS48;

        if ((ss0 >> 36) != (t_value)zone)
            fprintf(sim_deb, "disk ---   зона %04o: СС[0]=%016jo — ОС ждёт"
                " %04o, будет ОШЗОНЫ\n", zone, (uintmax_t)ss0, zone);
    }

    /*
     * Слова данных, свёртка 4:3. Логическая страница лежит на диске В ПОРЯДКЕ:
     * СНАЧАЛА 256 S-слов (buf[8..263]), ПОТОМ 768 D-слов (buf[264..1031]).
     *
     * Так требует РАСПАК, который распаковывает в два прохода:
     *   ЦИКР (034710) собирает младшие 16 разр. трёх подряд идущих упакованных
     *     слов в одно слово и кладёт 256 таких слов по 02000-02377 —
     *     то есть ВОССТАНАВЛИВАЕТ S-слова;
     *   ОЧМЛ (034716) обнуляет младшие 16 разр. 768 слов по 02400-03777,
     *     оставляя чистые D-слова.
     * Точка входа "вызывалки" (02000) — это ПЕРВОЕ S-слово, поэтому S-область
     * обязана быть началом зоны, а не её хвостом.
     */
    for (j = 0; j < ZONE_DATA_WORDS / 3; ++j) {
        /*
         * Группы кладутся в ОБРАТНОМ порядке: РАСПАК читает упакованные слова
         * СВЕРХУ ВНИЗ (034711: соп, адреса 03777, 03776, ...), а результат
         * пишет СНИЗУ ВВЕРХ (034710: зп 2377(1), адреса 02000, 02001, ...).
         * Значит верхняя тройка обязана нести ПЕРВУЮ логическую группу, иначе
         * страница получается перевёрнутой и в точке входа 02000 оказывается
         * последнее слово зоны, а не первое.
         */
        /*
         * Обращается ТОЛЬКО привязка S-слов, но не порядок D-слов:
         *   ЦИКР (034710) читает тройки СВЕРХУ ВНИЗ, а собранные из их РМР
         *     S-слова пишет СНИЗУ ВВЕРХ -> S выходят перевёрнутыми, значит
         *     верхняя тройка обязана нести S[0];
         *   ОЧМЛ (034716) обрабатывает D-слова НА МЕСТЕ, ничего не переставляя,
         *     значит D должны лежать в прямом порядке.
         * Раньше переворачивалась вся группа, и D-область тоже оказывалась
         * задом наперёд: вызывалка читала свои таблицы (02454, 02624) из
         * пустого хвоста зоны и обнуляла себе приписку.
         */
        int g = (ZONE_DATA_WORDS / 3 - 1) - j;      /* S — в обратном порядке */
        t_value s = buf[8 + g] & BITS48;
        for (k = 0; k < 3; ++k) {
            t_value w    = buf[8 + ZONE_DATA_WORDS/3 + 3*j + k];   /* D — по порядку */
            t_value d    = w & BITS48;
            t_value frag = (s >> (32 - 16*k)) & 0xFFFF;
            int off  = DISK_DATA_OFFSET + 3*j + k;      /* смещение от НАМ */
            int addr;

            if (off >= nwords)
                continue;                   /* за пределами РАЗМ — не наше дело */
            addr = mmu_iom_data_pa(memaddr + 3*j + k);
            memory[addr] = (d << 16) | frag;
            tag[addr]    = TAG_INSN48;      /* см. disk_word_to_mem */
        }
    }

    return SCPE_OK;
}

/*
 * Запись одной зоны (8 служебных + 1024 слова данных) из ОЗУ на диск.
 */
t_stat svs_disk_write(UNIT *u, int zone, int sysaddr, int memaddr, int nwords)
{
    t_value buf[ZONE_SIZE];
    int i, j, k;

    if (!(u->flags & UNIT_ATT))
        return SCPE_UNATT;
    if (u->flags & UNIT_RO)
        return SCPE_RO;

    /*
     * ЧАСТИЧНАЯ запись (РАЗМ меньше полной заявки) обязана сохранить всё, чего
     * заявка не касается: зона на диске пишется целиком, и незатребованные
     * слова нельзя обнулять — это стёрло бы данные. Поэтому сначала читаем
     * зону с диска, а потом перекрываем только запрошенное. Если зоны ещё нет
     * (разреженный файл, чтение за концом) — начинаем с нулей.
     */
    if (nwords < DISK_DATA_OFFSET + ZONE_DATA_WORDS) {
        if (fseek(u->fileref, (long)ZONE_SIZE * zone * 8, SEEK_SET) != 0 ||
            sim_fread(buf, 8, ZONE_SIZE, u->fileref) != ZONE_SIZE)
            memset(buf, 0, sizeof(buf));
    }

    for (i = 0; i < 8 && i < nwords; ++i)
        buf[i] = mem_to_disk_word(sysaddr + i);

    /* Развёртка 3:4, обратная свёртке в svs_disk_read: из 768 слов памяти
     * восстанавливаем 768 D-слов и 256 S-слов. Тип слова сохраняем: свёрнутое
     * слово несёт настоящий тег (035/036) из образа диска, и он же переносится
     * в распакованное слово через TagR (см. комментарий в svs_disk_read). */
    for (j = 0; j < ZONE_DATA_WORDS / 3; ++j) {
        t_value s = 0;

        int touched = 0;

        for (k = 0; k < 3; ++k) {
            int off = DISK_DATA_OFFSET + 3*j + k;
            t_value word, d, frag;

            if (off >= nwords) {
                /* Слово вне заявки — оставляем то, что уже лежит в зоне. */
                t_value old_d = buf[8 + ZONE_DATA_WORDS/3 + 3*j + k] & BITS48;
                s = (s << 16) | ((buf[8 + j] >> (32 - 16*k)) & 0xFFFF);
                buf[8 + ZONE_DATA_WORDS/3 + 3*j + k] =
                    old_d | ((t_value)DISK_TAG_INSN << 48);
                continue;
            }
            touched = 1;
            word = memory[mmu_iom_data_pa(memaddr + 3*j + k)];
            d    = (word >> 16) & BITS48;
            frag = word & 0xFFFF;

            buf[8 + ZONE_DATA_WORDS/3 + 3*j + k] = d | ((t_value)DISK_TAG_INSN << 48);
            s = (s << 16) | frag;
        }
        if (touched)
            buf[8 + j] = s | ((t_value)DISK_TAG_INSN << 48);
    }

    if (u->dptr->dctrl & DEB_DAT)
        sim_debug(DEB_DAT, u->dptr, "::: запись МД зона %04o СС@%05o данные@%05o\n",
                  zone, sysaddr, memaddr);

    if (fseek(u->fileref, (long)ZONE_SIZE * zone * 8, SEEK_SET) != 0 ||
        sim_fwrite(buf, 8, ZONE_SIZE, u->fileref) != ZONE_SIZE) {
        return SCPE_IOERR;
    }
    return SCPE_OK;
}

/*
 * Точка входа для ПВВ: обмен зоной с устройством dev.
 *
 * TODO (привязка к ПВВ): вызывается из svs_iom.c при обработке заявки обмена с
 * МД, после разбора зоны/адресов из блока БАКПВВ или заявки в ТОЧ. Направление
 * (чтение/запись), номер зоны, адреса служебных слов и данных берутся из
 * описателя обмена (СМ/ДО/СО/СПУ, см. ПВВ.md §5).
 */
/*
 * Во сколько раз номер зоны в слове СПУ больше настоящего:
 * 2 — тип ёмкости 7,25 МБ (АДАП удваивает), 1 — 29 МБ (номер как есть).
 */
/*
 * Направление (номер тракта), к которому подключён накопитель.
 * По табл.2 инструкции на УБД оно однозначно задаётся номером ВУ: ВУ/4.
 */
int svs_disk_napr(int dev)
{
    if (dev < 0 || dev >= NUM_DISK_UNITS)
        return -1;
    return DISK_NAPR(dev);
}

int svs_disk_zone_scale(int dev)
{
    if (dev < 0 || dev >= NUM_DISK_UNITS)
        return 2;
    return (disk_unit[dev].flags & UNIT_29MB) ? 1 : 2;
}

t_stat svs_disk_io(int dev, int zone, int sysaddr, int memaddr, int is_write, int nwords)
{
    UNIT *u;

    if (dev < 0 || dev >= NUM_DISK_UNITS)
        return SCPE_NXDEV;
    u = &disk_unit[dev];

    controller.dev     = dev;
    controller.zone    = zone;
    controller.memory  = memaddr;
    controller.sysarea = sysaddr;

    return is_write ? svs_disk_write(u, zone, sysaddr, memaddr, nwords)
                    : svs_disk_read(u, zone, sysaddr, memaddr, nwords);
}
