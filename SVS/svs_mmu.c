/*
 * SVS memory management unit.
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
 * Трансляция виртуального адреса в физический.
 */
static int va_to_pa(CORE *cpu, int vaddr)
{
    int paddr;

    if (cpu->M[PSW] & PSW_MMAP_DISABLE) {
        /* Приписка отключена. */
        paddr = vaddr;
    } else {
        /* Приписка работает. */
        int vpage    = vaddr >> 10;
        int offset   = vaddr & BITS(10);
        int physpage = IS_SUPERVISOR(cpu->RUU) ?
                       cpu->STLB[vpage] : cpu->UTLB[vpage];

        paddr = (physpage << 10) | offset;
    }
    return paddr;
}

/*
 * Запись слова и тега в память по виртуальному адресу.
 * Возвращает физический адрес слова.
 */
static int mmu_store_with_tag(CORE *cpu, int vaddr, t_value val, uint8 t)
{
    vaddr &= BITS(15);
    if (vaddr == 0)
        return 0;

    mmu_protection_check(cpu, vaddr);

    /* Различаем адреса с припиской и без */
    if (cpu->M[PSW] & PSW_MMAP_DISABLE) {
        /* Приписка отключена. */
        if (vaddr < 010) {
            /* Игнорируем запись в тумблерные регистры. */
            if (svs_trace >= TRACE_INSTRUCTIONS) {
                fprintf(sim_log, "cpu%d --- Ignore write to pult register %d\n",
                    cpu->index, vaddr);
            }
            return 0;
        }
    } else {
        /* Приписка работает. */
        /* ЗПСЧ: ЗП */
        if (cpu->M[DWP] == vaddr && (cpu->M[PSW] & PSW_WRITE_WATCH))
            longjmp(cpu->exception, STOP_STORE_ADDR_MATCH);

        if (sim_brk_summ & SWMASK('W') &&
            sim_brk_test(vaddr, SWMASK('W')))
            longjmp(cpu->exception, STOP_WWATCH);
    }

    /* Вычисляем физический адрес. */
    int paddr = va_to_pa(cpu, vaddr);

    /* Пишем в память. */
    memory[paddr] = val;
    tag[paddr] = t;

    return paddr;
}

/*
 * Запись 48-битного слова в память.
 */
void mmu_store(CORE *cpu, int vaddr, t_value val)
{
    /* Вычисляем тег. */
    uint8 t = (cpu->RUU & (RUU_CHECK_RIGHT | RUU_CHECK_LEFT)) ?
        TAG_NUMBER48 : TAG_INSN48;

    int paddr = mmu_store_with_tag(cpu, vaddr, val, t);

    if (paddr != 0 && svs_trace >= TRACE_ALL) {
        fprintf(sim_log, "cpu%d       Memory Write [%05o %07o] = %02o:",
            cpu->index, vaddr, paddr, t);
        fprint_sym(sim_log, 0, &val, 0, 0);
        fprintf(sim_log, "\n");
    }
}

/*
 * Запись 64-битного слова в память.
 */
void mmu_store64(CORE *cpu, int vaddr, t_value val)
{
    int paddr = mmu_store_with_tag(cpu, vaddr, val, cpu->TagR);

    if (paddr != 0 && svs_trace >= TRACE_ALL) {
        fprintf(sim_log, "cpu%d       Memory Write [%05o %07o] = %02o:",
            cpu->index, vaddr, paddr, cpu->TagR);
        fprintf(sim_log, "%02o %04o:%04o %04o %04o %04o\n",
            (int) (val >> 60) & 017,
            (int) (val >> 48) & 07777,
            (int) (val >> 36) & 07777,
            (int) (val >> 24) & 07777,
            (int) (val >> 12) & 07777,
            (int) val & 07777);
    }
}

/*
 * Чтение операнда и тега из памяти по виртуальному адресу.
 * Возвращает физический адрес слова.
 */
static int mmu_load_with_tag(CORE *cpu, int vaddr, t_value *val, uint8 *t)
{
    vaddr &= BITS(15);
    if (vaddr == 0) {
        *val = 0;
        *t = 0;
        return 0;
    }

    mmu_protection_check(cpu, vaddr);

    /* Различаем адреса с припиской и без */
    if (cpu->M[PSW] & PSW_MMAP_DISABLE) {
        /* Приписка отключена. */
    } else {
        /* Приписка работает. */
        /* ЗПСЧ: СЧ */
        if (cpu->M[DWP] == vaddr && !(cpu->M[PSW] & PSW_WRITE_WATCH))
            longjmp(cpu->exception, STOP_LOAD_ADDR_MATCH);

        if (sim_brk_summ & SWMASK('R') &&
            sim_brk_test(vaddr, SWMASK('R')))
            longjmp(cpu->exception, STOP_RWATCH);
    }

    /* Вычисляем физический адрес слова */
    int paddr = va_to_pa(cpu, vaddr);

    if (paddr >= 010) {
        /* Из памяти */
        *val = memory[paddr];
        *t = tag[paddr];
    } else {
        /* С тумблерных регистров */
        *val = cpu->pult[paddr];
        *t = TAG_INSN48;
    }
    return paddr;
}

/*
 * Чтение 64-битного операнда.
 * Тег попадает в регистр тега.
 */
t_value mmu_load64(CORE *cpu, int vaddr, int tag_check)
{
    t_value val;
    uint8 t;
    int paddr = mmu_load_with_tag(cpu, vaddr, &val, &t);

    if (paddr != 0 && svs_trace >= TRACE_ALL) {
        if (paddr < 010)
            fprintf(sim_log, "cpu%d       Read  TR%o = ", cpu->index, paddr);
        else
            fprintf(sim_log, "cpu%d       Memory Read [%05o %07o] = %02o:",
                cpu->index, vaddr, paddr, t);
        fprintf(sim_log, "%02o %04o:%04o %04o %04o %04o\n",
            (int) (val >> 60) & 017,
            (int) (val >> 48) & 07777,
            (int) (val >> 36) & 07777,
            (int) (val >> 24) & 07777,
            (int) (val >> 12) & 07777,
            (int) val & 07777);
    }

    /* Прерывание (контроль числа), если попалось 48-битное слово. */
    if (tag_check && IS_48BIT(t) /*&& (mmu_unit.flags & CHECK_ENB)*/) {
        cpu->bad_addr = paddr & 7;
        svs_debug("--- (%05o) контроль числа", paddr);
        longjmp(cpu->exception, STOP_RAM_CHECK);
    }

    cpu->TagR = t;
    return val;
}

/*
 * Чтение 48-битного операнда.
 */
t_value mmu_load(CORE *cpu, int vaddr)
{
    t_value val;
    uint8 t;
    int paddr = mmu_load_with_tag(cpu, vaddr, &val, &t);

    if (paddr != 0 && svs_trace >= TRACE_ALL) {
        if (paddr < 010)
            fprintf(sim_log, "cpu%d       Read  TR%o = ", cpu->index, paddr);
        else
            fprintf(sim_log, "cpu%d       Memory Read [%05o %07o] = %02o:",
                cpu->index, vaddr, paddr, t);
        fprint_sym(sim_log, 0, &val, 0, 0);
        fprintf(sim_log, "\n");
    }

    /* Прерывание (контроль числа), если попалось 64-битное слово.
     * На тумблерных регистрах контроля числа не бывает. */
    if (paddr >= 010 && ! IS_48BIT(t) /*&& (mmu_unit.flags & CHECK_ENB)*/) {
        cpu->bad_addr = paddr & 7;
        svs_debug("--- (%05o) контроль числа", paddr);
        longjmp(cpu->exception, STOP_RAM_CHECK);
    }

    /* Тег не запоминаем. */
    return val & BITS48;
}

static void mmu_fetch_check(CORE *cpu, int vaddr)
{
    /* В режиме супервизора защиты нет */
    if (! IS_SUPERVISOR(cpu->RUU)) {
        int page = cpu->UTLB[vaddr >> 10];
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
    uint8 t;

    if (vaddr == 0) {
        if (cpu_dev[0].dctrl)
            svs_debug("--- передача управления на 0");
        longjmp(cpu->exception, STOP_INSN_CHECK);
    }

    mmu_fetch_check(cpu, vaddr);

    /* КРА */
    if (cpu->M[IBP] == vaddr && ! IS_SUPERVISOR(cpu->RUU))
        longjmp(cpu->exception, STOP_INSN_ADDR_MATCH);

    /* Вычисляем физический адрес слова */
    int paddr = IS_SUPERVISOR(cpu->RUU) ? vaddr : va_to_pa(cpu, vaddr);

    if (paddr >= 010) {
        /* Из памяти */
        val = memory[paddr];
        t = tag[paddr];
    } else {
        /* from switch regs */
        val = cpu->pult[paddr];
        t = TAG_INSN48;
    }

    if (svs_trace >= TRACE_INSTRUCTIONS && cpu_dev[0].dctrl &&
        ! (cpu->RUU & RUU_RIGHT_INSTR)) {
        // When both trace and cpu debug enabled,
        // print the fetch information.
        fprintf(sim_log, "cpu%d       Fetch [%05o %07o] = %o:",
            cpu->index, vaddr, paddr, t);
        fprint_sym(sim_log, 0, &val, 0, SWMASK('I'));
        fprintf(sim_log, "\n");
    }

    /* Прерывание (контроль команды), если попалась не 48-битная команда.
     * Тумблерные регистры только с командной сверткой. */
    if (paddr >= 010 && ! IS_INSN48(t)) {
        svs_debug("--- (%05o) контроль команды", vaddr);
        longjmp(cpu->exception, STOP_INSN_CHECK);
    }

    *paddrp = paddr;
    return val & BITS48;
}

void mmu_set_rp(CORE *cpu, int idx, t_value val, int supervisor)
{
    uint32 p0, p1, p2, p3;
    const uint32 mask = (MEMSIZE >> 10) - 1;

    /* Младшие 5 разрядов 4-х регистров приписки упакованы
     * по 5 в 1-20 рр, 6-е разряды - в 29-32 рр, 7-е разряды - в 33-36 рр и т.п.
     */
    p0 = (val       & 037) | (((val>>28) & 1) << 5) | (((val>>32) & 1) << 6) | (((val>>36) &  1) << 7) | (((val>>40) & 1) << 8) | (((val>>44) & 1) << 9);
    p1 = ((val>>5)  & 037) | (((val>>29) & 1) << 5) | (((val>>33) & 1) << 6) | (((val>>37) &  1) << 7) | (((val>>41) & 1) << 8) | (((val>>45) & 1) << 9);
    p2 = ((val>>10) & 037) | (((val>>30) & 1) << 5) | (((val>>34) & 1) << 6) | (((val>>38) &  1) << 7) | (((val>>42) & 1) << 8) | (((val>>46) & 1) << 9);
    p3 = ((val>>15) & 037) | (((val>>31) & 1) << 5) | (((val>>35) & 1) << 6) | (((val>>39) &  1) << 7) | (((val>>43) & 1) << 8) | (((val>>47) & 1) << 9);

    p0 &= mask;
    p1 &= mask;
    p2 &= mask;
    p3 &= mask;

    if (supervisor) {
        cpu->RPS[idx] = p0 | p1 << 12 | (t_value)p2 << 24 | (t_value)p3 << 36;
        cpu->STLB[idx*4] = p0;
        cpu->STLB[idx*4+1] = p1;
        cpu->STLB[idx*4+2] = p2;
        cpu->STLB[idx*4+3] = p3;
    } else {
        cpu->RP[idx] = p0 | p1 << 12 | (t_value)p2 << 24 | (t_value)p3 << 36;
        cpu->UTLB[idx*4] = p0;
        cpu->UTLB[idx*4+1] = p1;
        cpu->UTLB[idx*4+2] = p2;
        cpu->UTLB[idx*4+3] = p3;
    }
}

void mmu_setup(CORE *cpu)
{
    const uint32 mask = (MEMSIZE >> 10) - 1;
    int i;

    /* Перепись РПi в TLBj. */
    for (i=0; i<8; ++i) {
        cpu->UTLB[i*4] = cpu->RP[i] & mask;
        cpu->UTLB[i*4+1] = cpu->RP[i] >> 12 & mask;
        cpu->UTLB[i*4+2] = cpu->RP[i] >> 24 & mask;
        cpu->UTLB[i*4+3] = cpu->RP[i] >> 36 & mask;
        cpu->STLB[i*4] = cpu->RPS[i] & mask;
        cpu->STLB[i*4+1] = cpu->RPS[i] >> 12 & mask;
        cpu->STLB[i*4+2] = cpu->RPS[i] >> 24 & mask;
        cpu->STLB[i*4+3] = cpu->RPS[i] >> 36 & mask;
    }
}

void mmu_set_protection(CORE *cpu, int idx, t_value val)
{
    /* Разряды сумматора, записываемые в регистр защиты - 21-28 */
    int mask = 0xff << (idx * 8);

    val = ((val >> 20) & 0xff) << (idx * 8);
    cpu->RZ = (uint32)((cpu->RZ & ~mask) | val);
}
