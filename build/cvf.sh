#!/bin/bash
cd /mnt/f/myos/build
test -x ppm2bmp || (cd /mnt/f/myos && gcc -O2 -o build/ppm2bmp scripts/ppm2bmp.c)
cd /mnt/f/myos/build
for n in https google; do ./ppm2bmp "f-$n.ppm" "f-$n.bmp" && echo "ok-$n"; done
