#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
: "${CACHE:=$HOME/.cache/linux-pci-driver-lab}"
: "${GUEST_TIMEOUT:=120}"
: "${QEMU:=qemu-system-x86_64}"
version=$("$QEMU" --version | head -n 1)
case "$version" in
    *'version 8.2.2 '*) ;;
    *) echo "Expected QEMU 8.2.2; found: $version" >&2; exit 1 ;;
esac
set -- -machine pc -accel tcg -cpu qemu64 -m 256M -smp 2 \
    -nodefaults -no-reboot -nographic -serial mon:stdio \
    -device edu,addr=02.0 -kernel "$CACHE/guest/boot/vmlinuz-6.8.0-138-generic" \
    -initrd "$root/build/initramfs.cpio.gz" \
    -append 'console=ttyS0 rdinit=/init panic=1 panic_on_warn=1 loglevel=6 slub_debug=FZP'
# Overrides support extracted packages without changing the normal host setup.
if test -n "${QEMU_DATA_DIR:-}"; then set -- "$@" -L "$QEMU_DATA_DIR"; fi
if test -n "${QEMU_BIOS:-}"; then set -- "$@" -bios "$QEMU_BIOS"; fi
timeout --foreground "$GUEST_TIMEOUT" "$QEMU" "$@"
