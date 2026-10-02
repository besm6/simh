/*
 * BESM-6 КАДОПАМ and the ДКС terminal concentrator
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
 * LEONID BROUKHIS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF
 * OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.

 * Except as contained in this notice, the name of Leonid Broukhis shall not
 * be used in advertising or otherwise to promote the sale, use or other
 * dealings in this Software without prior written authorization from
 * Leonid Broukhis.
 */

/*
 * КАДОПАМ (канал программного доступа в оперативную память микро-ЭВМ) gives
 * the BESM-6 access to the RAM of the Электроника-60 machines of the ДКС:
 * instruction 032 reads a 16-bit word into bits 25-40 of the accumulator,
 * 0132 writes the low 16 bits.  Writing cell 0 selects the КРК channel;
 * reading it returns the channel status (bit 9+k = channel k present).
 *
 * One Э-60 is emulated: the OS's Э-60 index 2 on КРК channel 2, interrupting
 * on ПРП 6.  Only that index gets past the `ручной' check in СВЯЗЬ7.зпрос1.
 * The memory layout below is the one tabulated in svyaz7.be; see BESM6/ДКС.md
 * for the protocol, with references to the Диспак sources.
 *
 * Each telnet line set to "DKS" mode is an Э-60 line with local echo and line
 * editing.  A finished line is passed to the OS as one input array.
 */
#include "besm6_defs.h"
#include "sim_tmxr.h"

#define DKS_CHAN        2                       /* КРК channel of our Э-60 */
#define DKS_PRP         PRP_DKS_CHAN(DKS_CHAN)  /* its interrupt, ПРП 6 */
#define DKS_STATUS      (0400 << DKS_CHAN)      /* cell 0: channel present */

/* Э-60 index 2 layout (svyaz7.be tables абуфs, абуфh, абфосs, абфосh, адркс). */
#define A_BUFS          0140    /* ring Э-60 -> BESM-6: +0 source, +1 receiver, +2.. slots */
#define A_BUFH          0146    /* ring BESM-6 -> Э-60 */
#define A_FOSS          0154    /* system request: +0 code, +1 line/term, +2 area */
#define A_FOSH          0157    /* system reply: +0 = 2, +1 term no., +2 result */
#define RING_MASK       3       /* масотв for index 2 */
#define DOORBELL        077777  /* адркс */
#define DOORBELL_RING   0400    /* масабн */
#define ABONENT_OS      48      /* абонент 48 = system request/reply */

/* Terminal area, in words (svyaz7.be: sтрзпр, вводсs, прмсвs). */
#define AR_STATUS0      0       /* 020000 = input pending, cleared by the OS */
#define AR_STATUS1      1       /* 020000 = output active, cleared by the OS */
#define AR_CMD_IN       2       /* Э-60 -> OS: 4 input ready, 5 output done */
#define AR_INBUF        3       /* byte address of the input buffer */
#define AR_CMD_OUT      5       /* OS -> Э-60: 4 or 5 = take an array */
#define AR_OUTADDR      6       /* byte address of the output array */
#define AR_OUTLEN       7       /* its length in words */
#define AR_FLAG         020000

#define AREA_BASE       01000
#define AREA_SIZE       01400
#define AREA_INBUF      01210   /* after +8 and an 01200-word output array */
#define INBUF_BYTES     ((AREA_SIZE - AREA_INBUF) * 2 - 1)
#define AREA(n)         (AREA_BASE + (n) * AREA_SIZE)

#define DKS_LINES       24      /* TTY_MAX */

#define DBG_KADOPAM     001
#define DBG_RING        002
#define DBG_IRQ         004

/* Э-60 RAM, one per КРК channel; only DKS_CHAN is backed by an Э-60. */
static uint16 e60_mem[4][32768];
static uint32 cur_chan;
static int os_polled;           /* КОНФУС has read the channel status */

/* Pending system request: 0 none, 1 connect (await reply), 3 disconnect. */
static int sys_busy;
static int sys_line;
static uint32 sys_src;          /* абуфs source count when it was posted */
static uint32 bufh_recv;        /* our receiver count in абуфh */

static struct dks_line {
    int     up;                 /* the Э-60 sees the line as connected */
    int     want_connect;       /* connect request not yet sent */
    int     want_disconnect;
    int     term;               /* OS terminal number k+1, 0 if none */
    int     echo;
    int     len;                /* characters in the line being typed */
    char    buf[INBUF_BYTES];
    int     line_ready;         /* line finished, not yet passed to the OS */
    int     pending;            /* 0, or the +2 command awaiting acknowledgement */
    int     out_done;           /* output printed, completion not yet posted */
} line[DKS_LINES + 1];

t_stat dks_svc (UNIT *u);
t_stat dks_reset (DEVICE *dptr);

UNIT dks_unit = { UDATA (dks_svc, 0, 0) };

REG dks_reg[] = {
    { ORDATA (CHAN, cur_chan, 3) },
    { BRDATA (MEM, e60_mem[DKS_CHAN], 8, 16, 32768) },
    { 0 }
};

DEBTAB dks_deb[] = {
    { "KADOPAM", DBG_KADOPAM, "every 032/0132 access" },
    { "RING",    DBG_RING,    "ring slots, system requests, area writes" },
    { "IRQ",     DBG_IRQ,     "interrupts raised" },
    { NULL, 0 }
};

DEVICE dks_dev = {
    "DKS", &dks_unit, dks_reg, NULL,
    1, 8, 15, 1, 8, 16,
    NULL, NULL, &dks_reset, NULL, NULL, NULL,
    NULL, DEV_DISABLE | DEV_DIS | DEV_DEBUG, 0, dks_deb
};

static int dks_enabled (void)
{
    return ! (dks_dev.flags & DEV_DIS);
}

/*
 * The OS listens only after КОНФУС has loaded СВЯЗЬ7, read the channel
 * status and unmasked our interrupt.
 */
static int os_ready (void)
{
    return os_polled && (MPRP & DKS_PRP);
}

static uint32 mem (int addr)
{
    return e60_mem[DKS_CHAN][addr & 077777];
}

static void set_mem (int addr, uint32 val)
{
    e60_mem[DKS_CHAN][addr & 077777] = val & 0177777;
}

/*
 * Read by instruction 032: the result goes into bits 25-40 of the accumulator.
 */
t_value dks_read (int addr)
{
    t_value val;

    if (addr == 0) {
        val = dks_enabled () ? DKS_STATUS : 0;
        os_polled = 1;
    }
    else if (addr == 2)
        val = 0;        /* номнпр: meaning unknown, 0 keeps рспец out */
    else
        val = e60_mem[cur_chan & 3][addr];
    sim_debug (DBG_KADOPAM, &dks_dev, "032: [%o]%05o -> %06o\n",
               cur_chan, addr, (unsigned) val);
    return val;
}

/*
 * Write by instruction 0132: the low 16 bits of the accumulator.
 */
void dks_write (int addr, t_value acc)
{
    uint32 val = (uint32) acc & 0177777;

    sim_debug (DBG_KADOPAM, &dks_dev, "0132: [%o]%05o <- %06o\n",
               cur_chan, addr, val);
    if (addr == 0) {
        cur_chan = val & 7;
        return;
    }
    e60_mem[cur_chan & 3][addr] = val;
    if (! dks_enabled () || cur_chan != DKS_CHAN)
        return;
    if (addr == DOORBELL) {
        if (val & ~DOORBELL_RING)
            sim_debug (DBG_RING, &dks_dev, "doorbell %06o\n", val);
        if (val & DOORBELL_RING)
            sim_activate (&dks_unit, 20);
    } else if (addr >= AREA_BASE && addr < AREA(DKS_LINES + 1) &&
               (addr - AREA_BASE) % AREA_SIZE < 8) {
        sim_debug (DBG_RING, &dks_dev, "line %d area+%d <- %06o\n",
                   (addr - AREA_BASE) / AREA_SIZE,
                   (addr - AREA_BASE) % AREA_SIZE, val);
    }
}

/*
 * Post an abonent number into the ring towards the BESM-6.
 * Returns 0 when the ring is full: the OS has not caught up yet.
 */
static int e60_post (int abonent)
{
    uint32 src = mem (A_BUFS) & RING_MASK;
    uint32 recv = mem (A_BUFS + 1) & RING_MASK;

    if (((src - recv) & RING_MASK) == RING_MASK)
        return 0;
    set_mem (A_BUFS + 2 + src, abonent);
    set_mem (A_BUFS, (src + 1) & RING_MASK);
    sim_debug (DBG_RING, &dks_dev, "post %d (slot %d)\n", abonent, src);
    if (! (PRP & DKS_PRP))
        sim_debug (DBG_IRQ, &dks_dev, "ПРП 6\n");
    PRP |= DKS_PRP;
    return 1;
}

static int find_line (int e60_line)
{
    return (e60_line >= 1 && e60_line <= DKS_LINES && line[e60_line].up) ?
        e60_line : 0;
}

/*
 * Output array from the OS: print it, then report completion.
 */
static void take_output (int n)
{
    int a = AREA(n);
    uint32 cmd = mem (a + AR_CMD_OUT);
    uint32 addr = mem (a + AR_OUTADDR) >> 1;
    uint32 len = mem (a + AR_OUTLEN);
    uint32 i;

    if (cmd != 4 && cmd != 5) {
        sim_debug (DBG_RING, &dks_dev, "line %d: entry without command %o\n",
                   n, cmd);
        return;
    }
    set_mem (a + AR_CMD_OUT, 0);
    sim_debug (DBG_RING, &dks_dev, "line %d: output cmd %o, %o words at %05o\n",
               n, cmd, len, addr);
    for (i = 0; i < len; ++i) {
        uint32 w = mem (addr + i);
        if ((w & 0377) == 0)
            break;
        vt_send (n, w & 0177);
        if ((w >> 8 & 0377) == 0)
            break;
        vt_send (n, w >> 8 & 0177);
    }
    line[n].out_done = 1;
}

/*
 * Ring from the OS: take every entry posted since the last doorbell.
 */
static void drain_bufh (void)
{
    uint32 src = mem (A_BUFH) & RING_MASK;

    while (bufh_recv != src) {
        int abonent = mem (A_BUFH + 2 + bufh_recv);
        int n;

        bufh_recv = (bufh_recv + 1) & RING_MASK;
        set_mem (A_BUFH + 1, bufh_recv);
        sim_debug (DBG_RING, &dks_dev, "got %d\n", abonent);
        n = find_line (abonent);
        if (n)
            take_output (n);
    }
    /* The reply to a connect request: абфосh+0 is written last. */
    if (sys_busy == 1 && mem (A_FOSH) == 2) {
        struct dks_line *l = &line[sys_line];
        int term = mem (A_FOSH + 1);
        int result = mem (A_FOSH + 2);

        set_mem (A_FOSH, 0);
        sys_busy = 0;
        sim_debug (DBG_RING, &dks_dev, "line %d: connect reply term %d result %d\n",
                   sys_line, term, result);
        if (result == 0) {
            l->term = term;
        } else {
            vt_puts (sys_line, result == 1 ? "\r\nDKS: line busy\r\n" :
                     "\r\nDKS: no free terminal\r\n");
            l->up = 0;
        }
    }
}

static void post_sys_request (int n, int code)
{
    int a = AREA(n);

    set_mem (A_FOSS, code);
    set_mem (A_FOSS + 1, code == 1 ? n : line[n].term);
    set_mem (A_FOSS + 2, code == 1 ? a << 1 : 0);
    sys_src = mem (A_BUFS) & RING_MASK;
    if (! e60_post (ABONENT_OS))
        return;
    sys_busy = code;
    sys_line = n;
    sim_debug (DBG_RING, &dks_dev, "line %d: system request %d\n", n, code);
}

static void start_line (int n)
{
    int a = AREA(n), i;

    for (i = 0; i < 8; ++i)
        set_mem (a + i, 0);
    set_mem (a + AR_INBUF, (a + AREA_INBUF) << 1);
    line[n].len = 0;
    line[n].line_ready = 0;
    line[n].pending = 0;
    line[n].out_done = 0;
}

/*
 * Is the OS done with the last event of this line?
 */
static void check_ack (int n)
{
    int a = AREA(n);

    if (line[n].pending == 4 && ! (mem (a + AR_STATUS0) & AR_FLAG))
        line[n].pending = 0;
    else if (line[n].pending == 5 && ! (mem (a + AR_STATUS1) & AR_FLAG))
        line[n].pending = 0;
}

static void post_line_event (int n, int cmd)
{
    int a = AREA(n);

    set_mem (a + AR_CMD_IN, cmd);
    if (cmd == 4)
        set_mem (a + AR_STATUS0, mem (a + AR_STATUS0) | AR_FLAG);
    if (! e60_post (line[n].term)) {
        set_mem (a + AR_CMD_IN, 0);
        return;
    }
    line[n].pending = cmd;
}

static void pass_input (int n)
{
    int a = AREA(n) + AREA_INBUF, i;
    struct dks_line *l = &line[n];

    for (i = 0; i <= l->len; i += 2) {
        uint32 lo = i < l->len ? (unsigned char) l->buf[i] : 0;
        uint32 hi = i + 1 < l->len ? (unsigned char) l->buf[i+1] : 0;
        set_mem (a + i/2, lo | hi << 8);
    }
    post_line_event (n, 4);
    if (l->pending == 4) {
        sim_debug (DBG_RING, &dks_dev, "line %d: input %d chars\n", n, l->len);
        l->len = 0;
        l->line_ready = 0;
    }
}

/*
 * Advance the Э-60 state machine; called by the unit service and by the
 * terminal poll.
 */
static void dks_process (void)
{
    int n;

    if (! dks_enabled ())
        return;
    drain_bufh ();

    /* A disconnect gets no reply: wait until the OS has read the ring. */
    if (sys_busy == 3) {
        uint32 posted = (mem (A_BUFS) - sys_src) & RING_MASK;
        uint32 taken = (mem (A_BUFS + 1) - sys_src) & RING_MASK;

        if (taken != 0 && taken <= posted) {
            sys_busy = 0;
            line[sys_line].term = 0;
        }
    }
    for (n = 1; n <= DKS_LINES; ++n) {
        struct dks_line *l = &line[n];

        if (! sys_busy && l->want_disconnect) {
            l->want_disconnect = 0;
            if (l->term)
                post_sys_request (n, 3);
            l->up = 0;
            continue;
        }
        if (! sys_busy && l->want_connect && os_ready ()) {
            l->want_connect = 0;
            start_line (n);
            l->up = 1;
            post_sys_request (n, 1);
            if (! sys_busy) {
                l->up = 0;
                l->want_connect = 1;
            }
            continue;
        }
        if (! l->term)
            continue;
        check_ack (n);
        if (l->pending)
            continue;
        if (l->out_done) {
            post_line_event (n, 5);
            if (l->pending)
                l->out_done = 0;
        } else if (l->line_ready)
            pass_input (n);
    }
}

t_stat dks_svc (UNIT *u)
{
    dks_process ();
    return SCPE_OK;
}

t_stat dks_reset (DEVICE *dptr)
{
    int n;

    sim_cancel (&dks_unit);
    memset (e60_mem, 0, sizeof (e60_mem));
    cur_chan = 0;
    os_polled = 0;
    sys_busy = 0;
    bufh_recv = 0;
    for (n = 1; n <= DKS_LINES; ++n) {
        int up = line[n].up || line[n].want_connect;
        memset (&line[n], 0, sizeof (line[n]));
        line[n].echo = 1;
        line[n].want_connect = up;
    }
    return SCPE_OK;
}

/*
 * Interface for besm6_tty.c.
 */
void dks_line_state (int n, int connected)
{
    struct dks_line *l = &line[n];

    if (connected) {
        l->want_disconnect = 0;
        l->want_connect = 1;
        l->echo = 1;
        vt_puts (n, "Connected to DKS\r\n");
    } else {
        l->want_connect = 0;
        if (l->up)
            l->want_disconnect = 1;
    }
}

/* Whether the line accepts another typed character now. */
int dks_line_can_input (int n)
{
    return dks_enabled () && line[n].term && ! line[n].line_ready;
}

/* Turns local echo on or off; for ТАЙНА, once its trigger is known. */
void e60_set_echo (int n, int on)
{
    line[n].echo = on;
}

/*
 * A character typed on the line (KOI-7): local editing and echo.
 */
void dks_line_char (int n, int c)
{
    struct dks_line *l = &line[n];

    switch (c) {
    case '\r':
    case '\n':
        if (l->echo)
            vt_send (n, '\n');
        l->line_ready = 1;
        break;
    case '\b':
    case 0177:
        if (l->len > 0) {
            --l->len;
            if (l->echo)
                vt_puts (n, "\b \b");
        }
        break;
    default:
        if (l->len >= INBUF_BYTES - 1)
            break;
        l->buf[l->len++] = c;
        if (l->echo)
            vt_send (n, c);
    }
}

/* Called on every terminal clock tick. */
void dks_poll (void)
{
    dks_process ();
}

int dks_busy (void)
{
    int n;

    if (! dks_enabled ())
        return 0;
    if (sys_busy)
        return 1;
    for (n = 1; n <= DKS_LINES; ++n)
        if ((line[n].want_connect && os_ready ()) || line[n].want_disconnect ||
            line[n].pending || line[n].out_done || line[n].line_ready)
            return 1;
    return 0;
}
