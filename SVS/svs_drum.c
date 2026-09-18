/*
 * SVS magnetic drum device (МБ).
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

/*
 * Магнитные барабаны (МБ) в таблице устройств АДАП-а — блок ТУСМБ@030460:
 * восемь барабанов МБ 00-07, канал Х'20', класс 4 (у диска класс 5).
 *
 * Отличия от диска (svs_disk.c) — два, и оба принципиальные:
 *
 *  1. НИКАКОЙ СВЁРТКИ. Диск хранит 1024 слова данных зоны, сворачивая их в
 *     768 слов памяти (4:3, D-слова + фрагменты S-слов в РМР), потому что
 *     этого ждёт РАСПАК. Барабан хранит слова КАК ЕСТЬ, один к одному.
 *
 *  2. ТЕГИ СОХРАНЯЮТСЯ БУКВАЛЬНО. Диск при чтении метит всё как команду
 *     (TAG_INSN48), а при записи — байтом 035/036. Барабан пишет и читает
 *     полное 64-разрядное слово memory[] вместе с его тегом tag[] без
 *     какого-либо перевода: что записали, то и прочитали.
 *
 * Барабаны — ЧИСТЫЕ (scratch): образ заранее не нужен, файл создаётся при
 * подключении, чтение незаписанной зоны даёт нули. Файл разреженный — место
 * занимают только реально записанные зоны, поэтому номер зоны не ограничен
 * заранее заведённым размером.
 */

/*
 * Размер зоны — тот же, что у диска: 8 служебных слов + 1024 слова данных.
 * Но, в отличие от диска, слова данных не сворачиваются, поэтому в памяти их
 * тоже 1024, а не 768.
 */
#define ZONE_SERVICE_WORDS  8
#define ZONE_DATA_WORDS     1024
#define ZONE_WORDS          (ZONE_SERVICE_WORDS + ZONE_DATA_WORDS)

/*
 * Формат записи слова в образе барабана — 16 байт:
 *   байты 0-7   полное 64-разрядное слово memory[] (little-endian);
 *   байт  8     тег tag[];
 *   байты 9-15  резерв, нули.
 *
 * Формат внутренний: образ барабана ни с какими внешними инструментами не
 * разделяется, поэтому выбран ради простой арифметики смещений.
 */
#define DRUM_WORD_BYTES     16
#define ZONE_BYTES          (ZONE_WORDS * DRUM_WORD_BYTES)

#define NUM_DRUM_UNITS      8

t_stat drum_event(UNIT *u);

UNIT drum_unit[NUM_DRUM_UNITS] = {
    { UDATA(drum_event, UNIT_ATTABLE+UNIT_DISABLE, 0) },
    { UDATA(drum_event, UNIT_ATTABLE+UNIT_DISABLE, 0) },
    { UDATA(drum_event, UNIT_ATTABLE+UNIT_DISABLE, 0) },
    { UDATA(drum_event, UNIT_ATTABLE+UNIT_DISABLE, 0) },
    { UDATA(drum_event, UNIT_ATTABLE+UNIT_DISABLE, 0) },
    { UDATA(drum_event, UNIT_ATTABLE+UNIT_DISABLE, 0) },
    { UDATA(drum_event, UNIT_ATTABLE+UNIT_DISABLE, 0) },
    { UDATA(drum_event, UNIT_ATTABLE+UNIT_DISABLE, 0) },
};

/*
 * Состояние контроллера — для показа в регистрах, как у диска.
 */
static struct {
    int dev;                            /* номер барабана */
    int zone;                           /* номер зоны */
    int memory;                         /* адрес данных в ОЗУ */
    int sysarea;                        /* адрес служебных слов в ОЗУ */
} controller;

static REG drum_reg[] = {
    { ORDATA(УСТР,  controller.dev,     3) },
    { ORDATA(ЗОНА,  controller.zone,   20) },
    { ORDATA(МОЗУ,  controller.memory, 20) },
    { ORDATA(СЛУЖ,  controller.sysarea,20) },
    { 0 }
};

static MTAB drum_mod[] = {
    { 0 }
};

#define DEB_OPS 000001
#define DEB_DAT 000040

static DEBTAB drum_deb[] = {
    { "OPS",  DEB_OPS, "transactions" },
    { "DATA", DEB_DAT, "transfer data" },
    { NULL, 0 }
};

static t_stat drum_reset(DEVICE *dptr);
static t_stat drum_attach(UNIT *u, CONST char *cptr);
static t_stat drum_detach(UNIT *u);

DEVICE drum_dev = {
    "DRUM", drum_unit, drum_reg, drum_mod,
    NUM_DRUM_UNITS, 8, 21, 1, 8, 50,
    NULL, NULL, &drum_reset, NULL, &drum_attach, &drum_detach,
    NULL, DEV_DISABLE | DEV_DEBUG, 0, drum_deb
};

static t_stat drum_reset(DEVICE *dptr)
{
    int i;

    memset(&controller, 0, sizeof(controller));
    for (i = 0; i < NUM_DRUM_UNITS; ++i) {
        drum_unit[i].dptr = dptr;
        sim_cancel(&drum_unit[i]);
    }
    return SCPE_OK;
}

/*
 * Подключение барабана.
 *
 * В отличие от диска (svs_disk.c: там принудительно ставится SWMASK('E') и
 * образ обязан существовать), барабан — чистый: если файла нет, он создаётся
 * пустым. Незаписанные зоны читаются нулями (см. svs_drum_read).
 */
static t_stat drum_attach(UNIT *u, CONST char *cptr)
{
    sim_switches &= ~SWMASK('E');       /* существование образа не требуется */
    return attach_unit(u, cptr);
}

static t_stat drum_detach(UNIT *u)
{
    return detach_unit(u);
}

/*
 * Событие (завершение обмена). Пока не используется.
 */
t_stat drum_event(UNIT *u)
{
    //TODO: отложенное завершение обмена + прерывание ПРПВВ.
    return SCPE_OK;
}

/*
 * Чтение зоны барабана в память.
 *
 * Служебные слова кладутся по адресу sysaddr, данные — по адресу memaddr.
 * Адреса ВИРТУАЛЬНЫЕ (приходят из заявки), поэтому каждое обращение к памяти
 * идёт через mmu_iom_pa(), как и в svs_disk.c.
 *
 * Если зона за концом файла (ещё ни разу не записана), отдаём нули — барабан
 * чистый.
 */
static t_stat svs_drum_read(UNIT *u, int zone, int sysaddr, int memaddr)
{
    uint8 buf[ZONE_BYTES];
    size_t got = 0;
    int i;

    if (!(u->flags & UNIT_ATT))
        return SCPE_UNATT;

    memset(buf, 0, sizeof(buf));
    if (fseek(u->fileref, (long)ZONE_BYTES * zone, SEEK_SET) == 0)
        got = sim_fread(buf, 1, ZONE_BYTES, u->fileref);
    if (got < ZONE_BYTES)
        memset(buf + got, 0, ZONE_BYTES - got);

    if (u->dptr->dctrl & DEB_DAT)
        sim_debug(DEB_DAT, u->dptr, "::: чтение МБ зона %04o СС@%05o данные@%05o\n",
                  zone, sysaddr, memaddr);

    if (got == 0) {
        /*
         * Зона ни разу не записывалась — отдаём НУЛИ с корректными тегами.
         *
         * Синтезировать «правильные» служебные слова нельзя, и это видно по
         * самому ГЕНС-у:
         *
         *  - роспись МБ (МБРОС, генс.bemsh:178) сначала ПИШЕТ зону образцом
         *    КАННА5/К5АННА и только потом читает её обратно, так что пустая
         *    зона на этом пути почти недостижима;
         *  - там, где ГЕНС действительно читает неразмеченный барабан (РБ:
         *    "ПВ ОБМИМБ(М15) ЧТ. М.К." → СЧ КЛЮМК / НТЖ NМЛ+2 / ПО РБА
         *    "КЛЮЧ ВЕРЕН" → РОСМК), он ХОЧЕТ, чтобы ключ не сошёлся: именно
         *    несовпадение включает роспись. Правдоподобные адреса и суммы
         *    заставили бы чистый барабан выглядеть размеченным и пропустить
         *    роспись.
         *
         * Сверка при чтении (ПРБ3/ПРБ4, генс.bemsh:986) идёт по чётным СС
         * (физ. адрес сектора, СБР БТС) и нечётным (12-разр. контр. сумма);
         * ГЕНС пишет их сам, а барабан обязан лишь вернуть их буквально.
         * Единственное, что обязано быть корректным в пустой зоне, — ТЕГИ,
         * иначе чтение упадёт в контроль числа (КЧ).
         */
        for (i = 0; i < ZONE_WORDS; ++i)
            buf[(size_t)i * DRUM_WORD_BYTES + 8] = TAG_INSN48;

        if (u->dptr->dctrl & DEB_OPS)
            sim_debug(DEB_OPS, u->dptr,
                      "::: МБ зона %04o не записана — отдаю нули с тегами\n",
                      zone);
    }

    for (i = 0; i < ZONE_WORDS; ++i) {
        const uint8 *p = buf + (size_t)i * DRUM_WORD_BYTES;
        t_value word = 0;
        int addr, j;

        for (j = 0; j < 8; ++j)
            word |= (t_value)p[j] << (8*j);

        addr = mmu_iom_pa(i < ZONE_SERVICE_WORDS ?
                          sysaddr + i : memaddr + (i - ZONE_SERVICE_WORDS));
        if (addr < 0 || addr >= MEMSIZE)
            return SCPE_NXM;

        /* Слово и тег — буквально, без свёртки и без перевода тегов. */
        memory[addr] = word;
        tag[addr] = p[8];
    }
    return SCPE_OK;
}

/*
 * Запись зоны из памяти на барабан — точная обратная операция.
 */
static t_stat svs_drum_write(UNIT *u, int zone, int sysaddr, int memaddr)
{
    uint8 buf[ZONE_BYTES];
    int i;

    if (!(u->flags & UNIT_ATT))
        return SCPE_UNATT;
    if (u->flags & UNIT_RO)
        return SCPE_RO;

    memset(buf, 0, sizeof(buf));
    for (i = 0; i < ZONE_WORDS; ++i) {
        uint8 *p = buf + (size_t)i * DRUM_WORD_BYTES;
        t_value word;
        int addr, j;

        addr = mmu_iom_pa(i < ZONE_SERVICE_WORDS ?
                          sysaddr + i : memaddr + (i - ZONE_SERVICE_WORDS));
        if (addr < 0 || addr >= MEMSIZE)
            return SCPE_NXM;

        word = memory[addr];
        for (j = 0; j < 8; ++j)
            p[j] = (uint8)(word >> (8*j));
        p[8] = tag[addr];
    }

    if (u->dptr->dctrl & DEB_DAT)
        sim_debug(DEB_DAT, u->dptr, "::: запись МБ зона %04o СС@%05o данные@%05o\n",
                  zone, sysaddr, memaddr);

    if (fseek(u->fileref, (long)ZONE_BYTES * zone, SEEK_SET) != 0 ||
        sim_fwrite(buf, 1, ZONE_BYTES, u->fileref) != ZONE_BYTES) {
        return SCPE_IOERR;
    }
    return SCPE_OK;
}

/*
 * Обмен с барабаном по заявке ПВВ. Форма вызова та же, что у svs_disk_io():
 * sysaddr — база зоны (служебные слова), memaddr — адрес слов данных.
 */
t_stat svs_drum_io(int dev, int zone, int sysaddr, int memaddr, int is_write)
{
    UNIT *u;

    if (dev < 0 || dev >= NUM_DRUM_UNITS)
        return SCPE_NXDEV;
    if (zone < 0)
        return SCPE_NXM;
    u = &drum_unit[dev];

    controller.dev     = dev;
    controller.zone    = zone;
    controller.memory  = memaddr;
    controller.sysarea = sysaddr;

    return is_write ? svs_drum_write(u, zone, sysaddr, memaddr)
                    : svs_drum_read(u, zone, sysaddr, memaddr);
}
