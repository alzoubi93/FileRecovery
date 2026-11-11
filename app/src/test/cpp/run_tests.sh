#!/usr/bin/env bash
# يشغّل كل اختبارات المحرك على الحاسوب: عادي، بذاكرة مؤقتة صغيرة، ASan/UBSan، وتشويش.
set -e
cd "$(dirname "$0")"
INC=../../main/cpp
for M in 8192 16384 65536 262144; do
  g++ -std=c++17 -O2 -Wall -Wextra -DCARVER_MAX_FILE=$M -I$INC carver_test.cpp -o /tmp/ct_$M
  echo "[buffer ${M}x2] $(/tmp/ct_$M | tail -1)"
done
g++ -std=c++17 -O2 -I$INC carver_test.cpp -o /tmp/ct_prod && echo "[production 32MB] $(/tmp/ct_prod | tail -1)"
for M in 8192 262144; do
  g++ -std=c++17 -g -O1 -fsanitize=address,undefined -DCARVER_MAX_FILE=$M -I$INC carver_test.cpp -o /tmp/ct_asan_$M
  echo "[ASan+UBSan buffer ${M}x2] $(/tmp/ct_asan_$M 2>&1 | tail -1)"
  echo "[fuzz 300, buffer ${M}x2] $(CARVER_FUZZ=300 /tmp/ct_asan_$M 2>&1 | tail -1)"
done
