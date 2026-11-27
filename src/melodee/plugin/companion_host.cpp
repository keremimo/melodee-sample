// SPDX-License-Identifier: AGPL-3.0-only
// Copyright (C) 2026 Kerem Kilic (Ellic Studio)
#include "melodee/plugin/companion_host.h"
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <csignal>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <new>
#include <poll.h>
#include <sched.h>
#include <spawn.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace melodee {
using companion::Block;
using companion::Message;
using companion::Op;

namespace {
std::string error_text(const char* what) { return std::string(what) + ": " + std::strerror(errno); }

// A killed helper that had not gone yet (waitpid would have waited): reaped later.
std::vector<pid_t> unreaped;
void reap() {
    unreaped.erase(std::remove_if(unreaped.begin(), unreaped.end(), [](pid_t p) {
        int status = 0;
        pid_t r = waitpid(p, &status, WNOHANG);
        return r == p || (r < 0 && errno == ECHILD);
    }), unreaped.end());
}
} // namespace

CompanionHost::CompanionHost(std::string binary) : program(std::move(binary)) {}

CompanionHost::~CompanionHost() { stop(); }

bool CompanionHost::start(std::string& why) {
    if (running()) return true;
    reap();
    int pair[2] {-1, -1};
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, pair) != 0) { why = error_text("socket"); return false; }
    const int fd = int(syscall(SYS_memfd_create, "melodee-companion", 1u /* MFD_CLOEXEC */));
    if (fd < 0 || ftruncate(fd, sizeof(Block)) != 0) {
        why = error_text("memfd");
        if (fd >= 0) close(fd);
        close(pair[0]); close(pair[1]);
        return false;
    }
    void* mapped = mmap(nullptr, sizeof(Block), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mapped == MAP_FAILED) {
        why = error_text("mmap");
        close(fd); close(pair[0]); close(pair[1]);
        return false;
    }
    shared = new (mapped) Block();
    // The child's two, above anything the file actions write, so neither dup2 overwrites the other.
    const int child_socket = fcntl(pair[1], F_DUPFD_CLOEXEC, 64), child_block = fcntl(fd, F_DUPFD_CLOEXEC, 64);
    close(pair[1]);
    close(fd);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, child_socket, companion::helper_socket_fd);
    posix_spawn_file_actions_adddup2(&actions, child_block, companion::helper_block_fd);
    // Normal scheduling and signals, whatever the thread that starts it has. posix_spawn, not fork(): see
    // Vst2BridgeInstance::load (no copy of Melodee's page tables under its memory lock).
    posix_spawnattr_t attributes;
    posix_spawnattr_init(&attributes);
    sched_param normal {};
    posix_spawnattr_setschedpolicy(&attributes, SCHED_OTHER);
    posix_spawnattr_setschedparam(&attributes, &normal);
    sigset_t all, none;
    sigfillset(&all);
    sigemptyset(&none);
    posix_spawnattr_setsigdefault(&attributes, &all);
    posix_spawnattr_setsigmask(&attributes, &none);
    posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETSCHEDULER | POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSIGMASK);
    const char* args[] = {program.c_str(), "--melodee-companion-helper", nullptr};
    pid_t child = -1;
    const int spawned = posix_spawn(&child, program.c_str(), &actions, &attributes, const_cast<char* const*>(args), environ);
    posix_spawnattr_destroy(&attributes);
    posix_spawn_file_actions_destroy(&actions);
    if (child_socket >= 0) close(child_socket);
    if (child_block >= 0) close(child_block);
    if (spawned != 0 || child_socket < 0 || child_block < 0) {
        why = "the screen's process did not start: " + std::string(std::strerror(spawned ? spawned : EBADF));
        if (spawned == 0) { kill(child, SIGKILL); unreaped.push_back(child); }
        close(pair[0]);
        munmap(shared, sizeof(Block));
        shared = nullptr;
        return false;
    }
    pid = child;
    socket = pair[0];
    session_open = 0;
    params_open = 0;
    got.clear();
    return true;
}

void CompanionHost::stop() {
    if (socket >= 0) { close(socket); socket = -1; }
    if (pid > 0) {
        kill(pid, SIGKILL);
        bool gone = false;
        for (int i = 0; i < 20 && !gone; ++i) { // it dies at once unless the kernel holds it (a card read)
            int status = 0;
            pid_t r = waitpid(pid, &status, WNOHANG);
            gone = r == pid || (r < 0 && errno == ECHILD);
            if (!gone) usleep(5000);
        }
        if (!gone) unreaped.push_back(pid);
        pid = -1;
    }
    if (shared) { munmap(shared, sizeof(Block)); shared = nullptr; }
    session_open = 0;
    params_open = 0;
    got.clear();
}

bool CompanionHost::open(uint32_t session, const std::string& library, uint32_t params, std::string& why) {
    if (running() && session_open == session && session) return true;
    if (library.size() >= sizeof(Message::text)) { why = "the library's path is too long"; return false; }
    if (!start(why)) return false;
    shared->param_count = std::min<uint32_t>(params, companion::max_params);
    Message m;
    m.op = uint32_t(Op::Open);
    m.session = session;
    m.a = int32_t(params);
    companion::put_text(m.text, sizeof(m.text), library.c_str());
    Message answer;
    if (!ask(m, Op::Opened, answer, std::chrono::milliseconds(3000), why)) return false; // a library's load: longer
    if (answer.a != 1) {
        answer.text[sizeof(answer.text) - 1] = 0;
        why = answer.text[0] ? answer.text : "the screen could not be made";
        session_open = 0;
        return false;
    }
    session_open = session;
    params_open = shared->param_count;
    return true;
}

void CompanionHost::close_session(uint32_t session) {
    if (!running() || session_open != session || !session) return;
    Message m;
    m.op = uint32_t(Op::Close);
    m.session = session;
    if (send(socket, &m, sizeof(m), MSG_NOSIGNAL | MSG_DONTWAIT) != ssize_t(sizeof(m))) { stop(); return; }
    session_open = 0;
}

bool CompanionHost::receive(Message& m, int wait_ms, bool& none, std::string& why) {
    none = false;
    pollfd p {socket, POLLIN, 0};
    const int r = poll(&p, 1, std::max(wait_ms, 0));
    if (r < 0 && errno == EINTR) { none = true; return true; }
    if (r < 0) { why = error_text("poll"); return false; }
    if (r == 0) { none = true; return true; }
    if (!(p.revents & POLLIN)) { why = "the screen's process stopped"; return false; }
    const ssize_t n = recv(socket, &m, sizeof(m), MSG_DONTWAIT | MSG_TRUNC);
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) { none = true; return true; }
    if (n == 0) { why = "the screen's process stopped"; return false; }
    if (n != ssize_t(sizeof(m))) { why = n < 0 ? error_text("recv") : "the screen sent a message Melodee cannot read"; return false; }
    return true;
}

// A request of the screen's own: kept for the screen's reader, unless it is not one (out of place, or too many).
bool CompanionHost::accept(const Message& m, std::string& why) {
    if (m.op != uint32_t(Op::Edit) && m.op != uint32_t(Op::Program) && m.op != uint32_t(Op::Redraw)) {
        why = "the screen sent a message out of place";
        return false;
    }
    if (m.op == uint32_t(Op::Edit) && !std::isfinite(m.value)) { why = "the screen asked for a value that is not a number"; return false; }
    if (m.session != session_open) return true; // an earlier screen's, late: dropped
    if (got.size() >= 4 * size_t(flood)) { why = "the screen sent too many requests"; return false; }
    got.push_back(m);
    return true;
}

bool CompanionHost::ask(const Message& request, Op answer, Message& out, std::chrono::milliseconds limit, std::string& why) {
    if (!running()) { why = "the screen's process is not running"; return false; }
    if (send(socket, &request, sizeof(request), MSG_NOSIGNAL) != ssize_t(sizeof(request))) {
        why = "the screen's process stopped";
        stop();
        return false;
    }
    const auto deadline = std::chrono::steady_clock::now() + limit;
    unsigned count = 0;
    for (;;) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
        if (left < 0) { why = "the screen did not answer in time"; stop(); return false; }
        Message m;
        bool none = false;
        if (!receive(m, int(std::min<long long>(left, 1000)), none, why)) { stop(); return false; }
        if (none) continue;
        if (++count > flood) { why = "the screen sent too many requests"; stop(); return false; }
        if (m.op == uint32_t(answer)) {
            if (m.session != request.session) { why = "the screen answered for another screen"; stop(); return false; }
            out = m;
            return true;
        }
        if (!accept(m, why)) { stop(); return false; }
    }
}

bool CompanionHost::pump(std::string& why) {
    if (!running()) return true;
    for (unsigned count = 0;; ++count) {
        if (count > flood) { why = "the screen sent too many requests"; stop(); return false; }
        Message m;
        bool none = false;
        if (!receive(m, 0, none, why)) { stop(); return false; }
        if (none) return true;
        if (!accept(m, why)) { stop(); return false; }
    }
}

// --- The helper's side ---

namespace {
Block* helper_block = nullptr;
uint32_t helper_session = 0;
bool in_call = false;     // a host function asks for something only inside the screen's calls
unsigned sent_in_call = 0; // and only so many times in one call (Melodee stops a screen past its own limit)
// A value the screen asked for, given back as the parameter's value until Melodee's copy moves from where it was
// (Melodee made the change, or something else did) or a while has passed: so two wheel detents in a row add up.
struct Asked { float value = 0, was = 0; std::chrono::steady_clock::time_point at {}; bool on = false; };
Asked asked[companion::max_params];
constexpr auto asked_for = std::chrono::milliseconds(500);

void tell(Message& m) {
    m.session = helper_session;
    if (++sent_in_call > CompanionHost::flood / 2) return;
    send(companion::helper_socket_fd, &m, sizeof(m), MSG_NOSIGNAL | MSG_DONTWAIT);
}
bool host_param(const void*, uint32_t handle, melodee_companion_param_t* out) {
    if (!helper_block || !out || handle >= helper_block->param_count || handle >= companion::max_params) return false;
    *out = helper_block->params[handle];
    out->name[sizeof(out->name) - 1] = 0;
    out->text[sizeof(out->text) - 1] = 0;
    auto& a = asked[handle];
    if (a.on && (out->value != a.was || std::chrono::steady_clock::now() - a.at > asked_for)) a.on = false;
    if (a.on) out->value = a.value;
    return true;
}
void host_set(const void*, uint32_t handle, float value) {
    if (!in_call || !helper_block || handle >= helper_block->param_count || !std::isfinite(value)) return;
    Message m;
    m.op = uint32_t(Op::Edit);
    m.a = int32_t(handle);
    m.value = std::clamp(value, 0.0f, 1.0f);
    auto& a = asked[handle];
    if (!a.on) a.was = helper_block->params[handle].value;
    a = {m.value, a.was, std::chrono::steady_clock::now(), true};
    tell(m);
}
void host_program(const void*, melodee_companion_program_t* out) {
    if (!out || !helper_block) return;
    *out = helper_block->program;
    out->name[sizeof(out->name) - 1] = 0;
}
void host_select_program(const void*, int32_t program) {
    if (!in_call) return;
    Message m;
    m.op = uint32_t(Op::Program);
    m.a = program;
    tell(m);
}
void host_redraw(const void*) {
    Message m;
    m.op = uint32_t(Op::Redraw);
    tell(m);
}
const void* host_extension(const void*, const char*) { return nullptr; }

void close_other_files() {
#ifdef SYS_close_range
    if (syscall(SYS_close_range, 5u, ~0u, 0u) == 0) return;
#endif
    if (DIR* d = opendir("/proc/self/fd")) {
        std::vector<int> open;
        while (dirent* e = readdir(d)) {
            int fd = std::atoi(e->d_name);
            if (fd > companion::helper_block_fd && fd != dirfd(d)) open.push_back(fd);
        }
        closedir(d);
        for (int fd : open) close(fd);
    }
}
// What it holds now, plus `room` for a screen's own memory.
void limit_memory(size_t room) {
    size_t data_kb = 0;
    if (FILE* f = std::fopen("/proc/self/status", "r")) {
        char line[256];
        while (std::fgets(line, sizeof(line), f))
            if (!std::strncmp(line, "VmData:", 7)) data_kb = size_t(std::strtoull(line + 7, nullptr, 10));
        std::fclose(f);
    }
    rlimit data {};
    data.rlim_cur = data.rlim_max = rlim_t(data_kb * 1024 + room);
    setrlimit(RLIMIT_DATA, &data);
    rlimit core {0, 0};
    setrlimit(RLIMIT_CORE, &core);
}
} // namespace

int companion_helper_main() {
    prctl(PR_SET_PDEATHSIG, SIGKILL); // gone with Melodee
    if (getppid() == 1) return 1;
    prctl(PR_SET_NAME, "melodee-screen", 0, 0, 0);
    prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);
    close_other_files();
    sched_param normal {};
    sched_setscheduler(0, SCHED_OTHER, &normal);
    setpriority(PRIO_PROCESS, 0, 10);
    std::signal(SIGPIPE, SIG_IGN);
    void* mapped = mmap(nullptr, sizeof(Block), PROT_READ | PROT_WRITE, MAP_SHARED, companion::helper_block_fd, 0);
    close(companion::helper_block_fd);
    if (mapped == MAP_FAILED) return 2;
    helper_block = static_cast<Block*>(mapped);
    if (helper_block->magic != companion::magic || helper_block->version != companion::layout_version || helper_block->size != sizeof(Block))
        return 3;
    limit_memory(size_t(128) << 20);
    const int socket = companion::helper_socket_fd;
    void* library = nullptr;
    std::string loaded;
    const melodee_companion_t* api = nullptr;
    void* screen = nullptr;
    melodee_companion_host_t host {};
    host.abi = MELODEE_COMPANION_ABI;
    host.size = sizeof(host);
    host.host = nullptr;
    host.param = host_param;
    host.set = host_set;
    host.program = host_program;
    host.select_program = host_select_program;
    host.redraw = host_redraw;
    host.extension = host_extension;
    auto destroy = [&] {
        if (screen && api && api->destroy) { in_call = true; api->destroy(screen); in_call = false; }
        screen = nullptr;
    };
    for (;;) {
        Message m;
        const ssize_t n = recv(socket, &m, sizeof(m), 0);
        if (n < 0 && errno == EINTR) continue;
        if (n != ssize_t(sizeof(m))) break; // Melodee gone, or not Melodee
        Message reply;
        reply.session = m.session;
        switch (Op(m.op)) {
        case Op::Open: {
            destroy();
            for (auto& a : asked) a.on = false;
            m.text[sizeof(m.text) - 1] = 0;
            reply.op = uint32_t(Op::Opened);
            std::string why;
            if (loaded != m.text) {
                if (library) { dlclose(library); library = nullptr; }
                api = nullptr;
                loaded.clear();
                library = dlopen(m.text, RTLD_NOW | RTLD_LOCAL);
                if (!library) {
                    const char* e = dlerror();
                    why = e ? e : "it does not load";
                } else if (auto entry = reinterpret_cast<melodee_companion_entry_t>(dlsym(library, MELODEE_COMPANION_ENTRY)); !entry) {
                    why = "it exports no " MELODEE_COMPANION_ENTRY;
                } else if (!(api = entry(MELODEE_COMPANION_ABI))) {
                    why = "it does not run on this Melodee";
                } else if (api->abi < 1 || api->size < offsetof(melodee_companion_t, extension) || !api->create || !api->destroy
                           || !api->draw || !api->input || !api->controls) {
                    why = "its table is incomplete";
                }
                if (!why.empty()) {
                    api = nullptr;
                    if (library) dlclose(library);
                    library = nullptr;
                } else {
                    loaded = m.text;
                }
            }
            helper_session = m.session;
            host.param_count = std::min<uint32_t>(helper_block->param_count, companion::max_params);
            if (why.empty()) {
                in_call = true;
                sent_in_call = 0;
                screen = api->create(&host);
                in_call = false;
                if (!screen) why = "the screen could not be made";
            }
            reply.a = why.empty() ? 1 : 0;
            companion::put_text(reply.text, sizeof(reply.text), why.c_str());
            break;
        }
        case Op::Draw: {
            reply.op = uint32_t(Op::Drawn);
            if (!screen || m.session != helper_session) break;
            melodee_screen_frame_t f {};
            f.pixels = helper_block->pixels;
            f.width = companion::width;
            f.height = m.rows == MELODEE_SCREEN_STRIP_HEIGHT ? MELODEE_SCREEN_STRIP_HEIGHT : MELODEE_SCREEN_PAGE_HEIGHT;
            f.stride = companion::width;
            f.area = m.area;
            f.redraw_all = m.redraw_all;
            f.colour = m.colour;
            f.time_ms = m.time_ms;
            f.playing = m.playing;
            f.shift = m.shift;
            for (unsigned k = 0; k < 3; ++k) { f.pots[k] = m.pots[k]; f.pots_live[k] = m.pots_live[k]; }
            in_call = true;
            sent_in_call = 0;
            reply.a = api->draw(screen, &f) ? 1 : 0;
            api->controls(screen, reply.handles);
            in_call = false;
            break;
        }
        case Op::Input: {
            reply.op = uint32_t(Op::Used);
            if (!screen || m.session != helper_session) break;
            melodee_screen_input_t in {uint32_t(m.a), m.b, uint32_t(m.c ? 1 : 0)};
            in_call = true;
            sent_in_call = 0;
            reply.a = api->input(screen, &in) ? 1 : 0;
            api->controls(screen, reply.handles);
            in_call = false;
            break;
        }
        case Op::Close:
            if (m.session == helper_session) destroy();
            continue;
        default:
            return 4;
        }
        if (send(socket, &reply, sizeof(reply), MSG_NOSIGNAL) != ssize_t(sizeof(reply))) break;
    }
    destroy();
    return 0;
}

} // namespace melodee
