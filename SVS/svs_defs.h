/*
 * SVS simulator definitions
 *
 * Copyright (c) 2009-2017, Serge Vakulenko
 * Copyright (c) 2009-2017, Leonid Broukhis
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
#ifndef _SVS_DEFS_H_
#define _SVS_DEFS_H_    0

#include "sim_defs.h"
#include <setjmp.h>
#include <inttypes.h>

/*
 * Memory.
 */
#define NREGS           30              /* number of registers-modifiers */
#define MEMSIZE         (1024 * 1024)   /* memory size, words */

/*
 * Simulator stop codes
 */
enum {
    STOP_STOP = 1,                      /* Останов */
    STOP_IBKPT,                         /* Точка останова */
    STOP_RWATCH,                        /* Точка останова по считыванию */
    STOP_WWATCH,                        /* Точка останова по записи */
    STOP_RUNOUT,                        /* Выход за пределы памяти */
    STOP_BADCMD,                        /* Запрещенная команда */
    STOP_INSN_CHECK,                    /* Контроль команды */
    STOP_INSN_PROT,                     /* Команда в чужом листе */
    STOP_OPERAND_PROT,                  /* Число в чужом листе */
    STOP_RAM_CHECK,                     /* Контроль числа МОЗУ */
    STOP_CACHE_CHECK,                   /* Контроль числа БРЗ */
    STOP_OVFL,                          /* Переполнение АУ */
    STOP_DIVZERO,                       /* Деление на нуль */
    STOP_DOUBLE_INTR,                   /* Двойное внутреннее прерывание */
    STOP_DRUMINVDATA,                   /* Чтение неформатированного барабана */
    STOP_DISKINVDATA,                   /* Чтение неформатированного диска */
    STOP_INSN_ADDR_MATCH,               /* Останов по КРА */
    STOP_LOAD_ADDR_MATCH,               /* Останов по считыванию */
    STOP_STORE_ADDR_MATCH,              /* Останов по записи */
    STOP_UNIMPLEMENTED,                 /* Не реализовано */
};

/*
 * Разряды машинного слова, справа налево, начиная с 1.
 */
#define BBIT(n)         (1 << (n-1))            /* один бит, от 1 до 32 */
#define BIT40           000010000000000000LL    /* 40-й бит - старший разряд мантиссы */
#define BIT41           000020000000000000LL    /* 41-й бит - знак */
#define BIT42           000040000000000000LL    /* 42-й бит - дубль-знак в мантиссе */
#define BIT48           004000000000000000LL    /* 48-й бит - знак порядка */
#define BIT49           010000000000000000LL    /* бит 49 */
#define BITS(n)         (~0U >> (32-n))         /* маска битов n..1 */
#define BITS40          00017777777777777LL     /* биты 40..1 - мантисса */
#define BITS41          00037777777777777LL     /* биты 41..1 - мантисса и знак */
#define BITS42          00077777777777777LL     /* биты 42..1 - мантисса и оба знака */
#define BITS48          07777777777777777LL     /* биты 48..1 */
#define BITS48_42       07740000000000000LL     /* биты 48..42 - порядок */
#define ADDR(x)         ((x) & BITS(15))        /* адрес слова */

/*
 * Работа с тегами.
 */
#define TAG_INSN48      035
#define TAG_NUMBER48    036
#define TAG_BITSET      020

#define IS_INSN48(t)    ((t) == TAG_INSN48)

#define IS_48BIT(t)     ((t) == TAG_INSN48 || (t) == TAG_NUMBER48)

/*
 * Вычисление правдоподобного времени выполнения команды,
 * зная количество тактов в УУ и среднее в АУ.
 * Предполагаем, что в 50% случаев происходит совмещение
 * выполнения, поэтому суммируем большее и половину
 * от меньшего значения.
 */
#define MEAN_TIME(x,y)  (x>y ? x+y/2 : x/2+y)

#define TICKS_PER_SEC   500         /* Fast Clock Ticks Per Second (every 2ms) */
#define INSN_PER_TICK   2000        /* Uncalibrated instructions per clock tick */

extern UNIT tty_unit[];
extern UNIT clocks[];
extern t_value memory[MEMSIZE];     /* основная память (64-битная) */
extern uint8 tag[MEMSIZE];          /* тег для каждого слова основной памяти */
extern DEVICE cpu_dev[];
extern DEVICE iom_dev[];
extern DEVICE clock_dev;
extern DEVICE tty_dev;

/*
 * Состояние одного процессора.
 */
typedef struct {
    int index;              /* номер процессора 0...3 */

    uint32 PC;              /* счётчик команд СчАС */
    uint32 RK, Aex;         /* регистр команд, исполнительный адрес */
    uint32 RAU, RUU;        /* режим АУ, режим УУ */
    t_value ACC, RMR;       /* аккумулятор, РМР */
    uint32 M[NREGS];        /* регистры-модификаторы */

    /*
     * 64-битные регистры RP0-RP7 - для отображения регистров приписки,
     * группами по 4 ради компактности, 12 бит на страницу.
     * TLB0-TLB31 - постраничные регистры приписки, копии RPi.
     * Обращение к памяти должно вестись через TLBi.
     */
    t_value RP[8];          /* РП, регистры приписки страниц пользователя */
    t_value RPS[8];         /* РПС, регистры приписки страниц супервизора */
    uint32 UTLB[32];        /* они же постранично, пользователя */
    uint32 STLB[32];        /* они же постранично, супервизора */
    uint32 RZ;              /* РЗ, регистр защиты */

    uint8 TagR;             /* регистр тега */
    t_value RPR;            /* РПР: регистр внутренних прерываний */
    uint32 GRVP;            /* ГРВП: главный регистр внешних прерываний */
    uint32 GRM;             /* ГРМ: главный регистр маски */

    t_value PP, OPP;        /* ПП, ОПП */
    t_value POP, OPOP;      /* ПОП, ОПОП */
    t_value RKP;            /* РКП */

    t_value pult[8];        /* тумблерные регистры */

    jmp_buf exception;      /* прерывание */
    uint32 bad_addr;        /* адрес, вызвавший прерывание */
    int corr_stack;         /* коррекция стека при прерывании */

    int mpd_data;           /* данные для передачи в МПД */
    int mpd_nbits;          /* счётчик битов */
} CORE;

#ifndef NUM_CORES
#   define NUM_CORES 4              /* default 4 processors */
#else
#   if NUM_CORES > 4
#      error "SVS can have up to 4 processors"
#   endif
#endif

extern CORE cpu_core[];             /* state of processor 0 */

/*
 * Состояние одного ПВВ.
 */
typedef struct {
    int index;              /* номер ПВВ 0...3 */
    uint32 HA;              /* базовый адрес */
    uint32 UTA;             /* адрес таблицы устройств */
    uint32 IOQA;            /* адрес таблицы запросов */
    uint32 SQA;             /* адрес таблицы ответов */
} IOMDATA;

extern IOMDATA iom_data[4];         /* состояние ПВВ */

/*
 * Четыре режима трассировки.
 */
typedef enum {
    TRACE_NONE = 0,
    TRACE_EXTRACODES,               /* только экстракоды (кроме э75) */
    TRACE_INSTRUCTIONS,             /* только команды процессора */
    TRACE_ALL,                      /* команды, регистры и обращения к памяти */
} TRACEMODE;

extern TRACEMODE svs_trace;

/*
 * Разряды режима АУ.
 */
#define RAU_NORM_DISABLE        001     /* блокировка нормализации */
#define RAU_ROUND_DISABLE       002     /* блокировка округления */
#define RAU_LOG                 004     /* признак логической группы */
#define RAU_MULT                010     /* признак группы умножения */
#define RAU_ADD                 020     /* признак группы слодения */
#define RAU_OVF_DISABLE         040     /* блокировка переполнения */

#define RAU_MODE                (RAU_LOG | RAU_MULT | RAU_ADD)
#define SET_MODE(x,m)           (((x) & ~RAU_MODE) | (m))
#define SET_LOGICAL(x)          (((x) & ~RAU_MODE) | RAU_LOG)
#define SET_MULTIPLICATIVE(x)   (((x) & ~RAU_MODE) | RAU_MULT)
#define SET_ADDITIVE(x)         (((x) & ~RAU_MODE) | RAU_ADD)
#define IS_LOGICAL(x)           (((x) & RAU_MODE) == RAU_LOG)
#define IS_MULTIPLICATIVE(x)    (((x) & (RAU_ADD | RAU_MULT)) == RAU_MULT)
#define IS_ADDITIVE(x)          ((x) & RAU_ADD)

/*
 * Искусственный регистр режимов УУ, в реальной машине отсутствует.
 */
#define RUU_CHECK_RIGHT         000001  /* ПКП - признак контроля правой половины */
#define RUU_CHECK_LEFT          000002  /* ПКЛ - признак контроля левой половины */
#define RUU_EXTRACODE           000004  /* РежЭ - режим экстракода */
#define RUU_INTERRUPT           000010  /* РежПр - режим прерывания */
#define RUU_MOD_RK              000020  /* ПрИК - модификация регистром М[16] */
#define RUU_AVOST_DISABLE       000040  /* БРО - блокировка режима останова */
#define RUU_RIGHT_INSTR         000400  /* ПрК - признак правой команды */

#define IS_SUPERVISOR(x)        ((x) & (RUU_EXTRACODE | RUU_INTERRUPT))
#define SET_SUPERVISOR(x,m)     (((x) & ~(RUU_EXTRACODE | RUU_INTERRUPT)) | (m))

/*
 * Специальные регистры.
 */
#define MOD     020     /* модификатор адреса */
#define PSW     021     /* режимы УУ */
#define SPSW    027     /* упрятывание режимов УУ */
#define ERET    032     /* адрес возврата из экстракода */
#define IRET    033     /* адрес возврата из прерывания */
#define IBP     034     /* адрес прерывания по выполнению */
#define DWP     035     /* адрес прерывания по чтению/записи */

/*
 * Регистр 021: режимы УУ.
 * PSW: program status word.
 */
#define PSW_MMAP_DISABLE        000001  /* БлП - блокировка приписки */
#define PSW_PROT_DISABLE        000002  /* БлЗ - блокировка защиты */
#define PSW_INTR_HALT           000004  /* ПоП - признак останова при
                                           любом внутреннем прерывании */
#define PSW_CHECK_HALT          000010  /* ПоК - признак останова при
                                           прерывании по контролю */
#define PSW_WRITE_WATCH         000020  /* Зп(М29) - признак совпадения адреса
                                           операнда прии записи в память
                                           с содержанием регистра М29 */
#define PSW_INTR_DISABLE        002000  /* БлПр - блокировка внешнего прерывания */

/*
 * Регистр 027: сохранённые режимы УУ.
 * SPSW: saved program status word.
 */
#define SPSW_MMAP_DISABLE       000001  /* БлП - блокировка приписки */
#define SPSW_PROT_DISABLE       000002  /* БлЗ - блокировка защиты */
#define SPSW_EXTRACODE          000004  /* РежЭ - режим экстракода */
#define SPSW_INTERRUPT          000010  /* РежПр - режим прерывания */
#define SPSW_MOD_RK             000020  /* ПрИК(РК) - на регистр РК принята
                                           команда, которая должна быть
                                           модифицирована регистром М[16] */
#define SPSW_MOD_RR             000040  /* ПрИК(РР) - на регистре РР находится
                                           команда, выполненная с модификацией */
#define SPSW_RIGHT_INSTR        000400  /* ПрК - признак правой команды */
#define SPSW_NEXT_RK            001000  /* ГД./ДК2 - на регистр РК принята
                                           команда, следующая после вызвавшей
                                           прерывание */
#define SPSW_INTR_DISABLE       002000  /* БлПр - блокировка внешнего прерывания */

/*
 * Кириллица Unicode.
 */
#define CYRILLIC_CAPITAL_LETTER_A               0x0410
#define CYRILLIC_CAPITAL_LETTER_BE              0x0411
#define CYRILLIC_CAPITAL_LETTER_VE              0x0412
#define CYRILLIC_CAPITAL_LETTER_GHE             0x0413
#define CYRILLIC_CAPITAL_LETTER_DE              0x0414
#define CYRILLIC_CAPITAL_LETTER_IE              0x0415
#define CYRILLIC_CAPITAL_LETTER_ZHE             0x0416
#define CYRILLIC_CAPITAL_LETTER_ZE              0x0417
#define CYRILLIC_CAPITAL_LETTER_I               0x0418
#define CYRILLIC_CAPITAL_LETTER_SHORT_I         0x0419
#define CYRILLIC_CAPITAL_LETTER_KA              0x041a
#define CYRILLIC_CAPITAL_LETTER_EL              0x041b
#define CYRILLIC_CAPITAL_LETTER_EM              0x041c
#define CYRILLIC_CAPITAL_LETTER_EN              0x041d
#define CYRILLIC_CAPITAL_LETTER_O               0x041e
#define CYRILLIC_CAPITAL_LETTER_PE              0x041f
#define CYRILLIC_CAPITAL_LETTER_ER              0x0420
#define CYRILLIC_CAPITAL_LETTER_ES              0x0421
#define CYRILLIC_CAPITAL_LETTER_TE              0x0422
#define CYRILLIC_CAPITAL_LETTER_U               0x0423
#define CYRILLIC_CAPITAL_LETTER_EF              0x0424
#define CYRILLIC_CAPITAL_LETTER_HA              0x0425
#define CYRILLIC_CAPITAL_LETTER_TSE             0x0426
#define CYRILLIC_CAPITAL_LETTER_CHE             0x0427
#define CYRILLIC_CAPITAL_LETTER_SHA             0x0428
#define CYRILLIC_CAPITAL_LETTER_SHCHA           0x0429
#define CYRILLIC_CAPITAL_LETTER_HARD_SIGN       0x042a
#define CYRILLIC_CAPITAL_LETTER_YERU            0x042b
#define CYRILLIC_CAPITAL_LETTER_SOFT_SIGN       0x042c
#define CYRILLIC_CAPITAL_LETTER_E               0x042d
#define CYRILLIC_CAPITAL_LETTER_YU              0x042e
#define CYRILLIC_CAPITAL_LETTER_YA              0x042f
#define CYRILLIC_SMALL_LETTER_A                 0x0430
#define CYRILLIC_SMALL_LETTER_BE                0x0431
#define CYRILLIC_SMALL_LETTER_VE                0x0432
#define CYRILLIC_SMALL_LETTER_GHE               0x0433
#define CYRILLIC_SMALL_LETTER_DE                0x0434
#define CYRILLIC_SMALL_LETTER_IE                0x0435
#define CYRILLIC_SMALL_LETTER_ZHE               0x0436
#define CYRILLIC_SMALL_LETTER_ZE                0x0437
#define CYRILLIC_SMALL_LETTER_I                 0x0438
#define CYRILLIC_SMALL_LETTER_SHORT_I           0x0439
#define CYRILLIC_SMALL_LETTER_KA                0x043a
#define CYRILLIC_SMALL_LETTER_EL                0x043b
#define CYRILLIC_SMALL_LETTER_EM                0x043c
#define CYRILLIC_SMALL_LETTER_EN                0x043d
#define CYRILLIC_SMALL_LETTER_O                 0x043e
#define CYRILLIC_SMALL_LETTER_PE                0x043f
#define CYRILLIC_SMALL_LETTER_ER                0x0440
#define CYRILLIC_SMALL_LETTER_ES                0x0441
#define CYRILLIC_SMALL_LETTER_TE                0x0442
#define CYRILLIC_SMALL_LETTER_U                 0x0443
#define CYRILLIC_SMALL_LETTER_EF                0x0444
#define CYRILLIC_SMALL_LETTER_HA                0x0445
#define CYRILLIC_SMALL_LETTER_TSE               0x0446
#define CYRILLIC_SMALL_LETTER_CHE               0x0447
#define CYRILLIC_SMALL_LETTER_SHA               0x0448
#define CYRILLIC_SMALL_LETTER_SHCHA             0x0449
#define CYRILLIC_SMALL_LETTER_HARD_SIGN         0x044a
#define CYRILLIC_SMALL_LETTER_YERU              0x044b
#define CYRILLIC_SMALL_LETTER_SOFT_SIGN         0x044c
#define CYRILLIC_SMALL_LETTER_E                 0x044d
#define CYRILLIC_SMALL_LETTER_YU                0x044e
#define CYRILLIC_SMALL_LETTER_YA                0x044f

/*
 * Процедуры работы с памятью
 */
extern void mmu_store(CORE *cpu, int addr, t_value word);
extern void mmu_store64(CORE *cpu, int addr, t_value word);
extern t_value mmu_load(CORE *cpu, int addr);
extern t_value mmu_load64(CORE *cpu, int addr, int tag_check);
extern t_value mmu_fetch(CORE *cpu, int addr, int *paddrp);
extern void mmu_set_rp(CORE *cpu, int idx, t_value word, int supervisor);
extern void mmu_setup(CORE *cpu);
extern void mmu_set_protection(CORE *cpu, int idx, t_value word);

/*
 * Utility functions
 */
extern void gost_putc(unsigned char, FILE *);
extern int odd_parity(unsigned char);

/*
 * Терминалы.
 */
void tty_send(uint32 mask);
int tty_query(void);
void vt_print(void);
void tt_print(void);
void vt_receive(CORE *cpu);
int vt_is_idle(void);

/*
 * МПД.
 */
void mpd_reset(CORE *cpu);
void mpd_send_nibble(CORE *cpu, int data);
void mpd_receive_update(CORE *cpu);

/*
 * Отладочная выдача.
 */
void svs_fprint_cmd(FILE *of, uint32 cmd);
void svs_fprint_insn(FILE *of, uint32 insn);
void svs_log(const char *fmt, ...);
void svs_log_cont(const char *fmt, ...);
void svs_debug(const char *fmt, ...);
t_stat fprint_sym(FILE *of, t_addr addr, t_value *val,
                  UNIT *uptr, int32 sw);
void svs_trace_opcode(CORE *cpu, int paddr);
void svs_trace_registers(CORE *cpu);

/*
 * Арифметика.
 */
double svs_to_ieee(t_value word);
void svs_add(CORE *cpu, t_value val, int negate_acc, int negate_val);
void svs_divide(CORE *cpu, t_value val);
void svs_multiply(CORE *cpu, t_value val);
void svs_change_sign(CORE *cpu, int sign);
void svs_add_exponent(CORE *cpu, int val);
int svs_highest_bit(t_value val);
void svs_shift(CORE *cpu, int toright);
int svs_count_ones(t_value word);
t_value svs_pack(t_value val, t_value mask);
t_value svs_unpack(t_value val, t_value mask);

/*
 * Процессор ввода-вывода.
 */
void iom_reset(int index);
void iom_request(int index);

/*
 * Разряды главного регистра прерываний (ГРП)
 * Внешние:
 */
#define RPR_WATCHDOG    00000000000002000LL /* 11 */
/* Внутренние: */
#define RPR_DIVZERO     00000000034000000LL /* 23-21 */
#define RPR_OVERFLOW    00000000014000000LL /* 22-21 */
#define RPR_CHECK       00000000004000000LL /* 21 */
#define RPR_OPRND_PROT  00000000002000000LL /* 20 */
#define RPR_WATCHPT_W   00000000000200000LL /* 17 */
#define RPR_WATCHPT_R   00000000000100000LL /* 16 */
#define RPR_INSN_CHECK  00000000000040000LL /* 15 */
#define RPR_INSN_PROT   00000000000020000LL /* 14 */
#define RPR_ILL_INSN    00000000000010000LL /* 13 */
#define RPR_BREAKPOINT  00000000000004000LL /* 12 */
#define RPR_PAGE_MASK   00000000000000760LL /* 9-5 */
#define RPR_RAM_CHECK   00000000000000010LL /* 4 */
#define RPR_BLOCK_MASK  00000000000000007LL /* 3-1 */

#define RPR_SET_BLOCK(x,m)  (((x) & ~RPR_BLOCK_MASK) | ((m) & RPR_BLOCK_MASK))
#define RPR_SET_PAGE(x,m)   (((x) & ~RPR_PAGE_MASK) | (((m)<<4) & RPR_PAGE_MASK))

/*
 * Разряды регистра внешних прерываний РВП
 */
#define GRVP_PROGRAM    0400LL
#define GRVP_REQUEST    0200LL
#define GRVP_RESPONSE   0100LL
#define GRVP_IOM_FAIL   0040LL
#define GRVP_RAM_FAIL   0020LL
#define GRVP_TIMER      0010LL
#define GRVP_INTR_IOM   0004LL
#define GRVP_MULTI      0002LL
#define GRVP_PANEL_REQ  0001LL

/*
 * Разряды регистров РКП, ПП, ОПП, ПОП, ОПОП.
 */
#define CONF_IOM_RESET  (1LL << 47)     /* бит 48: Сброс ПВВ */
#define CONF_IOM1       (1LL << 45)     /* бит 46: ПВВ 1 */
#define CONF_IOM2       (1LL << 44)     /* бит 45: ПВВ 2 */
#define CONF_IOM3       (1LL << 43)     /* бит 44: ПВВ 3 */
#define CONF_IOM4       (1LL << 42)     /* бит 43: ПВВ 4 */
#define CONF_IOM_MASK   (0xfLL << 42)   /* биты 43-46: биты ПВВ */
#define CONF_CPU_MASK   (0xfLL << 38)   /* биты 39-42: биты процесоров СВС */
#define CONF_DATA_MASK  (0xfLL << 34)   /* биты 35-38: данные МПД */
#define CONF_MR         (1LL << 33)     /* бит 34: приём МПД */
#define CONF_MT         (1LL << 32)     /* бит 33: передача МПД */

#define CONF_GET_DATA(x)    (((x) >> 34) & 0xf)
#define CONF_SET_DATA(r,x)  (((r) & ~CONF_DATA_MASK) | (((x) & 0xfLL) << 34))

#endif
