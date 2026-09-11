#!/bin/bash
# Boot the locally restored retailOS 2.0.4 disk through Boot ROM and NOR EFI.
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
: "${IPOD_DISK:=../ipod_files/ipod_hdd_restored.img}"
: "${IPOD_NOR:=../ipod_files/ipod_nor_restored.bin}"
: "${IPOD_BOOTROM:=../ipod_files/bootrom_patched.bin}"
: "${IPOD_QEMU:=build/qemu-system-arm}"
: "${ICOUNT:=shift=4}"
exec "$IPOD_QEMU" \
    -M "ipod-classic,bootrom=$IPOD_BOOTROM" \
    -global s5l8702-aes.fused-key-bypass=on \
    -drive "if=mtd,format=raw,file=$IPOD_NOR" \
    -drive "if=ide,format=raw,file=$IPOD_DISK" \
    -icount "$ICOUNT" \
    "$@"
