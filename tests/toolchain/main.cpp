// Probes the pieces the recompiled code depends on: VFP rounding modes via
// fenv, lrintf/llrint, 64-bit atomics, exceptions and thread_local, and a
// std::thread plus std::atomic::wait, all on libctru. Prints to the debug
// console (svcOutputDebugString) and exits.
#include <3ds.h>
#include <atomic>
#include <cfenv>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include <thread>
#include <chrono>
#include <cstdarg>

static FILE* g_log;
static void logf(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap);
    if (g_log) { va_start(ap, fmt); vfprintf(g_log, fmt, ap); va_end(ap); fflush(g_log); }
}

thread_local int tls_value = 7;
static std::atomic<int> flag{0};

int main() {
    consoleDebugInit(debugDevice_SVC);
    g_log = fopen("sdmc:/3ds/rt64-3ds/fenv_test.log", "w");
    logf("toolchain test start\n");
    volatile float f = 2.5f;
    fesetround(FE_TONEAREST); long a = lrintf(f);
    fesetround(FE_TOWARDZERO); long b = lrintf(f);
    fesetround(FE_UPWARD); long c = lrintf(f);
    fesetround(FE_DOWNWARD); long d = lrintf(-f);
    fesetround(FE_TONEAREST);
    logf("fenv: nearest %ld zero %ld up %ld down(-2.5) %ld (expect 2 2 3 -3)\n", a, b, c, d);
    std::atomic<uint64_t> a64{1}; a64.fetch_add(0x100000000ull);
    logf("atomic64: %llx (expect 100000001)\n", (unsigned long long)a64.load());
    int caught = 0;
    try { throw std::runtime_error("x"); } catch (const std::exception&) { caught = 1; }
    logf("exceptions: %d tls: %d\n", caught, tls_value);
    auto t0 = std::chrono::steady_clock::now();
    std::thread th([]{ tls_value = 9; std::this_thread::sleep_for(std::chrono::milliseconds(50)); flag.store(1); flag.notify_all(); });
    flag.wait(0);
    th.join();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    logf("thread+atomic wait: ok after %lld ms (expect ~50); main tls still %d\n", (long long)ms, tls_value);
    logf("TOOLCHAIN TEST DONE\n");
    if (g_log) fclose(g_log);
    svcSleepThread(2000000000LL);
    return 0;
}
