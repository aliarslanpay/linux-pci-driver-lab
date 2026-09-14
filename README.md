# linux-pci-driver-lab
Personal project targeting QEMU 8.2.2's emulated EDU PCI device and Ubuntu's
6.8.0-138-generic kernel. Experimental modules run only in the disposable guest.

Ubuntu 24.04 amd64 host prerequisites: gcc/g++ 13, make, curl, dpkg-dev,
libelf-dev, qemu-system-x86 (8.2.2), busybox-static, cpio, gzip.
Run `make bootstrap`, then `make guest`. Kernel inputs are pinned by SHA-256.
No full kernel build, disk image, host module insertion or networked guest.
