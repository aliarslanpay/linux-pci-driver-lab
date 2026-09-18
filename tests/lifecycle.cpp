// SPDX-License-Identifier: GPL-2.0-only
// Guest-only sysfs unbind test; this is not physical hot-unplug coverage.
#include "edu_lab.h"
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <iostream>
#include <thread>

static bool sysfs_write(const char *path, const char *value) {
    const int fd = ::open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0) return false;
    const auto n = ::write(fd, value, std::strlen(value));
    ::close(fd);
    return n == static_cast<ssize_t>(std::strlen(value));
}
int main(int argc, char **argv) {
    if (argc != 2) return 1;
    const int fd = ::open("/dev/edu-lab", O_RDWR | O_CLOEXEC);
    if (fd < 0) return 1;
    errno = 0;
    if (::syscall(SYS_delete_module, "edu_lab", O_NONBLOCK) != -1 || errno != EWOULDBLOCK) {
        ::close(fd); return 1;
    }
    std::atomic<int> transfer{-2};
    std::thread worker([&] {
        edu_lab_dma d{};
        d.header = {EDU_LAB_ABI_VERSION, sizeof(d), 0, 0};
        d.length = 4096;
        for (unsigned i = 0; i < 4096; ++i) d.data[i] = static_cast<unsigned char>(i * 23 + 7);
        const int rc = ::ioctl(fd, EDU_LAB_DMA_LOOPBACK, &d);
        bool correct = rc == 0;
        for (unsigned i = 0; i < 4096; ++i)
            correct = correct && d.data[i] == static_cast<unsigned char>(i * 23 + 7);
        transfer = correct ? 0 : 1;
    });
    bool observed_busy = false;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline && transfer == -2) {
        edu_lab_caps c{}; c.header = {EDU_LAB_ABI_VERSION, sizeof(c), 0, 0};
        if (::ioctl(fd, EDU_LAB_GET_CAPS, &c) < 0 && errno == EBUSY) {
            observed_busy = true; break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const bool unbound = observed_busy && sysfs_write("/sys/bus/pci/drivers/edu_lab/unbind", argv[1]);
    worker.join();
    edu_lab_caps c{}; c.header = {EDU_LAB_ABI_VERSION, sizeof(c), 0, 0};
    errno = 0;
    const int rc = ::ioctl(fd, EDU_LAB_GET_CAPS, &c);
    const bool revoked = rc == -1 && errno == ENODEV;
    ::close(fd);
    if (!unbound || !revoked || transfer != 0) return 1;
    if (!sysfs_write("/sys/bus/pci/drivers/edu_lab/bind", argv[1])) return 1;
    std::cout << "LIFECYCLE PASS pinned-module, in-flight DMA unbind, stale-fd ENODEV, rebind\n";
}
