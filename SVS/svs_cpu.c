/*
 * SVS CPU simulator.
 *
 * Copyright (c) 1997-2009, Leonid Broukhis
 * Copyright (c) 2009-2017, Serge Vakulenko
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
#include "svs_defs.h"
#include <math.h>
#include <float.h>
#include <time.h>

t_value memory[MEMSIZE];                /* physical memory */

CORE cpu_core[10];                      /* state of all processors */

int32 tmr_poll = CLK_DELAY;             /* pgm timer poll */

TRACEMODE svs_trace;                    /* trace mode */

extern const char *scp_errors[];

/* Wired (non-registered) bits of interrupt registers (GRP and PRP)
 * cannot be cleared by writing to the GRP and must be cleared by clearing
 * the registers generating the corresponding interrupts.
 */
#define GRP_WIRED_BITS (GRP_DRUM1_FREE | GRP_DRUM2_FREE |\
                        GRP_CHAN3_DONE | GRP_CHAN4_DONE |\
                        GRP_CHAN5_DONE | GRP_CHAN6_DONE |\
                        GRP_CHAN3_FREE | GRP_CHAN4_FREE |\
                        GRP_CHAN5_FREE | GRP_CHAN6_FREE |\
                        GRP_CHAN7_FREE )

#define PRP_WIRED_BITS (PRP_UVVK1_END | PRP_UVVK2_END |\
                        PRP_PCARD1_PUNCH | PRP_PCARD2_PUNCH |\
                        PRP_PTAPE1_PUNCH | PRP_PTAPE2_PUNCH )

t_stat cpu_examine(t_value *vptr, t_addr addr, UNIT *uptr, int32 sw);
t_stat cpu_deposit(t_value val, t_addr addr, UNIT *uptr, int32 sw);
t_stat cpu_reset(DEVICE *dev);
t_stat cpu_req(UNIT *u, int32 val, CONST char *cptr, void *desc);
t_stat cpu_set_pult(UNIT *u, int32 val, CONST char *cptr, void *desc);
t_stat cpu_show_pult(FILE *st, UNIT *up, int32 v, CONST void *dp);
t_stat cpu_set_trace(UNIT *u, int32 val, CONST char *cptr, void *desc);
t_stat cpu_set_itrace(UNIT *u, int32 val, CONST char *cptr, void *desc);
t_stat cpu_set_etrace(UNIT *u, int32 val, CONST char *cptr, void *desc);
t_stat cpu_show_trace(FILE *st, UNIT *up, int32 v, CONST void *dp);
t_stat cpu_clr_trace(UNIT *uptr, int32 val, CONST char *cptr, void *desc);

/*
 * CPU data structures
 *
 * cpu_dev[]    CPU device descriptors
 * cpu_unit[]   CPU unit descriptors
 * cpu0_reg     CPU register list
 * cpu_mod      CPU modifiers list
 */

UNIT cpu_unit[10] = {
    { UDATA(NULL, UNIT_FIX, MEMSIZE) },
    { UDATA(NULL, UNIT_FIX, MEMSIZE) },
    { UDATA(NULL, UNIT_FIX, MEMSIZE) },
    { UDATA(NULL, UNIT_FIX, MEMSIZE) },
    { UDATA(NULL, UNIT_FIX, MEMSIZE) },
    { UDATA(NULL, UNIT_FIX, MEMSIZE) },
    { UDATA(NULL, UNIT_FIX, MEMSIZE) },
    { UDATA(NULL, UNIT_FIX, MEMSIZE) },
    { UDATA(NULL, UNIT_FIX, MEMSIZE) },
    { UDATA(NULL, UNIT_FIX, MEMSIZE) },
};

#define ORDATAVM(nm,loc,wd) REGDATA(nm,(loc),8,wd,0,1,NULL,NULL,REG_VMIO,0,0)

REG cpu0_reg[] = {
    { ORDATA   ( "PC",    cpu_core[0].PC,       15) },  /* program counter */
    { ORDATA   ( "RK",    cpu_core[0].RK,       24) },  /* instruction register */
    { ORDATA   ( "Aex",   cpu_core[0].Aex,      15) },  /* effective address */
    { ORDATAVM ( "ACC",   cpu_core[0].ACC,      48) },  /* accumulator */
    { ORDATAVM ( "RMR",   cpu_core[0].RMR,      48) },  /* LSB register */
    { BINRDATA ( "RAU",   cpu_core[0].RAU,       6) },  /* ALU modes */
    { ORDATA   ( "M1",    cpu_core[0].M[1],     15) },  /* index (modifier) registers */
    { ORDATA   ( "M2",    cpu_core[0].M[2],     15) },
    { ORDATA   ( "M3",    cpu_core[0].M[3],     15) },
    { ORDATA   ( "M4",    cpu_core[0].M[4],     15) },
    { ORDATA   ( "M5",    cpu_core[0].M[5],     15) },
    { ORDATA   ( "M6",    cpu_core[0].M[6],     15) },
    { ORDATA   ( "M7",    cpu_core[0].M[7],     15) },
    { ORDATA   ( "M10",   cpu_core[0].M[010],   15) },
    { ORDATA   ( "M11",   cpu_core[0].M[011],   15) },
    { ORDATA   ( "M12",   cpu_core[0].M[012],   15) },
    { ORDATA   ( "M13",   cpu_core[0].M[013],   15) },
    { ORDATA   ( "M14",   cpu_core[0].M[014],   15) },
    { ORDATA   ( "M15",   cpu_core[0].M[015],   15) },
    { ORDATA   ( "M16",   cpu_core[0].M[016],   15) },
    { ORDATA   ( "M17",   cpu_core[0].M[017],   15) },  /* also the stack pointer */
    { ORDATA   ( "M20",   cpu_core[0].M[020],   15) },  /* MOD - address modifier register */
    { ORDATA   ( "M21",   cpu_core[0].M[021],   15) },  /* PSW - CU modes */
    { ORDATA   ( "M27",   cpu_core[0].M[027],   15) },  /* SPSW - saved CU modes */
    { ORDATA   ( "M32",   cpu_core[0].M[032],   15) },  /* ERET - extracode return address */
    { ORDATA   ( "M33",   cpu_core[0].M[033],   15) },  /* IRET - interrupt return address */
    { ORDATA   ( "M34",   cpu_core[0].M[034],   16) },  /* IBP - instruction bkpt address */
    { ORDATA   ( "M35",   cpu_core[0].M[035],   16) },  /* DWP - watchpoint address */
    { BINRDATA ( "RUU",   cpu_core[0].RUU,       9) },  /* execution modes  */
    { ORDATAVM ( "GRP",   cpu_core[0].GRP,      48) },  /* main interrupt reg */
    { ORDATAVM ( "MGRP",  cpu_core[0].MGRP,     48) },  /* mask of the above  */
    { ORDATA   ( "PRP",   cpu_core[0].PRP,      24) },  /* peripheral interrupt reg */
    { ORDATA   ( "MPRP",  cpu_core[0].MPRP,     24) },  /* mask of the above*/
    { ORDATAVM ( "RP0",   cpu_core[0].RP[0],    48) },
    { ORDATAVM ( "RP1",   cpu_core[0].RP[1],    48) },
    { ORDATAVM ( "RP2",   cpu_core[0].RP[2],    48) },
    { ORDATAVM ( "RP3",   cpu_core[0].RP[3],    48) },
    { ORDATAVM ( "RP4",   cpu_core[0].RP[4],    48) },
    { ORDATAVM ( "RP5",   cpu_core[0].RP[5],    48) },
    { ORDATAVM ( "RP6",   cpu_core[0].RP[6],    48) },
    { ORDATAVM ( "RP7",   cpu_core[0].RP[7],    48) },
    { ORDATA   ( "RZ",    cpu_core[0].RZ,       32) },
    { ORDATAVM ( "FP1",   cpu_core[0].pult[1],  50) },
    { ORDATAVM ( "FP2",   cpu_core[0].pult[2],  50) },
    { ORDATAVM ( "FP3",   cpu_core[0].pult[3],  50) },
    { ORDATAVM ( "FP4",   cpu_core[0].pult[4],  50) },
    { ORDATAVM ( "FP5",   cpu_core[0].pult[5],  50) },
    { ORDATAVM ( "FP6",   cpu_core[0].pult[6],  50) },
    { ORDATAVM ( "FP7",   cpu_core[0].pult[7],  50) },
    { 0 }
};

MTAB cpu_mod[] = {
    { MTAB_XTD|MTAB_VDV,
        0, "IDLE",  "IDLE",     &sim_set_idle,      &sim_show_idle,     NULL,
                                "Enables idle detection mode" },
    { MTAB_XTD|MTAB_VDV,
        0, NULL,    "NOIDLE",   &sim_clr_idle,      NULL,               NULL,
                                "Disables idle detection" },
    { MTAB_XTD|MTAB_VDV,
        0, NULL,    "REQ",      &cpu_req,           NULL,               NULL,
                                "Sends a request interrupt" },
    { MTAB_XTD|MTAB_VDV|MTAB_VALO,
        0, "PULT",  "PULT",     &cpu_set_pult,      &cpu_show_pult,     NULL,
                                "Selects a hardwired program or switch reg." },
    { MTAB_XTD|MTAB_VDV,
        0, "TRACE", "TRACE",    &cpu_set_trace,     &cpu_show_trace,    NULL,
                                "Enables full tracing of processor state" },
    { MTAB_XTD|MTAB_VDV,
        0, NULL,    "ITRACE",   &cpu_set_itrace,    NULL,               NULL,
                                "Enables instruction tracing" },
    { MTAB_XTD|MTAB_VDV,
        0, NULL,    "ETRACE",   &cpu_set_etrace,    NULL,               NULL,
                                "Enables extracode only tracing" },
    { MTAB_XTD|MTAB_VDV,
        0, NULL,    "NOTRACE",  &cpu_clr_trace,     NULL,               NULL,
                                "Disables tracing" },

    //TODO: Разрешить/запретить контроль числа.
    //{ 2, 0, "NOCHECK", "NOCHECK" },
    //{ 2, 2, "CHECK",   "CHECK" },
    { 0 }
};

DEVICE cpu_dev[10] = {
    { "CPU0", &cpu_unit[0], cpu0_reg, cpu_mod,
      1, 8, 17, 1, 8, 50,
      &cpu_examine, &cpu_deposit, &cpu_reset,
      NULL, NULL, NULL, (void*)&cpu_core[0], DEV_DEBUG },
    { "CPU1", &cpu_unit[1], NULL, cpu_mod,
      1, 8, 17, 1, 8, 50,
      &cpu_examine, &cpu_deposit, &cpu_reset,
      NULL, NULL, NULL, (void*)&cpu_core[1], DEV_DEBUG },
    { "CPU2", &cpu_unit[2], NULL, cpu_mod,
      1, 8, 17, 1, 8, 50,
      &cpu_examine, &cpu_deposit, &cpu_reset,
      NULL, NULL, NULL, (void*)&cpu_core[2], DEV_DEBUG },
    { "CPU3", &cpu_unit[3], NULL, cpu_mod,
      1, 8, 17, 1, 8, 50,
      &cpu_examine, &cpu_deposit, &cpu_reset,
      NULL, NULL, NULL, (void*)&cpu_core[3], DEV_DEBUG },
    { "CPU4", &cpu_unit[4], NULL, cpu_mod,
      1, 8, 17, 1, 8, 50,
      &cpu_examine, &cpu_deposit, &cpu_reset,
      NULL, NULL, NULL, (void*)&cpu_core[4], DEV_DEBUG },
    { "CPU5", &cpu_unit[5], NULL, cpu_mod,
      1, 8, 17, 1, 8, 50,
      &cpu_examine, &cpu_deposit, &cpu_reset,
      NULL, NULL, NULL, (void*)&cpu_core[5], DEV_DEBUG },
    { "CPU6", &cpu_unit[6], NULL, cpu_mod,
      1, 8, 17, 1, 8, 50,
      &cpu_examine, &cpu_deposit, &cpu_reset,
      NULL, NULL, NULL, (void*)&cpu_core[6], DEV_DEBUG },
    { "CPU7", &cpu_unit[7], NULL, cpu_mod,
      1, 8, 17, 1, 8, 50,
      &cpu_examine, &cpu_deposit, &cpu_reset,
      NULL, NULL, NULL, (void*)&cpu_core[7], DEV_DEBUG },
    { "CPU8", &cpu_unit[8], NULL, cpu_mod,
      1, 8, 17, 1, 8, 50,
      &cpu_examine, &cpu_deposit, &cpu_reset,
      NULL, NULL, NULL, (void*)&cpu_core[8], DEV_DEBUG },
    { "CPU9", &cpu_unit[9], NULL, cpu_mod,
      1, 8, 17, 1, 8, 50,
      &cpu_examine, &cpu_deposit, &cpu_reset,
      NULL, NULL, NULL, (void*)&cpu_core[9], DEV_DEBUG },
};

/*
 * SCP data structures and interface routines
 *
 * sim_name             simulator name string
 * sim_PC               pointer to saved PC register descriptor
 * sim_emax             maximum number of words for examine/deposit
 * sim_devices          array of pointers to simulated devices
 * sim_stop_messages    array of pointers to stop messages
 * sim_load             binary loader
 */

char sim_name[] = "СВС";

REG *sim_PC = &cpu0_reg[0];

int32 sim_emax = 1;     /* max number of addressable units per instruction */

DEVICE *sim_devices[] = {
    &cpu_dev[0],        /* процессоры */
#if NUM_CORES > 1
    &cpu_dev[1],
#endif
#if NUM_CORES > 2
    &cpu_dev[2],
#endif
#if NUM_CORES > 3
    &cpu_dev[3],
#endif
#if NUM_CORES > 4
    &cpu_dev[4],
#endif
#if NUM_CORES > 5
    &cpu_dev[5],
#endif
#if NUM_CORES > 6
    &cpu_dev[6],
#endif
#if NUM_CORES > 7
    &cpu_dev[7],
#endif
#if NUM_CORES > 8
    &cpu_dev[8],
#endif
#if NUM_CORES > 9
    &cpu_dev[9],
#endif

    &clock_dev,         /* таймер */

    &tty_dev,           /* терминалы */
    0
};

const char *sim_stop_messages[] = {
    "Неизвестная ошибка",                 /* Unknown error */
    "Останов",                            /* STOP */
    "Точка останова",                     /* Emulator breakpoint */
    "Точка останова по считыванию",       /* Emulator read watchpoint */
    "Точка останова по записи",           /* Emulator write watchpoint */
    "Выход за пределы памяти",            /* Run out end of memory */
    "Запрещенная команда",                /* Invalid instruction */
    "Контроль команды",                   /* A data-tagged word fetched */
    "Команда в чужом листе",              /* Paging error during fetch */
    "Число в чужом листе",                /* Paging error during load/store */
    "Контроль числа МОЗУ",                /* RAM parity error */
    "Контроль числа БРЗ",                 /* Write cache parity error */
    "Переполнение АУ",                    /* Arith. overflow */
    "Деление на нуль",                    /* Division by zero or denorm */
    "Двойное внутреннее прерывание",      /* SIMH: Double internal interrupt */
    "Чтение неформатированного барабана", /* Reading unformatted drum */
    "Чтение неформатированного диска",    /* Reading unformatted disk */
    "Останов по КРА",                     /* Hardware breakpoint */
    "Останов по считыванию",              /* Load watchpoint */
    "Останов по записи",                  /* Store watchpoint */
    "Не реализовано",                     /* Unimplemented I/O or special reg. access */
};

/*
 * Memory examine
 */
t_stat cpu_examine(t_value *vptr, t_addr addr, UNIT *uptr, int32 sw)
{
    DEVICE *dev = find_dev_from_unit(uptr);
    if (! dev)
        return SCPE_IERR;

    CORE *cpu = (CORE*) dev->ctxt;
    if (! cpu)
        return SCPE_IERR;

    if (addr >= MEMSIZE)
        return SCPE_NXM;
    if (vptr) {
        if (addr < 010) {
            if ((pult_tab[cpu->pult_switch][0] >> addr) & 1) {
                /* hardwired */
                *vptr = pult_tab[cpu->pult_switch][addr];
            } else {
                /* from switch regs */
                *vptr = cpu->pult[addr];
            }
        } else
            *vptr = memory[addr];
    }
    return SCPE_OK;
}

/*
 * Memory deposit
 */
t_stat cpu_deposit(t_value val, t_addr addr, UNIT *uptr, int32 sw)
{
    DEVICE *dev = find_dev_from_unit(uptr);
    if (! dev)
        return SCPE_IERR;

    CORE *cpu = (CORE*) dev->ctxt;
    if (! cpu)
        return SCPE_IERR;

    if (addr >= MEMSIZE)
        return SCPE_NXM;
    if (addr < 010) {
        /* Deposited values for the switch register address range
         * always go to switch registers.
         */
        cpu->pult[addr] = SET_PARITY(val, PARITY_INSN);
    } else
        memory[addr] = SET_PARITY(val, PARITY_INSN);
    return SCPE_OK;
}

/*
 * Reset routine
 */
t_stat cpu_reset(DEVICE *dev)
{
    CORE *cpu = (CORE*) dev->ctxt;
    if (! cpu)
        return SCPE_IERR;

    cpu->index = dev->name[3] - '0';
    cpu->ACC = 0;
    cpu->RMR = 0;
    cpu->RAU = 0;
    cpu->RUU = RUU_EXTRACODE | RUU_AVOST_DISABLE;

    int i;
    for (i=0; i<NREGS; ++i)
        cpu->M[i] = 0;

    /* Регистр 17: БлП, БлЗ, ПОП, ПОК, БлПр */
    cpu->M[PSW] = PSW_MMAP_DISABLE | PSW_PROT_DISABLE | PSW_INTR_HALT |
        PSW_CHECK_HALT | PSW_INTR_DISABLE;

    /* Регистр 23: БлП, БлЗ, РежЭ, БлПр */
    cpu->M[SPSW] = SPSW_MMAP_DISABLE | SPSW_PROT_DISABLE | SPSW_EXTRACODE |
        SPSW_INTR_DISABLE;

    cpu->GRP = 0;
    cpu->MGRP = 0;
    cpu->MPRP = 0;

    cpu->RZ = 0;
    for (i = 0; i < 8; ++i)
        cpu->RP[i] = 0;

    // Disabled due to a conflict with loading
    // cpu->PC = 1;             /* "reset cpu; go" should start from 1  */

    sim_brk_types = SWMASK('E') | SWMASK('R') | SWMASK('W');
    sim_brk_dflt = SWMASK('E');

    // Setup register descriptors.
    if (! dev->registers) {
        REG *rptr, *r;

        rptr = (REG*) malloc(sizeof(cpu0_reg));
        if (! rptr) {
            return SCPE_MEM;
        }
        memcpy(rptr, &cpu0_reg, sizeof(cpu0_reg));
        dev->registers = rptr;
        for (r = rptr; r->name != NULL; r++) {
            /* Update register pointer. */
            r->loc = (char*)r->loc + sizeof(CORE)*cpu->index;
        }
    }

    if (svs_trace) {
        fprintf(sim_log, "cpu%d --- Reset\n", cpu->index);
    }

    return SCPE_OK;
}

/*
 * Request routine
 */
t_stat cpu_req(UNIT *u, int32 val, CONST char *cptr, void *desc)
{
    DEVICE *dev = find_dev_from_unit(u);
    if (! dev)
        return SCPE_IERR;

    CORE *cpu = (CORE*) dev->ctxt;
    if (! cpu)
        return SCPE_IERR;

    cpu->GRP |= GRP_PANEL_REQ;
    return SCPE_OK;
}

/*
 * Hardwired program selector validation
 */
t_stat cpu_set_pult(UNIT *u, int32 val, CONST char *cptr, void *desc)
{
    DEVICE *dev = find_dev_from_unit(u);
    if (! dev)
        return SCPE_IERR;

    CORE *cpu = (CORE*) dev->ctxt;
    if (! cpu)
        return SCPE_IERR;

    int sw;
    if (cptr)
        sw = atoi(cptr);
    else
        sw = 0;

    if (sw >= 0 && sw <= 10) {
        cpu->pult_switch = sw;
        if (sw)
            sim_printf("Pult switch set to hardwired program %d\n", sw);
        else
            sim_printf("Pult switch set to switch registers\n");
        return SCPE_OK;
    }
    printf("Illegal value %s\n", cptr);
    return SCPE_ARG;
}

t_stat cpu_show_pult(FILE *st, UNIT *up, int32 v, CONST void *dp)
{
    DEVICE *dev = find_dev_from_unit(up);
    if (! dev)
        return SCPE_IERR;

    CORE *cpu = (CORE*) dev->ctxt;
    if (! cpu)
        return SCPE_IERR;

    fprintf(st, "Pult switch position = %d", cpu->pult_switch);
    return SCPE_OK;
}

/*
 * Trace level selector
 */
t_stat cpu_set_trace(UNIT *u, int32 val, CONST char *cptr, void *desc)
{
    if (! sim_log) {
        sim_printf("Cannot enable tracing: please set console log first\n");
        return SCPE_INCOMP;
    }
    svs_trace = TRACE_ALL;
    sim_printf("Trace instructions, registers and memory access\n");
    return SCPE_OK;
}

t_stat cpu_set_etrace(UNIT *u, int32 val, CONST char *cptr, void *desc)
{
    if (! sim_log) {
        sim_printf("Cannot enable tracing: please set console log first\n");
        return SCPE_INCOMP;
    }
    svs_trace = TRACE_EXTRACODES;
    sim_printf("Trace extracodes (except e75)\n");
    return SCPE_OK;
}

t_stat cpu_set_itrace(UNIT *u, int32 val, CONST char *cptr, void *desc)
{
    if (! sim_log) {
        sim_printf("Cannot enable tracing: please set console log first\n");
        return SCPE_INCOMP;
    }
    svs_trace = TRACE_INSTRUCTIONS;
    sim_printf("Trace instructions only\n");
    return SCPE_OK;
}

t_stat cpu_clr_trace (UNIT *uptr, int32 val, CONST char *cptr, void *desc)
{
    svs_trace = TRACE_NONE;
    return SCPE_OK;
}

t_stat cpu_show_trace(FILE *st, UNIT *up, int32 v, CONST void *dp)
{
    switch (svs_trace) {
    case TRACE_NONE:         break;
    case TRACE_EXTRACODES:   fprintf(st, "trace extracodes"); break;
    case TRACE_INSTRUCTIONS: fprintf(st, "trace instructions"); break;
    case TRACE_ALL:          fprintf(st, "trace all"); break;
    }
    return SCPE_OK;
}

/*
 * Write Unicode symbol to file.
 * Convert to UTF-8 encoding:
 * 00000000.0xxxxxxx -> 0xxxxxxx
 * 00000xxx.xxyyyyyy -> 110xxxxx, 10yyyyyy
 * xxxxyyyy.yyzzzzzz -> 1110xxxx, 10yyyyyy, 10zzzzzz
 */
void
utf8_putc(unsigned ch, FILE *fout)
{
    if (ch < 0x80) {
        putc(ch, fout);
        return;
    }
    if (ch < 0x800) {
        putc(ch >> 6 | 0xc0, fout);
        putc((ch & 0x3f) | 0x80, fout);
        return;
    }
    putc(ch >> 12 | 0xe0, fout);
    putc(((ch >> 6) & 0x3f) | 0x80, fout);
    putc((ch & 0x3f) | 0x80, fout);
}

/*
 * *call ОКНО - так называлась служебная подпрограмма в мониторной
 * системе "Дубна", которая печатала полное состояние всех регистров.
 */
void svs_okno(CORE *cpu, const char *message)
{
    svs_log_cont("_%%%%%% %s: ", message);
    if (sim_log)
        svs_fprint_cmd(sim_log, cpu->RK);
    svs_log("_");

    /* СчАС, системные индекс-регистры 020-035. */
    svs_log("_    СчАС:%05o  20:%05o  21:%05o  27:%05o  32:%05o  33:%05o  34:%05o  35:%05o",
        cpu->PC, cpu->M[020], cpu->M[021], cpu->M[027],
        cpu->M[032], cpu->M[033], cpu->M[034], cpu->M[035]);

    /* Индекс-регистры 1-7. */
    svs_log("_       1:%05o   2:%05o   3:%05o   4:%05o   5:%05o   6:%05o   7:%05o",
        cpu->M[1], cpu->M[2], cpu->M[3], cpu->M[4],
        cpu->M[5], cpu->M[6], cpu->M[7]);

    /* Индекс-регистры 010-017. */
    svs_log("_      10:%05o  11:%05o  12:%05o  13:%05o  14:%05o  15:%05o  16:%05o  17:%05o",
        cpu->M[010], cpu->M[011], cpu->M[012], cpu->M[013],
        cpu->M[014], cpu->M[015], cpu->M[016], cpu->M[017]);

    /* Сумматор, РМР, режимы АУ и УУ. */
    svs_log("_      СМ:%04o %04o %04o %04o  РМР:%04o %04o %04o %04o  РАУ:%02o    РУУ:%03o",
        (int) (cpu->ACC >> 36) & BITS(12), (int) (cpu->ACC >> 24) & BITS(12),
        (int) (cpu->ACC >> 12) & BITS(12), (int) cpu->ACC & BITS(12),
        (int) (cpu->RMR >> 36) & BITS(12), (int) (cpu->RMR >> 24) & BITS(12),
        (int) (cpu->RMR >> 12) & BITS(12), (int) cpu->RMR & BITS(12),
        cpu->RAU, cpu->RUU);
}

/*
 * Команда "рег"
 */
static void cmd_002(CORE *cpu)
{
    svs_debug("*** рег %03o", cpu->Aex & 0377);

    switch (cpu->Aex & 0377) {
    /* TODO: Регистры команды РЕГ
     * 36 МГРП
     * 37 ГРП
     * 46 маска РВП
     * 47 РВП
     * 44 тег (для ЗПП и СЧТ)
     * 50 прерывания процессорам
     * 51 ответы (другой тип прерывания) процессорам. (Ответ -> ПВВ вызывает reset ПВВ.)
     * 52 прерывания от процессоров
     * 53 ответы от процессоров
     * 54 конфигурация процессоров (online)
     * 55 конфигурация памяти
     * 56 часы
     * 57 таймер
     */
#if 0
    case 0: case 1: case 2: case 3: case 4: case 5: case 6: case 7:
        /* Запись в БРЗ */
        //mmu_setcache(cpu, cpu->Aex & 7, cpu->ACC);
        break;
    case 020: case 021: case 022: case 023:
    case 024: case 025: case 026: case 027:
        /* Запись в регистры приписки */
        mmu_set_rp(cpu, cpu->Aex & 7, cpu->ACC);
        break;
    case 030: case 031: case 032: case 033:
        /* Запись в регистры защиты */
        mmu_set_protection(cpu, cpu->Aex & 3, cpu->ACC);
        break;
    case 036:
        /* Запись в маску главного регистра прерываний */
        cpu->MGRP = cpu->ACC;
        break;
    case 037:
        /* Clearing the main interrupt register: */
        /* it is impossible to clear wired (stateless) bits this way */
        cpu->GRP &= cpu->ACC | GRP_WIRED_BITS;
        break;
    case 64: case 65: case 66: case 67: case 68: case 69: case 70: case 71:
    case 72: case 73: case 74: case 75: case 76: case 77: case 78: case 79:
    case 80: case 81: case 82: case 83: case 84: case 85: case 86: case 87:
    case 88: case 89: case 90: case 91: case 92: case 93: case 94: case 95:
        /* 0100 - 0137:
         * Бит 1: управление блокировкой режима останова БРО.
         * Биты 2 и 3 - признаки формирования контрольных
         * разрядов (ПКП и ПКЛ). */
        if (cpu->Aex & 1)
            cpu->RUU |= RUU_AVOST_DISABLE;
        else
            cpu->RUU &= ~RUU_AVOST_DISABLE;
        if (cpu->Aex & 2)
            cpu->RUU |= RUU_PARITY_RIGHT;
        else
            cpu->RUU &= ~RUU_PARITY_RIGHT;
        if (cpu->Aex & 4)
            cpu->RUU |= RUU_PARITY_LEFT;
        else
            cpu->RUU &= ~RUU_PARITY_LEFT;
        break;
    case 0200: case 0201: case 0202: case 0203:
    case 0204: case 0205: case 0206: case 0207:
        /* Чтение БРЗ */
        //cpu->ACC = mmu_getcache(cpu, cpu->Aex & 7);
        break;
    case 0237:
        /* Чтение главного регистра прерываний */
        cpu->ACC = cpu->GRP;
        break;
#endif
    default:
#if 0
        if ((cpu->Aex & 0340) == 0140) {
            /* TODO: watchdog reset mechanism */
            longjmp(cpu->exception, STOP_UNIMPLEMENTED);
        }
#endif
        /* Неиспользуемые адреса */
        svs_debug("*** %05o%s: РЕГ %o - неправильный адрес спец.регистра",
            cpu->PC, (cpu->RUU & RUU_RIGHT_INSTR) ? "п" : "л", cpu->Aex);
        break;
    }
}

static int is_extracode(int opcode)
{
    switch (opcode) {
    case 050: case 051: case 052: case 053: /* э50...э77 кроме э75 */
    case 054: case 055: case 056: case 057:
    case 060: case 061: case 062: case 063:
    case 064: case 065: case 066: case 067:
    case 070: case 071: case 072: case 073:
    case 074: case 076: case 077:
    case 0200:                              /* э20 */
    case 0210:                              /* э21 */
        return 1;
    }
    return 0;
}

/*
 * Execute one instruction, placed on address PC:RUU_RIGHT_INSTR.
 * When stopped, perform a longjmp to cpu->exception,
 * sending a stop code.
 */
void cpu_one_instr(CORE *cpu)
{
    int reg, opcode, addr, paddr, nextpc, next_mod;
    t_value word;

    /*
     * Instruction execution time in 100 ns ticks; not really used
     * as the amortized 1 MIPS instruction rate is assumed.
     * The assignments of MEAN_TIME(x,y) to the delay variable
     * are kept as a reference.
     */
    uint32 delay;

    cpu->corr_stack = 0;
    word = mmu_fetch(cpu, cpu->PC, &paddr);
    if (cpu->RUU & RUU_RIGHT_INSTR)
        cpu->RK = (uint32)word;         /* get right instruction */
    else
        cpu->RK = (uint32)(word >> 24); /* get left instruction */

    cpu->RK &= BITS(24);

    reg = cpu->RK >> 20;
    if (cpu->RK & BBIT(20)) {
        addr = cpu->RK & BITS(15);
        opcode = (cpu->RK >> 12) & 0370;
    } else {
        addr = cpu->RK & BITS(12);
        if (cpu->RK & BBIT(19))
            addr |= 070000;
        opcode = (cpu->RK >> 12) & 077;
    }

    /* Трассировка команды: адрес, код и мнемоника. */
    if (svs_trace >= TRACE_INSTRUCTIONS ||
        (svs_trace == TRACE_EXTRACODES && is_extracode(opcode))) {
        svs_trace_opcode(cpu, paddr);
    }

    nextpc = ADDR(cpu->PC + 1);
    if (cpu->RUU & RUU_RIGHT_INSTR) {
        cpu->PC += 1;                               /* increment PC */
        cpu->RUU &= ~RUU_RIGHT_INSTR;
    } else {
        cpu->RUU |= RUU_RIGHT_INSTR;
    }

    if (cpu->RUU & RUU_MOD_RK) {
        addr = ADDR(addr + cpu->M[MOD]);
    }
    next_mod = 0;
    delay = 0;

    switch (opcode) {
    case 000:                                       /* зп, atx */
        cpu->Aex = ADDR(addr + cpu->M[reg]);
        mmu_store(cpu, cpu->Aex, cpu->ACC);
        if (! addr && reg == 017)
            cpu->M[017] = ADDR(cpu->M[017] + 1);
        delay = MEAN_TIME(3, 3);
        break;
    case 001:                                       /* зпм, stx */
        cpu->Aex = ADDR(addr + cpu->M[reg]);
        mmu_store(cpu, cpu->Aex, cpu->ACC);
        cpu->M[017] = ADDR(cpu->M[017] - 1);
        cpu->corr_stack = 1;
        cpu->ACC = mmu_load(cpu, cpu->M[017]);
        cpu->RAU = SET_LOGICAL(cpu->RAU);
        delay = MEAN_TIME(6, 6);
        break;
    case 002:                                       /* рег, mod */
        cpu->Aex = ADDR(addr + cpu->M[reg]);
        if (! IS_SUPERVISOR(cpu->RUU))
            longjmp(cpu->exception, STOP_BADCMD);
        cmd_002(cpu);
        /* Режим АУ - логический, если операция была "чтение" */
        if (cpu->Aex & 0200)
            cpu->RAU = SET_LOGICAL(cpu->RAU);
        delay = MEAN_TIME(3, 3);
        break;
    case 003:                                       /* счм, xts */
        mmu_store(cpu, cpu->M[017], cpu->ACC);
        cpu->M[017] = ADDR(cpu->M[017] + 1);
        cpu->corr_stack = -1;
        cpu->Aex = ADDR(addr + cpu->M[reg]);
        cpu->ACC = mmu_load(cpu, cpu->Aex);
        cpu->RAU = SET_LOGICAL(cpu->RAU);
        delay = MEAN_TIME(6, 6);
        break;
    case 004:                                       /* сл, a+x */
        if (! addr && reg == 017) {
            cpu->M[017] = ADDR(cpu->M[017] - 1);
            cpu->corr_stack = 1;
        }
        cpu->Aex = ADDR(addr + cpu->M[reg]);
        svs_add(cpu, mmu_load(cpu, cpu->Aex), 0, 0);
        cpu->RAU = SET_ADDITIVE(cpu->RAU);
        delay = MEAN_TIME(3, 11);
        break;
    case 005:                                       /* вч, a-x */
        if (! addr && reg == 017) {
            cpu->M[017] = ADDR(cpu->M[017] - 1);
            cpu->corr_stack = 1;
        }
        cpu->Aex = ADDR(addr + cpu->M[reg]);
        svs_add(cpu, mmu_load(cpu, cpu->Aex), 0, 1);
        cpu->RAU = SET_ADDITIVE(cpu->RAU);
        delay = MEAN_TIME(3, 11);
        break;
    case 006:                                       /* вчоб, x-a */
        if (! addr && reg == 017) {
            cpu->M[017] = ADDR(cpu->M[017] - 1);
            cpu->corr_stack = 1;
        }
        cpu->Aex = ADDR(addr + cpu->M[reg]);
        svs_add(cpu, mmu_load(cpu, cpu->Aex), 1, 0);
        cpu->RAU = SET_ADDITIVE(cpu->RAU);
        delay = MEAN_TIME(3, 11);
        break;
    case 007:                                       /* вчаб, amx */
        if (! addr && reg == 017) {
            cpu->M[017] = ADDR(cpu->M[017] - 1);
            cpu->corr_stack = 1;
        }
        cpu->Aex = ADDR(addr + cpu->M[reg]);
        svs_add(cpu, mmu_load(cpu, cpu->Aex), 1, 1);
        cpu->RAU = SET_ADDITIVE(cpu->RAU);
        delay = MEAN_TIME(3, 11);
        break;
    case 010:                                       /* сч, xta */
        if (! addr && reg == 017) {
            cpu->M[017] = ADDR(cpu->M[017] - 1);
            cpu->corr_stack = 1;
        }
        cpu->Aex = ADDR(addr + cpu->M[reg]);
        cpu->ACC = mmu_load(cpu, cpu->Aex);
        cpu->RAU = SET_LOGICAL(cpu->RAU);
        delay = MEAN_TIME(3, 3);
        break;
    case 011:                                       /* и, aax */
        if (! addr && reg == 017) {
            cpu->M[017] = ADDR(cpu->M[017] - 1);
            cpu->corr_stack = 1;
        }
        cpu->Aex = ADDR(addr + cpu->M[reg]);
        cpu->ACC &= mmu_load(cpu, cpu->Aex);
        cpu->RMR = 0;
        cpu->RAU = SET_LOGICAL(cpu->RAU);
        delay = MEAN_TIME(3, 4);
        break;
    case 012:                                       /* нтж, aex */
        if (! addr && reg == 017) {
            cpu->M[017] = ADDR(cpu->M[017] - 1);
            cpu->corr_stack = 1;
        }
        cpu->Aex = ADDR(addr + cpu->M[reg]);
        cpu->RMR = cpu->ACC;
        cpu->ACC ^= mmu_load(cpu, cpu->Aex);
        cpu->RAU = SET_LOGICAL(cpu->RAU);
        delay = MEAN_TIME(3, 3);
        break;
    case 013:                                       /* слц, arx */
        if (! addr && reg == 017) {
            cpu->M[017] = ADDR(cpu->M[017] - 1);
            cpu->corr_stack = 1;
        }
        cpu->Aex = ADDR(addr + cpu->M[reg]);
        cpu->ACC += mmu_load(cpu, cpu->Aex);
        if (cpu->ACC & BIT49)
            cpu->ACC = (cpu->ACC + 1) & BITS48;
        cpu->RMR = 0;
        cpu->RAU = SET_MULTIPLICATIVE(cpu->RAU);
        delay = MEAN_TIME(3, 6);
        break;
    case 014:                                       /* знак, avx */
        if (! addr && reg == 017) {
            cpu->M[017] = ADDR(cpu->M[017] - 1);
            cpu->corr_stack = 1;
        }
        cpu->Aex = ADDR(addr + cpu->M[reg]);
        svs_change_sign(cpu, mmu_load(cpu, cpu->Aex) >> 40 & 1);
        cpu->RAU = SET_ADDITIVE(cpu->RAU);
        delay = MEAN_TIME(3, 5);
        break;
    case 015:                                       /* или, aox */
        if (! addr && reg == 017) {
            cpu->M[017] = ADDR(cpu->M[017] - 1);
            cpu->corr_stack = 1;
        }
        cpu->Aex = ADDR(addr + cpu->M[reg]);
        cpu->ACC |= mmu_load(cpu, cpu->Aex);
        cpu->RMR = 0;
        cpu->RAU = SET_LOGICAL(cpu->RAU);
        delay = MEAN_TIME(3, 4);
        break;
    case 016:                                       /* дел, a/x */
        if (! addr && reg == 017) {
            cpu->M[017] = ADDR(cpu->M[017] - 1);
            cpu->corr_stack = 1;
        }
        cpu->Aex = ADDR(addr + cpu->M[reg]);
        svs_divide(cpu, mmu_load(cpu, cpu->Aex));
        cpu->RAU = SET_MULTIPLICATIVE(cpu->RAU);
        delay = MEAN_TIME(3, 50);
        break;
    case 017:                                       /* умн, a*x */
        if (! addr && reg == 017) {
            cpu->M[017] = ADDR(cpu->M[017] - 1);
            cpu->corr_stack = 1;
        }
        cpu->Aex = ADDR(addr + cpu->M[reg]);
        svs_multiply(cpu, mmu_load(cpu, cpu->Aex));
        cpu->RAU = SET_MULTIPLICATIVE(cpu->RAU);
        delay = MEAN_TIME(3, 18);
        break;
    case 020:                                       /* сбр, apx */
        if (! addr && reg == 017) {
            cpu->M[017] = ADDR(cpu->M[017] - 1);
            cpu->corr_stack = 1;
        }
        cpu->Aex = ADDR(addr + cpu->M[reg]);
        cpu->ACC = svs_pack(cpu->ACC, mmu_load(cpu, cpu->Aex));
        cpu->RMR = 0;
        cpu->RAU = SET_LOGICAL(cpu->RAU);
        delay = MEAN_TIME(3, 53);
        break;
    case 021:                                       /* рзб, aux */
        if (! addr && reg == 017) {
            cpu->M[017] = ADDR(cpu->M[017] - 1);
            cpu->corr_stack = 1;
        }
        cpu->Aex = ADDR(addr + cpu->M[reg]);
        cpu->ACC = svs_unpack(cpu->ACC, mmu_load(cpu, cpu->Aex));
        cpu->RMR = 0;
        cpu->RAU = SET_LOGICAL(cpu->RAU);
        delay = MEAN_TIME(3, 53);
        break;
    case 022:                                       /* чед, acx */
        if (! addr && reg == 017) {
            cpu->M[017] = ADDR(cpu->M[017] - 1);
            cpu->corr_stack = 1;
        }
        cpu->Aex = ADDR(addr + cpu->M[reg]);
        cpu->ACC = svs_count_ones(cpu->ACC) + mmu_load(cpu, cpu->Aex);
        if (cpu->ACC & BIT49)
            cpu->ACC = (cpu->ACC + 1) & BITS48;
        cpu->RAU = SET_LOGICAL(cpu->RAU);
        delay = MEAN_TIME(3, 56);
        break;
    case 023:                                       /* нед, anx */
        if (! addr && reg == 017) {
            cpu->M[017] = ADDR(cpu->M[017] - 1);
            cpu->corr_stack = 1;
        }
        cpu->Aex = ADDR(addr + cpu->M[reg]);
        if (cpu->ACC) {
            int n = svs_highest_bit(cpu->ACC);

            /* "Остаток" сумматора, исключая бит,
             * номер которого определен, помещается в РМР,
             * начиная со старшего бита РМР. */
            svs_shift(cpu, 48 - n);

            /* Циклическое сложение номера со словом по Аисп. */
            cpu->ACC = n + mmu_load(cpu, cpu->Aex);
            if (cpu->ACC & BIT49)
                cpu->ACC = (cpu->ACC + 1) & BITS48;
        } else {
            cpu->RMR = 0;
            cpu->ACC = mmu_load(cpu, cpu->Aex);
        }
        cpu->RAU = SET_LOGICAL(cpu->RAU);
        delay = MEAN_TIME(3, 32);
        break;
    case 024:                                       /* слп, e+x */
        if (! addr && reg == 017) {
            cpu->M[017] = ADDR(cpu->M[017] - 1);
            cpu->corr_stack = 1;
        }
        cpu->Aex = ADDR(addr + cpu->M[reg]);
        svs_add_exponent(cpu, (mmu_load(cpu, cpu->Aex) >> 41) - 64);
        cpu->RAU = SET_MULTIPLICATIVE(cpu->RAU);
        delay = MEAN_TIME(3, 5);
        break;
    case 025:                                       /* вчп, e-x */
        if (! addr && reg == 017) {
            cpu->M[017] = ADDR(cpu->M[017] - 1);
            cpu->corr_stack = 1;
        }
        cpu->Aex = ADDR(addr + cpu->M[reg]);
        svs_add_exponent(cpu, 64 - (mmu_load(cpu, cpu->Aex) >> 41));
        cpu->RAU = SET_MULTIPLICATIVE(cpu->RAU);
        delay = MEAN_TIME(3, 5);
        break;
    case 026: {                                     /* сд, asx */
        int n;
        if (! addr && reg == 017) {
            cpu->M[017] = ADDR(cpu->M[017] - 1);
            cpu->corr_stack = 1;
        }
        cpu->Aex = ADDR(addr + cpu->M[reg]);
        n = (mmu_load(cpu, cpu->Aex) >> 41) - 64;
        svs_shift(cpu, n);
        cpu->RAU = SET_LOGICAL(cpu->RAU);
        delay = MEAN_TIME(3, 4 + abs(n));
        break;
    }
    case 027:                                       /* рж, xtr */
        if (! addr && reg == 017) {
            cpu->M[017] = ADDR(cpu->M[017] - 1);
            cpu->corr_stack = 1;
        }
        cpu->Aex = ADDR(addr + cpu->M[reg]);
        cpu->RAU = (mmu_load(cpu, cpu->Aex) >> 41) & 077;
        delay = MEAN_TIME(3, 3);
        break;
    case 030:                                       /* счрж, rte */
        cpu->Aex = ADDR(addr + cpu->M[reg]);
        cpu->ACC = (t_value) (cpu->RAU & cpu->Aex & 0177) << 41;
        cpu->RAU = SET_LOGICAL(cpu->RAU);
        delay = MEAN_TIME(3, 3);
        break;
    case 031:                                       /* счмр, yta */
        cpu->Aex = ADDR(addr + cpu->M[reg]);
        if (IS_LOGICAL(cpu->RAU)) {
            cpu->ACC = cpu->RMR;
        } else {
            t_value x = cpu->RMR;
            cpu->ACC = (cpu->ACC & ~BITS41) | (cpu->RMR & BITS40);
            svs_add_exponent(cpu, (cpu->Aex & 0177) - 64);
            cpu->RMR = x;
        }
        delay = MEAN_TIME(3, 5);
        break;
    case 032:                                       /* зпп, запись полноразрядная */
        cpu->Aex = ADDR(addr + cpu->M[reg]);
        if (! IS_SUPERVISOR(cpu->RUU))
            longjmp(cpu->exception, STOP_BADCMD);
        //TODO: команда ЗПП
        svs_debug("*** зпп %05o", cpu->Aex);
        delay = MEAN_TIME(3, 8);
        break;
    case 033:                                       /* счп, считывание полноразрядное */
        cpu->Aex = ADDR(addr + cpu->M[reg]);
        if (! IS_SUPERVISOR(cpu->RUU))
            longjmp(cpu->exception, STOP_BADCMD);
        //TODO: команда СЧП
        svs_debug("*** счп %05o", cpu->Aex);
        delay = MEAN_TIME(3, 8);
        break;
    case 034:                                       /* слпа, e+n */
        cpu->Aex = ADDR(addr + cpu->M[reg]);
        svs_add_exponent(cpu, (cpu->Aex & 0177) - 64);
        cpu->RAU = SET_MULTIPLICATIVE(cpu->RAU);
        delay = MEAN_TIME(3, 5);
        break;
    case 035:                                       /* вчпа, e-n */
        cpu->Aex = ADDR(addr + cpu->M[reg]);
        svs_add_exponent(cpu, 64 - (cpu->Aex & 0177));
        cpu->RAU = SET_MULTIPLICATIVE(cpu->RAU);
        delay = MEAN_TIME(3, 5);
        break;
    case 036: {                                     /* сда, asn */
        int n;
        cpu->Aex = ADDR(addr + cpu->M[reg]);
        n = (cpu->Aex & 0177) - 64;
        svs_shift(cpu, n);
        cpu->RAU = SET_LOGICAL(cpu->RAU);
        delay = MEAN_TIME(3, 4 + abs(n));
        break;
    }
    case 037:                                       /* ржа, ntr */
        cpu->Aex = ADDR(addr + cpu->M[reg]);
        cpu->RAU = cpu->Aex & 077;
        delay = MEAN_TIME(3, 3);
        break;
    case 040:                                       /* уи, ati */
        cpu->Aex = ADDR(addr + cpu->M[reg]);
        if (IS_SUPERVISOR(cpu->RUU)) {
            int reg = cpu->Aex & 037;
            cpu->M[reg] = ADDR(cpu->ACC);
            /* breakpoint/watchpoint regs will match physical
             * or virtual addresses depending on the current
             * mapping mode.
             */
            if ((cpu->M[PSW] & PSW_MMAP_DISABLE) &&
                (reg == IBP || reg == DWP))
                cpu->M[reg] |= BBIT(16);

        } else
            cpu->M[cpu->Aex & 017] = ADDR(cpu->ACC);
        cpu->M[0] = 0;
        delay = MEAN_TIME(14, 3);
        break;
    case 041: {                                     /* уим, sti */
        unsigned rg, ad;

        cpu->Aex = ADDR(addr + cpu->M[reg]);
        rg = cpu->Aex & (IS_SUPERVISOR(cpu->RUU) ? 037 : 017);
        ad = ADDR(cpu->ACC);
        if (rg != 017) {
            cpu->M[017] = ADDR(cpu->M[017] - 1);
            cpu->corr_stack = 1;
        }
        cpu->ACC = mmu_load(cpu, rg != 017 ? cpu->M[017] : ad);
        cpu->M[rg] = ad;
        if ((cpu->M[PSW] & PSW_MMAP_DISABLE) && (rg == IBP || rg == DWP))
            cpu->M[rg] |= BBIT(16);
        cpu->M[0] = 0;
        cpu->RAU = SET_LOGICAL(cpu->RAU);
        delay = MEAN_TIME(14, 3);
        break;
    }
    case 042:                                       /* счи, ita */
        delay = MEAN_TIME(6, 3);
load_modifier:
        cpu->Aex = ADDR(addr + cpu->M[reg]);
        cpu->ACC = ADDR(cpu->M[cpu->Aex & (IS_SUPERVISOR(cpu->RUU) ? 037 : 017)]);
        cpu->RAU = SET_LOGICAL(cpu->RAU);
        break;
    case 043:                                       /* счим, its */
        mmu_store(cpu, cpu->M[017], cpu->ACC);
        cpu->M[017] = ADDR(cpu->M[017] + 1);
        delay = MEAN_TIME(9, 6);
        goto load_modifier;
    case 044:                                       /* уии, mtj */
        cpu->Aex = addr;
        if (IS_SUPERVISOR(cpu->RUU)) {
transfer_modifier:
            cpu->M[cpu->Aex & 037] = cpu->M[reg];
            if ((cpu->M[PSW] & PSW_MMAP_DISABLE) &&
                ((cpu->Aex & 037) == IBP || (cpu->Aex & 037) == DWP))
                cpu->M[cpu->Aex & 037] |= BBIT(16);

        } else
            cpu->M[cpu->Aex & 017] = cpu->M[reg];
        cpu->M[0] = 0;
        delay = 6;
        break;
    case 045:                                       /* сли, j+m */
        cpu->Aex = addr;
        if ((cpu->Aex & 020) && IS_SUPERVISOR(cpu->RUU))
            goto transfer_modifier;
        cpu->M[cpu->Aex & 017] = ADDR(cpu->M[cpu->Aex & 017] + cpu->M[reg]);
        cpu->M[0] = 0;
        delay = 6;
        break;
    case 046:                                       /* cоп, специальное обращение к памяти */
        cpu->Aex = addr;
        if (! IS_SUPERVISOR(cpu->RUU))
            longjmp(cpu->exception, STOP_BADCMD);
        //TODO: команда 046
        svs_debug("*** соп %05o", cpu->Aex);
        delay = 6;
        break;
    case 047:                                       /* э47, x47 */
        cpu->Aex = addr;
        if (! IS_SUPERVISOR(cpu->RUU))
            longjmp(cpu->exception, STOP_BADCMD);
        cpu->M[cpu->Aex & 017] = ADDR(cpu->M[cpu->Aex & 017] + cpu->Aex);
        cpu->M[0] = 0;
        delay = 6;
        break;
    case 050: case 051: case 052: case 053:
    case 054: case 055: case 056: case 057:
    case 060: case 061: case 062: case 063:
    case 064: case 065: case 066: case 067:
    case 070: case 071: case 072: case 073:
    case 074: case 075: case 076: case 077:         /* э50...э77 */
    case 0200:                                      /* э20 */
    case 0210:                                      /* э21 */
stop_as_extracode:
            cpu->Aex = ADDR(addr + cpu->M[reg]);
            /* Адрес возврата из экстракода. */
            cpu->M[ERET] = nextpc;
            /* Сохранённые режимы УУ. */
            cpu->M[SPSW] = (cpu->M[PSW] & (PSW_INTR_DISABLE | PSW_MMAP_DISABLE |
                                           PSW_PROT_DISABLE)) | IS_SUPERVISOR(cpu->RUU);
            /* Текущие режимы УУ. */
            cpu->M[PSW] = PSW_INTR_DISABLE | PSW_MMAP_DISABLE |
                          PSW_PROT_DISABLE | /*?*/ PSW_INTR_HALT;
            cpu->M[14] = cpu->Aex;
            cpu->RUU = SET_SUPERVISOR(cpu->RUU, SPSW_EXTRACODE);

            if (opcode <= 077)
                cpu->PC = 0500 + opcode;            /* э50-э77 */
            else
                cpu->PC = 0540 + (opcode >> 3);     /* э20, э21 */
            cpu->RUU &= ~RUU_RIGHT_INSTR;
            delay = 7;
            break;
    case 0220:                                      /* мода, utc */
        cpu->Aex = ADDR(addr + cpu->M[reg]);
        next_mod = cpu->Aex;
        delay = 4;
        break;
    case 0230:                                      /* мод, wtc */
        if (! addr && reg == 017) {
            cpu->M[017] = ADDR(cpu->M[017] - 1);
            cpu->corr_stack = 1;
        }
        cpu->Aex = ADDR(addr + cpu->M[reg]);
        next_mod = ADDR(mmu_load(cpu, cpu->Aex));
        delay = MEAN_TIME(13, 3);
        break;
    case 0240:                                      /* уиа, vtm */
        cpu->Aex = addr;
        cpu->M[reg] = addr;
        cpu->M[0] = 0;
        if (IS_SUPERVISOR(cpu->RUU) && reg == 0) {
            cpu->M[PSW] &= ~(PSW_INTR_DISABLE |
                             PSW_MMAP_DISABLE | PSW_PROT_DISABLE);
            cpu->M[PSW] |= addr & (PSW_INTR_DISABLE |
                                   PSW_MMAP_DISABLE | PSW_PROT_DISABLE);
        }
        delay = 4;
        break;
    case 0250:                                      /* слиа, utm */
        cpu->Aex = ADDR(addr + cpu->M[reg]);
        cpu->M[reg] = cpu->Aex;
        cpu->M[0] = 0;
        if (IS_SUPERVISOR(cpu->RUU) && reg == 0) {
            cpu->M[PSW] &= ~(PSW_INTR_DISABLE |
                             PSW_MMAP_DISABLE | PSW_PROT_DISABLE);
            cpu->M[PSW] |= addr & (PSW_INTR_DISABLE |
                                   PSW_MMAP_DISABLE | PSW_PROT_DISABLE);
        }
        delay = 4;
        break;
    case 0260:                                      /* по, uza */
        cpu->Aex = ADDR(addr + cpu->M[reg]);
        cpu->RMR = cpu->ACC;
        delay = MEAN_TIME(12, 3);
        if (IS_ADDITIVE(cpu->RAU)) {
            if (cpu->ACC & BIT41)
                break;
        } else if (IS_MULTIPLICATIVE(cpu->RAU)) {
            if (! (cpu->ACC & BIT48))
                break;
        } else if (IS_LOGICAL(cpu->RAU)) {
            if (cpu->ACC)
                break;
        } else
            break;
        cpu->PC = cpu->Aex;
        cpu->RUU &= ~RUU_RIGHT_INSTR;
        delay += 3;
        break;
    case 0270:                                      /* пе, u1a */
        cpu->Aex = ADDR(addr + cpu->M[reg]);
        cpu->RMR = cpu->ACC;
        delay = MEAN_TIME(12, 3);
        if (IS_ADDITIVE(cpu->RAU)) {
            if (! (cpu->ACC & BIT41))
                break;
        } else if (IS_MULTIPLICATIVE(cpu->RAU)) {
            if (cpu->ACC & BIT48)
                break;
        } else if (IS_LOGICAL(cpu->RAU)) {
            if (! cpu->ACC)
                break;
        } else
            /* fall thru, i.e. branch */;
        cpu->PC = cpu->Aex;
        cpu->RUU &= ~RUU_RIGHT_INSTR;
        delay += 3;
        break;
    case 0300:                                      /* пб, uj */
        cpu->Aex = ADDR(addr + cpu->M[reg]);
        cpu->PC = cpu->Aex;
        cpu->RUU &= ~RUU_RIGHT_INSTR;
        delay = 7;
        break;
    case 0310:                                      /* пв, vjm */
        cpu->Aex = addr;
        cpu->M[reg] = nextpc;
        cpu->M[0] = 0;
        cpu->PC = addr;
        cpu->RUU &= ~RUU_RIGHT_INSTR;
        delay = 7;
        break;
    case 0320:                                      /* выпр, iret */
        cpu->Aex = addr;
        if (! IS_SUPERVISOR(cpu->RUU)) {
            longjmp(cpu->exception, STOP_BADCMD);
        }
        cpu->M[PSW] = (cpu->M[PSW] & PSW_WRITE_WATCH) |
                      (cpu->M[SPSW] & (SPSW_INTR_DISABLE |
                                       SPSW_MMAP_DISABLE | SPSW_PROT_DISABLE));
        cpu->PC = cpu->M[(reg & 3) | 030];
        cpu->RUU &= ~RUU_RIGHT_INSTR;
        if (cpu->M[SPSW] & SPSW_RIGHT_INSTR)
            cpu->RUU |= RUU_RIGHT_INSTR;
        else
            cpu->RUU &= ~RUU_RIGHT_INSTR;
        cpu->RUU = SET_SUPERVISOR(cpu->RUU,
                                  cpu->M[SPSW] & (SPSW_EXTRACODE | SPSW_INTERRUPT));
        if (cpu->M[SPSW] & SPSW_MOD_RK)
            next_mod = cpu->M[MOD];
        /*svs_okno("Выход из прерывания");*/
        delay = 7;
        break;
    case 0330:                                      /* стоп, stop */
        cpu->Aex = ADDR(addr + cpu->M[reg]);
        delay = 7;
        if (! IS_SUPERVISOR(cpu->RUU)) {
            if (cpu->M[PSW] & PSW_CHECK_HALT)
                break;
            else {
                opcode = 063;
                goto stop_as_extracode;
            }
        }
        longjmp(cpu->exception, STOP_STOP);
        break;
    case 0340:                                      /* пио, vzm */
branch_zero:
        cpu->Aex = addr;
        delay = 4;
        if (! cpu->M[reg]) {
            cpu->PC = addr;
            cpu->RUU &= ~RUU_RIGHT_INSTR;
            delay += 3;
        }
        break;
    case 0350:                                      /* пино, v1m */
        cpu->Aex = addr;
        delay = 4;
        if (cpu->M[reg]) {
            cpu->PC = addr;
            cpu->RUU &= ~RUU_RIGHT_INSTR;
            delay += 3;
        }
        break;
    case 0360:                                      /* э36, *36 */
        goto branch_zero;
    case 0370:                                      /* цикл, vlm */
        cpu->Aex = addr;
        delay = 4;
        if (! cpu->M[reg])
            break;
        cpu->M[reg] = ADDR(cpu->M[reg] + 1);
        cpu->PC = addr;
        cpu->RUU &= ~RUU_RIGHT_INSTR;
        delay += 3;
        break;
    default:
        /* Unknown instruction - cannot happen. */
        longjmp(cpu->exception, STOP_STOP);
        break;
    }

    if (next_mod) {
        /* Модификация адреса следующей команды. */
        cpu->M[MOD] = next_mod;
        cpu->RUU |= RUU_MOD_RK;
    } else
        cpu->RUU &= ~RUU_MOD_RK;

    /* Трассировка изменённых регистров. */
    if (svs_trace == TRACE_ALL) {
        svs_trace_registers(cpu);
    }
#if 0
    //TODO: обнаружение цикла "ЖДУ" диспака
    /* Не находимся ли мы в цикле "ЖДУ" диспака? */
    if (cpu->RUU == 047 && cpu->PC == 04440 && cpu->RK == 067704440) {
        //check_initial_setup();
        sim_idle(0, TRUE);
    }
#endif
}

/*
 * Операция прерывания 1: внутреннее прерывание.
 * Описана в 9-м томе технического описания БЭСМ-6, страница 119.
 */
void op_int_1(CORE *cpu, const char *msg)
{
    /*svs_okno(msg);*/
    cpu->M[SPSW] = (cpu->M[PSW] & (PSW_INTR_DISABLE | PSW_MMAP_DISABLE |
                                   PSW_PROT_DISABLE)) | IS_SUPERVISOR(cpu->RUU);
    if (cpu->RUU & RUU_RIGHT_INSTR)
        cpu->M[SPSW] |= SPSW_RIGHT_INSTR;
    cpu->M[IRET] = cpu->PC;
    cpu->M[PSW] |= PSW_INTR_DISABLE | PSW_MMAP_DISABLE | PSW_PROT_DISABLE;
    if (cpu->RUU & RUU_MOD_RK) {
        cpu->M[SPSW] |= SPSW_MOD_RK;
        cpu->RUU &= ~RUU_MOD_RK;
    }
    cpu->PC = 0500;
    cpu->RUU &= ~RUU_RIGHT_INSTR;
    cpu->RUU = SET_SUPERVISOR(cpu->RUU, SPSW_INTERRUPT);
}

/*
 * Операция прерывания 2: внешнее прерывание.
 * Описана в 9-м томе технического описания БЭСМ-6, страница 129.
 */
void op_int_2(CORE *cpu)
{
    /*svs_okno("Внешнее прерывание");*/
    cpu->M[SPSW] = (cpu->M[PSW] & (PSW_INTR_DISABLE | PSW_MMAP_DISABLE |
                                   PSW_PROT_DISABLE)) | IS_SUPERVISOR(cpu->RUU);
    cpu->M[IRET] = cpu->PC;
    cpu->M[PSW] |= PSW_INTR_DISABLE | PSW_MMAP_DISABLE | PSW_PROT_DISABLE;
    if (cpu->RUU & RUU_MOD_RK) {
        cpu->M[SPSW] |= SPSW_MOD_RK;
        cpu->RUU &= ~RUU_MOD_RK;
    }
    cpu->PC = 0501;
    cpu->RUU &= ~RUU_RIGHT_INSTR;
    cpu->RUU = SET_SUPERVISOR(cpu->RUU, SPSW_INTERRUPT);
}

/*
 * Main instruction fetch/decode loop
 */
t_stat sim_instr(void)
{
    //TODO: выполнение инструкций на процессорах cpu1..9
    CORE *cpu = &cpu_core[0];
    t_stat r;
    int iintr = 0;

    /* Трассировка начального состояния. */
    if (svs_trace == TRACE_ALL) {
        svs_trace_registers(cpu);
    }

    /* Restore register state */
    cpu->PC &= BITS(15);                            /* mask PC */
    mmu_setup(cpu);                                 /* copy RP to TLB */

    /* An internal interrupt or user intervention */
    r = setjmp(cpu->exception);
    if (r) {
        const char *message = (r >= SCPE_BASE) ?
            scp_errors[r - SCPE_BASE] :
            sim_stop_messages[r];

        if (svs_trace >= TRACE_INSTRUCTIONS) {
            fprintf(sim_log, "cpu%d --- %s\n",
                cpu->index, message);
        }
        cpu->M[017] += cpu->corr_stack;

        /*
         * ПоП и ПоК вызывают останов при любом внутреннем прерывании
         * или прерывании по контролю, соответственно.
         * Если произошёл останов по ПоП или ПоК,
         * то продолжение выполнения начнётся с команды, следующей
         * за вызвавшей прерывание. Как если бы кнопка "ТП" (тип
         * перехода) была включена. Подробнее на странице 119 ТО9.
         */
        switch (r) {
        default:
ret:        return r;
        case STOP_RWATCH:
        case STOP_WWATCH:
            /* Step back one insn to reexecute it */
            if (! (cpu->RUU & RUU_RIGHT_INSTR)) {
                --cpu->PC;
            }
            cpu->RUU ^= RUU_RIGHT_INSTR;
            goto ret;
        case STOP_BADCMD:
            if (cpu->M[PSW] & PSW_INTR_HALT)        /* ПоП */
                goto ret;
            op_int_1(cpu, sim_stop_messages[r]);
            // SPSW_NEXT_RK is not important for this interrupt
            cpu->GRP |= GRP_ILL_INSN;
            break;
        case STOP_INSN_CHECK:
            if (cpu->M[PSW] & PSW_CHECK_HALT)       /* ПоК */
                goto ret;
            op_int_1(cpu, sim_stop_messages[r]);
            // SPSW_NEXT_RK must be 0 for this interrupt; it is already
            cpu->GRP |= GRP_INSN_CHECK;
            break;
        case STOP_INSN_PROT:
            if (cpu->M[PSW] & PSW_INTR_HALT)        /* ПоП */
                goto ret;
            if (cpu->RUU & RUU_RIGHT_INSTR) {
                ++cpu->PC;
            }
            cpu->RUU ^= RUU_RIGHT_INSTR;
            op_int_1(cpu, sim_stop_messages[r]);
            // SPSW_NEXT_RK must be 1 for this interrupt
            cpu->M[SPSW] |= SPSW_NEXT_RK;
            cpu->GRP |= GRP_INSN_PROT;
            break;
        case STOP_OPERAND_PROT:
#if 0
/* ДИСПАК держит признак ПоП установленным.
 * При запуске СЕРП возникает обращение к чужому листу. */
            if (cpu->M[PSW] & PSW_INTR_HALT)        /* ПоП */
                goto ret;
#endif
            if (cpu->RUU & RUU_RIGHT_INSTR) {
                ++cpu->PC;
            }
            cpu->RUU ^= RUU_RIGHT_INSTR;
            op_int_1(cpu, sim_stop_messages[r]);
            cpu->M[SPSW] |= SPSW_NEXT_RK;
            // The offending virtual page is in bits 5-9
            cpu->GRP |= GRP_OPRND_PROT;
            cpu->GRP = GRP_SET_PAGE(cpu->GRP, cpu->bad_addr);
            break;
        case STOP_RAM_CHECK:
            if (cpu->M[PSW] & PSW_CHECK_HALT)       /* ПоК */
                goto ret;
            op_int_1(cpu, sim_stop_messages[r]);
            // The offending interleaved block # is in bits 1-3.
            cpu->GRP |= GRP_CHECK | GRP_RAM_CHECK;
            cpu->GRP = GRP_SET_BLOCK(cpu->GRP, cpu->bad_addr);
            break;
        case STOP_CACHE_CHECK:
            if (cpu->M[PSW] & PSW_CHECK_HALT)       /* ПоК */
                goto ret;
            op_int_1(cpu, sim_stop_messages[r]);
            // The offending BRZ # is in bits 1-3.
            cpu->GRP |= GRP_CHECK;
            cpu->GRP &= ~GRP_RAM_CHECK;
            cpu->GRP = GRP_SET_BLOCK(cpu->GRP, cpu->bad_addr);
            break;
        case STOP_INSN_ADDR_MATCH:
            if (cpu->M[PSW] & PSW_INTR_HALT)        /* ПоП */
                goto ret;
            if (cpu->RUU & RUU_RIGHT_INSTR) {
                ++cpu->PC;
            }
            cpu->RUU ^= RUU_RIGHT_INSTR;
            op_int_1(cpu, sim_stop_messages[r]);
            cpu->M[SPSW] |= SPSW_NEXT_RK;
            cpu->GRP |= GRP_BREAKPOINT;
            break;
        case STOP_LOAD_ADDR_MATCH:
            if (cpu->M[PSW] & PSW_INTR_HALT)        /* ПоП */
                goto ret;
            if (cpu->RUU & RUU_RIGHT_INSTR) {
                ++cpu->PC;
            }
            cpu->RUU ^= RUU_RIGHT_INSTR;
            op_int_1(cpu, sim_stop_messages[r]);
            cpu->M[SPSW] |= SPSW_NEXT_RK;
            cpu->GRP |= GRP_WATCHPT_R;
            break;
        case STOP_STORE_ADDR_MATCH:
            if (cpu->M[PSW] & PSW_INTR_HALT)        /* ПоП */
                goto ret;
            if (cpu->RUU & RUU_RIGHT_INSTR) {
                ++cpu->PC;
            }
            cpu->RUU ^= RUU_RIGHT_INSTR;
            op_int_1(cpu, sim_stop_messages[r]);
            cpu->M[SPSW] |= SPSW_NEXT_RK;
            cpu->GRP |= GRP_WATCHPT_W;
            break;
        case STOP_OVFL:
            /* Прерывание по АУ вызывает останов, если БРО=0
             * и установлен ПоП или ПоК.
             * Страница 118 ТО9.*/
            if (! (cpu->RUU & RUU_AVOST_DISABLE) && /* ! БРО */
                ((cpu->M[PSW] & PSW_INTR_HALT) ||   /* ПоП */
                 (cpu->M[PSW] & PSW_CHECK_HALT)))   /* ПоК */
                goto ret;
            op_int_1(cpu, sim_stop_messages[r]);
            cpu->GRP |= GRP_OVERFLOW|GRP_RAM_CHECK;
            break;
        case STOP_DIVZERO:
            if (! (cpu->RUU & RUU_AVOST_DISABLE) && /* ! БРО */
                ((cpu->M[PSW] & PSW_INTR_HALT) ||   /* ПоП */
                 (cpu->M[PSW] & PSW_CHECK_HALT)))   /* ПоК */
                goto ret;
            op_int_1(cpu, sim_stop_messages[r]);
            cpu->GRP |= GRP_DIVZERO|GRP_RAM_CHECK;
            break;
        }
        ++iintr;
    }

    if (iintr > 1) {
        return STOP_DOUBLE_INTR;
    }

    /* Main instruction fetch/decode loop */
    for (;;) {
        if (sim_interval <= 0) {                /* check clock queue */
            r = sim_process_event();
            if (r) {
                return r;
            }
        }

        if (cpu->PC > BITS(15) && IS_SUPERVISOR(cpu->RUU)) {
          /*
           * Runaway instruction execution in supervisor mode
           * warrants attention.
           */
            return STOP_RUNOUT;                 /* stop simulation */
        }

        if ((sim_brk_summ & SWMASK('E')) &&     /* breakpoint? */
            sim_brk_test(cpu->PC, SWMASK('E')) &&
            ! (cpu->RUU & RUU_RIGHT_INSTR)) {
            return STOP_IBKPT;                  /* stop simulation */
        }

        if (cpu->PRP & cpu->MPRP) {
            /* There are interrupts pending in the peripheral
             * interrupt register */
            cpu->GRP |= GRP_SLAVE;
        }

        if (! iintr && ! (cpu->RUU & RUU_RIGHT_INSTR) &&
            ! (cpu->M[PSW] & PSW_INTR_DISABLE) &&
            (cpu->GRP & cpu->MGRP)) {
            /* external interrupt */
            op_int_2(cpu);
        }
        cpu_one_instr(cpu);                     /* one instr */
        iintr = 0;

        sim_interval -= 1;                      /* count down instructions */
    }
}

/*
 * A 250 Hz clock as per the original documentation,
 * and matching the available software binaries.
 * Some installations used 50 Hz with a modified OS
 * for a better user time/system time ratio.
 */
t_stat fast_clk(UNIT *this)
{
    static unsigned counter;
    static unsigned tty_counter;

    DEVICE *dev = find_dev_from_unit(this);
    if (! dev)
        return SCPE_IERR;

    CORE *cpu = (CORE*) dev->ctxt;
    if (! cpu)
        return SCPE_IERR;

    ++counter;
    ++tty_counter;

    cpu->GRP |= GRP_TIMER;

    if ((counter & 3) == 0) {
        /*
         * The OS used the (undocumented, later addition)
         * slow clock interrupt to initiate servicing
         * terminal I/O. Its frequency was reportedly about 50-60 Hz;
         * 16 ms is a good enough approximation.
         */
        cpu->GRP |= GRP_SLOW_CLK;
    }

    /* Baudot TTYs are synchronised to the main timer rather than the
     * serial line clock. Their baud rate is 50.
     */
    if (tty_counter == CLK_TPS/50) {
        tt_print();
        tty_counter = 0;
    }

    tmr_poll = sim_rtcn_calb(CLK_TPS, 0);               /* calibrate clock */
    return sim_activate_after(this, 1000000/CLK_TPS);   /* reactivate unit */
}

UNIT clocks[] = {
    { UDATA(fast_clk, UNIT_IDLE, 0), CLK_DELAY },   /* Bit 40 of the GRP, 250 Hz */
};

t_stat clk_reset(DEVICE *dev)
{
    sim_register_clock_unit(&clocks[0]);

    /* Схема автозапуска включается по нереализованной кнопке "МР" */

    if (!sim_is_running) {                              /* RESET (not IORESET)? */
        tmr_poll = sim_rtcn_init(clocks[0].wait, 0);    /* init timer */
        sim_activate(&clocks[0], tmr_poll);             /* activate unit */
    }
    return SCPE_OK;
}

DEVICE clock_dev = {
    "CLK", clocks, NULL, NULL,
    1, 0, 0, 0, 0, 0,
    NULL, NULL, &clk_reset,
    NULL, NULL, NULL, NULL,
    DEV_DEBUG
};
