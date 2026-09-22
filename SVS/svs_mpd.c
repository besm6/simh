/*
 * SVS MPD (мультиплексор передачи данных) + telnet (TMXR) + консоль SIMH.
 *
 * Copyright (c) 2009, Leo Broukhis
 * Copyright (c) 2009, Serge Vakulenko
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

/*
 * У МПД было 7 процессорных каналов.
 *
 * Элементом состояния линии в МПД была принадлежность к одному из
 * процессоров.
 *
 * Принадлежность к процессору №0 (такого не бывает) означает, что
 * линия свободна.
 *
 * При поступлении данных с такой линии МПД отдаёт его случайному
 * готовому процессору.
 *
 * Процессор может послать в МПД команду закрепления линии за
 * любым готовым процессором, или команду освобождения линии, т.е.
 * закрепления за №0.
 *
 * Если процессор посылает данные в линию, которая ему не
 * принадлежит, то эти данные приходят процессору - хозяину этой
 * линии, как если бы они приходили с самой линии.
 */

#include "svs_defs.h"
#include "sim_sock.h"
#include "sim_tmxr.h"
#include <time.h>

/*
 * Юнит TTYN (N — восьмеричный номер линии МПД, цифры 1..77).
 * SIMH разбирает N как десятичное, поэтому «tty70» → юнит 70, а в слоге МПД
 * номер линии — те же цифры восьмерично (070). Юнит 0 — таймер опроса.
 *
 * Telnet: `attach tty <port>`. Ввод/вывод линий идёт слогами МПД.
 * Последовательного бит-бэнга и Consul нет.
 */
#define MPD_LINE_MAX    077         /* максимальный номер линии МПД */
#define TTY_UNIT_MAX    77          /* set tty77 → линия 077 */

time_t tty_last_time[TTY_UNIT_MAX+1];
int tty_idle_count[TTY_UNIT_MAX+1];

/* Command line buffers for TELNET mode. */
char vt_cbuf[CBUFSIZE][TTY_UNIT_MAX+1];
char *vt_cptr[TTY_UNIT_MAX+1];

t_stat vt_clk(UNIT *);
static void mpd_poll_input(CORE *cpu);
static int unit_to_line(int unum);
static int line_to_unit(int line);
t_stat tty_setconsole(UNIT *up, int32 v, CONST char *cp, void *dp);
t_stat tty_showconsole(FILE *f, UNIT *up, int32 v, CONST void *dp);

/*
 * Линия, с которой в МПД приходят символы консоли SIMH.
 * По умолчанию 075 (2053); в 2253 — 070 (`set tty70 console`).
 */
int mpd_console_line = 075;
extern const char *get_sim_sw(const char *cptr);

int attached_console;

UNIT tty_unit[TTY_UNIT_MAX+1] = {
    { UDATA(vt_clk, UNIT_IDLE, 0) },        /* fake unit, clock + telnet attach */
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
 * Index == SIMH unit number; MPD line number = unit_to_line(index).
 */
TMLN tty_line[TTY_UNIT_MAX+1];
TMXR tty_desc = { TTY_UNIT_MAX+1, 0, 0, tty_line };        /* mux descriptor */

#define TTY_UNICODE_CHARSET     0
#define TTY_KOI7_JCUKEN_CHARSET (1<<UNIT_V_UF)
#define TTY_KOI7_QWERTY_CHARSET (2<<UNIT_V_UF)
#define TTY_RAW_CHARSET         (3<<UNIT_V_UF)
#define TTY_CHARSET_MASK        (3<<UNIT_V_UF)
#define TTY_OFFLINE_STATE       0
#define TTY_ONLINE_STATE        (1<<(UNIT_V_UF+2))
#define TTY_STATE_MASK          (3<<(UNIT_V_UF+2))
#define TTY_DESTRUCTIVE_BSPACE  0
#define TTY_AUTHENTIC_BSPACE    (1<<(UNIT_V_UF+4))
#define TTY_BSPACE_MASK         (1<<(UNIT_V_UF+4))
#define TTY_CMDLINE_MASK        (1<<(UNIT_V_UF+5))

/* Юнит tty70 → линия 070: цифры номера юнита читаются восьмерично. */
static int unit_to_line(int unum)
{
    char buf[16];
    char *end;
    long line;

    if (unum < 1 || unum > TTY_UNIT_MAX)
        return -1;
    sprintf(buf, "%d", unum);
    line = strtol(buf, &end, 8);
    if (*end || line < 1 || line > MPD_LINE_MAX)
        return -1;
    return (int)line;
}

static int line_to_unit(int line)
{
    char buf[16];

    if (line < 1 || line > MPD_LINE_MAX)
        return -1;
    sprintf(buf, "%o", line);
    return atoi(buf);
}

static void reset_line(int num)
{
    /* Reset to a sensible default */
    tty_unit[num].flags &= ~(TTY_CHARSET_MASK|TTY_BSPACE_MASK|TTY_CMDLINE_MASK);
}

t_stat tty_reset(DEVICE *dptr)
{
    static int units_inited = 0;
    int i;

    if (!units_inited) {
        for (i = 1; i <= TTY_UNIT_MAX; ++i)
            tty_unit[i] = (UNIT){ UDATA(NULL, 0, 0) };
        units_inited = 1;
    }
    tty_line[0].conn = 1;                   /* faked, always busy */

    /* Schedule the very first TTY interrupt to match the next clock interrupt. */
    return sim_clock_coschedule(tty_unit, 0);
}

t_stat vt_clk(UNIT *this)
{
    CORE *cpu = &cpu_core[0];
    int num;

    /* Polling receiving from sockets */
    tmxr_poll_rx(&tty_desc);
    mpd_poll_input(cpu);

    /* Are there any new network connections? */
    num = tmxr_poll_conn(&tty_desc);
    if (num > 0 && num <= TTY_UNIT_MAX && unit_to_line(num) > 0) {
        char buf[80];
        TMLN *t = &tty_line[num];
        int line = unit_to_line(num);

        svs_debug("--- tty%d (T%03o): a new connection from %s",
                  num, line, t->ipad);
        reset_line(num);
        t->rcve = 1;
        tty_unit[num].flags &= ~TTY_STATE_MASK;
        tty_unit[num].flags |= TTY_ONLINE_STATE;

        switch (tty_unit[num].flags & TTY_CHARSET_MASK) {
        case TTY_KOI7_JCUKEN_CHARSET:
            tmxr_linemsg(t, "Encoding is KOI-7 (jcuken)\r\n");
            break;
        case TTY_KOI7_QWERTY_CHARSET:
            tmxr_linemsg(t, "Encoding is KOI-7 (qwerty)\r\n");
            break;
        case TTY_RAW_CHARSET:
            tmxr_linemsg(t, "Encoding is RAW\r\n");
            break;
        case TTY_UNICODE_CHARSET:
            tmxr_linemsg(t, "Encoding is UTF-8\r\n");
            break;
        }
        if (sim_int_char < 040 || sim_int_char == 0177) {
            sprintf(buf, "WRU – Break to sim> prompt character - is ^%c\r\n",
                    sim_int_char ^ 0100);
        } else {
            sprintf(buf, "WRU – Break to sim> prompt character - is %c\r\n",
                    sim_int_char);
        }
        tmxr_linemsg(t, buf);
        tty_idle_count[num] = 0;
        tty_last_time[num] = time(0);
        sprintf(buf, "%.24s from %s\r\n", ctime(&tty_last_time[num]), t->ipad);
        tmxr_linemsg(t, buf);

        /* Entering ^C (ETX) to get a prompt. */
        t->rxb[t->rxbpi++] = '\3';
    }

    /*
     * It the operator console is remote, we still need to probe the local keyboard
     * for a WRU, say, 10 times a second.
     */
    if (!attached_console) {
        static int divider;
        if (++divider == TICKS_PER_SEC/10) {
            divider = 0;
            if (SCPE_STOP == sim_poll_kbd())
                stop_cpu = 1;
        }
    }

    /* Polling sockets for transmission. */
    tmxr_poll_tx(&tty_desc);
    return sim_clock_coschedule(this, 0);
}

t_stat tty_setmode(UNIT *u, int32 val, CONST char *cptr, void *desc)
{
    int num = u - tty_unit;
    TMLN *t;

    if (unit_to_line(num) < 1)
        return SCPE_NXUN;
    t = &tty_line[num];

    switch (val & TTY_STATE_MASK) {
    case TTY_OFFLINE_STATE:
        if (t->conn) {
            if (t->rcve) {
                tmxr_reset_ln(t);
                t->rcve = 0;
            } else
                t->conn = 0;
        }
        break;
    case TTY_ONLINE_STATE:
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
 *
 * attach ttyN console — same as set ttyN console
 * attach ttyN none    — mark line unusable
 */
t_stat tty_attach(UNIT *u, CONST char *cptr)
{
    int num = u - tty_unit;
    char gbuf[CBUFSIZE];
    int r, n;
    int saved_conn[TTY_UNIT_MAX+1];

    /* All arguments but the magic words "console" and "none" are passed
     * to tmxr_attach().
     */
    get_glyph(cptr, gbuf, 0);
    /* Disallowing future connections to a line */
    if (strcmp(gbuf, "NONE") == 0) {
        if (unit_to_line(num) < 1)
            return SCPE_NXUN;
        /* Marking the TTY as unusable. */
        tty_line[num].conn = 1;
        tty_line[num].rcve = 0;
        svs_debug("--- turning off T%03o", unit_to_line(num));
        return SCPE_OK;
    }
    if (strcmp(gbuf, "CONSOLE") == 0)
        return tty_setconsole(u, 0, NULL, NULL);

    /* Saving and restoring all .conn,
     * because tmxr_attach() zeroes them. */
    for (n = 1; n <= TTY_UNIT_MAX; ++n)
        saved_conn[n] = tty_line[n].conn;
    /* The unit number is ignored for the port assignment */
    r = tmxr_attach(&tty_desc, &tty_unit[0], cptr);
    for (n = 1; n <= TTY_UNIT_MAX; ++n)
        if (saved_conn[n])
            tty_line[n].conn = 1;
    return r;
}

t_stat tty_detach(UNIT *u)
{
    return tmxr_detach(&tty_desc, &tty_unit[0]);
}

t_stat tty_showconsole(FILE *f, UNIT *up, int32 v, CONST void *dp)
{
    if (unit_to_line(up - tty_unit) == mpd_console_line)
        fprintf(f, "SIMH console");
    return SCPE_OK;
}

t_stat tty_setconsole(UNIT *up, int32 v, CONST char *cp, void *dp)
{
    int num = up - tty_unit;
    int line = unit_to_line(num);

    if (line < 1)
        return SCPE_ARG;
    /* Attaching SIMH console to a particular terminal. */
    mpd_console_line = line;
    attached_console = 1;
    up->flags &= ~TTY_STATE_MASK;
    up->flags |= TTY_ONLINE_STATE;
    tty_line[num].conn = 1;
    tty_line[num].rcve = 0;
    svs_debug("--- console on T%03o", line);
    return SCPE_OK;
}

/*
 * TTY control:
 * set ttyN unicode     - selecting UTF-8 encoding
 * set ttyN jcuken      - selecting KOI-7 encoding, JCUKEN layout
 * set ttyN qwerty      - selecting KOI-7 encoding, QWERTY layout
 * set ttyN raw         - selecting transmission of raw chars
 * set ttyN off         - disconnecting a line
 * set ttyN online      - mark line online
 * set ttyN console     - SIMH console ↔ MPD line N (octal digits, 1..77)
 * set ttyN destrbs     - destructive (erasing) backspace
 * set ttyN authbs      - authentic backspace (cursor left)
 * set tty disconnect=N - forceful termination of a telnet connection
 * show tty             - showing modes and types
 * show tty connections - showing IP-addresses and connection times
 * show tty statistics  - showing TX/RX byte counts
 */
MTAB tty_mod[] = {
    { TTY_CHARSET_MASK, TTY_UNICODE_CHARSET, "UTF-8 input",
      "UNICODE" },
    { TTY_CHARSET_MASK, TTY_KOI7_JCUKEN_CHARSET, "KOI7 (jcuken) input",
      "JCUKEN" },
    { TTY_CHARSET_MASK, TTY_KOI7_QWERTY_CHARSET, "KOI7 (qwerty) input",
      "QWERTY" },
    { TTY_CHARSET_MASK, TTY_RAW_CHARSET, "RAW input/output",
      "RAW" },
    { TTY_STATE_MASK, TTY_OFFLINE_STATE, "offline",
      "OFF", &tty_setmode },
    { TTY_STATE_MASK, TTY_ONLINE_STATE, "online",
      "ONLINE", &tty_setmode },
    { MTAB_XTD | MTAB_VUN, 0, "SIMH console", "CONSOLE",
      &tty_setconsole, &tty_showconsole, NULL,
      "use this MPD line for the SIMH console" },
    { TTY_BSPACE_MASK, TTY_DESTRUCTIVE_BSPACE, "destructive backspace",
      "DESTRBS" },
    { TTY_BSPACE_MASK, TTY_AUTHENTIC_BSPACE, NULL,
      "AUTHBS" },
    { MTAB_XTD | MTAB_VDV | MTAB_VALR, 1, NULL,
      "DISCONNECT", &tmxr_dscln, NULL, (void*) &tty_desc,
      "terminates telnet connection" },
    { UNIT_ATT, UNIT_ATT, "connections",
      NULL, NULL, &tmxr_show_summ, (void*) &tty_desc },
    { MTAB_XTD | MTAB_VDV | MTAB_NMO, 1, "CONNECTIONS",
      NULL, NULL, &tmxr_show_cstat, (void*) &tty_desc },
    { MTAB_XTD | MTAB_VDV | MTAB_NMO, 0, "STATISTICS",
      NULL, NULL, &tmxr_show_cstat, (void*) &tty_desc },
    { MTAB_XTD | MTAB_VUN | MTAB_NC, 0, NULL,
      "LOG", &tmxr_set_log, &tmxr_show_log, (void*) &tty_desc },
    { MTAB_XTD | MTAB_VUN | MTAB_NC, 0, NULL,
      "NOLOG", &tmxr_set_nolog, NULL, (void*) &tty_desc },
    { 0 }
};

DEVICE tty_dev = {
    "TTY", tty_unit, tty_reg, tty_mod,
    TTY_UNIT_MAX + 1, 8, 6, 1, 8, 50,
    NULL, NULL, &tty_reset, NULL, &tty_attach, &tty_detach,
    NULL, DEV_NET|DEV_DEBUG
};

/*
 * Sending a character to a terminal with the given number.
 * num is the SIMH unit index; MPD line = unit_to_line(num).
 */
void vt_putc(int num, int c)
{
    TMLN *t = &tty_line[num];
    int line = unit_to_line(num);

    if (line == mpd_console_line)
        sim_putchar(c);
    if (!t->conn)
        return;
    if (t->rcve) {
        /* A telnet connection. */
        tmxr_putc_ln(t, c);
    }
}

/*
 * Sending a string to a terminal with the given number.
 */
void vt_puts(int num, const char *s)
{
    TMLN *t = &tty_line[num];
    int line = unit_to_line(num);
    const char *p;

    if (line == mpd_console_line) {
        /* Console output. */
        for (p = s; *p; ++p)
            sim_putchar(*p);
    }
    if (!t->conn)
        return;
    if (t->rcve) {
        /* A telnet connection. */
        tmxr_linemsg(t, s);
    } else if (line != mpd_console_line) {
        /* Console output. */
        for (p = s; *p; ++p)
            sim_putchar(*p);
    }
}

const char *koi7_rus_to_unicode[32] = {
    "Ю", "А", "Б", "Ц", "Д", "Е", "Ф", "Г",
    "Х", "И", "Й", "К", "Л", "М", "Н", "О",
    "П", "Я", "Р", "С", "Т", "У", "Ж", "В",
    "Ь", "Ы", "З", "Ш", "Э", "Щ", "Ч", "\0x7f",
};

/* Videoton-340 employed single byte control codes rather than ESC sequences. */
void vt_send(int num, uint32 sym)
{
    if ((tty_unit[num].flags & TTY_CHARSET_MASK) == TTY_RAW_CHARSET) {
        vt_putc(num, sym);
    } else if (sym < 0x60) {
        switch (sym) {
        case '\031':
            /* Up */
            vt_puts(num, "\033[");
            sym = 'A';
            break;
        case '\032':
            /* Down */
            vt_puts(num, "\033[");
            sym = 'B';
            break;
        case '\030':
            /* Right */
            vt_puts(num, "\033[");
            sym = 'C';
            break;
        case '\b':
            /* Left */
            vt_puts(num, "\033[");
            if ((tty_unit[num].flags & TTY_BSPACE_MASK) == TTY_DESTRUCTIVE_BSPACE) {
                /* Erasing the previous char. */
                vt_puts(num, "D \033[");
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
            vt_puts(num, "\033[H\033[");
            sym = 'J';
            break;
        case '\n':
            /* Also does carriage return */
            vt_putc(num, '\r');
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
                vt_puts(num, "\033[2m");
                vt_putc(num, sym | 0x40);
                vt_puts(num, "\033[");
                /* Terminating the ESC sequence */
                sym = 'm';
            }
        }
        if (sym)
            vt_putc(num, sym);
    } else {
        vt_puts(num, koi7_rus_to_unicode[sym - 0x60]);
    }
}

/*
 * Converting from Unicode to KOI-7.
 * Returns -1 if unsuccessful.
 */
static int unicode_to_koi7(unsigned val)
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

static t_stat cmd_set(int32 num, CONST char *cptr);
static t_stat cmd_show(int32 num, CONST char *cptr);
static t_stat cmd_exit(int32 num, CONST char *cptr);
static t_stat cmd_help(int32 num, CONST char *cptr);

static CTAB cmd_table[] = {
    { "SET", &cmd_set, 0,
      "set unicode              select UTF-8 encoding\r\n"
      "set jcuken               select KOI7 encoding, 'jcuken' keymap\r\n"
      "set qwerty               select KOI7 encoding, 'qwerty' keymap\r\n"
      "set raw                  select no I/O conversions\r\n"
      "set online               mark line online\r\n"
      "set off                  disconnect the line\r\n"
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
    { "QUIT", &cmd_exit, 0, NULL },
    { "BYE", &cmd_exit, 0, NULL },
    { "HELP", &cmd_help, 0,
      "h{elp}                   type this message\r\n"
      "h{elp} <command>         type help for command\r\n"
    },
    { 0 }
};

/*
 * Set command
 */
static t_stat cmd_set(int32 num, CONST char *cptr)
{
    char gbuf[CBUFSIZE];
    int len;

    cptr = (CONST char*) get_sim_sw(cptr);
    if (!cptr)
        return SCPE_INVSW;
    if (!*cptr)
        return SCPE_NOPARAM;
    cptr = get_glyph(cptr, gbuf, 0);
    if (*cptr)
        return SCPE_2MARG;

    len = strlen(gbuf);
    if (strncmp("UNICODE", gbuf, len) == 0) {
        tty_unit[num].flags &= ~TTY_CHARSET_MASK;
        tty_unit[num].flags |= TTY_UNICODE_CHARSET;
    } else if (strncmp("JCUKEN", gbuf, len) == 0) {
        tty_unit[num].flags &= ~TTY_CHARSET_MASK;
        tty_unit[num].flags |= TTY_KOI7_JCUKEN_CHARSET;
    } else if (strncmp("QWERTY", gbuf, len) == 0) {
        tty_unit[num].flags &= ~TTY_CHARSET_MASK;
        tty_unit[num].flags |= TTY_KOI7_QWERTY_CHARSET;
    } else if (strncmp("RAW", gbuf, len) == 0) {
        tty_unit[num].flags &= ~TTY_CHARSET_MASK;
        tty_unit[num].flags |= TTY_RAW_CHARSET;
    } else if (strncmp("ONLINE", gbuf, len) == 0) {
        tty_unit[num].flags &= ~TTY_STATE_MASK;
        tty_unit[num].flags |= TTY_ONLINE_STATE;
    } else if (strncmp("OFF", gbuf, len) == 0) {
        tty_unit[num].flags &= ~TTY_STATE_MASK;
        tty_unit[num].flags |= TTY_OFFLINE_STATE;
        return tty_setmode(&tty_unit[num], TTY_OFFLINE_STATE, 0, 0);
    } else if (strncmp("DESTRBS", gbuf, len) == 0) {
        tty_unit[num].flags &= ~TTY_BSPACE_MASK;
        tty_unit[num].flags |= TTY_DESTRUCTIVE_BSPACE;
    } else if (strncmp("AUTHBS", gbuf, len) == 0) {
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
static t_stat cmd_show(int32 num, CONST char *cptr)
{
    TMLN *t = &tty_line[num];
    char gbuf[CBUFSIZE];
    MTAB *m;
    int len;

    cptr = (CONST char*) get_sim_sw(cptr);
    if (!cptr)
        return SCPE_INVSW;
    if (!*cptr) {
        sprintf(gbuf, "TTY%d", num);
        tmxr_linemsg(t, gbuf);
        for (m = tty_mod; m->mask || m->pstring || m->mstring; m++) {
            if (m->pstring && m->mask &&
                (tty_unit[num].flags & m->mask) == m->match) {
                tmxr_linemsg(t, ", ");
                tmxr_linemsg(t, m->pstring);
            }
        }
        if (t->txlog)
            tmxr_linemsg(t, ", log");
        tmxr_linemsg(t, "\r\n");
        return SCPE_OK;
    }
    cptr = get_glyph(cptr, gbuf, 0);
    if (*cptr)
        return SCPE_2MARG;
    len = strlen(gbuf);
    if (strncmp("STATISTICS", gbuf, len) == 0) {
        sprintf(gbuf, "line %d: input queued/total = %d/%d, "
                "output queued/total = %d/%d\r\n", num,
                t->rxbpi - t->rxbpr, t->rxcnt,
                t->txbpi - t->txbpr, t->txcnt);
        tmxr_linemsg(t, gbuf);
    } else {
        return SCPE_NXPAR;
    }
    return SCPE_OK;
}

/*
 * Exit command
 */
static t_stat cmd_exit(int32 num, CONST char *cptr)
{
    return SCPE_EXIT;
}

/*
 * Find command routine
 */
static CTAB *lookup_cmd(char *command)
{
    CTAB *c;
    int len;

    len = strlen(command);
    for (c = cmd_table; c->name; c++) {
        if (strncmp(command, c->name, len) == 0)
            return c;
    }
    return 0;
}

/*
 * Help command
 */
static t_stat cmd_help(int32 num, CONST char *cptr)
{
    TMLN *t = &tty_line[num];
    char gbuf[CBUFSIZE];
    CTAB *c;

    cptr = (CONST char*) get_sim_sw(cptr);
    if (!cptr)
        return SCPE_INVSW;
    if (!*cptr) {
        /* Listing all commands. */
        tmxr_linemsg(t, "Commands may be abbreviated.  Commands are:\r\n\r\n");
        for (c = cmd_table; c && c->name; c++)
            if (c->help)
                tmxr_linemsg(t, c->help);
        return SCPE_OK;
    }
    cptr = get_glyph(cptr, gbuf, 0);
    if (*cptr)
        return SCPE_2MARG;
    c = lookup_cmd(gbuf);
    if (!c)
        return SCPE_ARG;
    /* Describing a command. */
    tmxr_linemsg(t, c->help);
    return SCPE_OK;
}

/*
 * Executing a command.
 */
void vt_cmd_exec(int num)
{
    TMLN *t = &tty_line[num];
    char gbuf[CBUFSIZE];
    CONST char *cptr;
    CTAB *cmdp;
    t_stat err;
    extern char *scp_errors[];

    cptr = get_glyph(vt_cbuf[num], gbuf, 0);        /* get command glyph */
    cmdp = lookup_cmd(gbuf);                        /* lookup command */
    if (!cmdp) {
        tmxr_linemsg(t, scp_errors[SCPE_UNK - SCPE_BASE]);
        tmxr_linemsg(t, "\r\n");
        return;
    }
    err = cmdp->action(num, cptr);                  /* if found, exec */
    if (err >= SCPE_BASE) {                         /* error? */
        tmxr_linemsg(t, scp_errors[err - SCPE_BASE]);
        tmxr_linemsg(t, "\r\n");
    }
    if (err == SCPE_EXIT) {                         /* close telnet session */
        tmxr_reset_ln(t);
    }
}

/*
 * Command line interface mode.
 */
void vt_cmd_loop(int num, int c)
{
    TMLN *t = &tty_line[num];
    char *cbuf, **cptr;

    cbuf = vt_cbuf[num];
    cptr = &vt_cptr[num];

    switch (c) {
    case '\r':
    case '\n':
        tmxr_linemsg(t, "\r\n");
        if (*cptr <= cbuf) {
            /* An empty line - returning to terminal emulation. */
            tty_unit[num].flags &= ~TTY_CMDLINE_MASK;
            break;
        }
        /* Executing. */
        **cptr = 0;
        vt_cmd_exec(num);
        tmxr_linemsg(t, "sim>");
        *cptr = vt_cbuf[num];
        break;
    case '\b':
    case 0177:
        /* Backspace. */
        if (*cptr <= cbuf)
            break;
        tmxr_linemsg(t, "\b \b");
        while (*cptr > cbuf) {
            --*cptr;
            if (!(**cptr & 0x80))
                break;
        }
        break;
    case 'U' & 037:
        /* Erase line. */
erase_line:
        while (*cptr > cbuf) {
            --*cptr;
            if (!(**cptr & 0x80))
                tmxr_linemsg(t, "\b \b");
        }
        break;
    case 033:
        /* Escape [ X. */
        if (tmxr_getc_ln(t) != '[' + TMXR_VALID)
            break;
        switch (tmxr_getc_ln(t) - TMXR_VALID) {
        case 'A': /* Up arrow */
            if (*cptr <= cbuf) {
                *cptr = cbuf + strlen(cbuf);
                if (*cptr > cbuf)
                    tmxr_linemsg(t, cbuf);
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
        tmxr_putc_ln(t, c);
        break;
    }
}

/*
 * Getting a char from a terminal with the given number.
 * Returns -1 if there is no char to input.
 */
int vt_getc(int num)
{
    TMLN *t = &tty_line[num];
    extern int32 sim_int_char;
    int c;

    if (!t->conn) {
        /* Пользователь отключился. */
        if (t->ipad) {
            svs_debug("--- tty%d: disconnecting %s", num, t->ipad);
            t->ipad = NULL;
        }
        tty_setmode(tty_unit+num, TTY_OFFLINE_STATE, 0, 0);
        tty_unit[num].flags &= ~TTY_STATE_MASK;
        return -1;
    }
    if (!t->rcve)
        return -1;

    /* A telnet line. */
    c = tmxr_getc_ln(t);
    if (!(c & TMXR_VALID))
        return -1;
    tty_idle_count[num] = 0;
    tty_last_time[num] = time(0);

    if (tty_unit[num].flags & TTY_CMDLINE_MASK) {
        /* Continuing CLI mode. */
        vt_cmd_loop(num, c & 0377);
        return -1;
    }
    if ((c & 0377) == sim_int_char) {
        /* Entering CLI mode. */
        tty_unit[num].flags |= TTY_CMDLINE_MASK;
        tmxr_linemsg(t, "sim>");
        vt_cptr[num] = vt_cbuf[num];
        return -1;
    }
    return c & 0377;
}

/*
 * Reading UTF-8, returning KOI-7.
 * The resulting char is in the range 0..0177.
 * If no input, returns -1.
 */
static int vt_kbd_input_unicode(int num)
{
    int c1, c2, c3, r;
again:
    r = vt_getc(num);
    if (r < 0 || r > 0377)
        return r;
    c1 = r & 0377;
    if (!(c1 & 0x80))
        return unicode_to_koi7(c1);

    r = vt_getc(num);
    if (r < 0 || r > 0377)
        return r;
    c2 = r & 0377;
    if (!(c1 & 0x20))
        return unicode_to_koi7((c1 & 0x1f) << 6 | (c2 & 0x3f));

    r = vt_getc(num);
    if (r < 0 || r > 0377)
        return r;
    c3 = r & 0377;
    if (c1 == 0xEF && c2 == 0xBB && c3 == 0xBF) {
        /* Skip zero width no-break space. */
        goto again;
    }
    return unicode_to_koi7((c1 & 0x0f) << 12 | (c2 & 0x3f) << 6 | (c3 & 0x3f));
}

/*
 * Alternatively, entering Cyrillics can be done without switching keyboard
 * layouts. Period and comma are entered with shift, less-than and greater-than
 * are mapped to tilde-grave. Semicolon is }, quote is |.
 */
static int vt_kbd_input_koi7(int num)
{
    int r;

    r = vt_getc(num);
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
 * Приём из МПД. Протокол целиком — в МПД.md.
 *
 * Слог передаётся двумя байтами, старшим вперёд, по байту на строб: первый
 * байт — номер линии, второй — данные. Младший полубайт байта лежит в поле
 * данных ПОП (разр.38-35), старший — в том же поле ОПОП; признак приёма —
 * разр.34 ПОП, признак свободного передатчика — разр.33.
 *
 * Так их и читает МОТТ (мотт.bemsh, блок ПСЛ, физ.076252-076266): две
 * итерации (`уиа -1(М14)`), на каждой `сда 70` сдвигает R влево на 8 и
 * подмешивает байт из `рег 253`/`сда 136` и `рег 252`/`сда 142`, затем
 * `рег 52`/`рег 53` гасят регистры и `рег 50` даёт СТРОБ ПРИЕМА.
 *
 * `receive_state`: 0 — отдавать нечего, 2 — очередь старшего байта,
 * 3 — очередь младшего.
 *
 * Поля данных ставятся через `CONF_SET_DATA`: в ПОП живут ещё и разряды
 * межпроцессорных прерываний.
 */
static int receive_state = 0;
static uint32 receive_syllable = 0;

/*
 * Режим линии: 1 — выдача, 0 — приём. Задают служебные слоги «линию на
 * выдачу» (разр.4 младшего байта) и «линию на приём» (младший байт нулевой),
 * а возвращается режим в ответе на ЗСЛ разрядом 4 (поле ПНП маски КВЗСЛ).
 * Номер линии — 7 разрядов, отсюда размер.
 */
static uint8 mpd_line_send[0200];

/*
 * Поставить символ в очередь приёма МПД слогом с указанной линии.
 *
 * Разр.8 байта — дополнение до ЧЁТНОСТИ: `odd_parity()` даёт 1, когда в
 * разр.7-1 нечётное число единиц, и этот разряд достраивает байт до чётного.
 */
static void mpd_queue_char(CORE *cpu, int line, int c)
{
    c &= 0177;
    receive_syllable = (line << 8) | c | (odd_parity(c) << 7);
    receive_state = 2;
    if (SVS_DEV_TRACE())
        fprintf(sim_deb, "cpu%d --- МПД приём слога 0x%04x\n",
                cpu->index, receive_syllable);
    tty_strobe(cpu);
}

/*
 * Ввод с консоли SIMH и с telnet-линий — слогами МПД.
 *
 * Символ с консоли подаётся слогом с линии `mpd_console_line`; символы с
 * telnet — слогом с номером линии этой сессии (`unit_to_line`).
 *
 * Пока предыдущий слог не забран (`receive_state != 0`), источники не
 * опрашиваются: символ останется в очереди SIMH/tmxr до следующего раза.
 */
static void mpd_poll_input(CORE *cpu)
{
    int c, unum, line;

    if (receive_state != 0)
        return;

    /* Console (keyboard) input. */
    c = sim_poll_kbd();
    if (c == SCPE_STOP)
        stop_cpu = 1;   /* just in case */
    if (c & SCPE_KFLAG) {
        mpd_queue_char(cpu, mpd_console_line, c);
        return;
    }

    for (unum = 1; unum <= TTY_UNIT_MAX; ++unum) {
        line = unit_to_line(unum);
        if (line < 1)
            continue;
        if (!tty_line[unum].conn || !tty_line[unum].rcve)
            continue;

        switch (tty_unit[unum].flags & TTY_CHARSET_MASK) {
        case TTY_KOI7_JCUKEN_CHARSET:
            c = vt_kbd_input_koi7(unum);
            break;
        case TTY_RAW_CHARSET:
        case TTY_KOI7_QWERTY_CHARSET:
            c = vt_getc(unum);
            break;
        case TTY_UNICODE_CHARSET:
            c = vt_kbd_input_unicode(unum);
            break;
        default:
            c = '?';
            break;
        }
        if (c < 0)
            continue;
        if (c > 0177)
            continue;
        if ((tty_unit[unum].flags & TTY_CHARSET_MASK) != TTY_RAW_CHARSET) {
            if (c == '\r' || c == '\n')
                c = 3;              /* ETX is used as Enter */
            if (c == '\177')
                c = '\b';           /* ASCII DEL -> BS */
        }
        mpd_queue_char(cpu, line, c);
        return;
    }
}

void tty_strobe(CORE *cpu)
{
    int byte;

    switch (receive_state) {
    case 2: byte = (receive_syllable >> 8) & 0xff; break;   /* старший */
    case 3: byte = receive_syllable & 0xff; break;          /* младший */
    default:
        receive_state = 0;
        return;
    }
    cpu->POP  = CONF_SET_DATA(cpu->POP,  byte);
    cpu->OPOP = CONF_SET_DATA(cpu->OPOP, byte >> 4);
    cpu->POP |= CONF_MT | CONF_MR;
    receive_state++;
}

/*
 * Checking if all terminals are idle.
 * SIMH should not enter idle mode until they are.
 */
int vt_is_idle(void)
{
    return 1;
}

/*
 * Сброс МПД в исходное состояние.
 */
void mpd_reset(CORE *cpu)
{
    cpu->mpd_nbits = 0;
    cpu->mpd_data = 0;
    receive_state = 0;
    receive_syllable = 0;
    memset(mpd_line_send, 0, sizeof(mpd_line_send));

    /* Готов к передаче. */
    cpu->POP |= CONF_MT;
}

/*
 * Слог данных: разр.7-1 — символ КОИ-7, разр.8 дополняет байт до ЧЁТНОСТИ (§2).
 *
 * Показываем ВСЕ символы: непечатаемый — восьмеричным кодом в угловых
 * скобках, а символ со сбитой чётностью помечаем апострофом слева.
 * Перевод строки и возврат каретки печатаются как есть. Вывод идёт на
 * консоль SIMH и/или на telnet-сессию этой линии.
 */
static void mpd_emit_char(int line, int sym, int bad_parity)
{
    int unum = line_to_unit(line);

    if (bad_parity) {
        if (line == mpd_console_line)
            sim_putchar('`');
        else if (unum > 0 && tty_line[unum].rcve)
            tmxr_putc_ln(&tty_line[unum], '`');
    }

    if ((sym < 040 && sym != 012 && sym != 015) || sym == 0177) {
        char buf[8], *b;
        sprintf(buf, "<%03o>", sym);
        if (line == mpd_console_line) {
            for (b = buf; *b; ++b)
                sim_putchar(*b);
        }
        if (unum > 0 && tty_line[unum].rcve)
            tmxr_linemsg(&tty_line[unum], buf);
        return;
    }

    if (unum > 0)
        vt_send(unum, sym);
    else if (line == mpd_console_line) {
        /* console line without a matching unit — raw */
        if (sym == '\n')
            sim_putchar('\r');
        if (sym && sym != '\r' && sym != '\003')
            sim_putchar(sym);
    }
}

/*
 * Отправка полубайта.
 */
void mpd_send_nibble(CORE *cpu, int data)
{
    if (CPU_TRACE(cpu, DEB_INSN))
        fprintf(sim_deb, "cpu%d --- МПД передача полубайта\n", cpu->index);

    cpu->mpd_data <<= 4;
    cpu->mpd_data |= data & 0xf;
    cpu->mpd_nbits += 4;

    /* Передатчик занят. */
    cpu->POP &= ~CONF_MT;

    if (cpu->mpd_nbits >= 16) {
        /* Имеем полный слог, 16 бит. */
        /* Выдаем символы на stdout. */
        if (cpu->mpd_data & 0x8000) {
            /*
             * Служебный слог: разр.15-9 — номер линии, младший байт —
             * команда, разр.3-1 — номер ЭВМ. Команды перечислены в МПД.md
             * §3В; печатаем их по имени, иначе три разные команды выглядят
             * на консоли одинаково.
             */
            int line = (cpu->mpd_data >> 8) & 0177;
            int cmd  = cpu->mpd_data & 0377;
            const char *name =
                (cmd & 0200) ? "ЗСЛ"    :   /* разр.8 — запрос состояния */
                (cmd & 0100) ? "ОПРОС"  :   /* разр.7 — опрос линии */
                (cmd & 010)  ? "ВЫДАЧА" :   /* разр.4 — линию на выдачу */
                               "ПРИЕМ";     /* иначе — линию на приём */

            printf("<Т%o %s ЭВМ%d>", line, name, cmd & 7);

            /*
             * Ответ на служебный слог — тоже СЛУЖЕБНЫЙ слог.
             *
             * На приёме разр.16 разбирает ПАСМПД (мотт.bemsh:580-584,
             * физ.076272): `СДА 64+8 / УИ М17 / И Е8 / ПЕ СООБАС`. Взведённый
             * разряд уводит в СООБАС (физ.076722) — «СООБЩЕНИЕ ОТ АС», то есть
             * в разбор ответа на опрос или закрепление линии. Там:
             *
             *   СЛИА -'200'(М17) / ПИО ВЫХОД(М17)   номер линии; ноль — выход
             *   СЧ R / И П7 / ПО АС1                номер ЭВМ; ноль — автоподкл.
             *   ... / И Е8П6 / ПО МПДСВС            разр.8-6 — поле ошибки
             *   И Е7 / ПЕ КЧС1                      ОШ ЧЕТН ПРИЕМА
             *   И Е8 / ПО НЕТОШ                     ОШ КАН КВУ-АС
             *
             * Младший байт слога — команда. Установка линии на выдачу
             * (разр.4) и на приём (нулевой байт) задаёт режим и ответа не
             * требует: ответ на неё возвращает МОТТ в УСТВД2, тот взводит
             * ШАС01, и команда идёт снова (замерено: 23361 повтор слога
             * 0xbd09 за 20 млн команд). Отвечаем только на ЗАПРОСЫ — опрос
             * линии (разр.7) и ЗСЛ (разр.8).
             *
             * В ответе: разр.16 и номер линии из запроса, разр.8, 7, 6
             * нулевые, разр.3-1 (номер ЭВМ) из запроса, а разр.4 (ПНП) несёт
             * ТЕКУЩИЙ РЕЖИМ ЛИНИИ.
             *
             *   ЗСЛ   — ПРОЛИН сверяет ответ с эталоном `СЧ НОМАС / И П7 /
             *           ИЛИ Е4` под маской `КВЗСЛ КОНД В'157'` (поля ЧК, ПП,
             *           ПНП, N ЭВМ). Разр.4 = 1 означает «ЛИНИЯ УСТАНОВЛЕНА
             *           НА ВЫДАЧУ»; при нуле МОТТ взводит ШАС01 и повторяет
             *           установку.
             *   ОПРОС — АС76А сверяет только разр.3-1 с НОМАС; разр.4 он
             *           смотрит лишь на ветви несовпадения.
             */
            if (!(cmd & 0300)) {
                /* Задание режима: разр.4 — на выдачу, иначе на приём. */
                mpd_line_send[line] = (cmd & 010) != 0;
            } else if (receive_state == 0) {
                receive_syllable = (cpu->mpd_data & 0177400) |
                                   (mpd_line_send[line] ? 010 : 0) |
                                   (cpu->mpd_data & 07);
                receive_state = 2;
                tty_strobe(cpu);
            }
        } else {
            int line = (cpu->mpd_data >> 8) & 0177;
            int sym = cpu->mpd_data & 0177;
            int bad = odd_parity(cpu->mpd_data & 0377);

            mpd_emit_char(line, sym, bad);
        }
        fflush(stdout);

        if (CPU_TRACE(cpu, DEB_INSN))
            fprintf(sim_deb, "cpu%d --- МПД передача слога 0x%04x\n",
                    cpu->index, cpu->mpd_data);
        cpu->mpd_nbits = 0;
        cpu->mpd_data = 0;

        /* Делаем вид, что передача закончилась. */
        cpu->POP |= CONF_MT;
    }
}

/*
 * Подтверждение считывания принятого байта.
 */
void mpd_receive_update(CORE *cpu)
{
    /* СТРОБ ПРИЕМА от процессора. */
    tty_strobe(cpu);
}
