# ioctl ABI version 1

The authoritative shared header is `include/edu_lab.h`. Fixed-width fields,
explicit reserved words and aligned 64-bit counters give stable layouts.
There are no embedded userspace pointers. Only x86-64 callers are supported.
Driver and client assert struct sizes at compile time.

Every request begins with this 16-byte header:

| Offset | Field | Required input |
| --- | --- | --- |
| 0 | u32 version | 1 |
| 4 | u32 size | exact full request struct size |
| 8 | u32 flags | 0 |
| 12 | u32 reserved | 0 |

| Command | Size | Input | Output |
| --- | --- | --- | --- |
| GET_CAPS, magic E/nr 0 | 80 | header | EDU ID, limits, features, state and four counters |
| COMPUTE, magic E/nr 1 | 32 | header, input 0–12, zero reserved[2] | u32 factorial result |
| DMA_LOOPBACK, magic E/nr 2 | 4128 | header, EDU offset, length, zero reserved[2], inline data | returned bytes in same inline array |

All three use `_IOWR`. An unrecognized command, including a changed encoded
size, returns ENOTTY. The header's own wrong size/version or nonzero
flags/reserved return EINVAL. Capabilities fields beyond the header are output
only; their incoming values are ignored and output is initialized to zero.
Compute `result` is output only. DMA data beyond `length` is returned unchanged
from the request; no uninitialized kernel bytes are exposed.

DMA payload starts at byte 32. Computation input/result start at bytes 16/20.
Capabilities counters start at byte 48: computations, DMA loopbacks,
interrupts, timeouts. Counters reset per successful probe. A completed hardware
operation is counted even if copying its result back to userspace fails.

| errno | Meaning |
| --- | --- |
| EINVAL | unsupported header, reserved fields or operation range |
| EFAULT | unreadable/writable request memory |
| ENOTTY | unknown ioctl command or encoded layout |
| EBUSY | device operation mutex currently owned; retry within a deadline |
| ETIMEDOUT | first wait failure; device enters FAILED |
| EIO | subsequent valid operation on FAILED, or inconsistent completion |
| ENODEV | binding removed; existing fd remains safe to close |
| ENOMEM | allocation could not be satisfied |

GET_CAPS also returns EBUSY during an operation. No cancel ioctl is supplied.
Recovery is unload/reload (after closing fds) or controlled sysfs unbind/rebind
in the guest. EDU must be idle before reprobe enables bus mastering.
