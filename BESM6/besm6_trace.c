/*
 * besm6_trace.c: BESM-6 instruction and register tracing
 *
 * Copyright (c) 2026, Serge Vakulenko
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
 *
 * Except as contained in this notice, the name of Leonid Broukhis or
 * Serge Vakulenko shall not be used in advertising or otherwise to promote
 * the sale, use or other dealings in this Software without prior written
 * authorization from Leonid Broukhis and Serge Vakulenko.
 *
 * Ported from the b6sim tracer (v7besm/cmd/sim/trace.cpp).  All tracing is
 * gated by the caller on `sim_deb && cpu_dev.dctrl' (SIMH `set cpu debug'),
 * which enables the whole trace at once: instructions, register changes,
 * memory read/write, and exceptions/interrupts.  Output goes to sim_deb.
 */
#include "besm6_defs.h"

/*
 * Previous register state, for printing only what has changed.
 */
static t_value prev_ACC, prev_RMR, prev_GRP, prev_MGRP;
static t_value prev_RP[8];
static uint32  prev_M[NREGS], prev_RAU, prev_RUU, prev_PRP, prev_MPRP, prev_RZ;

/*
 * Symbol table extracted from an a.out image: function names and their
 * addresses, kept sorted by address so besm6_sym_find() can locate the
 * function that contains a given PC (nearest preceding symbol).
 */
typedef struct {
    uint32 addr;
    char  *name;
} besm6_symbol_t;

static besm6_symbol_t *sym_table;
static int sym_count, sym_alloc;

/*
 * Drop the current symbol table.  Called by the loader before parsing a
 * new image; the table survives CPU reset so names persist across `run'.
 */
void besm6_sym_clear ()
{
    int i;

    for (i = 0; i < sym_count; i++)
        free (sym_table[i].name);
    sym_count = 0;
}

/*
 * Append one function symbol.  Names are copied; the array grows as needed.
 */
void besm6_sym_add (uint32 addr, const char *name)
{
    if (sym_count >= sym_alloc) {
        sym_alloc = sym_alloc ? sym_alloc * 2 : 64;
        sym_table = realloc (sym_table, sym_alloc * sizeof (sym_table[0]));
    }
    sym_table[sym_count].addr = addr;
    sym_table[sym_count].name = strdup (name);
    ++sym_count;
}

/*
 * Order two symbols by address, for qsort().
 */
static int sym_compare (const void *a, const void *b)
{
    uint32 aa = ((const besm6_symbol_t *) a)->addr;
    uint32 ba = ((const besm6_symbol_t *) b)->addr;

    return (aa > ba) - (aa < ba);
}

/*
 * Sort the table by address.  Called once, after the loader has added
 * every symbol, so that besm6_sym_find() can binary-search it.
 */
void besm6_sym_sort ()
{
    qsort (sym_table, sym_count, sizeof (sym_table[0]), sym_compare);
}

/*
 * Find the function containing the given address: the symbol with the
 * largest address <= addr.  Sets *at_start when addr is exactly a function
 * entry.  Returns the name, or NULL if no symbol precedes the address.
 */
const char *besm6_sym_find (uint32 addr, int *at_start)
{
    int lo = 0, hi = sym_count - 1, found = -1;

    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (sym_table[mid].addr <= addr) {
            found = mid;
            lo = mid + 1;
        } else
            hi = mid - 1;
    }
    if (found < 0)
        return 0;
    *at_start = (addr == sym_table[found].addr);
    return sym_table[found].name;
}

/*
 * Print a 48-bit word in octal, as four space-separated groups of four digits.
 */
static void fprint_word_octal (FILE *of, t_value val)
{
    fprintf (of, "%04o %04o %04o %04o",
             (int) (val >> 36) & BITS(12),
             (int) (val >> 24) & BITS(12),
             (int) (val >> 12) & BITS(12),
             (int) val         & BITS(12));
}

/*
 * Reset the previous-register snapshot, so the first besm6_trace_registers()
 * call after a reset dumps the non-zero initial state.
 */
void besm6_trace_reset ()
{
    prev_ACC = 0;
    prev_RMR = 0;
    prev_GRP = 0;
    prev_MGRP = 0;
    prev_RAU = 0;
    prev_RUU = 0;
    prev_PRP = 0;
    prev_MPRP = 0;
    prev_RZ = 0;
    memset (prev_M, 0, sizeof (prev_M));
    memset (prev_RP, 0, sizeof (prev_RP));
}

/*
 * Print the executive (effective) address of an extracode, if any.
 * Called only for short-form extracodes (opcodes 050...077).
 */
static void trace_executive_address (uint32 cmd)
{
    int reg, addr;

    reg = (cmd >> 20) & 017;
    addr = cmd & 07777;
    if (cmd & BBIT(19))
        addr |= 070000;
    addr = ADDR (addr + M[reg]);
    if (RUU & RUU_MOD_RK)
        addr = ADDR (addr + M[MOD]);
    fprintf (sim_deb, " = %o", addr);
}

/*
 * Print the current instruction: address, half-word flag, octal fields and
 * mnemonics.  For extracodes, the executive address is appended.
 */
void besm6_trace_instruction ()
{
    int opcode;

    fprintf (sim_deb, "%05o %c: ", PC,
             (RUU & RUU_RIGHT_INSTR) ? 'R' : 'L');
    besm6_fprint_insn (sim_deb, RK);
    besm6_fprint_cmd (sim_deb, RK);

    /* Short-form extracodes 050...077: show the executive address. */
    if (! (RK & BBIT(20))) {
        opcode = (RK >> 12) & 077;
        if (opcode >= 050 && opcode <= 077)
            trace_executive_address (RK);
    }
    fprintf (sim_deb, "\n");
}

/*
 * Print changes in CPU registers since the previous instruction.
 * Covers all registers, including supervisor-only ones and the page table.
 */
void besm6_trace_registers ()
{
    int i;

    if (ACC != prev_ACC) {
        fprintf (sim_deb, "      ACC = ");
        fprint_word_octal (sim_deb, ACC);
        fprintf (sim_deb, "\n");
    }
    if (RMR != prev_RMR) {
        fprintf (sim_deb, "      RMR = ");
        fprint_word_octal (sim_deb, RMR);
        fprintf (sim_deb, "\n");
    }
    /* Modifier registers M0...M17 and the special registers M20...M35. */
    for (i = 0; i < NREGS; i++) {
        if (M[i] != prev_M[i])
            fprintf (sim_deb, "      M%o = %05o\n", i, M[i]);
    }
    if (RAU != prev_RAU)
        fprintf (sim_deb, "      RAU = %02o\n", RAU);
    /* Ignore the left/right half-word flag: it toggles every instruction
     * and is already shown by the L/R marker on the instruction line. */
    if ((RUU & ~RUU_RIGHT_INSTR) != prev_RUU)
        fprintf (sim_deb, "      RUU = %03o\n", RUU & ~RUU_RIGHT_INSTR);
    if (GRP != prev_GRP) {
        fprintf (sim_deb, "      GRP = ");
        fprint_word_octal (sim_deb, GRP);
        fprintf (sim_deb, "\n");
    }
    if (MGRP != prev_MGRP) {
        fprintf (sim_deb, "      MGRP = ");
        fprint_word_octal (sim_deb, MGRP);
        fprintf (sim_deb, "\n");
    }
    if (PRP != prev_PRP)
        fprintf (sim_deb, "      PRP = %08o\n", PRP);
    if (MPRP != prev_MPRP)
        fprintf (sim_deb, "      MPRP = %08o\n", MPRP);
    /* Page table: memory-mapping registers RP0...RP7 and protection RZ. */
    for (i = 0; i < 8; i++) {
        if (RP[i] != prev_RP[i]) {
            fprintf (sim_deb, "      RP%o = ", i);
            fprint_word_octal (sim_deb, RP[i]);
            fprintf (sim_deb, "\n");
        }
    }
    if (RZ != prev_RZ)
        fprintf (sim_deb, "      RZ = %011o\n", RZ);

    /* Update the previous state. */
    prev_ACC = ACC;
    prev_RMR = RMR;
    prev_GRP = GRP;
    prev_MGRP = MGRP;
    prev_RAU = RAU;
    prev_RUU = RUU & ~RUU_RIGHT_INSTR;
    prev_PRP = PRP;
    prev_MPRP = MPRP;
    prev_RZ = RZ;
    for (i = 0; i < NREGS; i++)
        prev_M[i] = M[i];
    for (i = 0; i < 8; i++)
        prev_RP[i] = RP[i];
}

/*
 * Print a memory read or write.
 */
void besm6_trace_memory (int addr, t_value val, const char *opname)
{
    fprintf (sim_deb, "      Memory %s [%05o] = ", opname, addr & BITS(15));
    fprint_word_octal (sim_deb, val);
    fprintf (sim_deb, "\n");
}

/*
 * Print an exception or interrupt.
 */
void besm6_trace_exception (const char *message)
{
    fprintf (sim_deb, "----- %05o%c: %s -----\n", PC,
             (RUU & RUU_RIGHT_INSTR) ? 'R' : 'L', message);
}

/*
 * Print a separator line naming the function at the new PC, after a call
 * (пв/vjm) or a register return (пб/uj through a saved link).  When the PC
 * is not exactly a function entry - i.e. a return landing in the middle of
 * the caller - the name is prefixed with "back to".
 */
void besm6_trace_call_return ()
{
    int at_start = 0;
    const char *name = besm6_sym_find (PC, &at_start);

    fprintf (sim_deb, "--------------------------------------------------");
    if (name)
        fprintf (sim_deb, at_start ? " %s" : " back to %s", name);
    fprintf (sim_deb, "\n");
}
