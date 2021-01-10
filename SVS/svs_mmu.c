/*
 * SVS TLB registers.
 *
 * Copyright (c) 2009, Leonid Broukhis
 * Copyright (c) 2017, Serge Vakulenko
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

/* There were several hardwired configurations of registers
 * corresponding to up to 7 first words of the memory space, selected by
 * a packet switch. Here selection 0 corresponds to settable switch registers,
 * the others are hardwired.
 * The configuration is selected with "SET CPU PULT=N" where 0 <= N <= 10
 * is the configuration number.
 */

/* Location 0 of each configuration is the bitset of its hardwired locations */
t_value pult_tab[11][8] = {
/* Switch registers */
    { 0 },
/* Hardwired program 1, a simple CU test */
    { 0376,
      SET_PARITY(01240000007100002LL, PARITY_INSN),   /* 1: vtm (2), vjm 2(1) */
      SET_PARITY(00657777712577777LL, PARITY_INSN),   /* 2: utm -1(1), utm -1(2) */
      SET_PARITY(00444000317400007LL, PARITY_INSN),   /* 3: mtj 3(1), vzm 7(3) */
      SET_PARITY(01045000317500007LL, PARITY_INSN),   /* 4: j+m 3(2), v1m 7(3)*/
      SET_PARITY(00650000107700002LL, PARITY_INSN),   /* 5: utm 1(1), vlm 2(1) */
      SET_PARITY(01257777713400001LL, PARITY_INSN),   /* 6: utm -1(2), vzm 1(2) */
      SET_PARITY(00330000003000001LL, PARITY_INSN)    /* 7: stop, uj 1 */
    },
/* Hardwired program 2, RAM write test. The "arx" insn (cyclic add)
 * in word 3 could be changed to "atx" insn (load) to use a constant
 * bit pattern with a "constant/variable code" front panel switch (TODO).
 * The bit pattern to use is taken from switch register 7.
 */
    { 0176,
      SET_PARITY(00770000306400012LL, PARITY_INSN), /* 1: vlm 3(1), vtm 12(1) */
      SET_PARITY(00010000000000010LL, PARITY_INSN), /* 2: xta 0, atx 10 */
      SET_PARITY(00010001000130007LL, PARITY_INSN), /* 3: xta 10, arx 7 */
      SET_PARITY(00500777700000010LL, PARITY_INSN), /* 4: atx -1(1), atx 10 */
      SET_PARITY(00512777702600001LL, PARITY_INSN), /* 5: aex -1(1), uza 1 */
      SET_PARITY(00737777703000001LL, PARITY_INSN)  /* 6: stop -1(1), uj 1 */
    },
/* Hardwired program 3, RAM read test to use after program 2, arx/atx applies */
    { 0176,
      SET_PARITY(00770000306400012LL, PARITY_INSN), /* 1: vlm 3(1), vtm 12(1) */
      SET_PARITY(00010000000000010LL, PARITY_INSN), /* 2: xta 0, atx 10 */
      SET_PARITY(00010001000130007LL, PARITY_INSN), /* 3: xta 10, arx 7 */
      SET_PARITY(00000000000000010LL, PARITY_INSN), /* 4: atx 0, atx 10 */
      SET_PARITY(00512777702600001LL, PARITY_INSN), /* 5: aex -1(1), uza 1 */
      SET_PARITY(00737777703000001LL, PARITY_INSN)  /* 6: stop -1(1), uj 1 */
    },
/* Hardwired program 4, RAM write-read test to use after program 2, arx/atx applies */
    { 0176,
      SET_PARITY(00640001200100011LL, PARITY_INSN), /* 1: vtm 12(1), xta 11 */
      SET_PARITY(00000001005127777LL, PARITY_INSN), /* 2: atx 10, aex -1(1) */
      SET_PARITY(00260000407377777LL, PARITY_INSN), /* 3: uza 4, stop -1(1) */
      SET_PARITY(00010001000130007LL, PARITY_INSN), /* 4: xta 10, arx 7 */
      SET_PARITY(00500777707700002LL, PARITY_INSN), /* 5: atx -1(1), vlm 2(1) */
      SET_PARITY(00300000100000000LL, PARITY_INSN)  /* 6: uj 1 */
    },
/* Hardwired program 5, ALU test; switch reg 7 should contain a
   normalized f. p. value, e.g. 1.0 = 4050 0000 0000 0000 */
    { 0176,
      SET_PARITY(00004000700000011LL, PARITY_INSN), /* 1: a+x 7, atx 11 */
      SET_PARITY(00025001100000010LL, PARITY_INSN), /* 2: e-x 11, atx 10 */
      SET_PARITY(00017001000160010LL, PARITY_INSN), /* 3: a*x 10, a/x 10 */
      SET_PARITY(00005001000340145LL, PARITY_INSN), /* 4: a-x 10, e+n 145 */
      SET_PARITY(00270000603300000LL, PARITY_INSN), /* 5: u1a 6, stop */
      SET_PARITY(00010001103000001LL, PARITY_INSN)  /* 6: xta 11, uj 1*/
    },
/* Hardwired program 6, reading from punch tape (originally) or a disk (rework);
 * various bit groups not hardwired, marked [] (TODO). Disk operation is encoded.
 */
    { 0376,
      SET_PARITY(00640000300100006LL, PARITY_INSN), /* 1: vtm [3](1), xta 6 */
      SET_PARITY(00433002004330020LL, PARITY_INSN), /* 2: ext 20(1), ext 20(1) */
      SET_PARITY(00036015204330020LL, PARITY_INSN), /* 3: asn 152, ext 20(1) */
      SET_PARITY(00010000704330000LL, PARITY_INSN), /* 4: xta 7, ext (1) */
      SET_PARITY(00036014404330020LL, PARITY_INSN), /* 5: asn 144, ext 20(1) */
      SET_PARITY(00330000000002401LL, PARITY_INSN), /* 6: stop, =24[01] */
      SET_PARITY(04000000001400000LL, PARITY_NUMBER) /* 7: bits 37-47 not hardwired */
    },
/* Hardwired program 7, RAM peek/poke, bits 1-15 of word 1 not hardwired (TODO) */
    { 0176,
    },
/* Hardwired program 8, reading the test program from a fixed drum location */
    { 0036,
    },
/* Hardwired program 9, drum I/O */
    { 0176,
      SET_PARITY(00647774100100007LL, PARITY_INSN), /* 1: vtm -31(1), xta 7 */
      SET_PARITY(00033000212460000LL, PARITY_INSN), /* 2: ext 2, vtm 60000(2) */
      SET_PARITY(00040000013700003LL, PARITY_INSN), /* 3: ati, vlm 3(2) */
      SET_PARITY(00013000607700002LL, PARITY_INSN), /* 4: arx 6, vlm 2(1) */
      SET_PARITY(00330000103000005LL, PARITY_INSN), /* 5: stop 1, uj 5 */
      SET_PARITY(00000000000010004LL, PARITY_NUMBER) /* 6: =10004 */
    },
/* Hardwired program 10, magtape read */
    { 0176,
    },
};

static void mmu_protection_check(CORE *cpu, int vaddr)
{
    /* Защита блокируется в режиме супервизора для физических (!) адресов 1-7 (ТО-8) - WTF? */
    int tmp_prot_disabled = (cpu->M[PSW] & PSW_PROT_DISABLE) ||
        (IS_SUPERVISOR(cpu->RUU) && (cpu->M[PSW] & PSW_MMAP_DISABLE) && vaddr < 010);

    /* Защита не заблокирована, а лист закрыт */
    if (! tmp_prot_disabled && (cpu->RZ & (1 << (vaddr >> 10)))) {
        cpu->bad_addr = vaddr >> 10;
        if (cpu_dev[0].dctrl)
            svs_debug("--- (%05o) защита числа", vaddr);
        longjmp(cpu->exception, STOP_OPERAND_PROT);
    }
}

/*
 * Запись слова в память
 */
void mmu_store(CORE *cpu, int vaddr, t_value val)
{
    vaddr &= BITS(15);
    if (vaddr == 0)
        return;

    mmu_protection_check(cpu, vaddr);

    /* Различаем адреса с припиской и без */
    if (cpu->M[PSW] & PSW_MMAP_DISABLE)
        vaddr |= 0100000;

    /* ЗПСЧ: ЗП */
    if (cpu->M[DWP] == vaddr && (cpu->M[PSW] & PSW_WRITE_WATCH))
        longjmp(cpu->exception, STOP_STORE_ADDR_MATCH);

    if (sim_brk_summ & SWMASK('W') &&
        sim_brk_test(vaddr, SWMASK('W')))
        longjmp(cpu->exception, STOP_WWATCH);

    if (vaddr >= 0100000 && vaddr < 0100010) {
        /* Игнорируем запись в тумблерные регистры. */
        if (svs_trace >= TRACE_INSTRUCTIONS) {
            fprintf(sim_log, "cpu%d --- Ignore write to pult register %d\n",
                cpu->index, vaddr - 0100000);
        }
        return;
    }

    /* Вычисляем физический адрес. */
    int paddr = (vaddr >= 0100000) ? (vaddr - 0100000) :
        (vaddr & 01777) | (cpu->TLB[vaddr >> 10] << 10);

    /* Вычисляем тег. */
    val = SET_PARITY(val, cpu->RUU ^ PARITY_INSN);

    /* Пишем в память. */
    memory[paddr] = val;

    if (svs_trace >= TRACE_ALL) {
        fprintf(sim_log, "cpu%d       Memory Write [%05o %07o] = %o:",
            cpu->index, vaddr & BITS(15), paddr, (int)(val >> 48));
        fprint_sym(sim_log, 0, &val, 0, 0);
        fprintf(sim_log, "\n");
    }
}

static t_value mmu_memaccess(CORE *cpu, int vaddr)
{
    t_value val;

    /* Вычисляем физический адрес слова */
    int paddr = (vaddr >= 0100000) ? (vaddr - 0100000) :
        (vaddr & 01777) | (cpu->TLB[vaddr >> 10] << 10);

    if (paddr >= 010) {
        /* Из памяти */
        val = memory[paddr];
    } else {
        /* С тумблерных регистров */
        if ((pult_tab[cpu->pult_switch][0] >> paddr) & 1) {
            /* hardwired */
            val = pult_tab[cpu->pult_switch][paddr];
        } else {
            /* from switch regs */
            val = cpu->pult[paddr];
        }
    }

    if (svs_trace >= TRACE_ALL) {
        if (paddr < 010)
            fprintf(sim_log, "cpu%d       Read  TR%o = ", cpu->index, paddr);
        else
            fprintf(sim_log, "cpu%d       Memory Read [%05o %07o] = %o:",
                cpu->index, vaddr & BITS(15), paddr, (int)(val >> 48));
        fprint_sym(sim_log, 0, &val, 0, 0);
        fprintf(sim_log, "\n");
    }

    /* На тумблерных регистрах контроля числа не бывает */
    if (paddr >= 010 && ! IS_NUMBER(val) /*&& (mmu_unit.flags & CHECK_ENB)*/) {
        cpu->bad_addr = paddr & 7;
        svs_debug("--- (%05o) контроль числа", paddr);
        longjmp(cpu->exception, STOP_RAM_CHECK);
    }
    return val;
}

/*
 * Чтение операнда
 */
t_value mmu_load(CORE *cpu, int vaddr)
{
    vaddr &= BITS(15);
    if (vaddr == 0)
        return 0;

    mmu_protection_check(cpu, vaddr);

    /* Различаем адреса с припиской и без */
    if (cpu->M[PSW] & PSW_MMAP_DISABLE)
        vaddr |= 0100000;

    /* ЗПСЧ: СЧ */
    if (cpu->M[DWP] == vaddr && !(cpu->M[PSW] & PSW_WRITE_WATCH))
        longjmp(cpu->exception, STOP_LOAD_ADDR_MATCH);

    if (sim_brk_summ & SWMASK('R') &&
        sim_brk_test(vaddr, SWMASK('R')))
        longjmp(cpu->exception, STOP_RWATCH);

    return mmu_memaccess(cpu, vaddr) & BITS48;
}

static void mmu_fetch_check(CORE *cpu, int vaddr)
{
    /* В режиме супервизора защиты нет */
    if (! IS_SUPERVISOR(cpu->RUU)) {
        int page = cpu->TLB[vaddr >> 10];
        /*
         * Для команд в режиме пользователя признак защиты -
         * 0 в регистре приписки.
         */
        if (page == 0) {
            cpu->bad_addr = vaddr >> 10;
            if (cpu_dev[0].dctrl)
                svs_debug("--- (%05o) защита команды", vaddr);
            longjmp(cpu->exception, STOP_INSN_PROT);
        }
    }
}

/*
 * Выборка команды
 */
t_value mmu_fetch(CORE *cpu, int vaddr, int *paddrp)
{
    t_value val;

    if (vaddr == 0) {
        if (cpu_dev[0].dctrl)
            svs_debug("--- передача управления на 0");
        longjmp(cpu->exception, STOP_INSN_CHECK);
    }

    mmu_fetch_check(cpu, vaddr);

    /* Различаем адреса с припиской и без */
    if (IS_SUPERVISOR(cpu->RUU))
        vaddr |= 0100000;

    /* КРА */
    if (cpu->M[IBP] == vaddr)
        longjmp(cpu->exception, STOP_INSN_ADDR_MATCH);

    /* Вычисляем физический адрес слова */
    int paddr = (vaddr >= 0100000) ? (vaddr - 0100000) :
        (vaddr & 01777) | (cpu->TLB[vaddr >> 10] << 10);

    if (paddr >= 010) {
        /* Из памяти */
        val = memory[paddr];
    } else {
        if ((pult_tab[cpu->pult_switch][0] >> paddr) & 1) {
            /* hardwired */
            val = pult_tab[cpu->pult_switch][paddr];
        } else {
            /* from switch regs */
            val = cpu->pult[paddr];
        }
    }

    if (svs_trace >= TRACE_INSTRUCTIONS && cpu_dev[0].dctrl &&
        ! (cpu->RUU & RUU_RIGHT_INSTR)) {
        // When both trace and cpu debug enabled,
        // print the fetch information.
        fprintf(sim_log, "cpu%d       Fetch [%05o %07o] = %o:",
            cpu->index, vaddr & BITS(15), paddr, (int)(val >> 48));
        fprint_sym(sim_log, 0, &val, 0, SWMASK('I'));
        fprintf(sim_log, "\n");
    }

    /* Тумблерные регистры пока только с командной сверткой */
    if (paddr >= 010 && ! IS_INSN(val)) {
        svs_debug("--- (%05o) контроль команды", vaddr);
        longjmp(cpu->exception, STOP_INSN_CHECK);
    }

    *paddrp = paddr;
    return val & BITS48;
}

void mmu_set_rp(CORE *cpu, int idx, t_value val)
{
    uint32 p0, p1, p2, p3;
    const uint32 mask = (MEMSIZE >> 10) - 1;

    /* Младшие 5 разрядов 4-х регистров приписки упакованы
     * по 5 в 1-20 рр, 6-е разряды - в 29-32 рр, 7-е разряды - в 33-36 рр и т.п.
     */
    p0 = (val       & 037) | (((val>>28) & 1) << 5) | (((val>>32) & 1) << 6) |
        (((val>>36) &  1) << 7) | (((val>>40) & 1) << 8) | (((val>>44) & 1) << 9);
    p1 = ((val>>5)  & 037) | (((val>>29) & 1) << 5) | (((val>>33) & 1) << 6) |
        (((val>>37) &  1) << 7) | (((val>>41) & 1) << 8) | (((val>>45) & 1) << 9);
    p2 = ((val>>10) & 037) | (((val>>30) & 1) << 5) | (((val>>34) & 1) << 6) |
        (((val>>38) &  1) << 7) | (((val>>42) & 1) << 8) | (((val>>46) & 1) << 9);
    p3 = ((val>>15) & 037) | (((val>>31) & 1) << 5) | (((val>>35) & 1) << 6) |
        (((val>>39) &  1) << 7) | (((val>>43) & 1) << 8) | (((val>>47) & 1) << 9);

    p0 &= mask;
    p1 &= mask;
    p2 &= mask;
    p3 &= mask;

    cpu->RP[idx] = p0 | p1 << 12 | p2 << 24 | (t_value) p3 << 36;
    cpu->TLB[idx*4] = p0;
    cpu->TLB[idx*4+1] = p1;
    cpu->TLB[idx*4+2] = p2;
    cpu->TLB[idx*4+3] = p3;
}

void mmu_setup(CORE *cpu)
{
    const uint32 mask = (MEMSIZE >> 10) - 1;
    int i;

    /* Перепись РПi в TLBj. */
    for (i=0; i<8; ++i) {
        cpu->TLB[i*4] = cpu->RP[i] & mask;
        cpu->TLB[i*4+1] = cpu->RP[i] >> 12 & mask;
        cpu->TLB[i*4+2] = cpu->RP[i] >> 24 & mask;
        cpu->TLB[i*4+3] = cpu->RP[i] >> 36 & mask;
    }
}

void mmu_set_protection(CORE *cpu, int idx, t_value val)
{
    /* Разряды сумматора, записываемые в регистр защиты - 21-28 */
    int mask = 0xff << (idx * 8);

    val = ((val >> 20) & 0xff) << (idx * 8);
    cpu->RZ = (uint32)((cpu->RZ & ~mask) | val);
}
