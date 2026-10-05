#!/usr/bin/env python3
"""
Правка зоны конфигурации системного диска sbor2053.bin (Диспак БЭСМ-6) по
файлу разделов в духе ВЫДИНС.

ВЫДИНС пишет конфигурацию в логическую зону 0750 (ИСКОНФ, vydins.be), в образе
это зона 0754 (физическая = логическая + 4).  Первые 512 слов данных - до 16
вариантов по 040 слов, каждый - копия слов '72000'-'72037' ВЫДИНС-а, которые
ГЕНС видит как '71740'-'71777' (gens.be).  Слово 016 варианта (NБЭСМ, разр.3-1)
- номер машины; вариант выбирается при вызове ОС (ТР2 разр.27-25, "номер
конфигурации ГЕНС").  Непустой вариант - ненулевое слово 0 (КОНФИГ).

    python3 makeBESM2053.py файл [образ [копия]] [--variant K[,K...]]

Образ по умолчанию - sbor2053.bin в текущем каталоге; без второго имени он
правится на месте.  Правятся все непустые варианты или только перечисленные
в --variant.  Служебные слова зоны, кроме контрольной суммы, не меняются:
СС[3] и СС[7] - сумма 1024 слов данных с циклическим переносом по 48
разрядам; теги слов сохраняются.

Файл разделов
-------------
Одна строка - один раздел: имя и параметры.  Строки, начинающиеся с '*' или
'#', и пустые строки пропускаются.  Как и в ВЫДИНС, имя узнаётся по первым
пяти буквам (ЕСАЦП = ЕСАЦПУ); латинские буквы, похожие на русские, считаются
русскими (VT = VТ).  Номера каналов и терминалов - восьмеричные, списки -
через запятую, диапазоны - через минус.  Раздел ТЕРМ добавляет терминалы;
остальные разделы целиком задают то, за что отвечают.  Чего в файле нет, то
в образе остаётся как было.

    ТЕРМ [АС,]ВУ:список как ТЕРМ в ВЫДИНС (vydins.be ТЕРМ, изтерм, усдиап);
                        каждый раздел ТЕРМ только добавляет разряды, все
                        ставят ТРАКТЫ слово 015 разр.29 (раздел ТЕРМ задан).
                        Напрямую: ВУ = ТТ, VТ (каналы 1-30) или СОNS (31, 32):
                        ШКУСТР слово 027 разр.49-N; для VТ ещё ШКVТ слово 036
                        |= (шкала раздела) >> 24 (ГЕНС1 сдвигает его в тппv).
                        Через АС (терминалы ДКС): ВУ = ТТ или VТ, номера
                        1-237; база Б = 034 (ШКТТ) для ТТ, 036 (ШКVТ) для VТ:
                          N 1-60    слово Б+1   разр.49-N
                          N 61-77   слово Б     разр.49-(N-060)
                          N 100-157 слово 013 (ШАС128) разр.49-(N-077)
                          N 160-237 слово 014   разр.49-(N-0157)
    ШКОПТТ список       операторские терминалы: ШКОПТ слово 030 разр.49-N.
    ТКАНАЛ ФN:список,...
                        логические каналы 0-7 -> физический канал N: ТКАНА
                        слово 026, по 6 разрядов на канал L (разр.6L+6..6L+1);
                        меняются только перечисленные L.
    VIDI N              видеотоны по каналам КОНСУЛА: N = 1 - '32', 2 - '31',
                        3 - оба, 0 или НЕТ - ни одного.  ПМБ слово 010
                        разр.8 ('31') и 7 ('32').
    ЕСТЕРМ список | НЕТ терминалы ЕС через АС-6 (1-35): ПРОГОН слово 004
                        разр.33-N; ГЕНС1 переносит их в шестр.  Для КОНСУЛОВ
                        '31'/'32' без VIDI нужен ЕСТЕРМ 31/32: только по нему
                        БОНБОТ ставит линии код КОНСУЛА (Е17 в ТСЛ, bonbot.be
                        н4), иначе линия считается ТТ напрямую.  (Младшие 8
                        разрядов ПРОГОН ГЕНС1 берёт ещё в длину аварийной
                        выдачи на МЛ, АВМЛС; они не трогаются.)
    ЕСМЛ N | НЕТ        МЛ ЕС-5017 на направлении N: КОНФИГ+1 слово 001
                        разр.46-48 (ВЫДИНС: ИЛИ N<<45; ГЕНС по нему грузит
                        esml.be).  esml.be жёстко использует '110', поэтому
                        работает только N = 4.
    СОЮЗ ДА | НЕТ       связь БЭСМ-6 - ПМ-6: ТРАКТЫ разр.31.  ДИСП70 отдаёт
                        будильник задачи 24 модулю ОСА (есацпу), ГЕНС1 читает
                        ОСА в листы 020-021.  Как и ВЫДИНС (преобр, маскнф),
                        занимает эти листы в РАСПП (слово 011, лист K -
                        разр.48-K, здесь разр.32-31); без них ГЕНС1
                        останавливается.
                        Кроме того, правит код ГЕНС2 в образе: при СОЮЗ ДА
                        ГЕНС2 (зона 0475 = логическая 0471, грузится в
                        060000) идёт на КРАБ (gens2.be:572-574), а там в этой
                        сборке стоп: 060520 = сч 0 / стоп КРАБ.  ДА заменяет
                        слово на пб ВОЗВ2(М6) / мода: генерация идёт дальше,
                        как без связи, а разряд 31 ПРЕДЕЛ остаётся для ОСА.
                        Переход на КРАБА (обычный проход по каталогу работ,
                        после которого снова ВОЗВ1), как в закомментированном
                        исходном коде, без машины-партнёра зацикливается
                        (проверено).  НЕТ возвращает стоп.  Слово, не
                        совпадающее ни с одним из двух вариантов, не
                        трогается; контрольная сумма зоны (СС[3], СС[7])
                        пересчитывается.
    ТМГУ ДА | НЕТ       терминалы АС по схеме МГУ (ТЕРМАС): ТРАКТЫ разр.39
                        (ВЫДИНС ТМГУ -> зппар).  ГЕНС1 тогда читает ТЕРМАС
                        (зона 0414) в лист 020 (gens1.be:960-965) - туда же,
                        куда ОСА, поэтому для ОСА нужно ТМГУ НЕТ (РУКАВА:
                        на машине с КАДОПАМ ТМГУ отключена).
    СЭВМ ДА | НЕТ       комплекс ЭВМ (Минск, SDS-910, М-6000): ТРАКТЫ разр.28
                        и лист 034 в РАСПП (разр.20).
    ЕСАЦПУ N[,N] | НЕТ  АЦПУ ЕС (N = 0, 1) через АС-6 вместо АЦПУ БЭСМ-6:
                        СЛОЙК слово 005 разр.43-N.
    ЕСПИ 0 | НЕТ        ПИ ЕС через АС-6: СЛОЙК разр.41.
    ЕСПЛ 0 | НЕТ        ПЛ ЕС через АС-6: СЛОЙК разр.40.  По разр.40-43
                        СЛОЙК ГЕНС грузит модули ЕС.

Пример: КОНСУЛ на канале '32' и ЕС-5017:

    ТЕРМ СОNS:32
    ШКОПТТ 32
    ТКАНАЛ Ф32:0-7
    VIDI НЕТ
    ЕСТЕРМ 32
    ЕСМЛ 4

Скрипт печатает, как настроить SIMH: тип линий ТЕРМ (СОNS -> set ttyN
consul, с VIDI -> vt; VТ -> vt; ТТ -> tt), set dks enabled для ТЕРМ АС,
set mg4 es|noes для ЕСМЛ, set osa enabled для СОЮЗ и устройств ЕС.  Номер линии SIMH равен номеру канала:
'31' - tty25, '32' - tty26.
"""
import argparse
import re
import shutil
import struct
import sys

WORD = 8                            # байт на слово в образе диска
ZONE_WORDS = 8 + 1024               # 8 служебных + 1024 данных
M48 = (1 << 48) - 1

CONF_ZONE = 0o750 + 4               # ИСКОНФ: логическая 0750
VARIANT_WORDS = 0o40
NVARIANTS = 16

KONFIG1, PROGON, SLOJK, PMB, RASPP = 0o1, 0o4, 0o5, 0o10, 0o11
SHAS128, TRAKTY, NBESM = 0o13, 0o15, 0o16
TKANA, SHKUSTR, SHKOPT = 0o26, 0o27, 0o30
SHKTT, SHKVT = 0o34, 0o36          # и следующие слова: ШКТТ2, ШКVТ2

E29 = 1 << 28                       # ТРАКТЫ: раздел ТЕРМ задан
SOYUZ = 1 << 30                     # ТРАКТЫ разр.31: СОЮЗ ДА
SEVM = 1 << 27                      # ТРАКТЫ разр.28: СЭВМ ДА
TMGU = 1 << 38                      # ТРАКТЫ разр.39: ТМГУ ДА
SOYUZ_PAGES = 3 << 30               # РАСПП: листы 020-021, резидент ОСА
SEVM_PAGE = 1 << 19                 # РАСПП: лист 034
VIDI_BIT = {0o31: 1 << 7, 0o32: 1 << 6}
ESTERM_MASK = sum(1 << (32 - n) for n in range(1, 0o36))
ESML_MASK = 7 << 45                 # КОНФИГ+1: ЕСМЛ N, разр.46-48
ESML_DIR = 4                        # единственное рабочее направление ЕС
ES_PRINTER = {0: 1 << 42, 1: 1 << 41}   # СЛОЙК: ЕСАЦПУ N
ES_PI, ES_PL = 1 << 40, 1 << 39     # СЛОЙК: ЕСПИ 0, ЕСПЛ 0

GENS2_ZONE = 0o471 + 4              # ГЕНС2 в образе
KRAB_WORD = 0o520                   # 060520 - 060000
KRAB_STOP = 0o0010000033300520      # сч 0 / стоп КРАБ(М6)
KRAB_JUMP = 0o3300042502200000      # пб ВОЗВ2(М6) / мода
KRAB_OLD = 0o3300024002200000       # пб КРАБА(М6) / мода - прежняя правка

# Латинские буквы, похожие на русские, - к русским.
HOMOGLYPHS = str.maketrans("ABCEHKMOPTXY", "АВСЕНКМОРТХУ")


def norm(s):
    return s.upper().translate(HOMOGLYPHS)


def cyclic_sum48(words):
    """Сумма с циклическим переносом по 48 разрядам."""
    s = 0
    for w in words:
        s += w & M48
        s = (s & M48) + (s >> 48)
    return s


def scale_bit(n):
    """Е48-1(N): разряд 49-N, канал 1 - разр.48."""
    return 1 << (48 - n)


class ConfError(Exception):
    pass


def numbers(text, lo, hi):
    """Восьмеричный список с диапазонами: '1-7,13' -> [1..7, 11]."""
    out = []
    for part in text.split(","):
        part = part.strip()
        if not part:
            continue
        m = re.fullmatch(r"([0-7]+)(?:-([0-7]+))?", part)
        if not m:
            raise ConfError("ожидался восьмеричный номер или диапазон: %r" % part)
        a = int(m.group(1), 8)
        b = int(m.group(2), 8) if m.group(2) else a
        if not lo <= a <= b <= hi:
            raise ConfError("номер вне диапазона %o-%o: %r" % (lo, hi, part))
        out.extend(range(a, b + 1))
    if not out:
        raise ConfError("пустой список")
    return out


def is_no(arg):
    return norm(arg) == norm("НЕТ")


def yes_no(arg):
    if norm(arg) == norm("ДА"):
        return True
    if is_no(arg):
        return False
    raise ConfError("ожидалось ДА или НЕТ: %r" % arg)


class Config:
    """Разобранный файл: правки слов варианта и подсказки для SIMH."""

    def __init__(self):
        self.ops = []           # (слово, функция w -> w)
        self.code = []          # (зона, слово, {допустимые}, новое, имя)
        self.simh = []
        self.notes = []
        self.consuls = {}       # канал -> место в simh, уточняется по VIDI
        self.vidi = None
        self.osa = False
        self.dks = False

    def op(self, word, fn):
        self.ops.append((word, fn))

    def set_bits(self, word, mask, value):
        self.op(word, lambda w: (w & ~mask) | value)

    # --- разделы -----------------------------------------------------

    def s_term(self, arg):
        m = re.fullmatch(r"([^:]+):(.+)", arg)
        if not m:
            raise ConfError("ожидалось [АС,]ВУ:список")
        kinds = [norm(k) for k in m.group(1).split(",")]
        tt, vt, cons = norm("ТТ"), norm("VТ"), norm("СОNS")
        if len(kinds) == 2 and kinds[0] == norm("АС") and kinds[1] in (tt, vt):
            self.term_as(kinds[1] == vt, numbers(m.group(2), 1, 0o237))
        elif len(kinds) == 1 and kinds[0] in (tt, vt):
            self.term_direct(kinds[0], numbers(m.group(2), 1, 0o30))
        elif len(kinds) == 1 and kinds[0] == cons:
            self.term_direct(kinds[0], numbers(m.group(2), 0o31, 0o32))
        else:
            raise ConfError("вид: ТТ, VТ, СОNS или АС,ТТ / АС,VТ")
        self.op(TRAKTY, lambda w: w | E29)

    def term_direct(self, kind, chans):
        """изтерм: ШКУСТР |= шкала; для VТ - ЯШКVТ |= шкала >> 24."""
        scale = 0
        for n in chans:
            scale |= scale_bit(n)
            if kind == norm("VТ"):
                self.simh.append("set tty%d vt" % n)
            elif kind == norm("ТТ"):
                self.simh.append("set tty%d tt" % n)
            else:
                self.consuls[n] = len(self.simh)
                self.simh.append(None)
        self.op(SHKUSTR, lambda w: w | scale)
        if kind == norm("VТ"):
            self.op(SHKVT, lambda w: w | scale >> 24)

    def term_as(self, vt, terms):
        """усдиап: терминалы АС по словам ШКТТ/ШКVТ (+1) и ШАС128 (+1)."""
        base = SHKVT if vt else SHKTT
        for n in terms:
            if n < 0o61:
                word, k = base + 1, n
            elif n < 0o100:
                word, k = base, n - 0o60
            elif n < 0o160:
                word, k = SHAS128, n - 0o77
            else:
                word, k = SHAS128 + 1, n - 111     # слиа -111: десятичное
            self.op(word, lambda w, k=k: w | scale_bit(k))
        self.dks = True

    def s_shkopt(self, arg):
        chans = set(numbers(arg, 1, 0o32))
        self.op(SHKOPT, lambda w: sum(scale_bit(n) for n in chans))

    def s_tkanal(self, arg):
        text = norm(arg)
        groups = re.findall(r"Ф([0-7]+):([^Ф]*)", text)
        if not groups or text.count("Ф") != len(groups):
            raise ConfError("ожидалось ФN:список,...")
        for phys, lst in groups:
            n = int(phys, 8)
            if not 1 <= n <= 0o40:
                raise ConfError("физический канал вне 1-40: %s" % phys)
            for ch in numbers(lst.rstrip(","), 0, 7):
                self.set_bits(TKANA, 0o77 << (6 * ch), n << (6 * ch))

    def s_vidi(self, arg):
        n = 0 if is_no(arg) else int(arg, 8)
        if not 0 <= n <= 3:
            raise ConfError("VIDI: 0-3 или НЕТ")
        self.vidi = n
        value = (VIDI_BIT[0o31] if n & 2 else 0) | (VIDI_BIT[0o32] if n & 1 else 0)
        self.set_bits(PMB, VIDI_BIT[0o31] | VIDI_BIT[0o32], value)

    def s_ester(self, arg):
        if is_no(arg):
            self.set_bits(PROGON, ESTERM_MASK, 0)
            return
        terms = set(numbers(arg, 1, 0o35))
        self.set_bits(PROGON, ESTERM_MASK, sum(1 << (32 - n) for n in terms))

    def s_esml(self, arg):
        if is_no(arg):
            self.set_bits(KONFIG1, ESML_MASK, 0)
            self.simh.append("set mg4 noes")
            return
        n = int(arg, 8)
        if n not in (3, 4):
            raise ConfError("ЕСМЛ: направление 3 или 4")
        if n != ESML_DIR:
            self.notes.append("ЕСМЛ %d: esml.be работает только с направлением 4" % n)
        self.set_bits(KONFIG1, ESML_MASK, n << 45)
        self.simh.append("set mg%d es" % n)

    def s_soyuz(self, arg):
        on = yes_no(arg)
        self.set_bits(TRAKTY, SOYUZ, SOYUZ if on else 0)
        self.set_bits(RASPP, SOYUZ_PAGES, SOYUZ_PAGES if on else 0)
        self.code.append((GENS2_ZONE, KRAB_WORD, {KRAB_STOP, KRAB_JUMP, KRAB_OLD},
                          KRAB_JUMP if on else KRAB_STOP,
                          "ГЕНС2 060520 (КРАБ) = %s" % ("пб ВОЗВ2" if on else "стоп КРАБ")))
        if on:
            self.osa = True

    def s_tmgu(self, arg):
        on = yes_no(arg)
        self.set_bits(TRAKTY, TMGU, TMGU if on else 0)

    def s_sevm(self, arg):
        on = yes_no(arg)
        self.set_bits(TRAKTY, SEVM, SEVM if on else 0)
        self.set_bits(RASPP, SEVM_PAGE, SEVM_PAGE if on else 0)

    def s_esacp(self, arg):
        mask = ES_PRINTER[0] | ES_PRINTER[1]
        if is_no(arg):
            self.set_bits(SLOJK, mask, 0)
            return
        value = sum(ES_PRINTER[n] for n in set(numbers(arg, 0, 1)))
        self.set_bits(SLOJK, mask, value)
        self.osa = True

    def es_dev(self, arg, bit):
        if is_no(arg):
            self.set_bits(SLOJK, bit, 0)
            return
        if numbers(arg, 0, 0) != [0]:
            raise ConfError("ожидалось 0 или НЕТ")
        self.set_bits(SLOJK, bit, bit)
        self.osa = True

    def s_espi(self, arg):
        self.es_dev(arg, ES_PI)

    def s_espl(self, arg):
        self.es_dev(arg, ES_PL)

    SECTIONS = {
        "ТЕРМ": s_term, "ТЕРМИ": s_term, "ШКОПТ": s_shkopt, "ТКАНА": s_tkanal,
        "VIDI": s_vidi, "ЕСТЕР": s_ester, "ЕСМЛ": s_esml, "СОЮЗ": s_soyuz,
        "СЭВМ": s_sevm, "ТМГУ": s_tmgu, "ЕСАЦП": s_esacp, "ЕСПИ": s_espi, "ЕСПЛ": s_espl,
    }

    def parse(self, path):
        sections = {norm(k): v for k, v in self.SECTIONS.items()}
        with open(path, encoding="utf-8") as f:
            for lineno, line in enumerate(f, 1):
                text = line.strip()
                if not text or text[0] in "*#":
                    continue
                name, _, arg = text.partition(" ")
                fn = sections.get(norm(name)[:5])
                try:
                    if fn is None:
                        raise ConfError("неизвестный раздел")
                    if not arg.strip():
                        raise ConfError("нет параметров")
                    fn(self, "".join(arg.split()))
                except (ConfError, ValueError) as e:
                    sys.exit("makeBESM2053: %s:%d: %s: %s" % (path, lineno, name, e))
        for n, i in self.consuls.items():
            vidi = self.vidi is not None and self.vidi & (2 if n == 0o31 else 1)
            self.simh[i] = "set tty%d %s" % (n, "vt" if vidi else "consul")
        if self.dks:
            self.simh.append("set dks enabled")
        if self.osa:
            self.simh.append("set osa enabled")


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("conf", help="файл разделов")
    ap.add_argument("src", nargs="?", default="sbor2053.bin",
                    help="образ системного диска (по умолчанию sbor2053.bin)")
    ap.add_argument("dst", nargs="?",
                    help="куда записать результат (по умолчанию - на место src)")
    ap.add_argument("--variant", default="",
                    help="номера вариантов через запятую (0-15); по умолчанию все непустые")
    args = ap.parse_args()

    try:
        only = {int(x) for x in args.variant.split(",") if x.strip()}
    except ValueError:
        sys.exit("makeBESM2053: --variant %r: нужны номера через запятую" % args.variant)
    if any(not 0 <= v < NVARIANTS for v in only):
        sys.exit("makeBESM2053: --variant: номер варианта 0-15")

    conf = Config()
    conf.parse(args.conf)
    if not conf.ops and not conf.code:
        sys.exit("makeBESM2053: %s: нет ни одного раздела" % args.conf)

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
        def zget(zone, i):
            at = (zone * ZONE_WORDS + i) * WORD
            return struct.unpack("<Q", data[at:at + WORD])[0]

        def zput(zone, i, value):
            at = (zone * ZONE_WORDS + i) * WORD
            raw = zget(zone, i)
            data[at:at + WORD] = struct.pack("<Q", (raw & ~M48) | (value & M48))

        def zsum(zone):
            cs = cyclic_sum48([zget(zone, i) for i in range(8, ZONE_WORDS)])
            for i in (3, 7):
                zput(zone, i, cs)
            return cs

        def get(i):
            return zget(CONF_ZONE, i)

        def put(i, value):
            zput(CONF_ZONE, i, value)

        done = []
        for v in range(NVARIANTS):
            first = 8 + v * VARIANT_WORDS
            if not get(first) & M48:
                if v in only:
                    sys.exit("makeBESM2053: вариант %d пуст" % v)
                continue
            if only and v not in only:
                continue
            for word, fn in conf.ops:
                put(first + word, fn(get(first + word) & M48))
            done.append((v, get(first + NBESM) & 7))

        cs = zsum(CONF_ZONE)
        for zone, word, allowed, new, name in conf.code:
            old = zget(zone, 8 + word) & M48
            if old not in allowed:
                sys.exit("makeBESM2053: зона %04o слово %04o = %016o: не та сборка, %s не сделана"
                         % (zone, word, old, name))
            zput(zone, 8 + word, new)
            zsum(zone)
            conf.notes.append(name)

        f.seek(0)
        f.write(data)

    print("makeBESM2053: %s -> %s, зона %04o" % (args.src, dst, CONF_ZONE))
    print("              варианты: %s" % (", ".join("%d (машина %d)" % d for d in done) or "нет"))
    for note in conf.notes:
        print("              %s" % note)
    print("              КС зоны %016o; в SIMH: %s" % (cs, "; ".join(conf.simh) or "-"))


if __name__ == "__main__":
    main()
