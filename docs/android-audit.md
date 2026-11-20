# Android (`arm64-v8a` / Adreno 740 / 8 GB RAM) Port Audit

Audit of `recomp/`, `runtime/`, and `tools/` for porting `xc2-recomp` to Android (`arm64-v8a`, Snapdragon 8 Gen 2, Adreno 740, 8 GB unified LPDDR5X). Every finding below maps directly to Phases 2–5 with file, function, and line hints.

---

## 1. Phase 2: CPU Architecture (`x86-64` AVX2 -> `ARM64` NEON, Asm, Signals, Atomics)

### 1.1 Vector Lifting & SIMD Intrinsics
The recompiler (`recomp/gen.py`, `lift_functions`, lines 250–251, 572, 661) lifts AArch64 NEON via `recomp/simd_c.py` (`lift_simd_c`) using Clang GNU vector extensions (`__attribute__((vector_size(16), may_alias))`, `__builtin_shufflevector`, `__builtin_convertvector`). Clang lowers 128-bit GNU vector types directly to native ARM64 NEON instructions without x86 intrinsics. (`recomp/lifter.py`, lines 561–594 and 711–805, also retains an unused `simd='asm'` path.)

All hard x86-64 intrinsics, builtins, and assembly constraints are confined to seven sites in `runtime/`:

| File | Function / Symbol | Lines | Current x86-64 Dependency | ARM64 (`__aarch64__`) Port Action |
| :--- | :--- | :--- | :--- | :--- |
| `runtime/simd.h` | top-level, `simd_rm_set_rc`, `simd_rm_enter`, `simd_rm_rz`, `simd_rm_leave` | 13, 215–231 | Unconditional `#include <immintrin.h>`; `_mm_getcsr()` / `_mm_setcsr()` MXCSR rounding control (`0x6000`). | Replace `<immintrin.h>` with `runtime/simd_platform.h` (`<immintrin.h>` on x86-64, SIMDe + `<arm_neon.h>` on `__aarch64__`). Read/write ARM `fpcr` bits 23:22 (`mrs`/`msr fpcr`, direct 1:1 `RMode` match). Optimize hot scalar-lane helpers (`sl_gen`, lines 98–112, `simd_frepxp32/64`, `simd_frsqrte32/64`) with `<arm_neon.h>` intrinsics (`vrecpxs_f32`, `vrsqrtes_f32`). |
| `runtime/simd_fp.h` | `fp_opaque32` | 190 | `__asm__("" : "+x"(u));` uses x86 SSE `"x"` register constraint (compile error on ARM64). | Guard with `#if defined(__aarch64__)` using `"+w"(u)` vector/FP register constraint (`"+x"(u)` on `__x86_64__`). |
| `runtime/cpu.h` | `rt_fpcr_sync`, tail `#include` | 42–64, 359–361 | `rt_fpcr_sync` is a no-op `{ (void)c; }` on `__aarch64__` (line 47), and lines 359–361 skip `#include "simd.h"` on `__aarch64__` (leftovers from `simd='asm'`). | Include `simd.h` unconditionally for `--simd=c`, and implement `rt_fpcr_sync` on `__aarch64__` by syncing `c->fpcr` (`FPCR_FZ` bit 24 and rounding mode bits 23:22) into host `fpcr`. |
| `runtime/platform/platform.h` & `fp_x86.h` | `plat_pause`, `fp_ftz_on`, `fp_get`, `fp_set` | `platform.h:10–13, 73–77`; `fp_x86.h:5, 11–12` | `platform.h` includes `fp_x86.h` (`<xmmintrin.h>`, `_mm_getcsr`/`_mm_setcsr` FTZ+DAZ `0x8040`) and emits `pause` on x86-64. | Add `runtime/platform/fp_arm64.h` (`mrs`/`msr fpcr` setting `FZ` bit 24 + `FZ16` bit 19) and emit `__asm__ volatile("yield" ::: "memory")` in `plat_pause()`. |
| `runtime/main.c` | `check_cpu` | 379–384 | Unguarded `__builtin_cpu_init()` and `__builtin_cpu_supports("avx2" / "fma" / "bmi2")` — fails to compile on ARM64. | Guard x86 check with `#if defined(__x86_64__)`. On `#elif defined(__aarch64__)`, verify `AT_HWCAP` (`HWCAP_ASIMD`, `HWCAP_FPHP`, `HWCAP_ASIMDDP`, `HWCAP_ATOMICS`) via `getauxval`. |
| `runtime/vk_rec.c` | `lock_in`, `vk_rec_wait`, `vk_rec_end` | 10, 59, 92, 123 | Unconditional `#include <immintrin.h>` and `_mm_pause()` spin-wait calls. | Drop `<immintrin.h>`; call `plat_pause()` from `platform/platform.h`. |
| `runtime/ov_movie.c` | `to_nv12` | 28, 200–204 | Unconditional `#include <emmintrin.h>` and `_mm_loadu_si128` / `_mm_unpacklo_epi8` / `_mm_unpackhi_epi8` UV interleaving. | Include `simd_platform.h` and add `#if defined(__aarch64__)` fast path using `vst2q_u8` (`uint8x16x2_t`) from `<arm_neon.h>`. |

Already guarded for non-x86:
* `runtime/sha256.c` (`compress_ni`, lines 8–10, 45–85): guarded by `#if defined(__x86_64__) || defined(_M_X64)` with portable scalar fallback (`compress_sw`); optional ARMv8 Crypto (`__ARM_FEATURE_CRYPTO`, `vsha256hq_u32`) can be added later.
* `runtime/sysinfo.c` (`fill`, lines 6–8, 18–29): `<cpuid.h>` guarded by `#if defined(__x86_64__)`; add `#elif defined(__ANDROID__)` reading `__system_property_get("ro.soc.model", ...)` / `/proc/cpuinfo` so the F10 overlay shows `Snapdragon 8 Gen 2`.
* `runtime/third_party/imgui/imgui_internal.h` (lines 62–64): SSE4.2 `<nmmintrin.h>` only enabled on `#ifdef __SSE4_2__`.
* `installer/hostcheck.c`: desktop installer host probe only (not built for Android).

### 1.2 Inline Assembly, Fibers & Signal Contexts
Three low-level x86-64 SysV ABI routines require native AAPCS64 implementations:

1. **Async Preemption Stub (`runtime/kernel.c`, `kern_ap_stub`, lines 500–578):**
   * Currently `#if defined(_WIN32) ... #else` with x86-64 assembly saving `rflags`, `%rax..%rbp`, `mxcsr`, `%ymm0..15` (`0x2d0` frame) and calling `kern_ap_rip` / `kern_ap_yield`.
   * Add `#elif defined(__aarch64__)` saving `x0–x18`, `x29`, `x30`, `nzcv` (`mrs xN, nzcv`), `fpcr`, `fpsr`, and `q0–q31` (512 bytes of NEON vectors) on a 16-byte-aligned stack frame, calling `kern_ap_rip` (`x16 = resume PC`) and `kern_ap_yield`, restoring state, and branching via `br x16`.
2. **Cooperative Guest Fibers (`runtime/platform/linux.c`, `plat_fiber_swap`, `plat_fiber_entry`, `plat_fiber_new`, lines 280–354):**
   * Currently x86-64 SysV assembly saving `rbx`, `rbp`, `r12–r15`, `mxcsr`, `fpucw`.
   * Add `#if defined(__aarch64__)` AAPCS64 context switch saving callee-saved `x19–x30`, `sp`, `d8–d15`, and `fpcr`/`fpsr`.
3. **Signal Handlers (`runtime/platform/linux.c`, `intr_handler` lines 205–210 & `fault_handler` lines 921–935):**
   * Currently indexes `u->uc_mcontext.gregs[REG_RIP / REG_RSP / REG_ERR]` (`PF_WRITE = 2`, `PF_INSTR = 16`).
   * On `#if defined(__aarch64__)`, read/write `u->uc_mcontext.pc` and `u->uc_mcontext.sp`. Walk `u->uc_mcontext.__reserved` records for `struct esr_context` (`ESR_MAGIC = 0x45535201`): extract `esr`, where `is_write = (esr & (1u << 6)) != 0` (`WnR`) and `is_exec = ((esr >> 26) == 0x20 || (esr >> 26) == 0x21)` (Instruction Abort from lower/same EL).

### 1.3 Memory Ordering & Atomics (`x86` TSO -> `ARM64` Weakly Ordered)
* **Guest Exclusive/Atomic Ops (`runtime/cpu.h`, lines 84–200):** Uses `__atomic_*` builtins with `__ATOMIC_SEQ_CST`. Verify `ldxp128` / `stxp128` (`__int128` `__atomic_load_n` / `__atomic_compare_exchange_n`, lines 114–128) compile lock-free to `CASP` (no `libatomic` call) under `-march=armv8.2-a+lse+fp16+dotprod`.
* **Platform Atomics (`runtime/platform/platform.h`, lines 37–51):** `plat_cas32/64`, `plat_xchg32/64`, `plat_xadd32/64`, `plat_inc32`, `plat_dec32` all use `__ATOMIC_SEQ_CST`.
* **Lock-Free Rings & Volatile Flags to Harden:**
  * `runtime/audio_mixer.c` (`ring_pull` lines 90–120, `ring_push`): already uses C11 `memory_order_acquire` / `memory_order_release` on `wr`, `rd`, `stop_epoch`, `flush_until`, `playing`; change `atomic_load(&m->rd)` on line 93 to explicit `memory_order_relaxed` / `acquire` for clarity.
  * `runtime/vk_rec.c` (`VkRec` op-stream handoff, lines 45–135): audit `volatile` state transitions between guest producer and replay consumer threads; upgrade plain reads/writes paired with `_mm_pause()` to `__atomic_load_n(..., __ATOMIC_ACQUIRE)` / `__atomic_store_n(..., __ATOMIC_RELEASE)`.
  * `runtime/kernel.c` (`kern_ap_*` async preemption flag & core park/unpark) and `runtime/tex_astc_mt.c` (`P.next`, `P.gen`): verify acquire/release ordering around shared payload pointers.
* **Timers (`runtime/cpu.h`, `cntpct_el0` & `runtime/platform/linux.c`, `plat_now_ns`):** `plat_now_ns` uses `CLOCK_MONOTONIC` (`clock_gettime`), and guest `MRS CNTPCT_EL0` scales `kern_now_ns()` to the Switch's 19.2 MHz tick rate (`cpu.h`). Optionally read ARM64 `cntvct_el0` + `cntfrq_el0` directly for zero-syscall host profiling.

---

## 2. Phase 3: Vulkan Unified Memory Rewrite & Adreno 740 Compatibility

### 2.1 Hard Vulkan Extensions & Dynamic Loader (`libadrenotools` / Turnip)
* **Hard `VK_EXT_external_memory_host` Requirement (`runtime/vk_device.c`, `k_required_ext`, lines 91–94 & `pick_device`, lines 143–144):**
  * `VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME` is currently in `k_required_ext`, causing `pick_device` to immediately reject stock Qualcomm Vulkan and Adreno drivers that omit host-pointer import.
  * **Fix:** Move `VK_EXT_external_memory_host` to optional extensions on `#ifdef __ANDROID__` (not needed under `memory = unified_vulkan`).
* **Surface Extensions (`runtime/gfx_window_sdl.c`, lines 199–205 & `runtime/vk_device.c`):**
  * Surface creation already goes through `SDL_Vulkan_GetInstanceExtensions` and `SDL_Vulkan_CreateSurface` (`VK_KHR_android_surface` on Android) — zero hardcoded `VK_KHR_win32_surface` / `VK_KHR_xlib_surface` / `VK_KHR_wayland_surface` in the SDL backend (`gfx_window_win.c` is Windows-only).
* **Required Device Features (`runtime/vk_device.c`, `vk_device_init`, lines 325–335):**
  * Checks `shaderInt64`, `geometryShader`, `textureCompressionBC`, and `depthClipControl` (`VK_EXT_depth_clip_control`). Adreno 740 supports all four on Mesa Turnip and Vulkan 1.3 Qualcomm drivers; note `shaderFloat64` (`vk_caps.c`, line 171) is `0` on Adreno 740, which activates Ryujinx's softfloat64 SPIR-V path (`gfx_shader_set_host`, `vk_device.c:414`) — critical for pre-translating the Android shader cache (Section 3.4).
* **Dynamic Vulkan Function Loading for Custom Turnip Drivers (`runtime/vk_device.c`, `runtime/vk_vma.cpp` lines 4–5):**
  * Currently links `-lvulkan` directly and compiles VMA with `#define VMA_STATIC_VULKAN_FUNCTIONS 1` (`vk_vma.cpp:4`).
  * On Android, loading custom Mesa Turnip (`libvulkan_freedreno.so`) via `libadrenotools` (`adrenotools_import_user_vulkan`) or `SDL_Vulkan_LoadLibrary` requires resolving `vkGetInstanceProcAddr` dynamically instead of calling global libvulkan symbols, and switching `vk_vma.cpp` on `#ifdef __ANDROID__` to `VMA_STATIC_VULKAN_FUNCTIONS 0` + `VMA_DYNAMIC_VULKAN_FUNCTIONS 1` (populating `VmaVulkanFunctions.vkGetInstanceProcAddr` / `vkGetDeviceProcAddr`).

### 2.2 Why Current Linux `shadow_on` Fails on 8 GB Android
In `runtime/vk_memory.c`:
1. On non-Win32 (`vk_memory_init`, lines 1357–1359), `shadow_on = 1` allocates a separate VMA `HOST_VISIBLE | HOST_COHERENT` mirror buffer (`shadow_pool`, lines 909–937) for every guest NVN `POOL_IMPORT`, duplicating **~1.3+ GiB** of RAM.
2. Worse, `runtime/platform/linux.c` (`uffd_open` / `plat_mem_written`, lines 807–913) relies on `userfaultfd` + `PAGEMAP_SCAN` (`PM_SCAN_WP_MATCHING`, Linux 6.7+), which is unavailable on Android 5.15/6.1 kernels and blocked by SELinux/seccomp.
3. When write-tracking is unavailable (`mem_written_pages` returns `UINT64_MAX`, `tracked == 0`):
   * `shadow_sync_pool` (`vk_memory.c:987–990`) `memcpy`s the **entire** pool on every command buffer submit.
   * `make_fresh` / `make_fresh_snap` (`runtime/vk_texture.c`, lines 710–713 and 723–727) checks `if (ch) upload(e)` where `ch == -1` (`untracked` is non-zero!), **re-uploading every mirrored texture on every draw/submit**.

### 2.3 `memory = unified_vulkan` Architecture & Dirty Staging Pool
1. **Zero-Copy Unified Guest Backing (`runtime/vk_memory.c`, `runtime/memory.c`, `runtime/main.c`):**
   * Add `XC2_VK_MEMORY=unified_vulkan` (default on `#ifdef __ANDROID__`).
   * Instead of importing `mmap` pointers via `VK_EXT_external_memory_host` or duplicating 1.3 GiB in `shadow_pool`:
     * Either allocate the guest NVN memory pools / unified backing slabs directly via `vkAllocateMemory` (`VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT`, falling back without `HOST_CACHED_BIT` only if required; Adreno 740 unified memory exposes cached coherent device-local memory) + persistent `vkMapMemory`, **or** allocate Android `AHardwareBuffer` / `memfd` (`ashmem`) imported via `VK_ANDROID_external_memory_android_hardware_buffer` / `VK_KHR_external_memory_fd` if fixed VA remap (`MAP_FIXED`) into the guest's `LOW_SIZE` arena (`memory.c:28`) is needed because `mem_init()` (`main.c:615`) runs before `vk_init()`.
     * Even simpler for `POOL_IMPORT` if guest `mem_init` reserves VA first: if `vk_init` runs early (before guest heap commits) or allocates unified `VkBuffer` + `VkDeviceMemory` and remaps/aliases, **or** if `POOL_IMPORT` uses an `AHardwareBuffer`/fd-backed shared mapping, CPU and Adreno GPU read the exact same physical LPDDR5X pages with zero `VK_EXT_external_memory_host` and zero 1.3 GiB copy. (Note: Evaluate exact `mem_init` vs `vk_init` ordering vs `AHardwareBuffer`/`MAP_FIXED` during Phase 3 implementation; if `vkAllocateMemory` returns a host pointer via `vkMapMemory`, guest heap/pool allocators or `guest_base` can point directly to it when VA layout permits.)
2. **Lightweight Ring-Buffered Dirty Staging Pool (128–256 MB Cap):**
   * `runtime/vk_usnap.c` (`vk_usnap_take`, lines 1–180) already snapshots per-submit dynamic uniform buffer ranges into tiny host-visible arenas (`ARENA_MIN = 256 KiB`, `ARENA_KEEP = 16`, ~350–440 KiB/submit).
   * Extend `vk_usnap.c` / `StageArena` (`vk_memory.c:1277–1334`) with a hard **128–256 MB ring cap** to cover per-frame dynamic constant/vertex uploads (`nvnBufferFlushMappedRange` / `vk_guest_written`), preventing >30 FPS mid-frame overwrite flickering without mirroring static geometry or textures.
3. **Android Page Write-Tracking (`runtime/platform/linux.c` & `runtime/vk_texture.c`):**
   * Replace `userfaultfd` (`linux.c:807–913`) on `#ifdef __ANDROID__` with `mprotect(PROT_READ)` + `SIGSEGV` (`esr_context` `WnR`) dirty-page bitmap tracking on watched texture pages (`mem_watch_add` / `plat_mem_written`), and fix `vk_texture.c:710–727` so `nvnTextureWriteTexels` / `nvnMemoryPoolFlushMappedRange` marks ranges dirty without triggering `ch == -1` unconditional re-uploads.

### 2.4 Native ASTC & Desktop Upscaler Guards
* **Native ASTC LDR (`runtime/vk_formats.c` lines 115–123, `runtime/vk_astc.c` lines 293–340, `runtime/vk_texture.c` lines 595–658):**
  * **Already implemented!** When `vk_astc_mode()` (`vk_astc.c:299`) sees `XC2_ASTC=auto` (default) or `native` and `vk_caps.astc_ldr` (`textureCompressionASTC_LDR`, `vk_caps.c:173`) is true, `tex_vk_format` maps NVN formats `0x79..0x94` directly to `VK_FORMAT_ASTC_*_UNORM_BLOCK` / `SRGB_BLOCK`, leaving `astc_bw = 0` so raw 16-byte ASTC blocks upload directly to Adreno 740 with zero RGBA8 expansion.
* **Desktop AMD FSR 3.1 / 4 & Frame Generation Guards:**
  * `runtime/vk_fsr3.c` (lines 1–60: `ffx_api/ffx_api.h`, `ffx_upscale.h`, Win32 `.dll` loader) and `runtime/vk_fg.c` (lines 1–60: `ffx_framegeneration.h`, Win32 `.dll` loader): wrap both files in `#ifndef __ANDROID__` and provide lightweight no-op C stubs for `#ifdef __ANDROID__` (`vk_fsr3_available() -> 0`, `vk_fg_available() -> 0`, etc.).
  * Keep `runtime/vk_fsr.c` (spatial FSR 1.0 EASU + RCAS compute shaders in `fsr_easu_spv.h` / `fsr_rcas_spv.h`) enabled for cheap 720p -> 1080p upscaling on Adreno 740.

---

## 3. Phase 4: Android OS Layer, VA Space, SDL3 Lifecycle & Third-Party Deps

### 3.1 Android 39-Bit Virtual Address Space & Zygote Collision (CRITICAL)
* **Current Layout (`recomp/layout.py` lines 17–19, `runtime/host.h` lines 72–75, `runtime/memory.c` lines 28, 219, 240–248):**
  * `FIRST_BASE = 0x70_0000_0000` (448 GiB) for guest code, `LOW_SIZE = 6 GiB` (`0x00_8000_0000..0x02_0000_0000`), `ALIAS_BASE = 0x10_0000_0000` (64 GiB) + `ALIAS_SIZE = 64 GiB` (`0x10_0000_0000..0x20_0000_0000`), `RT_ADDRESS_LIMIT = 1 TiB`.
  * **Android ARM64 Problem:** Standard Android kernels use 39-bit user VA (`TASK_SIZE = 0x80_0000_0000 = 512 GiB`), and Zygote64 pre-maps ART, Bionic, Scudo heap, and system `.so` libraries across `0x6f_0000_0000..0x7f_ffff_ffff`, directly colliding with `0x70_0000_0000`! Furthermore, `memory.c:219` calls `plat_reexec()` (`execv("/proc/self/exe")`) if `mmap` hits ASLR, which terminates an Android Zygote app process.
  * **Fix (Solely via `recomp/layout.py` + `runtime/memory.c` / `host.h`):**
    * Never call `plat_reexec()` on `#ifdef __ANDROID__`.
    * Shrink `ALIAS_SIZE` on `#ifdef __ANDROID__` from `64 GiB` to `8 GiB` (or `16 GiB`, e.g., `0x10_0000_0000..0x12_0000_0000`, which takes zero physical RAM until committed and sits in empty middle VA space on Android Zygote64).
    * If `0x70_0000_0000` collides with Zygote mappings on Android, add an Android target profile in `recomp/layout.py` placing module bases in clean lower-middle VA (e.g., `0x20_0000_0000`, 128 GiB, well below Zygote's `0x6f_0000_0000..0x7f_ffff_ffff` region and above `ALIAS_BASE`), keeping desktop `FIRST_BASE = 0x70_0000_0000` untouched.

### 3.2 Android Bionic Libc Incompatibilities (`runtime/platform/linux.c`)
| Function | Lines | Linux / glibc Call | Android Bionic (API 29+) Replacement |
| :--- | :--- | :--- | :--- |
| `plat_thread_join_timeout` | 384 | `pthread_timedjoin_np` (absent in Bionic) | Track thread completion with a `PlatEvent` / `pthread_cond_timedwait` (`CLOCK_MONOTONIC`) before `pthread_join`. |
| `uffd_open` / `plat_mem_written` | 807–913 | `userfaultfd` + `PAGEMAP_SCAN` (`Linux 6.7+`) | Guard out on `__ANDROID__`; replace with `mprotect(PROT_READ)` + `SIGSEGV` dirty-page bitmap (Section 2.3). |
| `plat_file_open_direct` | 1005 | `O_DIRECT` (fails on Android FUSE / `sdcardfs`) | Fall back cleanly without `O_DIRECT` when `open(..., O_DIRECT)` returns `EINVAL`. |
| `plat_local_tz_offset` | 1318 | `tzset(); return -timezone;` | Use `localtime_r(&now, &tm)` -> `tm.tm_gmtoff` (supported in Bionic and glibc). |
| `plat_open_folder` / `plat_message` | 1407–1460 | `xdg-open` / `zenity` | No-op / JNI toast or Android Intent on `__ANDROID__`. |
| `plat_proc_spawn` | 1572 | `posix_spawn` for `xc2_xlate` | Guard on `__ANDROID__` (Android SELinux `W^X` forbids executing binaries from data dirs; pre-translated shader cache is used instead). |
| `rt_log` sink (`runtime/main.c`) | 95–155 | Writes `logs/xc2.log` + `stderr` | Also mirror lines to `__android_log_print(ANDROID_LOG_INFO, "XC2Recomp", ...)` (`-llog`) so `adb logcat` captures crashes and boot traces live. |

### 3.3 SDL3 Main Thread & Window Lifecycle Inversion
* **Desktop (`runtime/main.c` lines 570–660 & `runtime/gfx_window_sdl.c` lines 188–224):**
  * Desktop `main()` runs `rt_boot()` on the process main thread, while `gfx_window_sdl.c` (`window_thread`) spawns a **background pthread** that calls `SDL_Init(SDL_INIT_VIDEO)`, `SDL_CreateWindow`, and pumps `SDL_WaitEventTimeout`.
* **Android Requirement (`SDLActivity` + `<SDL3/SDL_main.h>`):**
  * On Android, `ANativeWindow` surface lifecycle (`SDL_EVENT_WILL_ENTER_BACKGROUND`, `SDL_EVENT_DID_ENTER_FOREGROUND`, `SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED`) and `SDL_CreateWindow` **must** execute on the SDL main thread (`SDL_main` / `SDL_AppInit` / `SDL_AppEvent` / `SDL_AppIterate` / `SDL_AppQuit`).
  * **Fix:** On `#ifdef __ANDROID__`, keep SDL window creation and event pumping on the `SDLActivity` main thread, and spawn the guest `rt_boot()` thread from `SDL_AppInit` once the storage root and initial `SDL_Window` are ready.
* **Gamepad & Touch Gestures (`runtime/input.c` lines 1–160, `runtime/input_sdl.c`, `runtime/ui/menu_sdl.cpp`):**
  * `runtime/input.c` already uses SDL3's `SDL_Gamepad` API (`SDL_GetGamepads`, `SDL_OpenGamepad`, `PAD_LAYOUT_AUTO` Nintendo/Xbox detection), which maps Retroid Pocket 6 / Odin 2 built-in controllers out of the box.
  * Add 3-finger touch tap detection (`SDL_EVENT_FINGER_DOWN` counting 3 simultaneous fingers -> `menu_toggle()` for `F1` menu, 4-finger or top-right corner tap -> `F10` perf overlay toggle) and forward single-finger touch (`x * width`, `y * height`) to `menu_event_mouse_pos` / `menu_event_mouse_button` when `menu_is_open()` is true so ImGui menus can be operated by touch as well as gamepad.

### 3.4 Shader Pre-Translation (`xc2_xlate` & `shaderFloat64=0`)
* `runtime/gfx_shader.c` (lines 198–245) spawns `xc2_xlate --serve`, built by `tools/build_xlate.py` as a `.NET 10 NativeAOT` host executable.
* On Android, spawning a child executable from `/sdcard` is blocked by SELinux, and compiling 5,000+ shaders on-device wastes RAM and battery.
* **Critical Cache Key Detail:** `gfx_shader_set_host` (`vk_device.c:414` -> `gfx_shader.c`) folds host GPU capabilities (`float64`, `sbuf_align`, `subgroup`, `gather_bits`) into the shader translation request! Because Adreno 740 has `shaderFloat64 = 0` (unlike desktop AMD/NVIDIA where `shaderFloat64 = 1`), `--target android-arm64` in `tools/prepare_shaders.py` / host packaging **must** translate `seed-programs.bin` + `seed-answers.bin` with Adreno 740's host profile (`float64=0`) into `xc2-data/shader-cache/` so runtime hits 100% pre-built `.spv` + `.res.json` files without invoking `xc2_xlate`. (As a safety net for unseeded shader variants met late in play, `tools/xlate` can optionally also be cross-compiled via `dotnet publish -r linux-bionic-arm64` into a JNI-loaded `libxc2_xlate.so` packaged in the APK's executable `jniLibs/arm64-v8a/`.)

### 3.5 Executable-Relative Paths to Redirect on Android
Every runtime subsystem currently locates files beside `plat_exe_path()` (which on Android resolves to `/system/bin/app_process64`!):
1. `runtime/main.c` (`log_open` line 106, `main` lines 580–594): `logs/`, `exefs/`, `romfs/`, `profile/`.
2. `runtime/cfg.c` (`cfg_init` line 651): `xc2.ini`.
3. `runtime/romfs.c` (`index_path` lines 543–548): `romfs-index.bin` and `.romfs-verified-v1`.
4. `runtime/vk_device.c` (`mesa_cache_env` line 50): `mesa-cache/`.
5. `runtime/vk_pipeline.c` (`cache_path_init` lines 157–165): `pipeline-cache.bin` and `shader-prepared.bin`.
6. `runtime/gfx_shader.c` (`cache_init` lines 198–218): `shader-cache/`, `seed-programs.bin`, `seed-answers.bin`.
7. `runtime/mods.c` (lines 201–230): `mods/`.

**Fix:** Add `plat_data_dir(char *out, size_t cap)` in `runtime/platform/platform.h`. On desktop it returns the directory of `plat_exe_path()` (zero behavior change); on `#ifdef __ANDROID__` it returns the JNI/env-configured data root (`/sdcard/Android/data/com.xc2recomp.app/files` or `XC2_DATA_DIR`).

### 3.6 Third-Party Dependencies & Android NDK r27+ Compatibility
From `THIRD_PARTY_NOTICES.md`, `tools/build_game.py` (lines 103–116), and `tools/make_release_linux.py` (lines 49–97):

| Dependency | Version / Location | License | Android NDK (`arm64-v8a`) Status |
| :--- | :--- | :--- | :--- |
| **SDL3** | SDL 3.x (external link `-lSDL3`) | zlib | Full official Android `SDLActivity` + AAudio + Gamepad + Vulkan surface support; build from source via CMake/NDK into `libSDL3.so`. |
| **Dear ImGui** | 1.92.7 (`runtime/third_party/imgui/`) | MIT | Vendored C++; compiles cleanly on NDK Clang (`c++_static`, `#embed` supported in Clang 19+ / NDK r27+). |
| **Vulkan Memory Allocator** | 3.4.0 (`build/deps/include/vk_mem_alloc.h`, fetched by `build_game.py:VMA_FETCH`) | MIT | Header-only C++; switch `vk_vma.cpp` to `VMA_DYNAMIC_VULKAN_FUNCTIONS 1` on Android. |
| **Vulkan-Headers** | 1.4.363 (`make_release_linux.py:72`) | Apache-2.0 | Header-only; compatible with NDK r27+ Vulkan 1.3 loader. |
| **zstd** | 1.5.7 (`make_release_linux.py:70`) | BSD-3-Clause | Pure C + optional ARM64 NEON; compiles cleanly as static `libzstd.a` under NDK. |
| **libopus** | 1.4/1.5 (`-lopus` in `ov_media.c` / `ov_movie.c`) | BSD-3-Clause | Pure C + ARM64 NEON fixed/float optimizations; compiles cleanly as static `libopus.a` under NDK. |
| **libvpx** | 1.13+ (`-lvpx` VP9/VP8 decode in `ov_movie.c`) | BSD-3-Clause | Supports `--target=arm64-android-gcc` with ARM64 NEON intrinsics; static `libvpx.a`. |
| **AMD FidelityFX SDK** | 2.1.0 / FSR 3.1.5 & 4 (`runtime/ffx_api/`, `vk_fsr3.c`, `vk_fg.c`) | MIT | **Desktop Windows x64 `.dll` only.** Guard out with `#ifndef __ANDROID__`; keep `vk_fsr.c` (FSR 1.0 SPIR-V compute). |

Build flag updates needed in `tools/build_game.py` (lines 103–116) for `--target android-arm64`:
* Replace `-march=x86-64-v3` and `-no-pie` with `--target=aarch64-linux-android29 -march=armv8.2-a+lse+fp16+dotprod -fPIC -shared`.
* Replace `-l:libstdc++.a -lpthread` with NDK `-static-libstdc++` (`libc++_static.a`) + `-landroid -llog -lm -ldl`.

---

## 4. Phase 5: 8 GB RAM Survival Budget & Snapdragon 8 Gen 2 Tuning

### 4.1 Immediate Memory Wins Identified in `runtime/`
To stay comfortably below Android `lmkd`'s ~5.5–6.0 GB process RSS kill threshold on an 8 GB device:
1. **Eliminate `shadow_pool` (`runtime/vk_memory.c:909–937, 1357–1359`):** Saves **~1,300+ MiB** via `memory = unified_vulkan`.
2. **Disable `vk_headroom` Anchor (`runtime/vk_headroom.c`, `vk_headroom_init`, lines 45–80):** Desktop allocates up to `128 MiB` per device memory type just to hold budget headroom (`XC2_VRAM_HEADROOM`). Default `XC2_VRAM_HEADROOM=0` on `#ifdef __ANDROID__` -> saves **128–256 MiB** immediately.
3. **Shrink Persistent Upload Staging Pool (`runtime/vk_memory.c`, `STG_BLOCK` / `STG_MIN_BLOCKS`, lines 2274–2275):** Currently pre-allocates `2 * 64 MiB = 128 MiB` (`STG_MIN_BLOCKS = 2`, `STG_BLOCK = 64 MiB`) and never frees below 128 MiB. On `#ifdef __ANDROID__`, lower `STG_BLOCK` to `16 MiB` (`STG_MIN_BLOCKS = 1`) and release idle blocks via `vmaSetCurrentFrameIndex` + `vmaFreeMemoryPages` -> saves **96–112 MiB**.
4. **Enforce Positional `pread` for 17 GB RomFS (`runtime/romfs.c`, lines 607–616):** Keep `XC2_ROMFS_MMAP=0` (default) on Android so 17 GB `romfs.bin` page-cache mappings never bloat `VmRSS` or compete with anonymous guest/GPU pages under `lmkd`.
5. **Native Hardware ASTC (`XC2_ASTC=native` / `auto`):** Zero CPU RGBA8 decompression buffers (`runtime/tex_astc_mt.c` worker pool stays idle and unallocated) and 4x–8x smaller GPU texture footprint.
6. **720p Ceiling + FSR 1 (`runtime/resolution.c` & `xc2.ini` Android profile):** Cap internal scene render to `1280x720` (`0.75x`–`1.0x` scale) with `vk_fsr.c` EASU+RCAS upscaling to 1080p, saving hundreds of MiB in MSAA/HDR/GBuffer render targets (`vk_texture.c`).

### 4.2 Telemetry & `1+4+3` Core Affinity Sites
* **RAM & VMA Telemetry (`runtime/perfmon.c` & `runtime/ui/menu.cpp` lines 1165–1249):**
  * `perf_full_draw` (`menu.cpp:1201–1212`) already displays `VRAM` (`vram_used_mb` / `vram_total_mb`) and `RAM` (`ram_used_mb` / `ram_total_mb` / `ram_game_mb`).
  * Update `runtime/perfmon.c` on `#ifdef __ANDROID__` to parse `/proc/self/status` (`VmRSS`, `VmSwap` — critical for ZRAM swap pressure detection before `lmkd` strikes!), `/proc/meminfo` (`MemAvailable`), and `vmaGetHeapBudgets`, plus Adreno GPU busy % / clock from `/sys/class/kgsl/kgsl-3d0/gpu_busy_percentage` & `gpuclk` when readable.
* **Snapdragon 8 Gen 2 (`1+4+3`) Thread Pinning (`sched_setaffinity`):**
  * **Gold / Prime Cores (`4..7`):**
    * Guest virtual cores 0–2 (`runtime/kernel.c`, guest core worker pthreads).
    * Vulkan replay / submission thread (`runtime/vk_rec.c`, `replay_thread`).
  * **Silver Efficiency Cores (`0..3`) or Gold-spillover:**
    * Audio renderer 5 ms mixer thread (`runtime/audren.c`, `render_thread`, line 473) and SDL audio output thread.
    * Background pipeline compile workers (`runtime/vk_pipeline.c`, background `Job` threads) and save commit / movie decode threads (`runtime/fs_host.c`, `runtime/ov_movie.c`).
  * Note: Probe per-core `cpuinfo_max_freq` under `/sys/devices/system/cpu/cpu*/cpufreq/cpuinfo_max_freq` at startup rather than blindly hardcoding CPU indices, falling back to `0..3` / `4..7` if sysfs is unreadable, so Snapdragon 8 Gen 2 (`0..2` Silver, `3..6` Gold, `7` Prime on SM8550!) maps accurately to actual little vs big/prime clusters!
