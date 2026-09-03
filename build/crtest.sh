#!/bin/bash
cd /mnt/f/myos
SP="/mnt/c/Users/Developer/AppData/Local/Temp/claude/f--myos/11185cec-5879-4f1b-b499-19f06b52d664/scratchpad"
gcc -O2 -I kernel -o "$SP/test_crypto" "$SP/test_crypto.c" kernel/sha256.c kernel/aes.c kernel/x25519.c kernel/string.c 2>&1 | head -20
"$SP/test_crypto"
