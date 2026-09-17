#!/bin/sh
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
client=${1:-$root/build/edu-client}
timeout 10 "$client" host-test
timeout 10 "$client" --help > /dev/null
expect_fail() {
    pattern=$1
    shift
    if timeout 10 "$client" "$@" > "$root/build/host-error.log" 2>&1; then
        echo "unexpected success: $*" >&2
        exit 1
    fi
    grep -q "$pattern" "$root/build/host-error.log"
}
expect_fail 'numeric argument exceeds limit' compute 13
expect_fail 'unsigned decimal integer' compute -1
expect_fail 'DMA exceeds EDU buffer' dma 2 4095
expect_fail 'numeric argument exceeds limit' stress 101 1 10
expect_fail 'numeric argument below limit' stress 1 0 10
expect_fail 'invalid command' nonsense
expect_fail 'open .*No such file' --device /does-not-exist/edu-lab info
expect_fail 'ioctl: Inappropriate ioctl' --device /dev/null info
echo 'HOST_CHECK PASS CLI rejection and observable fd/ioctl errors'
