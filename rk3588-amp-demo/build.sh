#!/usr/bin/env bash
# Build the minimal RK3588 AMP demo firmware -> amp1.bin
set -e
CROSS="${CROSS:-aarch64-linux-gnu-}"
cd "$(dirname "$(realpath "$0")")"

"${CROSS}gcc" -ffreestanding -nostdlib -mgeneral-regs-only -O2 -Wall -c amp_start.S -o amp_start.o
"${CROSS}gcc" -ffreestanding -nostdlib -mgeneral-regs-only -O2 -Wall -c amp1.c -o amp1.o
"${CROSS}ld" -T amp.ld amp_start.o amp1.o -o amp1.elf
"${CROSS}objcopy" -O binary amp1.elf amp1.bin
echo "amp1.bin ready ($(stat -c%s amp1.bin) bytes), load/entry=0x30000000, cpu=0x300, stack=0x30200000"
