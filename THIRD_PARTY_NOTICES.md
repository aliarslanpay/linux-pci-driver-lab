# Third-party software

The project source is licensed under GPL-2.0-only; see `LICENSE`. The shared
UAPI header additionally uses the Linux-syscall-note exception, reproduced in
`LICENSES/exceptions/Linux-syscall-note`.

The build and test setup relies on external software including QEMU, Linux,
BusyBox, GCC, and Ubuntu packages. These dependencies are downloaded or provided
by the host system and are not vendored in this repository. The driver targets
QEMU's published EDU interface; no QEMU device code or Linux kernel source is
copied into the project.

| External dependency | Upstream licensing/source |
| --- | --- |
| QEMU 8.2.2 EDU | GPL-2.0; https://www.qemu.org/ |
| Ubuntu Linux image/headers | Linux GPL-2.0 with applicable file exceptions; https://kernel.ubuntu.com/ |
| BusyBox 1.36.1 static | GPL-2.0; https://busybox.net/ |
| SeaBIOS | LGPL-3.0; https://www.seabios.org/ |
| GNU GCC/libstdc++ | GPL-3.0, with the applicable GCC Runtime Library Exception; https://gcc.gnu.org/ |
| GNU cpio/make | GPL-3.0; https://www.gnu.org/ |

For complete license and copyright information for these dependencies, refer to
the corresponding upstream projects and packages. The bootstrap script uses
Ubuntu's package service and verifies pinned hashes. Generated guest images and
toolchains are not distributed with this repository.
