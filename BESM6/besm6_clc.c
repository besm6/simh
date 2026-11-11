/*
 * besm6_clc.c: BESM-6 CalComp plotter (CLC)
 *
 * Copyright (c) 2026, Leonid Broukhis
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

/*
 * The CalComp plotter as Диспак drives it (doc/ГП.md, "CalComp: Э50 1205 и
 * 1211"): data go out with `рег 035` (РЕГ 29 in КОБГР, РЕГ '35' in ТРУБКА
 * БУДИЛ), readiness is ПРП 8.
 *
 * ПРП 8 is a latch: the OS clears it with увв 030 when it takes the
 * interrupt, and its handlers (ТРУБКА АВХОД) do not close the mask, so a
 * level would interrupt forever.  The device sets the latch when it becomes
 * ready: on attach ("switched on", the answer to «ВК.Г»), CLC_RATE after
 * every output, and when the OS opens ПРП 8 in МПРП while the device is idle
 * (a new Э50 1211 after the previous array ended on an interrupt without
 * output).
 *
 * With `set tty dks` and no ДКС device, besm6_tty.c fakes ПРП 8 for the
 * К-71 ДКС code (КРК channel 0); do not attach the plotter then.
 *
 * Every output is written to the attached file as one line of 16 octal
 * digits: the whole accumulator, since КОБГР sends a 6- or 8-bit syllable
 * (ГРА НЕТ / ГРА ДА) and Э50 1205 sends the user's word as is.
 */
#include "besm6_defs.h"

#define CLC_RATE        (1*MSEC)

t_stat clc_event (UNIT *u);
t_stat clc_reset (DEVICE *dptr);
t_stat clc_attach (UNIT *uptr, CONST char *cptr);
t_stat clc_detach (UNIT *uptr);

UNIT clc_unit = { UDATA (clc_event, UNIT_SEQ+UNIT_ATTABLE, 0) };

DEVICE clc_dev = {
    "CLC", &clc_unit, NULL, NULL,
    1, 8, 19, 1, 8, 48,
    NULL, NULL, &clc_reset, NULL, &clc_attach, &clc_detach,
    NULL, DEV_DISABLE | DEV_DEBUG
};

t_stat clc_reset (DEVICE *dptr)
{
    sim_cancel (&clc_unit);
    PRP &= ~PRP_PLOTTER;
    if (clc_unit.flags & UNIT_ATT)
        PRP |= PRP_PLOTTER;
    return SCPE_OK;
}

t_stat clc_attach (UNIT *u, CONST char *cptr)
{
    t_stat s = attach_unit (u, cptr);
    if (s != SCPE_OK)
        return s;
    sim_cancel (u);
    PRP |= PRP_PLOTTER;
    if (clc_dev.dctrl)
        besm6_debug ("CLC: attached, ready");
    return SCPE_OK;
}

t_stat clc_detach (UNIT *u)
{
    sim_cancel (u);
    PRP &= ~PRP_PLOTTER;
    if (clc_dev.dctrl)
        besm6_debug ("CLC: detached");
    return detach_unit (u);
}

/*
 * `рег 035` (special register write).  Returns 0 when no plotter is
 * attached; the caller then treats the address as unused, as before.
 */
int clc_output (t_value val)
{
    if (! (clc_unit.flags & UNIT_ATT))
        return 0;
    fprintf (clc_unit.fileref, "%016llo\n", (unsigned long long) (val & BITS48));
    fflush (clc_unit.fileref);
    PRP &= ~PRP_PLOTTER;
    sim_activate_after (&clc_unit, CLC_RATE);
    if (clc_dev.dctrl)
        besm6_debug ("CLC: %016llo", (unsigned long long) (val & BITS48));
    return 1;
}

/*
 * МПРП is being written with ПРП 8 newly open.
 */
void clc_unmasked (void)
{
    if ((clc_unit.flags & UNIT_ATT) && ! sim_is_active (&clc_unit))
        PRP |= PRP_PLOTTER;
}

t_stat clc_event (UNIT *u)
{
    if (u->flags & UNIT_ATT)
        PRP |= PRP_PLOTTER;
    return SCPE_OK;
}
