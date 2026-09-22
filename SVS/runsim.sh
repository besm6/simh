#!/bin/bash
# Запуск симулятора со сторожем размера трассы.
#
# Трасса команд (set cpu0 debug=insn) растёт ~3 МБ/с, и незавершающийся `go`
# (точка останова, до которой АДАП не доходит) за четверть часа даёт десятки
# гигабайт. Сторож снимает симулятор, как только трасса превысит лимит.
#
#   ./runsim.sh [ini-файл] [лимит-в-ГБ] [таймаут-секунд]
INI=${1:-dispak.ini}
LIMIT_GB=${2:-5}
TIMEOUT=${3:-300}
TRACE=dispak.trace
OUT=dispak.out            # stdout симулятора — рядом с проектом, НЕ в /tmp
LIMIT=$((LIMIT_GB * 1024 * 1024 * 1024))
SAMPLE=$((32 * 1024 * 1024))   # сколько байт с начала и с конца сохранять при срыве

rm -f "$TRACE" "$OUT" "$TRACE".head "$TRACE".tail "$OUT".head "$OUT".tail
timeout "$TIMEOUT" ../BIN/svs "$INI" > "$OUT" 2>&1 &
SIM=$!

while kill -0 $SIM 2>/dev/null; do
    SZ=0
    for F in "$TRACE" "$OUT"; do
        [ -f "$F" ] && SZ=$((SZ + $(stat -c %s "$F" 2>/dev/null || echo 0)))
    done
    if true; then
        if [ "$SZ" -gt "$LIMIT" ]; then
            echo "runsim: вывод превысил ${LIMIT_GB} ГБ — снимаю симулятор" >&2
            kill -9 $SIM 2>/dev/null
            wait $SIM 2>/dev/null
            # Перед удалением оставляем ОГРАНИЧЕННЫЕ образцы: без них разбирать
            # зацикливание нечем, а целиком эти файлы держать нельзя.
            for F in "$TRACE" "$OUT"; do
                [ -f "$F" ] || continue
                head -c "$SAMPLE" "$F" > "$F.head"
                tail -c "$SAMPLE" "$F" > "$F.tail"
            done
            rm -f "$TRACE" "$OUT"
            echo "runsim: образцы сохранены в *.head / *.tail (по $((SAMPLE/1024/1024)) МБ)" >&2
            exit 124
        fi
    fi
    sleep 2
done
wait $SIM
cat "$OUT"
