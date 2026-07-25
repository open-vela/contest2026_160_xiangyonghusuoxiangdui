#!/bin/sh
# Build the minimal RK3588 PMU M0 heartbeat firmware -> m0.bin
#
# Toolchain: arm-none-eabi (Cortex-M0 / ARMv6-M / Thumb). Override with:
#   CROSS=/path/to/arm-none-eabi- ./build-m0.sh
set -e
CROSS=${CROSS:-arm-none-eabi-}
cd "$(dirname "$0")"

${CROSS}gcc -mcpu=cortex-m0 -mthumb -nostartfiles -nostdlib \
	-Wl,-T,m0.ld -Wl,--build-id=none -o m0.elf m0_start.S
${CROSS}objcopy -O binary m0.elf m0.bin

echo "=== disassembly ==="
${CROSS}objdump -d m0.elf
echo "=== sections ==="
${CROSS}objdump -h m0.elf
echo "=== m0.bin ==="
ls -l m0.bin
