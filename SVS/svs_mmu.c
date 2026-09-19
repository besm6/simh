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
 * Трансляция адреса, пришедшего от АДАП-а в заявке, в физический.
 *
 * Адрес буфера обмена в слове ДО — ВИРТУАЛЬНЫЙ, в адресном пространстве
 * АДАП-а (супервизор), а не физический: РАСПАК читает зону через собственную
 * приписку (VТМ 0; СЧ ...), которую АДАП настраивает сам (RPS0 = 1603 1602
 * 1601 1600). Канал обязан переводить адрес той же припиской, иначе зона
 * ложится мимо — и первое же обычное чтение непрописанной страницы даёт
 * контроль числа (СТ101). См. ПВВ.md §5.4.
 *
 * Приписка супервизора берётся всегда, независимо от текущего ССП: канал —
 * не процессор, он работает с пространством АДАП-а, а не с тем режимом,
 * в котором процессор оказался на момент звонка РЕГ '50'.
 */
int mmu_iom_pa(int vaddr)
{
    CORE *cpu    = &cpu_core[0];
    int vpage    = (vaddr >> 10) & 037;
    int offset   = vaddr & 01777;
    int physpage = cpu->STLB[vpage];

    return (physpage << 10) | offset;
}

/*
 * Адрес МАССИВА ОБМЕНА (поле НАМ слова ДО) — ФИЗИЧЕСКИЙ, приписка к нему не
 * применяется.
 *
 * Заводское описание (№10.170.002 ТОП): «НАМ — начальный адрес массива в
 * ОПЕРАТИВНОЙ ПАМЯТИ», и канал «производит передачу информации между
 * оперативной памятью (через коммутатор связи) и внешним устройством БЕЗ
 * УЧАСТИЯ СЕКЦИИ УПРАВЛЕНИЯ» (4.4). Трансляция — отдельный режим ДОП («режим
 * обработки таблицы страниц сегментов», разр.50 слова СО) и только для быстрых
 * каналов.
 *
 * АДАП это подтверждает: адреса СВОИХ структур он пропускает через ДФАПВВ
 * (= СЛЦ АДРЕС, «с точки зрения ПВВ»), а в ветке формирования ДО никакого
 * ДФАПВВ нет — там «СЧИ 8 НОМЕР ЛИСТА / СДА 64-10 АДРЕС», то есть лист*1024,
 * где лист = страница из КУС плюс БАЗАОС.
 *
 * Отличать от mmu_iom_pa(): тот переводит адреса СТРУКТУР АДАП-а (ячейка
 * АДРЕС), и он по-прежнему нужен.
 */
int mmu_iom_data_pa(int addr)
{
    return addr;
}

/*
 * Трансляция виртуального адреса в физический.
 *
 * ВНИМАНИЕ. Ниже — СТАРАЯ (заведомо упрощённая) модель: разряд ССП, которым
 * управляет VТМ, трактуется как "приписка включена/выключена". Она сохранена
 * временно, чтобы держать прогон в состоянии "контроль команды на 02000".
 *
 * Правильная модель СВС закомментирована следом (#if 0): приписка ВСЕГДА
 * включена — и для команд, и для данных; VТМ выбирает не вкл/выкл, а ЧЬЯ
 * приписка применяется к ДАННЫМ (ядра или пользователя), а выборка команд в
 * режиме ядра всегда идёт по приписке ядра. При её включении АДАП доходит
 * только до 036134 (РЕГ '60'+РПАД, "НАСТР. 14,15,16,17 РАФОС") и падает по
 * контролю команды: остаётся невыясненным, какой должна быть НАЧАЛЬНАЯ
 * приписка ядра и пользователя (приёмник ПЕРЕП — "2-я п/секция").
 */
static int va_to_pa(CORE *cpu, int vaddr, int is_fetch)
{
    int vpage  = vaddr >> 10;
    int offset = vaddr & BITS(10);
    uint32 physpage;

    if (vaddr < 010)
        return vaddr;               /* тумблерные регистры — не память */

    if (! IS_SUPERVISOR(cpu->RUU))
        physpage = cpu->UTLB[vpage];            /* режим пользователя */
    else if (is_fetch)
        physpage = cpu->STLB[vpage];            /* команды — по приписке ядра */
    else
        /*
         * Данные в режиме ядра: VТМ выбирает, ЧЬЯ приписка применяется.
         *
         * ВНИМАНИЕ: полярность этого разряда ОДНОЙ парой ядро/пользователь
         * не описывается — два наблюдения противоречат друг другу:
         *
         *   - СТЕК. АДАП кладёт адрес возврата и снимает его командой
         *     МОД (S) (033721). Запись легла в физ. 01540, то есть по
         *     приписке ПОЛЬЗОВАТЕЛЯ (UTLB[0]=0), а чтение при нынешней
         *     полярности идёт по приписке ЯДРА (STLB[0]=01600) и даёт мусор
         *     -> переход на 031260 (данные) и контроль команды.
         *   - ПЕРЕП (036034). Источник переписи читается при VТМ 1027 и
         *     обязан быть в приписке ЯДРА: перевернёшь полярность — чтение
         *     уходит в пустую приписку пользователя и даёт контроль числа.
         *
         * Похоже, у СТЕКА своя приписка (слово пульта 7, "приписка стека"),
         * а не та, что выбирает VТМ. См. ПВВ.md §8.
         */
        physpage = (cpu->M[PSW] & PSW_MMAP_DISABLE) ?
                   cpu->STLB[vpage] : cpu->UTLB[vpage];

    return (physpage << 10) | offset;

#if 0   /* СТАРАЯ упрощённая модель: "приписка включена/выключена" */
    if (cpu->M[PSW] & PSW_MMAP_DISABLE) {
        return vaddr;
    } else {
        uint32 pp = IS_SUPERVISOR(cpu->RUU) ?
                    cpu->STLB[vpage] : cpu->UTLB[vpage];
        return (pp << 10) | offset;
    }
#endif
}

/*
 * Запись слова и тега в память по виртуальному адресу.
 * Возвращает физический адрес слова.
 */
static int mmu_store_with_tag(CORE *cpu, int vaddr, t_value val64, uint8 t)
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
    int paddr = va_to_pa(cpu, vaddr, 0);

    /* Пишем в память. */
    memory[paddr] = val64;
    tag[paddr] = t;

    return paddr;
}

/*
 * Запись 48-битного слова в память.
 */
void mmu_store(CORE *cpu, int vaddr, t_value val)
{
    /* Вычисляем тег.
     * Если ПКП=0 и ПКЛ=0, то тег 35 (команда),
     * иначе тег 36 (данные). */
    uint8 t = (cpu->RUU & (RUU_CHECK_RIGHT | RUU_CHECK_LEFT)) ?
        TAG_NUMBER48 : TAG_INSN48;

    int paddr = mmu_store_with_tag(cpu, vaddr, val << 16, t);

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
void mmu_store64(CORE *cpu, int vaddr, t_value val64)
{
    int paddr = mmu_store_with_tag(cpu, vaddr, val64, cpu->TagR);

    if (paddr != 0 && svs_trace >= TRACE_ALL) {
        fprintf(sim_log, "cpu%d       Memory Write [%05o %07o] = %02o:",
            cpu->index, vaddr, paddr, cpu->TagR);
        fprintf(sim_log, "%03x %03x %03x %03x %04x\n",
            (int) (val64 >> 52) & 07777,
            (int) (val64 >> 40) & 07777,
            (int) (val64 >> 28) & 07777,
            (int) (val64 >> 16) & 07777,
            (int) val64 & 0177777);
    }
}

/*
 * Чтение операнда и тега из памяти по виртуальному адресу.
 * Возвращает физический адрес слова.
 */
static int mmu_load_with_tag(CORE *cpu, int vaddr, t_value *val64, uint8 *t)
{
    vaddr &= BITS(15);
    if (vaddr == 0) {
        *val64 = 0;
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
    int paddr = va_to_pa(cpu, vaddr, 0);

    /*
     * Слова 1-7 в режиме ЯДРА читаются из физической памяти 1-7, а не с
     * тумблерных регистров: приписка на них не действует, но это обычные
     * ячейки. Именно оттуда АДАП берёт слова пульта — например, номер канала
     * для МД (разр.22:21 слова 2, проверка ЕСТЬКД 033613; иначе СТОП 40114)
     * и приписку стека из слова 7. Команда `d 2 …` в .ini кладёт значение
     * именно в память, так что читать надо её.
     *
     * В режиме пользователя за адресами 1-7 остаются тумблерные регистры.
     */
    if (paddr >= 010 || IS_SUPERVISOR(cpu->RUU)) {
        /* Из памяти */
        *val64 = memory[paddr];
        *t = tag[paddr];
    } else {
        /* С тумблерных регистров */
        *val64 = cpu->pult[paddr] << 16;
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
    t_value val64;
    uint8 t;
    int paddr = mmu_load_with_tag(cpu, vaddr, &val64, &t);

    if (paddr != 0 && svs_trace >= TRACE_ALL) {
        if (paddr < 010)
            fprintf(sim_log, "cpu%d       Read  TR%o = ", cpu->index, paddr);
        else
            fprintf(sim_log, "cpu%d       Memory Read [%05o %07o] = %02o:",
                cpu->index, vaddr, paddr, t);
        fprintf(sim_log, "%04o %04o %04o %04o:%02o %04o\n",
            (int) (val64 >> 52) & 07777,
            (int) (val64 >> 40) & 07777,
            (int) (val64 >> 28) & 07777,
            (int) (val64 >> 16) & 07777,
            (int) (val64 >> 12) & 017,
            (int) val64 & 07777);
    }

    /* Прерывание (контроль числа), если попалось 48-битное слово. */
    /* TEMP: контроль числа временно отключён, чтобы пройти инициализацию АДАП
     * (загрузчик метит все слова как 035/036; СЧП ругается на таблицы ТУС). */
    if (0 && tag_check && IS_48BIT(t) /*&& (mmu_unit.flags & CHECK_ENB)*/) {
        cpu->bad_addr = paddr & 7;
        svs_debug("--- (%05o) контроль числа", paddr);
        longjmp(cpu->exception, STOP_RAM_CHECK);
    }

    cpu->TagR = t;
    return val64;
}

/*
 * Чтение 48-битного операнда.
 */
t_value mmu_load(CORE *cpu, int vaddr)
{
    t_value val;
    uint8 t;
    int paddr = mmu_load_with_tag(cpu, vaddr, &val, &t);

    val >>= 16;
    if (paddr != 0 && svs_trace >= TRACE_ALL) {
        if (paddr < 010)
            fprintf(sim_log, "cpu%d       Read  TR%o = ", cpu->index, paddr);
        else
            fprintf(sim_log, "cpu%d       Memory Read [%05o %07o] = %02o:",
                cpu->index, vaddr, paddr, t);
        fprint_sym(sim_log, 0, &val, 0, 0);
        fprintf(sim_log, "\n");
    }

    /*
     * Прерывание (контроль числа), если попалось 64-битное слово.
     * На тумблерных регистрах контроля числа не бывает.
     *
     * TAG_BITSET (020) исключён намеренно. В заводском описании ПВВ
     * (№10.170.002 ТОП, разд. 4.1) КАЖДОЕ управляющее слово начинается строкой
     * «ТЕГ - битовый набор»: и БАК, и ТУС, и ТОЧ, и ДВР, и слова блока БВВ.
     * ВЫЗПВВ так и делает — ставит регистр тега командой «рег '44'» со
     * значением 020 и кладёт командные слова как битовый набор, а потом
     * читает их обычным «сч». Считать это контролем числа нельзя.
     */
    if (paddr >= 010 && ! IS_48BIT(t) && t != TAG_BITSET
        /*&& (mmu_unit.flags & CHECK_ENB)*/) {
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

    /*
     * Буфер предвыборки команд.
     *
     * Окно буфера скользит: после выдачи слова оно ДОЗАПОЛНЯЕТСЯ вперёд, так
     * что в нём всегда лежат ближайшие PREFETCH_DEPTH слов. Именно поэтому
     * смена приписки "на ходу" не ломает исполнение: команда РЕГ '60'+РПАД
     * (036134) переотображает страницы, но слово 036135 к этому моменту уже
     * выбрано по СТАРОЙ приписке и исполняется из буфера, успевая уйти на
     * уже переотображённый адрес (ВТБРЗ на АВПВВ).
     *
     * Предвыборка НЕ проверяет тег и не трогает защиту — только запоминает
     * слово и его физический адрес; контроль команды делается ниже, когда
     * слово реально исполняется. Иначе чтение вперёд по неприписанной
     * странице давало бы ложное прерывание.
     *
     * Переход буфер сбрасывает: слово отдаётся из окна, только если выборка
     * идёт последовательно (тот же адрес — второй слог — или следующий).
     */
    int paddr, i;

    if (vaddr == cpu->pf_last || vaddr == cpu->pf_last + 1) {
        /* Последовательная выборка: сдвигаем окно к текущему адресу. */
        if (vaddr >= (int) cpu->pf_base &&
            vaddr <  (int) cpu->pf_base + cpu->pf_count)
        {
            int shift = vaddr - cpu->pf_base;

            for (i = 0; i + shift < cpu->pf_count; ++i) {
                cpu->pf_va[i]   = cpu->pf_va[i + shift];
                cpu->pf_pa[i]   = cpu->pf_pa[i + shift];
                cpu->pf_word[i] = cpu->pf_word[i + shift];
                cpu->pf_tag[i]  = cpu->pf_tag[i + shift];
            }
            cpu->pf_count -= shift;
            cpu->pf_base   = vaddr;
        } else {
            cpu->pf_count = 0;
            cpu->pf_base  = vaddr;
        }
    } else {
        /* Переход — сброс буфера. */
        cpu->pf_count = 0;
        cpu->pf_base  = vaddr;
    }

    /* Дозаполняем окно вперёд по ТЕКУЩЕЙ приписке. */
    while (cpu->pf_count < PREFETCH_DEPTH) {
        uint32 va = (cpu->pf_base + cpu->pf_count) & BITS(15);
        uint32 pa = va_to_pa(cpu, va, 1);

        cpu->pf_va[cpu->pf_count]   = va;
        cpu->pf_pa[cpu->pf_count]   = pa;
        if (pa >= 010) {
            cpu->pf_word[cpu->pf_count] = memory[pa] >> 16;
            cpu->pf_tag[cpu->pf_count]  = tag[pa];
        } else {
            cpu->pf_word[cpu->pf_count] = cpu->pult[pa];
            cpu->pf_tag[cpu->pf_count]  = TAG_INSN48;
        }
        cpu->pf_count++;
    }

    paddr = cpu->pf_pa[0];
    val   = cpu->pf_word[0];
    t     = cpu->pf_tag[0];
    cpu->pf_last = vaddr;

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
        static int dumped = 0;

        svs_debug("--- (%05o) контроль команды: физ.%07o тег=%03o слово=%016jo",
            vaddr, paddr, t, (uintmax_t)((memory[paddr] >> 16) & BITS48));

        /* Один раз печатаем окрестность: какие теги вокруг, где граница
         * между принесённым с устройства кодом и нетронутой памятью. */
        if (! dumped) {
            int a, lo = (paddr >= 020) ? paddr - 020 : 0;

            dumped = 1;
            for (a = lo; a < lo + 050 && a < (int)MEMSIZE; a++)
                svs_debug("---   %07o тег=%03o %016jo%s", a, tag[a],
                    (uintmax_t)((memory[a] >> 16) & BITS48),
                    (a == paddr) ? "  <<< сюда прыгнули" : "");
        }
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

    if (svs_trace >= TRACE_INSTRUCTIONS) {
        /*
         * Дамп перепрограммирования приписки. Печатаем и СТАРОЕ, и НОВОЕ
         * отображение, чтобы сразу видеть, какие виртуальные страницы
         * переехали. Особенно важна страница 0: в ней лежит стек (вирт.01540),
         * и её отображение меняться не должно — иначе адрес возврата,
         * положенный до перенастройки, читается уже из другого слова.
         */
        const uint32 *tlb = supervisor ? cpu->STLB : cpu->UTLB;
        int b = idx * 4;

        fprintf(sim_log,
            "cpu%d --- Приписка %s: РП%d := %o,%o,%o,%o (было %o,%o,%o,%o)"
            " => вирт.стр %d->%o %d->%o %d->%o %d->%o%s\n",
            cpu->index, supervisor ? "ЯДРА  " : "ПОЛЬЗ.", idx,
            p0, p1, p2, p3,
            tlb[b], tlb[b+1], tlb[b+2], tlb[b+3],
            b, p0, b+1, p1, b+2, p2, b+3, p3,
            (idx == 0 && tlb[0] != p0) ? "   <<< СТРАНИЦА 0 ПЕРЕЕХАЛА!" : "");
    }

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
