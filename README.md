# linux-pci-driver-lab

A Linux PCI driver in C for QEMU's EDU device, with a C++17 client for factorial
requests, DMA loopbacks, and concurrent tests. It runs in a disposable x86-64
guest; no physical PCI hardware is required.

## Implementation

- PCI probe/remove, BAR0 MMIO access, and one MSI vector for completion.
- A versioned ioctl ABI on `/dev/edu-lab` for capabilities, factorials 0–12, and
  DMA loopbacks through a 4096-byte coherent buffer with a 28-bit DMA mask.
- One operation at a time. Contending ioctls return `EBUSY`; each hardware wait
  has a one-second timeout. A timeout leaves the binding in `FAILED`, requiring
  unload/reload or unbind/rebind before further operations.
- Open descriptors hold a reference to the device object. Remove waits for the
  current operation, releases PCI resources, and leaves stale descriptors
  returning `ENODEV`.

The client checks computation results and returned DMA bytes. Stress workers
use separate RAII descriptors and retry `EBUSY` within a shared deadline.

## Build

The build recipe uses Ubuntu 24.04 amd64, GCC/G++ 13, and QEMU 8.2.2. The guest
launcher requires that QEMU version and uses TCG, so KVM is unnecessary. Keep
the source path free of spaces for Kbuild.

```sh
sudo apt-get update
sudo apt-get install -y --no-install-recommends \
  g++-13 make curl dpkg-dev libelf-dev qemu-system-x86 busybox-static cpio gzip
qemu-system-x86_64 --version
make bootstrap
make all
```

`make bootstrap` downloads and checksums the guest kernel and matching headers,
then extracts them under `~/.cache/linux-pci-driver-lab`. It needs Internet
access to Ubuntu's snapshot service and does not install a kernel on the host.
Package filenames and SHA-256 hashes are in
[scripts/kernel-packages.txt](scripts/kernel-packages.txt).

`make all` builds the client and module. The module targets
`6.8.0-138-generic` (package version `6.8.0-138.138`), rather than the running
host kernel. `make clean` removes build outputs.

## Testing

```sh
make host-check sanitize
make integration
```

Host checks cover argument parsing, descriptor ownership, and error paths;
`sanitize` repeats them with ASan/UBSan. Integration builds a BusyBox initramfs,
boots QEMU, and runs the guest tests: factorials, DMA boundaries, invalid ioctls,
thread/process contention, timeouts, probe failures, and unbind/rebind.

The guest has no disk, network adapter, or shared filesystem. Module loading
stays inside the guest, and build/test commands need no root access once the
host tools are installed.

Integration saves serial output to `build/guest.log`, requires
`EDU_LAB_INTEGRATION_PASS`, and rejects selected kernel/QEMU error signatures.
The VM deadline defaults to 120 seconds; use
`GUEST_TIMEOUT=180 make integration` to change it. `make guest` runs the same
guest with raw serial output.

The [CI workflow](.github/workflows/ci.yml) runs bootstrap, host checks,
sanitizers, and TCG integration on Ubuntu 24.04, and uploads the guest log.

## Guest client

Commands available inside the guest:

```sh
/edu-client info
/edu-client compute 12
/edu-client dma 64 4032
/edu-client self-test
/edu-client stress 4 4 30  # iterations per client, clients, whole-run seconds
```

The default `/init` runs the tests and powers down. For an interactive shell,
see [verification notes](docs/VERIFICATION.md#interactive-guest).
`--device PATH` selects a misc node; `--help` lists commands and limits.

## Documentation

- [Architecture](docs/ARCHITECTURE.md): synchronization, DMA ownership, and cleanup.
- [ioctl ABI](docs/ABI.md): layouts, commands, and error codes.
- [Verification notes](docs/VERIFICATION.md): test commands, expected output, and limits.
- [Dependency notices](THIRD_PARTY_NOTICES.md) and [license](LICENSE).
