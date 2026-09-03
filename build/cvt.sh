#!/bin/bash
cd /mnt/f/myos/build
test -x ppm2bmp || (cd /mnt/f/myos && gcc -O2 -o build/ppm2bmp scripts/ppm2bmp.c && cd build)
for n in example google; do ./ppm2bmp "t-$n.ppm" "t-$n.bmp" && echo "ok-$n"; done
