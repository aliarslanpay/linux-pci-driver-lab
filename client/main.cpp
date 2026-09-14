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
int main() {
    const int fd = open("/dev/edu-lab", O_RDWR | O_CLOEXEC);
    if (fd < 0) { std::cerr << "open: " << std::strerror(errno) << '\n'; return 1; }
    edu_lab_compute req{};
    req.header = {EDU_LAB_ABI_VERSION, sizeof(req), 0, 0};
    req.input = 5;
    const int rc = ioctl(fd, EDU_LAB_COMPUTE, &req);
    close(fd);
    if (rc < 0) { std::cerr << "compute: " << std::strerror(errno) << '\n'; return 1; }
    std::cout << "COMPUTE input=5 result=" << req.result << '\n';
    return req.result == 120 ? 0 : 1;
}
