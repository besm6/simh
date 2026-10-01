#!/usr/bin/env python3
"""
Рабочий образ системного диска СВС из дистрибутивного svs2053.bin.

Дистрибутивный образ не загружается как есть: два поля служебных слов зоны
записаны не в том соглашении, которого ждут первичный загрузчик и ОС.
Скрипт делает КОПИЮ, правит в ней эти два поля, выключает архив и ставит
год;
дистрибутив открывается только на чтение.

Разметка служебных слов зоны (диски.bemsh, шапки «0-СС» и «1-СС»).
Слова 0-3 описывают зону, слова 4-7 — их копия:

    СС[0], СС[4]   разр.48-37 - НОМЕР ЗОНЫ
    СС[1], СС[5]   разр.43    - контроль номера дорожки
                   разр.41    - новая контрольная сумма
                   разр.40    - СС расписаны по-новому
                   разр.39-25 - КЛЮЧ СС (код 070707)
                   разр.24-13 - НОМЕР ПАКЕТА
                   разр.12-1  - контрольная сумма 'сектора'
    СС[3], СС[7]   контрольная сумма зоны (по словам данных); меняется
                   только в зоне параметров ГЕНС-а (п.3, 4)

1. НОМЕР ПАКЕТА (СС[1], СС[5]).  ВЫЗПВВ сверяет служебное слово строже, чем
   ГЕНС:

       2252  сч 0021 / и D02373      маска 0007777777770000 (разр.39-13)
       2253  нтж D02017              эталон 0007070740050000
             (не сошлось -> ПЛОХО, зона отвергается)

   Ключ 070707 совпадает, расходится номер пакета: в образе 04151 (2153),
   ВЫЗПВВ ждёт 04005 (2053) - то самое число, что стоит в имени svs2053.bin.
   Значение одинаково во всех служебных словах образа.  ГЕНС этого не видит
   (маскирует только разр.39-25, один ключ), ДИСКИ сверяет номер пакета со
   своим же ТЗНМД (ОШNПАК) и принимает любой.

2. НОМЕР ЗОНЫ (СС[0], СС[4]).  ДИСКИ сам пишет его при записи зоны
   (диски.bemsh, ОБ4А):

       СЧ КУС2-1(М16) / И Е36П25 / СДА 64-12 / ИЛИ (М10) / ЗП (М10) / ЗП 4(М10)

   то есть кладёт поле зоны из КУС2 в разр.48-37 и ОДНУ И ТУ ЖЕ величину в
   СС[0] и в СС[4].  При чтении он её оттуда достаёт и сверяет с КУС2
   (ПРЗОНЫ, физ.073503), при несовпадении - ОШЗОНЫ.  В образе же СС[0] зоны n
   равно (2n), а СС[4] - (2n+1), то есть номер УДВОЕН: для зоны 0462
   служебное слово даёт 01144 против 0462 в КУС2, и загрузка зацикливается.

3. АРХИВ (зона 0754, параметры ГЕНС-а).  ВЫЗСВС читает зону КУСПАР (0754),
   выбирает в ней вариант ГЕНС-а (блоки по 040 слов, в слове 14 - номер СВС)
   и переписывает его в ИНФО -> ПАРОС; слово 13 варианта становится ТРАКТЫ,
   а ГЕНС кладёт его в ПРЕДЕЛ.  Разряд 16 ПРЕДЕЛ - раздел генерации
   "АРХИВ ДА"; СТАТ2С запускает задачу архива только при нём:

       СЧ ПРЕДЕЛ / И Е16 / ПО АРХНЕТ

   По умолчанию Е16 НЕ трогается (как в дистрибутиве); --set-archive 0/1
   гасит/зажигает Е16 в слове 13 всех вариантов.

4. ГОД (там же, слово 15 варианта).  Слово 15 становится КГОД ("МЛ.ЦИФРА
   ГОДА, ЧИСЛО И НАЧ.N ОБЩ.ДИСК", ГЕНС.bemsh), а ГЕНС1 собирает из него ГОД:

       СЧ NБЭСМ / И Е39П1 / ИЛИ КГОД / ЗП ГОД

   Год хранится двумя десятичными цифрами: единицы - разр.24-21, десятки -
   разр.20-17.  Обе печатает сборка 2153: ГЕНС2 (ПДАТ, «ДД.ММ.ГГ») и
   директива ДАТА/ВРЕ (ПРВР), - разр.20-17 берутся как СЧ П17 / СДА 64-16 /
   И ГОД.  В вариантах образа там 9 или 8 (199x/198x), отсюда «96» при
   2026 годе.  Скрипт ставит во все варианты обе цифры года --year.

5. ЗОНА СТАТИСТИКИ (логическая зона 030 тома статистики НОММЛ - системного
   диска, в образе зона 034: физическая = логическая + 4).  В дистрибутиве
   она не размечена (нет ключа КЛЮЧСТ), и ГЕНС2 (ПДАТ1) ставит нулевую дату
   «00.00.96 00.00.00»; проверка часов в ГЕНС2 (ВЫХ777) без даты в ГОД не
   проходит.  Скрипт размечает зону так, как это делает сама ОС после
   первого прогона (СТАТ1С: ЧТЗОНЫ/РОСПИС, НАЧС30, НОРМ; СТАТ2С), на
   текущие дату и время (--year для младшей цифры года).  Слова - состав.bemsh:

       0    СПЕЦ    ГОД<<36 | ГОД&Е35П17 | Е10   (дата машинная, разр.10-1 -
                    указатель свободного слова буфера накопления = 01000)
       1    ШКРЗСТ  по 6 разр. на ЭВМ (поле n-1 для ЭВМ n): текущая зона = 030
       2    ШКЗЗСТ  Е48 - зона 030 занята; разр.6-1 - последняя выданная, 030
       015  КЛЮЧСТ  П'КЛЮЧСТ'
       016  ГОДСТ   ГОД (дата астрономическая)
       0532 СЧЕТЧ   ВРЕМЯ<<18 (ВРЕМЯ - 50 в секунду от полуночи)
       0533 ДАТГЕН  ГОД

   ГОД - как его собирает ГЕНС: КГОД (слово 15) первого варианта ГЕНС-а
   для СВС --svs без разр.12-4, № ЭВМ (слово 14) в разр.3-1, число и месяц
   десятичными цифрами в разр.36-25 (как пишет директива ДАТА).
   Остальные слова данных - нули, тег 1, как у ОС.

6. СМЕНА В МГРП.  Номер смены (разр.24-22 ячейки МГРП резидента ДИСП70,
   а не аппаратного регистра) ставит приказ СМЕ только в памяти; резидент
   при каждом вызове ОС заново читается с диска, и на диске смена 0.
   Проверка часов в ГЕНС2 (ВЫХ777) без смены не проходит.  Скрипт пишет
   смену --shift (по умолчанию 1) в МГРП на диске: адрес МГРП (01464) - из
   таблицы имён тома (зоны 0504/0742, как в svs_disk.c), слово лежит в
   странице 0 резидента, которую ГЕНС читает из логической зоны 0621
   (КУСЧТЗ, «ДИСП70,КИТ,ДИСКИ,КАЧКА») = зона образа 0625.

После правок пересчитывается контрольная сумма зоны СС[3]/СС[7] - сумма слов
данных с циклическим переносом по 48 разрядам (ПОДКС в ВЫЗСВС; иначе СТОП 204).

Подробности и замеры - ПВВ.md §7Б.9.

    python3 tools/makeSVS2053.py <дистрибутив> <копия> [--pack N] [--year Г]
                                 [--set-archive 0|1] [--zero-archive-params]
                                 [--svs N] [--no-stat-zone] [--shift N]

--pack задаёт номер пакета восьмеричным числом (по умолчанию 4005),
--year - год, в образ идут две его младшие цифры (по умолчанию текущий).
--set-archive 0 гасит разр.16 ПРЕДЕЛ (архив выключен), 1 - зажигает
   (архив включён, задача архива запускается). Без опции разряд не трогается
   (остаётся как в дистрибутиве).
--zero-archive-params обнуляет данные зоны параметров архива (phys 0755),
   сохраняя служебные слова и пересчитывая КС зоны в 0: задача архива читает
   зону как пустую («нет параметров»), а не как испорченную. Не зависит от
   --set-archive.
--svs N - номер СВС, для которой размечается зона статистики (по умолчанию 1,
   как NSVS в svs.ini); --no-stat-zone оставляет зону как в дистрибутиве.
--shift N - номер смены (0-7) в МГРП резидента на диске (по умолчанию 1);
   0 оставляет МГРП как в дистрибутиве.
"""
import argparse
import shutil
import struct
import sys
import time

WORD = 8                            # байт на слово в образе диска
ZONE_WORDS = 8 + 1024               # 8 служебных + 1024 данных
M48 = (1 << 48) - 1

KEY_MASK  = 0o77777 << 24           # разр.39-25: ключ СС
KEY       = 0o70707 << 24           # его значение в исправных словах
PACK_MASK = 0o7777 << 12            # разр.24-13: номер пакета
ZONE_MASK = 0o7777 << 36            # разр.48-37: номер зоны
PACK_DEFAULT = 0o4005               # 2053 - то, чего ждёт ВЫЗПВВ
PARAM_ZONE = 0o754                  # КУСПАР: параметры ГЕНС-а
ARCH_PARAM_ZONE = 0o755             # зона параметров архива (АРХИВ.md §8.2.3.2)
VARIANT_WORDS = 0o40                # длина варианта ГЕНС-а
VARIANT_SIG = 0o0000056000004005    # слово 0 варианта (ИНФО)
PREDEL = 13                         # слово варианта -> ТРАКТЫ -> ПРЕДЕЛ
E16 = 1 << 15                       # АРХИВ ДА
KGOD = 15                           # слово варианта -> КГОД -> ГОД
YEAR_SHIFT = 20                     # разр.24-21: единицы года
TENS_SHIFT = 16                     # разр.20-17: десятки года
YEAR_MASK = 0o17 << YEAR_SHIFT | 0o17 << TENS_SHIFT
NSVS_WORD = 14                      # слово варианта: номер СВС (NБЭСМ)
STAT_ZONE = 0o30 + 4                # зона статистики: логическая 030 + 4
STAT_KEY = 0o1242547515630462       # П'КЛЮЧСТ'
STAT_TAG = 1                        # тег слов, которые пишет ОС
SPEC, SHKRZST, SHKZZST, KLUCHST, GODST = 0, 1, 2, 0o15, 0o16
SCHETCH, DATGEN = 0o532, 0o533      # состав.bemsh
E35P17 = 0o1777777 << 16            # разр.35-17
SYM_NAMES, SYM_ADDRS = 0o504, 0o742 # таблица имён тома: имена / адреса
SYM_MGRP = 0x2c23302f0f0f           # МГРП·· (ГОСТ)
RES_ZONE = 0o621 + 4                # страница 0 резидента: логическая 0621 + 4
SHIFT_SHIFT = 21                    # разр.24-22 МГРП: номер смены


def cyclic_sum48(words):
    """Сумма с циклическим переносом по 48 разрядам (СЛЦ, ПОДКС)."""
    s = 0
    for w in words:
        s += w & M48
        s = (s & M48) + (s >> 48)
    return s


def stat_zone_words(kgod, nsvs, now):
    """Слова данных зоны статистики, как их оставляет ОС (см. п.5)."""
    mday, mon = now.tm_mday, now.tm_mon
    date = ((mday // 10) << 4 | mday % 10) << 5 | ((mon // 10) << 4 | mon % 10)
    god = (kgod & ~0o7770 & 0o77777777) | (nsvs & 7) | (date << 24)
    vremya = (now.tm_hour * 3600 + now.tm_min * 60 + now.tm_sec) * 50
    w = [0] * 1024
    w[SPEC] = ((god & 0o7777) << 36) | (god & E35P17) | (1 << 9)
    w[SHKRZST] = 0o30 << (6 * (nsvs - 1))
    w[SHKZZST] = (1 << 47) | 0o30
    w[KLUCHST] = STAT_KEY
    w[GODST] = god
    w[SCHETCH] = vremya << 18
    w[DATGEN] = god
    return w


def zone_data(data, z):
    """1024 слова данных зоны z (без тегов)."""
    base = (z * ZONE_WORDS + 8) * WORD
    return [struct.unpack("<Q", data[base + i * WORD:base + (i + 1) * WORD])[0]
            & M48 for i in range(1024)]


def sym_addr(data, name):
    """Адрес ячейки из таблицы имён тома (как disk_load_autotime_syms)."""
    names, addrs = zone_data(data, SYM_NAMES), zone_data(data, SYM_ADDRS)
    for i, n in enumerate(names):
        if n == name:
            w = addrs[i // 2]
            return (w >> 24) & 0o77777777 if i % 2 == 0 else w & 0o77777777
    return None


def patch_word(data, z, i, fn):
    """Заменить слово данных i зоны z на fn(старое), пересчитать КС зоны."""
    base = z * ZONE_WORDS * WORD
    off = base + (8 + i) * WORD
    raw = struct.unpack("<Q", data[off:off + WORD])[0]
    old = raw & M48
    data[off:off + WORD] = struct.pack("<Q", (raw & ~M48) | (fn(old) & M48))
    cs = cyclic_sum48(zone_data(data, z))
    for k in (3, 7):
        o = base + k * WORD
        r = struct.unpack("<Q", data[o:o + WORD])[0]
        data[o:o + WORD] = struct.pack("<Q", (r & ~M48) | cs)
    return old


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("src", help="дистрибутивный svs2053.bin (только чтение)")
    ap.add_argument("dst", help="куда положить исправленную копию")
    ap.add_argument("--pack", default="%o" % PACK_DEFAULT,
                    help="номер пакета, восьмеричный (по умолчанию 4005)")
    ap.add_argument("--year", type=int, default=time.localtime().tm_year,
                    help="год; в образ идут две младшие цифры (по умолчанию текущий)")
    ap.add_argument("--set-archive", type=int, choices=(0, 1), default=None,
                    help="0 — выключить архив (разр.16 ПРЕДЕЛ), 1 — включить;"
                         " по умолчанию разряд не трогается")
    ap.add_argument("--zero-archive-params", action="store_true",
                    help="обнулить данные зоны параметров архива (phys 0755),"
                         " СС оставить, КС пересчитать в 0 — задача архива"
                         " увидит «нет параметров»")
    ap.add_argument("--svs", type=int, default=1, choices=range(1, 9),
                    help="номер СВС для зоны статистики (по умолчанию 1)")
    ap.add_argument("--no-stat-zone", action="store_true",
                    help="не размечать зону статистики (оставить как в дистрибутиве)")
    ap.add_argument("--shift", type=int, default=1, choices=range(0, 8),
                    help="номер смены в МГРП резидента (по умолчанию 1, 0 - не трогать)")
    args = ap.parse_args()
    digit = args.year % 10
    tens = args.year // 10 % 10

    try:
        pack = int(args.pack, 8)
    except ValueError:
        sys.exit("makeSVS2053: --pack %r не восьмеричное число" % args.pack)
    if not 0 <= pack <= 0o7777:
        sys.exit("makeSVS2053: --pack %o не влезает в 12 разрядов" % pack)

    shutil.copyfile(args.src, args.dst)

    with open(args.dst, "r+b") as f:
        data = bytearray(f.read())
        if len(data) % (ZONE_WORDS * WORD):
            sys.exit("makeSVS2053: %s не кратен размеру зоны (%d слов)"
                     % (args.src, ZONE_WORDS))
        nzones = len(data) // (ZONE_WORDS * WORD)
        npack = nzone = 0

        for z in range(nzones):
            base = z * ZONE_WORDS * WORD

            for i in (0, 4):                    # чётные СС: номер зоны
                off = base + i * WORD
                raw = struct.unpack("<Q", data[off:off + WORD])[0]
                val = raw & M48
                if val == 0:                    # зона не размечена
                    continue
                new = (val & ~ZONE_MASK & M48) | ((z & 0o7777) << 36)
                if new != val:
                    data[off:off + WORD] = struct.pack("<Q", (raw & ~M48) | new)
                    nzone += 1

            for i in (1, 5):                    # нечётные СС: номер пакета
                off = base + i * WORD
                raw = struct.unpack("<Q", data[off:off + WORD])[0]
                val = raw & M48
                if (val & KEY_MASK) != KEY:     # правим только ключевые слова
                    continue
                new = (val & ~PACK_MASK & M48) | (pack << 12)
                if new != val:
                    data[off:off + WORD] = struct.pack("<Q", (raw & ~M48) | new)
                    npack += 1

        narch = nyear = 0
        kgod = None                             # КГОД первого варианта СВС --svs
        nvar = 0                                # вариантов ГЕНС-а в образе
        if PARAM_ZONE < nzones:                 # архив выключен, год
            base = PARAM_ZONE * ZONE_WORDS * WORD

            def word(i):
                return struct.unpack("<Q", data[base + i * WORD:
                                                  base + (i + 1) * WORD])[0]

            for v in range(8, ZONE_WORDS - VARIANT_WORDS + 1, VARIANT_WORDS):
                if word(v) & M48 != VARIANT_SIG:
                    continue
                nvar += 1
                if args.set_archive is not None:
                    off = base + (v + PREDEL) * WORD
                    raw = word(v + PREDEL)
                    new = (raw | E16) if args.set_archive else (raw & ~E16)
                    if new != raw:
                        data[off:off + WORD] = struct.pack("<Q", new)
                        narch += 1
                off = base + (v + KGOD) * WORD
                raw = word(v + KGOD)
                new = ((raw & ~YEAR_MASK) | (digit << YEAR_SHIFT)
                       | (tens << TENS_SHIFT))
                if new != raw:
                    data[off:off + WORD] = struct.pack("<Q", new)
                    nyear += 1
                if kgod is None and word(v + NSVS_WORD) & 7 == args.svs:
                    kgod = new & M48
            if narch or nyear:
                cs = cyclic_sum48([word(i) for i in range(8, ZONE_WORDS)])
                for i in (3, 7):
                    off = base + i * WORD
                    data[off:off + WORD] = struct.pack(
                        "<Q", (word(i) & ~M48) | cs)

        nzeroed = 0
        if args.zero_archive_params and ARCH_PARAM_ZONE < nzones:
            base = ARCH_PARAM_ZONE * ZONE_WORDS * WORD

            def aword(i):
                return struct.unpack("<Q", data[base + i * WORD:
                                                  base + (i + 1) * WORD])[0]

            for i in range(8, ZONE_WORDS):      # только слова данных
                off = base + i * WORD
                raw = aword(i)
                if raw & M48:                   # чистим значение, тег сохраняем
                    data[off:off + WORD] = struct.pack("<Q", raw & ~M48)
                    nzeroed += 1
            cs = cyclic_sum48([aword(i) for i in range(8, ZONE_WORDS)])  # = 0
            for i in (3, 7):                    # КС зоны -> 0, СС остальные целы
                off = base + i * WORD
                data[off:off + WORD] = struct.pack("<Q", (aword(i) & ~M48) | cs)

        stat_done = False
        # Без вариантов ГЕНС-а образ не системный (svs2048): зону не трогаем.
        if not args.no_stat_zone and nvar:
            if kgod is None:
                sys.exit("makeSVS2053: нет варианта ГЕНС-а для СВС %d" % args.svs)
            if STAT_ZONE >= nzones:
                sys.exit("makeSVS2053: в образе нет зоны статистики %04o" % STAT_ZONE)
            now = time.localtime()
            words = stat_zone_words(kgod, args.svs, now)
            base = STAT_ZONE * ZONE_WORDS * WORD
            for i, v in enumerate(words):
                off = base + (8 + i) * WORD
                data[off:off + WORD] = struct.pack("<Q", (STAT_TAG << 48) | v)
            cs = cyclic_sum48(words)
            for i in (3, 7):
                off = base + i * WORD
                raw = struct.unpack("<Q", data[off:off + WORD])[0]
                data[off:off + WORD] = struct.pack("<Q", (raw & ~M48) | cs)
            stat_done = True

        mgrp = mgrp_old = None
        if args.shift and nvar:
            mgrp = sym_addr(data, SYM_MGRP)
            if mgrp is None or mgrp >= 1024:
                sys.exit("makeSVS2053: МГРП не найден в странице 0 резидента")
            mgrp_old = patch_word(
                data, RES_ZONE, mgrp,
                lambda v: (v & ~(7 << SHIFT_SHIFT)) | (args.shift << SHIFT_SHIFT))

        f.seek(0)
        f.write(data)

    print("makeSVS2053: %s -> %s" % (args.src, args.dst))
    print("             зон %d; номер пакета -> %o в %d словах;"
          " номер зоны исправлен в %d словах" % (nzones, pack, npack, nzone))
    if args.set_archive is None:
        print("             архив: разр.16 ПРЕДЕЛ не тронут (зона %04o)" % PARAM_ZONE)
    else:
        print("             архив %s (разр.16 ПРЕДЕЛ) в %d вариантах ГЕНС-а (зона %04o)"
              % ("включён" if args.set_archive else "выключен", narch, PARAM_ZONE))
    print("             год %d: цифры %d%d в %d вариантах"
          % (args.year, tens, digit, nyear))
    if mgrp is not None:
        print("             смена %d в МГРП (%05o, зона %04o): %016o -> %016o"
              % (args.shift, mgrp, RES_ZONE, mgrp_old,
                 (mgrp_old & ~(7 << SHIFT_SHIFT)) | (args.shift << SHIFT_SHIFT)))
    if not stat_done and not args.no_stat_zone:
        print("             зона статистики не тронута: в образе нет вариантов ГЕНС-а")
    if stat_done:
        print("             зона статистики %04o (лог. 030) размечена: СВС %d, %s"
              % (STAT_ZONE, args.svs, time.strftime("%d.%m.%Y %H:%M:%S", now)))
    if args.zero_archive_params:
        print("             параметры архива обнулены (зона %04o), слов %d"
              % (ARCH_PARAM_ZONE, nzeroed))


if __name__ == "__main__":
    main()
