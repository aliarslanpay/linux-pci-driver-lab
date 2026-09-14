#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
: "${CACHE:=$HOME/.cache/linux-pci-driver-lab}"
: "${GUEST_TIMEOUT:=120}"
timeout --foreground "$GUEST_TIMEOUT" "${QEMU:-qemu-system-x86_64}" ${QEMU_EXTRA_ARGS:-} \
    -machine pc -accel tcg -cpu qemu64 -m 256M -smp 2 \
    -nodefaults -no-reboot -nographic -serial mon:stdio \
    -device edu -kernel "$CACHE/guest/boot/vmlinuz-6.8.0-138-generic" \
    -initrd "$root/build/initramfs.cpio.gz" \
    -append "console=ttyS0 rdinit=/init panic=1 loglevel=6"
