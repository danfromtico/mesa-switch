#include "util_fps_limiter.h"
#include "log/log.h"
#include <windows.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace dxvk {
void Logger::info(const std::string& text) { std::fprintf(stderr, "%s\n", text.c_str()); }
void Logger::warn(const std::string& text) { std::fprintf(stderr, "%s\n", text.c_str()); }
}

static std::array<unsigned int, 3> waitCounts{};
extern "C" void SwitchTimingWait(unsigned int kind) { waitCounts.at(kind)++; }

static long long counter() {
    LARGE_INTEGER value;
    if (!QueryPerformanceCounter(&value)) std::exit(2);
    return value.QuadPart;
}

static unsigned long long threadTime() {
    FILETIME created, exited, kernel, user;
    if (!GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user))
        std::exit(2);
    const auto ticks = [](FILETIME t) {
        return (static_cast<unsigned long long>(t.dwHighDateTime) << 32) | t.dwLowDateTime;
    };
    return ticks(kernel) + ticks(user);
}

static unsigned long long threadCycles() {
    ULONG64 cycles;
    if (!QueryThreadCycleTime(GetCurrentThread(), &cycles)) std::exit(2);
    return cycles;
}

template<size_t N>
static double percentile(const std::array<double, N>& sorted, double quantile) {
    const double position = quantile * (N - 1);
    const size_t lower = static_cast<size_t>(position);
    return sorted[lower] + (position - lower) * (sorted[std::min(lower + 1, N - 1)] - sorted[lower]);
}

static void measure(int cap, long long frequency) {
    constexpr size_t frames = 240;
    std::array<long long, frames + 1> framesAt{};
    std::array<double, frames> periods{}, errors{};
    dxvk::FpsLimiter limiter;
    limiter.setTargetFrameRate(cap, 1);
    for (int i = 0; i < 8; i++) limiter.delay();

    waitCounts.fill(0);
    const auto cycles0 = threadCycles();
    const auto cpu0 = threadTime();
    const auto cpuStart = counter();
    // Every boundary, including boundary zero, is a timestamp at return from delay().
    for (auto& stamp : framesAt) {
        limiter.delay();
        stamp = counter();
    }
    const auto cpu1 = threadTime();
    const auto cycles1 = threadCycles();
    const auto cpuTicks = framesAt.back() - cpuStart;
    const auto frameTicks = framesAt.back() - framesAt.front();
    if (cpuTicks <= 0 || frameTicks <= 0 || cpu1 < cpu0 || cycles1 < cycles0) std::exit(3);
    for (size_t i = 0; i < frames; ++i) {
        const auto ticks = framesAt[i + 1] - framesAt[i];
        if (ticks < 0) std::exit(3);
        periods[i] = 1000.0 * ticks / frequency;
        errors[i] = std::abs(periods[i] - 1000.0 / cap);
        std::printf("%d,%zu,%lld,%lld,%lld,%lld,%llu,%llu\n", cap, i + 1, ticks,
                    frequency, frameTicks, cpuTicks, cpu1 - cpu0, cycles1 - cycles0);
    }
    std::sort(periods.begin(), periods.end());
    std::sort(errors.begin(), errors.end());
    const double oneCorePercent = 100.0 * ((cpu1 - cpu0) / 1e7) / (double(cpuTicks) / frequency);
    std::fprintf(stderr,
        "cap=%d frames=%zu fps=%.4f one_core_percent=%.4f p95_error_ms=%.4f "
        "p95_interval_ms=%.4f p99_interval_ms=%.4f nt_waits=%u highres_waits=%u fallback_waits=%u\n",
        cap, frames, frames * double(frequency) / frameTicks, oneCorePercent,
        percentile(errors, .95), percentile(periods, .95), percentile(periods, .99),
        waitCounts[0], waitCounts[1], waitCounts[2]);
}

int main(int argc, char** argv) {
    if (argc > 2 || (argc == 2 && std::strcmp(argv[1], "30") && std::strcmp(argv[1], "60"))) {
        std::fputs("usage: frame-timing [30|60]\n", stderr);
        return 1;
    }
    LARGE_INTEGER frequency;
    if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0) return 2;
    (void)counter();
    (void)threadTime();
    (void)threadCycles();
    std::puts("cap,frame,interval_ticks,qpc_hz,frame_window_ticks,cpu_window_ticks,thread_cpu_100ns,thread_cycles");
    if (argc == 1 || std::strcmp(argv[1], "30") == 0) measure(30, frequency.QuadPart);
    if (argc == 1 || std::strcmp(argv[1], "60") == 0) measure(60, frequency.QuadPart);
}
