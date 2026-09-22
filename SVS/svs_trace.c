/*
 * SVS instruction and register tracing.
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

static CORE cpu_state[NUM_CORES];           /* previous state for comparison */

/*
 * Сброс снимка регистров: первая же выдача svs_trace_registers() после
 * сброса процессора покажет всё ненулевое начальное состояние.
 */
void svs_trace_reset(CORE *cpu)
{
    memset(&cpu_state[cpu->index], 0, sizeof(cpu_state[0]));
}

/*
 * Исполнительный адрес короткого экстракода (э50...э77).
 * Вызывается до сложения с М[МОД], поэтому считаем его здесь сами.
 */
static void trace_executive_address(CORE *cpu)
{
    int reg = (cpu->RK >> 20) & 017;
    int addr = cpu->RK & 07777;

    if (cpu->RK & BBIT(19))
        addr |= 070000;
    addr = ADDR(addr + cpu->M[reg]);
    if (cpu->RUU & RUU_MOD_RK)
        addr = ADDR(addr + cpu->M[MOD]);
    fprintf(sim_deb, " = %o", addr);
}

void svs_trace_opcode(CORE *cpu, int paddr)
{
    // Print instruction.
    fprintf(sim_deb, "cpu%d %05o %07o %c: ",
        cpu->index, cpu->PC, paddr,
        (cpu->RUU & RUU_RIGHT_INSTR) ? 'R' : 'L');
    svs_fprint_insn(sim_deb, cpu->RK);
    fprintf(sim_deb, " ");
    svs_fprint_cmd(sim_deb, cpu->RK);

    /* Короткие экстракоды 050...077: показываем исполнительный адрес. */
    if (! (cpu->RK & BBIT(20))) {
        int opcode = (cpu->RK >> 12) & 077;

        if (opcode >= 050 && opcode <= 077)
            trace_executive_address(cpu);
    }
    fprintf(sim_deb, "\n");
}

/*
 * Print 32-bit value as octal.
 */
static void fprint_32bits(FILE *of, t_value value)
{
    fprintf(of, "%03o %04o %04o",
        (int) (value >> 24) & 0377,
        (int) (value >> 12) & 07777,
        (int) value & 07777);
}

/*
 * Заголовок строки обращения к памяти. Физические адреса 1-7 в режиме
 * пользователя — тумблерные регистры, их печатаем отдельно.
 */
static void trace_memory_header(CORE *cpu, const char *opname,
    int vaddr, int paddr, uint8 t)
{
    if (paddr < 010)
        fprintf(sim_deb, "cpu%d       %s  TR%o = ",
            cpu->index, opname, paddr);
    else
        fprintf(sim_deb, "cpu%d       Memory %s [%05o %07o] = %02o:",
            cpu->index, opname, vaddr, paddr, t);
}

/*
 * Обращение к памяти за 48-битным словом.
 */
void svs_trace_memory(CORE *cpu, const char *opname,
    int vaddr, int paddr, uint8 t, t_value val)
{
    trace_memory_header(cpu, opname, vaddr, paddr, t);
    fprint_sym(sim_deb, 0, &val, 0, 0);
    fprintf(sim_deb, "\n");
}

/*
 * Обращение к памяти за 64-битным словом.
 */
void svs_trace_memory64(CORE *cpu, const char *opname,
    int vaddr, int paddr, uint8 t, t_value val64)
{
    trace_memory_header(cpu, opname, vaddr, paddr, t);
    fprintf(sim_deb, "%04o %04o %04o %04o:%02o %04o\n",
        (int) (val64 >> 52) & 07777,
        (int) (val64 >> 40) & 07777,
        (int) (val64 >> 28) & 07777,
        (int) (val64 >> 16) & 07777,
        (int) (val64 >> 12) & 017,
        (int) val64 & 07777);
}

/*
 * Выборка команды.
 */
void svs_trace_fetch(CORE *cpu, int vaddr, int paddr, uint8 t, t_value val)
{
    fprintf(sim_deb, "cpu%d       Fetch [%05o %07o] = %o:",
        cpu->index, vaddr, paddr, t);
    fprint_sym(sim_deb, 0, &val, 0, SWMASK('I'));
    fprintf(sim_deb, "\n");
}

/*
 * Прерывание или исключительная ситуация.
 *
 * Сообщение собирается в буфер и выдаётся одним fprintf(): в sim_deb пишет
 * только Fprintf() (scp.h подменяет им fprintf), а голый vfprintf() шёл бы
 * мимо буфера отладки и вылезал в файл раньше остальной строки.
 */
void svs_trace_exception(CORE *cpu, const char *fmt, ...)
{
    va_list args;
    char msg[256];

    va_start(args, fmt);
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);

    fprintf(sim_deb, "cpu%d ----- %05o%c: %s -----\n", cpu->index, cpu->PC,
        (cpu->RUU & RUU_RIGHT_INSTR) ? 'R' : 'L', msg);
}

/*
 * Печать регистров процессора, изменившихся с прошлого вызова.
 */
void svs_trace_registers(CORE *cpu)
{
    CORE *prev = &cpu_state[cpu->index];
    int i;

    if (cpu->ACC != prev->ACC) {
        fprintf(sim_deb, "cpu%d       Write ACC = ", cpu->index);
        fprint_sym(sim_deb, 0, &cpu->ACC, 0, 0);
        fprintf(sim_deb, "\n");
    }
    if (cpu->RMR != prev->RMR) {
        fprintf(sim_deb, "cpu%d       Write RMR = ", cpu->index);
        fprint_sym(sim_deb, 0, &cpu->RMR, 0, 0);
        fprintf(sim_deb, "\n");
    }
    for (i = 0; i < NREGS; i++) {
        if (cpu->M[i] != prev->M[i])
            fprintf(sim_deb, "cpu%d       Write M%o = %05o\n",
                cpu->index, i, cpu->M[i]);
    }
    if (cpu->RAU != prev->RAU)
        fprintf(sim_deb, "cpu%d       Write RAU = %02o\n",
            cpu->index, cpu->RAU);
    if ((cpu->RUU & ~RUU_RIGHT_INSTR) != (prev->RUU & ~RUU_RIGHT_INSTR))
        fprintf(sim_deb, "cpu%d       Write RUU = %03o\n",
            cpu->index, cpu->RUU);
    for (i = 0; i < 8; i++) {
        if (cpu->RP[i] != prev->RP[i]) {
            fprintf(sim_deb, "cpu%d       Write RP%o = ",
                cpu->index, i);
            fprint_sym(sim_deb, 0, &cpu->RP[i], 0, 0);
            fprintf(sim_deb, "\n");
        }
        if (cpu->RPS[i] != prev->RPS[i]) {
            fprintf(sim_deb, "cpu%d       Write RPS%o = ",
                cpu->index, i);
            fprint_sym(sim_deb, 0, &cpu->RPS[i], 0, 0);
            fprintf(sim_deb, "\n");
        }
    }
    if (cpu->RZ != prev->RZ) {
        fprintf(sim_deb, "cpu%d       Write RZ = ", cpu->index);
        fprint_32bits(sim_deb, cpu->RZ);
        fprintf(sim_deb, "\n");
    }
    if (cpu->bad_addr != prev->bad_addr) {
        fprintf(sim_deb, "cpu%d       Write EADDR = %03o\n",
            cpu->index, cpu->bad_addr);
    }
    if (cpu->TagR != prev->TagR) {
        fprintf(sim_deb, "cpu%d       Write TAG = %03o\n",
            cpu->index, cpu->TagR);
    }
    if (cpu->PP != prev->PP) {
        fprintf(sim_deb, "cpu%d       Write PP = ", cpu->index);
        fprint_sym(sim_deb, 0, &cpu->PP, 0, 0);
        fprintf(sim_deb, "\n");
    }
    if (cpu->OPP != prev->OPP) {
        fprintf(sim_deb, "cpu%d       Write OPP = ", cpu->index);
        fprint_sym(sim_deb, 0, &cpu->OPP, 0, 0);
        fprintf(sim_deb, "\n");
    }
    if (cpu->POP != prev->POP) {
        fprintf(sim_deb, "cpu%d       Write POP = ", cpu->index);
        fprint_sym(sim_deb, 0, &cpu->POP, 0, 0);
        fprintf(sim_deb, "\n");
    }
    if (cpu->OPOP != prev->OPOP) {
        fprintf(sim_deb, "cpu%d       Write OPOP = ", cpu->index);
        fprint_sym(sim_deb, 0, &cpu->OPOP, 0, 0);
        fprintf(sim_deb, "\n");
    }
    if (cpu->RKP != prev->RKP) {
        fprintf(sim_deb, "cpu%d       Write RKP = ", cpu->index);
        fprint_sym(sim_deb, 0, &cpu->RKP, 0, 0);
        fprintf(sim_deb, "\n");
    }
    if (cpu->RPR != prev->RPR) {
        fprintf(sim_deb, "cpu%d       Write RPR = ", cpu->index);
        fprint_sym(sim_deb, 0, &cpu->RPR, 0, 0);
        fprintf(sim_deb, "\n");
    }
    if (cpu->GRVP != prev->GRVP) {
        fprintf(sim_deb, "cpu%d       Write GRVP = ", cpu->index);
        fprint_32bits(sim_deb, cpu->GRVP);
        fprintf(sim_deb, "\n");
    }
    if (cpu->GRM != prev->GRM) {
        fprintf(sim_deb, "cpu%d       Write GRM = ", cpu->index);
        fprint_32bits(sim_deb, cpu->GRM);
        fprintf(sim_deb, "\n");
    }

    *prev = *cpu;
}
