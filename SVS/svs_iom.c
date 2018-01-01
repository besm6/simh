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
 * Коды операций ПВВ.
 */
#define IOM_START_IO            001
#define IOM_SET_CHANNEL_BUSY    002
#define IOM_RESET_CHANNEL_BUSY  003
#define IOM_LOAD_HOME_ADDRESS   004
#define IOM_LOAD_UTAB_ADDRESS   005
#define IOM_LOAD_IOQ_ADDRESS    006
#define IOM_LOAD_SQ_ADDRESS     007
#define IOM_SCAN_OUT            010
#define IOM_SCAN_IN             011
#define IOM_SYNC_IO             012
#define IOM_GET_STATUS          013
#define IOM_INHIBIT             014
#define IOM_ACTIVATE            015
#define IOM_LOAD_DFO_FLAGS      016

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

    t_value request = memory[iom->HA];
    if (request == 0)
        return;

    /* Запрос выполнен. */
    memory[iom->HA] = 0;

    switch (request >> 42) {
    case IOM_START_IO:
    case IOM_SET_CHANNEL_BUSY:
    case IOM_RESET_CHANNEL_BUSY:
    case IOM_LOAD_HOME_ADDRESS:
        iom->HA = request & BITS(20);
        if (svs_trace >= TRACE_INSTRUCTIONS)
            fprintf(sim_log, "iom%d --- Установка базового адреса ПВВ: %#o\n",
                iom->index, iom->HA);
        break;
    case IOM_LOAD_UTAB_ADDRESS:
    case IOM_LOAD_IOQ_ADDRESS:
    case IOM_LOAD_SQ_ADDRESS:
    case IOM_SCAN_OUT:
    case IOM_SCAN_IN:
    case IOM_SYNC_IO:
    case IOM_GET_STATUS:
    case IOM_INHIBIT:
    case IOM_ACTIVATE:
    case IOM_LOAD_DFO_FLAGS:
    default:
        if (svs_trace >= TRACE_INSTRUCTIONS)
            fprintf(sim_log, "iom%d --- Неизвестный запрос к ПВВ: %#jx\n",
                iom->index, (intmax_t)request);
    }
}
