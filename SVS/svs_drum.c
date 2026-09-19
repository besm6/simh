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
 * Зона барабана делится на ЧЕТЫРЕ сектора по 256 слов: сектор 3 — последняя
 * четверть зоны. Номер сектора АДАП кладёт в тот же физический адрес, что и
 * зону: РМР слова СПУ = зона*32 + сектор*8 (см. svs_iom.c, ветка МБ).
 * Замерено: полная заявка несёт РАЗМ=1024 и сектор 0, секторная — РАЗМ=256
 * и РМР 024530 = 0512*32 + 3*8.
 */
#define ZONE_SECTORS        4
#define SECTOR_WORDS        (ZONE_DATA_WORDS / ZONE_SECTORS)

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
 * Перенос одного слова между образом зоны и памятью.
 *
 * В образе слово лежит по индексу idx (служебные слова 0-7, дальше данные),
 * в памяти — по ВИРТУАЛЬНОМУ адресу addr, который переводится через
 * mmu_iom_data_pa(), как и в svs_disk.c. Слово и тег переносятся буквально,
 * без свёртки 4:3 и без перевода тегов.
 */
static t_stat drum_word_to_mem(const uint8 *buf, int idx, int addr)
{
    const uint8 *p = buf + (size_t)idx * DRUM_WORD_BYTES;
    t_value word = 0;
    int pa, j;

    pa = mmu_iom_data_pa(addr);
    if (pa < 0 || pa >= MEMSIZE)
        return SCPE_NXM;

    for (j = 0; j < 8; ++j)
        word |= (t_value)p[j] << (8*j);

    memory[pa] = word;
    tag[pa] = p[8];
    return SCPE_OK;
}

static t_stat drum_word_from_mem(uint8 *buf, int idx, int addr)
{
    uint8 *p = buf + (size_t)idx * DRUM_WORD_BYTES;
    t_value word;
    int pa, j;

    pa = mmu_iom_data_pa(addr);
    if (pa < 0 || pa >= MEMSIZE)
        return SCPE_NXM;

    word = memory[pa];
    for (j = 0; j < 8; ++j)
        p[j] = (uint8)(word >> (8*j));
    p[8] = tag[pa];
    return SCPE_OK;
}

/*
 * Чтение зоны барабана в память.
 *
 * Массив обмена ДО у барабана содержит ТОЛЬКО слова данных: они кладутся с
 * адреса memaddr, и первое же слово заявки — это слово данных, а не служебное.
 * Служебные слова зоны АДАП читает отдельной заявкой («чтение СС», разр.21
 * КУС, своя длина ДЛССД) и держит в области СС1Н (КНАПР КОНД А(СС1Н) в ЗКМБ),
 * поэтому в массив ДО они не попадают. У МД раскладка другая: там РАЗМ=784
 * покрывает и служебные слова (см. DISK_DATA_OFFSET в svs_disk.c).
 *
 * Адреса ВИРТУАЛЬНЫЕ (приходят из заявки), поэтому каждое обращение к памяти
 * идёт через mmu_iom_data_pa(), как и в svs_disk.c.
 *
 * Если зона за концом файла (ещё ни разу не записана), отдаём нули — барабан
 * чистый.
 */
static t_stat svs_drum_read(UNIT *u, int zone, int sector,
                            int memaddr, int nwords)
{
    int base = sector * SECTOR_WORDS;   /* начало сектора в словах данных */
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
        sim_debug(DEB_DAT, u->dptr, "::: чтение МБ зона %04o данные@%05o\n",
                  zone, memaddr);

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

    /*
     * Данные: заявка покрывает nwords слов, начиная с СЕКТОРА, а не с начала
     * зоны. Индекс в образе и адрес в памяти здесь расходятся — в этом вся
     * разница между полной и секторной заявкой.
     */
    for (i = 0; i < nwords; ++i) {
        if (drum_word_to_mem(buf, ZONE_SERVICE_WORDS + base + i,
                             memaddr + i) != SCPE_OK)
            return SCPE_NXM;
    }
    return SCPE_OK;
}

/*
 * Запись зоны из памяти на барабан — точная обратная операция.
 */
static t_stat svs_drum_write(UNIT *u, int zone, int sector,
                             int memaddr, int nwords)
{
    uint8 buf[ZONE_BYTES];
    int base = sector * SECTOR_WORDS;   /* начало сектора в словах данных */
    int i;

    if (!(u->flags & UNIT_ATT))
        return SCPE_UNATT;
    if (u->flags & UNIT_RO)
        return SCPE_RO;

    memset(buf, 0, sizeof(buf));
    /*
     * ЧАСТИЧНАЯ запись обязана сохранить всё, чего заявка не касается: зона
     * пишется целиком, и обнулять незатребованные слова нельзя. Поэтому при
     * неполном РАЗМ сначала подтягиваем зону с барабана, а потом перекрываем
     * только запрошенное. Незаписанной зоны может не быть — тогда нули.
     */
    if (base != 0 || nwords < ZONE_DATA_WORDS) {
        size_t got = 0;

        if (fseek(u->fileref, (long)ZONE_BYTES * zone, SEEK_SET) == 0)
            got = sim_fread(buf, 1, ZONE_BYTES, u->fileref);
        if (got < ZONE_BYTES)
            memset(buf + got, 0, ZONE_BYTES - got);
    }

    /* Слова вне сектора заявки сохраняются такими, какими были в зоне. */
    for (i = 0; i < nwords; ++i) {
        if (drum_word_from_mem(buf, ZONE_SERVICE_WORDS + base + i,
                               memaddr + i) != SCPE_OK)
            return SCPE_NXM;
    }

    if (u->dptr->dctrl & DEB_DAT)
        sim_debug(DEB_DAT, u->dptr, "::: запись МБ зона %04o данные@%05o\n",
                  zone, memaddr);

    if (fseek(u->fileref, (long)ZONE_BYTES * zone, SEEK_SET) != 0 ||
        sim_fwrite(buf, 1, ZONE_BYTES, u->fileref) != ZONE_BYTES) {
        return SCPE_IOERR;
    }
    return SCPE_OK;
}

/*
 * Обмен с барабаном по заявке ПВВ. В отличие от svs_disk_io(), отдельного
 * адреса служебных слов нет: массив ДО у барабана — это только данные.
 */
t_stat svs_drum_io(int dev, int zone, int sector,
                   int memaddr, int is_write, int nwords)
{
    UNIT *u;

    if (dev < 0 || dev >= NUM_DRUM_UNITS)
        return SCPE_NXDEV;
    if (zone < 0)
        return SCPE_NXM;
    /*
     * Сектор и РАЗМ обязаны укладываться в зону: иначе перенос вышел бы за
     * буфер образа. Это память эмулятора, а не гостя, — отвергаем заявку.
     */
    if (sector < 0 || sector >= ZONE_SECTORS ||
        nwords < 0 || sector * SECTOR_WORDS + nwords > ZONE_DATA_WORDS)
        return SCPE_NXM;
    u = &drum_unit[dev];

    controller.dev     = dev;
    controller.zone    = zone;
    controller.memory  = memaddr;
    controller.sysarea = 0;         /* у барабана служебные слова вне ДО */

    return is_write ? svs_drum_write(u, zone, sector, memaddr, nwords)
                    : svs_drum_read(u, zone, sector, memaddr, nwords);
}
