/*
 * SVS fast write cache and TLB registers
 *（стойка БРУС)
 *
 * Copyright (c) 2009, Leonid Broukhis
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

/*
 * MMU data structures
 *
 * mmu_dev      MMU device descriptor
 * mmu_unit     MMU unit descriptor
 * mmu_reg      MMU register list
 */
UNIT mmu_unit = {
    UDATA(NULL, UNIT_FIX, 8)
};

/*
 * 64-битные регистры RP0-RP7 - для отображения регистров приписки,
 * группами по 4 ради компактности, 12 бит на страницу.
 * TLB0-TLB31 - постраничные регистры приписки, копии RPi.
 * Обращение к памяти должно вестись через TLBi.
 */
t_value RP[8];
uint32 TLB[32];

uint32 RZ;

uint32 iintr_data;    /* protected page number or parity check location */

/* There were several hardwired configurations of registers
 * corresponding to up to 7 first words of the memory space, selected by
 * a packet switch. Here selection 0 corresponds to settable switch registers,
 * the others are hardwired.
 * The configuration is selected with "SET CPU PULT=N" where 0 <= N <= 10
 * is the configuration number.
 */
unsigned pult_packet_switch;

/* Location 0 of each configuration is the bitset of its hardwired locations */
t_value pult[11][8] = {
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

#define ORDATAVM(nm,loc,wd) REGDATA(nm,(loc),8,wd,0,1,NULL,NULL,REG_VMIO,0,0)
#define ORDATAH(nm,loc,wd) REGDATA(nm,(loc),8,wd,0,1,NULL,NULL,REG_HIDDEN,0,0)

REG mmu_reg[] = {
    { ORDATAVM ( "РП0",   RP[0],      48) },                      /* Регистры приписки, по 12 бит */
    { ORDATAVM ( "РП1",   RP[1],      48) },
    { ORDATAVM ( "РП2",   RP[2],      48) },
    { ORDATAVM ( "РП3",   RP[3],      48) },
    { ORDATAVM ( "РП4",   RP[4],      48) },
    { ORDATAVM ( "РП5",   RP[5],      48) },
    { ORDATAVM ( "РП6",   RP[6],      48) },
    { ORDATAVM ( "РП7",   RP[7],      48) },
    { ORDATA   ( "РЗ",    RZ,         32) },                      /* Регистр защиты */
    { ORDATAVM ( "ТР1",   pult[0][1], 50) },                      /* Тумблерные регистры */
    { ORDATAVM ( "ТР2",   pult[0][2], 50) },
    { ORDATAVM ( "ТР3",   pult[0][3], 50) },
    { ORDATAVM ( "ТР4",   pult[0][4], 50) },
    { ORDATAVM ( "ТР5",   pult[0][5], 50) },
    { ORDATAVM ( "ТР6",   pult[0][6], 50) },
    { ORDATAVM ( "ТР7",   pult[0][7], 50) },
    { 0 }
};

#define CHECK_ENB 2

MTAB mmu_mod[] = {
    { 2, 0, "NOCHECK", "NOCHECK" },
    { 2, 2, "CHECK",   "CHECK" },
    { 0 }
};

t_stat mmu_reset(DEVICE *dptr);

t_stat mmu_examine(t_value *vptr, t_addr addr, UNIT *uptr, int32 sw)
{
    //mmu_print_brz();
    return SCPE_NOFNC;
}

DEVICE mmu_dev = {
    "MMU", &mmu_unit, mmu_reg, mmu_mod,
    1, 8, 3, 1, 8, 50,
    &mmu_examine, NULL, &mmu_reset,
    NULL, NULL, NULL, NULL,
    DEV_DEBUG
};

/*
 * Reset routine
 */
t_stat mmu_reset(DEVICE *dptr)
{
    int i;
    for (i = 0; i < 8; ++i) {
        RP[i] = 0;
    }
    RZ = 0;
    /*
     * Front panel switches survive the reset
     */
    sim_cancel(&mmu_unit);
    return SCPE_OK;
}

void mmu_protection_check(int addr)
{
    /* Защита блокируется в режиме супервизора для физических (!) адресов 1-7 (ТО-8) - WTF? */
    int tmp_prot_disabled = (M[PSW] & PSW_PROT_DISABLE) ||
        (IS_SUPERVISOR(RUU) && (M[PSW] & PSW_MMAP_DISABLE) && addr < 010);

    /* Защита не заблокирована, а лист закрыт */
    if (! tmp_prot_disabled && (RZ & (1 << (addr >> 10)))) {
        iintr_data = addr >> 10;
        if (mmu_dev.dctrl)
            svs_debug("--- (%05o) защита числа", addr);
        longjmp(cpu_halt, STOP_OPERAND_PROT);
    }
}

/*
 * Запись слова в память
 */
void mmu_store(int addr, t_value val)
{
    int matching;

    addr &= BITS(15);
    if (addr == 0)
        return;
    if (sim_log && mmu_dev.dctrl) {
        fprintf(sim_log, "--- (%05o) запись ", addr);
        fprint_sym(sim_log, 0, &val, 0, 0);
        fprintf(sim_log, "\n");
    }

    mmu_protection_check(addr);

    /* Различаем адреса с припиской и без */
    if (M[PSW] & PSW_MMAP_DISABLE)
        addr |= 0100000;

    /* ЗПСЧ: ЗП */
    if (M[DWP] == addr && (M[PSW] & PSW_WRITE_WATCH))
        longjmp(cpu_halt, STOP_STORE_ADDR_MATCH);

    if (sim_brk_summ & SWMASK('W') &&
        sim_brk_test(addr, SWMASK('W')))
        longjmp(cpu_halt, STOP_WWATCH);
}

t_value mmu_memaccess(int addr)
{
    t_value val;

    /* Вычисляем физический адрес слова */
    addr = (addr > 0100000) ? (addr - 0100000) :
        (addr & 01777) | (TLB[addr >> 10] << 10);
    if (addr >= 010) {
        /* Из памяти */
        val = memory[addr];
    } else {
        /* С тумблерных регистров */
        if (mmu_dev.dctrl)
            svs_debug("--- (%05o) чтение ТР%o", PC, addr);
        if ((pult[pult_packet_switch][0] >> addr) & 1) {
            /* hardwired */
            val = pult[pult_packet_switch][addr];
        } else {
            /* from switch regs */
            val = pult[0][addr];
        }
    }
    if (sim_log && (mmu_dev.dctrl || (cpu_dev.dctrl && sim_deb))) {
        fprintf(sim_log, "--- (%05o) чтение ", addr & BITS(15));
        fprint_sym(sim_log, 0, &val, 0, 0);
        fprintf(sim_log, "\n");
    }

    /* На тумблерных регистрах контроля числа не бывает */
    if (addr >= 010 && ! IS_NUMBER(val) && (mmu_unit.flags & CHECK_ENB)) {
        iintr_data = addr & 7;
        svs_debug("--- (%05o) контроль числа", addr);
        longjmp(cpu_halt, STOP_RAM_CHECK);
    }
    return val;
}

/*
 * Чтение операнда
 */
t_value mmu_load(int addr)
{
    int matching = -1;
    t_value val;

    addr &= BITS(15);
    if (addr == 0)
        return 0;

    mmu_protection_check(addr);

    /* Различаем адреса с припиской и без */
    if (M[PSW] & PSW_MMAP_DISABLE)
        addr |= 0100000;

    /* ЗПСЧ: СЧ */
    if (M[DWP] == addr && !(M[PSW] & PSW_WRITE_WATCH))
        longjmp(cpu_halt, STOP_LOAD_ADDR_MATCH);

    if (sim_brk_summ & SWMASK('R') &&
        sim_brk_test(addr, SWMASK('R')))
        longjmp(cpu_halt, STOP_RWATCH);

    return mmu_memaccess(addr) & BITS48;
}

void mmu_fetch_check(int addr)
{
    /* В режиме супервизора защиты нет */
    if (! IS_SUPERVISOR(RUU)) {
        int page = TLB[addr >> 10];
        /*
         * Для команд в режиме пользователя признак защиты -
         * 0 в регистре приписки.
         */
        if (page == 0) {
            iintr_data = addr >> 10;
            if (mmu_dev.dctrl)
                svs_debug("--- (%05o) защита команды", addr);
            longjmp(cpu_halt, STOP_INSN_PROT);
        }
    }
}

/*
 * Предвыборка команды на БРС
 */
t_value mmu_prefetch(int addr, int actual)
{
    t_value val;
    int i;

    if (!actual) {
        return 0;
    } else {
        /* Чтобы лампочки мигали */
        i = addr & 3;
    }

    if (addr < 0100000) {
        int page = TLB[addr >> 10];

        /* Вычисляем физический адрес слова */
        addr = (addr & 01777) | (page << 10);
    } else {
        addr = addr & BITS(15);
    }

    if (addr < 010) {
        if ((pult[pult_packet_switch][0] >> addr) & 1) {
            /* hardwired */
            val = pult[pult_packet_switch][addr];
        } else {
            /* from switch regs */
            val = pult[0][addr];
        }
    } else
        val = memory[addr];

    return val;
}

/*
 * Выборка команды
 */
t_value mmu_fetch(int addr)
{
    t_value val;

    if (addr == 0) {
        if (mmu_dev.dctrl)
            svs_debug("--- передача управления на 0");
        longjmp(cpu_halt, STOP_INSN_CHECK);
    }

    mmu_fetch_check(addr);

    /* Различаем адреса с припиской и без */
    if (IS_SUPERVISOR(RUU))
        addr |= 0100000;

    /* КРА */
    if (M[IBP] == addr)
        longjmp(cpu_halt, STOP_INSN_ADDR_MATCH);

    val = mmu_prefetch(addr, 1);

    if (sim_log && mmu_dev.dctrl) {
        fprintf(sim_log, "--- (%05o) выборка ", addr);
        fprint_sym(sim_log, 0, &val, 0, SWMASK('I'));
        fprintf(sim_log, "\n");
    }

    /* Тумблерные регистры пока только с командной сверткой */
    if (addr >= 010 && ! IS_INSN(val)) {
        svs_debug("--- (%05o) контроль команды", addr);
        longjmp(cpu_halt, STOP_INSN_CHECK);
    }
    return val & BITS48;
}

void mmu_setrp(int idx, t_value val)
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

    RP[idx] = p0 | p1 << 12 | p2 << 24 | (t_value) p3 << 36;
    TLB[idx*4] = p0;
    TLB[idx*4+1] = p1;
    TLB[idx*4+2] = p2;
    TLB[idx*4+3] = p3;
}

void mmu_setup()
{
    const uint32 mask = (MEMSIZE >> 10) - 1;
    int i;

    /* Перепись РПi в TLBj. */
    for (i=0; i<8; ++i) {
        TLB[i*4] = RP[i] & mask;
        TLB[i*4+1] = RP[i] >> 12 & mask;
        TLB[i*4+2] = RP[i] >> 24 & mask;
        TLB[i*4+3] = RP[i] >> 36 & mask;
    }
}

void mmu_setprotection(int idx, t_value val)
{
    /* Разряды сумматора, записываемые в регистр защиты - 21-28 */
    int mask = 0xff << (idx * 8);
    val = ((val >> 20) & 0xff) << (idx * 8);
    RZ = (uint32)((RZ & ~mask) | val);
}
