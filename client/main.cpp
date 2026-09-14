// SPDX-License-Identifier: GPL-2.0-only
#include "edu_lab.h"
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include <iostream>
static_assert(sizeof(edu_lab_header) == 16);
static_assert(sizeof(edu_lab_caps) == 80);
static_assert(sizeof(edu_lab_compute) == 32);
static_assert(sizeof(edu_lab_dma) == 4128);
int main(int argc, char **argv) {
    const bool expect_timeout = argc == 2 && std::strcmp(argv[1], "--expect-timeout") == 0;
    const int fd = open("/dev/edu-lab", O_RDWR | O_CLOEXEC);
    if (fd < 0) { std::cerr << "open: " << std::strerror(errno) << '\n'; return 1; }
    edu_lab_compute req{};
    req.header = {EDU_LAB_ABI_VERSION, sizeof(req), 0, 0};
    req.input = 5;
    const int rc = ioctl(fd, EDU_LAB_COMPUTE, &req);
    const int saved_errno = errno;
    if (expect_timeout) {
        edu_lab_caps caps{};
        caps.header = {EDU_LAB_ABI_VERSION, sizeof(caps), 0, 0};
        const bool caps_ok = ioctl(fd, EDU_LAB_GET_CAPS, &caps) == 0;
        errno = 0;
        const int again = ioctl(fd, EDU_LAB_COMPUTE, &req);
        const bool failed_closed = again < 0 && errno == EIO;
        close(fd);
        const bool ok = rc < 0 && saved_errno == ETIMEDOUT && caps_ok &&
            caps.state == EDU_LAB_FAILED && caps.timeouts == 1 && caps.interrupts >= 1 && failed_closed;
        std::cout << "TIMEOUT " << (ok ? "PASS" : "FAIL") << '\n';
        return ok ? 0 : 1;
    }
    edu_lab_caps caps{};
    caps.header = {EDU_LAB_ABI_VERSION, sizeof(caps), 0, 0};
    const bool irq_ok = ioctl(fd, EDU_LAB_GET_CAPS, &caps) == 0 && caps.interrupts >= 1;
    close(fd);
    errno = saved_errno;
    if (rc < 0) { std::cerr << "compute: " << std::strerror(errno) << '\n'; return 1; }
    std::cout << "COMPUTE input=5 result=" << req.result << '\n';
    return req.result == 120 && irq_ok ? 0 : 1;
}
