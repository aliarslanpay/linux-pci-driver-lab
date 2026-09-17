// SPDX-License-Identifier: GPL-2.0-only
#include "edu_lab.h"
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

static_assert(sizeof(edu_lab_header) == 16);
static_assert(sizeof(edu_lab_caps) == 80);
static_assert(sizeof(edu_lab_compute) == 32);
static_assert(sizeof(edu_lab_dma) == 4128);
static_assert(offsetof(edu_lab_caps, computations) == 48);
static_assert(offsetof(edu_lab_dma, data) == 32);
using Clock = std::chrono::steady_clock;

class Fd {
    int fd_;
public:
    explicit Fd(const std::string &path) : fd_(::open(path.c_str(), O_RDWR | O_CLOEXEC)) {
        if (fd_ < 0) throw std::runtime_error("open " + path + ": " + std::strerror(errno));
    }
    ~Fd() { if (fd_ >= 0) ::close(fd_); }
    Fd(const Fd &) = delete;
    Fd &operator=(const Fd &) = delete;
    Fd(Fd &&other) noexcept : fd_(std::exchange(other.fd_, -1)) {}
    Fd &operator=(Fd &&other) noexcept {
        if (this != &other) {
            if (fd_ >= 0) ::close(fd_);
            fd_ = std::exchange(other.fd_, -1);
        }
        return *this;
    }
    int get() const { return fd_; }
};

static void require(bool ok, const std::string &message) {
    if (!ok) throw std::runtime_error(message);
}
static std::uint32_t number(const std::string &text, std::uint32_t min, std::uint32_t max) {
    require(!text.empty(), "empty numeric argument");
    std::uint64_t value = 0;
    for (char c : text) {
        require(c >= '0' && c <= '9', "expected an unsigned decimal integer: " + text);
        value = value * 10 + static_cast<unsigned>(c - '0');
        require(value <= max, "numeric argument exceeds limit: " + text);
    }
    require(value >= min, "numeric argument below limit: " + text);
    return static_cast<std::uint32_t>(value);
}
template <class T> static T request() {
    T req{};
    req.header = {EDU_LAB_ABI_VERSION, sizeof(T), 0, 0};
    return req;
}

// Retry only contention; the shared deadline bounds the whole stress run.
static void call(int fd, unsigned long cmd, void *req, Clock::time_point deadline,
                 std::uint64_t *busy = nullptr) {
    for (;;) {
        require(Clock::now() < deadline, "client deadline exceeded");
        if (::ioctl(fd, cmd, req) == 0) return;
        const int error = errno;
        if (error != EBUSY) throw std::runtime_error("ioctl: " + std::string(std::strerror(error)));
        if (busy) ++*busy;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}
static edu_lab_caps caps(int fd) {
    auto req = request<edu_lab_caps>();
    call(fd, EDU_LAB_GET_CAPS, &req, Clock::now() + std::chrono::seconds(5));
    return req;
}
static std::uint32_t factorial(std::uint32_t n) {
    std::uint32_t result = 1;
    for (std::uint32_t i = 2; i <= n; ++i) result *= i;
    return result;
}
static void compute(int fd, std::uint32_t n, Clock::time_point deadline, std::uint64_t *busy = nullptr) {
    auto req = request<edu_lab_compute>();
    req.input = n;
    call(fd, EDU_LAB_COMPUTE, &req, deadline, busy);
    require(req.result == factorial(n), "factorial result mismatch");
}
static unsigned char pattern(std::uint32_t i, std::uint32_t seed) {
    return static_cast<unsigned char>((i * 37U + seed * 53U + (i >> 3U) + 19U) & 255U);
}
static void dma(int fd, std::uint32_t length, std::uint32_t offset, std::uint32_t seed,
                Clock::time_point deadline, std::uint64_t *busy = nullptr) {
    auto req = request<edu_lab_dma>();
    req.length = length;
    req.offset = offset;
    for (std::uint32_t i = 0; i < length; ++i) req.data[i] = pattern(i, seed);
    call(fd, EDU_LAB_DMA_LOOPBACK, &req, deadline, busy);
    for (std::uint32_t i = 0; i < length; ++i)
        require(req.data[i] == pattern(i, seed), "DMA byte mismatch at " + std::to_string(i));
}
static void expect_error(int fd, unsigned long cmd, void *req, int wanted, const char *name) {
    errno = 0;
    const int rc = ::ioctl(fd, cmd, req);
    const int error = errno;
    require(rc == -1 && error == wanted,
            std::string(name) + ": expected errno " + std::to_string(wanted) + ", got " + std::to_string(error));
}
static void self_test(int fd) {
    const auto before = caps(fd);
    require(before.device_id == 0x010000ed && before.state == EDU_LAB_IDLE &&
            before.dma_bytes == EDU_LAB_DMA_BYTES && before.dma_bits == 28 &&
            before.features == (EDU_LAB_FEATURE_COMPUTE | EDU_LAB_FEATURE_DMA | EDU_LAB_FEATURE_IRQ),
            "capabilities mismatch");
    const auto deadline = Clock::now() + std::chrono::seconds(20);
    for (std::uint32_t n = 0; n <= 12; ++n) compute(fd, n, deadline);
    const std::uint32_t sizes[] = {1, 2, 63, 64, 255, 1024, 4095, 4096};
    for (auto size : sizes) {
        dma(fd, size, 0, size, deadline);
        if (size != 4096) dma(fd, size, 4096 - size, size + 1, deadline);
    }
    auto c = request<edu_lab_compute>();
    c.input = 13;
    expect_error(fd, EDU_LAB_COMPUTE, &c, EINVAL, "factorial overflow");
    c.input = 0; c.header.version = 2;
    expect_error(fd, EDU_LAB_COMPUTE, &c, EINVAL, "ABI version");
    c = request<edu_lab_compute>(); c.header.size--;
    expect_error(fd, EDU_LAB_COMPUTE, &c, EINVAL, "ABI length");
    c = request<edu_lab_compute>(); c.header.flags = 1;
    expect_error(fd, EDU_LAB_COMPUTE, &c, EINVAL, "ABI flags");
    c = request<edu_lab_compute>(); c.header.reserved = 1;
    expect_error(fd, EDU_LAB_COMPUTE, &c, EINVAL, "header reserved");
    c = request<edu_lab_compute>(); c.reserved[1] = 1;
    expect_error(fd, EDU_LAB_COMPUTE, &c, EINVAL, "compute reserved");
    auto d = request<edu_lab_dma>();
    for (auto length : {0U, 4097U, std::numeric_limits<std::uint32_t>::max()}) {
        d.length = length;
        expect_error(fd, EDU_LAB_DMA_LOOPBACK, &d, EINVAL, "DMA length");
    }
    d.length = 2; d.offset = 4095;
    expect_error(fd, EDU_LAB_DMA_LOOPBACK, &d, EINVAL, "DMA end overflow");
    d.length = 1; d.offset = std::numeric_limits<std::uint32_t>::max();
    expect_error(fd, EDU_LAB_DMA_LOOPBACK, &d, EINVAL, "DMA offset overflow");
    d.offset = 0; d.reserved[0] = 1;
    expect_error(fd, EDU_LAB_DMA_LOOPBACK, &d, EINVAL, "DMA reserved");
    expect_error(fd, _IO(EDU_LAB_IOC_MAGIC, 99), &c, ENOTTY, "unknown ioctl");
    expect_error(fd, _IOWR(EDU_LAB_IOC_MAGIC, 1, edu_lab_header), &c, ENOTTY, "wrong ioctl encoded size");
    expect_error(fd, EDU_LAB_COMPUTE, nullptr, EFAULT, "null user pointer");
    const auto after = caps(fd);
    require(after.computations - before.computations == 13 &&
            after.dma_loopbacks - before.dma_loopbacks == 15 &&
            after.interrupts - before.interrupts == 43 && after.timeouts == before.timeouts &&
            after.state == EDU_LAB_IDLE, "self-test counter mismatch");
    std::cout << "SELF_TEST PASS computations=13 dma_loopbacks=15 interrupts=43 rejected=15\n";
}
static void timeout_test(int fd, bool dma_mode) {
    auto c = request<edu_lab_compute>(); c.input = 5;
    auto d = request<edu_lab_dma>(); d.length = 64;
    const auto started = Clock::now();
    expect_error(fd, dma_mode ? EDU_LAB_DMA_LOOPBACK : EDU_LAB_COMPUTE,
                 dma_mode ? static_cast<void *>(&d) : static_cast<void *>(&c), ETIMEDOUT, "suppressed completion");
    const auto elapsed = std::chrono::duration<double, std::milli>(Clock::now() - started).count();
    const auto status = caps(fd);
    require(elapsed >= status.timeout_ms * 0.8 && elapsed < status.timeout_ms + 2000.0,
            "timeout outside guest scheduling tolerance");
    require(status.state == EDU_LAB_FAILED && status.timeouts == 1 && status.interrupts >= 1,
            "timeout status mismatch");
    expect_error(fd, EDU_LAB_COMPUTE, &c, EIO, "failed compute");
    expect_error(fd, EDU_LAB_DMA_LOOPBACK, &d, EIO, "failed DMA");
    std::cout << "TIMEOUT PASS mode=" << (dma_mode ? "dma" : "compute") << " elapsed_ms=" << elapsed << '\n';
}
struct WorkerResult {
    std::uint64_t computes = 0, dmas = 0, bytes = 0, busy = 0;
    std::string error;
};
static void stress(const std::string &path, unsigned iterations, unsigned clients, unsigned seconds) {
    const auto started = Clock::now();
    const auto deadline = started + std::chrono::seconds(seconds);
    std::vector<WorkerResult> results(clients);
    std::vector<std::thread> threads;
    std::atomic<unsigned> ready{0};
    std::atomic<bool> start{false};
    // Every worker opens its own fd; the driver shares one hardware queue.
    for (unsigned k = 0; k < clients; ++k) {
        threads.emplace_back([&, k] {
            auto &r = results[k];
            ++ready;
            while (!start.load()) std::this_thread::yield();
            try {
                Fd fd(path);
                for (unsigned i = 0; i < iterations; ++i) {
                    compute(fd.get(), (i + k) % 13, deadline, &r.busy); ++r.computes;
                    const auto length = 1U + ((i * 97U + k * 251U) % 4096U);
                    const auto offset = (i + k) % (4097U - length);
                    dma(fd.get(), length, offset, i + k * 1000U, deadline, &r.busy);
                    ++r.dmas; r.bytes += length;
                }
            } catch (const std::exception &e) { r.error = e.what(); }
        });
    }
    while (ready.load() != clients) std::this_thread::yield();
    start = true;
    for (auto &thread : threads) thread.join();
    WorkerResult total;
    for (const auto &r : results) {
        total.computes += r.computes; total.dmas += r.dmas;
        total.bytes += r.bytes; total.busy += r.busy;
    }
    const auto elapsed = std::chrono::duration<double, std::milli>(Clock::now() - started).count();
    std::cout << "STRESS computations=" << total.computes << " dma_loopbacks=" << total.dmas
              << " roundtrip_payload_bytes=" << total.bytes << " busy_retries=" << total.busy
              << " elapsed_ms=" << elapsed << " clients=" << clients << " environment=QEMU_EDU_emulation\n";
    for (const auto &r : results) require(r.error.empty(), "stress worker: " + r.error);
    require(total.computes == clients * iterations && total.dmas == clients * iterations, "stress count mismatch");
}
static void host_test() {
    require(number("4096", 1, 4096) == 4096 && factorial(12) == 479001600, "host arithmetic");
    for (const auto &text : {"-1", "429496729600000", "1x", "", "0"}) {
        bool rejected = false;
        try { (void)number(text, 1, 4096); } catch (const std::exception &) { rejected = true; }
        require(rejected, "host parser accepted invalid input");
    }
    int raw;
    {
        Fd first("/dev/null"); raw = first.get();
        Fd second(std::move(first));
        Fd third("/dev/null"); third = std::move(second);
        require(third.get() == raw && ::fcntl(raw, F_GETFD) >= 0, "RAII move failed");
    }
    errno = 0;
    require(::fcntl(raw, F_GETFD) < 0 && errno == EBADF, "RAII descriptor was not closed");
    std::cout << "HOST_TEST PASS parser, ABI, arithmetic, RAII moves/close\n";
}
static void help() {
    std::cout << "Usage: edu-client [--device PATH] COMMAND [ARGS]\n"
              << "  info\n  compute N                    N: 0..12\n"
              << "  dma LENGTH [OFFSET]          within 4096-byte EDU buffer\n"
              << "  self-test\n  stress ITERATIONS CLIENTS SECONDS\n"
              << "                               1..100, 1..8, 1..120\n"
              << "  expect-timeout compute|dma    requires test_drop_irq=1\n"
              << "  host-test                    no EDU device required\n";
}
int main(int argc, char **argv) {
    try {
        std::vector<std::string> args(argv + 1, argv + argc);
        std::string path = "/dev/edu-lab";
        if (args.size() >= 2 && args[0] == "--device") {
            path = args[1]; args.erase(args.begin(), args.begin() + 2);
        }
        if (args.empty() || args[0] == "--help") { help(); return 0; }
        const auto command = args[0];
        if (command == "host-test" && args.size() == 1) { host_test(); return 0; }
        if (command == "stress" && args.size() == 4) {
            const auto iterations = number(args[1], 1, 100);
            const auto clients = number(args[2], 1, 8);
            const auto seconds = number(args[3], 1, 120);
            stress(path, iterations, clients, seconds); return 0;
        }
        if (command == "compute" && args.size() == 2) {
            const auto n = number(args[1], 0, 12); Fd fd(path);
            compute(fd.get(), n, Clock::now() + std::chrono::seconds(5));
            std::cout << "COMPUTE PASS input=" << n << " result=" << factorial(n) << '\n'; return 0;
        }
        if (command == "dma" && (args.size() == 2 || args.size() == 3)) {
            const auto size = number(args[1], 1, 4096);
            const auto offset = args.size() == 3 ? number(args[2], 0, 4095) : 0;
            require(size <= 4096 - offset, "DMA exceeds EDU buffer");
            Fd fd(path); dma(fd.get(), size, offset, 1, Clock::now() + std::chrono::seconds(5));
            std::cout << "DMA PASS bytes=" << size << " offset=" << offset << '\n'; return 0;
        }
        if (command == "info" && args.size() == 1) {
            Fd fd(path); const auto c = caps(fd.get());
            std::cout << "CAPS ABI=" << c.header.version << " state=" << c.state << " dma_bytes=" << c.dma_bytes
                      << " dma_bits=" << c.dma_bits << " timeout_ms=" << c.timeout_ms
                      << " computations=" << c.computations << " dma_loopbacks=" << c.dma_loopbacks
                      << " interrupts=" << c.interrupts << " timeouts=" << c.timeouts << '\n'; return 0;
        }
        if (command == "self-test" && args.size() == 1) { Fd fd(path); self_test(fd.get()); return 0; }
        if (command == "expect-timeout" && args.size() == 2 && (args[1] == "compute" || args[1] == "dma")) {
            Fd fd(path); timeout_test(fd.get(), args[1] == "dma"); return 0;
        }
        throw std::runtime_error("invalid command or argument count; see --help");
    } catch (const std::exception &e) {
        std::cerr << "edu-client: " << e.what() << '\n'; return 1;
    }
}
