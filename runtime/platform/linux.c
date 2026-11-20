// Linux implementation of platform.h: pthreads, CLOCK_MONOTONIC, futex locks and condition variables, POSIX files,
// /proc and sysfs for the load counters, posix_spawn for child processes.
#define _GNU_SOURCE
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/fs.h>
#include <linux/futex.h>
#ifdef __ANDROID__
#include <sys/system_properties.h>
#else
#include <linux/userfaultfd.h>
#endif
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/random.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <SDL3/SDL_clipboard.h>
#include <SDL3/SDL_messagebox.h>

#include "platform.h"

// The write-watch ABI of Linux 6.7 (uapi linux/fs.h and linux/userfaultfd.h), for headers older than it: the release
// builds against the Steam Runtime's 5.10 headers and runs on newer kernels. The kernel refuses what it lacks at run time.
#ifndef PAGEMAP_SCAN
struct page_region {
    __u64 start;
    __u64 end;
    __u64 categories;
};
struct pm_scan_arg {
    __u64 size;
    __u64 flags;
    __u64 start;
    __u64 end;
    __u64 walk_end;
    __u64 vec;
    __u64 vec_len;
    __u64 max_pages;
    __u64 category_inverted;
    __u64 category_mask;
    __u64 category_anyof_mask;
    __u64 return_mask;
};
#define PAGEMAP_SCAN _IOWR('f', 16, struct pm_scan_arg)
#define PAGE_IS_WRITTEN (1 << 1)
#define PAGE_IS_PRESENT (1 << 3)
#define PAGE_IS_SWAPPED (1 << 4)
#define PAGE_IS_PFNZERO (1 << 5)
#define PM_SCAN_WP_MATCHING (1 << 0)
#define PM_SCAN_CHECK_WPASYNC (1 << 1)
#endif
#ifndef UFFD_FEATURE_WP_ASYNC
#define UFFD_FEATURE_WP_UNPOPULATED (1 << 13)
#define UFFD_FEATURE_WP_ASYNC (1 << 15)
#endif
#ifndef UFFD_USER_MODE_ONLY
#define UFFD_USER_MODE_ONLY 1
#endif

extern char **environ;

// ---- threads

struct PlatThread {
    pthread_t t;
    PlatThreadFn fn;
    void *arg;
    volatile int32_t tid;    // the kernel's id once the thread runs, for plat_thread_set_priority
#ifdef __ANDROID__
    volatile uint32_t done;  // futex-woken on exit: Bionic has no pthread_timedjoin_np
#endif
    char name[16];           // the kernel keeps 15 characters
};

// A signal stack for the calling thread, so the fault and crash handlers still run when its own stack is exhausted.
// Never freed: threads that end are few, and a stack freed while a handler might run on it would be worse.
static void alt_stack(void) {
    stack_t cur;
    if (!sigaltstack(NULL, &cur) && !(cur.ss_flags & SS_DISABLE)) return;
    size_t size = 64 << 10;   // a signal frame with the AVX-512 state takes several KiB
    void *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p == MAP_FAILED) return;
    stack_t ss = {.ss_sp = p, .ss_size = size};
    sigaltstack(&ss, NULL);
}

static int read_u64_file(const char *path, uint64_t *v);

#ifdef __ANDROID__
__attribute__((weak)) const char *cfg_get(const char *name);

// Classify heterogeneous ARM64 big.LITTLE clusters (e.g. Snapdragon 8 Gen 2 1+4+3: cpu0..2 Cortex-A510 Silver @ 2.02 GHz,
// cpu3..6 Cortex-A710/A715 Gold @ 2.80 GHz, cpu7 Cortex-X3 Prime @ 3.20+ GHz) into Silver efficiency and Gold+Prime masks.
static void classify_android_clusters(const uint64_t freqs[64], uint64_t avail, uint64_t *eff_out, uint64_t *perf_out) {
    uint64_t min_f = UINT64_MAX, max_f = 0;
    int n_cpus = 0;
    for (int i = 0; i < 64; i++) {
        if (!(avail >> i & 1)) continue;
        n_cpus++;
        if (freqs[i]) {
            if (freqs[i] < min_f) min_f = freqs[i];
            if (freqs[i] > max_f) max_f = freqs[i];
        }
    }
    if (max_f > min_f && min_f < UINT64_MAX) {
        uint64_t eff = 0, perf = 0;
        for (int i = 0; i < 64; i++) {
            if (!(avail >> i & 1)) continue;
            if (freqs[i] && freqs[i] <= min_f) eff |= 1ull << i;
            else perf |= 1ull << i;
        }
        if (eff && perf) {
            *eff_out = eff;
            *perf_out = perf;
            return;
        }
    }
    // Fallback when cpufreq/topology sysfs is hidden on an 8-core Snapdragon 8 Gen 2 (1+4+3: cpu0..2 Silver, cpu3..7 Gold/Prime).
    if (n_cpus == 8 && (avail & 0xffull) == 0xffull) {
        *eff_out = 0x07ull;
        *perf_out = 0xf8ull;
        return;
    }
    *eff_out = *perf_out = 0;
}

static pthread_once_t g_android_pin_once = PTHREAD_ONCE_INIT;
static uint64_t g_android_eff_mask, g_android_perf_mask;
static int g_android_pin_on;

static void init_android_clusters(void) {
    const char *v = cfg_get ? cfg_get("XC2_ANDROID_PIN") : NULL;
    if (!v) v = getenv("XC2_ANDROID_PIN");
    g_android_pin_on = !v || !*v || strcmp(v, "0") != 0;
    if (!g_android_pin_on) return;

    cpu_set_t set;
    CPU_ZERO(&set);
    if (sched_getaffinity(0, sizeof set, &set) != 0) return;
    uint64_t avail = 0, freqs[64] = {0};
    int any_freq = 0;
    for (int i = 0; i < 64; i++) {
        if (!CPU_ISSET(i, &set)) continue;
        avail |= 1ull << i;
        char path[128];
        snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", i);
        if (read_u64_file(path, &freqs[i]) && freqs[i]) any_freq = 1;
    }
    if (!any_freq) {
        for (int i = 0; i < 64; i++) {
            if (!(avail >> i & 1)) continue;
            char path[128];
            snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/topology/cpu_capacity", i);
            read_u64_file(path, &freqs[i]);
        }
    }
    classify_android_clusters(freqs, avail, &g_android_eff_mask, &g_android_perf_mask);
}

static uint64_t android_role_mask(const char *name) {
    if (!name || !*name) return 0;
    // Performance-critical threads: main guest loop, Horizon guest threads, and Vulkan replay/present/completion/vsync.
    static const char *const perf_names[] = {
        "xc2 main", "guest main", "guest", "vk replay", "vk present", "vk completion", "vsync"
    };
    for (size_t i = 0; i < sizeof perf_names / sizeof *perf_names; i++)
        if (!strcmp(name, perf_names[i])) return g_android_perf_mask;

    // Background audio, video, decompression, pipeline workers, and telemetry threads -> Silver efficiency cores.
    static const char *const eff_names[] = {
        "audren", "movie", "astc", "save commit", "use preload", "pipe autosave",
        "pipe worker", "lib prebuild", "vk headroom", "motion", "perfmon", "reap", "flight dump"
    };
    for (size_t i = 0; i < sizeof eff_names / sizeof *eff_names; i++)
        if (!strcmp(name, eff_names[i])) return g_android_eff_mask;
    return 0;
}

static void android_pin_self(const char *name) {
    pthread_once(&g_android_pin_once, init_android_clusters);
    if (!g_android_pin_on) return;
    uint64_t mask = android_role_mask(name);
    if (!mask) return;
    cpu_set_t set;
    CPU_ZERO(&set);
    for (int i = 0; i < 64; i++)
        if (mask >> i & 1) CPU_SET(i, &set);
    sched_setaffinity(0, sizeof set, &set);
}
#endif

static void *thread_main(void *p) {
    PlatThread *t = p;
    alt_stack();
    __atomic_store_n(&t->tid, (int32_t)syscall(SYS_gettid), __ATOMIC_RELEASE);
    if (t->name[0]) pthread_setname_np(pthread_self(), t->name);
#ifdef __ANDROID__
    android_pin_self(t->name);
#endif
    t->fn(t->arg);
#ifdef __ANDROID__
    __atomic_store_n(&t->done, 1, __ATOMIC_RELEASE);
    syscall(SYS_futex, &t->done, FUTEX_WAKE | FUTEX_PRIVATE_FLAG, INT_MAX, NULL, NULL, 0);
#endif
    return NULL;
}

PlatThread *plat_thread_new(PlatThreadFn fn, void *arg, const char *name) {
    return plat_thread_new_ex(fn, arg, name, 0);
}

PlatThread *plat_thread_new_ex(PlatThreadFn fn, void *arg, const char *name, size_t stack) {
    PlatThread *t = calloc(1, sizeof *t);
    if (!t) return NULL;
    t->fn = fn;
    t->arg = arg;
    if (name) snprintf(t->name, sizeof t->name, "%s", name);
    pthread_attr_t a;
    pthread_attr_init(&a);
    // The stack is mapped lazily, as Windows reserves it: only the pages a thread touches cost memory.
    pthread_attr_setstacksize(&a, stack ? stack : PLAT_STACK_DEFAULT);
    int e = pthread_create(&t->t, &a, thread_main, t);
    pthread_attr_destroy(&a);
    if (e) { free(t); return NULL; }
    return t;
}

// The physical core of each logical processor is in /sys/devices/system/cpu/cpu<n>/topology/core_id together with its
// package; the lowest-numbered processor of each (package, core) pair is the core's first.
int plat_cpu_cores(uint64_t *all, uint64_t *first) {
    cpu_set_t set;
    *all = *first = 0;
    if (sched_getaffinity(0, sizeof set, &set)) return 0;
    int cores = 0;
    uint64_t seen[64][2];
    for (int i = 0; i < 64; i++) {
        if (!CPU_ISSET(i, &set)) continue;
        *all |= 1ull << i;
        uint64_t core = 0, pkg = 0;
        char path[96];
        snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/topology/core_id", i);
        if (!read_u64_file(path, &core)) core = (uint64_t)i;
        snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%d/topology/physical_package_id", i);
        read_u64_file(path, &pkg);
        int k = 0;
        while (k < cores && (seen[k][0] != core || seen[k][1] != pkg)) k++;
        if (k == cores) seen[cores][0] = core, seen[cores][1] = pkg, cores++, *first |= 1ull << i;
    }
    return cores;
}

// Linux has no process-wide affinity: each thread of /proc/self/task gets the mask, and threads started later inherit
// it from the thread that starts them.
int plat_process_affinity(uint64_t mask) {
    cpu_set_t set;
    CPU_ZERO(&set);
    for (int i = 0; i < 64; i++)
        if (mask >> i & 1) CPU_SET(i, &set);
    DIR *d = opendir("/proc/self/task");
    if (!d) return 0;
    int ok = 1;
    for (struct dirent *e; (e = readdir(d));)
        if (e->d_name[0] != '.' && sched_setaffinity((pid_t)atoi(e->d_name), sizeof set, &set)) ok = 0;
    closedir(d);
    return ok;
}

// The first field of /proc/self/task/<tid>/schedstat is the time the thread has run, in ns.
uint64_t plat_thread_cpu_ns(uint32_t tid) {
    char path[64];
    snprintf(path, sizeof path, "/proc/self/task/%u/schedstat", tid);
    FILE *f = fopen(path, "r");
    unsigned long long ns = 0;
    if (f) {
        if (fscanf(f, "%llu", &ns) != 1) ns = 0;
        fclose(f);
    }
    return ns;
}

// ---- thread interruption: a signal handshake in place of Windows' SuspendThread / Get/SetThreadContext.
// The caller moves the handle from IDLE to REQ and signals the thread. The thread's handler stores where it was and
// moves REQ to STOPPED, then spins until the caller moves it on to GO (with the redirect in to), changes its own
// return context, and goes back to IDLE. A caller that sees no STOPPED within INTR_WAIT_NS (the thread is blocked in
// the kernel and runs no handler) takes REQ back to IDLE; a handler that then finds IDLE returns at once.
enum { INTR_IDLE, INTR_REQ, INTR_STOPPED, INTR_GO };
#define INTR_WAIT_NS 200000u

struct PlatIntr {
    pthread_t pt;
    volatile int32_t state;
    uint64_t rip, rsp, to;
};

static __thread PlatIntr *intr_me;
static int intr_sig;

static void intr_handler(int sig, siginfo_t *si, void *uc) {
    (void)sig, (void)si;
    PlatIntr *t = intr_me;
    ucontext_t *u = uc;
    if (!t) return;
#if defined(__aarch64__)
    t->rip = (uint64_t)u->uc_mcontext.pc;
    t->rsp = (uint64_t)u->uc_mcontext.sp;
#else
    t->rip = (uint64_t)u->uc_mcontext.gregs[REG_RIP];
    t->rsp = (uint64_t)u->uc_mcontext.gregs[REG_RSP];
#endif
    int32_t req = INTR_REQ;
    if (!__atomic_compare_exchange_n(&t->state, &req, INTR_STOPPED, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return;
    while (__atomic_load_n(&t->state, __ATOMIC_ACQUIRE) != INTR_GO) plat_pause();
    uint64_t to = t->to;
    t->to = 0;
#if defined(__aarch64__)
    if (to) {
        uint64_t fpcr;
        __asm__ volatile("mrs %0, fpcr\n\tmsr fpcr, xzr" : "=r"(fpcr));
        ((void (*)(void))(uintptr_t)to)();
        __asm__ volatile("msr fpcr, %0" : : "r"(fpcr));
    }
    __atomic_store_n(&t->state, INTR_IDLE, __ATOMIC_RELEASE);
#else
    if (to) u->uc_mcontext.gregs[REG_RIP] = (greg_t)to;
    __atomic_store_n(&t->state, INTR_IDLE, __ATOMIC_RELEASE);
#endif
}

PlatIntr *plat_intr_self(void) {
    static PlatMutex install = PLAT_MUTEX_INIT;
    if (!__atomic_load_n(&intr_sig, __ATOMIC_ACQUIRE)) {
        plat_mutex_lock(&install);
        if (!intr_sig) {
            int sig = SIGRTMIN + 4;   // glibc keeps the first real-time signals for itself; SIGRTMIN skips them
            struct sigaction sa = {.sa_sigaction = intr_handler, .sa_flags = SA_SIGINFO | SA_RESTART | SA_ONSTACK};
            sigemptyset(&sa.sa_mask);
            if (!sigaction(sig, &sa, NULL)) __atomic_store_n(&intr_sig, sig, __ATOMIC_RELEASE);
        }
        plat_mutex_unlock(&install);
    }
    if (!intr_sig) return NULL;
    PlatIntr *t = calloc(1, sizeof *t);
    if (!t) return NULL;
    t->pt = pthread_self();
    intr_me = t;
    return t;
}

// Called by the thread itself as it ends, or by another once the thread no longer runs.
void plat_intr_free(PlatIntr *t) {
    if (!t) return;
    if (intr_me == t) intr_me = NULL;
    free(t);
}

int plat_intr_stop(PlatIntr *t, uint64_t *rip, uint64_t *rsp) {
    int32_t idle = INTR_IDLE;
    if (!__atomic_compare_exchange_n(&t->state, &idle, INTR_REQ, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return 0;
    if (pthread_kill(t->pt, intr_sig)) {
        __atomic_store_n(&t->state, INTR_IDLE, __ATOMIC_RELEASE);
        return 0;
    }
    uint64_t end = plat_now_ns() + INTR_WAIT_NS;
    while (__atomic_load_n(&t->state, __ATOMIC_ACQUIRE) != INTR_STOPPED) {
        if (plat_now_ns() < end) {
            plat_pause();
            continue;
        }
        int32_t req = INTR_REQ;
        if (__atomic_compare_exchange_n(&t->state, &req, INTR_IDLE, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return 0;
    }
    *rip = t->rip;
    *rsp = t->rsp;
    return 1;
}

int plat_intr_resume(PlatIntr *t, uint64_t to) {
    t->to = to;
    __atomic_store_n(&t->state, INTR_GO, __ATOMIC_RELEASE);
    return 1;
}

// ---- fibers: a stack switch of our own. glibc's swapcontext saves and restores the signal mask with a system call
// on every switch, and the game switches fibers often. fiber_swap saves the callee-saved registers, the MXCSR and the
// x87 control word on the current stack, stores the stack pointer in *save and loads the other one. A new fiber's
// stack is laid out as fiber_swap leaves one, returning into fiber_entry, which calls fn(arg) (r12 and r13).
struct PlatFiber {
    void *sp;
    void *stack;   // mapped stack (NULL for a thread's own context)
    size_t size;
};

void plat_fiber_swap(void **save, void *load);
void plat_fiber_entry(void);
#if defined(__aarch64__)
__asm__(".text\n"
        ".globl plat_fiber_swap\n"
        ".type plat_fiber_swap, @function\n"
        ".p2align 4\n"
        "plat_fiber_swap:\n"
        "    .cfi_startproc\n"
        "    sub sp, sp, #176\n"
        "    mrs x2, fpcr\n"
        "    mrs x3, fpsr\n"
        "    stp x2,  x3,  [sp, #0]\n"
        "    stp d8,  d9,  [sp, #16]\n"
        "    stp d10, d11, [sp, #32]\n"
        "    stp d12, d13, [sp, #48]\n"
        "    stp d14, d15, [sp, #64]\n"
        "    stp x19, x20, [sp, #80]\n"
        "    stp x21, x22, [sp, #96]\n"
        "    stp x23, x24, [sp, #112]\n"
        "    stp x25, x26, [sp, #128]\n"
        "    stp x27, x28, [sp, #144]\n"
        "    stp x29, x30, [sp, #160]\n"
        "    mov x2, sp\n"
        "    str x2, [x0]\n"
        "    mov sp, x1\n"
        "    ldp x2,  x3,  [sp, #0]\n"
        "    msr fpcr, x2\n"
        "    msr fpsr, x3\n"
        "    ldp d8,  d9,  [sp, #16]\n"
        "    ldp d10, d11, [sp, #32]\n"
        "    ldp d12, d13, [sp, #48]\n"
        "    ldp d14, d15, [sp, #64]\n"
        "    ldp x19, x20, [sp, #80]\n"
        "    ldp x21, x22, [sp, #96]\n"
        "    ldp x23, x24, [sp, #112]\n"
        "    ldp x25, x26, [sp, #128]\n"
        "    ldp x27, x28, [sp, #144]\n"
        "    ldp x29, x30, [sp, #160]\n"
        "    add sp, sp, #176\n"
        "    ret\n"
        "    .cfi_endproc\n"
        ".size plat_fiber_swap, .-plat_fiber_swap\n"
        ".globl plat_fiber_entry\n"
        ".type plat_fiber_entry, @function\n"
        ".p2align 4\n"
        "plat_fiber_entry:\n"
        "    mov x29, #0\n"
        "    mov x30, #0\n"
        "    mov x0, x20\n"
        "    blr x19\n"
        "    brk #1\n"
        ".size plat_fiber_entry, .-plat_fiber_entry\n");
#else
__asm__(".text\n"
        ".globl plat_fiber_swap\n"
        ".type plat_fiber_swap, @function\n"
        ".p2align 4\n"
        "plat_fiber_swap:\n"
        "    .cfi_startproc\n"
        "    pushq %rbp\n"
        "    pushq %rbx\n"
        "    pushq %r12\n"
        "    pushq %r13\n"
        "    pushq %r14\n"
        "    pushq %r15\n"
        "    subq $16, %rsp\n"
        "    stmxcsr 8(%rsp)\n"
        "    fnstcw (%rsp)\n"
        "    movq %rsp, (%rdi)\n"
        "    movq %rsi, %rsp\n"
        "    fldcw (%rsp)\n"
        "    ldmxcsr 8(%rsp)\n"
        "    addq $16, %rsp\n"
        "    popq %r15\n"
        "    popq %r14\n"
        "    popq %r13\n"
        "    popq %r12\n"
        "    popq %rbx\n"
        "    popq %rbp\n"
        "    ret\n"
        "    .cfi_endproc\n"
        ".size plat_fiber_swap, .-plat_fiber_swap\n"
        ".globl plat_fiber_entry\n"
        ".type plat_fiber_entry, @function\n"
        ".p2align 4\n"
        "plat_fiber_entry:\n"
        "    movq %r13, %rdi\n"
        "    callq *%r12\n"
        "    ud2\n"
        ".size plat_fiber_entry, .-plat_fiber_entry\n");
#endif

static __thread PlatFiber *fiber_cur;
static __thread PlatFiber fiber_own;

PlatFiber *plat_fiber_self(void) {
    fiber_own = (PlatFiber){0};
    fiber_cur = &fiber_own;
    return &fiber_own;
}

void plat_fiber_unself(void) { fiber_cur = NULL; }

#ifndef MAP_STACK
#define MAP_STACK 0
#endif

PlatFiber *plat_fiber_new(size_t stack, void (*fn)(void *arg), void *arg) {
    PlatFiber *f = calloc(1, sizeof *f);
    if (!f) return NULL;
    // Reserved, not committed; the lowest page stays inaccessible, so an overflow faults instead of corrupting.
    void *m = mmap(NULL, stack, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_STACK, -1, 0);
    if (m == MAP_FAILED) {
        free(f);
        return NULL;
    }
    mprotect(m, 4096, PROT_NONE);
    f->stack = m, f->size = stack;
    uint64_t *top = (uint64_t *)(((uintptr_t)m + stack) & ~(uintptr_t)15);
#if defined(__aarch64__)
    // 22 words (176 bytes): fpcr, fpsr, d8-d15, x19-x28, x29, x30 (plat_fiber_entry); SP is 16-aligned after pop.
    uint64_t *sp = top - 22;
    memset(sp, 0, 22 * sizeof *sp);
    sp[10] = (uint64_t)(uintptr_t)fn;    // x19
    sp[11] = (uint64_t)(uintptr_t)arg;   // x20
    sp[21] = (uint64_t)(uintptr_t)plat_fiber_entry;   // x30 (LR)
#else
    // fiber_entry starts with the stack pointer 16-aligned as its call needs, so fn enters as the ABI says: code the
    // compiler built for an aligned stack (vmovaps on a stack slot) faults otherwise.
    uint64_t *sp = top - 11;   // fpu state (2 words), r15, r14, r13, r12, rbx, rbp, return address, 2 words of padding
    sp[0] = 0x037f;            // x87 control word: the default
    sp[1] = 0x1f80;            // MXCSR: the default
    sp[2] = sp[3] = 0;         // r15, r14
    sp[4] = (uint64_t)(uintptr_t)arg;   // r13
    sp[5] = (uint64_t)(uintptr_t)fn;    // r12
    sp[6] = sp[7] = 0;         // rbx, rbp
    sp[8] = (uint64_t)(uintptr_t)plat_fiber_entry;
    sp[9] = sp[10] = 0;
#endif
    f->sp = sp;
    return f;
}

void plat_fiber_switch(PlatFiber *to) {
    PlatFiber *from = fiber_cur;
    fiber_cur = to;   // on this thread; the switch back into from sets it on whichever thread that is
    plat_fiber_swap(&from->sp, to->sp);
}

void plat_fiber_free(PlatFiber *f) {
    if (!f) return;
    if (f->stack) munmap(f->stack, f->size);
    free(f);
}

int plat_thread_spawn(PlatThreadFn fn, void *arg, const char *name, size_t stack) {
    PlatThread *t = plat_thread_new_ex(fn, arg, name, stack);
    if (t) plat_thread_detach(t);
    return t != NULL;
}

void plat_thread_join(PlatThread *t) {
    pthread_join(t->t, NULL);
    free(t);
}

int plat_thread_join_ns(PlatThread *t, uint64_t ns) {
#ifdef __ANDROID__
    uint64_t deadline = plat_now_ns() + ns;
    while (!__atomic_load_n(&t->done, __ATOMIC_ACQUIRE)) {
        uint64_t now = plat_now_ns();
        if (now >= deadline) return 0;
        uint64_t rem = deadline - now;
        struct timespec ts = {(time_t)(rem / 1000000000u), (long)(rem % 1000000000u)};
        long r = syscall(SYS_futex, &t->done, FUTEX_WAIT | FUTEX_PRIVATE_FLAG, 0, &ts, NULL, 0);
        if (r == -1 && errno == ETIMEDOUT && !__atomic_load_n(&t->done, __ATOMIC_ACQUIRE)) return 0;
    }
    pthread_join(t->t, NULL);
    free(t);
    return 1;
#else
    struct timespec at;
    clock_gettime(CLOCK_REALTIME, &at);   // pthread_timedjoin_np takes a CLOCK_REALTIME deadline
    uint64_t end = (uint64_t)at.tv_sec * 1000000000u + (uint64_t)at.tv_nsec + ns;
    at.tv_sec = (time_t)(end / 1000000000u), at.tv_nsec = (long)(end % 1000000000u);
    if (pthread_timedjoin_np(t->t, NULL, &at)) return 0;
    free(t);
    return 1;
#endif
}

// The thread keeps using t->fn and t->arg, so the struct stays allocated (a few bytes per detached thread).
void plat_thread_detach(PlatThread *t) { pthread_detach(t->t); }

uint64_t plat_thread_affinity(PlatThread *t, uint64_t mask) {
    cpu_set_t old, set;
#ifdef __ANDROID__
    pid_t tid = t ? pthread_gettid_np(t->t) : 0;
    if (tid < 0 || sched_getaffinity(tid, sizeof old, &old)) return 0;
    CPU_ZERO(&set);
    for (int i = 0; i < 64; i++)
        if (mask >> i & 1) CPU_SET(i, &set);
    if (sched_setaffinity(tid, sizeof set, &set)) return 0;
#else
    pthread_t th = t ? t->t : pthread_self();
    if (pthread_getaffinity_np(th, sizeof old, &old)) return 0;
    CPU_ZERO(&set);
    for (int i = 0; i < 64; i++)
        if (mask >> i & 1) CPU_SET(i, &set);
    if (pthread_setaffinity_np(th, sizeof set, &set)) return 0;
#endif
    uint64_t prev = 0;
    for (int i = 0; i < 64; i++)
        if (CPU_ISSET(i, &old)) prev |= 1ull << i;
    return prev;
}

uint32_t plat_cpu_count(void) {
    cpu_set_t set;
    if (!sched_getaffinity(0, sizeof set, &set) && CPU_COUNT(&set) > 0) return (uint32_t)CPU_COUNT(&set);
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (uint32_t)n : 1;
}

// Windows' levels map to nice values: IDLE 19, LOWEST 4, BELOW_NORMAL 2, NORMAL 0, ABOVE_NORMAL -2, HIGHEST -5,
// TIME_CRITICAL -10. Applied only when the process may lower a nice value again (platform.h says why).
static int nice_of(int prio) {
    return prio <= PLAT_PRIO_IDLE ? 19 : prio == PLAT_PRIO_LOWEST ? 4 : prio == PLAT_PRIO_BELOW_NORMAL ? 2
         : prio == PLAT_PRIO_NORMAL ? 0 : prio == PLAT_PRIO_ABOVE_NORMAL ? -2 : prio == PLAT_PRIO_HIGHEST ? -5 : -10;
}

static int may_renice(int nice) {
    static int lowest = 100;   // the lowest nice value this process may set; 100 until probed
    if (lowest == 100) {
        struct rlimit r;
        int l = !getrlimit(RLIMIT_NICE, &r) && r.rlim_cur != RLIM_INFINITY ? 20 - (int)r.rlim_cur : -20;
        // CAP_SYS_NICE shows as being allowed to set a nice value below the limit's
        pid_t tid = (pid_t)syscall(SYS_gettid);
        errno = 0;
        int cur = getpriority(PRIO_PROCESS, (id_t)tid);
        if (!errno && l > cur - 1 && !setpriority(PRIO_PROCESS, (id_t)tid, cur - 1)) {
            setpriority(PRIO_PROCESS, (id_t)tid, cur);
            l = -20;
        }
        __atomic_store_n(&lowest, l, __ATOMIC_RELAXED);
    }
    return lowest <= 0 && nice >= lowest;
}

static void renice(pid_t tid, int prio) {
    int nice = nice_of(prio);
    if (tid > 0 && may_renice(nice)) setpriority(PRIO_PROCESS, (id_t)tid, nice);
}

void plat_thread_priority(int prio) { renice((pid_t)syscall(SYS_gettid), prio); }

void plat_thread_set_priority(PlatThread *t, int prio) { renice(__atomic_load_n(&t->tid, __ATOMIC_ACQUIRE), prio); }

int plat_thread_priority_get(void) {
    errno = 0;
    int nice = getpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid));
    if (errno) return PLAT_PRIO_NORMAL;
    return nice >= 19 ? PLAT_PRIO_IDLE : nice >= 4 ? PLAT_PRIO_LOWEST : nice >= 2 ? PLAT_PRIO_BELOW_NORMAL
         : nice >= 0 ? PLAT_PRIO_NORMAL : nice >= -2 ? PLAT_PRIO_ABOVE_NORMAL : nice >= -5 ? PLAT_PRIO_HIGHEST
         : PLAT_PRIO_TIME_CRITICAL;
}

void plat_yield(void) { sched_yield(); }

// ---- time

uint64_t plat_now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}

uint64_t plat_counter(void) { return plat_now_ns(); }
uint64_t plat_counter_hz(void) { return 1000000000u; }

// The timer slack (50 us by default) is how late the kernel may wake a sleeping thread to batch wakeups. New threads
// inherit it from the thread that creates them.
void plat_timer_1ms(void) { prctl(PR_SET_TIMERSLACK, 1000ul, 0, 0, 0); }

// clock_nanosleep wakes within the timer slack (50 us by default) of its deadline; the last PLAT_SPIN_NS are spent
// yielding so the deadline is met closely, as on Windows.
#define PLAT_SPIN_NS 100000u

void plat_sleep_ns(uint64_t ns) {
    uint64_t end = plat_now_ns() + ns;
    if (ns > PLAT_SPIN_NS) {
        uint64_t at = end - PLAT_SPIN_NS;
        struct timespec ts = {(time_t)(at / 1000000000u), (long)(at % 1000000000u)};
        while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR) {}
    }
    while (plat_now_ns() < end) sched_yield();
}

void plat_wait_ns(uint64_t ns) {
    struct timespec ts = {(time_t)(ns / 1000000000u), (long)(ns % 1000000000u)};
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR) {}
}

void plat_sleep_ms(uint32_t ms) {
    struct timespec ts = {(time_t)(ms / 1000), (long)(ms % 1000) * 1000000};
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR) {}
}

// ---- locks: a reader-writer lock in one 32-bit word, zero when free.
// bit 31: a writer holds it; bit 30: some thread sleeps on the word; bits 0-29: readers holding it.
// A thread that cannot take the lock sets the sleeper bit and waits on the futex; every release that finds the bit
// clears it and wakes all sleepers, which retry (and set it again if they still cannot get in). Readers do not enter
// while the sleeper bit is set, so a waiting writer is not starved by a stream of readers.

#define LK_W 0x80000000u
#define LK_S 0x40000000u
#define LK_R 0x3fffffffu
#define LK_SPINS 64

static long futex(volatile uint32_t *w, int op, uint32_t val, const struct timespec *ts) {
    return syscall(SYS_futex, w, op | FUTEX_PRIVATE_FLAG, val, ts, NULL, 0);
}

static inline volatile uint32_t *lk(PlatMutex *m) { return (volatile uint32_t *)&m->p; }

// Sets the sleeper bit on s and sleeps while the word still equals the result.
static void lk_sleep(volatile uint32_t *w, uint32_t s) {
    if (!(s & LK_S) && !__atomic_compare_exchange_n(w, &s, s | LK_S, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) return;
    futex(w, FUTEX_WAIT, s | LK_S, NULL);
}

static void lk_wake(volatile uint32_t *w) {
    __atomic_fetch_and(w, ~LK_S, __ATOMIC_RELAXED);
    futex(w, FUTEX_WAKE, INT_MAX, NULL);
}

void plat_mutex_init(PlatMutex *m) { m->p = 0; }

int plat_mutex_trylock(PlatMutex *m) {
    volatile uint32_t *w = lk(m);
    uint32_t s = __atomic_load_n(w, __ATOMIC_RELAXED);
    while (!(s & (LK_W | LK_R)))
        if (__atomic_compare_exchange_n(w, &s, s | LK_W, 1, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return 1;
    return 0;
}

void plat_mutex_lock(PlatMutex *m) {
    volatile uint32_t *w = lk(m);
    for (int spin = 0;; spin++) {
        uint32_t s = __atomic_load_n(w, __ATOMIC_RELAXED);
        if (!(s & (LK_W | LK_R))) {
            if (__atomic_compare_exchange_n(w, &s, s | LK_W, 1, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return;
            continue;
        }
        if (spin < LK_SPINS) plat_pause();
        else lk_sleep(w, s);
    }
}

void plat_mutex_unlock(PlatMutex *m) {
    volatile uint32_t *w = lk(m);
    uint32_t s = __atomic_fetch_and(w, ~LK_W, __ATOMIC_RELEASE);
    if (s & LK_S) lk_wake(w);
}

int plat_mutex_trylock_shared(PlatMutex *m) {
    volatile uint32_t *w = lk(m);
    uint32_t s = __atomic_load_n(w, __ATOMIC_RELAXED);
    while (!(s & (LK_W | LK_S)))
        if (__atomic_compare_exchange_n(w, &s, s + 1, 1, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return 1;
    return 0;
}

void plat_mutex_lock_shared(PlatMutex *m) {
    volatile uint32_t *w = lk(m);
    for (int spin = 0;; spin++) {
        uint32_t s = __atomic_load_n(w, __ATOMIC_RELAXED);
        if (!(s & (LK_W | LK_S))) {
            if (__atomic_compare_exchange_n(w, &s, s + 1, 1, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return;
            continue;
        }
        if (spin < LK_SPINS) plat_pause();
        else lk_sleep(w, s);
    }
}

void plat_mutex_unlock_shared(PlatMutex *m) {
    volatile uint32_t *w = lk(m);
    uint32_t s = __atomic_sub_fetch(w, 1, __ATOMIC_RELEASE);
    if (!(s & LK_R) && (s & LK_S)) lk_wake(w);
}

// ---- condition variables: a sequence number in the first 32 bits. A waiter reads it before releasing the lock and
// sleeps only while it is unchanged, so a signal between the release and the sleep is not lost.

static inline volatile uint32_t *cv(PlatCond *c) { return (volatile uint32_t *)&c->p; }

void plat_cond_init(PlatCond *c) { c->p = 0; }

void plat_cond_wait(PlatCond *c, PlatMutex *m) {
    uint32_t seq = __atomic_load_n(cv(c), __ATOMIC_RELAXED);
    plat_mutex_unlock(m);
    futex(cv(c), FUTEX_WAIT, seq, NULL);
    plat_mutex_lock(m);
}

int plat_cond_wait_ns(PlatCond *c, PlatMutex *m, uint64_t ns) {
    uint32_t seq = __atomic_load_n(cv(c), __ATOMIC_RELAXED);
    struct timespec ts = {(time_t)(ns / 1000000000u), (long)(ns % 1000000000u)};
    plat_mutex_unlock(m);
    long r = futex(cv(c), FUTEX_WAIT, seq, &ts);
    int timed_out = r == -1 && errno == ETIMEDOUT;
    plat_mutex_lock(m);
    return !timed_out;
}

void plat_cond_signal(PlatCond *c) {
    __atomic_add_fetch(cv(c), 1, __ATOMIC_RELEASE);
    futex(cv(c), FUTEX_WAKE, 1, NULL);
}

void plat_cond_broadcast(PlatCond *c) {
    __atomic_add_fetch(cv(c), 1, __ATOMIC_RELEASE);
    futex(cv(c), FUTEX_WAKE, INT_MAX, NULL);
}

// ---- files

int plat_file_exists(const char *path) {
    struct stat st;
    return !stat(path, &st) && !S_ISDIR(st.st_mode);
}

int plat_dir_exists(const char *path) {
    struct stat st;
    return !stat(path, &st) && S_ISDIR(st.st_mode);
}

int plat_full_path(const char *path, char *out, size_t cap) {
    if (path[0] == '/') return snprintf(out, cap, "%s", path) < (int)cap;
    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof cwd)) return 0;
    return snprintf(out, cap, "%s/%s", cwd, path) < (int)cap;
}

void plat_file_mkdir_p(const char *path) {
    char tmp[PATH_MAX];
    if (snprintf(tmp, sizeof tmp, "%s", path) >= (int)sizeof tmp) return;
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/' || *p == '\\') {
            char c = *p;
            *p = 0;
            mkdir(tmp, 0755);
            *p = c;
        }
    }
    mkdir(tmp, 0755);
}

static int write_all(int fd, const void *data, size_t n) {
    const uint8_t *p = data;
    while (n) {
        ssize_t k = write(fd, p, n);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) return 0;
        p += k, n -= (size_t)k;
    }
    return 1;
}

uint8_t *plat_file_read(const char *path, uint64_t *size) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return NULL;
    struct stat st;
    uint8_t *buf = NULL;
    if (!fstat(fd, &st) && S_ISREG(st.st_mode) && st.st_size > 0 && st.st_size < (1ll << 30)) {
        buf = malloc((size_t)st.st_size + 1);
        size_t got = 0;
        while (buf && got < (size_t)st.st_size) {
            ssize_t k = read(fd, buf + got, (size_t)st.st_size - got);
            if (k < 0 && errno == EINTR) continue;
            if (k <= 0) break;
            got += (size_t)k;
        }
        if (buf && got != (size_t)st.st_size) {
            free(buf);
            buf = NULL;
        }
        if (buf) {
            buf[st.st_size] = 0;
            *size = (uint64_t)st.st_size;
        }
    }
    close(fd);
    return buf;
}

int plat_file_write(const char *path, const void *data, size_t n) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) return 0;
    int ok = write_all(fd, data, n);
    return close(fd) == 0 && ok;
}

// O_APPEND makes each write land whole at the end of the file, from any thread.
PlatAppend *plat_append_open(const char *path) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_APPEND | O_CLOEXEC, 0644);
    return fd < 0 ? NULL : (PlatAppend *)(intptr_t)(fd + 1);
}

int plat_append_write(PlatAppend *f, const void *data, size_t n) {
    int fd = (int)(intptr_t)f - 1;
    ssize_t k;
    do k = write(fd, data, n);
    while (k < 0 && errno == EINTR);
    return k == (ssize_t)n;
}

// Flushes the directory holding path, so a rename into it survives a crash.
static void sync_dir_of(const char *path) {
    char dir[PATH_MAX];
    if (snprintf(dir, sizeof dir, "%s", path) >= (int)sizeof dir) return;
    char *slash = strrchr(dir, '/');
    if (slash == dir) slash[1] = 0;
    else if (slash) *slash = 0;
    else strcpy(dir, ".");
    int fd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd >= 0) {
        fsync(fd);
        close(fd);
    }
}

int plat_file_replace(const char *from, const char *to) {
    if (rename(from, to)) return 0;
    sync_dir_of(to);
    return 1;
}

int plat_file_write_atomic(const char *path, const void *data, size_t n) {
#ifndef __ANDROID__
    // A rename replaces a read-only file here (only the folder's permission counts) but not on Windows: a file the
    // player made read-only stays as it is on both.
    if (access(path, W_OK) && errno != ENOENT) return 0;
#endif
    char tmp[PATH_MAX + 48];
    int len = snprintf(tmp, sizeof tmp, "%s.%u.%u.tmp", path, plat_process_id(), plat_thread_id());
    if (len < 0 || len >= (int)sizeof tmp) return 0;
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
#ifdef __ANDROID__
        return plat_file_write(path, data, n);
#else
        return 0;
#endif
    }
    int ok = write_all(fd, data, n) && fsync(fd) == 0;
    ok = close(fd) == 0 && ok;
    if (ok && !rename(tmp, path)) {
        sync_dir_of(path);
        return 1;
    }
#ifdef __ANDROID__
    // Android Scoped Storage FUSE rejects rename() over a file pushed via adb (shell UID); unlink + rename succeeds.
    if (ok && !unlink(path) && !rename(tmp, path)) {
        sync_dir_of(path);
        return 1;
    }
#endif
    unlink(tmp);
#ifdef __ANDROID__
    if (ok) return plat_file_write(path, data, n);
#endif
    return 0;
}

int plat_file_flush(const char *path) {
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    int ok = fsync(fd) == 0;
    close(fd);
    return ok;
}

int plat_file_sync(FILE *f) { return !fflush(f) && !fsync(fileno(f)); }

void plat_file_delete(const char *path) { unlink(path); }

int plat_file_truncate(const char *path, uint64_t size) { return truncate(path, (off_t)size) == 0; }

int64_t plat_file_size(const char *path) {
    struct stat st;
    return stat(path, &st) ? -1 : (int64_t)st.st_size;
}

// The owner's write permission stands for Windows' read-only attribute.
int plat_file_set_readonly(const char *path, int on) {
    struct stat st;
    if (stat(path, &st)) return 0;
    mode_t m = on ? st.st_mode & ~(mode_t)0222 : st.st_mode | S_IWUSR;
    return chmod(path, m & 07777) == 0;
}

int plat_file_list(const char *dir, const char *ext, int (*cb)(const char *name, void *ctx), void *ctx) {
    DIR *d = opendir(dir);
    if (!d) return 0;
    size_t el = strlen(ext);
    for (struct dirent *e; (e = readdir(d));) {
        size_t n = strlen(e->d_name);
        if (n < el || strcmp(e->d_name + n - el, ext) || e->d_name[0] == '.') continue;
        if (!cb(e->d_name, ctx)) break;
    }
    closedir(d);
    return 1;
}

// ---- virtual memory. Reservations are PROT_NONE MAP_NORESERVE mappings; commit is mprotect read-write, decommit
// MADV_DONTNEED (private anonymous pages read zero afterwards) and PROT_NONE. Linux keeps no committed/reserved
// distinction, so a byte per guest page records it (pstate, mapped lazily: only the pages touched cost memory).
//
// Write watch: a userfaultfd in asynchronous write-protect mode (Linux 6.7) over the watched reservations. The kernel
// clears a page's protection on the first write without a round trip to us, and PAGEMAP_SCAN lists the pages whose
// protection was cleared and protects them again. Without it (an older kernel), plat_mem_written reports the ranges as
// untracked and the back end uploads them in full.

#define PG 4096ull
#define PS_LIMIT (1ull << 39)
enum { PS_FREE, PS_RESERVED, PS_NONE, PS_READ, PS_RW };   // PS_NONE.. are committed: PLAT_PROT_* + PS_NONE
#ifdef __ANDROID__
// Android SELinux blocks userfaultfd on unprivileged app processes, so watched reservations track dirty 4 KiB pages
// with soft PROT_READ protection + lock-free atomic flags in pstate, disarmed in fault_handler on first write and in
// plat_mem_disarm before kernel pread() / mremap().
#define PS_MASK 0x07u
#define PS_NOWATCH 0x10u
#define PS_ARMED 0x20u
#define PS_WATCHED 0x40u
#define PS_DIRTY 0x80u
#else
#define PS_MASK 0xffu
#endif
static uint8_t *pstate;
static int ww_fd = -1, pm_fd = -1;   // userfaultfd and /proc/self/pagemap; -1 when write watch is unavailable
static PlatMutex vm_lock = PLAT_MUTEX_INIT;

static void ps_set(uint64_t addr, uint64_t n, uint8_t v) {
    if (addr < PS_LIMIT) memset(pstate + addr / PG, v, (size_t)((n + PG - 1) / PG));
}

static void ww_open(void) {
#ifdef __ANDROID__
    return;
#else
    int fd = (int)syscall(SYS_userfaultfd, O_CLOEXEC | O_NONBLOCK | UFFD_USER_MODE_ONLY);
    struct uffdio_api api = {.api = UFFD_API,
                             .features = UFFD_FEATURE_WP_ASYNC | UFFD_FEATURE_WP_UNPOPULATED};
    if (fd < 0 || ioctl(fd, UFFDIO_API, &api)) {
        fprintf(stderr, "platform: no asynchronous userfaultfd write protection (%s): written memory is not tracked\n",
                strerror(errno));
        if (fd >= 0) close(fd);
        return;
    }
    pm_fd = open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC);
    if (pm_fd < 0) {
        close(fd);
        return;
    }
    ww_fd = fd;
#endif
}

int plat_mem_reserve(uint64_t addr, uint64_t n, int watch) {
    plat_mutex_lock(&vm_lock);
    if (!pstate) {
        void *p = mmap(NULL, PS_LIMIT / PG, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        pstate = p == MAP_FAILED ? NULL : p;
    }
    if (watch && ww_fd < 0 && pm_fd < 0) ww_open();
    void *p = pstate && addr + n <= PS_LIMIT
                  ? mmap((void *)(uintptr_t)addr, n, PROT_NONE,
                         MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED_NOREPLACE, -1, 0)
                  : MAP_FAILED;
    int ok = p == (void *)(uintptr_t)addr;
    if (p != MAP_FAILED && !ok) munmap(p, n);   // a kernel without MAP_FIXED_NOREPLACE put it elsewhere
    if (ok) {
#ifdef __ANDROID__
        ps_set(addr, n, (uint8_t)(PS_RESERVED | (watch ? PS_WATCHED : 0)));
#else
        ps_set(addr, n, PS_RESERVED);
#endif
        // Transparent huge pages would populate (and count as written) 2 MiB at the first write of one page; writes are
        // tracked per 4 KiB page, as on Windows.
        if (watch) madvise(p, n, MADV_NOHUGEPAGE);
#ifndef __ANDROID__
        struct uffdio_register reg = {.range = {addr, n}, .mode = UFFDIO_REGISTER_MODE_WP};
        if (watch && ww_fd >= 0 && ioctl(ww_fd, UFFDIO_REGISTER, &reg))
            fprintf(stderr, "platform: write watch over %#llx+%#llx failed (%s)\n", (unsigned long long)addr,
                    (unsigned long long)n, strerror(errno));
#endif
    }
    plat_mutex_unlock(&vm_lock);
    return ok;
}

void *plat_mem_alloc(size_t n) {
    void *p = mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    return p == MAP_FAILED ? NULL : p;
}

int plat_mem_commit(uint64_t addr, size_t n) {
#ifdef __ANDROID__
    if (pstate && addr < PS_LIMIT && (__atomic_load_n(&pstate[addr / PG], __ATOMIC_RELAXED) & PS_WATCHED)) {
        if (mprotect((void *)(uintptr_t)addr, n, PROT_READ) == 0) {
            uint64_t p0 = addr / PG, p1 = (addr + n + PG - 1) / PG;
            if (p1 > PS_LIMIT / PG) p1 = PS_LIMIT / PG;
            for (uint64_t i = p0; i < p1; i++)
                __atomic_store_n(&pstate[i], PS_RW | PS_WATCHED | PS_ARMED, __ATOMIC_RELEASE);
            return 1;
        }
        if (mprotect((void *)(uintptr_t)addr, n, PROT_READ | PROT_WRITE)) return 0;
        uint64_t p0 = addr / PG, p1 = (addr + n + PG - 1) / PG;
        if (p1 > PS_LIMIT / PG) p1 = PS_LIMIT / PG;
        for (uint64_t i = p0; i < p1; i++)
            __atomic_store_n(&pstate[i], PS_RW | PS_WATCHED | PS_NOWATCH, __ATOMIC_RELEASE);
        return 1;
    }
#endif
    if (mprotect((void *)(uintptr_t)addr, n, PROT_READ | PROT_WRITE)) return 0;
    ps_set(addr, n, PS_RW);
    return 1;
}

int plat_mem_decommit(uint64_t addr, uint64_t n) {
    if (madvise((void *)(uintptr_t)addr, n, MADV_DONTNEED) || mprotect((void *)(uintptr_t)addr, n, PROT_NONE)) return 0;
#ifdef __ANDROID__
    if (pstate && addr < PS_LIMIT) {
        uint64_t p0 = addr / PG, p1 = (addr + n + PG - 1) / PG;
        if (p1 > PS_LIMIT / PG) p1 = PS_LIMIT / PG;
        for (uint64_t i = p0; i < p1; i++) {
            uint8_t w = __atomic_load_n(&pstate[i], __ATOMIC_RELAXED) & PS_WATCHED;
            __atomic_store_n(&pstate[i], (uint8_t)(PS_RESERVED | w), __ATOMIC_RELEASE);
        }
        return 1;
    }
#endif
    ps_set(addr, n, PS_RESERVED);
    return 1;
}

int plat_mem_protect(uint64_t addr, uint64_t n, int prot) {
#ifdef __ANDROID__
    if (pstate && addr < PS_LIMIT && (__atomic_load_n(&pstate[addr / PG], __ATOMIC_RELAXED) & PS_WATCHED)) {
        uint64_t p0 = addr / PG, p1 = (addr + n + PG - 1) / PG;
        if (p1 > PS_LIMIT / PG) p1 = PS_LIMIT / PG;
        if (prot == PLAT_PROT_RW) {
            int armed_ok = mprotect((void *)(uintptr_t)addr, n, PROT_READ) == 0;
            if (!armed_ok && mprotect((void *)(uintptr_t)addr, n, PROT_READ | PROT_WRITE)) return 0;
            for (uint64_t i = p0; i < p1; i++) {
                uint8_t keep = __atomic_load_n(&pstate[i], __ATOMIC_RELAXED) & (PS_WATCHED | PS_DIRTY | PS_NOWATCH);
                uint8_t arm = armed_ok && !(keep & PS_NOWATCH) ? PS_ARMED : PS_NOWATCH;
                __atomic_store_n(&pstate[i], (uint8_t)(PS_RW | keep | arm), __ATOMIC_RELEASE);
            }
            return 1;
        }
        int p = prot == PLAT_PROT_READ ? PROT_READ : PROT_NONE;
        if (mprotect((void *)(uintptr_t)addr, n, p)) return 0;
        for (uint64_t i = p0; i < p1; i++) {
            uint8_t keep = __atomic_load_n(&pstate[i], __ATOMIC_RELAXED) & (PS_WATCHED | PS_DIRTY | PS_NOWATCH);
            __atomic_store_n(&pstate[i], (uint8_t)((PS_NONE + prot) | keep), __ATOMIC_RELEASE);
        }
        return 1;
    }
#endif
    int p = prot == PLAT_PROT_RW ? PROT_READ | PROT_WRITE : prot == PLAT_PROT_READ ? PROT_READ : PROT_NONE;
    if (mprotect((void *)(uintptr_t)addr, n, p)) return 0;
    ps_set(addr, n, (uint8_t)(PS_NONE + prot));
    return 1;
}

int plat_mem_state(uint64_t addr, uint64_t limit, uint64_t *end) {
    if (!pstate || addr >= PS_LIMIT) {
        *end = limit;
        return PLAT_MEM_FREE;
    }
    uint64_t lim = limit < PS_LIMIT ? limit : PS_LIMIT, i = addr / PG;
    uint8_t v = pstate[i] & PS_MASK;
    while ((i + 1) * PG < lim && (pstate[i + 1] & PS_MASK) == v) i++;
    *end = (i + 1) * PG < lim ? (i + 1) * PG : lim;
    return v == PS_FREE ? PLAT_MEM_FREE : v == PS_RESERVED ? PLAT_MEM_RESERVED : v - PS_NONE;
}

#ifdef __ANDROID__
void plat_mem_disarm(uint64_t addr, uint64_t n) {
    if (!n || !pstate || addr >= PS_LIMIT) return;
    uint64_t lo = addr & ~(PG - 1);
    uint64_t hi = addr + n > PS_LIMIT ? PS_LIMIT : (addr + n + PG - 1) & ~(PG - 1);
    uint64_t run_lo = UINT64_MAX;
    for (uint64_t a = lo; a < hi; a += PG) {
        uint64_t i = a / PG;
        uint8_t v = __atomic_load_n(&pstate[i], __ATOMIC_ACQUIRE);
        if ((v & (PS_MASK | PS_WATCHED)) == (PS_RW | PS_WATCHED)) {
            int was_armed = (v & PS_ARMED) != 0;
            while ((v & (PS_MASK | PS_WATCHED)) == (PS_RW | PS_WATCHED) &&
                   ((v & PS_ARMED) || !(v & PS_DIRTY))) {
                was_armed |= (v & PS_ARMED) != 0;
                uint8_t nv = (uint8_t)((v & ~PS_ARMED) | PS_DIRTY);
                if (__atomic_compare_exchange_n(&pstate[i], &v, nv, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) break;
            }
            if (was_armed) {
                if (run_lo == UINT64_MAX) run_lo = a;
                continue;
            }
        }
        if (run_lo != UINT64_MAX) {
            mprotect((void *)(uintptr_t)run_lo, (size_t)(a - run_lo), PROT_READ | PROT_WRITE);
            run_lo = UINT64_MAX;
        }
    }
    if (run_lo != UINT64_MAX)
        mprotect((void *)(uintptr_t)run_lo, (size_t)(hi - run_lo), PROT_READ | PROT_WRITE);
}

static int arm_flush(uint64_t run_lo, uint64_t run_hi) {
    if (run_lo == UINT64_MAX) return 1;
    if (mprotect((void *)(uintptr_t)run_lo, (size_t)(run_hi - run_lo), PROT_READ) == 0) return 1;
    for (uint64_t a = run_lo; a < run_hi; a += PG)
        __atomic_fetch_and(&pstate[a / PG], (uint8_t)~PS_ARMED, __ATOMIC_ACQ_REL),
            __atomic_fetch_or(&pstate[a / PG], PS_NOWATCH, __ATOMIC_ACQ_REL);
    return 0;
}
#endif

uint64_t plat_mem_written(uint64_t addr, uint64_t n, uint64_t *pages, uint64_t max, int reset) {
    if (!max || !n) return 0;
#ifdef __ANDROID__
    if (!pstate || addr >= PS_LIMIT || n > PS_LIMIT - addr) return UINT64_MAX;
    uint64_t p0 = addr / PG, p1 = (addr + n + PG - 1) / PG;
    uint64_t got = 0, run_lo = UINT64_MAX, run_hi = 0;
    for (uint64_t i = p0; i < p1 && got < max;) {
        if ((i & 7) == 0 && i + 8 <= p1) {
            uint64_t w;
            memcpy(&w, &pstate[i], 8);
            const uint64_t watched8 = 0x4040404040404040ull;
            const uint64_t dirty_or_nowatch8 = 0x9090909090909090ull;
            if ((w & watched8) == watched8 && !(w & dirty_or_nowatch8)) {
                if (run_lo != UINT64_MAX) {
                    if (!arm_flush(run_lo, run_hi)) return UINT64_MAX;
                    run_lo = UINT64_MAX;
                }
                i += 8;
                continue;
            }
        }
        uint8_t v = __atomic_load_n(&pstate[i], __ATOMIC_ACQUIRE);
        if (!(v & PS_WATCHED) || (v & PS_NOWATCH)) {
            arm_flush(run_lo, run_hi);
            return UINT64_MAX;
        }
        int need_mprotect = 0;
        if (v & PS_DIRTY) {
            pages[got++] = i * PG;
            if (reset) {
                while (v & PS_DIRTY) {
                    int can_arm = (v & PS_MASK) == PS_RW && !(v & PS_NOWATCH);
                    uint8_t nv = (uint8_t)((v & ~PS_DIRTY) | (can_arm ? PS_ARMED : 0));
                    int was_armed = (v & PS_ARMED) != 0;
                    if (__atomic_compare_exchange_n(&pstate[i], &v, nv, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
                        need_mprotect = can_arm && !was_armed;
                        break;
                    }
                }
            }
        }
        if (need_mprotect) {
            if (run_lo == UINT64_MAX) run_lo = i * PG;
            run_hi = (i + 1) * PG;
        } else if (run_lo != UINT64_MAX) {
            if (!arm_flush(run_lo, run_hi)) return UINT64_MAX;
            run_lo = UINT64_MAX;
        }
        i++;
    }
    if (!arm_flush(run_lo, run_hi)) return UINT64_MAX;
    return got;
#else
    if (ww_fd < 0) return UINT64_MAX;
    struct page_region r[256];
    uint64_t got = 0;
    for (uint64_t at = addr, end = addr + n; at < end && got < max;) {
        // Written means: write-protection cleared, on a populated page (present or swapped out) that is not the shared
        // zero page. A page never touched since its commit has no protection marker either and counts as written to
        // the kernel; a read maps the zero page there. Neither is a write, as on Windows.
        struct pm_scan_arg arg = {.size = sizeof arg,
                                  .flags = reset ? PM_SCAN_WP_MATCHING | PM_SCAN_CHECK_WPASYNC : 0,
                                  .start = at, .end = end, .vec = (uint64_t)(uintptr_t)r, .vec_len = 256,
                                  .max_pages = max - got,
                                  .category_inverted = PAGE_IS_PFNZERO,
                                  .category_mask = PAGE_IS_WRITTEN | PAGE_IS_PFNZERO,
                                  .category_anyof_mask = PAGE_IS_PRESENT | PAGE_IS_SWAPPED,
                                  .return_mask = PAGE_IS_WRITTEN};
        long k = ioctl(pm_fd, PAGEMAP_SCAN, &arg);
        if (k < 0) return UINT64_MAX;
        for (long i = 0; i < k; i++)
            for (uint64_t p = r[i].start; p < r[i].end && got < max; p += PG) pages[got++] = p;
        if (arg.walk_end <= at) break;
        at = arg.walk_end;
    }
    return got;
#endif
}

// One handler for the synchronous faults. A SIGSEGV whose page-fault error code has the write bit set goes to the
// write-fault hook first (memory.c thaws a frozen region); a fault in this executable's code then goes to the crash
// hook (main.c); anything else gets the default action. Runs on the thread's signal stack.
static int (*fault_hook)(uint64_t addr);
static void (*crash_fn)(const PlatCrash *c);

#if defined(__aarch64__)
// Walks ucontext's __reserved extension records for ESR_EL1 (ESR_MAGIC 0x45535201): WnR is bit 6, EC is bits 31..26
// (0x20/0x21 = instruction abort).
static void arm64_fault_access(const ucontext_t *u, int *is_write, int *is_exec) {
    *is_write = *is_exec = 0;
    const uint8_t *buf = u->uc_mcontext.__reserved;
    size_t cap = sizeof u->uc_mcontext.__reserved, off = 0;
    while (off + 8 <= cap) {
        uint32_t magic, size;
        memcpy(&magic, buf + off, 4);
        memcpy(&size, buf + off + 4, 4);
        if (!magic || size < 8 || (size & 15) || off + size > cap) break;
        if (magic == 0x45535201u && size >= 16) {
            uint64_t esr;
            memcpy(&esr, buf + off + 8, 8);
            uint32_t ec = (uint32_t)((esr >> 26) & 0x3fu);
            *is_exec = (ec == 0x20u || ec == 0x21u);
            *is_write = !*is_exec && ((esr & (1ull << 6)) != 0);
            return;
        }
        off += size;
    }
}
#endif

static void fault_handler(int sig, siginfo_t *si, void *uc) {
    ucontext_t *u = uc;
#if defined(__aarch64__)
    int is_write = 0, is_exec = 0;
    arm64_fault_access(u, &is_write, &is_exec);
    uintptr_t rip = (uintptr_t)u->uc_mcontext.pc;
#else
    long err = u->uc_mcontext.gregs[REG_ERR];
    int is_write = (err & 2) != 0, is_exec = (err & 16) != 0;
    uintptr_t rip = (uintptr_t)u->uc_mcontext.gregs[REG_RIP];
#endif
    if (sig == SIGSEGV && is_write) {
#ifdef __ANDROID__
        uint64_t a = (uint64_t)(uintptr_t)si->si_addr;
        if (pstate && a < PS_LIMIT) {
            uint64_t pg = a / PG, base = pg * PG;
            uint8_t v = __atomic_load_n(&pstate[pg], __ATOMIC_ACQUIRE);
            while ((v & (PS_MASK | PS_ARMED)) == (PS_RW | PS_ARMED)) {
                uint8_t nv = (uint8_t)((v & ~PS_ARMED) | PS_DIRTY);
                if (__atomic_compare_exchange_n(&pstate[pg], &v, nv, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
                    if (mprotect((void *)(uintptr_t)base, PG, PROT_READ | PROT_WRITE) == 0) return;
                    break;
                }
            }
            if ((v & (PS_MASK | PS_WATCHED | PS_DIRTY)) == (PS_RW | PS_WATCHED | PS_DIRTY) &&
                mprotect((void *)(uintptr_t)base, PG, PROT_READ | PROT_WRITE) == 0)
                return;
        }
#endif
        if (fault_hook && fault_hook((uint64_t)(uintptr_t)si->si_addr)) return;
    }
    Dl_info at, self;
    if (crash_fn && dladdr((void *)rip, &at) && dladdr((void *)plat_crash_hook, &self) && at.dli_fbase == self.dli_fbase) {
        PlatCrash c = {sig == SIGSEGV ? "access violation" : sig == SIGBUS ? "bus error"
                       : sig == SIGILL ? "illegal instruction" : "arithmetic exception",
                       (uint32_t)sig, rip, rip - (uintptr_t)self.dli_fbase, -1, 0};
        if (sig == SIGSEGV || sig == SIGBUS)
            c.access = is_exec ? 8 : is_write ? 1 : 0, c.addr = (uint64_t)(uintptr_t)si->si_addr;
        crash_fn(&c);
    }
    signal(sig, SIG_DFL);   // returns into the faulting instruction, which faults again with the default action
}

static void fault_install(void) {
    static int done;
    if (done) return;
    done = 1;
    alt_stack();
    struct sigaction sa = {.sa_sigaction = fault_handler, .sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER};
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
}

void plat_mem_fault_hook(int (*fn)(uint64_t addr)) {
    fault_hook = fn;
    fault_install();
}

void plat_crash_hook(void (*fn)(const PlatCrash *c)) {
    crash_fn = fn;
    fault_install();
}

void plat_reexec(void) {
    static char args[16384];
    char *argv[256];
    int argc = 0;
    FILE *f = fopen("/proc/self/cmdline", "rb");
    size_t n = f ? fread(args, 1, sizeof args - 1, f) : 0;
    if (f) fclose(f);
    args[n] = 0;
    for (size_t i = 0; i < n && argc < 255; i += strlen(args + i) + 1) argv[argc++] = args + i;
    argv[argc] = NULL;
    fflush(stdout);
    fflush(stderr);
    execv("/proc/self/exe", argv);
    fprintf(stderr, "FATAL: re-running the process failed (%s)\n", strerror(errno));
    _exit(70);
}

int plat_mem_read(uint64_t addr, void *out, size_t n) {
    struct iovec local = {out, n}, remote = {(void *)(uintptr_t)addr, n};
    return process_vm_readv(getpid(), &local, 1, &remote, 1, 0) == (ssize_t)n;
}

int plat_mem_readable(uint64_t addr, uint64_t n) {
    uint8_t b;
    uint64_t end = addr + (n ? n : 1);
    for (uint64_t a = addr; a < end; a = (a | 4095) + 1)
        if (!plat_mem_read(a, &b, 1)) return 0;
    return 1;
}

void *plat_aligned_alloc(size_t size, size_t align) {
    void *p = NULL;
    return posix_memalign(&p, align < sizeof(void *) ? sizeof(void *) : align, size) ? NULL : p;
}

void plat_aligned_free(void *p) { free(p); }

// A PlatFile is the descriptor + 1, so descriptor 0 is not NULL.
PlatFile *plat_file_open(const char *path) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    return fd < 0 ? NULL : (PlatFile *)(intptr_t)(fd + 1);
}

PlatFile *plat_file_open_unbuffered(const char *path) {
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_DIRECT);
    return fd < 0 ? NULL : (PlatFile *)(intptr_t)(fd + 1);
}

static int fd_of(PlatFile *f) { return (int)(intptr_t)f - 1; }

void plat_file_close(PlatFile *f) {
    if (f) close(fd_of(f));
}

int64_t plat_file_pread(PlatFile *f, void *buf, uint32_t n, uint64_t off) {
    ssize_t k;
    do k = pread(fd_of(f), buf, n, (off_t)off);
    while (k < 0 && errno == EINTR);
#ifdef __ANDROID__
    if (k < 0 && errno == EFAULT && n) {
        plat_mem_disarm((uint64_t)(uintptr_t)buf, n);
        do k = pread(fd_of(f), buf, n, (off_t)off);
        while (k < 0 && errno == EINTR);
    }
    if (k > 0) plat_mem_disarm((uint64_t)(uintptr_t)buf, (uint64_t)k);
#endif
    return k >= 0 ? k : errno == EFAULT ? -2 : -1;
}

int plat_file_id(PlatFile *f, PlatFileId *out) {
    struct stat st;
    if (fstat(fd_of(f), &st)) return 0;
    *out = (PlatFileId){(uint64_t)st.st_dev, (uint64_t)st.st_ino, (uint64_t)st.st_size,
                        (uint64_t)st.st_mtim.tv_sec * 1000000000u + (uint64_t)st.st_mtim.tv_nsec,
                        (uint64_t)st.st_ctim.tv_sec * 1000000000u + (uint64_t)st.st_ctim.tv_nsec};
    return 1;
}

const void *plat_file_map(PlatFile *f) {
    struct stat st;
    if (fstat(fd_of(f), &st) || !st.st_size) return NULL;
    void *v = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_SHARED, fd_of(f), 0);
    return v == MAP_FAILED ? NULL : v;
}

static int fs_err(int e) {
    switch (e) {
    case ENOENT: case ENOTDIR: return PLAT_FS_NOT_FOUND;
    case EEXIST: return PLAT_FS_EXISTS;
    case ENOTEMPTY: return PLAT_FS_NOT_EMPTY;
    case EBUSY: case ETXTBSY: return PLAT_FS_BUSY;
    default: return PLAT_FS_ERROR;
    }
}

int plat_fs_kind(const char *path) {
    struct stat st;
    if (lstat(path, &st)) return PLAT_FS_MISSING;
    return S_ISLNK(st.st_mode) ? PLAT_FS_LINK : S_ISDIR(st.st_mode) ? PLAT_FS_DIR : PLAT_FS_FILE;
}

int plat_fs_mkdir(const char *path) { return mkdir(path, 0755) ? fs_err(errno) : PLAT_FS_OK; }

int plat_fs_rmdir(const char *path) {
    struct stat st;
    if (!lstat(path, &st) && S_ISLNK(st.st_mode)) return unlink(path) ? fs_err(errno) : PLAT_FS_OK;
    return rmdir(path) ? fs_err(errno) : PLAT_FS_OK;
}

// Deleting needs write access to the directory, not to the file: a read-only file goes as any other.
int plat_fs_unlink(const char *path) { return unlink(path) ? fs_err(errno) : PLAT_FS_OK; }

// RENAME_NOREPLACE keeps Windows' rule that a rename never replaces; a file system without it (EINVAL) gets a check
// first, which another process could race but the save directories are ours.
int plat_fs_rename(const char *from, const char *to, int durable) {
    if (syscall(SYS_renameat2, AT_FDCWD, from, AT_FDCWD, to, RENAME_NOREPLACE)) {
        if (errno != EINVAL && errno != ENOSYS) return fs_err(errno);
        if (plat_fs_kind(to) != PLAT_FS_MISSING) return PLAT_FS_EXISTS;
        if (rename(from, to)) return fs_err(errno);
    }
    if (durable) {
        sync_dir_of(from);
        sync_dir_of(to);
    }
    return PLAT_FS_OK;
}

int plat_fs_link(const char *existing, const char *path) { return link(existing, path) ? fs_err(errno) : PLAT_FS_OK; }

int plat_fs_links(const char *path) {
    struct stat st;
    return stat(path, &st) ? 0 : (int)st.st_nlink;
}

void plat_fs_sync_dir(const char *path) {
    int fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd >= 0) {
        fsync(fd);
        close(fd);
    }
}

int plat_fs_list(const char *dir, int (*cb)(const char *name, int kind, uint64_t size, void *ctx), void *ctx) {
    DIR *d = opendir(dir);
    if (!d) return fs_err(errno);
    for (struct dirent *e; (e = readdir(d));) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        struct stat st;
        if (fstatat(dirfd(d), e->d_name, &st, AT_SYMLINK_NOFOLLOW)) continue;
        int kind = S_ISLNK(st.st_mode) ? PLAT_FS_LINK : S_ISDIR(st.st_mode) ? PLAT_FS_DIR : PLAT_FS_FILE;
        if (!cb(e->d_name, kind, kind == PLAT_FS_FILE ? (uint64_t)st.st_size : 0, ctx)) break;
    }
    closedir(d);
    return PLAT_FS_OK;
}

PlatFile *plat_fs_open(const char *path, int write, int *err) {
    struct stat st;
    int fd = open(path, (write ? O_RDWR : O_RDONLY) | O_CLOEXEC);
    if (fd >= 0 && (fstat(fd, &st) || !S_ISREG(st.st_mode))) {
        close(fd);
        fd = -1, errno = ENOENT;
    }
    if (fd < 0 && err) *err = fs_err(errno);
    return fd < 0 ? NULL : (PlatFile *)(intptr_t)(fd + 1);
}

PlatFile *plat_fs_create(const char *path, int *err) {
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (fd < 0 && err) *err = fs_err(errno);
    return fd < 0 ? NULL : (PlatFile *)(intptr_t)(fd + 1);
}

int64_t plat_file_pwrite(PlatFile *f, const void *buf, uint32_t n, uint64_t off) {
    ssize_t k;
    do k = pwrite(fd_of(f), buf, n, (off_t)off);
    while (k < 0 && errno == EINTR);
    return k;
}

int plat_file_set_size(PlatFile *f, uint64_t size) { return ftruncate(fd_of(f), (off_t)size) ? fs_err(errno) : 0; }

int plat_file_fsync(PlatFile *f) { return fsync(fd_of(f)) == 0; }

int plat_temp_dir(char *out, size_t cap) {
    const char *t = getenv("TMPDIR");
    if (!t || !*t) t = "/tmp";
    size_t n = strlen(t);
    while (n > 1 && t[n - 1] == '/') n--;
    return snprintf(out, cap, "%.*s", (int)n, t) < (int)cap;
}

uintptr_t plat_exe_base(void) {
    Dl_info d;
    return dladdr((void *)plat_exe_base, &d) ? (uintptr_t)d.dli_fbase : 0;
}

int plat_module_of(uintptr_t addr, char *name, size_t cap, uintptr_t *base) {
    Dl_info d;
    if (!dladdr((void *)addr, &d) || !d.dli_fname) return 0;
    const char *n = strrchr(d.dli_fname, '/');
    *base = (uintptr_t)d.dli_fbase;
    return snprintf(name, cap, "%s", n ? n + 1 : d.dli_fname) < (int)cap;
}

int plat_exe_path(char *out, size_t cap) {
    ssize_t n = readlink("/proc/self/exe", out, cap - 1);
    if (n <= 0 || (size_t)n >= cap - 1) return 0;
    out[n] = 0;
    return 1;
}

int plat_data_dir(char *out, size_t cap) {
#ifdef __ANDROID__
    const char *env = getenv("XC2_DATA_DIR");
    const char *cand = (env && *env) ? env
                     : plat_dir_exists("/storage/emulated/0/Android/data/com.xc2recomp.app/files")
                         ? "/storage/emulated/0/Android/data/com.xc2recomp.app/files"
                         : "/sdcard/Android/data/com.xc2recomp.app/files";
    size_t n = strlen(cand);
    while (n > 1 && (cand[n - 1] == '/' || cand[n - 1] == '\\')) n--;
    if (!n || n >= cap) return 0;
    memcpy(out, cand, n);
    out[n] = 0;
    return 1;
#else
    if (!plat_exe_path(out, cap)) return 0;
    char *cut = plat_last_sep(out);
    if (cut) *cut = 0;
    else if (cap < 2) return 0;
    else strcpy(out, ".");
    return 1;
#endif
}

size_t plat_getenv(const char *name, char *out, size_t cap) {
    const char *v = getenv(name);
    size_t n = v ? strlen(v) : 0;
    if (!n || n >= cap) return 0;
    memcpy(out, v, n + 1);
    return n;
}

// Cached per thread: plat_rmutex_lock asks on every acquire, hundreds of thousands of times a second in play, and
// gettid is a full syscall on Linux (Windows reads GetCurrentThreadId from the TEB). A thread's id never changes, and
// the runtime starts child processes with posix_spawn only, so no forked copy inherits a stale id.
uint32_t plat_thread_id(void) {
    static __thread uint32_t tid;
    if (!tid) tid = (uint32_t)syscall(SYS_gettid);
    return tid;
}

// The first set variable of names ("ru_RU.UTF-8") as a BCP-47 tag ("ru-RU") into tag; "" for C and POSIX.
static void locale_tag(const char *const *names, int n, char *tag, size_t cap) {
    const char *v = NULL;
    for (int i = 0; i < n && !(v && *v); i++) v = getenv(names[i]);
    size_t k = 0;
    if (v && strcmp(v, "C") && strncmp(v, "C.", 2) && strcmp(v, "POSIX"))
        for (; v[k] && v[k] != '.' && v[k] != '@' && k < cap - 1; k++) tag[k] = v[k] == '_' ? '-' : v[k];
    tag[k] = 0;
}

const char *plat_ui_language(void) {
    static char tag[32];
    static const char *const names[] = {"LC_ALL", "LC_MESSAGES", "LANG"};
    if (!tag[0]) locale_tag(names, 3, tag, sizeof tag);
    return tag;
}

const char *plat_user_locale(void) {
    static char tag[32];
    static const char *const names[] = {"LC_ALL", "LC_TIME", "LANG"};
    if (!tag[0]) locale_tag(names, 3, tag, sizeof tag);
    return tag;
}

// Noto CJK as the common distributions and SteamOS install it; one collection covers every CJK language.
int plat_font_path(const char *lang, char *out, size_t cap) {
    static const char *files[] = {
#ifdef __ANDROID__
        "/system/fonts/NotoSansCJK-Regular.ttc",
        "/product/fonts/NotoSansCJK-Regular.ttc",
#endif
        "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/google-noto-cjk/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/truetype/noto/NotoSansCJK-Regular.ttc",
    };
    if (strcmp(lang, "ja") && strcmp(lang, "ko") && strcmp(lang, "zh-Hans") && strcmp(lang, "zh-Hant")) return 0;
    for (size_t i = 0; i < sizeof files / sizeof *files; i++)
        if (plat_file_exists(files[i])) return snprintf(out, cap, "%s", files[i]) < (int)cap;
    return 0;
}

// PRETTY_NAME of /etc/os-release and the kernel release: "SteamOS (Linux 6.16.12-valve)".
int plat_os_version(char *out, size_t cap) {
#ifdef __ANDROID__
    {
        char rel[PROP_VALUE_MAX] = "", sdk[PROP_VALUE_MAX] = "";
        __system_property_get("ro.build.version.release", rel);
        __system_property_get("ro.build.version.sdk", sdk);
        struct utsname u;
        if (!uname(&u) && rel[0] && sdk[0])
            return snprintf(out, cap, "Android %s (SDK %s, Linux %s)", rel, sdk, u.release) < (int)cap;
        if (rel[0]) return snprintf(out, cap, "Android %s", rel) < (int)cap;
    }
#endif
    char name[128] = "Linux";
    FILE *f = fopen("/etc/os-release", "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof line, f))
            if (!strncmp(line, "PRETTY_NAME=", 12)) {
                char *v = line + 12, *e;
                if (*v == '"') v++;
                if ((e = strpbrk(v, "\"\n"))) *e = 0;
                snprintf(name, sizeof name, "%s", v);
                break;
            }
        fclose(f);
    }
    struct utsname u;
    if (uname(&u)) return snprintf(out, cap, "%s", name) < (int)cap;
    return snprintf(out, cap, "%s (Linux %s)", name, u.release) < (int)cap;
}

int plat_stderr_path(char *out, size_t cap) {
    struct stat st;
    if (fstat(2, &st) || !S_ISREG(st.st_mode)) return 0;
    ssize_t n = readlink("/proc/self/fd/2", out, cap - 1);
    if (n <= 0 || (size_t)n >= cap - 1) return 0;
    out[n] = 0;
    return 1;
}

// ---- process

void plat_exit(int code) {
    fflush(stdout);
    fflush(stderr);
    _exit(code);
}

int plat_setenv(const char *name, const char *value) {
    return (value ? setenv(name, value, 1) : unsetenv(name)) == 0;
}

uint32_t plat_process_id(void) { return (uint32_t)getpid(); }

uint64_t plat_process_cpu_ns(void) {
    struct timespec t;
    if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &t)) return 0;
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}

// Field 22 of /proc/self/stat is the start time in clock ticks after boot; CLOCK_BOOTTIME is now after boot.
uint64_t plat_process_uptime_ns(void) {
    FILE *f = fopen("/proc/self/stat", "r");
    if (!f) return 0;
    char buf[1024];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    char *p = strrchr(buf, ')');   // the command name may hold spaces
    unsigned long long start = 0;
    if (!p || sscanf(p + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %*u %*u %*d %*d %*d %*d %*d %*d %llu",
                     &start) != 1)
        return 0;
    struct timespec t;
    clock_gettime(CLOCK_BOOTTIME, &t);
    uint64_t now = (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
    uint64_t at = start * (1000000000u / (uint64_t)sysconf(_SC_CLK_TCK));
    return now > at ? now - at : 0;
}

uint64_t plat_unix_time_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    return (uint64_t)t.tv_sec * 1000000000u + (uint64_t)t.tv_nsec;
}

void plat_local_time(struct tm *out) {
    time_t t = time(NULL);
    localtime_r(&t, out);
}

// The range of the Windows CRT's localtime_s and gmtime_s (1970 to 3000), which glibc exceeds: the game's time
// service reports times outside it as invalid on both hosts.
#define PLAT_TIME_MAX 32535215999ll

int plat_localtime(int64_t unix_s, struct tm *out) {
    time_t t = (time_t)unix_s;
    return unix_s >= 0 && unix_s <= PLAT_TIME_MAX && localtime_r(&t, out) != NULL;
}

int plat_gmtime(int64_t unix_s, struct tm *out) {
    time_t t = (time_t)unix_s;
    return unix_s >= 0 && unix_s <= PLAT_TIME_MAX && gmtime_r(&t, out) != NULL;
}

long plat_tz_west(void) {
#ifdef __ANDROID__
    time_t now = time(NULL);
    struct tm tm;
    if (localtime_r(&now, &tm)) return -tm.tm_gmtoff;
    return 0;
#else
    tzset();
    return timezone;
#endif
}

int plat_run_self(const char *env_name, const char *env_value, char *out, size_t cap, uint32_t timeout_ms) {
    // the arguments, NUL-separated, from /proc/self/cmdline
    static char args[16384];
    char *argv[256];
    int argc = 0;
    FILE *f = fopen("/proc/self/cmdline", "rb");
    size_t n = f ? fread(args, 1, sizeof args - 1, f) : 0;
    if (f) fclose(f);
    args[n] = 0;
    for (size_t i = 0; i < n && argc < 255; i += strlen(args + i) + 1) argv[argc++] = args + i;
    argv[argc] = NULL;
    char exe[PATH_MAX];
    if (!argc || !plat_exe_path(exe, sizeof exe)) return -1;
    int p[2];
    if (pipe2(p, O_CLOEXEC)) return -1;
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, p[1], 1);
    posix_spawn_file_actions_adddup2(&fa, p[1], 2);
    setenv(env_name, env_value, 1);
    pid_t pid;
    int e = posix_spawn(&pid, exe, &fa, NULL, argv, environ);
    unsetenv(env_name);
    posix_spawn_file_actions_destroy(&fa);
    close(p[1]);
    if (e) {
        close(p[0]);
        return -1;
    }
    size_t len = 0;
    char sink[512];
    ssize_t k;
    while (len + 1 < cap && (k = read(p[0], out + len, cap - 1 - len)) != 0)
        if (k > 0) len += (size_t)k;
        else if (errno != EINTR) break;
    if (cap) out[len] = 0;
    while ((k = read(p[0], sink, sizeof sink)) > 0 || (k < 0 && errno == EINTR)) {}
    close(p[0]);
    int status = 0;
    uint64_t end = plat_now_ns() + (uint64_t)timeout_ms * 1000000u;
    while (waitpid(pid, &status, timeout_ms ? WNOHANG : 0) == 0) {
        if (plat_now_ns() >= end) {
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            break;
        }
        plat_sleep_ms(10);
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
}

int plat_random(void *out, size_t n) {
    uint8_t *p = out;
    while (n) {
        ssize_t k = getrandom(p, n, 0);
        if (k < 0 && errno == EINTR) continue;
        if (k <= 0) return 0;
        p += k, n -= (size_t)k;
    }
    return 1;
}

// ---- desktop

int plat_clipboard_set(const char *utf8) { return SDL_SetClipboardText(utf8); }

// Started from a desktop launcher, the game has no terminal on stderr to show the message in.
void plat_fatal_notice(const char *utf8) {
    if (!isatty(STDERR_FILENO)) SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "XC2 Recomp", utf8, NULL);
}

static void reap_main(void *arg) {
    waitpid((pid_t)(intptr_t)arg, NULL, 0);
}

// The desktop's file manager answers for the folder; no integrity level stops it here.
int plat_folder_can_open(void) { return 1; }

// xdg-open picks the desktop's file manager; it runs detached, so a slow answer does not hold up the caller, and a
// thread of its own reaps it so it does not stay a zombie.
int plat_open_folder(const char *path) {
    struct stat st;
    if (stat(path, &st) || !S_ISDIR(st.st_mode)) return 0;
    posix_spawnattr_t a;
    posix_spawnattr_init(&a);
    posix_spawnattr_setflags(&a, POSIX_SPAWN_SETSID);
    char *argv[] = {"xdg-open", (char *)path, NULL};
    pid_t pid;
    int e = posix_spawnp(&pid, "xdg-open", NULL, &a, argv, environ);
    posix_spawnattr_destroy(&a);
    if (e) return 0;
    plat_thread_spawn(reap_main, (void *)(intptr_t)pid, "reap", 64 << 10);
    return 1;
}

// ---- dynamic libraries

void *plat_lib_open(const char *name) { return dlopen(name, RTLD_NOW | RTLD_LOCAL); }
void *plat_lib_sym(void *lib, const char *name) { return lib ? dlsym(lib, name) : NULL; }

// ---- machine load: /proc/stat and /proc/self/stat for the CPU, hwmon for the temperature, the amdgpu sysfs files of
// the render device for the GPU.

struct PlatLoad {
    uint64_t busy, total, proc;   // the previous call's jiffies; total 0 before the first
    uint64_t proc_ns;
    char gpu[256];                // /sys/class/drm/cardN/device of the GPU in use; empty when unknown
    char temp[256];               // the CPU's hwmon temp1_input; empty when unknown
};

static int read_u64_file(const char *path, uint64_t *v) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    unsigned long long x;
    int ok = fscanf(f, "%llu", &x) == 1;
    fclose(f);
    if (ok) *v = x;
    return ok;
}

// The hwmon device of the CPU package: k10temp (AMD), coretemp (Intel), or the first that has a temp1_input.
static void find_cpu_temp(char *out, size_t cap) {
    out[0] = 0;
    DIR *d = opendir("/sys/class/hwmon");
    if (d) {
        for (struct dirent *e; (e = readdir(d));) {
            if (e->d_name[0] == '.') continue;
            char path[256], name[64] = "";
            snprintf(path, sizeof path, "/sys/class/hwmon/%s/name", e->d_name);
            FILE *f = fopen(path, "r");
            if (!f) continue;
            if (!fgets(name, sizeof name, f)) name[0] = 0;
            fclose(f);
            name[strcspn(name, "\n")] = 0;
            int cpu = !strcmp(name, "k10temp") || !strcmp(name, "coretemp") || !strcmp(name, "zenpower");
            snprintf(path, sizeof path, "/sys/class/hwmon/%s/temp1_input", e->d_name);
            if ((cpu || !out[0]) && plat_file_exists(path)) snprintf(out, cap, "%s", path);
            if (cpu && out[0]) break;
        }
        closedir(d);
    }
#ifdef __ANDROID__
    if (!out[0]) {
        DIR *td = opendir("/sys/class/thermal");
        if (td) {
            for (struct dirent *e; (e = readdir(td));) {
                if (strncmp(e->d_name, "thermal_zone", 12)) continue;
                char path[256], type[64] = "";
                snprintf(path, sizeof path, "/sys/class/thermal/%s/type", e->d_name);
                FILE *f = fopen(path, "r");
                if (!f) continue;
                if (!fgets(type, sizeof type, f)) type[0] = 0;
                fclose(f);
                type[strcspn(type, "\r\n")] = 0;
                int cpu = strstr(type, "cpu") != NULL || strstr(type, "cpuss") != NULL || strstr(type, "socd") != NULL;
                snprintf(path, sizeof path, "/sys/class/thermal/%s/temp", e->d_name);
                if ((cpu || !out[0]) && plat_file_exists(path)) snprintf(out, cap, "%s", path);
                if (cpu && out[0]) break;
            }
            closedir(td);
        }
    }
#endif
}

// The DRM card with gpu_busy_percent (amdgpu); with several, the one with the most VRAM in use, as the game's device
// is not known by LUID on Linux. On Qualcomm Android, fall back to /sys/class/kgsl/kgsl-3d0.
static void find_gpu(char *out, size_t cap) {
    out[0] = 0;
    uint64_t best = 0;
    DIR *d = opendir("/sys/class/drm");
    if (d) {
        for (struct dirent *e; (e = readdir(d));) {
            if (strncmp(e->d_name, "card", 4) || strchr(e->d_name, '-')) continue;
            char dev[256], path[300];
            snprintf(dev, sizeof dev, "/sys/class/drm/%s/device", e->d_name);
            snprintf(path, sizeof path, "%s/gpu_busy_percent", dev);
            uint64_t v, used = 0;
            if (!read_u64_file(path, &v)) continue;
            snprintf(path, sizeof path, "%s/mem_info_vram_used", dev);
            read_u64_file(path, &used);
            if (!out[0] || used > best) snprintf(out, cap, "%s", dev), best = used;
        }
        closedir(d);
    }
#ifdef __ANDROID__
    if (!out[0] && plat_dir_exists("/sys/class/kgsl/kgsl-3d0"))
        snprintf(out, cap, "/sys/class/kgsl/kgsl-3d0");
#endif
}

PlatLoad *plat_load_open(const uint8_t *gpu_luid) {
    (void)gpu_luid;
    PlatLoad *l = calloc(1, sizeof *l);
    if (!l) return NULL;
    find_cpu_temp(l->temp, sizeof l->temp);
    find_gpu(l->gpu, sizeof l->gpu);
    return l;
}

void plat_load_close(PlatLoad *l) { free(l); }

void plat_cpu_load(PlatLoad *l, float *total_pct, float *proc_pct, float *temp_c) {
    *total_pct = *proc_pct = *temp_c = -1;
    if (!l) return;
    FILE *f = fopen("/proc/stat", "r");
    unsigned long long v[10] = {0};
    int n = f ? fscanf(f, "cpu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu", &v[0], &v[1], &v[2], &v[3], &v[4],
                       &v[5], &v[6], &v[7], &v[8], &v[9])
              : 0;
    if (f) fclose(f);
    uint64_t pns = plat_process_cpu_ns();
    if (n >= 8) {
        uint64_t idle = v[3] + v[4], total = 0;
        for (int i = 0; i < 8; i++) total += v[i];   // guest time is already inside user time
        uint64_t busy = total - idle;
        if (l->total && total > l->total) {
            uint64_t dt = total - l->total;
            *total_pct = 100.0f * (float)(busy - l->busy) / (float)dt;
            // the process time is in ns, the machine's in jiffies summed over every CPU
            double dt_ns = (double)dt * 1e9 / (double)sysconf(_SC_CLK_TCK);
            *proc_pct = (float)(100.0 * (double)(pns - l->proc_ns) / dt_ns);
        }
        l->busy = busy, l->total = total, l->proc_ns = pns;
    }
    uint64_t mc;
    if (l->temp[0] && read_u64_file(l->temp, &mc)) *temp_c = mc > 1000 ? (float)mc / 1000.0f : (float)mc;
}

void plat_gpu_load(PlatLoad *l, float *load_pct, float *vram_used_mb) {
    *load_pct = *vram_used_mb = -1;
    if (!l || !l->gpu[0]) return;
    char path[300];
    uint64_t v;
    snprintf(path, sizeof path, "%s/gpu_busy_percent", l->gpu);
    if (read_u64_file(path, &v)) *load_pct = (float)v;
#ifdef __ANDROID__
    if (*load_pct < 0) {
        snprintf(path, sizeof path, "%s/gpu_busy_percentage", l->gpu);
        if (read_u64_file(path, &v)) *load_pct = (float)v;
    }
    if (*load_pct < 0) {
        snprintf(path, sizeof path, "%s/gpubusy", l->gpu);
        FILE *f = fopen(path, "r");
        if (f) {
            unsigned long long busy = 0, total = 0;
            if (fscanf(f, "%llu %llu", &busy, &total) == 2 && total > 0)
                *load_pct = 100.0f * (float)busy / (float)total;
            fclose(f);
        }
    }
#endif
    snprintf(path, sizeof path, "%s/mem_info_vram_used", l->gpu);
    if (read_u64_file(path, &v)) *vram_used_mb = (float)(v >> 20);
}

#ifdef __ANDROID__
void plat_android_gpu_extra(PlatLoad *l, float *temp_c, float *clock_mhz) {
    if (!l || !l->gpu[0]) return;
    char path[300];
    uint64_t v;
    if (temp_c && *temp_c < 0) {
        snprintf(path, sizeof path, "%s/temp", l->gpu);
        if (read_u64_file(path, &v)) *temp_c = v > 1000 ? (float)v / 1000.0f : (float)v;
    }
    if (clock_mhz && *clock_mhz < 0) {
        snprintf(path, sizeof path, "%s/clock_mhz", l->gpu);
        if (read_u64_file(path, &v) && v > 0) *clock_mhz = (float)v;
        else {
            snprintf(path, sizeof path, "%s/gpuclk", l->gpu);
            if (read_u64_file(path, &v) && v > 0) *clock_mhz = (float)v / 1000000.0f;
        }
    }
}

static void parse_proc_status_text(const char *buf, float *rss_mb, float *swap_mb, float *anon_mb) {
    if (rss_mb) *rss_mb = -1;
    if (swap_mb) *swap_mb = -1;
    if (anon_mb) *anon_mb = -1;
    if (!buf) return;
    const char *p = buf;
    while (*p) {
        unsigned long long k = 0;
        if (rss_mb && sscanf(p, "VmRSS: %llu kB", &k) == 1) *rss_mb = (float)k / 1024.0f;
        else if (swap_mb && sscanf(p, "VmSwap: %llu kB", &k) == 1) *swap_mb = (float)k / 1024.0f;
        else if (anon_mb && sscanf(p, "RssAnon: %llu kB", &k) == 1) *anon_mb = (float)k / 1024.0f;
        const char *nl = strchr(p, '\n');
        if (!nl) break;
        p = nl + 1;
    }
}

void plat_android_mem_status(float *rss_mb, float *swap_mb, float *anon_mb) {
    if (rss_mb) *rss_mb = -1;
    if (swap_mb) *swap_mb = -1;
    if (anon_mb) *anon_mb = -1;
    FILE *f = fopen("/proc/self/status", "r");
    if (!f) return;
    char buf[4096];
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    parse_proc_status_text(buf, rss_mb, swap_mb, anon_mb);
}
#endif

void plat_mem_info(float *used_mb, float *total_mb, float *proc_mb) {
    *used_mb = *total_mb = *proc_mb = -1;
    FILE *f = fopen("/proc/meminfo", "r");
    if (f) {
        char line[128];
        unsigned long long total = 0, avail = 0, k;
        while (fgets(line, sizeof line, f)) {
            if (sscanf(line, "MemTotal: %llu kB", &k) == 1) total = k;
            else if (sscanf(line, "MemAvailable: %llu kB", &k) == 1) avail = k;
        }
        fclose(f);
        if (total) *total_mb = (float)(total >> 10), *used_mb = (float)((total - avail) >> 10);
    }
#ifdef __ANDROID__
    plat_android_mem_status(proc_mb, NULL, NULL);
    if (*proc_mb >= 0) return;
#endif
    f = fopen("/proc/self/statm", "r");
    unsigned long long size, resident;
    if (f && fscanf(f, "%llu %llu", &size, &resident) == 2) *proc_mb = (float)(resident * (uint64_t)sysconf(_SC_PAGESIZE) >> 20);
    if (f) fclose(f);
}

void plat_process_memory(uint64_t *resident, uint64_t *private_bytes) {
    *resident = *private_bytes = 0;
    FILE *f = fopen("/proc/self/status", "r");
    if (!f) return;
    char line[128];
    unsigned long long k;
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, "VmRSS: %llu kB", &k) == 1) *resident = (uint64_t)k << 10;
        else if (sscanf(line, "RssAnon: %llu kB", &k) == 1) *private_bytes = (uint64_t)k << 10;
    }
    fclose(f);
}

// ---- child processes

struct PlatChild {
    pid_t pid;
    int in, out;   // in: our write end, out: our read end
};

PlatChild *plat_child_start(const char *exe, const char *arg, uint32_t *err) {
    if (err) *err = 0;
    int in[2], out[2];
    if (pipe2(in, O_CLOEXEC)) return NULL;
    if (pipe2(out, O_CLOEXEC)) {
        close(in[0]), close(in[1]);
        return NULL;
    }
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, in[0], 0);
    posix_spawn_file_actions_adddup2(&fa, out[1], 1);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    char *argv[] = {(char *)exe, (char *)arg, NULL};
    pid_t pid;
    int e = posix_spawn(&pid, exe, &fa, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    close(in[0]);
    close(out[1]);
    if (e) {
        close(in[1]), close(out[0]);
        if (err) *err = (uint32_t)e;
        return NULL;
    }
    PlatChild *c = malloc(sizeof *c);
    if (!c) {
        kill(pid, SIGKILL);
        waitpid(pid, NULL, 0);
        close(in[1]), close(out[0]);
        return NULL;
    }
    c->pid = pid, c->in = in[1], c->out = out[0];
    return c;
}

void plat_child_stop(PlatChild *c) {
    if (!c) return;
    close(c->in);
    close(c->out);
    kill(c->pid, SIGKILL);
    waitpid(c->pid, NULL, 0);
    free(c);
}

// A child that died makes the write fail with EPIPE; SIGPIPE is ignored for the process (main.c), as Windows has none.
int plat_child_write(PlatChild *c, const void *data, size_t n) { return write_all(c->in, data, n); }

long plat_child_read(PlatChild *c, void *buf, size_t cap) {
    ssize_t k;
    do k = read(c->out, buf, cap);
    while (k < 0 && errno == EINTR);
    return (long)k;
}
