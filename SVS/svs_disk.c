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

/*
 * Размер зоны на диске: 8 служебных слов + 1024 слова данных = 1 лист.
 */
#define ZONE_SIZE   (8 + 1024)              /* слов в зоне */
#define DISK_SIZE   (1024 * ZONE_SIZE)      /* слов на устройстве (1024 зоны) */

/*
 * Формат слова в образе диска (svs2053.bin и т.п.):
 *
 *   8 байт на слово, little-endian. 48-разрядное слово БЭСМ-6 лежит в МЛАДШИХ
 *   6 байтах; 7-й байт — тип слова: 1 = команда, 2 = число (данные); 8-й байт
 *   не используется. В памяти СВС слово хранится как 64-разрядное: 48 разрядов
 *   значения в старших разрядах (17..64), младшие 16 (РМР) = 0, а тип слова —
 *   в отдельном массиве тегов tag[] (035 = команда, 036 = число).
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

#define NUM_DISK_UNITS 8

UNIT disk_unit[NUM_DISK_UNITS] = {
    { UDATA(disk_event, UNIT_FIX+UNIT_ATTABLE+UNIT_ROABLE+UNIT_DISABLE, DISK_SIZE) },
    { UDATA(disk_event, UNIT_FIX+UNIT_ATTABLE+UNIT_ROABLE+UNIT_DISABLE, DISK_SIZE) },
    { UDATA(disk_event, UNIT_FIX+UNIT_ATTABLE+UNIT_ROABLE+UNIT_DISABLE, DISK_SIZE) },
    { UDATA(disk_event, UNIT_FIX+UNIT_ATTABLE+UNIT_ROABLE+UNIT_DISABLE, DISK_SIZE) },
    { UDATA(disk_event, UNIT_FIX+UNIT_ATTABLE+UNIT_ROABLE+UNIT_DISABLE, DISK_SIZE) },
    { UDATA(disk_event, UNIT_FIX+UNIT_ATTABLE+UNIT_ROABLE+UNIT_DISABLE, DISK_SIZE) },
    { UDATA(disk_event, UNIT_FIX+UNIT_ATTABLE+UNIT_ROABLE+UNIT_DISABLE, DISK_SIZE) },
    { UDATA(disk_event, UNIT_FIX+UNIT_ATTABLE+UNIT_ROABLE+UNIT_DISABLE, DISK_SIZE) },
};

static REG disk_reg[] = {
    { ORDATA(УСТР,  controller.dev,     3) },
    { ORDATA(ЗОНА,  controller.zone,   10) },
    { ORDATA(МОЗУ,  controller.memory, 20) },
    { ORDATA(СЛУЖ,  controller.sysarea,20) },
    { ORDATA(РС,    controller.status, 24) },
    { 0 }
};

static MTAB disk_mod[] = {
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

DEVICE disk_dev = {
    "DISK", disk_unit, disk_reg, disk_mod,
    NUM_DISK_UNITS, 8, 21, 1, 8, 50,
    NULL, NULL, &disk_reset, NULL, &disk_attach, &disk_detach,
    NULL, DEV_DISABLE | DEV_DEBUG, 0, disk_deb
};

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

static t_stat disk_attach(UNIT *u, CONST char *cptr)
{
    /* Образ диска существует заранее; принудительно требуем '-e'. */
    sim_switches |= SWMASK('E');
    return attach_unit(u, cptr);
}

static t_stat disk_detach(UNIT *u)
{
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
    t_value value48  = w & BITS48;          /* младшие 6 байт — значение */
    unsigned tagbyte = (w >> 48) & 0xFF;    /* 7-й байт — тип слова */

    memory[addr] = value48 << 16;           /* значение в разрядах 17..64, РМР=0 */
    tag[addr] = (tagbyte == DISK_TAG_INSN) ? TAG_INSN48 : TAG_NUMBER48;
}

/*
 * Обратное преобразование: слово ОЗУ + тег → слово образа диска.
 */
static t_value mem_to_disk_word(int addr)
{
    t_value value48  = (memory[addr] >> 16) & BITS48;
    unsigned tagbyte = (tag[addr] == TAG_INSN48) ? DISK_TAG_INSN : DISK_TAG_DATA;

    return value48 | ((t_value)tagbyte << 48);
}

/*
 * Чтение одной зоны (8 служебных + 1024 слова данных) с диска в ОЗУ.
 * Служебные слова кладутся по адресу sysaddr, данные — по адресу memaddr.
 * Возвращает SCPE_OK либо SCPE_IOERR.
 */
t_stat svs_disk_read(UNIT *u, int zone, int sysaddr, int memaddr)
{
    t_value buf[ZONE_SIZE];
    int i;

    if (!(u->flags & UNIT_ATT))
        return SCPE_UNATT;

    if (fseek(u->fileref, (long)ZONE_SIZE * zone * 8, SEEK_SET) != 0 ||
        sim_fread(buf, 8, ZONE_SIZE, u->fileref) != ZONE_SIZE) {
        return SCPE_IOERR;
    }

    if (u->dptr->dctrl & DEB_DAT)
        sim_debug(DEB_DAT, u->dptr, "::: чтение МД зона %04o СС@%05o данные@%05o\n",
                  zone, sysaddr, memaddr);

    for (i = 0; i < 8; ++i)
        disk_word_to_mem(buf[i], sysaddr + i);
    for (i = 0; i < 1024; ++i)
        disk_word_to_mem(buf[8 + i], memaddr + i);

    return SCPE_OK;
}

/*
 * Запись одной зоны (8 служебных + 1024 слова данных) из ОЗУ на диск.
 */
t_stat svs_disk_write(UNIT *u, int zone, int sysaddr, int memaddr)
{
    t_value buf[ZONE_SIZE];
    int i;

    if (!(u->flags & UNIT_ATT))
        return SCPE_UNATT;
    if (u->flags & UNIT_RO)
        return SCPE_RO;

    for (i = 0; i < 8; ++i)
        buf[i] = mem_to_disk_word(sysaddr + i);
    for (i = 0; i < 1024; ++i)
        buf[8 + i] = mem_to_disk_word(memaddr + i);

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
t_stat svs_disk_io(int dev, int zone, int sysaddr, int memaddr, int is_write)
{
    UNIT *u;

    if (dev < 0 || dev >= NUM_DISK_UNITS)
        return SCPE_NXDEV;
    u = &disk_unit[dev];

    controller.dev     = dev;
    controller.zone    = zone;
    controller.memory  = memaddr;
    controller.sysarea = sysaddr;

    return is_write ? svs_disk_write(u, zone, sysaddr, memaddr)
                    : svs_disk_read(u, zone, sysaddr, memaddr);
}
