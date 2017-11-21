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

void svs_trace_opcode(CORE *cpu, int paddr)
{
    // Print instruction.
    fprintf(sim_log, "cpu%d %05o %07o %c: ",
        cpu->index, cpu->PC, paddr,
        (cpu->RUU & RUU_RIGHT_INSTR) ? 'R' : 'L');
    svs_fprint_insn(sim_log, cpu->RK);
    fprintf(sim_log, " ");
    svs_fprint_cmd(sim_log, cpu->RK);
    fprintf(sim_log, "\n");
}

/*
 * Печать регистров процессора, изменившихся с прошлого вызова.
 */
void svs_trace_registers(CORE *cpu)
{
    CORE *prev = &cpu_state[cpu->index];
    int i;

    if (cpu->ACC != prev->ACC) {
        fprintf(sim_log, "cpu%d       Write ACC = ", cpu->index);
        fprint_sym(sim_log, 0, &cpu->ACC, 0, 0);
        fprintf(sim_log, "\n");
    }
    if (cpu->RMR != prev->RMR) {
        fprintf(sim_log, "cpu%d       Write RMR = ", cpu->index);
        fprint_sym(sim_log, 0, &cpu->RMR, 0, 0);
        fprintf(sim_log, "\n");
    }
    for (i = 0; i < NREGS; i++) {
        if (cpu->M[i] != prev->M[i])
            fprintf(sim_log, "cpu%d       Write M%o = %05o\n",
                cpu->index, i, cpu->M[i]);
    }
    if (cpu->RAU != prev->RAU)
        fprintf(sim_log, "cpu%d       Write RAU = %02o\n",
            cpu->index, cpu->RAU);
    if ((cpu->RUU & ~RUU_RIGHT_INSTR) != (prev->RUU & ~RUU_RIGHT_INSTR))
        fprintf(sim_log, "cpu%d       Write RUU = %03o\n",
            cpu->index, cpu->RUU);
    if (cpu->GRP != prev->GRP) {
        fprintf(sim_log, "cpu%d       Write GRP = ", cpu->index);
        fprint_sym(sim_log, 0, &cpu->GRP, 0, 0);
        fprintf(sim_log, "\n");
    }
    if (cpu->MGRP != prev->MGRP) {
        fprintf(sim_log, "cpu%d       Write MGRP = ", cpu->index);
        fprint_sym(sim_log, 0, &cpu->MGRP, 0, 0);
        fprintf(sim_log, "\n");
    }
    if (cpu->RVP != prev->RVP) {
        fprintf(sim_log, "cpu%d       Write RVP = 0x%08x\n",
            cpu->index, cpu->RVP);
        fprintf(sim_log, "\n");
    }
    if (cpu->MRVP != prev->MRVP) {
        fprintf(sim_log, "cpu%d       Write MRVP = 0x%08x\n",
            cpu->index, cpu->MRVP);
        fprintf(sim_log, "\n");
    }
    for (i = 0; i < 8; i++) {
        if (cpu->RP[i] != prev->RP[i]) {
            fprintf(sim_log, "cpu%d       Write RP%o = ",
                cpu->index, i);
            fprint_sym(sim_log, 0, &cpu->RP[i], 0, 0);
            fprintf(sim_log, "\n");
        }
    }
    if (cpu->RZ != prev->RZ)
        fprintf(sim_log, "cpu%d       Write RZ = 0x%08x\n",
            cpu->index, cpu->RZ);
    if (cpu->bad_addr != prev->bad_addr)
        fprintf(sim_log, "cpu%d       Write EADDR = %03o\n",
            cpu->index, cpu->bad_addr);

    *prev = *cpu;
}
