# Architecture

## Scope and data path

The supported system is x86-64 Linux 6.8.0-138-generic with QEMU 8.2.2 EDU
`1234:11e8`, BAR0 MMIO (1 MiB), identification `0x010000ed`, and one MSI vector.
The guest recipe fixes EDU at PCI address `0000:00:02.0`. `/dev/edu-lab` is a
root-only misc device (0600). A single EDU instance is supported; the misc name
is intentionally fixed. 32-bit compat ioctls are not implemented or tested.

The request path is:

```text
userspace -> ioctl validation -> nonblocking operation mutex -> MMIO command
          -> EDU completion IRQ -> acknowledgement + kernel completion
          -> userspace result
```

The driver never exposes raw registers, DMA addresses, or a user-selected CPU
address. Factorial input is restricted to 0–12 to avoid EDU's 32-bit overflow.

## Synchronization and state

`op_mutex` protects operation state, MMIO programming, the coherent buffer, and
completed-operation/timeout counters. The IRQ handler has a separate spinlock
for `expected_irq`, its counter, and completion signaling. It acknowledges and
flushes the actual interrupt status before waking the owner. State transitions:

| Current state | Event | Next state / result |
| --- | --- | --- |
| IDLE | valid compute | COMPUTE, then IDLE on MSI completion |
| IDLE | valid loopback | DMA_TO, then DMA_FROM, then IDLE |
| Any active operation | another ioctl | EBUSY; no queued request |
| Active operation | wait expires | FAILED; ETIMEDOUT |
| FAILED | valid compute/DMA | EIO; no hardware command |
| FAILED | capabilities | FAILED status and counters |
| Bound device | remove after acquiring mutex | REMOVED |
| REMOVED, existing fd | ioctl | ENODEV |

Preparation clears stale IRQ status and reinitializes the completion under the
IRQ lock before starting hardware. Successful completion checks that the
relevant busy bit is clear. Each wait uses `wait_for_completion_timeout`
(1000 ms), not an unbounded busy loop. DMA has two waits, one per direction.
Rejected requests do not enter hardware. The IRQ handler cannot start another
transfer.

A timeout is terminal for that binding. This avoids attributing a late IRQ to a
new operation and prevents reusing or modifying a DMA buffer still owned by EDU.
The default-off, load-time `test_drop_irq=1` hook acknowledges real IRQs but
suppresses completion signaling. It exercises a missing software completion,
not an electrically stuck device or a DMA engine that never stops.

## DMA ownership

The driver configures streaming/coherent masks to 28 bits, allocates one
4096-byte buffer with `dma_alloc_coherent`, and programs the returned **DMA
handle** using `writeq`. The CPU pointer is used only for CPU copies. It also
checks that the allocation's last byte fits the mask. Kernel memory for the
4128-byte ioctl request is allocated on the heap to avoid a large kernel stack
frame.

Offsets refer to EDU's on-device buffer at `0x40000`. Length must be 1–4096 and
`length <= 4096 - offset`; this rejects wraparound. The first DMA copies CPU RAM
to EDU, then the host buffer is cleared, then the second DMA copies EDU back to
RAM. The client checks every byte. Clearing the host contents prevents a false
pass from accidentally returning the original CPU bytes without the inbound
DMA. CPU access occurs only before command submission or after completion; DMA
barriers order coherent-buffer access and command/result processing.

EDU adds an emulated 100 ms timer to each DMA leg in the pinned upstream source,
so a loopback takes roughly 200 ms in this configuration. This is emulator
behavior rather than a bandwidth optimization target.

## Resource lifetime and failure paths

Acquisition follows this order:

```text
device enable -> BAR reservation -> DMA mask -> MMIO mapping
              -> liveness/idle checks -> coherent buffer -> bus master
              -> MSI vector -> IRQ handler -> misc registration
```

Error paths unwind resources in reverse order. `test_fail_probe=1..6` injects
default-off failures after major acquisitions; every injection is followed by a
successful normal load and computation.

Open descriptors pin the module via `.owner` and hold a `kref` to the per-device
object. Remove deregisters the misc node, takes the operation mutex, marks the
object removed, disables computation IRQ generation, and polls boundedly for
engine quiescence. It then revokes PCI bus mastering before releasing the IRQ,
vector, and DMA allocation; finally it unmaps/releases BAR0 and disables PCI.
Existing descriptors retain only the small removed object and never touch the
released BAR or PCI pointer. Reprobe refuses busy engines before enabling DMA.

The default EDU exposes no documented transfer cancellation/reset. If the
remove drain expires, bus mastering is revoked before memory is freed and an
error is logged. In the supported QEMU PCI model, disabled bus mastering blocks
DMA access; this is not a general recovery recipe for unknown physical hardware.
The tested timeout hook lets hardware finish; genuinely wedged DMA was not
tested.

## Limitations

The driver requires MSI and does not fall back to shared INTx. It uses one
coherent DMA buffer and a single operation queue; there is no streaming DMA
mapping, zero-copy path, multiple queues, or asynchronous userspace API. Access
is root-only. Power management, AER recovery, physical hot-unplug, and
production-hardware support are outside the scope of this project.

## References

- [QEMU EDU specification](https://github.com/qemu/qemu/blob/v8.2.2/docs/specs/edu.rst)
- [QEMU EDU implementation](https://github.com/qemu/qemu/blob/v8.2.2/hw/misc/edu.c)
- [Linux 6.8 PCI guide](https://docs.kernel.org/6.8/PCI/pci.html)
- [Linux 6.8 DMA guide](https://docs.kernel.org/6.8/core-api/dma-api-howto.html)
