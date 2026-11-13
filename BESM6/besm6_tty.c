/*
 * besm6_tty.c: BESM-6 teletype device
 *
 * Copyright (c) 2009-2017, Leo Broukhis
 * Copyright (c) 2009, Serge Vakulenko
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

#include "besm6_defs.h"
#include "sim_tmxr.h"

/*
 * Physical lines (SIMH unit numbers, decimal):
 *   1-24  (01-030)  bit-serial "direct" lines, or lines of the terminal multiplexer;
 *   25-26 (031-032) the two parallel "Consul" interfaces, never assigned to telnet;
 *   27-64 (033-0100) lines of the terminal multiplexer only.
 * At most one multiplexer is configured per session: ДКС, ТМГУ or АС
 * ("аппаратура сопряжения"), see mux_kind.
 */
#define TTY_MAX         24              /* Serial TTY lines */
#define CONSUL_FIRST    (TTY_MAX + 1)   /* Consul 0 = 031, Consul 1 = 032 */
#define LINES_MAX       64              /* Including Consuls and multiplexer lines */
#define DKS_MAX_LINE    41              /* the Э-60 memory holds areas for 41 lines */
/*
 * According to a table in http://ru.wikipedia.org/wiki/МТК-2
 */
char * rus[] = { 0, "Т", "\r", "О", " ", "Х", "Н", "М", "\n", "Л", "Р", "Г", "И", "П", "Ц", "Ж",
                 "Е", "З", "Д", "Б", "С", "Ы", "Ф", "Ь", "А", "В", "Й", 0, "У", "Я", "К", 0 };

char * lat[] = { 0, "T", "\r", "O", " ", "H", "N", "M", "\n", "L", "R", "G", "I", "P", "C", "V",
                 "E", "Z", "D", "B", "S", "Y", "F", "X", "A", "W", "J", 0, "U", "Q", "K", 0 };

/* $ = Кто там? */
char * dig[] = { 0, "5", "\r", "9", " ", "Щ", ",", ".", "\n", ")", "4", "Ш", "8", "0", ":", "=",
                 "3", "+", "$", "?", "'", "6", "Э", "/", "-", "2", "Ю", 0, "7", "1", "(", 0 };

/* Register shift codes, and the codes that print the same in every register. */
#define MTK2_RUS        0
#define MTK2_DIG        033
#define MTK2_LAT        037
#define MTK2_SPACE      4
#define MTK2_LF         010

static char ** const mtk2_reg [3] = { rus, lat, dig };
static const int mtk2_shift [3] = { MTK2_RUS, MTK2_LAT, MTK2_DIG };

/* For serial lines */
int tty_active [TTY_MAX+1], tty_sym [TTY_MAX+1];
int tty_typed [TTY_MAX+1], tty_instate [TTY_MAX+1];

/* Teletype registers, as indexes into mtk2_reg[]: the one the printer is in, and
 * the one the guest last saw the keyboard select (-1: unknown). */
static int tt_outreg [TTY_MAX+1], tt_inreg [TTY_MAX+1];
/* An MTK-2 code to send after the one in tty_typed[], or -1. */
static int tt_next [TTY_MAX+1];

/*
 * The guest reads teletype input through a 32-bit left shift (ОТТVТ in МОТТ), where
 * Videoton input takes 24 (VТ19): a teletype on line N answers on bit 17-N of the
 * input register, the bit Videoton line N+8 uses, and lines 17-24 have none.
 * Output is on bit 25-N for both.
 */
#define TT_LINES        16

static uint32 tt_inbit (int num)
{
    return num <= TT_LINES ? 1 << (TT_LINES - num) : 0;
}

static char * process (int num, int sym)
{
    /* Inversion is required for Baudot TTYs */
    sym ^= 31;
    switch (sym) {
    case MTK2_RUS:
        tt_outreg[num] = 0;
        break;
    case MTK2_LAT:
        tt_outreg[num] = 1;
        break;
    case MTK2_DIG:
        tt_outreg[num] = 2;
        break;
    default:
        return mtk2_reg[tt_outreg[num]][sym];
    }
    return "";
}

/*
 * A KOI-7 keystroke as an MTK-2 code, or -1 if the teletype has no such key.
 * *regp is the register the code needs, or -1 if it prints in any.
 */
extern const char * koi7_rus_to_unicode [32];

static int tomtk2 (int c, int *regp)
{
    char buf [2];
    const char *s;
    int r, code;

    *regp = -1;
    switch (c) {
    case 3: case '\r': case '\n':
        /* The guest ends a line on ПС; ВК is only for the printer. */
        return MTK2_LF;
    case ' ':
        return MTK2_SPACE;
    }
    if (c < ' ' || c > 0x7e)
        return -1;
    if (c >= 0x60)
        s = koi7_rus_to_unicode[c - 0x60];
    else {
        buf[0] = c;
        buf[1] = 0;
        s = buf;
    }
    for (r = 0; r < 3; ++r)
        for (code = 1; code < 32; ++code)
            if (mtk2_reg[r][code] && strcmp (mtk2_reg[r][code], s) == 0) {
                *regp = r;
                return code;
            }
    return -1;
}

/* For all lines */
time_t tty_last_time [LINES_MAX+1];
int tty_idle_count [LINES_MAX+1];
int32 tty_lnorder[LINES_MAX+1] = { -1 };

/* The serial interrupt generator frequency, common for all VT lines */
int tty_rate = 300;

/* Interrupt generator mode: 1 - model time, 0 - wallclock time */
int tty_turbo = 1;

uint32 vt_sending, vt_receiving;
uint32 tt_sending, tt_receiving;

// Attachments survive the reset
uint32 tt_mask = 0, vt_mask = 0;

uint32 TTY_OUT = 0, TTY_IN = 0, vt_idle = 0;
uint32 CONSUL_IN[2];

/* Set when a typed character is placed in CONSUL_IN and cleared when the guest reads
 * the register.  CONSUL_IN is one character deep, so without this consul_receive()
 * overwrites a character the guest has not taken yet -- see the comment there. */
static char cons_input_pending[2];
uint32 MUX_SYLLABLE, mux_reg_busy;

uint32 CONS_CAN_PRINT[2] = { 01000, 00400 };
uint32 CONS_HAS_INPUT[2] = { 04000, 02000 };

uint32 CONS_READY[2] = { 0200, 040 };

/*
 * ТМГУ: Consul 0 is the МАМС concentrator channel for ТЕРМАС
 * (увв '174'/'4174', READY2 E8 out / E7 in, ПРП 10/12). Key exchange
 * is 377 252 125 both ways (ТЕРМАС КЛПВВ/КЛПВЫВ). After link-up, a
 * terminal-protocol connect (opcode 041) is queued for each terminal.
 * `set tty tmgu` chooses it as the multiplexer; `tmgutrace` also logs
 * concentrator bytes without `set tty debug`.
 *
 * Input flow control: after the BESM reads a byte (увв '4174') the next one
 * is offered only after ВДЗП := 1 (увв '176'), as ТЕРМАС ВЫХВВ/ТЕРМИН/СЛУЖ
 * issue it.  The last byte of a message is acknowledged only after ТЕРМАС
 * has taken the message out of БУФВВ.
 *
 * Output frames are parsed: addr byte with E8 -> count n and n info bytes,
 * without E8 -> 2 info bytes (phys, symbol).  The 042 reply gives the
 * logical number; a short frame with symbol 000 ends the output (ТЕРМАС
 * СТЕР), and the concentrator answers 047 (ОКНВЫД, end of exchange).
 *
 * Every multiplexer line (see line_kind()) is a terminal behind the
 * concentrator, its physical number being the SIMH line number.  After the key
 * exchange a connect 041 is queued for every line 1-24 put on the multiplexer
 * ("set ttyN mux": a wired terminal, on from the start) and for every other
 * multiplexer line once it is connected (telnet or console); one at a time, the
 * next one waits for the 042 reply with the logical number.  No disconnect
 * message is known: a line keeps its logical number when the client goes away.
 * A short output frame is address, physical number (ТНФ), symbol; the symbols
 * go to the line of that number (number 0: a terminal not connected yet, the
 * output is dropped); typed characters go to the BESM as short frames
 * 001 <logical number> <KOI-7 with even parity>, echoed locally like on АС lines.
 */
#define TMGU_IN_RDY   0100              /* E7 — готовность ввода */
#define TMGU_OUT_RDY  0200              /* E8 — готовность вывода (= CONS_READY[0]) */
#define TMGU_QMAX     64

/* The terminal multiplexer of this session, chosen at most once. */
#define MUX_NONE        0
#define MUX_DKS         1
#define MUX_TMGU        2
#define MUX_AS          3
static int mux_kind;
static const char *const mux_name[] = { "none", "ДКС", "ТМГУ", "АС" };
#define tmgu_on         (mux_kind == MUX_TMGU)

static int line_is_mux (int num);

static int tmgu_key_out;                /* matched bytes of outgoing key (0..3) */
static int tmgu_key_in;                 /* next key index to feed BESM (0..3) */
static int tmgu_linked;                 /* both keys done */
static int tmgu_trace;
#define TMGU_TRACE (tty_dev.dctrl || tmgu_trace)
static unsigned char tmgu_inq[TMGU_QMAX];
static int tmgu_in_len, tmgu_in_pos;
static const unsigned char tmgu_key[3] = { 0377, 0252, 0125 };
static int tmgu_acked = 1;              /* ВДЗП: можно предлагать следующий байт */
static unsigned char tmgu_ofr[16];      /* собираемый выходной кадр */
static int tmgu_ofr_len, tmgu_ofr_need;
static int tmgu_log [LINES_MAX+1];      /* лог. номер линии из ответа 042, 0 - нет */
static char tmgu_up [LINES_MAX+1];      /* линия была подключена при прошлом опросе */
static char tmgu_want [LINES_MAX+1];    /* нужно послать 041 */
static int tmgu_wait;                   /* физ. номер, ждущий 042, 0 - никто */

static void tmgu_offer_input (void);
static void tmgu_poll_lines (void);

static void tmgu_queue (const unsigned char *p, int n)
{
    while (n-- > 0 && tmgu_in_len < TMGU_QMAX)
        tmgu_inq[tmgu_in_len++] = *p++;
    tmgu_offer_input ();
}

/* Forgets the link and every terminal; lines connected now ask again after it. */
static void tmgu_reset_link (void)
{
    int n;

    tmgu_key_out = 0;
    tmgu_key_in = 0;
    tmgu_linked = 0;
    tmgu_in_len = tmgu_in_pos = 0;
    tmgu_acked = 1;
    tmgu_ofr_len = tmgu_ofr_need = 0;
    tmgu_wait = 0;
    for (n = 0; n <= LINES_MAX; ++n) {
        tmgu_log[n] = 0;
        tmgu_up[n] = 0;
        tmgu_want[n] = 0;
    }
}

static void tmgu_send_connect (int phys)
{
    /* Level-1 frame: addr|E8, length, info.
     * Info word БУФВВ+2: byte0=0, byte1=041 (connect), byte2=phys. */
    unsigned char msg[] = {
        0201, 6, 0, 041, 1, 0, 0, 0
    };
    msg[4] = (unsigned char) phys;
    tmgu_wait = phys;
    tmgu_want[phys] = 0;
    if (TMGU_TRACE)
        besm6_debug (">>> TMGU: queue connect 041 phys=%o", phys);
    tmgu_queue (msg, (int) sizeof msg);
}

static void tmgu_check_linked (void)
{
    int n;

    if (tmgu_linked || tmgu_key_in < 3 || tmgu_key_out < 3)
        return;
    tmgu_linked = 1;
    if (TMGU_TRACE)
        besm6_debug ("### TMGU: key exchange done");
    /* Lines put on the concentrator explicitly are wired terminals: they are
     * "on" from the link-up, connected or not (the OS writes the start-up
     * text to an operator terminal before anyone connects to it). */
    for (n = 1; n <= TTY_MAX; ++n)
        if (line_is_mux (n))
            tmgu_want[n] = 1;
    tmgu_poll_lines ();
}

static void tmgu_offer_input (void)
{
    unsigned char b;

    if (!tmgu_on || cons_input_pending[0] || !tmgu_acked)
        return;
    if (tmgu_key_in < 3)
        b = tmgu_key[tmgu_key_in];
    else if (tmgu_in_pos < tmgu_in_len)
        b = tmgu_inq[tmgu_in_pos];
    else
        return;
    CONSUL_IN[0] = b;
    cons_input_pending[0] = 1;
    READY2 |= TMGU_IN_RDY;
    PRP |= CONS_HAS_INPUT[0];
    if (TMGU_TRACE)
        besm6_debug ("<<< TMGU: %03o", b);
}

/* увв '176': ВДЗП := 1, ТЕРМАС принял байт ввода. */
void tmgu_input_ack (void)
{
    if (!tmgu_on)
        return;
    if (TMGU_TRACE && !tmgu_acked)
        besm6_debug ("<<< TMGU: ВДЗП");
    tmgu_acked = 1;
    tmgu_offer_input ();
}

/* Полный выходной кадр ТЕРМАС: p[0] - адрес, далее информация. */
static void tmgu_out_frame (const unsigned char *p, int n)
{
    if ((p[0] & 0200) && n >= 6 && p[3] == 042) {
        /* ответ на подключение: 001 042 физ лог ... */
        if (p[4] >= 1 && p[4] <= LINES_MAX)
            tmgu_log[p[4]] = p[5];
        if (tmgu_wait == p[4])
            tmgu_wait = 0;
        if (TMGU_TRACE)
            besm6_debug (">>> TMGU: 042 phys=%o log=%o", p[4], p[5]);
    } else if (!(p[0] & 0200) && n == 3 && p[2] != 0) {
        unsigned c = p[2] & 0177;
        if (p[1] >= 1 && p[1] <= LINES_MAX && line_is_mux (p[1]) && c != 0177)
            vt_send (p[1], c);
    } else if (!(p[0] & 0200) && n == 3 && p[2] == 0 &&
               p[1] >= 1 && p[1] <= LINES_MAX && tmgu_log[p[1]]) {
        /* конец выдачи -> 047 ОКОНЧАНИЕ ОБМЕНА, кредит 0 (бесконечный) */
        unsigned char msg[] = { 0201, 6, 0, 047, 0, 0, 0, 0 };
        msg[4] = (unsigned char) tmgu_log[p[1]];
        if (TMGU_TRACE)
            besm6_debug (">>> TMGU: end of output phys=%o, queue 047 log=%o",
                         p[1], msg[4]);
        tmgu_queue (msg, (int) sizeof msg);
    }
}

static void tmgu_out_byte (unsigned b)
{
    if (tmgu_ofr_len == 0)
        tmgu_ofr_need = (b & 0200) ? 2 : 3;     /* адрес [+ счетчик] */
    else if (tmgu_ofr_len == 1 && (tmgu_ofr[0] & 0200))
        tmgu_ofr_need = 2 + (int) b;
    if (tmgu_ofr_len < (int) sizeof tmgu_ofr)
        tmgu_ofr[tmgu_ofr_len] = (unsigned char) b;
    if (++tmgu_ofr_len >= tmgu_ofr_need) {
        if (tmgu_ofr_len > (int) sizeof tmgu_ofr)
            tmgu_ofr_len = (int) sizeof tmgu_ofr;
        tmgu_out_frame (tmgu_ofr, tmgu_ofr_len);
        tmgu_ofr_len = 0;
    }
}

/* Command line buffers for TELNET mode. */
char vt_cbuf [CBUFSIZE] [LINES_MAX+1];
char *vt_cptr [LINES_MAX+1];

void tt_print();
void consul_receive();
void mux_receive (void);
void dks_receive (void);
t_stat vt_clk(UNIT *);
t_stat mux_event(UNIT *);
extern const char *get_sim_sw (const char *cptr);

int attached_console;

/* АС (mux_send): the guest has turned reception off on this line. */
static char mux_rcv_off [LINES_MAX+1];
static void mux_reset_state (void);

UNIT tty_unit [] = {
    { UDATA (vt_clk, UNIT_DIS|UNIT_IDLE, 0) },       /* fake unit, clock */
    /* 1-24: direct or multiplexer lines */
    { UDATA (mux_event, UNIT_SEQ, 0) },
    { UDATA (mux_event, UNIT_SEQ, 0) },
    { UDATA (mux_event, UNIT_SEQ, 0) },
    { UDATA (mux_event, UNIT_SEQ, 0) },
    { UDATA (mux_event, UNIT_SEQ, 0) },
    { UDATA (mux_event, UNIT_SEQ, 0) },
    { UDATA (mux_event, UNIT_SEQ, 0) },
    { UDATA (mux_event, UNIT_SEQ, 0) },
    { UDATA (mux_event, UNIT_SEQ, 0) },
    { UDATA (mux_event, UNIT_SEQ, 0) },
    { UDATA (mux_event, UNIT_SEQ, 0) },
    { UDATA (mux_event, UNIT_SEQ, 0) },
    { UDATA (mux_event, UNIT_SEQ, 0) },
    { UDATA (mux_event, UNIT_SEQ, 0) },
    { UDATA (mux_event, UNIT_SEQ, 0) },
    { UDATA (mux_event, UNIT_SEQ, 0) },
    { UDATA (mux_event, UNIT_SEQ, 0) },
    { UDATA (mux_event, UNIT_SEQ, 0) },
    { UDATA (mux_event, UNIT_SEQ, 0) },
    { UDATA (mux_event, UNIT_SEQ, 0) },
    { UDATA (mux_event, UNIT_SEQ, 0) },
    { UDATA (mux_event, UNIT_SEQ, 0) },
    { UDATA (mux_event, UNIT_SEQ, 0) },
    { UDATA (mux_event, UNIT_SEQ, 0) },
    /* 25-26: the parallel (Consul) interface */
    { UDATA (NULL, UNIT_SEQ, 0) },
    { UDATA (NULL, UNIT_SEQ, 0) },
    /* 27-64: multiplexer lines */
    { UDATA (mux_event, UNIT_SEQ, 0) },
    { UDATA (mux_event, UNIT_SEQ, 0) },
    { UDATA (mux_event, UNIT_SEQ, 0) },
    { UDATA (mux_event, UNIT_SEQ, 0) },
    { UDATA (mux_event, UNIT_SEQ, 0) },
    { UDATA (mux_event, UNIT_SEQ, 0) },
    { UDATA (mux_event, UNIT_SEQ, 0) },
    { UDATA (mux_event, UNIT_SEQ, 0) },
    { UDATA (mux_event, UNIT_SEQ, 0) },
    { UDATA (mux_event, UNIT_SEQ, 0) },
    { UDATA (mux_event, UNIT_SEQ, 0) },
    { UDATA (mux_event, UNIT_SEQ, 0) },
    { UDATA (mux_event, UNIT_SEQ, 0) },
    { UDATA (mux_event, UNIT_SEQ, 0) },
    { UDATA (mux_event, UNIT_SEQ, 0) },
    { UDATA (mux_event, UNIT_SEQ, 0) },
    { UDATA (mux_event, UNIT_SEQ, 0) },
    { UDATA (mux_event, UNIT_SEQ, 0) },
    { UDATA (mux_event, UNIT_SEQ, 0) },
    { UDATA (mux_event, UNIT_SEQ, 0) },
    { UDATA (mux_event, UNIT_SEQ, 0) },
    { UDATA (mux_event, UNIT_SEQ, 0) },
    { UDATA (mux_event, UNIT_SEQ, 0) },
    { UDATA (mux_event, UNIT_SEQ, 0) },
    { UDATA (mux_event, UNIT_SEQ, 0) },
    { UDATA (mux_event, UNIT_SEQ, 0) },
    { UDATA (mux_event, UNIT_SEQ, 0) },
    { UDATA (mux_event, UNIT_SEQ, 0) },
    { UDATA (mux_event, UNIT_SEQ, 0) },
    { UDATA (mux_event, UNIT_SEQ, 0) },
    { UDATA (mux_event, UNIT_SEQ, 0) },
    { UDATA (mux_event, UNIT_SEQ, 0) },
    { UDATA (mux_event, UNIT_SEQ, 0) },
    { UDATA (mux_event, UNIT_SEQ, 0) },
    { UDATA (mux_event, UNIT_SEQ, 0) },
    { UDATA (mux_event, UNIT_SEQ, 0) },
    { UDATA (mux_event, UNIT_SEQ, 0) },
    { UDATA (mux_event, UNIT_SEQ, 0) },
    { 0 }
};

REG tty_reg[] = {
    { 0 }
};

/*
 * Line descriptors for the TMXR multiplexor.
 * The .conn field contains the socket number and denotes a used line.
 * For local terminals, .conn = 1.
 * To match line indexes with TTY numbers (1-based),
 * line 0 is kept used (.conn = 1).
 * The .rcve field is set to 1 for network connections.
 * For local connections, it is 0.
 */
TMLN tty_line [LINES_MAX+1];
TMXR tty_desc = { LINES_MAX+1, 0, 0, tty_line, tty_lnorder };        /* mux descriptor */

/* The charset field is three bits wide: five encodings, of which two are raw. */
#define TTY_UNICODE_CHARSET     0
#define TTY_KOI7_JCUKEN_CHARSET (1<<UNIT_V_UF)
#define TTY_KOI7_QWERTY_CHARSET (2<<UNIT_V_UF)
#define TTY_RAW_CHARSET         (3<<UNIT_V_UF)
#define TTY_RAW8_CHARSET        (4<<UNIT_V_UF)
#define TTY_CHARSET_MASK        (7<<UNIT_V_UF)
#define TTY_OFFLINE_STATE       0
#define TTY_TELETYPE_STATE      (1<<(UNIT_V_UF+3))
#define TTY_VT340_STATE         (2<<(UNIT_V_UF+3))
#define TTY_CONSUL_STATE        (3<<(UNIT_V_UF+3))
#define TTY_STATE_MASK          (3<<(UNIT_V_UF+3))
#define TTY_DESTRUCTIVE_BSPACE  0
#define TTY_AUTHENTIC_BSPACE    (1<<(UNIT_V_UF+5))
#define TTY_BSPACE_MASK         (1<<(UNIT_V_UF+5))
#define TTY_CMDLINE_MASK        (1<<(UNIT_V_UF+6))
#define TTY_INVERSE_READY       (1<<(UNIT_V_UF+7))
#define TTY_MUX_MODE            (1<<(UNIT_V_UF+8))  /* line 1-24 is on the multiplexer */

/*
 * What a physical line is in the current configuration.
 */
#define LINE_DIRECT     0       /* bit-serial line 1-24 */
#define LINE_CONSUL     1       /* Consul interface 25-26 */
#define LINE_MUX        2       /* a line of the configured multiplexer */
#define LINE_UNUSED     3       /* no hardware behind it in this configuration */

static int line_kind (int num)
{
    if (num < 1 || num > LINES_MAX)
        return LINE_UNUSED;
    if (num == CONSUL_FIRST) {
        /* ТМГУ uses Consul 0 as the concentrator channel; under ДКС its input
         * interrupt (ПРП 12) is the "attention" signal of the Э-60. */
        return (mux_kind == MUX_TMGU || mux_kind == MUX_DKS) ?
            LINE_UNUSED : LINE_CONSUL;
    }
    if (num == CONSUL_FIRST + 1)
        return LINE_CONSUL;
    if (mux_kind == MUX_DKS && num > DKS_MAX_LINE)
        return LINE_UNUSED;
    if (num <= TTY_MAX)
        return (mux_kind != MUX_NONE && (tty_unit[num].flags & TTY_MUX_MODE)) ?
            LINE_MUX : LINE_DIRECT;
    return mux_kind != MUX_NONE ? LINE_MUX : LINE_UNUSED;
}

/* A bit-serial line: driven through the 033 0140 / 033 4100 bit registers. */
#define TTY_SERIAL(num) (line_kind (num) == LINE_DIRECT)

static int line_is_mux (int num)
{
    return line_kind (num) == LINE_MUX;
}

/* An error for a line that cannot be used in this configuration. */
static t_stat check_usable (int num)
{
    if (num == 0 || line_kind (num) != LINE_UNUSED)
        return SCPE_OK;
    if (num == CONSUL_FIRST)
        return sim_messagef (SCPE_ARG, "tty%d (Консул 0, 031) занят мультиплексором %s\n",
                             num, mux_name[mux_kind]);
    if (mux_kind == MUX_NONE)
        return sim_messagef (SCPE_ARG, "tty%d (%03o): линия мультиплексора; "
                             "мультиплексор не выбран (set tty dks|tmgu|mux)\n", num, num);
    return sim_messagef (SCPE_ARG, "tty%d (%03o): нет такой линии у %s\n",
                         num, num, mux_name[mux_kind]);
}

/* Validation routine for per-line MTAB entries: no settings on an unusable line. */
static t_stat tty_check_unit (UNIT *u, int32 val, CONST char *cptr, void *desc)
{
    return check_usable ((int) (u - tty_unit));
}

/*
 * Both raw encodings are byte pipes: no KOI-7 tables, no Videoton control codes,
 * no Enter/Backspace fix-up.
 */
static int tty_raw (int num)
{
    int charset = tty_unit[num].flags & TTY_CHARSET_MASK;

    return charset == TTY_RAW_CHARSET || charset == TTY_RAW8_CHARSET;
}

/*
 * RAW8 additionally carries all eight bits of a character and computes no parity,
 * where RAW keeps the authentic 7-bits-plus-parity contract of the hardware.
 */
static int tty_raw8 (int num)
{
    return (tty_unit[num].flags & TTY_CHARSET_MASK) == TTY_RAW8_CHARSET;
}

static void reset_line(int num)
{
    /* Reset to a sensible default */
    tty_unit[num].flags &= ~(TTY_CHARSET_MASK|TTY_BSPACE_MASK|TTY_CMDLINE_MASK|TTY_INVERSE_READY);
    if (num <= TTY_MAX) {
        tty_typed[num] = -1;
        tty_instate[num] = 0;
        tt_inreg[num] = tt_next[num] = -1;
        tt_outreg[num] = 0;
    }
}

t_stat tty_reset (DEVICE *dptr)
{
    memset(tty_active, 0, sizeof(tty_active));
    memset(tty_sym, 0, sizeof(tty_sym));
    memset(tty_typed, 0, sizeof(tty_typed));
    memset(tty_instate, 0, sizeof(tty_instate));
    vt_sending = vt_receiving = 0;
    TTY_IN = TTY_OUT = 0;
    CONSUL_IN[0] = CONSUL_IN[1] = 0;
    cons_input_pending[0] = cons_input_pending[1] = 0;
    memset(tt_outreg, 0, sizeof(tt_outreg));
    memset(tt_inreg, -1, sizeof(tt_inreg));
    memset(tt_next, -1, sizeof(tt_next));
    READY2 |= CONS_READY[0] | CONS_READY[1];
    if (tty_unit[25].flags & TTY_INVERSE_READY)
        READY2 &= ~CONS_READY[0];
    if (tty_unit[26].flags & TTY_INVERSE_READY)
        READY2 &= ~CONS_READY[1];
    memset(mux_rcv_off, 0, sizeof(mux_rcv_off));
    mux_reset_state ();
    tmgu_reset_link ();
    if (tmgu_on) {
        READY2 |= TMGU_OUT_RDY | TMGU_IN_RDY;
        tmgu_offer_input ();
    }
    vt_idle = 1;
    tty_line[0].conn = 1;                   /* faked, always busy */
    PRP |= CONS_CAN_PRINT[0] | CONS_CAN_PRINT[1];
    // Schedule the very first TTY interrupt to match the next clock interrupt.
    return sim_clock_coschedule (tty_unit, 0);
}

/* Bit 19 of GRP, should be <tty_rate> Hz */
t_stat vt_clk (UNIT * this)
{
    int num;
    /* Whether this line was already in vt_mask when a client connected, i.e. the
     * guest still believes it is live and it needs re-initializing.  Only the
     * bit-serial lines can ever be in that state: a mux line and both Consuls
     * never enter vt_mask, so zero is what they are.  This used to be
     * uninitialized, and the "Type HYC" hint below was then printed or not on a
     * garbage value for exactly those lines. */
    int old = 0;
    GRP |= MGRP & GRP_SERIAL;

    /* With КАДОПАМ, ПРП 8 is the interrupt of КРК channel 0.  With `set tty
     * dks` but without the ДКС device it is faked while the mask is open.
     * Otherwise ПРП 8 belongs to the CalComp plotter (CLC). */
    if ((MPRP & 0200) && mux_kind == MUX_DKS && (dks_dev.flags & DEV_DIS)) {
        static int bit;
        PRP |= 0200;
        READY = READY ^ (1 << (bit++ % 24));
    }
    /* Polling receiving from sockets */
    tmxr_poll_rx (&tty_desc);

    vt_print();
    vt_receive();
    consul_receive();
    mux_receive();
    dks_receive();

    /* Are there any new network connections?  One on the common port never
     * gets a Consul (031, 032) nor a line unusable in this configuration: those
     * are marked busy for the time of the poll.  A Consul with a port of its own
     * ("attach tty Line=25,port") still accepts connections there, and one
     * dialling out ("Line=26,Connect=host:port") completes its connection. */
    {
        char blocked [LINES_MAX+1];
        int n;
        for (num = 1; num <= LINES_MAX; ++num) {
            int k = line_kind (num);
            blocked[num] = ((k == LINE_CONSUL && !tty_line[num].master &&
                             !tty_line[num].destination) ||
                            k == LINE_UNUSED) && !tty_line[num].conn;
            if (blocked[num])
                tty_line[num].conn = 1;
        }
        num = tmxr_poll_conn (&tty_desc);
        for (n = 1; n <= LINES_MAX; ++n)
            if (blocked[n])
                tty_line[n].conn = 0;
    }
    if (num > 0 && num <= LINES_MAX) {
        char buf [80];
        TMLN *t = &tty_line [num];
        /* Sampled BEFORE reset_line(), which clears TTY_CHARSET_MASK. */
        int charset = tty_unit[num].flags & TTY_CHARSET_MASK;
        besm6_debug ("*** tty%d: a new connection from %s",
                     num, t->ipad);
        reset_line (num);
        /* A line the operator explicitly made raw STAYS raw across the connect.
         * Without this a "set ttyN raw" issued before the client connected was
         * silently discarded and the line came up UTF-8 -- and the "Encoding is
         * RAW" message a dozen lines below was unreachable code. */
        if (charset == TTY_RAW_CHARSET || charset == TTY_RAW8_CHARSET)
            tty_unit[num].flags |= charset;
        t->rcve = 1;
        tty_unit[num].flags &= ~TTY_STATE_MASK;
        tty_unit[num].flags |= TTY_VT340_STATE;
        if (TTY_SERIAL (num)) {
	    old = vt_mask & (1 << (TTY_MAX - num));
            vt_mask |= 1 << (TTY_MAX - num);
	}
        switch (tty_unit[num].flags & TTY_CHARSET_MASK) {
        case TTY_KOI7_JCUKEN_CHARSET:
            tmxr_linemsg (t, "Encoding is KOI-7 (jcuken)\r\n");
            break;
        case TTY_KOI7_QWERTY_CHARSET:
            tmxr_linemsg (t, "Encoding is KOI-7 (qwerty)\r\n");
            break;
        case TTY_RAW_CHARSET:
            tmxr_linemsg (t, "Encoding is RAW\r\n");
            break;
        case TTY_RAW8_CHARSET:
            tmxr_linemsg (t, "Encoding is RAW8\r\n");
            break;
        case TTY_UNICODE_CHARSET:
            tmxr_linemsg (t, "Encoding is UTF-8\r\n");
            break;
        }
        if (sim_int_char < 040 || sim_int_char == 0177) {
            sprintf (buf, "WRU – Break to sim> prompt character - is ^%c\r\n",
                     sim_int_char ^ 0100);
        } else {
            sprintf (buf, "WRU – Break to sim> prompt character - is %c\r\n",
                     sim_int_char);
        }
        tmxr_linemsg (t, buf);
        tty_idle_count[num] = 0;
        tty_last_time[num] = sim_get_time (0);
        sprintf (buf, "%.24s from %s\r\n",
                 ctime (&tty_last_time[num]),
                 t->ipad);
        tmxr_linemsg (t, buf);

        /* Entering ^C (ETX) to get a prompt.  Not on a raw8 line: there the byte is
         * data, not a request, and it reaches the guest as a typed character. */
        if (charset != TTY_RAW8_CHARSET &&
            !(line_is_mux (num) && (mux_kind == MUX_DKS || mux_kind == MUX_TMGU)))
            t->rxb [t->rxbpi++] = '\3';
	if (old) {
            sprintf (buf, "Type HYC<enter> (no echo is normal)\r\n");
            tmxr_linemsg (t, buf);
	}
    }

    /*
     * It the operator console is remote, we still need to probe the local keyboard
     * for a WRU, say, 10 times a second.
     */
    if (!attached_console) {
        static int divider;
        if (++divider == CLK_TPS/10) {
            divider = 0;
            sim_poll_kbd();
        }
    }

    /* Polling sockets for transmission. */
    tmxr_poll_tx (&tty_desc);
    /* If the TTY system is not idle, schedule the next interrupt
     * by instruction count using the target interrupt rate of 300 Hz;
     * otherwise we can wait for a roughly equivalent wallclock time period,
     * e.g. until the next 250 Hz wallclock interrupt, but making sure 
     * that the model time interval between GRP_SERIAL interrupts 
     * is never less than expected.
     */
    if (vt_is_idle()) {
        /* When idle, slow down the TTY interrupts to match the clock interrupts. */
        return sim_clock_coschedule (this, 0);
    } else if (tty_turbo) {
        /* In "turbo" mode, the TTY works at the model speed */
        return sim_activate(this, 1000*MSEC/tty_rate);
    } else {
        /* In "non-turbo" mode, the TTY interrupts imitate the true feel of the speed */
        return sim_activate_after(this, 1000*MSEC/tty_rate);
    }
}

t_stat tty_setmode (UNIT *u, int32 val, CONST char *cptr, void *desc)
{
    int num = u - tty_unit;
    TMLN *t = &tty_line [num];
    uint32 mask = num <= TTY_MAX ? 1 << (TTY_MAX - num) : 0;

    if ((val & TTY_STATE_MASK) != TTY_OFFLINE_STATE) {
        t_stat r = check_usable (num);
        if (r != SCPE_OK)
            return r;
    }
    switch (val & TTY_STATE_MASK) {
    case TTY_OFFLINE_STATE:
        if (t->conn) {
            if (t->rcve) {
                tmxr_reset_ln (t);
                t->rcve = 0;
            } else
                t->conn = 0;
            if (num <= TTY_MAX) {
                tty_sym[num] =
                    tty_active[num] =
                    tty_typed[num] =
                    tty_instate[num] = 0;
                vt_mask &= ~mask;
                tt_mask &= ~mask;
            }
        }
        break;
    case TTY_TELETYPE_STATE:
        if (num > TT_LINES || !TTY_SERIAL (num))
            return SCPE_NXPAR;
        t->conn = 1;
        t->rcve = 0;
        tt_mask |= mask;
        vt_mask &= ~mask;
        break;
    case TTY_VT340_STATE:
        t->conn = 1;
        t->rcve = 0;
        if (TTY_SERIAL (num)) {
            vt_mask |= mask;
            tt_mask &= ~mask;
        }
        break;
    case TTY_CONSUL_STATE:
        if (line_kind (num) != LINE_CONSUL)
            return SCPE_NXPAR;
        t->conn = 1;
        t->rcve = 0;
        break;
    }
    return SCPE_OK;
}

/*
 * Allowing telnet connections is done with
 *      attach tty <port>
 * Where <port> is the port number for telnet, e.g. 4199.
 */
t_stat tty_attach (UNIT *u, CONST char *cptr)
{
    int num = u - tty_unit;
    char gbuf[CBUFSIZE];
    char conn_save [LINES_MAX+1];
    int r, n;

    /* All arguments but the magic words "console" and "none" are passed
     * to tmxr_attach().
     */
    get_glyph (cptr, gbuf, 0);
    if (strcmp (gbuf, "NONE") == 0 || strcmp (gbuf, "CONSOLE") == 0) {
        r = check_usable (num);
        if (r != SCPE_OK)
            return r;
    }
    if (line_kind (num) == LINE_CONSUL && (tty_unit[num].flags & TTY_INVERSE_READY))
        READY2 &= ~CONS_READY[num-CONSUL_FIRST];
    /* Disallowing future connections to a line */
    if (strcmp (gbuf, "NONE") == 0) {
        /* Marking the TTY as unusable. */
        tty_line[num].conn = 1;
        tty_line[num].rcve = 0;
        if (num <= TTY_MAX) {
            vt_mask &= ~(1 << (TTY_MAX - num));
            tt_mask &= ~(1 << (TTY_MAX - num));
        }
        besm6_debug ("*** turning off T%03o", num);
        return SCPE_OK;
    }
    if (strcmp (gbuf, "CONSOLE")) {
        /* Saving and restoring all .conn,
         * because tmxr_attach() zeroes them. */
        for (n=1; n<=LINES_MAX; ++n)
            conn_save[n] = tty_line[n].conn != 0;
        /* The unit number is ignored for the port assignment */
        r = tmxr_attach (&tty_desc, &tty_unit[0], cptr);
        for (n=1; n<=LINES_MAX; ++n)
            if (conn_save[n])
                tty_line[n].conn = 1;
        return r;
    } else {
        /* Attaching SIMH console to a particular terminal. */
        if ((u->flags & TTY_STATE_MASK) != TTY_CONSUL_STATE) {
            u->flags &= ~TTY_STATE_MASK;
            u->flags |= TTY_VT340_STATE;
        }
        tty_line[num].conn = 1;
        tty_line[num].rcve = 0;
        if (TTY_SERIAL (num))
            vt_mask |= 1 << (TTY_MAX - num);
        besm6_debug ("*** console on T%03o", num);
        attached_console = 1;
        return SCPE_OK;
    }
    return SCPE_ALATT;
}

t_stat tty_detach (UNIT *u)
{
    return tmxr_detach (&tty_desc, &tty_unit[0]);
}

t_stat tty_showrate (FILE *f, UNIT *up, int32 v, CONST void *dp) {
    fprintf(f, "%d Baud", tty_rate);
    return SCPE_OK;
}

t_stat tty_showturbo (FILE *f, UNIT *up, int32 v, CONST void *dp) {
    fprintf(f, tty_turbo ? "Turbo" : "Authentic feel");
    return SCPE_OK;
}

t_stat tty_setrate (UNIT *up, int32 v, CONST char *cp, void *dp) {
    int rate;
    if (cp)
        rate = atoi(cp);
    else
        return SCPE_MISVAL;
    if (rate <= 0 || rate > 19200 || rate % 300 != 0)
        return SCPE_ARG;
    rate /= 300;
    if ((rate-1) & rate)
        return SCPE_ARG;
    tty_rate = rate * 300;
    return SCPE_OK;
}

t_stat tty_setturbo (UNIT *up, int32 v, CONST char *cp, void *dp) {
    if (!cp)
        return SCPE_MISVAL;
    if (!MATCH_CMD("ON", cp))
        tty_turbo = 1;
    else if (!MATCH_CMD("OFF", cp))
        tty_turbo = 0;
    else
        return SCPE_ARG;
    return SCPE_OK;
}

/*
 * TTY control:
 * set ttyN unicode     - selecting UTF-8 encoding
 * set ttyN jcuken      - selecting KOI-7 encoding, JCUKEN layout
 * set ttyN qwerty      - selecting KOI-7 encoding, QWERTY layout
 * set ttyN raw         - selecting transmission of raw chars
 * set ttyN raw8        - the same, but eight bits wide and with no parity
 * set ttyN off         - disconnecting a line
 * set ttyN tt          - a Baudot TTY
 * set ttyN vt          - a Videoton-340 terminal
 * set ttyN consul      - a "Consul-254" typewriter
 * set ttyN destrbs     - destructive (erasing) backspace
 * set ttyN authbs      - authentic backspace (cursor left)
 * set tty dks          - the multiplexer is the ДКС (Электроника-60); enables device DKS
 * set tty tmgu         - the multiplexer is the ТМГУ/МАМС concentrator on Consul 0
 * set tty tmgutrace    - the same, and log concentrator bytes
 * set tty mux          - the multiplexer is the АС ("аппаратура сопряжения")
 *                        At most one of these per session.
 * set ttyN mux         - line N (1-24) is on the multiplexer (synonym: dks)
 * set ttyN direct      - line N (1-24) is a bit-serial line (synonym: nodks)
 *                        Lines 27-64 (033-0100) are always on the multiplexer;
 *                        25-26 (031, 032) are always the Consuls, and Consul 0 is
 *                        unusable under ТМГУ and ДКС.  Telnet never connects to
 *                        a Consul: use "attach ttyN console" for those.
 * set tty lineorder=L  - telnet connection order, e.g. 13;1-10 (no Consuls)
 * set tty disconnect=N - forceful termination of a telnet connection
 * set tty rate=N       - I/O rate in Hz
 * set tty turbo={ON,OFF} - TTY interrupts use model time (on) or wallclock (0ff)
 * attach ttyN console  - the SIMH console is line N
 * show tty             - showing modes and types, the multiplexer and the use
 *                        of every line
 * show tty connections - showing IP-addresses and connection times
 * show tty statistics  - showing TX/RX byte counts
 */

/* Choosing the multiplexer of the session; a different second choice is an error. */
static t_stat select_mux (int kind)
{
    if (mux_kind == kind)
        return SCPE_OK;
    if (mux_kind != MUX_NONE)
        return sim_messagef (SCPE_ARG, "Мультиплексор уже выбран: %s\n", mux_name[mux_kind]);
    if (kind != MUX_DKS && !(dks_dev.flags & DEV_DIS))
        return sim_messagef (SCPE_ARG, "Мультиплексор уже выбран: ДКС (set dks enabled)\n");
    if (kind == MUX_TMGU || kind == MUX_DKS) {
        if (tty_line[CONSUL_FIRST].conn)
            return sim_messagef (SCPE_ARG, "tty%d (Консул 0, 031) подключён; %s его "
                                 "занимает\n", CONSUL_FIRST, mux_name[kind]);
    }
    mux_kind = kind;
    return SCPE_OK;
}

/* For besm6_dks.c: "set dks enabled" chooses the ДКС. */
t_stat tty_select_dks (void)
{
    return select_mux (MUX_DKS);
}

/* For besm6_dks.c: "set dks disabled" leaves the session without a multiplexer. */
void tty_deselect_dks (void)
{
    if (mux_kind == MUX_DKS)
        mux_kind = MUX_NONE;
}

/*
 * "set tty mux|dks" chooses the multiplexer, "set ttyN mux|dks" puts line N on it.
 * One MTAB entry serves both levels, since set_cmd() stops at the first entry of
 * that name; the device level arrives as unit 0.
 */
static t_stat tty_set_mux (UNIT *u, int32 val, CONST char *cptr, void *desc)
{
    int num = u - tty_unit;
    uint32 mask;
    t_stat r;

    if (cptr && *cptr)
        return SCPE_ARG;
    if (num == 0) {
        r = select_mux (val);
        if (r == SCPE_OK && val == MUX_DKS)
            dks_dev.flags &= ~DEV_DIS;
        return r;
    }
    if (num > TTY_MAX)
        return line_is_mux (num) ? SCPE_OK : check_usable (num);
    if (mux_kind == MUX_NONE)
        return sim_messagef (SCPE_ARG, "Мультиплексор не выбран: set tty dks|tmgu|mux\n");
    if (mux_kind == MUX_DKS && num > DKS_MAX_LINE)
        return check_usable (num);
    mask = 1 << (TTY_MAX - num);
    u->flags |= TTY_MUX_MODE;
    vt_mask &= ~mask;
    tt_mask &= ~mask;
    if ((u->flags & TTY_STATE_MASK) == TTY_TELETYPE_STATE) {
        u->flags &= ~TTY_STATE_MASK;
        u->flags |= TTY_VT340_STATE;
    }
    return SCPE_OK;
}

static t_stat tty_set_direct (UNIT *u, int32 val, CONST char *cptr, void *desc)
{
    int num = u - tty_unit;
    uint32 mask;

    if (num < 1 || num > TTY_MAX)
        return sim_messagef (SCPE_ARG, "Прямые линии - tty1..tty%d\n", TTY_MAX);
    mask = 1 << (TTY_MAX - num);
    if (u->flags & TTY_MUX_MODE) {
        u->flags &= ~TTY_MUX_MODE;
        if (tty_line[num].conn) {
            if ((u->flags & TTY_STATE_MASK) == TTY_TELETYPE_STATE)
                tt_mask |= mask;
            else
                vt_mask |= mask;
        }
    }
    return SCPE_OK;
}

static t_stat tty_set_tmgu (UNIT *u, int32 val, CONST char *cptr, void *desc)
{
    t_stat r;

    if (cptr && *cptr)
        return SCPE_ARG;
    r = select_mux (MUX_TMGU);
    if (r != SCPE_OK)
        return r;
    if (val == 2)
        tmgu_trace = 1;
    tmgu_reset_link ();
    cons_input_pending[0] = 0;
    READY2 |= TMGU_OUT_RDY | TMGU_IN_RDY;
    tmgu_offer_input ();
    return SCPE_OK;
}

/* "set tty lineorder": the TMXR routine, then no Consuls and no unusable lines. */
static t_stat tty_set_lnorder (UNIT *u, int32 val, CONST char *cptr, void *desc)
{
    int32 save [LINES_MAX+1];
    t_stat r;
    int i;

    memcpy (save, tty_lnorder, sizeof (save));
    r = tmxr_set_lnorder (u, val, cptr, desc);
    if (r != SCPE_OK)
        return r;
    for (i = 0; i <= LINES_MAX && tty_lnorder[i] >= 0; ++i) {
        int n = tty_lnorder[i], k = line_kind (n);
        if (n == 0)
            continue;
        if (k == LINE_CONSUL || k == LINE_UNUSED) {
            memcpy (tty_lnorder, save, sizeof (save));
            if (k == LINE_CONSUL)
                return sim_messagef (SCPE_ARG, "tty%d - Консул: telnet к нему не "
                                     "подключается\n", n);
            return check_usable (n);
        }
    }
    return SCPE_OK;
}

static t_stat tty_show_line (FILE *st, UNIT *u, int32 v, CONST void *desc)
{
    static const char *const kname[] = { "direct", "Consul", NULL, "unused" };
    int k = line_kind ((int) (u - tty_unit));

    if (k == LINE_MUX)
        fprintf (st, "on %s", mux_name[mux_kind]);
    else
        fprintf (st, "%s", kname[k]);
    return SCPE_OK;
}

static t_stat tty_show_config (FILE *st, UNIT *u, int32 v, CONST void *desc)
{
    static const char *const kname[] = { "direct", "Consul", "mux", "unused" };
    int n, k, first = 1, from;

    fprintf (st, "multiplexer: %s%s", mux_name[mux_kind],
             tmgu_on && tmgu_trace ? " (trace)" : "");
    /* Runs of lines of one kind, in octal as the OS numbers them. */
    for (from = 1; from <= LINES_MAX; from = n) {
        k = line_kind (from);
        for (n = from + 1; n <= LINES_MAX && line_kind (n) == k; ++n)
            ;
        fprintf (st, first ? "\n  " : ", ");
        first = 0;
        if (n - 1 == from)
            fprintf (st, "%03o %s", from, kname[k]);
        else
            fprintf (st, "%03o-%03o %s", from, n - 1, kname[k]);
    }
    return SCPE_OK;
}

MTAB tty_mod[] = {
    { TTY_CHARSET_MASK, TTY_UNICODE_CHARSET, "UTF-8 input",
      "UNICODE", &tty_check_unit },
    { TTY_CHARSET_MASK, TTY_KOI7_JCUKEN_CHARSET, "KOI7 (jcuken) input",
      "JCUKEN", &tty_check_unit },
    { TTY_CHARSET_MASK, TTY_KOI7_QWERTY_CHARSET, "KOI7 (qwerty) input",
      "QWERTY", &tty_check_unit },
    /* RAW must precede RAW8: MATCH_CMD() matches a typed prefix against the entry,
     * so with RAW8 first a "set ttyN raw" would silently select RAW8. */
    { TTY_CHARSET_MASK, TTY_RAW_CHARSET, "RAW input/output",
      "RAW", &tty_check_unit },
    { TTY_CHARSET_MASK, TTY_RAW8_CHARSET, "RAW 8-bit input/output",
      "RAW8", &tty_check_unit },
    { TTY_STATE_MASK, TTY_OFFLINE_STATE, "offline",
      "OFF", &tty_setmode },
    { TTY_STATE_MASK, TTY_TELETYPE_STATE, "Teletype",
      "TT", &tty_setmode },
    { TTY_STATE_MASK, TTY_VT340_STATE, "Videoton-340",
      "VT", &tty_setmode },
    { TTY_STATE_MASK, TTY_CONSUL_STATE, "Consul-254",
      "CONSUL", &tty_setmode },
    { TTY_BSPACE_MASK, TTY_DESTRUCTIVE_BSPACE, "destructive backspace",
      "DESTRBS", &tty_check_unit },
    { TTY_BSPACE_MASK, TTY_AUTHENTIC_BSPACE, NULL,
      "AUTHBS", &tty_check_unit },
    { TTY_INVERSE_READY, TTY_INVERSE_READY, "inverse ready",
      "INVREADY", &tty_check_unit },
    { TTY_INVERSE_READY, 0, "regular ready",
      "REGREADY", &tty_check_unit },
    { MTAB_XTD | MTAB_VUN, 0, "LINE",
      NULL, NULL, &tty_show_line, NULL, "direct / Consul / multiplexer line" },
    { MTAB_XTD | MTAB_VDV | MTAB_VUN, MUX_AS, NULL,
      "MUX", &tty_set_mux, NULL, NULL,
      "tty: the multiplexer is the АС; ttyN: line N is on the multiplexer" },
    { MTAB_XTD | MTAB_VDV | MTAB_VUN, MUX_DKS, NULL,
      "DKS", &tty_set_mux, NULL, NULL,
      "tty: the multiplexer is the ДКС; ttyN: line N is on the multiplexer" },
    { MTAB_XTD | MTAB_VUN, 0, NULL,
      "DIRECT", &tty_set_direct, NULL, NULL, "line N (1-24) is a bit-serial line" },
    { MTAB_XTD | MTAB_VUN, 0, NULL,
      "NODKS", &tty_set_direct, NULL, NULL, "same as DIRECT" },
    { MTAB_XTD | MTAB_VDV, 1, NULL,
      "TMGU", &tty_set_tmgu, NULL, NULL,
      "the multiplexer is the ТМГУ/МАМС concentrator on Consul 0 (ТЕРМАС)" },
    { MTAB_XTD | MTAB_VDV, 2, NULL,
      "TMGUTRACE", &tty_set_tmgu, NULL, NULL,
      "as TMGU, and log concentrator bytes" },
    /* Printed by "show tty": "show tty <param>" fails on the disabled unit 0. */
    { MTAB_XTD | MTAB_VDV, 0, "CONFIG",
      NULL, NULL, &tty_show_config, NULL, "the multiplexer and the use of every line" },
    { MTAB_XTD | MTAB_VDV | MTAB_VALR, 1, NULL,
      "DISCONNECT", &tmxr_dscln, NULL, (void*) &tty_desc, "terminates telnet connection" },
    { MTAB_XTD | MTAB_VDV | MTAB_VALR, 1, "RATE",
      "RATE", &tty_setrate, &tty_showrate, NULL, "{300,600,1200,2400,4800,9600,19200}" },
    { MTAB_XTD | MTAB_VDV | MTAB_VALR, 1, "TURBO",
      "TURBO", &tty_setturbo, &tty_showturbo, NULL, "{ON, OFF}"},
    { MTAB_XTD | MTAB_VDV, 0, "SUMMARY",
      NULL, NULL, &tmxr_show_summ, (void*) &tty_desc },
    { MTAB_XTD | MTAB_VDV | MTAB_NMO, 1, "CONNECTIONS",
      NULL, NULL, &tmxr_show_cstat, (void*) &tty_desc },
    { MTAB_XTD | MTAB_VDV | MTAB_NMO, 0, "STATISTICS",
      NULL, NULL, &tmxr_show_cstat, (void*) &tty_desc },
    { MTAB_XTD | MTAB_VDV | MTAB_NMO | MTAB_VALR,   0,   "LINEORDER",
      "LINEORDER",   &tty_set_lnorder, &tmxr_show_lnorder, (void *) &tty_desc,
      "Line connection order, e.g. 26;20-24;1-10" },
    { MTAB_XTD | MTAB_VUN | MTAB_NC, 0, NULL,
      "LOG", &tmxr_set_log, &tmxr_show_log, (void*) &tty_desc },
    { MTAB_XTD | MTAB_VUN | MTAB_NC, 0, NULL,
      "NOLOG", &tmxr_set_nolog, NULL, (void*) &tty_desc },
    { 0 }
};

DEVICE tty_dev = {
    "TTY", tty_unit, tty_reg, tty_mod,
    LINES_MAX+1, 2, 1, 1, 2, 1,
    NULL, NULL, &tty_reset, NULL, &tty_attach, &tty_detach,
    NULL, DEV_MUX|DEV_DEBUG
};

void tty_send (uint32 mask)
{
    if (mask && tty_dev.dctrl)
        besm6_debug ("*** TTY: transmit %08o", mask);

    TTY_OUT = mask;
}

/*
 * Sending a character to a terminal with the given number.
 */
void vt_putc (int num, int c)
{
    TMLN *t = &tty_line [num];

    if (! t->conn)
        return;
    if (t->rcve) {
        /* A telnet connection. */
        tmxr_putc_ln (t, c);
    } else {
        /* Console output. */
        sim_putchar(c);
    }
}

/*
 * Sending a string to a terminal with the given number.
 */
void vt_puts (int num, const char *s)
{
    TMLN *t = &tty_line [num];

    if (! t->conn)
        return;
    if (t->rcve) {
        /* A telnet connection. */
        tmxr_linemsg (t, s);
    } else {
        /* Console output. */
        while (*s) sim_putchar(*s++);
    }
}

const char * koi7_rus_to_unicode [32] = {
    "Ю", "А", "Б", "Ц", "Д", "Е", "Ф", "Г",
    "Х", "И", "Й", "К", "Л", "М", "Н", "О",
    "П", "Я", "Р", "С", "Т", "У", "Ж", "В",
    "Ь", "Ы", "З", "Ш", "Э", "Щ", "Ч", "\0x7f",
};

/* Videoton-340 employed single byte control codes rather than ESC sequences. */
void vt_send(int num, uint32 sym)
{
    if (tty_raw (num)) {
        vt_putc(num, sym);
    } else if (sym < 0x60) {
        switch (sym) {
        case '\031':
            /* Up */
            vt_puts (num, "\033[");
            sym = 'A';
            break;
        case '\032':
            /* Down */
            vt_puts (num, "\033[");
            sym = 'B';
            break;
        case '\030':
            /* Right */
            vt_puts (num, "\033[");
            sym = 'C';
            break;
        case '\b':
            /* Left */
            vt_puts (num, "\033[");
            if ((tty_unit[num].flags & TTY_BSPACE_MASK) == TTY_DESTRUCTIVE_BSPACE) {
                /* Erasing the previous char. */
                vt_puts (num, "D \033[");
            }
            sym = 'D';
            break;
        case '\v':
        case '\033':
        case '\0':
            /* Sending the actual char */
            break;
        case '\037':
            /* Clear screen */
            vt_puts (num, "\033[H\033[");
            sym = 'J';
            break;
        case '\n':
            /* Also does carriage return */
            vt_putc (num, '\r');
            sym = '\n';
            break;
        case '\f':
            /* Home */
            vt_puts(num, "\033[");
            sym = 'H';
            break;
        case '\r':
        case '\003':
            /* Not displayed */
            sym = 0;
            break;
        default:
            if (sym < ' ') {
                /* Other control chars were displayed as dimmed. */
                vt_puts (num, "\033[2m");
                vt_putc (num, sym | 0x40);
                vt_puts (num, "\033[");
                /* Terminating the ESC sequence */
                sym = 'm';
            }
        }
        if (sym)
            vt_putc (num, sym);
    } else
        vt_puts (num, koi7_rus_to_unicode[sym - 0x60]);
}

/*
 * Handling output to all connected terminals.
 */
void vt_print()
{
    uint32 workset = (TTY_OUT & vt_mask) | vt_sending;
    int num;

    if (workset == 0) {
        ++vt_idle;
        return;
    }
    for (num = besm6_highest_bit (workset) - TTY_MAX;
         workset; num = besm6_highest_bit (workset) - TTY_MAX) {
        int mask = 1 << (TTY_MAX - num);
        int c = (TTY_OUT & mask) != 0;
        switch (tty_active[num]*2+c) {
        case 0: /* idle */
            besm6_debug ("Warning: inactive ttys should have been screened");
            continue;
        case 1: /* start bit */
            vt_sending |= mask;
            tty_active[num] = 1;
            break;
        case 18: /* stop bit */
            tty_sym[num] = ~tty_sym[num] & 0x7f;
            vt_send (num, tty_sym[num]);
            tty_active[num] = 0;
            tty_sym[num] = 0;
            vt_sending &= ~mask;
            break;
        case 19: /* framing error */
            vt_putc (num, '#');
            break;
        default:
            /* little endian ordering */
            if (c) {
                tty_sym[num] |= 1 << (tty_active[num]-1);
            }
            ++tty_active[num];
            break;
        }
        workset &= ~mask;
    }
    vt_idle = 0;
}


/*
 * Output to Baudot teletypes.
 */
void tt_print()
{
    uint32 workset = (TTY_OUT & tt_mask) | tt_sending;
    int num;

    if (workset == 0) {
        return;
    }
    if (tty_dev.dctrl)
        besm6_debug("<<< TTY_OUT: %08o", TTY_OUT);

    for (num = besm6_highest_bit (workset) - TTY_MAX;
         workset; num = besm6_highest_bit (workset) - TTY_MAX) {
        int mask = 1 << (TTY_MAX - num);
        int c = (TTY_OUT & mask) != 0;
        switch (tty_active[num]*2+c) {
        case 0: /* idle */
            break;
        case 1: /* start bit */
            tt_sending |= mask;
            tty_active[num] = 1;
            break;
        case 12: /* stop bit */
            vt_puts (num, process (num, tty_sym[num]));
            tty_active[num] = 0;
            tty_sym[num] = 0;
            tt_sending &= ~mask;
            break;
        case 13: /* framing error */
            vt_putc (num, '#');
            break;
        default:
            /* big endian ordering */
            if (c) {
                tty_sym[num] |= 1 << (5-tty_active[num]);
            }
            ++tty_active[num];
            break;
        }
        workset &= ~mask;
    }
    vt_idle = 0;
}

/*
 * Converting from Unicode to KOI-7.
 * Returns -1 if unsuccessful.
 */
static int unicode_to_koi7 (unsigned val)
{
    if (val <= '_') return val;
    else if ('a' <= val && val <= 'z') return val + 'Z' - 'z';
    else switch (val) {
        case 0x007f:              return 0x7f;
        case 0x0410: case 0x0430: return 0x61;
        case 0x0411: case 0x0431: return 0x62;
        case 0x0412: case 0x0432: return 0x77;
        case 0x0413: case 0x0433: return 0x67;
        case 0x0414: case 0x0434: return 0x64;
        case 0x0415: case 0x0435: return 0x65;
        case 0x0416: case 0x0436: return 0x76;
        case 0x0417: case 0x0437: return 0x7a;
        case 0x0418: case 0x0438: return 0x69;
        case 0x0419: case 0x0439: return 0x6a;
        case 0x041a: case 0x043a: return 0x6b;
        case 0x041b: case 0x043b: return 0x6c;
        case 0x041c: case 0x043c: return 0x6d;
        case 0x041d: case 0x043d: return 0x6e;
        case 0x041e: case 0x043e: return 0x6f;
        case 0x041f: case 0x043f: return 0x70;
        case 0x0420: case 0x0440: return 0x72;
        case 0x0421: case 0x0441: return 0x73;
        case 0x0422: case 0x0442: return 0x74;
        case 0x0423: case 0x0443: return 0x75;
        case 0x0424: case 0x0444: return 0x66;
        case 0x0425: case 0x0445: return 0x68;
        case 0x0426: case 0x0446: return 0x63;
        case 0x0427: case 0x0447: return 0x7e;
        case 0x0428: case 0x0448: return 0x7b;
        case 0x0429: case 0x0449: return 0x7d;
        case 0x042b: case 0x044b: return 0x79;
        case 0x042c: case 0x044c: return 0x78;
        case 0x042d: case 0x044d: return 0x7c;
        case 0x042e: case 0x044e: return 0x60;
        case 0x042f: case 0x044f: return 0x71;
        }
    return -1;
}

/*
 * Set command
 */
static t_stat cmd_set (int32 num, CONST char *cptr)
{
    char gbuf [CBUFSIZE];
    int len;

    cptr = (CONST char *)get_sim_sw (cptr);
    if (! cptr)
        return SCPE_INVSW;
    if (! *cptr)
        return SCPE_NOPARAM;
    cptr = get_glyph (cptr, gbuf, 0);
    if (*cptr)
        return SCPE_2MARG;

    len = strlen (gbuf);
    if (strncmp ("UNICODE", gbuf, len) == 0) {
        tty_unit[num].flags &= ~TTY_CHARSET_MASK;
        tty_unit[num].flags |= TTY_UNICODE_CHARSET;
    } else if (strncmp ("JCUKEN", gbuf, len) == 0) {
        tty_unit[num].flags &= ~TTY_CHARSET_MASK;
        tty_unit[num].flags |= TTY_KOI7_JCUKEN_CHARSET;
    } else if (strncmp ("QWERTY", gbuf, len) == 0) {
        tty_unit[num].flags &= ~TTY_CHARSET_MASK;
        tty_unit[num].flags |= TTY_KOI7_QWERTY_CHARSET;
    } else if (strncmp ("RAW", gbuf, len) == 0) {
        tty_unit[num].flags &= ~TTY_CHARSET_MASK;
        tty_unit[num].flags |= TTY_RAW_CHARSET;
    } else if (strncmp ("RAW8", gbuf, len) == 0) {
        tty_unit[num].flags &= ~TTY_CHARSET_MASK;
        tty_unit[num].flags |= TTY_RAW8_CHARSET;
    } else if (strncmp ("TT", gbuf, len) == 0) {
        if (num > TT_LINES || !TTY_SERIAL (num))
            return SCPE_NXPAR;
        tty_unit[num].flags &= ~TTY_STATE_MASK;
        tty_unit[num].flags |= TTY_TELETYPE_STATE;
        tt_mask |= 1 << (TTY_MAX - num);
        vt_mask &= ~(1 << (TTY_MAX - num));
    } else if (strncmp ("VT", gbuf, len) == 0) {
        tty_unit[num].flags &= ~TTY_STATE_MASK;
        tty_unit[num].flags |= TTY_VT340_STATE;
        if (TTY_SERIAL (num)) {
            vt_mask |= 1 << (TTY_MAX - num);
            tt_mask &= ~(1 << (TTY_MAX - num));
        }
    } else if (strncmp ("CONSUL", gbuf, len) == 0) {
        tty_unit[num].flags &= ~TTY_STATE_MASK;
        tty_unit[num].flags |= TTY_CONSUL_STATE;
    } else if (strncmp ("DESTRBS", gbuf, len) == 0) {
        tty_unit[num].flags &= ~TTY_BSPACE_MASK;
        tty_unit[num].flags |= TTY_DESTRUCTIVE_BSPACE;
    } else if (strncmp ("AUTHBS", gbuf, len) == 0) {
        tty_unit[num].flags &= ~TTY_BSPACE_MASK;
        tty_unit[num].flags |= TTY_AUTHENTIC_BSPACE;
    } else {
        return SCPE_NXPAR;
    }
    return SCPE_OK;
}

/*
 * Show command
 */
static t_stat cmd_show (int32 num, CONST char *cptr)
{
    TMLN *t = &tty_line [num];
    char gbuf [CBUFSIZE];
    MTAB *m;
    int len;

    cptr = (CONST char *)get_sim_sw (cptr);
    if (! cptr)
        return SCPE_INVSW;
    if (! *cptr) {
        sprintf (gbuf, "TTY%d", num);
        tmxr_linemsg (t, gbuf);
        for (m=tty_mod; m->mask; m++) {
            if (m->pstring &&
                (tty_unit[num].flags & m->mask) == m->match) {
                tmxr_linemsg (t, ", ");
                tmxr_linemsg (t, m->pstring);
            }
        }
        if (t->txlog)
            tmxr_linemsg (t, ", log");
        tmxr_linemsg (t, "\r\n");
        return SCPE_OK;
    }
    cptr = get_glyph (cptr, gbuf, 0);
    if (*cptr)
        return SCPE_2MARG;

    len = strlen (gbuf);
    if (strncmp ("STATISTICS", gbuf, len) == 0) {
        sprintf (gbuf, "line %d: input queued/total = %d/%d, "
                 "output queued/total = %d/%d\r\n", num,
                 t->rxbpi - t->rxbpr, t->rxcnt,
                 t->txbpi - t->txbpr, t->txcnt);
        tmxr_linemsg (t, gbuf);
    } else {
        return SCPE_NXPAR;
    }
    return SCPE_OK;
}

/*
 * Exit command
 */
static t_stat cmd_exit (int32 num, CONST char *cptr)
{
    return SCPE_EXIT;
}

static t_stat cmd_help (int32 num, CONST char *cptr);

static CTAB cmd_table[] = {
    { "SET", &cmd_set, 0,
      "set unicode              select UTF-8 encoding\r\n"
      "set jcuken               select KOI7 encoding, 'jcuken' keymap\r\n"
      "set qwerty               select KOI7 encoding, 'qwerty' keymap\r\n"
      "set raw                  select no I/O conversions\r\n"
      "set raw8                 the same, eight bits wide and with no parity\r\n"
      "set tt                   use Teletype mode\r\n"
      "set vt                   use Videoton-340 mode\r\n"
      "set consul               use Consul-254 mode\r\n"
      "set destrbs              destructive backspace\r\n"
      "set authbs               authentic backspace\r\n"
    },
    { "SHOW", &cmd_show, 0,
      "sh{ow}                   show modes of the terminal\r\n"
      "sh{ow} s{tatistics}      show network statistics\r\n"
    },
    { "EXIT", &cmd_exit, 0,
      "exi{t} | q{uit} | by{e}  exit from simulation\r\n"
    },
    { "QUIT", &cmd_exit, 0, NULL
    },
    { "BYE", &cmd_exit, 0, NULL
    },
    { "HELP", &cmd_help, 0,
      "h{elp}                   type this message\r\n"
      "h{elp} <command>         type help for command\r\n"
    },
    { 0 }
};

/*
 * Find command routine
 */
static CTAB *lookup_cmd (char *command)
{
    CTAB *c;
    int len;

    len = strlen (command);
    for (c=cmd_table; c->name; c++) {
        if (strncmp (command, c->name, len) == 0)
            return c;
    }
    return 0;
}

/*
 * Help command
 */
static t_stat cmd_help (int32 num, CONST char *cptr)
{
    TMLN *t = &tty_line [num];
    char gbuf [CBUFSIZE];
    CTAB *c;

    cptr = (CONST char *)get_sim_sw (cptr);
    if (! cptr)
        return SCPE_INVSW;
    if (! *cptr) {
        /* Listing all commands. */
        tmxr_linemsg (t, "Commands may be abbreviated.  Commands are:\r\n\r\n");
        for (c=cmd_table; c && c->name; c++)
            if (c->help)
                tmxr_linemsg (t, c->help);
        return SCPE_OK;
    }
    cptr = get_glyph (cptr, gbuf, 0);
    if (*cptr)
        return SCPE_2MARG;
    c = lookup_cmd (gbuf);
    if (! c)
        return SCPE_ARG;
    /* Describing a command. */
    tmxr_linemsg (t, c->help);
    return SCPE_OK;
}

/*
 * Executing a command.
 */
void vt_cmd_exec (int num)
{
    TMLN *t = &tty_line [num];
    char gbuf [CBUFSIZE];
    CONST char *cptr;
    CTAB *cmdp;
    t_stat err;

    cptr = get_glyph (vt_cbuf [num], gbuf, 0);      /* get command glyph */
    cmdp = lookup_cmd (gbuf);                       /* lookup command */
    if (! cmdp) {
        tmxr_linemsg (t, sim_error_text (SCPE_UNK));
        tmxr_linemsg (t, "\r\n");
        return;
    }
    err = cmdp->action (num, cptr);                 /* if found, exec */
    if (err >= SCPE_BASE) {                         /* error? */
        tmxr_linemsg (t, sim_error_text (err));
        tmxr_linemsg (t, "\r\n");
    }
    if (err == SCPE_EXIT) {                         /* close telnet session */
        tmxr_reset_ln (t);
    }
}

/*
 * Command line interface mode.
 */
void vt_cmd_loop (int num, int c)
{
    TMLN *t = &tty_line [num];
    char *cbuf, **cptr;

    cbuf = vt_cbuf [num];
    cptr = &vt_cptr [num];

    switch (c) {
    case '\r':
    case '\n':
        tmxr_linemsg (t, "\r\n");
        if (*cptr <= cbuf) {
            /* An empty line - returning to terminal emulation. */
            tty_unit[num].flags &= ~TTY_CMDLINE_MASK;
            break;
        }
        /* Executing. */
        **cptr = 0;
        vt_cmd_exec (num);
        tmxr_linemsg (t, "sim>");
        *cptr = vt_cbuf[num];
        break;
    case '\b':
    case 0177:
        /* Backspace. */
        if (*cptr <= cbuf)
            break;
        tmxr_linemsg (t, "\b \b");
        while (*cptr > cbuf) {
            --*cptr;
            if (! (**cptr & 0x80))
                break;
        }
        break;
    case 'U' & 037:
        /* Erase line. */
        erase_line:     while (*cptr > cbuf) {
            --*cptr;
            if (! (**cptr & 0x80))
                tmxr_linemsg (t, "\b \b");
        }
        break;
    case 033:
        /* Escape [ X. */
        if (tmxr_getc_ln (t) != '[' + TMXR_VALID)
            break;
        switch (tmxr_getc_ln (t) - TMXR_VALID) {
        case 'A': /* Up arrow */
            if (*cptr <= cbuf) {
                *cptr = cbuf + strlen (cbuf);
                if (*cptr > cbuf)
                    tmxr_linemsg (t, cbuf);
            }
            break;
        case 'B': /* Down arrow */
            goto erase_line;
        }
        break;
    default:
        if (c < ' ' || *cptr > cbuf+CBUFSIZE-5)
            break;
        *(*cptr)++ = c;
        tmxr_putc_ln (t, c);
        break;
    }
}

/*
 * Getting a char from a terminal with the given number.
 * Returns -1 if there is no char to input.
 */
int vt_getc (int num)
{
    TMLN *t = &tty_line [num];
    extern int32 sim_int_char;
    int c;
#ifdef REMOTE_TIMEOUT
    time_t now;
#endif
    if (! t->conn) {
        /* Пользователь отключился. */
        if (t->ipad) {
            besm6_debug ("*** tty%d: disconnecting %s",
                         num, 
                         t->ipad);
            t->ipad = NULL;
        }
        tty_setmode (tty_unit+num, TTY_OFFLINE_STATE, 0, 0);
        tty_unit[num].flags &= ~TTY_STATE_MASK;
        return -128;
    }
    if (t->rcve) {
        /* A telnet line. */
        c = tmxr_getc_ln (t);
        /* TWO TAGS, NOT ONE.  tmxr_getc_ln() marks a character that came off the
         * socket with TMXR_VALID, but one injected by the SCP `send TTY:n,"..."'
         * command with SCPE_KFLAG (sim_send_poll_data(), scp.c) -- and it consumes
         * the injected character before returning it.  A mux that tests only
         * TMXR_VALID therefore makes `send' a silent no-op on its lines: the byte
         * is dequeued and dropped here.  Every other SIMH mux accepts both
         * (pdp11_dz.c, for one).  The console path below has always been right,
         * which is why `attach ttyNN console' plus a bare `send "..."' works. */
        if (! (c & (TMXR_VALID | SCPE_KFLAG))) {
#ifdef REMOTE_TIMEOUT
            now = sim_get_time (0);
            if (now > tty_last_time[num] + 5*60) {
                ++tty_idle_count[num];
                if (tty_idle_count[num] > 3) {
                    tmxr_linemsg (t, "\r\nSIMH: END OF SESSION\r\n");
                    tmxr_reset_ln (t);
                    return -1;
                }
                tmxr_linemsg (t, "\r\nSIMH: WAKE UP!\r\n");
                tty_last_time[num] = now;
            }
#endif
            return -1;
        }
        tty_idle_count[num] = 0;
        tty_last_time[num] = sim_get_time (0);

        if (tty_unit[num].flags & TTY_CMDLINE_MASK) {
            /* Continuing CLI mode. */
            vt_cmd_loop (num, c & 0377);
            return -1;
        }
        if ((c & 0377) == sim_int_char) {
            /* Entering CLI mode. */
            tty_unit[num].flags |= TTY_CMDLINE_MASK;
            tmxr_linemsg (t, "sim>");
            vt_cptr[num] = vt_cbuf[num];
            return -1;
        }
    } else {
        /* Console (keyboard) input. */
        c = sim_poll_kbd();
        if (! (c & SCPE_KFLAG))
            return -1;
    }
    c &= 0377;
    return c ? c : 8;
}

/*
 * Reading UTF-8, returning KOI-7.
 * The resulting char is in the range 0..0177.
 * If no input, returns -1.
 */
static int vt_kbd_input_unicode (int num)
{
    /* A sequence may arrive split across polls (the console gets one byte
     * per poll from `send'), so the bytes already read are kept per line. */
    static unsigned char part [LINES_MAX+1][2];
    static int nparts [LINES_MAX+1];
    int c, c1, c2, r;
  again:
    r = vt_getc (num);
    if (r < 0 || r > 0377)
        return r;
    c = r & 0377;
    if (nparts[num] == 0) {
        if (! (c & 0x80))
            return unicode_to_koi7 (c);
        part[num][0] = c;
        nparts[num] = 1;
        goto again;
    }
    c1 = part[num][0];
    if ((c1 & 0x20) && nparts[num] == 1) {
        part[num][1] = c;
        nparts[num] = 2;
        goto again;
    }
    nparts[num] = 0;
    if (! (c1 & 0x20))
        return unicode_to_koi7 ((c1 & 0x1f) << 6 | (c & 0x3f));
    c2 = part[num][1];
    if (c1 == 0xEF && c2 == 0xBB && c == 0xBF) {
        /* Skip zero width no-break space. */
        goto again;
    }
    return unicode_to_koi7 ((c1 & 0x0f) << 12 | (c2 & 0x3f) << 6 |
                            (c & 0x3f));
}

/*
 * Alternatively, entering Cyrillics can be done without switching keyboard layouts.
 * Period and comma are entered with shift, less-than and greater-than are mapped to tilde-grave.
 * Semicolon is }, quote is |.
 */
static int vt_kbd_input_koi7 (int num)
{
    int r;

    r = vt_getc (num);
    if (r < 0 || r > 0377)
        return r;
    r &= 0377;
    switch (r) {
    case '\r': return '\003';
    case 'q': return 'j';
    case 'w': return 'c';
    case 'e': return 'u';
    case 'r': return 'k';
    case 't': return 'e';
    case 'y': return 'n';
    case 'u': return 'g';
    case 'i': return '{';
    case 'o': return '}';
    case 'p': return 'z';
    case '[': return 'h';
    case '{': return '[';
    case 'a': return 'f';
    case 's': return 'y';
    case 'd': return 'w';
    case 'f': return 'a';
    case 'g': return 'p';
    case 'h': return 'r';
    case 'j': return 'o';
    case 'k': return 'l';
    case 'l': return 'd';
    case ';': return 'v';
    case '}': return ';';
    case '\'': return '|';
    case '|': return '\'';
    case 'z': return 'q';
    case 'x': return '~';
    case 'c': return 's';
    case 'v': return 'm';
    case 'b': return 'i';
    case 'n': return 't';
    case 'm': return 'x';
    case ',': return 'b';
    case '<': return ',';
    case '.': return '`';
    case '>': return '.';
    case '~': return '>';
    case '`': return '<';
    default: return r;
    }
}

int odd_parity(unsigned char c)
{
    c = (c & 0x55) + ((c >> 1) & 0x55);
    c = (c & 0x33) + ((c >> 2) & 0x33);
    c = (c & 0x0F) + ((c >> 4) & 0x0F);
    return c & 1;
}

/*
 * Converting Enter and Backspace to conventional values,
 * unless the mode is RAW.
 */
int vt_fix(int num, int c) {
    if (! tty_raw (num)) {
        switch (c) {
        case '\r': case '\n':
            return 3;     /* ETX is used as Enter */
        case '\177':
            return '\b';  /* ASCII DEL -> BS */
        }
    }
    return c;
}

/*
 * Receive a character (in KOI-7) from a line.
 */
int getsym(int num) {
    switch (tty_unit[num].flags & TTY_CHARSET_MASK) {
    case TTY_KOI7_JCUKEN_CHARSET:
        return vt_kbd_input_koi7 (num);
    case TTY_RAW_CHARSET:
    case TTY_RAW8_CHARSET:
    case TTY_KOI7_QWERTY_CHARSET:
        return vt_getc (num);
    case TTY_UNICODE_CHARSET:
        return vt_kbd_input_unicode (num);
    default:
        return '?';
    }
}

/*
 * Handling input from all connected terminals.
 */
void vt_receive()
{
    uint32 workset = vt_mask;
    int num;

    TTY_IN &= ~workset;
    for (num = besm6_highest_bit (workset) - TTY_MAX;
         workset; num = besm6_highest_bit (workset) - TTY_MAX) {
        uint32 mask = 1 << (TTY_MAX - num);
        switch (tty_instate[num]) {
        case 0:
            if (tty_typed[num] <= -2) {
                /* A "physically" disconnected line; upon reconnecting,
                 * there will be no echo and no reaction to commands;
                 * <enter>HYC<enter> needs to be typed
                 * to re-initialize the line
                 */
                TTY_IN |= mask;         /* "long start" */
                break;
            }
            tty_typed[num] = getsym(num);
            if (tty_typed[num] < 0) {
                break;
            }
            if (tty_typed[num] <= 0177) {
                tty_typed[num] = vt_fix(num, tty_typed[num]);
                tty_instate[num] = 1;
                TTY_IN |= mask;         /* start bit */
                GRP |= GRP_TTY_START;   /* not used ? */
                /* auto-enabling the interrupt just in case
                 * (seems to be unneeded as the interrupt is never disabled)
                 */
                MGRP |= GRP_SERIAL;       
                vt_receiving |= mask;
            }
            break;
        case 1: case 2: case 3: case 4: case 5: case 6: case 7:
            /* need inverted byte */
            TTY_IN |= (tty_typed[num] & (1 << (tty_instate[num]-1))) ? 0 : mask;
                tty_instate[num]++;
                break;
        case 8:
            TTY_IN |= odd_parity(tty_typed[num]) ? 0 : mask;        /* even parity of inverted */
            tty_instate[num]++;
            break;
        case 9: case 10: case 11:
            /* stop bits are 0 */
            tty_instate[num]++;
            break;
        case 12:
            tty_instate[num] = 0;   /* ready for the next char */
            vt_receiving &= ~mask;
            break;
        }
        workset &= ~mask;
    }
    if (vt_receiving)
        vt_idle = 0;
}

#define DIRECT
#ifdef DIRECT
#define TT_CLEAR(x,y) x &= ~(y)
#define TT_SET(x,y) x |= (y)
#elif defined(INVERTED)
#define TT_SET(x,y) x &= ~(y)
#define TT_CLEAR(x,y) x |= (y)
#endif

/*
 * Input from Baudot teletypes.
 */
void tt_receive()
{
    uint32 workset = tt_mask;
    int num;

    for (num = besm6_highest_bit (workset) - TTY_MAX;
         workset; num = besm6_highest_bit (workset) - TTY_MAX) {
        uint32 mask = 1 << (TTY_MAX - num);
        uint32 inbit = tt_inbit (num);

        TT_CLEAR(TTY_IN, inbit);
        switch (tty_instate[num]) {
        case 0:
            if (tt_next[num] >= 0) {
                tty_typed[num] = tt_next[num];
                tt_next[num] = -1;
            } else {
                int c = getsym(num), code, r;

                if (c < 0 || c > 0177)
                    break;
                code = tomtk2 (c, &r);
                if (code < 0)
                    break;
                if (code == MTK2_LF) {
                    /* A new line is a new input buffer, and the guest keeps the
                     * keyboard register in the buffer. */
                    tt_inreg[num] = -1;
                } else if (r >= 0 && r != tt_inreg[num]) {
                    tt_next[num] = code;
                    tt_inreg[num] = r;
                    code = mtk2_shift[r];
                }
                tty_typed[num] = code;
            }
            tty_instate[num] = 1;
            TT_SET(TTY_IN, inbit);          /* start bit */
            GRP |= GRP_TTY_START;
            tt_receiving |= mask;
            break;
        case 1: case 2: case 3: case 4: case 5:
            /* need inverted byte, big endian ordering */
            TT_SET(TTY_IN, (tty_typed[num] & (1 << (5-tty_instate[num]))) ? 0 : inbit);
            tty_instate[num]++;
            break;
        case 6: case 7: case 8:
            /* stop bits are 0 */
            tty_instate[num]++;
            break;
        case 9:
            tty_instate[num] = 0;   /* ready for the next char */
            tt_receiving &= ~mask;
            break;
        }
        workset &= ~mask;
    }
    if (tt_receiving)
        vt_idle = 0;
}

/*
 * Checking if all terminals are idle. 
 * SIMH should not enter idle mode until they are.
 */
int vt_is_idle ()
{
    if (dks_busy ())
        return 0;
    return (tt_mask ? vt_idle > 300 : vt_idle > 10);
}

int tty_query ()
{
    /*      besm6_debug ("*** TTY: query");*/
    return TTY_IN;
}

static char cons_is_printing[2];

extern unsigned short gost_to_unicode(unsigned char);
extern unsigned char unicode_to_gost(unsigned short);
extern void uni2utf8(unsigned short ch, char buf[5]);

/*
 * The Consul-254 code, which is GOST-10859 only for digits and Cyrillic.
 * Both tables must agree with the Consul columns of ТАБКОД in ТЕРМ, the
 * table Диспак uses when a line is a plain Consul (not VIDI).  0200 is
 * "nothing": a control code in the first table, a character the Consul
 * has no key for in the second.
 */
static const unsigned char consul_to_gost [128] = {
    /* 000-007 */ 0000, 0001, 0002, 0003, 0004, 0005, 0006, 0007,
    /* 010-017 */ 0010, 0011, 0012, 0013, 0014, 0200, 0200, 0200,
    /* 020-027 */ 0200, 0200, 0200, 0200, 0200, 0200, 0200, 0017,
    /* 030-037 */ 0115, 0031, 0200, 0200, 0200, 0200, 0200, 0200,
    /* 040-047 */ 0040, 0041, 0042, 0043, 0044, 0045, 0046, 0047,
    /* 050-057 */ 0050, 0051, 0052, 0053, 0054, 0055, 0056, 0057,
    /* 060-067 */ 0060, 0061, 0062, 0063, 0064, 0065, 0066, 0067,
    /* 070-077 */ 0070, 0071, 0072, 0073, 0074, 0075, 0076, 0200,
    /* 100-107 */ 0030, 0015, 0016, 0020, 0021, 0022, 0023, 0024,
    /* 110-117 */ 0026, 0027, 0017, 0025, 0036, 0200, 0200, 0200,
    /* 120-127 */ 0200, 0200, 0200, 0200, 0200, 0200, 0200, 0017,
    /* 130-137 */ 0132, 0130, 0200, 0200, 0200, 0200, 0200, 0200,
    /* 140-147 */ 0131, 0126, 0113, 0101, 0077, 0037, 0112, 0114,
    /* 150-157 */ 0102, 0103, 0116, 0104, 0125, 0105, 0123, 0122,
    /* 160-167 */ 0107, 0110, 0120, 0111, 0100, 0035, 0034, 0124,
    /* 170-177 */ 0032, 0033, 0200, 0121, 0117, 0127, 0106, 0200,
};

static const unsigned char gost_to_consul [0140] = {
    /* 000-007 */ 0000, 0001, 0002, 0003, 0004, 0005, 0006, 0007,
    /* 010-017 */ 0010, 0011, 0012, 0013, 0014, 0101, 0102, 0027,
    /* 020-027 */ 0103, 0104, 0105, 0106, 0107, 0113, 0110, 0111,
    /* 030-037 */ 0100, 0031, 0170, 0171, 0166, 0165, 0114, 0145,
    /* 040-047 */ 0040, 0041, 0042, 0043, 0044, 0045, 0046, 0047,
    /* 050-057 */ 0050, 0051, 0052, 0053, 0054, 0055, 0056, 0057,
    /* 060-067 */ 0060, 0061, 0062, 0063, 0064, 0065, 0066, 0067,
    /* 070-077 */ 0070, 0071, 0072, 0073, 0074, 0075, 0076, 0144,
    /* 100-107 */ 0164, 0143, 0150, 0151, 0153, 0155, 0176, 0160,
    /* 110-117 */ 0161, 0163, 0146, 0142, 0147, 0030, 0152, 0174,
    /* 120-127 */ 0162, 0173, 0200, 0156, 0167, 0200, 0141, 0175,
    /* 130-137 */ 0131, 0140, 0130, 0200, 0200, 0200, 0200, 0200,
};

#define CONSUL_LF       020     /* also ends the input line */
#define CONSUL_CR       037
#define CONSUL_CANCEL   0141    /* the % key: cancels the input line */
#define CONSUL_NOGLYPH  0172    /* printed for what the Consul cannot print; typed, cancels a symbol */

static const unsigned short koi7_cyr_unicode [31] = {
    0x042e, 0x0410, 0x0411, 0x0426, 0x0414, 0x0415, 0x0424, 0x0413,
    0x0425, 0x0418, 0x0419, 0x041a, 0x041b, 0x041c, 0x041d, 0x041e,
    0x041f, 0x042f, 0x0420, 0x0421, 0x0422, 0x0423, 0x0416, 0x0412,
    0x042c, 0x042b, 0x0417, 0x0428, 0x042d, 0x0429, 0x0427,
};

/*
 * A KOI-7 keystroke as the Consul key that produces it, or -1 if it has none.
 */
static int koi7_to_consul (int c)
{
    int gost;

    switch (c) {
    case '\r': case '\n': case '\003':
        return CONSUL_LF;
    case '\b': case '\177':
        return CONSUL_NOGLYPH;
    case 'U' & 037:
        return CONSUL_CANCEL;
    }
    if (c < ' ')
        return -1;
    gost = unicode_to_gost (c < 0x60 ? c : koi7_cyr_unicode[c - 0x60]);
    /* unicode_to_gost() answers a space for anything it does not know. */
    if (gost == 017 && c != ' ')
        return -1;
    if (gost >= 0140 || gost_to_consul[gost] == 0200)
        return -1;
    return gost_to_consul[gost];
}

static void consul_putc (int line_num, int c)
{
    char buf[5];

    switch (c) {
    case CONSUL_LF:
        vt_puts (line_num, "\r\n");
        return;
    case CONSUL_CR:
        vt_putc (line_num, '\r');
        return;
    case CONSUL_NOGLYPH:
        vt_putc (line_num, '?');
        return;
    }
    /* Control codes, among them the 0236 that starts every output, print nothing. */
    if (consul_to_gost[c] == 0200)
        return;
    /* The printer's table shows GOST 017 as a visible ␣. */
    if (consul_to_gost[c] == 017) {
        vt_putc (line_num, ' ');
        return;
    }
    uni2utf8 (gost_to_unicode (consul_to_gost[c]), buf);
    vt_puts (line_num, buf);
}

void consul_print (int dev_num, uint32 cmd)
{
    int line_num = dev_num + TTY_MAX + 1;
    unsigned b = cmd & 0377;

    if (tmgu_on && dev_num == 0) {
        if (TMGU_TRACE)
            besm6_debug (">>> TMGU: %03o", b);
        READY2 &= ~TMGU_OUT_RDY;
        if (!tmgu_linked) {
            /* КЛПВЫВ: four leading zeros, then 377 252 125. */
            if (b == 0 && tmgu_key_out == 0)
                ;
            else if (tmgu_key_out < 3 && b == tmgu_key[tmgu_key_out]) {
                tmgu_key_out++;
                if (tmgu_key_out == 3 && TMGU_TRACE)
                    besm6_debug (">>> TMGU: output key ok");
                tmgu_check_linked ();
            } else if (b == tmgu_key[0])
                tmgu_key_out = 1;
            else
                tmgu_key_out = 0;
        } else
            tmgu_out_byte (b);
        READY2 |= TMGU_OUT_RDY;
        PRP |= CONS_CAN_PRINT[0];
        tmgu_offer_input ();
        vt_idle = 0;
        return;
    }

    if (tty_dev.dctrl)
        besm6_debug(">>> CONSUL%o: %03o", line_num, b);

    if (tty_unit[line_num].flags & TTY_INVERSE_READY)
        READY2 |= CONS_READY[dev_num];
    else
        READY2 &= ~CONS_READY[dev_num]; /* temporarily not ready  */
    
    switch (tty_unit[line_num].flags & TTY_STATE_MASK) {
    case TTY_VT340_STATE:
        /* A raw8 line is a byte pipe: the guest owns the character set (v7besm sends
         * UTF-8), so all eight bits are data.  The mask stays everywhere else, RAW
         * included, where bit 8 is parity and vt_send() indexes a KOI-7 table. */
        if (tty_raw8 (line_num))
            vt_send (line_num, cmd & 0377);
        else
            vt_send (line_num, cmd & 0177);
        break;
    case TTY_CONSUL_STATE:
        if (tty_raw (line_num))
            vt_putc (line_num, cmd & (tty_raw8 (line_num) ? 0377 : 0177));
        else
            consul_putc (line_num, cmd & 0177);
        break;
    }
    cons_is_printing[dev_num] = 1;
    vt_idle = 0;
}

/*
 * Terminals behind the concentrator: a connect 041 for every multiplexer line
 * that came up (one at a time), and each character typed -> a short frame.
 */
static void tmgu_poll_lines (void)
{
    unsigned char msg[3];
    int n, c;

    if (!tmgu_on)
        return;
    for (n = 1; n <= LINES_MAX; ++n) {
        int up = line_is_mux (n) && tty_line[n].conn;
        if (up != tmgu_up[n]) {
            /* No disconnect message is known: the OS keeps the logical number
             * of a line that went away, and a reconnect uses it again. */
            tmgu_up[n] = up;
            if (up && !tmgu_log[n] && tmgu_wait != n)
                tmgu_want[n] = 1;
        }
    }
    if (!tmgu_linked)
        return;
    if (!tmgu_wait && tmgu_in_len <= TMGU_QMAX - 8) {
        for (n = 1; n <= LINES_MAX; ++n)
            if (tmgu_want[n]) {
                tmgu_send_connect (n);
                break;
            }
    }
    for (n = 1; n <= LINES_MAX; ++n) {
        if (!tmgu_up[n] || !tmgu_log[n])
            continue;
        if (tmgu_in_len > TMGU_QMAX - 8)    /* очередь не вычитана */
            return;
        c = getsym (n);
        if (c < 0 || c > 0177)
            continue;
        vt_send (n, c == '\177' ? '\b' : c);    /* местное эхо, как у мультиплексора */
        c = vt_fix (n, c);
        msg[0] = 1;                     /* адрес = НОМАБ (как 201 в 041), 0 - сброс связи */
        msg[1] = (unsigned char) tmgu_log[n];
        msg[2] = (unsigned char) (odd_parity (c) ? c | 0200 : c);
        if (TMGU_TRACE)
            besm6_debug (">>> TMGU: typed %03o on line %d, queue 001 %03o %03o",
                         c, n, msg[1], msg[2]);
        tmgu_queue (msg, 3);
    }
}

void consul_receive ()
{
    int c, line_num, dev_num;

    if (tmgu_on) {
        if (cons_is_printing[0]) {
            READY2 |= TMGU_OUT_RDY;
            PRP |= CONS_CAN_PRINT[0];
            cons_is_printing[0] = 0;
        }
        tmgu_poll_lines ();
        tmgu_offer_input ();
    }

    for (dev_num = 0; dev_num < 2; ++dev_num){
        line_num = dev_num + TTY_MAX + 1;
        if (tmgu_on && dev_num == 0)
            continue;                       /* concentrator owns Consul 0 */
        if (cons_is_printing[dev_num]) {
            if (tty_unit[line_num].flags & TTY_INVERSE_READY)
                READY2 &= ~CONS_READY[dev_num];
            else
                READY2 |= CONS_READY[dev_num];
            PRP |= CONS_CAN_PRINT[dev_num];
            cons_is_printing[dev_num] = 0;
        }
        if (! tty_line[line_num].conn)
            continue;
        /* ONE CHARACTER DEEP, SO DO NOT TAKE THE NEXT ONE YET.  CONSUL_IN is a single
         * register and this used to overwrite it every tick regardless of whether the
         * guest had read the last character, so anything typed faster than the guest
         * services its ПРП interrupt was lost.  A human pressing an arrow key sends
         * three bytes in one instant and the middle one went; the character is left in
         * the line's own input queue instead, and taken on a later tick. */
        if (cons_input_pending[dev_num])
            continue;
        c = getsym(line_num);
        if (c < 0)                              /* -1 nothing typed, -128 disconnected */
            continue;
        if (tty_raw8 (line_num)) {
            /* The other half of consul_print(): eight bits of data, no room for a
             * parity bit and no 7-bit code to compute one from.  vt_fix() is already
             * a no-op on a raw line. */
            CONSUL_IN[dev_num] = c & 0377;
        } else if ((tty_unit[line_num].flags & TTY_STATE_MASK) == TTY_CONSUL_STATE) {
            /* A plain Consul: its own code and ODD parity over all eight bits, which
             * prsn checks.  A VIDI line (the Videoton branch below) wants even. */
            if (c > 0177)
                continue;
            if (! tty_raw (line_num)) {
                c = koi7_to_consul (c);
                if (c < 0)
                    continue;
            }
            CONSUL_IN[dev_num] = odd_parity(c) ? c : c | 0200;
        } else {
            if (c > 0177)                       /* not a KOI-7 code */
                continue;
            c = vt_fix(line_num, c);
            CONSUL_IN[dev_num] = odd_parity(c) ? c | 0200 : c;
        }
        cons_input_pending[dev_num] = 1;
        PRP |= CONS_HAS_INPUT[dev_num];
        vt_idle = 0;
    }
}

uint32 consul_read (int num)
{
    uint32 v = CONSUL_IN[num];

    if (tmgu_on && num == 0) {
        if (TMGU_TRACE)
            besm6_debug ("<<< TMGU read: %03o", v);
        cons_input_pending[0] = 0;
        READY2 &= ~TMGU_IN_RDY;
        if (tmgu_key_in < 3) {
            tmgu_key_in++;
            tmgu_check_linked ();
        } else if (tmgu_in_pos < tmgu_in_len) {
            tmgu_in_pos++;
            if (tmgu_in_pos == tmgu_in_len)
                tmgu_in_len = tmgu_in_pos = 0;
        }
        tmgu_acked = 0;                 /* следующий байт - после ВДЗП */
        return v;
    }
    if (tty_dev.dctrl)
        besm6_debug("<<< CONSUL%o: %03o", num+TTY_MAX+1, v);
    /* Taken: consul_receive() may fetch the next character now. */
    cons_input_pending[num] = 0;
    return v;
}

uint32 mux_read ()
{
    if (tty_dev.dctrl)
        besm6_debug("<<< MUX: %03o %03o", MUX_SYLLABLE >> 8, MUX_SYLLABLE & 0377);
    return MUX_SYLLABLE;
}

/*
 * АС, "аппаратура сопряжения" (set tty mux), as МОТТ-100 and ЯНУС drive it.
 * Syllables are 16 bits: physical line in bits 14-9, then
 *   data:     bits 8-1 the character (bit 8 parity);
 *   service:  E15 set, bits 3-1 a machine number (РНМ), bits 8-4 a code.
 * Each line is attached to one machine or free (mux_rnm, 0 = free).
 * Service codes from the BESM-6:
 *   000  release the line (МОТТ sos, end of output);
 *   010  "01": start of output, the line becomes this machine's;
 *   110  poll (МОТТ ОПРОС): a free line becomes this machine's, the reply
 *        is a service syllable with the line's РНМ;
 *   200  line reset (ЯНУС ИНИЦ): the reply is the syllable without E8.
 * To the BESM-6: typed characters as data syllables, replies, and a service
 * syllable with РНМ 0 when a terminal connects, which МОТТ (СООБАС -> АС1)
 * takes as автоподключение.  ПРП 7 = a syllable is in the receive register
 * (read by 4143, released by 153); ПРП 6 = ready to send, a level: it is
 * raised again on every tick while no syllable is in transmission.
 */
#define MUX_SERVICE     0x4000          /* E15 */
#define MUX_QMAX        16
static int mux_tx_busy;
static unsigned char mux_rnm [LINES_MAX+1];
static char mux_up [LINES_MAX+1];
static uint32 mux_q [MUX_QMAX];
static int mux_qn;

static void mux_reset_state (void)
{
    mux_tx_busy = 0;
    mux_qn = 0;
    memset (mux_rnm, 0, sizeof (mux_rnm));
    memset (mux_up, 0, sizeof (mux_up));
}

static void mux_queue (uint32 syl)
{
    if (mux_qn < MUX_QMAX)
        mux_q[mux_qn++] = syl;
}

t_stat mux_event(UNIT * u) {
    mux_tx_busy = 0;
    PRP |= PRP_MUX_DONE;
    return SCPE_OK;
}

void mux_send(uint32 syl)
{
    int line_num = (syl >> 8) & 077;
    int unit = (line_num >= 1 && line_num <= LINES_MAX &&
                line_kind (line_num) != LINE_CONSUL) ? line_num : 1;

    if (tty_dev.dctrl)
        besm6_debug(">>> MUX: %03o %03o", syl >> 8, syl & 0377);
    mux_tx_busy = 1;
    if (syl & MUX_SERVICE) {
        int mach = syl & 7, code = syl & 0370;
        if (code & 0200) {
            /* Line reset / status request */
            int recv = line_num <= LINES_MAX && ! mux_rcv_off[line_num];
            mux_queue ((syl & ~0200) | (recv ? 0 : 010));
        } else if (line_num <= LINES_MAX) {
            switch (code) {
            case 0:
                mux_rnm[line_num] = 0;
                break;
            case 010:
                mux_rnm[line_num] = mach;
                break;
            case 0110:
                if (! mux_rnm[line_num])
                    mux_rnm[line_num] = mach;
                mux_queue ((line_num << 8) | MUX_SERVICE | mux_rnm[line_num]);
                break;
            default:
                besm6_debug(">>> MUX: line %02o, unknown service code %03o", line_num, code);
            }
        }
    } else if (mux_kind == MUX_AS && line_is_mux (line_num)) {
        /* Bit 8 of the syllable is parity, except on a raw8 line, where it is data. */
        vt_send (line_num, syl & (tty_raw8 (line_num) ? 0377 : 0177));
        vt_idle = 0;
    } else
        besm6_debug(">>> MUX: bad line %03o", line_num);
    sim_activate_after(tty_unit + unit, 10);
}

void mux_receive ()
{
    int c, line_num;

    if (mux_kind != MUX_AS) {
        if (mux_reg_busy)
            PRP |= PRP_MUX_INPUT;
        return;
    }
    if (! mux_tx_busy)
        PRP |= PRP_MUX_DONE;                    /* ready to send: a level */
    /* A terminal that connected announces itself: service syllable, РНМ 0. */
    for (line_num = 1; line_num < 0100 && line_num <= LINES_MAX; ++line_num) {
        int up = line_is_mux (line_num) && tty_line[line_num].conn;
        if (up != mux_up[line_num]) {
            mux_up[line_num] = up;
            if (up)
                mux_queue ((line_num << 8) | MUX_SERVICE);
            else
                mux_rnm[line_num] = 0;
        }
    }
    if (mux_reg_busy) {
        PRP |= PRP_MUX_INPUT;
        return;
    }
    /* Nothing is offered before the OS opens ПРП 7 (ГЕНС1 КЦОТ, ЯНУС): its
     * увв 153 would drop a syllable taken earlier. */
    if (! (MPRP & PRP_MUX_INPUT))
        return;
    if (mux_qn > 0) {
        MUX_SYLLABLE = mux_q[0];
        memmove (mux_q, mux_q + 1, --mux_qn * sizeof (mux_q[0]));
        PRP |= PRP_MUX_INPUT;
        vt_idle = 0;
        mux_reg_busy = 1;
        return;
    }
    for (line_num = 1; line_num < 0100 && line_num <= LINES_MAX; ++line_num) {
        if (! tty_line[line_num].conn || ! line_is_mux (line_num))
            continue;
        c = getsym(line_num);
        if (c < 0)
            continue;
        if (tty_raw8(line_num)) {
            /* A byte pipe: eight bits of data, echoed as typed and with no parity. */
            vt_send(line_num, c & 0377);
            MUX_SYLLABLE = (line_num << 8) | (c & 0377);
        } else {
            if (c > 0177)                       /* not a KOI-7 code */
                continue;
            vt_send(line_num, c == '\177' ? '\b' : c);
            c = vt_fix(line_num, c);
            MUX_SYLLABLE = (line_num << 8) | (odd_parity(c) ? c | 0200 : c);
        }
        if (tty_dev.dctrl)
            besm6_debug("Got %03o from line %02o", MUX_SYLLABLE & 0377, line_num);
        PRP |= PRP_MUX_INPUT;
        vt_idle = 0;
        mux_reg_busy = 1;
        break;                                  /* one receive register */
    }
}

/*
 * Lines connected via the ДКС: track connections and feed typed characters
 * to the emulated Э-60, which does the echo and line editing itself.
 */
void dks_receive ()
{
    static char dks_up [LINES_MAX+1];
    int num, c;

    if (dks_dev.flags & DEV_DIS || mux_kind != MUX_DKS)
        return;
    for (num = 1; num <= DKS_MAX_LINE; ++num) {
        int up;

        if (! line_is_mux (num))
            continue;
        up = tty_line[num].conn != 0;
        if (up != dks_up[num]) {
            if (! up)
                vt_getc (num);          /* takes the line offline */
            dks_up[num] = up;
            dks_line_state (num, up);
        }
        if (! up)
            continue;
        while (dks_line_can_input (num)) {
            c = getsym (num);
            if (c < 0)
                break;
            if (c <= 0177)
                dks_line_char (num, c);
        }
    }
    dks_poll ();
}

void mux_clear()
{
    if (tty_dev.dctrl)
        besm6_debug(">>> MUX: clear, PRP = %05o", PRP);
    MUX_SYLLABLE = 0;
    mux_reg_busy = 0;
    /* With КАДОПАМ these ПРП bits are КРК channels 1 and 2. */
    if (! (dks_dev.flags & DEV_DIS))
        return;
    PRP &= ~PRP_MUX_INPUT;
    if (! mux_tx_busy)
        PRP |= PRP_MUX_DONE;
}
    
