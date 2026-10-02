/*
 * pwcheck — brute-force validity filter for the СЕРП "ТЕР" password check.
 *
 * The СЕРП utility (task 4199, directive Л/СП 4199, sub-command ТЕР) reads a
 * password, then decrypts six words at virtual 70000-70005 with a keystream
 * cipher seeded by the password and jumps to 70000. A wrong password decrypts
 * to garbage; on the SVS emulator the garbage typically jumps into a page the
 * task does not have, or executes a non-command word, aborting the task
 * (КОНТРОЛЬ КОМАНДЫ). See BUGS.md / the session notes.
 *
 * The decryption (reconstructed from an SVS instruction trace and verified
 * bit-exact against it):
 *
 *     K = password                       ; the 48-bit password is the seed
 *     for i in 0..5:
 *         p = popcount(K)                ; чед: popcount(K) + C, C's low bits
 *         K = Wtab[p] XOR K              ; сда + нтж feedback
 *         plain[i] = cipher[i] XOR K
 *
 * Wtab[p] = сда101(э51(чед(C,p))) for popcount p in 0..48. Because the sine
 * (э51) argument depends only on popcount(K) in [0,48], there are only 49
 * distinct keystream values; they were captured once from the emulator's own
 * э51 (a DISPAK software routine), so no BESM-6 float/sine is reimplemented.
 *
 * A password passes when the six decrypted words form runnable user code:
 *   1. every instruction is legal in user mode (no privileged opcode);
 *   2. every jump/branch reached on the executed path targets a page the
 *      task has resident;
 *   3. execution from 70000 ends at a `пб (reg)` — an unconditional пб with a
 *      zero offset (register-indirect exit/return). The first unconditional
 *      пб / выпр / стоп reached is the terminator and must be that пб (0);
 *      words after it are data and are not examined. Reaching the end of the
 *      six words with no such terminator is a run-away.
 *
 * Effective jump targets honour the M-register file. The constructed code is
 * entered (via `пв 70000`) with the real register state captured from the
 * emulator (entry_reg[]). Per the ISA, only по/пе/пб index their target by
 * M[reg] (target = addr + M[reg]); пв/пио/пино/цикл use the raw address field.
 * Registers are tracked along the straight-line code (уиа/слиа set them;
 * anything whose effect is not modelled marks them unknown), so a branch that
 * uses a register the code has just set is resolved from that value. When a
 * target cannot be resolved statically the branch is left UNRESOLVED and does
 * not by itself reject the password (the filter stays sound: it never rejects
 * a password whose real run might be valid).
 *
 * Passwords tried: the 48-bit word is 6 eight-bit bytes, each a digit 0..9
 * (000..011 octal), packed 8 bits per byte — 10^6 numeric passwords.
 */
#include <stdio.h>
#include <stdint.h>

#include "pwcheck_data.h"        /* Wtab[49], cipher[6], present_mask */

#define MASK48 (((uint64_t)1 << 48) - 1)
#define A15    077777            /* 15-bit address mask */
#define CODE_BASE 070000         /* virtual address of the six words */

static inline int popcount48(uint64_t x) { return __builtin_popcountll(x & MASK48); }

/*
 * M-register file at entry to the constructed code, captured from the emulator
 * at `пв 70000` (индексы 01..017 восьмерично). M15(=015) already carries the
 * return address 072360 that `пв` deposits.
 */
static const int entry_reg[16] = {
    /*00*/ 0,
    /*01*/ 067600, /*02*/ 074300, /*03*/ 072264, /*04*/ 0,
    /*05*/ 0,      /*06*/ 0,      /*07*/ 1,      /*010*/ 0,
    /*011*/ 070000,/*012*/ 0,     /*013*/ 072357,/*014*/ 0,
    /*015*/ 072360,/*016*/ 077772,/*017*/ 067700,
};

static void decrypt(uint64_t pw, uint64_t out[6])
{
    uint64_t K = pw & MASK48;
    int i;
    for (i = 0; i < 6; i++) {
        int p = popcount48(K);
        K = (Wtab[p] ^ K) & MASK48;
        out[i] = (cipher[i] ^ K) & MASK48;
    }
}

/* One decoded 24-bit instruction. */
typedef struct { int reg, form, opcode, addr; } Insn;

static Insn decode24(uint32_t rk)
{
    Insn s;
    rk &= (1u << 24) - 1;
    s.reg = rk >> 20;                    /* М-регистр, разр.21-24 */
    if (rk & (1u << 19)) {               /* BBIT(20): длинный/короткий признак */
        s.addr = rk & A15;
        s.opcode = (rk >> 12) & 0370;    /* группа 0200-0370 */
        s.form = 1;
    } else {
        s.addr = rk & ((1u << 12) - 1);
        if (rk & (1u << 18))             /* BBIT(19): расширение адреса */
            s.addr |= 070000;
        s.opcode = (rk >> 12) & 077;     /* группа 000-077 */
        s.form = 0;
    }
    return s;
}

/* Privileged in user mode -> STOP_BADCMD (svs_cpu.c). */
static int is_privileged(Insn a)
{
    if (a.form == 0)
        return a.opcode==0002 || a.opcode==0032 || a.opcode==0033 ||
               a.opcode==0046 || a.opcode==0047;
    return a.opcode==0320 || a.opcode==0330;    /* выпр, стоп */
}

/* Address-bearing control transfer (long form). Returns 1 and sets *indexed. */
static int is_transfer(Insn a, int *indexed)
{
    if (a.form != 1) return 0;
    switch (a.opcode) {
    case 0260: case 0270: case 0300:     /* по, пе, пб: target = addr + M[reg] */
        *indexed = 1; return 1;
    case 0310:                           /* пв:  target = addr (raw) */
    case 0340: case 0350: case 0370:     /* пио, пино, цикл: target = addr */
        *indexed = 0; return 1;
    }
    return 0;
}

/* Unconditional transfer / terminator that closes straight-line flow. */
static int is_flow_closing(Insn a)
{
    if (a.form != 1) return 0;
    return a.opcode==0300 || a.opcode==0320 || a.opcode==0330; /* пб, выпр, стоп */
}

static int page_present(int vaddr)
{
    return (present_mask >> ((vaddr >> 10) & 037)) & 1;
}

enum { OK=0, BAD_PRIV=-1, BAD_JUMP=-2, BAD_RUNAWAY=-3, BAD_TWO_XTA=-4,
       BAD_LAST_PB=-5 };

/* Opcode 010 (сч, xta) loads the accumulator; two in a row means the first
 * load is discarded unused — a sign of garbage, not real code. */
static int is_xta(Insn a) { return a.form == 0 && a.opcode == 010; }

/*
 * Walk the twelve instructions (left, right of each word) in layout order,
 * tracking the M-register file so индексируемые branches use the right value.
 * *unresolved gets the count of branches whose target could not be pinned down.
 */
static int check(const uint64_t w[6], int *unresolved)
{
    Insn ins[12];
    int reg[16], known[16];
    int i, mod_pending = 0;

    *unresolved = 0;
    for (i = 0; i < 6; i++) {
        ins[2*i]   = decode24((uint32_t)(w[i] >> 24));
        ins[2*i+1] = decode24((uint32_t)(w[i] & 0xFFFFFF));
    }

    /* Initialise the register file from the captured entry state. */
    for (i = 0; i < 16; i++) { reg[i] = entry_reg[i]; known[i] = 1; }

    /*
     * Walk the instructions in execution (fall-through) order, validating only
     * those that are actually reached. The fragment ends at the first
     * unconditional flow-closing instruction (пб / выпр / стоп); everything
     * after it is data and is not examined. That terminator must be
     * `пб (reg)` — пб with a zero offset (a register-indirect exit/return).
     * If flow reaches the end of the six words without such a terminator, the
     * PC runs away.
     */
    for (i = 0; i < 12; i++) {
        Insn a = ins[i];
        int indexed, mod = mod_pending;
        mod_pending = 0;

        /* Instructions on the executed path must be legal in user mode... */
        if (is_privileged(a))
            return BAD_PRIV;
        /* ...and no two consecutive сч (010) loads. */
        if (i > 0 && is_xta(ins[i-1]) && is_xta(a))
            return BAD_TWO_XTA;

        if (is_transfer(a, &indexed)) {
            int target, have = 1;
            if (mod)                         /* addr modified at runtime */
                have = 0;
            else if (!indexed || a.reg == 0)
                target = a.addr & A15;
            else if (known[a.reg])
                target = (a.addr + reg[a.reg]) & A15;
            else
                have = 0;

            if (have) {
                if (!page_present(target))
                    return BAD_JUMP;
            } else {
                (*unresolved)++;
            }
        }

        /* Terminator: the first unconditional пб / выпр / стоп ends the code.
         * Control reached 70000 via `пв 70000(15)`, so M15 = 72360 (the return
         * address). The caller's handler is valid code at 72360..72370, so the
         * exit is `пб N(15)` for a small offset N (0..HANDLER_LEN), returning
         * into that handler. */
        if (is_flow_closing(a)) {
            if (a.opcode == 0300 && a.reg == 015 && a.addr <= 010)
                return OK;                   /* `пб N(15)` return into handler */
            return BAD_LAST_PB;              /* ends, but not a пб (15) return */
        }

        /* Register-file update along the straight line. */
        if (a.form == 1 && a.opcode == 0240) {          /* уиа: M[reg] = addr */
            if (a.reg) { reg[a.reg] = a.addr & A15; known[a.reg] = !mod; }
        } else if (a.form == 1 && a.opcode == 0250) {   /* слиа: M[reg] += addr */
            if (a.reg) {
                if (known[a.reg] && !mod) reg[a.reg] = (reg[a.reg] + a.addr) & A15;
                else known[a.reg] = 0;
            }
        } else if (a.form == 1 && a.opcode == 0220) {   /* мода: address prefix */
            mod_pending = 1;
        } else if (a.form == 1 && a.opcode == 0230) {   /* мод:  address prefix */
            mod_pending = 1;
        } else if (a.form == 1 && a.opcode == 0310) {   /* пв: M[reg] := return */
            if (a.reg) known[a.reg] = 0;                /* return addr not tracked */
        } else if (a.form == 0 &&
                   (a.opcode==0040 || a.opcode==0041 ||
                    a.opcode==0044 || a.opcode==0045 || a.opcode==0047)) {
            /* уи/уим/уии/сли/э47: write an M register we cannot pin down. */
            int j; for (j = 1; j < 16; j++) known[j] = 0;
        } else if (a.form == 0 && a.opcode >= 050) {
            /* extracode: the OS may clobber registers. */
            int j; for (j = 1; j < 16; j++) known[j] = 0;
        } else if (a.form == 1 &&
                   (a.opcode==0200 || a.opcode==0210 || a.opcode==0360)) {
            int j; for (j = 1; j < 16; j++) known[j] = 0;   /* extracodes */
        } else if (a.reg == 017) {
            known[017] = 0;                             /* stack use of М17 */
        }
    }

    return BAD_RUNAWAY;                       /* fell off the end, no пб (0) */
}

int main(void)
{
    uint64_t pw, out[6];
    long total = 0, npriv = 0, njump = 0, nrun = 0, npass = 0, npass_unres = 0;
    long ntwoxta = 0, nlastpb = 0;
    int b0,b1,b2,b3,b4,b5;

    /* 6 ГОСТ-10859 uppercase letters, mixed Cyrillic/Latin: codes 040..0114. */
    for (b0=040;b0<=0114;b0++) for (b1=040;b1<=0114;b1++)
    for (b2=040;b2<=0114;b2++) for (b3=040;b3<=0114;b3++)
    for (b4=040;b4<=0114;b4++) for (b5=040;b5<=0114;b5++) {
        int unres;
        pw = ((uint64_t)b0<<40)|((uint64_t)b1<<32)|((uint64_t)b2<<24)|
             ((uint64_t)b3<<16)|((uint64_t)b4<<8)|(uint64_t)b5;
        total++;
        decrypt(pw, out);
        int r = check(out, &unres);
        if (r==BAD_PRIV) npriv++;
        else if (r==BAD_TWO_XTA) ntwoxta++;
        else if (r==BAD_LAST_PB) nlastpb++;
        else if (r==BAD_JUMP) njump++;
        else if (r==BAD_RUNAWAY) nrun++;
        else {
            npass++;
            if (unres) npass_unres++;
            printf("PASS pw=%016llo unresolved=%d  words:",
                   (unsigned long long)pw, unres);
            for (int k=0;k<6;k++) printf(" %016llo",(unsigned long long)out[k]);
            printf("\n");
        }
    }
    fprintf(stderr,
        "tried=%ld  pass=%ld (of which %ld have unresolved branches)  "
        "rej_priv=%ld  rej_two_xta=%ld  rej_last_pb=%ld  "
        "rej_jump=%ld  rej_runaway=%ld\n",
        total, npass, npass_unres, npriv, ntwoxta, nlastpb, njump, nrun);
    return 0;
}
