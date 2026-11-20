// Game-free fallback module glue for Android bring-up and --selftest before Switch NSP extraction.
// Once `generated/v301-android/` is generated from the user's own NSPs, `build/v301-android/modules.c`
// replaces this file in the link.
#include "dispatch.h"
#include "host.h"

static void selftest_noop_fn(Cpu *c) {
    (void)c;
}

const RtLayoutEntry rt_layout[] = {
    {"main", RT_MAIN_BASE, 0x200000ull},
};
const int rt_layout_count = 1;

static const uint64_t selftest_fn_addrs[] = {
    RT_MAIN_BASE + 0x100ull,
};
static const GuestFn selftest_fn_ptrs[] = {
    selftest_noop_fn,
};

int rt_linked_modules(RtModule *out, int max) {
    if (max <= 0) return 0;
    out[0] = (RtModule){
        "main",
        RT_MAIN_BASE,
        0x200000ull,
        selftest_fn_addrs,
        selftest_fn_ptrs,
        1,
    };
    return 1;
}

const RtFileHash rt_exefs_sha256[] = {
    {"", ""},
};
const int rt_exefs_sha256_count = 0;

const char rt_default_exefs[] = "exefs";
const char rt_default_romfs[] = "romfs";
const char rt_default_profile[] = "profile";
const int rt_game_version = 0;
const char rt_port_version[] = "0.1.0-android";
const char rt_game_name[] = "Xenoblade Chronicles 2 (Android Bring-up)";
const uint64_t rt_title_id = 0x0100e95004038000ull;
