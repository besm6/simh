#!/bin/sh
#
# Рабочий образ диска 2048 из дистрибутивного svs2048.bin.
#
#   tools/makeSVS2048.sh [дистрибутив] [копия]
#
# По умолчанию: $SVS2048 или ~/git/besm6.github.io/download/disks/svs2048.bin
# -> svs2048-fixed.bin.
#
# У svs2048.bin те же два расхождения в служебных словах, что у svs2053.bin
# (tools/makeSVS2053.py, п.1 и 2): номер пакета в нём 04144 (2148) вместо
# 04000 (2048), а номер зоны удвоен. Правит их тот же makeSVS2053.py с
# --pack 4000. Зоны параметров ГЕНС-а (0754) на этом томе нет, так что
# архив и год (п.3 и 4) его не затрагивают.

SRC=${1:-${SVS2048:-/home/$USER/git/besm6.github.io/download/disks/svs2048.bin}}
DST=${2:-svs2048-fixed.bin}

exec python3 "$(dirname "$0")/makeSVS2053.py" --pack 4000 "$SRC" "$DST"
