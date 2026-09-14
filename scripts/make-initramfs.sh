#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
: "${CACHE:=$HOME/.cache/linux-pci-driver-lab}"
# Static userspace avoids a guest distribution and shared-library packaging.
make -C "$root" build/edu-client-static
stage="$root/build/initramfs"
rm -rf "$stage"
mkdir -p "$stage/bin" "$stage/dev" "$stage/proc" "$stage/sys" "$stage/tmp"
cp "${BUSYBOX:-/bin/busybox}" "$stage/bin/busybox"
for app in sh mount umount insmod rmmod poweroff dmesg cat echo sleep test grep awk timeout; do
    ln -s busybox "$stage/bin/$app"
done
cp "$root/guest/init" "$stage/init"
cp "$root/driver/edu_lab.ko" "$stage/edu_lab.ko"
cp "$root/build/edu-client-static" "$stage/edu-client"
(cd "$stage" && find . -print0 | cpio --null -o --format=newc 2>/dev/null | gzip -n) > "$root/build/initramfs.cpio.gz"
