// Dynamic Vulkan loader for Android, with optional custom Mesa Turnip driver loading via libadrenotools or dlopen.
#ifdef __ANDROID__
#define VK_USE_PLATFORM_ANDROID_KHR
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include "vk_loader.h"

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <SDL3/SDL.h>

#include "cfg.h"
#include "platform/platform.h"

void rt_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr;
#define VK_LOADER_DEF(fn) PFN_##fn fn;
VK_LOADER_GLOBAL_FUNCS(VK_LOADER_DEF)
VK_LOADER_INSTANCE_FUNCS(VK_LOADER_DEF)
VK_LOADER_DEVICE_FUNCS(VK_LOADER_DEF)
#undef VK_LOADER_DEF

enum {
    ADRENOTOOLS_DRIVER_CUSTOM = 1 << 0,
    ADRENOTOOLS_DRIVER_FILE_REDIRECT = 1 << 1,
    ADRENOTOOLS_DRIVER_GPU_MAPPING_IMPORT = 1 << 2,
};
typedef void *(*pfn_adrenotools_open_libvulkan_t)(int dlopen_mode, int feature_flags, const char *tmp_lib_dir,
                                                  const char *hook_lib_dir, const char *custom_driver_dir,
                                                  const char *custom_driver_name, const char *file_redirect_dir,
                                                  void **user_mapping_handle);

static void *g_vk_lib;
static void *g_adreno_lib;
static int g_custom_driver;
static PlatMutex g_loader_lock = PLAT_MUTEX_INIT;

static void ensure_trailing_slash(char *buf, size_t cap) {
    size_t n = strlen(buf);
    if (n > 0 && buf[n - 1] != '/' && n + 1 < cap) {
        buf[n] = '/';
        buf[n + 1] = 0;
    }
}

static void find_jni_dir(char *out, size_t cap) {
    out[0] = 0;
    Dl_info dli;
    if (dladdr((const void *)vk_loader_open, &dli) && dli.dli_fname && dli.dli_fname[0] == '/') {
        const char *slash = strrchr(dli.dli_fname, '/');
        if (slash) {
            size_t len = (size_t)(slash - dli.dli_fname + 1);
            if (len < cap) {
                memcpy(out, dli.dli_fname, len);
                out[len] = 0;
            }
        }
    }
}

static int copy_driver_to_internal(const char *src_path, const char *base_name, char *out_dir, size_t dir_cap) {
    const char *aint = SDL_GetAndroidInternalStoragePath();
    if (!aint || !*aint) return 0;
    char dst_dir[PLAT_PATH_MAX * 2], dst_path[PLAT_PATH_MAX * 2 + 128];
    snprintf(dst_dir, sizeof dst_dir, "%s/vk-driver", aint);
    plat_file_mkdir_p(dst_dir);
    snprintf(dst_path, sizeof dst_path, "%s/%s", dst_dir, base_name);
    FILE *in = fopen(src_path, "rb");
    if (!in) return 0;
    FILE *out = fopen(dst_path, "wb");
    if (!out) {
        fclose(in);
        return 0;
    }
    char buf[65536];
    size_t n;
    int ok = 1;
    while ((n = fread(buf, 1, sizeof buf, in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) {
            ok = 0;
            break;
        }
    }
    fclose(in);
    if (fclose(out) != 0) ok = 0;
    if (!ok) return 0;
    chmod(dst_path, 0555);
    snprintf(out_dir, dir_cap, "%s/", dst_dir);
    return 1;
}

static void *open_custom_driver(const char *driver_spec) {
    char jni_dir[PLAT_PATH_MAX * 2];
    find_jni_dir(jni_dir, sizeof jni_dir);

    char custom_dir[PLAT_PATH_MAX * 2] = "";
    char custom_name[256] = "";
    char resolved_path[PLAT_PATH_MAX * 2 + 256] = "";

    if (!strcmp(driver_spec, "turnip") || !strcmp(driver_spec, "bundled") ||
        !strcmp(driver_spec, "default_turnip") || !strcmp(driver_spec, "libvulkan_turnip.so")) {
        snprintf(custom_dir, sizeof custom_dir, "%s", jni_dir);
        snprintf(custom_name, sizeof custom_name, "libvulkan_turnip.so");
    } else if (!strchr(driver_spec, '/')) {
        snprintf(custom_dir, sizeof custom_dir, "%s", jni_dir);
        snprintf(custom_name, sizeof custom_name, "%s", driver_spec);
    } else {
        const char *slash = strrchr(driver_spec, '/');
        size_t dlen = (size_t)(slash - driver_spec + 1);
        if (dlen >= sizeof custom_dir) dlen = sizeof custom_dir - 1;
        memcpy(custom_dir, driver_spec, dlen);
        custom_dir[dlen] = 0;
        snprintf(custom_name, sizeof custom_name, "%s", slash + 1);
        // External /sdcard storage is mounted noexec; stage a read-only copy inside app internal storage when needed.
        if (strncmp(driver_spec, "/data/", 6) != 0 && strncmp(driver_spec, "/system/", 8) != 0 &&
            strncmp(driver_spec, "/vendor/", 8) != 0) {
            copy_driver_to_internal(driver_spec, custom_name, custom_dir, sizeof custom_dir);
        }
    }
    ensure_trailing_slash(custom_dir, sizeof custom_dir);
    snprintf(resolved_path, sizeof resolved_path, "%s%s", custom_dir, custom_name);

    if (!g_adreno_lib && jni_dir[0]) {
        char adreno_path[PLAT_PATH_MAX * 2 + 64];
        snprintf(adreno_path, sizeof adreno_path, "%slibadrenotools.so", jni_dir);
        g_adreno_lib = dlopen(adreno_path, RTLD_NOW | RTLD_LOCAL);
    }
    if (!g_adreno_lib) g_adreno_lib = dlopen("libadrenotools.so", RTLD_NOW | RTLD_LOCAL);

    if (g_adreno_lib) {
        pfn_adrenotools_open_libvulkan_t open_fn =
            (pfn_adrenotools_open_libvulkan_t)dlsym(g_adreno_lib, "adrenotools_open_libvulkan");
        if (open_fn) {
            char hook_dir[PLAT_PATH_MAX * 2];
            const char *cfg_hook = cfg_get("XC2_VK_HOOK_DIR");
            snprintf(hook_dir, sizeof hook_dir, "%s", (cfg_hook && *cfg_hook) ? cfg_hook : jni_dir);
            ensure_trailing_slash(hook_dir, sizeof hook_dir);

            const char *cfg_tmp = cfg_get("XC2_VK_TMP_DIR");
            char tmp_dir[PLAT_PATH_MAX * 2] = "";
            const char *tmp_arg = NULL;
            if (cfg_tmp && *cfg_tmp) {
                snprintf(tmp_dir, sizeof tmp_dir, "%s", cfg_tmp);
                ensure_trailing_slash(tmp_dir, sizeof tmp_dir);
                tmp_arg = tmp_dir;
            }

            void *h = open_fn(RTLD_NOW, ADRENOTOOLS_DRIVER_CUSTOM, tmp_arg, hook_dir, custom_dir, custom_name, NULL, NULL);
            if (h) {
                rt_log("vk: loaded custom driver %s via libadrenotools (hook_dir=%s)", resolved_path, hook_dir);
                g_custom_driver = 1;
                return h;
            }
            rt_log("vk: libadrenotools could not open %s (%s); trying dlopen", resolved_path, dlerror());
        }
    }
    void *h = dlopen(resolved_path, RTLD_NOW | RTLD_LOCAL);
    if (h) {
        rt_log("vk: loaded custom driver %s via dlopen", resolved_path);
        g_custom_driver = 1;
    } else {
        rt_log("vk: failed to load custom driver %s (%s); falling back to system Vulkan", resolved_path, dlerror());
    }
    return h;
}

int vk_loader_open(PFN_vkGetInstanceProcAddr sdl_proc) {
    plat_mutex_lock(&g_loader_lock);
    if (!vkGetInstanceProcAddr) {
        const char *drv = cfg_get("XC2_VK_DRIVER");
        if (drv && *drv) {
            g_vk_lib = open_custom_driver(drv);
            if (g_vk_lib)
                vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)dlsym(g_vk_lib, "vkGetInstanceProcAddr");
        }
        if (!vkGetInstanceProcAddr && sdl_proc) {
            vkGetInstanceProcAddr = sdl_proc;
            g_custom_driver = 0;
        }
        if (!vkGetInstanceProcAddr) {
            g_vk_lib = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
            if (!g_vk_lib) g_vk_lib = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
            if (g_vk_lib)
                vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)dlsym(g_vk_lib, "vkGetInstanceProcAddr");
            g_custom_driver = 0;
        }
        if (vkGetInstanceProcAddr) {
#define LOAD_GLOBAL(fn) fn = (PFN_##fn)vkGetInstanceProcAddr(NULL, #fn);
            VK_LOADER_GLOBAL_FUNCS(LOAD_GLOBAL)
#undef LOAD_GLOBAL
        }
    }
    int ok = vkGetInstanceProcAddr != NULL && vkCreateInstance != NULL;
    plat_mutex_unlock(&g_loader_lock);
    return ok;
}

void vk_loader_load_instance(VkInstance instance) {
    if (!vkGetInstanceProcAddr || !instance) return;
#define LOAD_INST(fn) fn = (PFN_##fn)vkGetInstanceProcAddr(instance, #fn);
    VK_LOADER_INSTANCE_FUNCS(LOAD_INST)
#undef LOAD_INST
}

void vk_loader_load_device(VkInstance instance, VkDevice device) {
    if (!device) return;
#define LOAD_DEV(fn) \
    fn = vkGetDeviceProcAddr ? (PFN_##fn)vkGetDeviceProcAddr(device, #fn) : NULL; \
    if (!fn && vkGetInstanceProcAddr && instance) fn = (PFN_##fn)vkGetInstanceProcAddr(instance, #fn);
    VK_LOADER_DEVICE_FUNCS(LOAD_DEV)
#undef LOAD_DEV
}

int vk_loader_is_custom(void) { return g_custom_driver; }

int vk_loader_create_android_surface(VkInstance instance, void *anative_window, VkSurfaceKHR *out) {
    *out = VK_NULL_HANDLE;
    if (!instance || !anative_window || !vkGetInstanceProcAddr) return 0;
    PFN_vkCreateAndroidSurfaceKHR create_fn =
        (PFN_vkCreateAndroidSurfaceKHR)vkGetInstanceProcAddr(instance, "vkCreateAndroidSurfaceKHR");
    if (!create_fn) return 0;
    VkAndroidSurfaceCreateInfoKHR ci = {
        .sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR,
        .window = (struct ANativeWindow *)anative_window,
    };
    return create_fn(instance, &ci, NULL, out) == VK_SUCCESS;
}

#else
// Non-Android platforms link directly against the Vulkan loader.
typedef int vk_loader_unused_translation_unit;
#endif
