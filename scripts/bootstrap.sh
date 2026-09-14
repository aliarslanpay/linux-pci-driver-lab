#!/bin/sh
set -eu
: "${CACHE:=$HOME/.cache/linux-pci-driver-lab}"
base=https://snapshot.ubuntu.com/ubuntu/20260828T000000Z
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
mkdir -p "$CACHE/packages" "$CACHE/guest"
while read -r hash path; do
    file="$CACHE/packages/${path##*/}"
    if ! test -f "$file"; then
        curl -fL --retry 2 --connect-timeout 15 --max-time 180 "$base/$path" -o "$file.tmp"
        mv "$file.tmp" "$file"
    fi
    printf '%s  %s\n' "$hash" "$file" | sha256sum -c -
    dpkg-deb -x "$file" "$CACHE/guest"
done < "$root/scripts/kernel-packages.txt"
printf 'Guest inputs ready: %s\n' "$CACHE/guest"
