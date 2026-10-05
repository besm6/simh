/*
 * BESM-6 АС-6 adapter: the link to an ES EVM driven by the ОСА module
 *
 * Copyright (c) 2026, Leonid Broukhis
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
 * LEONID BROUKHIS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF
 * OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 *
 * Except as contained in this notice, the name of Leonid Broukhis shall not
 * be used in advertising or otherwise to promote the sale, use or other
 * dealings in this Software without prior written authorization from
 * Leonid Broukhis.
 */

/*
 * ОСА (re-dispak osa.be, osaner.be) talks to the АС-6 adapter through
 * instruction 033 with addresses 0200-0237 (write) and 04200-04237 (read).
 * It does not use interrupts: it polls the request register 04230 on timer
 * wakeups of task 24 when ПРЕДЕЛ bit 31 (СОЮЗ ДА) is set.  See doc/ОСА.md.
 *
 * This is the register-level model used to trace the protocol: it keeps
 * the registers ОСА writes and logs every access.  While the device is
 * disabled, reads return 0 (no requests) and writes are ignored.  When it
 * is enabled, an access to a register ОСА never uses stops the simulation.
 */
#include "besm6_defs.h"

#define DBG_REG         001     /* every 0200-0237 / 04200-04237 access */

static uint32 req;              /* 04230: request bits */
static uint32 mask;             /* 0234 */
static t_value desc;            /* 0201: transfer descriptor */
static t_value count;           /* 0221 */
static t_value ctl[2];          /* 0232, 0233 */
static t_value last[040];       /* last value written to each register */

t_stat osa_reset (DEVICE *dptr);

UNIT osa_unit = { UDATA (NULL, 0, 0) };

REG osa_reg[] = {
    { ORDATA (REQ,   req,   24) },
    { ORDATA (MASK,  mask,  24) },
    { ORDATA (DESC,  desc,  48) },
    { ORDATA (COUNT, count, 48) },
    { BRDATA (CTL,   ctl,   8, 48, 2) },
    { BRDATA (LAST,  last,  8, 48, 040) },
    { 0 }
};

DEBTAB osa_deb[] = {
    { "REG", DBG_REG, "every 0200-0237 / 04200-04237 access" },
    { NULL, 0 }
};

DEVICE osa_dev = {
    "OSA", &osa_unit, osa_reg, NULL,
    1, 8, 5, 1, 8, 48,
    NULL, NULL, &osa_reset, NULL, NULL, NULL,
    NULL, DEV_DISABLE | DEV_DIS | DEV_DEBUG, 0, osa_deb
};

t_stat osa_reset (DEVICE *dptr)
{
    req = mask = 0;
    desc = count = 0;
    ctl[0] = ctl[1] = 0;
    memset (last, 0, sizeof (last));
    return SCPE_OK;
}

static int osa_enabled (void)
{
    return ! (osa_dev.flags & DEV_DIS);
}

static const char *side (void)
{
    return (RUU & RUU_RIGHT_INSTR) ? "R" : "L";
}

/*
 * Write: 033 0200-0237, reg = address & 037.
 */
void osa_write (int reg, t_value acc)
{
    sim_debug (DBG_REG, &osa_dev, "%05o%s: 0%03o <- %016llo\n",
               PC, side (), 0200 + reg, (unsigned long long) acc);
    if (! osa_enabled ())
        return;
    last[reg] = acc;
    switch (reg) {
    case 000:                   /* reset of the link (СКИФГШ) */
        req = 0;
        break;
    case 001:                   /* start a transfer with a descriptor */
        desc = acc;
        break;
    case 002:                   /* acknowledge of request bit 7 */
    case 014:                   /* acknowledge of request bit 10 */
    case 016:                   /* transfer start, variant */
    case 031:                   /* from ОСАНЕР */
        break;
    case 021:                   /* byte count - 1, bits 48-41 */
        count = acc;
        break;
    case 030:                   /* clear the request bits written as 0 */
        req &= (uint32) acc;
        break;
    case 032:                   /* control word pair */
    case 033:
        ctl[reg - 032] = acc;
        break;
    case 034:                   /* request mask */
        mask = (uint32) acc & BITS(24);
        break;
    default:
        besm6_debug ("*** %05o%s: ОСА: unknown register 0%03o <- %016llo",
                     PC, side (), 0200 + reg, (unsigned long long) acc);
        longjmp (cpu_halt, STOP_UNIMPLEMENTED);
    }
}

/*
 * Read: 033 04200-04237, reg = address & 037.
 */
t_value osa_read (int reg)
{
    t_value val = 0;

    if (osa_enabled ()) {
        switch (reg) {
        case 030:               /* request bits */
            val = req;
            break;
        default:
            besm6_debug ("*** %05o%s: ОСА: unknown register 0%03o read",
                         PC, side (), 04200 + reg);
            longjmp (cpu_halt, STOP_UNIMPLEMENTED);
        }
    }
    sim_debug (DBG_REG, &osa_dev, "%05o%s: 0%03o -> %016llo\n",
               PC, side (), 04200 + reg, (unsigned long long) val);
    return val;
}
