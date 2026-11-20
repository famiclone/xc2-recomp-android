// Android Bionic constructor linked into NativeAOT libxc2_xlate.so: disables Scudo top-byte heap pointer tagging
// before the NativeAOT runtime initializes when libxc2_xlate.so is launched via posix_spawn/execve.
#include <dlfcn.h>

#ifndef M_BIONIC_SET_HEAP_TAGGING_LEVEL
#define M_BIONIC_SET_HEAP_TAGGING_LEVEL (-204)
#endif
#ifndef M_HEAP_TAGGING_LEVEL_NONE
#define M_HEAP_TAGGING_LEVEL_NONE 0
#endif

__attribute__((constructor(101))) static void xc2_xlate_bionic_init(void) {
    typedef int (*mallopt_fn)(int, int);
    mallopt_fn fn = (mallopt_fn)dlsym(RTLD_DEFAULT, "mallopt");
    if (fn) fn(M_BIONIC_SET_HEAP_TAGGING_LEVEL, M_HEAP_TAGGING_LEVEL_NONE);
}
