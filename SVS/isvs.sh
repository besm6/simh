#!/bin/bash
#
# Интерактивный сеанс с Диспаком: живой терминал, ввод с клавиатуры идёт в
# машину слогом с линии 61, вывод машины на эту линию печатается на экран.
#
#   ./isvs.sh [ini-файл] [лимит-ГБ]
#
# По умолчанию int.ini и 4 ГБ.
#
# Барабан пересобирается перед каждым сеансом (KEEP_DRUM=1 — оставить), образ
# системного диска сохраняется (REBUILD_DISK=1 — собрать заново, см. ниже).
# Путь к дистрибутиву задаётся переменной SVS2053.
#
# Потоки НЕ перенаправляются: sim_poll_kbd() отдаёт символы только при
# sim_ttisatty(), и под каналом ввод до машины не доходит (МПД.md §5).
# Поэтому размер журнала стережёт отдельный фоновый процесс: терминалом он не
# владеет и вывод симулятора не трогает.
#
# Что делать в сеансе:
#   ^E                  уйти в sim> посреди прогона
#   cont                вернуться в машину
#   do probe-mott.ini   поставить пробы на МОТТ
#   set cpu0 debug       покомандная трасса с регистрами…
#   set cpu0 window=76000:77777   …только по МОТТ
#   set cpu0 debug=insn  только команды, без регистров
#   set cpu0 nodebug     выключить

set -e

cd "$(dirname "$0")"

INI=${1:-int.ini}
LIMIT_GB=${2:-4}
LOG=int.log
LIMIT=$((LIMIT_GB * 1024 * 1024 * 1024))

SRC=${SVS2053:-/home/leob/git/besm6.github.io/download/disks/svs2053.bin}
IMG=svs2053-fixed.bin

if [ ! -r "$SRC" ]; then
    echo "isvs.sh: не найден дистрибутивный образ: $SRC" >&2
    echo "isvs.sh: укажите его через SVS2053=/путь/к/svs2053.bin" >&2
    exit 2
fi

if [ ! -x ../BIN/svs ]; then
    echo "isvs.sh: нет ../BIN/svs — соберите: (cd ../.. && make svs)" >&2
    exit 2
fi

# Образ по умолчанию сохраняется между сеансами; собирается, только если его
# нет или задано REBUILD_DISK=1. makeSVS2053.py кладёт на него смену в МГРП и
# зону статистики с текущей датой — без них быстрая установка времени
# (ТР1 разр.45 в int.ini) не проходит и ОС просит «НАБЕРИТЕ СМЕ…».
if [ -f "$IMG" ] && [ "${REBUILD_DISK:-0}" != 1 ]; then
    echo "isvs.sh: образ $IMG оставлен от прошлого сеанса"
else
    python3 tools/makeSVS2053.py "$SRC" "$IMG"
fi

if [ "${KEEP_DRUM:-0}" = 1 ]; then
    echo "isvs.sh: барабан drum5.bin оставлен от прошлого сеанса"
else
    rm -f drum5.bin
fi

rm -f "$LOG"

# Сторож размера журнала. Снимает симулятор, как только журнал перерос лимит;
# терминал остаётся за симулятором.
(
    while kill -0 $$ 2>/dev/null; do
        if [ -f "$LOG" ]; then
            SZ=$(stat -c %s "$LOG" 2>/dev/null || echo 0)
            if [ "$SZ" -gt "$LIMIT" ]; then
                echo >&2
                echo "isvs.sh: журнал перерос ${LIMIT_GB} ГБ — снимаю симулятор" >&2
                pkill -9 -P $$ -x svs 2>/dev/null || true
                exit 0
            fi
        fi
        sleep 2
    done
) &
WATCH=$!
trap 'kill $WATCH 2>/dev/null || true' EXIT

echo "isvs.sh: сеанс $INI, журнал $LOG, лимит ${LIMIT_GB} ГБ"
echo "isvs.sh: ^E — выход в sim>, cont — обратно в машину"
../BIN/svs "$INI"
