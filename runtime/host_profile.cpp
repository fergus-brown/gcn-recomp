// A sampling profiler of the host code, for devices where perf will not run: a Quest's
// SELinux policy refuses perf_event_open to the shell, so simpleperf cannot record there.
//
// GCN_PROFILE=<file> arms a profiling timer (ITIMER_PROF, 1 kHz of CPU time across the
// process). Its signal lands on whichever thread was running, and the handler records
// that thread's program counter. host_profile_dump() writes one line per distinct
// location, most frequent first:
//
//   <count> <thread id> <module>+0x<offset>
//
// The offsets are into the module as loaded, which is what llvm-addr2line takes:
//   llvm-addr2line -f -C -e <the unstripped binary> 0x<offset> ...
// tools/profile_report.py does that and sums by function.
//
// Linux and Android, and Windows by suspending threads (below); elsewhere both calls do
// nothing.
#include "runtime.h"
#include <cstdio>
#include <cstdlib>

#if defined(__linux__) && (defined(__aarch64__) || defined(__x86_64__))
#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>
#include <dlfcn.h>
#include <map>
#include <signal.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <ucontext.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {
constexpr size_t kMaxSamples = 1u << 22;  // over an hour at 1 kHz
struct Sample { uintptr_t pc; uint32_t tid; };
Sample* g_samples;
std::atomic<size_t> g_count{0};

void on_prof(int, siginfo_t*, void* ctx) {
    const size_t i = g_count.fetch_add(1, std::memory_order_relaxed);
    if (i >= kMaxSamples) return;
    const ucontext_t* uc = (const ucontext_t*)ctx;
#if defined(__aarch64__)
    g_samples[i].pc = (uintptr_t)uc->uc_mcontext.pc;
#else
    g_samples[i].pc = (uintptr_t)uc->uc_mcontext.gregs[REG_RIP];
#endif
    g_samples[i].tid = (uint32_t)syscall(SYS_gettid);
}
}  // namespace

void host_profile_start() {
    if (!getenv("GCN_PROFILE") || g_samples) return;
    g_samples = (Sample*)calloc(kMaxSamples, sizeof(Sample));
    struct sigaction sa{};
    sa.sa_sigaction = on_prof;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGPROF, &sa, nullptr);
    // GCN_PROFILE_DELAY=<seconds> starts sampling that long after boot, so a profile can
    // be of one stretch of a replayed route rather than everything before it too.
    // ITIMER_PROF counts CPU time, so the delay is taken as wall time by a thread instead.
    const int delay = getenv("GCN_PROFILE_DELAY") ? atoi(getenv("GCN_PROFILE_DELAY")) : 0;
    auto arm = [] {
        itimerval tv{};
        tv.it_interval.tv_usec = 1000;
        tv.it_value.tv_usec = 1000;
        setitimer(ITIMER_PROF, &tv, nullptr);
    };
    if (delay > 0) {
        std::thread([delay, arm] {
            std::this_thread::sleep_for(std::chrono::seconds(delay));
            arm();
        }).detach();
    } else {
        arm();
    }
    fprintf(stderr, "profiling to %s%s\n", getenv("GCN_PROFILE"), delay > 0 ? " after a delay" : "");
}

void host_profile_dump() {
    const char* path = getenv("GCN_PROFILE");
    if (!path || !g_samples) return;
    itimerval off{};
    setitimer(ITIMER_PROF, &off, nullptr);
    const size_t n = std::min(g_count.load(), kMaxSamples);
    std::map<std::pair<uintptr_t, uint32_t>, uint32_t> hist;
    for (size_t i = 0; i < n; i++) hist[{g_samples[i].pc, g_samples[i].tid}]++;
    std::vector<std::pair<uint32_t, std::pair<uintptr_t, uint32_t>>> rows;
    for (const auto& [k, v] : hist) rows.push_back({v, k});
    std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    FILE* f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "# %zu samples\n", n);
    for (const auto& [count, k] : rows) {
        Dl_info info{};
        if (dladdr((void*)k.first, &info) && info.dli_fname) {
            const char* name = info.dli_fname;
            for (const char* s = name; *s; s++) if (*s == '/') name = s + 1;
            fprintf(f, "%u %u %s+0x%lx\n", count, k.second, name,
                    (unsigned long)(k.first - (uintptr_t)info.dli_fbase));
        } else {
            fprintf(f, "%u %u ?+0x%lx\n", count, k.second, (unsigned long)k.first);
        }
    }
    fclose(f);
    fprintf(stderr, "profile: %zu samples written to %s\n", n, path);
}

#elif defined(_WIN32)
// Windows has no profiling signal, so a thread of our own samples the others: about every
// millisecond it suspends each thread of the process in turn, reads its instruction
// pointer and lets it go. That is wall time rather than CPU time, so a thread blocked in
// the kernel is sampled where it waits (ntdll); profile_report.py sums those by module,
// apart from the binary's own functions.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <timeapi.h>
#include <tlhelp32.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <map>
#include <thread>
#include <utility>
#include <vector>

namespace {
constexpr size_t kMaxSamples = 1u << 22;
struct Sample { uintptr_t pc; uint32_t tid; };
Sample* g_samples;
std::atomic<size_t> g_count{0};
std::atomic<bool> g_stop{false};
std::thread g_sampler;

// The process's threads, opened once and listed again twice a second (the guest makes host
// threads as it goes): a snapshot and an open per sample held the rate to a few hertz.
struct Watched { DWORD id; HANDLE h; };
std::vector<Watched> g_threads;

void refresh_threads(DWORD self) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te{};
    te.dwSize = sizeof(te);
    const DWORD pid = GetCurrentProcessId();
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
        bool have = false;
        for (const Watched& w : g_threads) have |= w.id == te.th32ThreadID;
        if (have) continue;
        if (HANDLE t = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE,
                                  te.th32ThreadID))
            g_threads.push_back({te.th32ThreadID, t});
    }
    CloseHandle(snap);
}

void sample_all() {
    for (size_t k = 0; k < g_threads.size();) {
        Watched& w = g_threads[k];
        DWORD code = 0;
        if (!GetExitCodeThread(w.h, &code) || code != STILL_ACTIVE) {
            CloseHandle(w.h);
            g_threads.erase(g_threads.begin() + (ptrdiff_t)k);
            continue;
        }
        if (SuspendThread(w.h) != (DWORD)-1) {
            CONTEXT ctx{};
            ctx.ContextFlags = CONTEXT_CONTROL;
            if (GetThreadContext(w.h, &ctx)) {
                const size_t i = g_count.fetch_add(1, std::memory_order_relaxed);
                if (i < kMaxSamples) g_samples[i] = {(uintptr_t)ctx.Rip, (uint32_t)w.id};
            }
            ResumeThread(w.h);
        }
        k++;
    }
}
}  // namespace

void host_profile_start() {
    if (!getenv("GCN_PROFILE") || g_samples) return;
    g_samples = (Sample*)calloc(kMaxSamples, sizeof(Sample));
    const int delay = getenv("GCN_PROFILE_DELAY") ? atoi(getenv("GCN_PROFILE_DELAY")) : 0;
    timeBeginPeriod(1);  // Sleep(1) is otherwise the scheduler's 15.6 ms tick
    g_sampler = std::thread([delay] {
        if (delay > 0) std::this_thread::sleep_for(std::chrono::seconds(delay));
        const DWORD self = GetCurrentThreadId();
        auto listed = std::chrono::steady_clock::now() - std::chrono::seconds(1);
        while (!g_stop.load()) {
            const auto now = std::chrono::steady_clock::now();
            if (now - listed > std::chrono::milliseconds(500)) {
                refresh_threads(self);
                listed = now;
            }
            sample_all();
            Sleep(1);
        }
    });
    fprintf(stderr, "profiling to %s%s\n", getenv("GCN_PROFILE"), delay > 0 ? " after a delay" : "");
}

void host_profile_dump() {
    const char* path = getenv("GCN_PROFILE");
    if (!path || !g_samples) return;
    g_stop = true;
    if (g_sampler.joinable()) g_sampler.join();
    const size_t n = std::min(g_count.load(), kMaxSamples);
    std::map<std::pair<uintptr_t, uint32_t>, uint32_t> hist;
    for (size_t i = 0; i < n; i++) hist[{g_samples[i].pc, g_samples[i].tid}]++;
    std::vector<std::pair<uint32_t, std::pair<uintptr_t, uint32_t>>> rows;
    for (const auto& [k, v] : hist) rows.push_back({v, k});
    std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    FILE* f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "# %zu samples\n", n);
    for (const auto& [count, k] : rows) {
        HMODULE mod = nullptr;
        char name[MAX_PATH] = "?";
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)k.first, &mod) && mod) {
            GetModuleFileNameA(mod, name, sizeof(name));
            const char* base = name;
            for (const char* s = name; *s; s++) if (*s == '\\' || *s == '/') base = s + 1;
            fprintf(f, "%u %u %s+0x%llx\n", count, k.second, base,
                    (unsigned long long)(k.first - (uintptr_t)mod));
        } else {
            fprintf(f, "%u %u ?+0x%llx\n", count, k.second, (unsigned long long)k.first);
        }
    }
    fclose(f);
    fprintf(stderr, "profile: %zu samples written to %s\n", n, path);
}

#else
void host_profile_start() {}
void host_profile_dump() {}
#endif
