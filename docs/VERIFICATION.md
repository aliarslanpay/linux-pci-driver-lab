# Verification notes

## Prerequisites

Use the Ubuntu 24.04 amd64 tools listed in the [README](../README.md#build).
The scripts require QEMU 8.2.2 and a static BusyBox binary. Bootstrap downloads
the checksummed `6.8.0-138.138` image and headers; the module targets
`6.8.0-138-generic`, independently of the host kernel.

The VM uses TCG, `qemu64`, two CPUs, 256 MiB RAM, and EDU at `0000:00:02.0`.
Module loading stays in the guest, which has no disk, network, or shared filesystem.

## Running the tests

Run from the repository root:

```sh
make bootstrap
make clean
make host-check sanitize
make integration
```

Builds use Kbuild `W=1` and C++17 with strict warnings. Driver and client assert
ioctl struct sizes at compile time.

Host checks cover parsing, arithmetic, RAII descriptor moves/close, CLI rejection,
failed opens, and an ioctl on `/dev/null`. `sanitize` repeats them with ASan/UBSan.
Successful checks print:

```text
HOST_TEST PASS parser, ABI, arithmetic, RAII moves/close
HOST_CHECK PASS CLI rejection and observable fd/ioctl errors
```

Check shell syntax separately:

```sh
for script in scripts/*.sh guest/init; do sh -n "$script" || exit 1; done
```

## Guest integration

`make integration` builds the module and static clients, packages the initramfs,
and runs [guest/init](../guest/init). The test sequence checks:

| Test | Check |
| --- | --- |
| Self-test | Factorials 0–12; 15 DMA size/offset combinations; 43 completion interrupts; 15 rejected ioctl requests |
| Thread contention | Four descriptors, four iterations each; 16 computations and 16 DMA loopbacks |
| Process contention | Two separate client processes, each completing three computations and three DMA loopbacks |
| Lifecycle | An open fd prevents module removal; unbind waits for an in-flight DMA; the stale fd returns `ENODEV`; rebind succeeds |
| Timeout injection | Compute and DMA with `test_drop_irq=1` return `ETIMEDOUT`; status becomes `FAILED`; subsequent valid operations return `EIO` |
| Probe failure injection | `test_fail_probe=1..6` leaves no misc node; each stage is followed by a normal reload and `5! = 120` |
| Reload | Five load/unload cycles, each with a 64-byte DMA loopback at offset 4032; the node and module disappear after unload |

DMA lengths are 1, 2, 63, 64, 255, 1024, 4095, and 4096 at offset zero and,
where distinct, at the buffer end. Every returned byte is compared. Rejected
requests cover ABI/reserved fields, ranges, unknown or incorrectly sized commands,
and a null pointer. The interrupt delta is 13 factorials plus two per loopback.

A successful run includes these lines:

```text
SELF_TEST PASS computations=13 dma_loopbacks=15 interrupts=43 rejected=15
LIFECYCLE PASS pinned-module, in-flight DMA unbind, stale-fd ENODEV, rebind
PROBE_FAILURES PASS stages=6 each-followed-by-successful-reload
RELOAD PASS cycles=5
KERNEL_DIAGNOSTICS PASS no fault/warning signatures; slub_debug=FZP
EDU_LAB_INTEGRATION_PASS
```

Stress output includes payload bytes, `EBUSY` retries, and elapsed time.
`roundtrip_payload_bytes` counts each payload once across two DMA transfers.
Retries and timings vary with scheduling and describe QEMU execution, rather
than physical PCI performance. The shared client deadline bounds retries; an
in-progress ioctl can finish after it. Hardware waits are one second per
computation or DMA leg.

`build/guest.log` contains serial output. Integration fails on a QEMU error,
VM timeout, missing pass marker, or selected diagnostic signatures. The guest
scans dmesg for BUG, WARNING, Oops, panic, protection fault, and slab errors,
with `slub_debug=FZP` and `panic_on_warn=1` enabled.

The VM deadline defaults to 120 seconds; change it with
`GUEST_TIMEOUT=180 make integration`.
`make guest` shows raw serial output without the wrapper's log checks.

## Interactive guest

For manual testing, keep the three mount commands at the start of `guest/init`
and replace the subsequent test sequence with `exec /bin/sh`. Rebuild and boot
with `make guest`. Inside that guest shell:

```sh
insmod /edu_lab.ko
/edu-client info
/edu-client compute 12
/edu-client dma 64 4032
/edu-client self-test
/edu-client stress 4 4 30
rmmod edu_lab
poweroff -f
```

For a focused timeout check, load a fresh binding with
`insmod /edu_lab.ko test_drop_irq=1`, then run
`/edu-client expect-timeout compute` or `/edu-client expect-timeout dma`.
Unload/reload between the two checks because a timeout leaves the binding failed.

## Limitations

- Tests cover QEMU EDU and x86-64 callers. They do not cover physical hardware,
  physical hot-unplug, 32-bit userspace, IOMMU isolation, or power management.
- `test_drop_irq` suppresses software completion while hardware finishes; it
  does not test stuck DMA. Probe injection checks cleanup at six acquisition
  stages, rather than allocation exhaustion or MSI failure. Both hooks default off.
- Invalid-pointer coverage is limited to null; corrupt non-null mappings and
  adversarial loads are outside the test suite.
- ASan/UBSan cover the userspace client. The guest configuration uses SLUB
  checks and log scanning, not kernel KASAN, lockdep, or DMA-API debug coverage.
- The unsigned, out-of-tree module produces guest taint notices. Injected probe
  failures log error `-5`. Kbuild may warn about a compiler identity mismatch or
  skip BTF generation without `vmlinux`.

The [CI workflow](../.github/workflows/ci.yml) runs the same bootstrap, host
checks, sanitizers, and guest integration on `ubuntu-24.04`, and uploads the
guest log.
