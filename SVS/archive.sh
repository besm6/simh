#!/bin/bash
#
# Прогон входного модуля задачи архива (archive.b6) на user-level БЭСМ-6 (dispak).
#
# Диски (dispak ищет их как $BESM6_PATH/<номер>):
#   2053 -> svs2053-fixed.bin  — системный том (из дистрибутива, makeSVS2053.py)
#   2052 -> 2052.bin           — том архива (2248 с перебитым номером, mkvol.py)
#
# В user-level модели номера зон в информационных словах экстракода 70
# ЛОГИЧЕСКИЕ (= физическая - 4): логическая 751 = физическая 0755.
#
# Опции (см. -h):
#   -k, --keep-disk        не пересобирать образы, взять готовые
#   -z, --zero-params      обнулить зону параметров архива (phys 0755) — задача
#                          архива видит «нет параметров» и идёт в генерацию с нуля
#   -Z, --zero-2052        обнулить данные тома 2052.bin — чистый пустой том
#                          архива (без файлов исходного диска, только роспись)
#   -i, --input ФАЙЛ       подать ФАЙЛ на терминал архива (директивы); без -i —
#                          интерактивно: ввод с терминала, без лимита времени
#   -e, --terminal N       логич. номер терминала архива (0 — без; по умолч. 1)
#   -t, --trace            трасса экстракодов (дважды: -t -t — полная трасса команд)
#       --timeout СЕК      предел времени прогона (по умолч. 20)
#       --dispak ПУТЬ      двоичный dispak (по умолч. из PATH)
#       --src2053 ФАЙЛ     исходный дистрибутив svs2053.bin
#       --src2248 ФАЙЛ     исходный том для 2052 (по умолч. /usr/local/share/besm6/2248)
#       --src2048 ФАЙЛ     исходный том каталогов 2048 (копируется в записываемый 2048.bin)
#       --no-md29mb        не задавать 29-Мб геометрию МД (по умолч. задаётся)
#   -h, --help             эта справка
#   -- АРГ...              передать оставшиеся АРГ прямо dispak-у

set -e
cd "$(dirname "$0")"

# --- значения по умолчанию ---
SRC2053=/home/$USER/git/besm6.github.io/download/disks/svs2053.bin
SRC2248=/usr/local/share/besm6/2248
SRC2048=/usr/local/share/besm6/2048    # исходный том каталогов архива (МД 2048)
DISPAK_BIN=/home/leob/git/dispak/build/dispak/dispak   # сборка с E65_-переопределением; --dispak чтобы сменить
INPUT=              # пусто = интерактивно (ввод с терминала), иначе файл директив
TIMEOUT=20
KEEP_DISK=0
ZERO_PARAMS=0
ZERO_2052=0             # -Z: обнулить данные 2052.bin (чистый пустой том архива)
MD29MB=1                # dispak: том как 29-Мб МД (disk.c halfzones), иначе
                        # служебные слова зоны пишутся в неверном формате
TERMINAL=1              # логич. терминал архива -> E65_1 = 020000000|N (0 — без)
TRACE=0
DISPAK_EXTRA=()

usage() { awk 'NR>1 && /^#/ {sub(/^# ?/,""); print; next} NR>1 {exit}' "$0"; }

while [ $# -gt 0 ]; do
    case "$1" in
        -k|--keep-disk)   KEEP_DISK=1 ;;
        -z|--zero-params) ZERO_PARAMS=1 ;;
        -Z|--zero-2052)   ZERO_2052=1 ;;
        --src2048)        SRC2048="$2"; shift ;;
        -i|--input)       INPUT="$2"; shift ;;
        -e|--terminal)    TERMINAL="$2"; shift ;;
        -t|--trace)       TRACE=$((TRACE + 1)) ;;
        --timeout)        TIMEOUT="$2"; shift ;;
        --dispak)         DISPAK_BIN="$2"; shift ;;
        --src2053)        SRC2053="$2"; shift ;;
        --src2248)        SRC2248="$2"; shift ;;
        --no-md29mb)      MD29MB=0 ;;
        -h|--help)        usage; exit 0 ;;
        --)               shift; DISPAK_EXTRA+=("$@"); break ;;
        *) echo "archive.sh: неизвестная опция: $1" >&2; usage >&2; exit 2 ;;
    esac
    shift
done

# --- сборка образов ---
MK=()
[ "$ZERO_PARAMS" = 1 ] && MK+=(--zero-archive-params)
MK2052=()
[ "$ZERO_2052" = 1 ] && MK2052+=(--zero-data)

if [ "$KEEP_DISK" = 1 ] && [ -f svs2053-fixed.bin ] && [ -f 2052.bin ] && [ -f 2048.bin ]; then
    :   # переиспользуем готовые образы (быстрые эксперименты)
else
    python3 tools/makeSVS2053.py "${MK[@]}" "$SRC2053" svs2053-fixed.bin
    python3 tools/mkvol.py "${MK2052[@]}" "$SRC2248" 2052.bin
    # том каталогов архива (МД 2048) — ЗАПИСЫВАЕМАЯ копия (исходник только на чтение)
    cp -f "$SRC2048" 2048.bin
    chmod u+w 2048.bin
fi

D=./diskdir
mkdir -p "$D"
ln -sf "$PWD/svs2053-fixed.bin" "$D/2053"
ln -sf "$PWD/2052.bin"          "$D/2052"
ln -sf "$PWD/2048.bin"          "$D/2048"
rm -f "$D/0"                      # чистый барабан

# --- окружение dispak, вычисленное из опций ---
ENVV=(BESM6_PATH="$D:/usr/local/share/besm6")
ENVV+=(E65_0=2000000000062121)
[ "$MD29MB" = 1 ] && ENVV+=(MD29MB=1)
if [ "$TERMINAL" != 0 ]; then      # дескриптор терминала архива: разр.23 + номер
    ENVV+=(E65_1="$(printf '%o' $(( 8#20000000 | TERMINAL )))")
fi

TR=()
[ "$TRACE" -ge 1 ] && TR+=(-t)
[ "$TRACE" -ge 2 ] && TR+=(-t)

echo "archive.sh: $DISPAK_BIN --bootstrap archive.b6 (2053=svs2053-fixed.bin, 2052=2052.bin)${MK:+  [$MK]}${TERMINAL:+  терм=$TERMINAL}${INPUT:+  вход=$INPUT}"
if [ -n "$INPUT" ]; then
    # неинтерактивно: директивы из файла, со сторожем по времени
    env "${ENVV[@]}" timeout -s KILL "$TIMEOUT" \
        "$DISPAK_BIN" --bootstrap archive.b6 --input-encoding=utf8 "${TR[@]}" "${DISPAK_EXTRA[@]}" \
        < "$INPUT" 2>&1
else
    # интерактивно: ввод с терминала, без лимита времени (выход — КНЦ/КОНЕЦ или ^C)
    env "${ENVV[@]}" \
        "$DISPAK_BIN" --bootstrap archive.b6 --input-encoding=utf8 "${TR[@]}" "${DISPAK_EXTRA[@]}"
fi
