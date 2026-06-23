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

    if (svs_trace >= TRACE_INSTRUCTIONS)
        fprintf(sim_log, "iom%d --- Команда БАКПВВ@%o: КОП=%03o, слово=%#jx\n",
            iom->index, iom->BAK, BAK_KOP(cmd), (intmax_t)cmd);

    /*
     * Управляющая команда (МКФ, ППД, сброс канала, реконфигурация) — без
     * передачи данных. Подтверждаем завершение, обнуляя оба слова блока БАКПВВ:
     * старшие 48 разр. слова 0 → опрос ПО ВЫХМКФ выходит; СЕМБИТ снимается, и
     * следующий захват канала (МКФ: И СЕМБИТ; ПО ВЗЯЛМК) проходит.
     * TODO (Phase 3): для ПУСКОБ обойти очередь ТОЧ и выполнить обмен данными.
     */
    memory[iom->BAK]     = 0;
    memory[iom->BAK + 1] = 0;
}
