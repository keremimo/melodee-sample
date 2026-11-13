// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
// Crash reports (crash_log.h). The handler only makes calls that are safe in a signal handler (open, write, mkdir,
// read, clock_gettime, backtrace once warmed up) and reads what install() worked out beforehand.
#include "melodee/crash_log.h"
#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <exception>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__linux__)
#include <execinfo.h>
#include <link.h>
#include <sys/syscall.h>
#include <ucontext.h>
#endif

namespace melodee::crash_log {
namespace {

constexpr unsigned ring_size = 256, note_bytes = 80;
struct Note {
    uint32_t ms = 0;
    char text[note_bytes] {};
};
Note ring[ring_size];
std::atomic<uint32_t> written {0}, last_key {0};

char folders[2][512] {};
std::atomic<int> folder_at {-1};
std::atomic<long> owner {0}; // the thread writing a report
timespec started {};
char build_id[64] = "unknown";
uintptr_t load_base = 0, image_low = 0, image_high = 0;

uint32_t ms_now() {
    timespec now {};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return uint32_t((now.tv_sec - started.tv_sec) * 1000 + (now.tv_nsec - started.tv_nsec) / 1000000);
}

// Text to the report file and, unless `file_only`, to stderr.
struct Out {
    int file = -1;
    bool file_only = false;
    void put(const char* s, size_t n) const {
        for (int fd : {file, file_only ? -1 : 2}) {
            if (fd < 0) continue;
            size_t done = 0;
            while (done < n) {
                ssize_t w = ::write(fd, s + done, n - done);
                if (w <= 0) break;
                done += size_t(w);
            }
        }
    }
    void str(const char* s) const { put(s, std::strlen(s)); }
    void dec(uint64_t v) const {
        char b[24];
        int n = 0;
        do { b[23 - n++] = char('0' + v % 10); v /= 10; } while (v && n < 24);
        put(b + 24 - n, size_t(n));
    }
    void hex(uint64_t v) const {
        char b[18] = {'0', 'x'};
        int n = 2;
        bool any = false;
        for (int shift = 60; shift >= 0; shift -= 4) {
            unsigned d = unsigned(v >> shift) & 15;
            if (d || any || shift == 0) { b[n++] = "0123456789abcdef"[d]; any = true; }
        }
        put(b, size_t(n));
    }
    void at(uint64_t address) const { // an address, and where it lies in Melodee's binary
        hex(address);
        if (address >= image_low && address < image_high) { str(" (melodee+"); hex(address - load_base); str(")"); }
    }
};

const char* signal_name(int sig) {
    switch (sig) {
    case SIGSEGV: return "SIGSEGV";
    case SIGBUS: return "SIGBUS";
    case SIGFPE: return "SIGFPE";
    case SIGILL: return "SIGILL";
    case SIGABRT: return "SIGABRT";
    case SIGTRAP: return "SIGTRAP";
    default: return "signal";
    }
}

#if defined(__linux__)
int find_program(dl_phdr_info* info, size_t, void*) { // the first object is the program itself
    load_base = info->dlpi_addr;
    for (int i = 0; i < info->dlpi_phnum; ++i) {
        const auto& ph = info->dlpi_phdr[i];
        if (ph.p_type == PT_LOAD) {
            uintptr_t low = info->dlpi_addr + ph.p_vaddr, high = low + ph.p_memsz;
            if (!image_low || low < image_low) image_low = low;
            if (high > image_high) image_high = high;
        }
        if (ph.p_type != PT_NOTE) continue;
        auto* at = reinterpret_cast<const unsigned char*>(info->dlpi_addr + ph.p_vaddr);
        auto* end = at + ph.p_memsz;
        while (at + sizeof(ElfW(Nhdr)) <= end) {
            auto* note = reinterpret_cast<const ElfW(Nhdr)*>(at);
            const unsigned char* name = at + sizeof(ElfW(Nhdr));
            const unsigned char* desc = name + ((note->n_namesz + 3) & ~3u);
            if (note->n_type == NT_GNU_BUILD_ID && note->n_namesz == 4 && !std::memcmp(name, "GNU", 4)) {
                unsigned n = 0;
                for (unsigned k = 0; k < note->n_descsz && n + 2 < sizeof build_id; ++k) {
                    build_id[n++] = "0123456789abcdef"[desc[k] >> 4];
                    build_id[n++] = "0123456789abcdef"[desc[k] & 15];
                }
                build_id[n] = 0;
            }
            at = desc + ((note->n_descsz + 3) & ~3u);
        }
    }
    return 1;
}

// The lines of /proc/self/maps for code (r-xp): which file each address in the backtrace belongs to.
void code_maps(const Out& out) {
    int fd = ::open("/proc/self/maps", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return;
    char chunk[4096], line[512];
    size_t used = 0;
    for (;;) {
        ssize_t n = ::read(fd, chunk, sizeof chunk);
        if (n <= 0) break;
        for (ssize_t k = 0; k < n; ++k) {
            if (used + 1 < sizeof line) line[used++] = chunk[k];
            if (chunk[k] != '\n') continue;
            line[used] = 0;
            if (std::strstr(line, " r-xp ")) out.put(line, used);
            used = 0;
        }
    }
    ::close(fd);
}

void report(int sig, siginfo_t* info, void* context) {
    timespec wall {};
    clock_gettime(CLOCK_REALTIME, &wall);
    // The file: <folder>/crash-<unix time>.txt, the folder made now (nothing is made on the card until needed).
    Out out;
    char path[600] = {};
    int at = folder_at.load(std::memory_order_acquire);
    if (at >= 0) {
        const char* folder = folders[at];
        char parent[512];
        std::strncpy(parent, folder, sizeof parent - 1);
        parent[sizeof parent - 1] = 0;
        if (char* slash = std::strrchr(parent, '/')) { *slash = 0; ::mkdir(parent, 0755); }
        ::mkdir(folder, 0755);
        size_t n = std::strlen(folder);
        std::memcpy(path, folder, n);
        std::memcpy(path + n, "/crash-", 7);
        n += 7;
        char digits[24];
        int d = 0;
        uint64_t secs = uint64_t(wall.tv_sec);
        do { digits[d++] = char('0' + secs % 10); secs /= 10; } while (secs);
        while (d) path[n++] = digits[--d];
        std::memcpy(path + n, ".txt", 5);
        out.file = ::open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    }
    out.str("\n=== Melodee crash report ===\n");
    out.str("signal "); out.dec(unsigned(sig)); out.str(" "); out.str(signal_name(sig));
    out.str(", code "); out.dec(unsigned(info ? info->si_code : 0));
    out.str(", address "); out.hex(info ? uint64_t(uintptr_t(info->si_addr)) : 0); out.str("\n");
    out.str("thread "); out.dec(uint64_t(syscall(SYS_gettid)));
    char comm[32] = {};
    int fd = ::open("/proc/thread-self/comm", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        ssize_t n = ::read(fd, comm, sizeof comm - 1);
        if (n > 0 && comm[n - 1] == '\n') comm[n - 1] = 0;
        ::close(fd);
    }
    out.str(" \""); out.str(comm); out.str("\"\n");
    uint32_t up = ms_now();
    out.str("time "); out.dec(uint64_t(wall.tv_sec)); out.str(" (unix), "); out.dec(up / 1000); out.str(".");
    char ms[4] = {char('0' + up / 100 % 10), char('0' + up / 10 % 10), char('0' + up % 10), 0};
    out.str(ms); out.str(" s after start\n");
    out.str("build id "); out.str(build_id); out.str(", loaded at "); out.hex(load_base); out.str("\n");
#if defined(__aarch64__)
    if (context) {
        const auto& mc = static_cast<ucontext_t*>(context)->uc_mcontext;
        out.str("pc "); out.at(mc.pc); out.str("\nlr "); out.at(mc.regs[30]); out.str("\nsp "); out.hex(mc.sp); out.str("\n");
    }
#else
    (void)context;
#endif
    out.str("backtrace:\n");
    void* frames[64];
    int count = backtrace(frames, 64);
    for (int k = 0; k < count; ++k) { out.str("  #"); out.dec(unsigned(k)); out.str(" "); out.at(uint64_t(uintptr_t(frames[k]))); out.str("\n"); }
    if (out.file >= 0) backtrace_symbols_fd(frames, count, out.file);
    out.str("last presses, oldest first (page is a PageId number, shell/ui_state.h):\n");
    uint32_t total = written.load(std::memory_order_acquire);
    for (uint32_t n = total > ring_size ? total - ring_size : 0; n < total; ++n) {
        const Note& note = ring[n % ring_size];
        out.str("  +"); out.dec(note.ms / 1000); out.str(".");
        char part[4] = {char('0' + note.ms / 100 % 10), char('0' + note.ms / 10 % 10), char('0' + note.ms % 10), 0};
        out.str(part); out.str(" s  ");
        out.put(note.text, strnlen(note.text, note_bytes)); out.str("\n");
    }
    out.file_only = true;
    out.str("code mappings:\n");
    code_maps(out);
    out.file_only = false;
    if (out.file >= 0) {
        ::fsync(out.file);
        ::close(out.file);
        out.file = -1;
        out.str("crash report: "); out.str(path); out.str("\n");
    }
}

void on_signal(int sig, siginfo_t* info, void* context) {
    long self = long(syscall(SYS_gettid)), none = 0;
    if (!owner.compare_exchange_strong(none, self)) {
        if (none != self) for (;;) pause(); // another thread is writing its report; the process ends with it
        signal(sig, SIG_DFL); // a crash inside the report itself
        raise(sig);
        return;
    }
    report(sig, info, context);
    signal(sig, SIG_DFL);
    raise(sig); // the default action (the process ends; a fault comes back as the handler returns)
}
#endif

[[noreturn]] void on_terminate() {
    char line[note_bytes] = "terminate: no exception";
    if (auto e = std::current_exception()) {
        try { std::rethrow_exception(e); }
        catch (const std::exception& x) { std::snprintf(line, sizeof line, "terminate: %s", x.what()); }
        catch (...) { std::snprintf(line, sizeof line, "terminate: an exception of another kind"); }
    }
    note(line);
    std::abort();
}

} // namespace

void install() {
    static bool done = false;
    if (done) return;
    done = true;
    clock_gettime(CLOCK_MONOTONIC, &started);
#if defined(__linux__)
    void* warm[4];
    backtrace(warm, 4); // libgcc's unwinder loaded now, not inside the handler
    dl_iterate_phdr(find_program, nullptr);
    static char alternate[64 * 1024];
    stack_t stack {};
    stack.ss_sp = alternate; stack.ss_size = sizeof alternate;
    sigaltstack(&stack, nullptr); // this thread's stack overflowing still reports
    struct sigaction action {};
    action.sa_sigaction = on_signal;
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&action.sa_mask);
    for (int sig : {SIGSEGV, SIGBUS, SIGFPE, SIGILL, SIGABRT, SIGTRAP}) sigaction(sig, &action, nullptr);
#endif
    std::set_terminate(on_terminate);
}

void set_folder(const std::string& folder) {
    if (folder.empty()) { folder_at.store(-1, std::memory_order_release); return; }
    int next = folder_at.load(std::memory_order_acquire) == 0 ? 1 : 0;
    std::snprintf(folders[next], sizeof folders[next], "%s", folder.c_str());
    folder_at.store(next, std::memory_order_release);
}

void note(const char* text, uint32_t key) {
    uint32_t n;
    uint32_t total = written.load(std::memory_order_relaxed);
    if (key && total && last_key.load(std::memory_order_relaxed) == key) n = total - 1;
    else n = written.fetch_add(1, std::memory_order_acq_rel);
    last_key.store(key, std::memory_order_relaxed);
    Note& slot = ring[n % ring_size];
    slot.ms = ms_now();
    std::strncpy(slot.text, text, note_bytes - 1);
    slot.text[note_bytes - 1] = 0;
}

} // namespace melodee::crash_log
