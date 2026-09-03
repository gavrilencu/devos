#!/bin/bash
cd /mnt/f/myos/build
for n in home clicked; do ./ppm2bmp "c-$n.ppm" "c-$n.bmp" && echo "ok-$n"; done
