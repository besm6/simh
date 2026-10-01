/*
 * SVS display terminals ЕС-7920 (IBM 3270-compatible) on the ES channel.
 *
 * Copyright (c) 2026, Leonid Broukhis
 *
 * Дисплеи ЕС-7920 (АЦД, см. АЦД.md): до 16 устройств на ЕС-канале Х'01',
 * ТУС 020+n. Пользователь подключается настоящим эмулятором 3270
 * (c3270/x3270/wc3270) по tn3270:
 *
 *      attach display 4300
 *      c3270 -codepage cp1025 localhost:4300
 *
 * Все 16 дисплеев — за одним портом; подключение получает младший свободный.
 * Работают только дисплеи, включённые в вариант ГЕНС-а (шкала ШКУСТР).
 *
 * Что делает ОС (МОТТ, АСПВВ):
 *   КОП 03 — холостой ход; КОП 01 — запись: WCC 4B, знаки ДКОИ, пустые позиции
 *   (NUL), в конце SBA+IC; КОП 05 — стирание и запись (ЕС-7934); КОП 02 —
 *   чтение буфера: AID, адрес курсора, затем все 1920 позиций экрана.
 *   Ввод ОС опрашивает по таймеру: смотрит бит внимания в ТУС[020+n] (мл16,
 *   разр. 0x20), гасит его и тогда выдаёт чтение буфера. Состояние устройства
 *   ОС не проверяет: ответ всегда нормальный.
 *   Адреса буфера ОС пишет «сырыми» 6+6 разрядами и так же их читает (младшие
 *   6 разрядов каждого байта); клиенту 3270 они перекодируются в стандартную
 *   12-разрядную форму.
 *
 * Каждый дисплей хранит свой экран (ДКОИ) — по нему экран восстанавливается
 * при повторном подключении и отдаётся ОС, если та читает без нажатия клавиш.
 *
 * ЕС-7934 (печать на том же контроллере): attach -p DISPLAYn <файл> — запись
 * в дисплей n (КОП 01/05) печатается ещё и в файл, UTF-8. ПЕЧСВС шлёт на
 * строку одну стирание-запись: WCC (СУЗ Х'4B'), знаки ДКОИ, 0x25 (ПС7934) на
 * каждую протяжку, 0x19 (КТ7934, EM) в конце (печсвс.bemsh:18-24, 246-275).
 * Терминал должен быть в ШКППМ варианта ГЕНС-а (makeSVS2053.py --ppm), печать
 * АЦПУ уходит на него при ТР7 разр.23 (АЦПУ0) / 22 (АЦПУ1).
 */
#include "svs_defs.h"
#include "sim_tmxr.h"
#include "svs_display_cp.h"

#define NUM_DISPLAYS    16
#define DISP_ROWS       24
#define DISP_COLS       80
#define DISP_SIZE       (DISP_ROWS * DISP_COLS)
#define DISP_TUS_BASE   020             /* ТУС дисплея 0 */
#define DISP_ATTN       0x20            /* бит внимания в мл16 слова ТУС */
#define DISP_OUTQ       8192            /* очередь вывода линии, байт */
#define DISP_INREC      4096            /* принятая запись tn3270, байт */

/* Telnet. */
#define TN_IAC          255
#define TN_DONT         254
#define TN_DO           253
#define TN_WONT         252
#define TN_WILL         251
#define TN_SB           250
#define TN_SE           240
#define TN_EOR          239
#define OPT_BINARY      0
#define OPT_TTYPE       24
#define OPT_EOR         25
#define TTYPE_IS        0
#define TTYPE_SEND      1

/* Команды 3270 клиенту (удалённое подключение). */
#define CMD_WRITE       0xF1
#define CMD_ERASE_WRITE 0xF5
#define CMD_READ_BUFFER 0xF2

/* Приказы потока 3270. */
#define ORD_PT          0x05
#define ORD_GE          0x08
#define ORD_SBA         0x11
#define ORD_EUA         0x12
#define ORD_IC          0x13
#define ORD_SF          0x1D
#define ORD_SA          0x28
#define ORD_SFE         0x29
#define ORD_MF          0x2C
#define ORD_RA          0x3C

/* AID. */
#define AID_NONE        0x60
#define AID_CLEAR       0x6D
#define AID_PA1         0x6C
#define AID_PA2         0x6E
#define AID_PA3         0x6B

/* Состояние разбора telnet на приёме. */
enum { TS_DATA, TS_IAC, TS_OPT, TS_SB, TS_SB_IAC };

typedef struct {
    int online;                         /* клиент подключён */
    int ready;                          /* tn3270 согласован */
    int ts, tcmd;                       /* разбор telnet */
    unsigned char sb[64];               /* подпереговоры */
    int sblen;
    unsigned char rec[DISP_INREC];      /* принимаемая запись */
    int reclen;
    unsigned char outq[DISP_OUTQ];      /* очередь вывода */
    int outlen;
    int wait_rb;                        /* ждём ответ на Read Buffer */
    unsigned char screen[DISP_SIZE];    /* экран, ДКОИ */
    int cursor;
    int pending;                        /* ввод ждёт чтения ОС */
    unsigned char in_aid;
    int in_cursor;
    unsigned char in_image[DISP_SIZE];
} DISPLAY;

static DISPLAY disp[NUM_DISPLAYS];

static TMLN disp_line[NUM_DISPLAYS];
static TMXR disp_desc = { NUM_DISPLAYS, 0, 0, disp_line };

static t_stat disp_poll(UNIT *u);
static t_stat disp_reset(DEVICE *dptr);
static t_stat disp_attach(UNIT *u, CONST char *cptr);
static t_stat disp_detach(UNIT *u);
static t_stat disp_set_cp(UNIT *u, int32 val, CONST char *cptr, void *desc);
static t_stat disp_show_cp(FILE *st, UNIT *u, int32 v, CONST void *desc);
static t_stat disp_show_conn(FILE *st, UNIT *u, int32 v, CONST void *desc);

/*
 * Юниты 0-15 — дисплеи (attach -p — файл печати ЕС-7934), 16 — опрос сети
 * (к нему делается attach порта).
 */
#define DISP_UNIT_FLAGS  UNIT_ATTABLE | UNIT_SEQ
UNIT disp_unit[NUM_DISPLAYS + 1] = {
    { UDATA(NULL, DISP_UNIT_FLAGS, 0) }, { UDATA(NULL, DISP_UNIT_FLAGS, 0) },
    { UDATA(NULL, DISP_UNIT_FLAGS, 0) }, { UDATA(NULL, DISP_UNIT_FLAGS, 0) },
    { UDATA(NULL, DISP_UNIT_FLAGS, 0) }, { UDATA(NULL, DISP_UNIT_FLAGS, 0) },
    { UDATA(NULL, DISP_UNIT_FLAGS, 0) }, { UDATA(NULL, DISP_UNIT_FLAGS, 0) },
    { UDATA(NULL, DISP_UNIT_FLAGS, 0) }, { UDATA(NULL, DISP_UNIT_FLAGS, 0) },
    { UDATA(NULL, DISP_UNIT_FLAGS, 0) }, { UDATA(NULL, DISP_UNIT_FLAGS, 0) },
    { UDATA(NULL, DISP_UNIT_FLAGS, 0) }, { UDATA(NULL, DISP_UNIT_FLAGS, 0) },
    { UDATA(NULL, DISP_UNIT_FLAGS, 0) }, { UDATA(NULL, DISP_UNIT_FLAGS, 0) },
    { UDATA(&disp_poll, UNIT_ATTABLE | UNIT_IDLE, 0) },
};

static int disp_cp;                     /* 0 — cp1025, 1 — cp880 */

static REG disp_reg[] = {
    { 0 }
};

static MTAB disp_mod[] = {
    { MTAB_XTD | MTAB_VDV | MTAB_VALR, 0, "CODEPAGE", "CODEPAGE={CP1025|CP880}",
      &disp_set_cp, &disp_show_cp, NULL, "кодовая страница клиента 3270" },
    { MTAB_XTD | MTAB_VDV | MTAB_NMO, 0, "CONNECTIONS", NULL,
      NULL, &disp_show_conn, NULL, "подключения" },
    { 0 }
};

#define DEB_OPS 000001
#define DEB_DAT 000040
#define DEB_TN  000100

static DEBTAB disp_deb[] = {
    { "OPS",  DEB_OPS, "команды канала" },
    { "DATA", DEB_DAT, "данные" },
    { "TN",   DEB_TN,  "переговоры tn3270" },
    { NULL, 0 }
};

DEVICE disp_dev = {
    "DISPLAY", disp_unit, disp_reg, disp_mod,
    NUM_DISPLAYS + 1, 8, 19, 1, 8, 50,
    NULL, NULL, &disp_reset, NULL, &disp_attach, &disp_detach,
    NULL, DEV_DISABLE | DEV_DEBUG | DEV_MUX, 0, disp_deb
};

/*
 * Адрес буфера 3270: 6-разрядная группа -> код (12-разрядная адресация).
 */
static const unsigned char addr_code[64] = {
    0x40, 0xC1, 0xC2, 0xC3, 0xC4, 0xC5, 0xC6, 0xC7,
    0xC8, 0xC9, 0x4A, 0x4B, 0x4C, 0x4D, 0x4E, 0x4F,
    0x50, 0xD1, 0xD2, 0xD3, 0xD4, 0xD5, 0xD6, 0xD7,
    0xD8, 0xD9, 0x5A, 0x5B, 0x5C, 0x5D, 0x5E, 0x5F,
    0x60, 0x61, 0xE2, 0xE3, 0xE4, 0xE5, 0xE6, 0xE7,
    0xE8, 0xE9, 0x6A, 0x6B, 0x6C, 0x6D, 0x6E, 0x6F,
    0xF0, 0xF1, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7,
    0xF8, 0xF9, 0x7A, 0x7B, 0x7C, 0x7D, 0x7E, 0x7F,
};

/* Адрес из двух байтов: и «сырые» 6+6 разрядов ОС, и коды клиента. */
static int addr_decode(unsigned char b1, unsigned char b2)
{
    return (((b1 & 0x3F) << 6) | (b2 & 0x3F)) % DISP_SIZE;
}

static unsigned char to_client(unsigned char c)
{
    return disp_cp ? dkoi_to_cp880[c] : dkoi_to_cp1025[c];
}

static unsigned char from_client(unsigned char c)
{
    return disp_cp ? cp880_to_dkoi[c] : cp1025_to_dkoi[c];
}

/*
 * Вывод клиенту: очередь линии, сливается по возможности сразу и в опросе.
 */
static void flush_out(int n)
{
    DISPLAY *d = &disp[n];
    TMLN *t = &disp_line[n];
    int i = 0;

    while (i < d->outlen && t->conn) {
        if (tmxr_putc_ln(t, d->outq[i]) != SCPE_OK)
            break;
        i++;
    }
    if (i > 0) {
        memmove(d->outq, d->outq + i, d->outlen - i);
        d->outlen -= i;
    }
    tmxr_poll_tx(&disp_desc);
}

static void out_raw(int n, unsigned char c)
{
    DISPLAY *d = &disp[n];

    if (d->outlen < DISP_OUTQ)
        d->outq[d->outlen++] = c;
}

/* Байт данных записи: IAC удваивается. */
static void out_data(int n, unsigned char c)
{
    out_raw(n, c);
    if (c == TN_IAC)
        out_raw(n, TN_IAC);
}

static void out_eor(int n)
{
    out_raw(n, TN_IAC);
    out_raw(n, TN_EOR);
    flush_out(n);
}

static void out_cmd(int n, int cmd, int opt)
{
    out_raw(n, TN_IAC);
    out_raw(n, cmd);
    out_raw(n, opt);
}

/*
 * Экран целиком: стереть и записать (по подключении).
 */
static void send_screen(int n)
{
    DISPLAY *d = &disp[n];
    int i;

    out_data(n, CMD_ERASE_WRITE);
    out_data(n, 0xC3);                  /* WCC: сброс, клавиатура, MDT */
    for (i = 0; i < DISP_SIZE; i++)
        out_data(n, d->screen[i] ? to_client(d->screen[i]) : 0);
    out_data(n, ORD_SBA);
    out_data(n, addr_code[d->cursor >> 6]);
    out_data(n, addr_code[d->cursor & 077]);
    out_data(n, ORD_IC);
    out_eor(n);
}

/*
 * Записать поток ОС в экран дисплея и, если клиент готов, отдать ему.
 * erase: КОП 05 (стереть экран, адрес 0), иначе запись с позиции курсора.
 */
static void display_write(int n, int erase, const unsigned char *buf, int len)
{
    DISPLAY *d = &disp[n];
    int send = d->ready;
    int ba, i, a;

    if (erase) {
        memset(d->screen, 0, sizeof(d->screen));
        d->cursor = 0;
    }
    ba = d->cursor;
    if (send)
        out_data(n, erase ? CMD_ERASE_WRITE : CMD_WRITE);
    if (len > 0 && send)
        out_data(n, buf[0]);            /* WCC */

    for (i = 1; i < len; i++) {
        unsigned char c = buf[i];

        switch (c) {
        case ORD_SBA:
        case ORD_EUA:
            if (i + 2 >= len)
                goto done;
            a = addr_decode(buf[i+1], buf[i+2]);
            if (c == ORD_EUA)
                while (ba != a) {
                    d->screen[ba] = 0;
                    ba = (ba + 1) % DISP_SIZE;
                }
            ba = a;
            if (send) {
                out_data(n, c);
                out_data(n, addr_code[a >> 6]);
                out_data(n, addr_code[a & 077]);
            }
            i += 2;
            break;
        case ORD_RA:
            if (i + 3 >= len)
                goto done;
            a = addr_decode(buf[i+1], buf[i+2]);
            do {
                d->screen[ba] = buf[i+3];
                ba = (ba + 1) % DISP_SIZE;
            } while (ba != a);
            if (send) {
                out_data(n, c);
                out_data(n, addr_code[a >> 6]);
                out_data(n, addr_code[a & 077]);
                out_data(n, buf[i+3] ? to_client(buf[i+3]) : 0);
            }
            i += 3;
            break;
        case ORD_IC:
            d->cursor = ba;
            if (send)
                out_data(n, c);
            break;
        case ORD_PT:
            if (send)
                out_data(n, c);
            break;
        case ORD_SF:
        case ORD_GE:
            if (i + 1 >= len)
                goto done;
            d->screen[ba] = (c == ORD_GE) ? buf[i+1] : 0;
            ba = (ba + 1) % DISP_SIZE;
            if (send) {
                out_data(n, c);
                out_data(n, buf[i+1]);
            }
            i += 1;
            break;
        case ORD_SA:
            if (i + 2 >= len)
                goto done;
            if (send) {
                out_data(n, c);
                out_data(n, buf[i+1]);
                out_data(n, buf[i+2]);
            }
            i += 2;
            break;
        case ORD_SFE:
        case ORD_MF: {
            int pairs = (i + 1 < len) ? buf[i+1] : 0;
            int k;

            if (i + 1 + 2 * pairs >= len)
                goto done;
            if (c == ORD_SFE) {
                d->screen[ba] = 0;
                ba = (ba + 1) % DISP_SIZE;
            }
            if (send)
                for (k = 0; k <= 1 + 2 * pairs; k++)
                    out_data(n, buf[i + k]);
            i += 1 + 2 * pairs;
            break;
        }
        default:
            /* Знак (NUL — пустая позиция). */
            d->screen[ba] = c;
            ba = (ba + 1) % DISP_SIZE;
            if (send)
                out_data(n, c ? to_client(c) : 0);
            break;
        }
    }
done:
    if (send)
        out_eor(n);
}

/*
 * Байты массива обмена: по 6 в 48-разрядном слове, первый — в разр.48-41.
 */
static void get_bytes(int memaddr, unsigned char *buf, int n)
{
    int i;

    for (i = 0; i < n; i++) {
        t_value w = (memory[mmu_iom_data_pa(memaddr + i / 6)] >> 16) & BITS48;

        buf[i] = (w >> (8 * (5 - i % 6))) & 0xFF;
    }
}

static void put_bytes(int memaddr, int nbytes, const unsigned char *buf, int n)
{
    int w, k;

    for (w = 0; 6 * w < nbytes; ++w) {
        t_value v = 0;
        int pa = mmu_iom_data_pa(memaddr + w);

        for (k = 0; k < 6; ++k)
            v = (v << 8) | ((6 * w + k < n) ? buf[6 * w + k] : 0);
        memory[pa] = v << 16;
        tag[pa] = TAG_NUMBER48;
    }
}

/* Взвести бит внимания в ТУС дисплея: ОС опросит его в ОПРТУС. */
static void raise_attention(int n)
{
    IOMDATA *iom = &iom_data[0];
    uint32 a = iom->UTA + DISP_TUS_BASE + n;

    if (iom->UTA != 0 && a < MEMSIZE)
        memory[a] |= DISP_ATTN;
    sim_debug(DEB_OPS, &disp_dev, "дисплей %d: внимание, AID %02X, курсор %d\n",
              n, disp[n].in_aid, disp[n].in_cursor);
}

/*
 * ЕС-7934: напечатать поток записи в файл юнита n. buf[0] — WCC; NL (0x15)
 * и ПС7934 (0x25) — перевод строки, FF — прогон листа, CR — возврат каретки,
 * EM (0x19) — конец текста; приказы 3270 с операндами пропускаются, NUL —
 * пустое место.
 */
static void ppm_print(int n, const unsigned char *buf, int len)
{
    UNIT *u = &disp_unit[n];
    FILE *f = u->fileref;
    int i;

    for (i = 1; i < len; i++) {
        unsigned char c = buf[i];

        switch (c) {
        case 0x15: case 0x25:
            fputc('\n', f);
            break;
        case 0x0C:
            fputc('\f', f);
            break;
        case 0x0D:
            fputc('\r', f);
            break;
        case 0x19:
            i = len;
            break;
        case 0x00:
        case ORD_IC: case ORD_PT:
            break;
        case ORD_SBA: case ORD_EUA:
            i += 2;
            break;
        case ORD_RA:
            i += 3;
            break;
        case ORD_SF: case ORD_GE:
            i += 1;
            break;
        case ORD_SA:
            i += 2;
            break;
        case ORD_SFE: case ORD_MF:
            i += 1 + 2 * ((i + 1 < len) ? buf[i+1] : 0);
            break;
        default:
            if (svs_dkoi_unicode(c))
                utf8_putc(svs_dkoi_unicode(c), f);
            else
                svs_put_unknown(c, f);  /* кода нет в таблице: \XX */
            break;
        }
    }
    fflush(f);
    /* go/cont ставят файл на u->pos (scp.c, run_cmd) — сдвигать pos. */
    u->pos = (t_addr)ftell(f);
}

/*
 * Обмен с дисплеем n по команде ОС. nbytes — длина массива в байтах.
 */
t_stat svs_display_io(int n, int kop, int memaddr, int nbytes)
{
    DISPLAY *d;
    static unsigned char buf[6 * 01000];

    if (n < 0 || n >= NUM_DISPLAYS)
        return SCPE_NXUN;
    d = &disp[n];
    if (nbytes > (int)sizeof(buf))
        nbytes = sizeof(buf);

    switch (kop) {
    case 0x01:                          /* запись */
    case 0x05:                          /* стирание и запись */
        get_bytes(memaddr, buf, nbytes);
        sim_debug(DEB_OPS, &disp_dev, "дисплей %d: %s, %d байт\n", n,
                  kop == 0x05 ? "стирание и запись" : "запись", nbytes);
        if (sim_deb && (disp_dev.dctrl & DEB_DAT)) {
            int i;

            fprintf(sim_deb, "DISPLAY%d DATA:", n);
            for (i = 0; i < nbytes; i++)
                fprintf(sim_deb, " %02X", buf[i]);
            fprintf(sim_deb, "\n");
        }
        display_write(n, kop == 0x05, buf, nbytes);
        if (disp_unit[n].flags & UNIT_ATT)
            ppm_print(n, buf, nbytes);
        break;

    case 0x02: {                        /* чтение буфера */
        const unsigned char *img = d->pending ? d->in_image : d->screen;
        int cur = d->pending ? d->in_cursor : d->cursor;

        buf[0] = d->pending ? d->in_aid : AID_NONE;
        buf[1] = addr_code[cur >> 6];
        buf[2] = addr_code[cur & 077];
        memcpy(buf + 3, img, DISP_SIZE);
        put_bytes(memaddr, nbytes, buf, 3 + DISP_SIZE);
        sim_debug(DEB_OPS, &disp_dev, "дисплей %d: чтение буфера, AID %02X\n",
                  n, buf[0]);
        d->pending = 0;
        break;
    }

    case 0x03:                          /* холостой ход */
    default:
        sim_debug(DEB_OPS, &disp_dev, "дисплей %d: КОП %02X\n", n, kop);
        break;
    }
    return SCPE_OK;
}

/*
 * Принята запись от клиента (без IAC EOR).
 */
static void got_record(int n)
{
    DISPLAY *d = &disp[n];
    unsigned char *r = d->rec;
    int len = d->reclen;
    int i, ba;

    if (len < 1)
        return;
    if (d->wait_rb) {
        /* Ответ на Read Buffer: AID, курсор, все позиции экрана. */
        d->wait_rb = 0;
        if (len >= 3)
            d->in_cursor = addr_decode(r[1], r[2]);
        memset(d->in_image, 0, sizeof(d->in_image));
        for (i = 3, ba = 0; i < len && ba < DISP_SIZE; i++) {
            if (r[i] == ORD_SF && i + 1 < len) {
                d->in_image[ba++] = 0;
                i++;
            } else {
                d->in_image[ba++] = r[i] ? from_client(r[i]) : 0;
            }
        }
        memcpy(d->screen, d->in_image, DISP_SIZE);
        d->cursor = d->in_cursor;
        d->pending = 1;
        raise_attention(n);
        return;
    }

    /* Нажата клавиша внимания. */
    d->in_aid = r[0];
    switch (r[0]) {
    case AID_CLEAR:
        memset(d->screen, 0, sizeof(d->screen));
        d->cursor = 0;
        /* fall through */
    case AID_PA1:
    case AID_PA2:
    case AID_PA3:
        d->in_cursor = d->cursor;
        memcpy(d->in_image, d->screen, DISP_SIZE);
        d->pending = 1;
        raise_attention(n);
        break;
    default:
        /* ВВОД, ПФ: весь экран с позициями — командой Read Buffer. */
        if (len >= 3)
            d->in_cursor = addr_decode(r[1], r[2]);
        d->wait_rb = 1;
        out_data(n, CMD_READ_BUFFER);
        out_eor(n);
        break;
    }
}

/* Подпереговоры: TERMINAL-TYPE IS ... */
static void got_sb(int n)
{
    DISPLAY *d = &disp[n];

    if (d->sblen >= 2 && d->sb[0] == OPT_TTYPE && d->sb[1] == TTYPE_IS) {
        d->sb[d->sblen < (int)sizeof(d->sb) ? d->sblen : (int)sizeof(d->sb) - 1] = 0;
        sim_debug(DEB_TN, &disp_dev, "дисплей %d: тип терминала %s\n",
                  n, (char *)d->sb + 2);
        out_cmd(n, TN_DO, OPT_EOR);
        out_cmd(n, TN_WILL, OPT_EOR);
        out_cmd(n, TN_DO, OPT_BINARY);
        out_cmd(n, TN_WILL, OPT_BINARY);
        flush_out(n);
        d->ready = 1;
        send_screen(n);
    }
}

/* Разбор принятого байта telnet. */
static void got_byte(int n, unsigned char c)
{
    DISPLAY *d = &disp[n];

    switch (d->ts) {
    case TS_DATA:
        if (c == TN_IAC)
            d->ts = TS_IAC;
        else if (d->reclen < DISP_INREC)
            d->rec[d->reclen++] = c;
        break;
    case TS_IAC:
        d->ts = TS_DATA;
        switch (c) {
        case TN_IAC:
            if (d->reclen < DISP_INREC)
                d->rec[d->reclen++] = c;
            break;
        case TN_EOR:
            got_record(n);
            d->reclen = 0;
            break;
        case TN_SB:
            d->sblen = 0;
            d->ts = TS_SB;
            break;
        case TN_WILL: case TN_WONT: case TN_DO: case TN_DONT:
            d->tcmd = c;
            d->ts = TS_OPT;
            break;
        }
        break;
    case TS_OPT:
        d->ts = TS_DATA;
        sim_debug(DEB_TN, &disp_dev, "дисплей %d: %s %d\n", n,
                  d->tcmd == TN_WILL ? "WILL" : d->tcmd == TN_WONT ? "WONT" :
                  d->tcmd == TN_DO ? "DO" : "DONT", c);
        if (d->tcmd == TN_WILL && c == OPT_TTYPE) {
            out_raw(n, TN_IAC); out_raw(n, TN_SB); out_raw(n, OPT_TTYPE);
            out_raw(n, TTYPE_SEND); out_raw(n, TN_IAC); out_raw(n, TN_SE);
            flush_out(n);
        } else if (c != OPT_TTYPE && c != OPT_EOR && c != OPT_BINARY) {
            /* Прочие возможности не поддерживаются (TN3270E и т.п.). */
            if (d->tcmd == TN_WILL)
                out_cmd(n, TN_DONT, c);
            else if (d->tcmd == TN_DO)
                out_cmd(n, TN_WONT, c);
            flush_out(n);
        }
        break;
    case TS_SB:
        if (c == TN_IAC)
            d->ts = TS_SB_IAC;
        else if (d->sblen < (int)sizeof(d->sb))
            d->sb[d->sblen++] = c;
        break;
    case TS_SB_IAC:
        if (c == TN_SE) {
            d->ts = TS_DATA;
            got_sb(n);
        } else {
            if (d->sblen < (int)sizeof(d->sb))
                d->sb[d->sblen++] = c;
            d->ts = TS_SB;
        }
        break;
    }
}

/*
 * Опрос сети: подключения, приём, вывод.
 */
static t_stat disp_poll(UNIT *u)
{
    int n;

    n = tmxr_poll_conn(&disp_desc);
    if (n >= 0 && n < NUM_DISPLAYS) {
        DISPLAY *d = &disp[n];

        sim_debug(DEB_TN, &disp_dev, "дисплей %d: подключение с %s\n",
                  n, disp_line[n].ipad);
        disp_line[n].rcve = 1;
        d->online = 1;
        d->ready = 0;
        d->ts = TS_DATA;
        d->reclen = d->sblen = d->outlen = 0;
        d->wait_rb = 0;
        out_cmd(n, TN_DO, OPT_TTYPE);
        flush_out(n);
    }

    tmxr_poll_rx(&disp_desc);
    for (n = 0; n < NUM_DISPLAYS; n++) {
        DISPLAY *d = &disp[n];
        int32 c;

        if (!disp_line[n].conn) {
            if (d->online) {
                sim_debug(DEB_TN, &disp_dev, "дисплей %d: отключение\n", n);
                d->online = d->ready = d->wait_rb = 0;
                d->outlen = 0;
            }
            continue;
        }
        while ((c = tmxr_getc_ln(&disp_line[n])) & TMXR_VALID)
            got_byte(n, c & 0377);
        if (d->outlen)
            flush_out(n);
    }
    tmxr_poll_tx(&disp_desc);
    return sim_clock_coschedule(u, 0);
}

static t_stat disp_reset(DEVICE *dptr)
{
    if (disp_unit[NUM_DISPLAYS].flags & UNIT_ATT)
        return sim_clock_coschedule(&disp_unit[NUM_DISPLAYS], 0);
    sim_cancel(&disp_unit[NUM_DISPLAYS]);
    return SCPE_OK;
}

/*
 * attach display <порт> — один порт на все дисплеи; номер юнита неважен.
 * Линии «сырые»: переговоры tn3270 ведёт сам модуль (sim_tmxr их отвергает).
 */
static t_stat disp_attach(UNIT *u, CONST char *cptr)
{
    t_stat r;
    int n = (int)(u - disp_unit);

    /* attach -p DISPLAYn <файл>: печать ЕС-7934 в файл. */
    if ((sim_switches & SWMASK('P')) && n >= 0 && n < NUM_DISPLAYS) {
        sim_switches &= ~SWMASK('P');
        return attach_unit(u, cptr);
    }

    tmxr_set_notelnet(&disp_desc);
    /* SO_REUSEADDR (как attach -U): после перезапуска симулятора порт
       свободен сразу, хотя старые соединения клиентов ещё в TIME-WAIT. */
    sim_switches |= SWMASK('U');
    r = tmxr_attach(&disp_desc, &disp_unit[NUM_DISPLAYS], cptr);
    if (r == SCPE_OK)
        sim_clock_coschedule(&disp_unit[NUM_DISPLAYS], 0);
    return r;
}

static t_stat disp_detach(UNIT *u)
{
    int n = (int)(u - disp_unit);

    if (n >= 0 && n < NUM_DISPLAYS && (u->flags & UNIT_ATT))
        return detach_unit(u);          /* файл печати ЕС-7934 */
    sim_cancel(&disp_unit[NUM_DISPLAYS]);
    return tmxr_detach(&disp_desc, &disp_unit[NUM_DISPLAYS]);
}

static t_stat disp_set_cp(UNIT *u, int32 val, CONST char *cptr, void *desc)
{
    if (!cptr)
        return SCPE_ARG;
    if (strcasecmp(cptr, "CP1025") == 0)
        disp_cp = 0;
    else if (strcasecmp(cptr, "CP880") == 0)
        disp_cp = 1;
    else
        return SCPE_ARG;
    return SCPE_OK;
}

static t_stat disp_show_cp(FILE *st, UNIT *u, int32 v, CONST void *desc)
{
    fprintf(st, "codepage=%s", disp_cp ? "cp880" : "cp1025");
    return SCPE_OK;
}

static t_stat disp_show_conn(FILE *st, UNIT *u, int32 v, CONST void *desc)
{
    int n, any = 0;

    for (n = 0; n < NUM_DISPLAYS; n++)
        if (disp_line[n].conn) {
            fprintf(st, "%sdisplay%d: %s%s", any ? ", " : "", n,
                    disp_line[n].ipad ? disp_line[n].ipad : "?",
                    disp[n].ready ? "" : " (переговоры)");
            any = 1;
        }
    if (!any)
        fprintf(st, "нет подключений");
    return SCPE_OK;
}
