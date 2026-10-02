#!/usr/bin/env python3
"""
Операторский терминал и VIDI в зоне конфигурации системного диска sbor2053.bin
(Диспак БЭСМ-6).

ВЫДИНС пишет конфигурацию в логическую зону 0750 (ИСКОНФ, vydins.be), в образе
это зона 0754 (физическая = логическая + 4).  Первые 512 слов данных - до 16
вариантов по 040 слов, каждый - копия слов '72000'-'72037' ВЫДИНС-а, которые
ГЕНС видит как '71740'-'71777' (gens.be).  Слово 016 варианта (NБЭСМ, разр.3-1)
- номер машины; вариант выбирается при вызове ОС (ТР2 разр.27-25, "номер
конфигурации ГЕНС").  Непустой вариант - ненулевое слово 0 (КОНФИГ).

Скрипт правит в вариантах (по умолчанию во всех непустых, --variant - в
указанных) то, что задают разделы ТЕРМ, ШКОПТТ, ТКАНАЛ и VIDI, для одного
терминала N (номер линии SIMH = номер физического канала; в разделах он
пишется восьмерично: tty25 = '31', tty26 = '32'):

    ШКУСТР  слово 027  разр.49-N  - терминал включён напрямую (ТЕРМ);
                       для КОНСУЛОВ '31'/'32' это разр.24/23, по ним же
                       ГЕНС1 (пб35л) открывает их маски ПРП
    ШКVТ    слово 036  разр.25-N  - терминал - видеотон (ТЕРМ VТ:N,
                       ВЫДИНС зяшкvt; ГЕНС1 сдвигает в тппv)
    ТРАКТЫ  слово 015  разр.29    - раздел ТЕРМ задан
    ШКОПТ   слово 030  разр.49-N  - операторский терминал (ШКОПТТ);
                       ЗАМЕНЯЕТСЯ: N становится единственным
    ТКАНА   слово 026  по 6 разрядов на логический канал 0-7 (разр.6L+6..6L+1)
                       - номер физического канала; все 8 -> N (ТКАНАЛ ФN:0-7)
    ПМБ     слово 010  разр.8 - VIDI для '31', разр.7 - для '32'
                       (VIDI N: И П3 / СДА 64-6); --consul с --vidi зажигает
                       разряд своей линии, без --vidi - гасит
    ПРОГОН  слово 004  разр.33-N - ЕСТЕРМ N; ГЕНС1 переносит в шестр, и
                       только по нему БОНБОТ при подключении ставит линии
                       код КОНСУЛА (Е17 в ТСЛ, bonbot.be н4).  Без него
                       линия без VIDI считается ТТ напрямую, и ОС на неё
                       через '174'/'175' не выдаёт.  --consul без --vidi
                       зажигает разряд 8/7, с --vidi - гасит.  (rukava.be
                       велит 31, 32 в ЕСТЕРМ не ставить; младшие 8 разрядов
                       ПРОГОН ГЕНС1 берёт ещё в длину аварийной выдачи на МЛ
                       - АВМЛС, - на обычную работу это не влияет.)

Остальные терминалы и разряды не трогаются.  Служебные слова зоны, кроме
контрольной суммы, не меняются: СС[3] и СС[7] - сумма 1024 слов данных с
циклическим переносом по 48 разрядам; теги слов сохраняются.

    python3 makeBESM2053.py [образ [копия]] (--consul 25|26 [--vidi] | --vt N)
                            [--variant K[,K...]]

Образ по умолчанию - sbor2053.bin в текущем каталоге; без второго имени он
правится на месте.  --vt N - видеотон на последовательной линии 1-24.

Терминальный тип линии в SIMH должен соответствовать: --consul N --vidi ->
set ttyN vt; --consul N без --vidi -> set ttyN consul; --vt N -> set ttyN vt.
"""
import argparse
import shutil
import struct
import sys

WORD = 8                            # байт на слово в образе диска
ZONE_WORDS = 8 + 1024               # 8 служебных + 1024 данных
M48 = (1 << 48) - 1

CONF_ZONE = 0o750 + 4               # ИСКОНФ: логическая 0750
VARIANT_WORDS = 0o40
NVARIANTS = 16

PROGON, PMB, TRAKTY, NBESM = 0o4, 0o10, 0o15, 0o16
TKANA, SHKUSTR, SHKOPT, SHKVT = 0o26, 0o27, 0o30, 0o36
E29 = 1 << 28                       # ТРАКТЫ: раздел ТЕРМ задан
VIDI_BIT = {25: 1 << 7, 26: 1 << 6} # VIDI 2 - '31', VIDI 1 - '32'
ESTERM_BIT = {25: 1 << 7, 26: 1 << 6}   # ЕСТЕРМ N: разр.33-N


def cyclic_sum48(words):
    """Сумма с циклическим переносом по 48 разрядам."""
    s = 0
    for w in words:
        s += w & M48
        s = (s & M48) + (s >> 48)
    return s


def scale_bit(n):
    """Е48-1(N): разряд 49-N, терминал 1 - разр.48."""
    return 1 << (48 - n)


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("src", nargs="?", default="sbor2053.bin",
                    help="образ системного диска (по умолчанию sbor2053.bin)")
    ap.add_argument("dst", nargs="?",
                    help="куда записать результат (по умолчанию - на место src)")
    term = ap.add_mutually_exclusive_group(required=True)
    term.add_argument("--consul", type=int, choices=(25, 26),
                      help="операторский терминал - КОНСУЛ на линии 25 ('31') или 26 ('32')")
    term.add_argument("--vt", type=int, metavar="N",
                      help="операторский терминал - видеотон на линии N (1-24)")
    ap.add_argument("--vidi", action="store_true",
                    help="с --consul: видеотон по каналу КОНСУЛА (VIDI); без неё VIDI для линии гасится")
    ap.add_argument("--variant", default="",
                    help="номера вариантов через запятую (0-15); по умолчанию все непустые")
    args = ap.parse_args()

    if args.vidi and args.consul is None:
        sys.exit("makeBESM2053: --vidi имеет смысл только с --consul")
    if args.vt is not None and not 1 <= args.vt <= 24:
        sys.exit("makeBESM2053: --vt: номер линии 1-24")
    try:
        only = {int(x) for x in args.variant.split(",") if x.strip()}
    except ValueError:
        sys.exit("makeBESM2053: --variant %r: нужны номера через запятую" % args.variant)
    if any(not 0 <= v < NVARIANTS for v in only):
        sys.exit("makeBESM2053: --variant: номер варианта 0-15")

    n = args.consul if args.consul is not None else args.vt
    dst = args.dst or args.src
    if dst != args.src:
        shutil.copyfile(args.src, dst)

    with open(dst, "r+b") as f:
        data = bytearray(f.read())
        if len(data) % (ZONE_WORDS * WORD):
            sys.exit("makeBESM2053: %s не кратен размеру зоны (%d слов)"
                     % (args.src, ZONE_WORDS))
        if CONF_ZONE >= len(data) // (ZONE_WORDS * WORD):
            sys.exit("makeBESM2053: в %s нет зоны конфигурации %04o" % (args.src, CONF_ZONE))
        base = CONF_ZONE * ZONE_WORDS * WORD

        def get(i):
            return struct.unpack("<Q", data[base + i * WORD:base + (i + 1) * WORD])[0]

        def put(i, value):
            raw = get(i)
            data[base + i * WORD:base + (i + 1) * WORD] = struct.pack(
                "<Q", (raw & ~M48) | (value & M48))

        done = []
        for v in range(NVARIANTS):
            first = 8 + v * VARIANT_WORDS
            if not get(first) & M48:
                if v in only:
                    sys.exit("makeBESM2053: вариант %d пуст" % v)
                continue
            if only and v not in only:
                continue

            def upd(word, fn):
                put(first + word, fn(get(first + word) & M48))

            upd(SHKUSTR, lambda w: w | scale_bit(n))
            upd(TRAKTY, lambda w: w | E29)
            if args.vt is not None:
                upd(SHKVT, lambda w: w | scale_bit(n) >> 24)
            upd(SHKOPT, lambda w: scale_bit(n))
            upd(TKANA, lambda w: sum(n << (6 * ch) for ch in range(8)))
            if args.consul is not None:
                vidi, ester = VIDI_BIT[n], ESTERM_BIT[n]
                upd(PMB, lambda w: (w | vidi) if args.vidi else (w & ~vidi))
                upd(PROGON, lambda w: (w & ~ester) if args.vidi else (w | ester))
            done.append((v, get(first + NBESM) & 7))

        cs = cyclic_sum48([get(i) for i in range(8, ZONE_WORDS)])
        for i in (3, 7):
            put(i, cs)

        f.seek(0)
        f.write(data)

    if args.consul is not None:
        kind = "КОНСУЛ%s" % (" (VIDI: видеотон)" if args.vidi else "")
        simh = "set tty%d %s" % (n, "vt" if args.vidi else "consul")
    else:
        kind = "видеотон"
        simh = "set tty%d vt" % n
    print("makeBESM2053: %s -> %s, зона %04o" % (args.src, dst, CONF_ZONE))
    print("              операторский терминал '%o' (tty%d) - %s" % (n, n, kind))
    print("              варианты: %s" % (", ".join("%d (машина %d)" % d for d in done) or "нет"))
    print("              КС зоны %016o; в SIMH: %s" % (cs, simh))


if __name__ == "__main__":
    main()
