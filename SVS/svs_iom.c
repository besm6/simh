/*
 * SVS input/output module
 *
 * Copyright (c) 2017, Serge Vakulenko
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
 * Командное слово БАК (см. ПВВ.md §5.6).
 *
 * Процессор (АДАП) строит командное слово в адап.bemsh:2960-2965:
 *   адрес-с-точки-зрения-ПВВ блока БАКПВВ  ИЛИ  КОП,  затем СДА 64+16; ЗПП.
 * Из-за сдвига вправо на 16 перед ЗПП значащие 48 разрядов попадают в МЛАДШУЮ
 * половину 64-битного слова (старшие 16 разрядов нулевые), а не в старшую, как
 * при обычной записи аккумулятора. Слово кладётся в СТБАК (0100₈ = iom->HA),
 * после чего следует звонок в дверь: СЧ ЕПВВ; РЕГ '50' (адап.bemsh:2968-2969).
 *
 *   разряды 1..36   адрес 2-словного блока БАКПВВ с точки зрения ПВВ (физический;
 *                   ДФАПВВ прибавил базу АДРЕС). Здесь же лежат флаги СЕМБИТ
 *                   (разр.17) и ПОБ (разр.21); их выделение — в Phase 2.
 *   разряды 37..44  КОП — код операции (база М36, младший разряд = разряд 37)
 *
 * Пример (наблюдается при ВЫЗОС): слово 0x40000e8040 → КОП=004 (разр.39,
 * чтение/обмен), адрес блока = 0xe8040 = 3500100₈.
 */
#define BAK_BLOCK_ADDR(v)   ((uint32)((v) & 000777777777777LL)) /* разр.1..36: адрес блока БАКПВВ */
#define BAK_KOP(v)          ((int)(((v) >> 36) & 0377))         /* разр.37..44: поле КОП, база М36 */

#define BAK_KOP_EXCHANGE1   001     /* разряд 37 (=М36В'1') — вариант обмена */
#define BAK_KOP_EXCHANGE3   003     /* разряды 37-38 (=М36В'3') — вариант обмена */
#define BAK_KOP_RDEXCHANGE  004     /* разряд 39 (=М36В'4') — чтение/обмен */

/*
 * Перевод адреса "с точки зрения ПВВ" в физический адрес ОЗУ.
 *
 * Макрос ДФАПВВ (адап.bemsh:60) прибавляет к адресу константу АДРЕС, переводя
 * его в адресное пространство ПВВ. В реальной СВС ПВВ — отдельный процессор со
 * своим физическим адресным пространством; в симуляторе ОЗУ одно (массив
 * memory[]), поэтому адрес "с т.з. ПВВ" нужно перевести обратно вычитанием
 * АДРЕС. АДРЕС — константа компоновки модуля АДАП и равна разности адресов
 * блока БАКПВВ в двух представлениях: 3500100₈ (ПВВ) − 030100₈ (ОЗУ).
 *
 * Замечание: рабочая конфигурация (SVS/dispak.ini) держит приписку страниц
 * данных ПВВ единичной (virtual == physical), поэтому одного вычитания АДРЕС
 * достаточно для всех структур ПВВ (БАКПВВ, ТОЧ, ТУС, заявки).
 */
#define IOM_ADRES           03450000                /* база ДФАПВВ (3500100₈-030100₈) */
#define IOM_PHYS(a)         ((uint32)((a) - IOM_ADRES))

IOMDATA iom_data[4];        /* состояние ПВВ */

/*
 * Событие.
 */
static t_stat iom_event(UNIT *u)
{
    //TODO
    return SCPE_OK;
}

/*
 * IOM data structures
 *
 * iom_dev     IOM device descriptor
 * iom_unit    IOM unit descriptor
 * iom_reg     IOM register list
 */
static UNIT iom_unit[4] = {
    { UDATA (iom_event, UNIT_FIX+UNIT_ATTABLE, 0) },
    { UDATA (iom_event, UNIT_FIX+UNIT_ATTABLE, 0) },
    { UDATA (iom_event, UNIT_FIX+UNIT_ATTABLE, 0) },
    { UDATA (iom_event, UNIT_FIX+UNIT_ATTABLE, 0) },
};

static REG iom0_reg[] = {
    { ORDATA   (HA,     iom_data[0].HA,   20) },
    { ORDATA   (UTA,    iom_data[0].UTA,  20) },
    { ORDATA   (IOQA,   iom_data[0].IOQA, 20) },
    { ORDATA   (SQA,    iom_data[0].SQA,  20) },
    { 0 }
};

static MTAB iom_mod[] = {
    { 0 }
};

/*
 * Reset routine
 */
static t_stat iom_dev_reset(DEVICE *dptr)
{
    //TODO
    //sim_cancel(u);
    return SCPE_OK;
}

DEVICE iom_dev[4] = {
    { "IOM0", &iom_unit[0], iom0_reg, iom_mod,
      1, 8, 19, 1, 8, 50,
      NULL, NULL, &iom_dev_reset,
      NULL, NULL, NULL,
      (void*)&iom_data[0], DEV_DISABLE | DEV_DEBUG },
    { "IOM1", &iom_unit[1], iom0_reg, iom_mod,
      1, 8, 19, 1, 8, 50,
      NULL, NULL, &iom_dev_reset,
      NULL, NULL, NULL,
      (void*)&iom_data[1], DEV_DISABLE | DEV_DEBUG },
    { "IOM2", &iom_unit[2], iom0_reg, iom_mod,
      1, 8, 19, 1, 8, 50,
      NULL, NULL, &iom_dev_reset,
      NULL, NULL, NULL,
      (void*)&iom_data[2], DEV_DISABLE | DEV_DEBUG },
    { "IOM3", &iom_unit[3], iom0_reg, iom_mod,
      1, 8, 19, 1, 8, 50,
      NULL, NULL, &iom_dev_reset,
      NULL, NULL, NULL,
      (void*)&iom_data[3], DEV_DISABLE | DEV_DEBUG },
};

/*
 * Сброс ПВВ.
 */
void iom_reset(int index)
{
    IOMDATA *iom = &iom_data[index];

    iom->index = index;
    iom->HA = 0100;
    iom->BAK = 0;
    iom->UTA = 0;
    iom->IOQA = 0;
    iom->SQA = 0;
    if (svs_trace >= TRACE_INSTRUCTIONS)
        fprintf(sim_log, "iom%d --- Сброс ПВВ\n", iom->index);
}

/*
 * КОП командного слова БАКПВВ (база М36, разр.37..44; см. §5.6 и iom-лог).
 * Значения наблюдались в прогоне dispak.ini:
 *   004 — инициализация БАК (указатель на БАКПВВ), обрабатывается через СТБАК;
 *   005/006/007 — регистрация управляющих таблиц ПВВ (адрес в разр.1..36):
 *                 005→УТ (таблица устройств), 006→ТОЧ (очереди), 007→таблица ответов.
 *                 Адреса определены эмпирически: КОП=006 несёт A(ТОЧ)=30540
 *                 (совпадает с АТОЧ из адап.bemsh, D31577).
 *   015 — управляющая команда без адреса;
 *   016 — ПУСКОБ: обойти ТОЧ и выполнить обмен (см. iom_pusk_obmen).
 */
#define BAK_KOP_REG_UT      0005    /* регистрация таблицы устройств  → UTA  */
#define BAK_KOP_REG_TOCH    0006    /* регистрация таблицы очередей ТОЧ → IOQA */
#define BAK_KOP_REG_ANSW    0007    /* регистрация таблицы ответов    → SQA  */
#define BAK_KOP_PUSKOB      0016    /* ПУСКОБ: пуск обмена по очереди ТОЧ      */

/*
 * Маска значащего адреса в слове-указателе ТОЧ / звене заявки.
 * Звенья связаны через 0-е слово заявки; в старших разрядах указателя стоит
 * СЕМБИТ (разр.17 = 0o200000, семафорный/занятости бит, адап.bemsh: СЕМБИТ КОНД
 * М16В'1'). Аналог микрокоманды ДАЙМА — выделить адрес, отбросив признаки.
 */
#define IOM_DAIMA(v)        ((uint32)((v) & 077777))   /* разр.1..15: адрес заявки */
#define IOM_SEMBIT          000200000LL                /* разр.17: СЕМБИТ */

/*
 * ТВЗП — таблица заявок, отданных СВС (per-СВС), куда ВКЛТОЧ кладёт заявку по
 * ветви ОТДАЮЗ (НУС=0): СЧИМ АЗАЯВ; ИЛИ СЕМБИТ; ЗПП (НСВС) (адап.bemsh:3680).
 * В конфигурации dispak.ini (НАПРУС=0, один СВС, НСВС=0) слот ТВЗП[0] оказывается
 * по физическому адресу 030540+... — определён эмпирически как 031270. Это не
 * зарегистрированная каналом структура, поэтому адрес зашит здесь как константа
 * (TODO: получать ТВЗП из таблицы устройств или регистрировать командой БАК).
 */
#define IOM_TVZP            031270      /* ТВЗП[НСВС=0]: слот отданной заявки */

/*
 * Выполнить обмен по одной заявке (§5.2/§5.4):
 *   сл.2 ДО — буфер ОП и длина; сл.3 СО — направление (ТЕГ48 чтение = разр.48);
 *   сл.4 СПУ — физический адрес зоны. Ответ устройства — в сл.5/6 (ДР/ДРУ).
 * dev — номер устройства МД (из НАПРУС; здесь 0 = канал 0, устройство 0).
 */
static void iom_xfer_zaiavka(IOMDATA *iom, uint32 z, int dev)
{
    /* Слова данных хранятся как (значение48<<16)|тег: значащие 48 разр. — в
     * СТАРШЕЙ части 64-битного слова, поэтому извлекаем сдвигом вправо на 16
     * (так же показывает их команда `e`). Ср. командные слова БАК, где значащие
     * разряды лежат в МЛАДШЕЙ половине. */
    t_value do_  = (memory[z + 2] >> 16) & BITS48;  /* ДО  */
    t_value so   = (memory[z + 3] >> 16) & BITS48;  /* СО  */
    t_value spu  = (memory[z + 4] >> 16) & BITS48;  /* СПУ */
    int is_read  = (int)((so >> 47) & 1);       /* разр.48: ТЕГ48 чтение */
    /*
     * Физический адрес зоны — в РМР слова СПУ (младшие 16 разр. 64-битного слова,
     * §5.4: "копия младших 16 разр. адреса в РМР"). При типе ёмкости 1 (29 МБ,
     * ТУСЗ разр.28) зона записывается «как есть», поэтому берём РМР напрямую.
     */
    int zone     = (int)(memory[z + 4] & 0xFFFF);
    int memaddr  = (int)(do_ & 077777);         /* адрес буфера данных ОП из ДО */
    t_stat r;
    (void)spu;

    if (svs_trace >= TRACE_INSTRUCTIONS)
        fprintf(sim_log,
            "iom%d --- обмен: устр=%d заявка@%o %s зона=%o буфер=%o\n"
            "iom%d ---   ДО=%016jo(РМР %06o) СО=%016jo СПУ=%016jo(РМР %06o)\n",
            iom->index, dev, z, is_read ? "ЧТ" : "ЗП", zone, memaddr,
            iom->index, (uintmax_t)do_, (unsigned)(memory[z+2] & 0xFFFF),
            (uintmax_t)so, (uintmax_t)spu, (unsigned)(memory[z+4] & 0xFFFF));

    /* Зона = 8 служебных слов (в буфер) + 1024 слова данных (буфер+8). */
    r = svs_disk_io(dev, zone, memaddr, memaddr + 8, !is_read);

    if (svs_trace >= TRACE_INSTRUCTIONS)
        fprintf(sim_log, "iom%d ---   svs_disk_io → %s\n",
            iom->index, (r == SCPE_OK) ? "OK" : "ОШИБКА");

    /* Ответ устройства в сл.5/6 (ДР/ДРУ), младшие 16 разр.; 0 = успех. */
    memory[z + 5] = (r == SCPE_OK) ? 0 : 1;
    memory[z + 6] = 0;
}

/*
 * ПУСКОБ (КОП=016): обойти очередь ТОЧ и выполнить отложенный обмен.
 *
 * ТОЧ (адрес в iom->IOQA, зарегистрирован командой КОП=006) — таблица очередей,
 * по 2 слова на устройство (НОЧ — начало, КОЧ — конец списка заявок); индекс =
 * 2·НУС (§5.2, КАНСОВ адап.bemsh:2153). Заявка — 8-словный блок (§5.2):
 *   сл.2 ДО  — дескриптор данных (адрес буфера ОП + длина),
 *   сл.3 СО  — управление: ТЕГ48 чтение = разр.39,40,46,48; запись = разр.39,40,
 *   сл.4 СПУ — физический адрес зоны на диске,
 *   сл.5/6 ДР/ДРУ — ответ устройства (заполняем по завершении, младшие 16 разр.),
 *   сл.0 — связь со следующей заявкой (ДАЙМА выделяет адрес).
 *
 * Замечание о конфигурации: в текущем dispak.ini системный диск приходит в
 * ВКЛТОЧ с НУС=0 и уходит по ветви ОТДАЮЗ (заявка кладётся в ТВЗП[НСВС], а не в
 * ТОЧ), поэтому ТОЧ при КОП=016 пуста и этот обход — пока холостой. Чтобы обмен
 * пошёл через ТОЧ, нужна настройка таблиц устройств (НУС≠0) и подключённый диск.
 */
static void iom_pusk_obmen(IOMDATA *iom)
{
    uint32 toch = iom->IOQA;
    int nus, found = 0;

    if (toch == 0) {
        if (svs_trace >= TRACE_INSTRUCTIONS)
            fprintf(sim_log, "iom%d --- ПУСКОБ: ТОЧ не зарегистрирована\n", iom->index);
        return;
    }

    /* По 2 слова на устройство; индекс = 2·НУС, НУС до ДТУС=ККАН*16=112 (адап.bemsh:391).
     * Указатели — в старшей части слова (значение<<16), как и поля заявки. */
    for (nus = 0; nus < 112; nus++) {
        t_value head = (memory[toch + 2*nus] >> 16) & BITS48;
        uint32 z = IOM_DAIMA(head);

        if (head == 0)
            continue;

        /* Идём по цепочке заявок (связь — 0-е слово). dev=0: единственный МД на DISK0
         * (канал из ТУСЗ[НУС] разр.33:38 здесь 0; TODO: извлекать для многодисковой конфиг.). */
        while (z != 0) {
            uint32 next = IOM_DAIMA((memory[z] >> 16) & BITS48);  /* следующая заявка */
            found++;
            iom_xfer_zaiavka(iom, z, 0);
            z = next;
        }

        /* Очередь обработана — гасим НОЧ/КОЧ. */
        memory[toch + 2*nus]     = 0;
        memory[toch + 2*nus + 1] = 0;
    }

    /* Завершение обмена: внешнее прерывание ПРПВВ исходному СВС (ГРВП разр.3). */
    if (found) {
        cpu_core[0].GRVP |= GRVP_INTR_IOM;
        if (svs_trace >= TRACE_INSTRUCTIONS)
            fprintf(sim_log, "iom%d --- ПУСКОБ: обработано заявок %d, ПРПВВ\n",
                iom->index, found);
    } else if (svs_trace >= TRACE_INSTRUCTIONS) {
        fprintf(sim_log, "iom%d --- ПУСКОБ: ТОЧ@%o пуста\n", iom->index, toch);
    }
}

/*
 * Обслуживание отданной заявки из ТВЗП (ветвь ОТДАЮЗ, НУС=0).
 *
 * Вызывается на звонке РЕГ '50' с признаком ЕСВС ("Запись в ПП"), когда АДАП
 * только что положил заявку в ТВЗП[НСВС] (СЕМБИТ) и сигналит СВС. В реальной СВС
 * заявку дальше обрабатывает монитор СВС; в автономном АДАП этого монитора нет,
 * поэтому ПВВ-канал выполняет обмен сам: читает заявку, переносит данные,
 * проставляет ответ ДР/ДРУ и посылает завершение ПРПВВ.
 */
void iom_service_tvzp(int index)
{
    IOMDATA *iom = &iom_data[index];
    t_value parked = (memory[IOM_TVZP] >> 16) & BITS48;  /* данные в старшей части */
    uint32 z;

    if (!(parked & IOM_SEMBIT))
        return;                             /* заявки в ТВЗП нет */

    z = IOM_DAIMA(parked);
    if (svs_trace >= TRACE_INSTRUCTIONS)
        fprintf(sim_log, "iom%d --- ЕСВС: заявка из ТВЗП@%o → @%o\n",
            iom->index, IOM_TVZP, z);

    iom_xfer_zaiavka(iom, z, 0);            /* НАПРУС=0 → устройство 0 */

    /* Снимаем СЕМБИТ в слоте ТВЗП — канал освободил заявку (обмен завершён).
     * Данные слова — в старшей части (значение48<<16), поэтому разр.17 значения
     * соответствует разр.33 64-битного слова. */
    memory[IOM_TVZP] &= ~((t_value)IOM_SEMBIT << 16);

    /* Завершение: ПРПВВ исходному СВС (ГРВП разр.3). */
    cpu_core[0].GRVP |= GRVP_INTR_IOM;
}

/*
 * Запрос от процессора через регистр ПП.
 */
void iom_request(int index)
{
    IOMDATA *iom = &iom_data[index];

    /*
     * Звонок в дверь не несёт адреса. Сначала смотрим в СТБАК (= iom->HA, 0100₈):
     * при инициализации процессор кладёт туда командное слово БАК — указатель на
     * блок БАКПВВ (адрес блока в разр.1..36, КОП в разр.37..44). Значащие 48
     * разрядов — в МЛАДШЕЙ части 64-битного слова (см. описание формата выше).
     */
    t_value ptr = memory[iom->HA] & BITS48;
    if (ptr != 0) {
        int kop = BAK_KOP(ptr);
        iom->BAK = IOM_PHYS(BAK_BLOCK_ADDR(ptr));

        if (svs_trace >= TRACE_INSTRUCTIONS)
            fprintf(sim_log, "iom%d --- Инициализация БАК: КОП=%03o, БАКПВВ@%o\n",
                iom->index, kop, iom->BAK);

        /*
         * Подтверждение: гасим СТБАК. Цикл ожидания в процессоре
         * (адап.bemsh:2971: СЧП СТБАК; ПО ДОЖДЛ) выходит, когда старшие 48
         * разрядов слова становятся нулевыми.
         */
        memory[iom->HA] = 0;
        return;
    }

    /*
     * Иначе команда лежит в 2-словном блоке БАКПВВ по запомненному адресу
     * (ЗАПУСК/ПУСКОБ: ЗППР БАКПВВ; РЕГ '50'; затем опрос СЧПР БАКПВВ; ПО ВЫХМКФ,
     * адап.bemsh:3548-3556, 3830-3841).
     */
    if (iom->BAK == 0)
        return;
    t_value cmd = memory[iom->BAK] & BITS48;
    if (cmd == 0)
        return;

    {
        int kop = BAK_KOP(cmd);

        if (svs_trace >= TRACE_INSTRUCTIONS)
            fprintf(sim_log, "iom%d --- Команда БАКПВВ@%o: КОП=%03o, слово=%#jx\n",
                iom->index, iom->BAK, kop, (intmax_t)cmd);

        switch (kop) {
        case BAK_KOP_REG_UT:    /* регистрация таблицы устройств */
            iom->UTA = IOM_PHYS(BAK_BLOCK_ADDR(cmd));
            if (svs_trace >= TRACE_INSTRUCTIONS)
                fprintf(sim_log, "iom%d ---   УТ@%o\n", iom->index, iom->UTA);
            break;

        case BAK_KOP_REG_TOCH:  /* регистрация таблицы очередей ТОЧ */
            iom->IOQA = IOM_PHYS(BAK_BLOCK_ADDR(cmd));
            if (svs_trace >= TRACE_INSTRUCTIONS)
                fprintf(sim_log, "iom%d ---   ТОЧ@%o\n", iom->index, iom->IOQA);
            break;

        case BAK_KOP_REG_ANSW:  /* регистрация таблицы ответов */
            iom->SQA = IOM_PHYS(BAK_BLOCK_ADDR(cmd));
            if (svs_trace >= TRACE_INSTRUCTIONS)
                fprintf(sim_log, "iom%d ---   ОТВ@%o\n", iom->index, iom->SQA);
            break;

        case BAK_KOP_PUSKOB:    /* ПУСКОБ: пуск обмена по очереди ТОЧ */
            iom_pusk_obmen(iom);
            break;

        default:
            /*
             * Прочие управляющие команды (МКФ, ППД, сброс канала, реконфигурация) —
             * без передачи данных, только подтверждаем.
             */
            break;
        }
    }

    /*
     * Подтверждение завершения: обнуляем оба слова блока БАКПВВ. Старшие 48 разр.
     * слова 0 → опрос процессора (ПО ВЫХМКФ) выходит; СЕМБИТ снимается, и
     * следующий захват канала (МКФ: И СЕМБИТ; ПО ВЗЯЛМК) проходит.
     */
    memory[iom->BAK]     = 0;
    memory[iom->BAK + 1] = 0;
}
