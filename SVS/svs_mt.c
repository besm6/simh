/*
 * SVS magnetic tape drives (НМЛ ЕС-5012/5017) on the ES channel.
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
 */
#include "svs_defs.h"
#include "sim_tape.h"
#include <ctype.h>

/*
 * Ленты (МЛ) Диспака СВС — накопители ЕС на ЕС-канале (см. ПВВ.md, МЛ):
 * ТУС 040-047 на канале Х'0C' (МЛ 30-37) и ТУС 060-067 на Х'0D' (МЛ 40-47),
 * адап.bemsh:706-711. Юниты MT0-MT7 — канал Х'0C', MT8-MT15 — Х'0D'
 * (ЛУ 030-033 -> MT0-MT3, 040-047 -> MT8-MT15; ДАЙНУС, адап.bemsh:3473).
 * Образ — SIMH .tap (sim_tape); запись на ленте — ровно байты, которые
 * передаёт канал.
 *
 * КОП (таблица ТОП, адап.bemsh:1192-1206) — стандартные канальные команды
 * ленты IBM (ПВВ.md §7Б.14):
 *   01 запись, 02 чтение, 03 холостой ход (опрос готовности), 07 перемотка,
 *   17 стирание промежутка, 1F ленточная метка, 27/37 блок назад/вперёд,
 *   2F/3F файл назад/вперёд, C3/CB/FB — плотность.
 * В ТОП комментарии у 27/37 («ШАГ НА ФАЙЛ») и 2F/3F («ШАГ НА ЗОНУ»)
 * перепутаны местами; «зона» у 2F/3F верна лишь потому, что каждая зона на
 * ленте — отдельный файл (см. разметку ниже).
 *
 * Упаковка (поле ТГГ слова СО, разр.40-38): 6 — «короткие слова», 6 байт
 * на слово (48 разрядов, ЕС-формат ленты); иначе (4, БЭСМ-формат зон) —
 * 8 байт на слово: всё 64-разрядное слово памяти, при чтении тег
 * «битовый набор» (ЗАПАК/РАСПАК в есмл.bemsh сами пакуют слова БЭСМ).
 *
 * Ответ (ЕСМЛ, ВХДРМЛ/ГОТМГ/Н1348/ПРК4, есмл.bemsh:869-1218, 1558-1595):
 *   ДР мл16: НУС<<2, 0x1000 — есть ДРУ, 0x2000 — ВУН («ВУ не найдено»:
 *     ГОТМГ и опрос ОПР считают это выключенным контроллером и печатают
 *     «ВЫКЛ КОНТ-Р»). Лента без образа — привод есть, но не готов: БНС +
 *     ДРУ с «требуется вмешательство» и без «готов» (разр.39);
 *   ДР значение: разр.4,3 — КК, ВУК (конец работы); разр.1 — ОСУ (ленточная
 *     метка при чтении/пропуске); разр.2 — СБУ (сбой: нет кольца записи,
 *     конец записанной части ленты — тогда и «сбой данных» в ДРУ);
 *     разр.44 — прочли меньше заказанного, разр.47-45 — КСПС, разр.30-21 —
 *     последний адрес (число слов + 1);
 *   ДРУ (только при особом состоянии: шаг назад от начала ленты, конец
 *   записанной части, нет кольца, ошибка образа): разр.44 — сбой данных,
 *   39 — готов, 36 — начало ленты, 34 — нет кольца записи.
 *
 * Разметка (как mg_attach в BESM6/besm6_mg.c): `attach -n MTn …/0123.tap`,
 * а также attach несуществующего файла, создаёт образ и размечает его —
 * номер бобины берётся из самой правой группы цифр в имени файла (1..2047).
 * Формат — тот, что пишет ЕСМЛ при дисковой упаковке без дублирования
 * (УПНОВ=1, БЕЗДУБ=1): блок ДОМЛМД из 784 слов (адап.bemsh:1327) — СС[0..7],
 * 8 пустых слов и 768 слов упакованной зоны; данные нулевые, поэтому и
 * контрольная сумма СС[7] = 0. Зона z — блок z от начала ленты:
 *   СС[2] = бобина<<30 | бобина | 0777<<15 | флаги (ПРОБА1, есмл.bemsh:1627);
 *   СС[3] = СС[6] = 070707<<24 | z<<12 | z (ключ и зона, дмлмб.bemsh:1243).
 * Каждая зона — отдельный «файл»: метка, зона, метка, зона, …, метка, метка
 * (опознание от начала ленты ждёт метку перед зоной 0, а по зонам ЕСМЛ ходит
 * пропуском файла 2F/3F). Зон 2048 (предел при БЕЗДУБ, ОГР дисп80.bemsh:734).
 */
#define NUM_MT          16
#define MT_MAXREC       (8 * 2048)          /* байт в записи, с запасом */

/* ДР, мл16 */
#define DR_BNS          0x0001
#define DR_DRU          0x1000
#define DR_VUN          0x2000
/* ДР, 48-разрядное значение */
#define DR_OSU          (1LL << 0)          /* особый случай: метка */
#define DR_SBU          (1LL << 1)          /* сбой в устройстве */
#define DR_VUK          (1LL << 2)          /* конец работы устройства */
#define DR_KK           (1LL << 3)          /* конец работы канала */
#define DR_SHORT        (1LL << 43)         /* меньше заказанного */
/* ДРУ, 48-разрядное значение */
#define DRU_INTERV      (1LL << 46)         /* разр.47: требуется вмешательство */
#define DRU_DATACHK     (1LL << 43)         /* разр.44: сбой данных */
#define DRU_READY       (1LL << 38)         /* разр.39 */
#define DRU_LOADPT      (1LL << 35)         /* разр.36: начало ленты */
#define DRU_NORING      (1LL << 33)         /* разр.34: нет кольца записи */

#define MT_UNIT  { UDATA(NULL, UNIT_ATTABLE | UNIT_ROABLE | UNIT_DISABLE, 0) }
UNIT mt_unit[NUM_MT] = {
    MT_UNIT, MT_UNIT, MT_UNIT, MT_UNIT, MT_UNIT, MT_UNIT, MT_UNIT, MT_UNIT,
    MT_UNIT, MT_UNIT, MT_UNIT, MT_UNIT, MT_UNIT, MT_UNIT, MT_UNIT, MT_UNIT,
};

static REG mt_reg[] = {
    { 0 }
};

static MTAB mt_mod[] = {
    { MTAB_XTD | MTAB_VUN, 0, "FORMAT", "FORMAT",
      &sim_tape_set_fmt, &sim_tape_show_fmt, NULL, "формат образа ленты" },
    { 0 }
};

#define DEB_OPS 000001
#define DEB_DAT 000040

static DEBTAB mt_deb[] = {
    { "OPS",  DEB_OPS, "команды" },
    { "DATA", DEB_DAT, "данные записей" },
    { NULL, 0 }
};

static t_stat mt_reset(DEVICE *dptr);
static t_stat mt_attach(UNIT *u, CONST char *cptr);
static t_stat mt_detach(UNIT *u);

DEVICE mt_dev = {
    "MT", mt_unit, mt_reg, mt_mod,
    NUM_MT, 8, 24, 1, 8, 8,
    NULL, NULL, &mt_reset, NULL, &mt_attach, &mt_detach,
    NULL, DEV_DISABLE | DEV_DEBUG | DEV_TAPE, 0, mt_deb
};

static uint8 mt_buf[MT_MAXREC];

/*
 * Лента сошла с начала: любое движение вперёд, даже по чистой ленте, где
 * позиция образа не меняется. Иначе ЕСМЛ при опознании (ПРК3) видит «начало
 * ленты» после каждого шага вперёд и шагает снова. Гасится перемоткой и
 * шагом назад, дошедшим до начала.
 */
static int mt_offbot[NUM_MT];

static t_stat mt_reset(DEVICE *dptr)
{
    return SCPE_OK;
}

#define MT_ZONES        2048                /* зон на размеченной ленте */
#define MT_BLKWORDS     01420               /* слов в блоке (ДОМЛМД) */
#define MT_KEY          070707LL            /* ключ зоны в СС[3], СС[6] */
#define MT_FLAGS        (1LL << 46 | 1LL << 45 | 1LL << 28 | 1LL << 27) /* БЕЗДУБ, УПНОВ */

/*
 * Разметка ленты бобины reel: все зоны с СС и нулевыми данными, две метки,
 * перемотка.
 */
static t_stat mt_format(UNIT *u, int reel)
{
    t_value blk[MT_BLKWORDS];
    t_stat r = MTSE_OK;
    int z, i, k, n;

    sim_messagef(SCPE_OK, "%s: formatting tape volume %d\n", sim_uname(u), reel);
    memset(blk, 0, sizeof(blk));
    blk[1] = (t_value)0x987654321000LL << 16;   /* шифр задачи разметки */
    blk[2] = ((t_value)reel << 30 | reel | 0777LL << 15 | MT_FLAGS) << 16;
    /*
     * Каждой зоне предшествует метка: опознание от начала ленты (ПРК3/ПРК4,
     * есмл.bemsh:995-1040) шагает на блок вперёд и читает СС следующей
     * зоны, только если прошло метку (ОСУ, «стоим перед зоной»); пройдя
     * обычный блок, оно возвращается, ничего не прочитав. А по зонам ЕСМЛ
     * ходит пропуском файла (2F/3F, «ШАГ НА ЗОНУ» в ТОП), то есть зоны
     * разделены метками.
     */
    r = sim_tape_wrtmk(u);
    for (z = 0; z < MT_ZONES && r == MTSE_OK; ++z) {
        blk[3] = blk[6] = (MT_KEY << 24 | (t_value)z << 12 | z) << 16;
        for (i = n = 0; i < MT_BLKWORDS; ++i)
            for (k = 7; k >= 0; --k)
                mt_buf[n++] = (uint8)(blk[i] >> (8 * k));
        r = sim_tape_wrrecf(u, mt_buf, n);
        if (r == MTSE_OK)
            r = sim_tape_wrtmk(u);      /* зона — отдельный «файл» */
    }
    if (r == MTSE_OK)
        r = sim_tape_wrtmk(u);          /* вторая метка — конец записи */
    sim_tape_rewind(u);
    return (r == MTSE_OK) ? SCPE_OK : SCPE_IOERR;
}

/*
 * Номер бобины — самая правая группа цифр в имени файла (без каталога и
 * расширения): «/tmp/0123.tap» -> 123. 0 — цифр нет.
 */
static int mt_reel_from_name(UNIT *u)
{
    char *name = sim_filepath_parts(u->filename, "n");
    char *pos = name + strlen(name);
    int reel;

    while (pos > name && !isdigit((unsigned char)*--pos))
        ;
    while (pos > name && isdigit((unsigned char)pos[-1]))
        --pos;
    reel = isdigit((unsigned char)*pos) ? (int)strtoul(pos, NULL, 10) : 0;
    free(name);
    return reel;
}

static t_stat mt_attach(UNIT *u, CONST char *cptr)
{
    int32 saved_switches = sim_switches;
    t_stat r;

    mt_offbot[u - mt_unit] = 0;
    sim_switches |= SWMASK('E');
    for (;;) {
        r = sim_tape_attach(u, cptr);
        if (r == SCPE_OK && (sim_switches & SWMASK('N'))) {
            int reel = mt_reel_from_name(u);

            if (reel < 1 || reel >= 2048) {
                char *fname = strdup(u->filename);

                r = sim_messagef(SCPE_ARG, "%s: в имени файла нужен номер"
                    " бобины 1..2047 (%s)\n", sim_uname(u), cptr);
                sim_tape_detach(u);
                remove(fname);
                free(fname);
                return r;
            }
            r = mt_format(u, reel);
            break;
        }
        if (r == SCPE_OK || (saved_switches & SWMASK('E')) ||
            (sim_switches & SWMASK('N')))
            break;
        sim_switches |= SWMASK('N');    /* файла нет — создать и разметить */
    }
    sim_switches = saved_switches;
    return r;
}

static t_stat mt_detach(UNIT *u)
{
    return sim_tape_detach(u);
}

/*
 * Массив памяти -> байты записи: bpw байт на слово (8 — всё слово,
 * 6 — 48 разрядов), extra — байтов в неполном последнем слове (6-байтный
 * режим, НПС).
 */
static int mt_fetch(int memaddr, int nwords, int bpw, int extra)
{
    int n = 0, i, k, total = nwords * bpw + extra;

    if (total > MT_MAXREC)
        total = MT_MAXREC;
    for (i = 0; n < total; ++i) {
        t_value w = memory[mmu_iom_data_pa(memaddr + i)];

        if (bpw == 6)
            w = (w >> 16) & BITS48;
        for (k = bpw - 1; k >= 0 && n < total; --k)
            mt_buf[n++] = (uint8)(w >> (8 * k));
    }
    return n;
}

/*
 * Байты записи -> память; слов не больше заказанных. Возвращает число
 * затронутых слов (неполное последнее — тоже).
 */
static int mt_store(int memaddr, int nwords, int bpw, int nbytes)
{
    int w, k, n = 0;

    for (w = 0; w < nwords && n < nbytes; ++w) {
        t_value v = 0;
        int pa = mmu_iom_data_pa(memaddr + w);

        for (k = 0; k < bpw; ++k)
            v = (v << 8) | ((n < nbytes) ? mt_buf[n++] : 0);
        if (bpw == 6) {
            memory[pa] = v << 16;
            tag[pa] = TAG_NUMBER48;
        } else {
            memory[pa] = v;
            tag[pa] = TAG_BITSET;
        }
    }
    return w;
}

static void mt_dump(int num, const char *what, int nbytes)
{
    int i;

    if (!sim_deb || !(mt_dev.dctrl & DEB_DAT))
        return;
    fprintf(sim_deb, "MT%d DATA %s %d байт:", num, what, nbytes);
    for (i = 0; i < nbytes && i < 64; ++i)
        fprintf(sim_deb, " %02X", mt_buf[i]);
    fprintf(sim_deb, nbytes > 64 ? " ...\n" : "\n");
}

/*
 * Обмен с лентой num по команде kop. nwords/nps — РАЗМ и НПС из ДО,
 * ttg — поле ТГГ слова СО. Ответ — в *st (ДР/ДРУ кладёт svs_iom.c).
 */
t_stat svs_mt_io(int num, int kop, int memaddr, int nwords, int nps, int ttg,
                 IOM_ES_STATUS *st)
{
    UNIT *u;
    t_mtrlnt bc = 0;
    t_stat r = MTSE_OK;
    uint32 skipped;
    int bpw = (ttg == 6) ? 6 : 8;
    int extra = (bpw == 6) ? nps : 0;
    int want, words;

    st->dr48 = DR_VUK | DR_KK;
    st->drlow = 0;
    st->dru48 = 0;
    if (num < 0 || num >= NUM_MT)
        return SCPE_NXUN;
    u = &mt_unit[num];
    if (!(u->flags & UNIT_ATT)) {
        st->dr48 = 0;
        st->drlow = DR_BNS | DR_DRU;    /* не подключён: привод не готов */
        st->dru48 = DRU_INTERV;
        sim_debug(DEB_OPS, &mt_dev, "МЛ%d: КОП=%02X — не подключена\n", num, kop);
        return SCPE_OK;
    }

    switch (kop) {
    case 0x03:                          /* холостой ход */
        break;
    case 0x07:                          /* перемотка */
    case 0x0F:                          /* перемотка с разгрузкой */
        r = sim_tape_rewind(u);
        mt_offbot[num] = 0;
        break;
    case 0x17:                          /* стирание промежутка */
        break;
    case 0x1F:                          /* ленточная метка */
        r = sim_tape_wrtmk(u);
        break;
    case 0x37:                          /* блок вперёд */
        r = sim_tape_sprecf(u, &bc);
        break;
    case 0x27:                          /* блок назад */
        r = sim_tape_sprecr(u, &bc);
        break;
    case 0x3F:                          /* файл вперёд */
        r = sim_tape_spfilef(u, 1, &skipped);
        break;
    case 0x2F:                          /* файл назад */
        r = sim_tape_spfiler(u, 1, &skipped);
        break;
    case 0x01:                          /* запись */
        want = mt_fetch(memaddr, nwords, bpw, extra);
        mt_dump(num, "запись", want);
        r = sim_tape_wrrecf(u, mt_buf, want);
        bc = want;
        break;
    case 0x02:                          /* чтение */
        want = nwords * bpw + extra;
        r = sim_tape_rdrecf(u, mt_buf, &bc, MT_MAXREC);
        if (r != MTSE_OK)
            break;
        mt_dump(num, "чтение", bc);
        words = mt_store(memaddr, nwords, bpw, bc);
        if ((int)bc < want) {
            st->dr48 |= DR_SHORT |
                ((t_value)((bpw == 6) ? bc % 6 : 0) << 44) |
                ((t_value)((words + 1) & 01777) << 20);
        }
        break;
    default:                            /* плотность и прочее */
        break;
    }

    /*
     * Назад в начало ленты: если лента сходила с начала (шли вперёд по
     * чистому месту), это нормальный конец шага; «начало ленты» — только
     * когда шаг назад выдан, уже стоя в начале.
     */
    if ((kop == 0x27 || kop == 0x2F) && r == MTSE_BOT && mt_offbot[num]) {
        r = MTSE_OK;
        mt_offbot[num] = 0;
    }
    switch (kop) {                      /* движение вперёд — сошли с начала */
    case 0x01: case 0x02: case 0x1F: case 0x37: case 0x3F:
        mt_offbot[num] = 1;
        break;
    case 0x27: case 0x2F:
        if (sim_tape_bot(u))
            mt_offbot[num] = 0;
        break;
    }

    /*
     * ДРУ (уточнённое состояние) — только при особом состоянии, как его
     * выдаёт канал: ЕСМЛ после каждой операции опознания смотрит «начало
     * ленты» в ДРУ (ПРК3) и при нём снова шагает вперёд.
     */
    switch (r) {
    case MTSE_OK:
        break;
    case MTSE_TMK:                      /* метка: особый случай, без ДРУ */
        st->dr48 |= DR_OSU;
        break;
    case MTSE_BOT:                      /* шаг назад от начала */
        st->dr48 |= DR_SBU;             /* сбой в устройстве, как у 3420 */
        st->drlow = DR_DRU;
        st->dru48 = DRU_READY | DRU_LOADPT;
        break;
    case MTSE_EOM:
        /*
         * Дальше ничего не записано (чистая лента): сбой в устройстве и
         * «сбой данных» в ДРУ, а не метка — на метку ЕСМЛ при опознании
         * (ПРК4) шагает дальше бесконечно.
         */
        st->dr48 |= DR_SBU;
        st->drlow = DR_DRU;
        st->dru48 = DRU_READY | DRU_DATACHK;
        break;
    case MTSE_WRP:                      /* нет кольца записи */
    default:                            /* ошибка формата/ввода-вывода */
        st->dr48 |= DR_SBU;
        st->drlow = DR_DRU;
        st->dru48 = DRU_READY | (r == MTSE_WRP ? 0 : DRU_DATACHK);
        break;
    }
    if (st->drlow & DR_DRU) {
        if (sim_tape_wrp(u))
            st->dru48 |= DRU_NORING;
        if (sim_tape_bot(u) && !mt_offbot[num])
            st->dru48 |= DRU_LOADPT;
    }

    sim_debug(DEB_OPS, &mt_dev,
        "МЛ%d: КОП=%02X слов=%d ТГГ=%o адрес=%o -> %d байт, sim_tape %d, "
        "ДР=%016jo ДРУ=%016jo, позиция %ju\n", num, kop, nwords, ttg, memaddr,
        (int)bc, (int)r, (uintmax_t)st->dr48, (uintmax_t)st->dru48,
        (uintmax_t)u->pos);
    return SCPE_OK;
}
