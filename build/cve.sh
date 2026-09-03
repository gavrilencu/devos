#!/bin/bash
cd /mnt/f/myos
test -x build/ppm2bmp || gcc -O2 -o build/ppm2bmp scripts/ppm2bmp.c
cd build
for n in home typed edited loaded; do
  ./ppm2bmp "e-$n.ppm" "e-$n.bmp" && echo "ok-$n"
done
