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
 * Сброс ПВВ.
 */
void iom_reset(CORE *cpu)
{
    if (svs_trace >= TRACE_INSTRUCTIONS)
        fprintf(sim_log, "cpu%d --- Сброс ПВВ\n", cpu->index);
}

/*
 * Запрос от процессора через регистр ПП.
 */
void iom_request(CORE *cpu)
{
    t_value request = memory[0100];
    if (request == 0)
        return;

    if (svs_trace >= TRACE_INSTRUCTIONS)
        fprintf(sim_log, "cpu%d --- Запрос к ПВВ: %#jx\n",
            cpu->index, (intmax_t)request);

    /* Запрос выполнен. */
    memory[0100] = 0;
}
