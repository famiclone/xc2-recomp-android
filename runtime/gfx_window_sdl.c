// The SDL3 back end of the window (gfx_wsys.h), for hosts without Win32: the window on a thread of its own that also
// runs SDL's event loop, F11 fullscreen (borderless, the desktop's mode), the settings XC2_WINDOW_SIZE and
// XC2_FULLSCREEN, the display's refresh rate, focus, the Vulkan surface. The swapchain and the present are
// gfx_present.c's. Keyboard and mouse events go to input_sdl.c and to the menu (ui/menu_sdl.cpp).
//
// SDL wants its video calls on the thread that initialised video, so this thread does them all: the others hand it
// work through run_on_window (a user event and a wait) or post a user event. Video is initialised before the window
// exists: the Vulkan instance asks for SDL's surface extensions first (gfx_window_vk_extensions), the window comes with
// gfx_window_start (wsys_open).
//
// XC2_WINDOW_SIZE is in pixels. On a display that scales windows (Wayland, HiDPI) the window asks for its pixels in
// the display's units (SDL_GetWindowDisplayScale), and SDL reports mouse positions in those units: input_sdl.c and the
// menu scale them back (gfx_window_sdl_density).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vk.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include "cfg.h"
#include "gfx.h"
#include "gfx_wsys.h"
#include "input.h"
#include "input_sdl.h"
#include "ui/menu.h"
#include "platform/platform.h"

static SDL_Window *g_win;
static uint32_t g_ev_base;               // SDL_RegisterEvents: the user events below
enum { EV_RUN, EV_SETTINGS, EV_CLOSE, EV_COUNT };
static PlatEvent g_video_ready;          // manual-reset: SDL video is up (or failed)
static int g_video_ok;
static PlatOnce g_thread_once = PLAT_ONCE_INIT;
static uint32_t g_window_tid;            // the window thread, which runs the video calls
static volatile int32_t g_density_milli = 1000;   // pixels per window unit, x1000
#ifdef __ANDROID__
static int g_android_cb_mode;            // 1 when SDL_AppInit/SDL_AppEvent drive video on SDL's callback thread
#endif

// ---- work on the window thread
typedef struct {
    void (*fn)(void *arg);
    void *arg;
    PlatEvent done;
} RunReq;

static void run_on_window(void (*fn)(void *arg), void *arg) {
    if (plat_thread_id() == g_window_tid) {
        fn(arg);
        return;
    }
    RunReq r = {fn, arg};
    plat_event_init(&r.done, 1, 0);
    SDL_Event e = {.user = {.type = g_ev_base + EV_RUN, .data1 = &r}};
    if (!SDL_PushEvent(&e)) rt_fatal("gfx: cannot reach the window thread (%s)", SDL_GetError());
    plat_event_wait(&r.done);
}

static void post(int ev) {
    SDL_Event e = {.user = {.type = g_ev_base + (uint32_t)ev}};
    SDL_PushEvent(&e);
}

double gfx_window_sdl_density(void) { return __atomic_load_n(&g_density_milli, __ATOMIC_RELAXED) / 1000.0; }

SDL_Window *gfx_window_sdl(void) { return g_win; }

// ---- settings: size and fullscreen
static char g_applied_size[32];
static int g_applied_fs;

static void note_applied(void) {
    snprintf(g_applied_size, sizeof g_applied_size, "%s", cfg_value("XC2_WINDOW_SIZE"));
    g_applied_fs = gfx_cfg_fullscreen();
}

static int is_fullscreen(void) { return (SDL_GetWindowFlags(g_win) & SDL_WINDOW_FULLSCREEN) != 0; }

// The window's size in its units for w x h pixels.
static void set_pixel_size(int w, int h) {
    float scale = SDL_GetWindowDisplayScale(g_win);
    if (!(scale > 0)) scale = 1;
    SDL_SetWindowSize(g_win, (int)(w / scale + 0.5f), (int)(h / scale + 0.5f));
}

static void set_fullscreen(int on) {
    input_cursor_suspend(1);
    SDL_SetWindowFullscreen(g_win, on);   // the desktop's mode: borderless over the display
    SDL_SyncWindow(g_win);
    input_cursor_suspend(0);
}

// Window thread: the window takes the size and fullscreen state of the settings when they changed. A window resized by
// hand keeps its size when another setting changes.
static void apply_settings(void) {
    int size_changed = strcmp(cfg_value("XC2_WINDOW_SIZE"), g_applied_size) != 0, fs = gfx_cfg_fullscreen();
    int fs_changed = fs != g_applied_fs;
    note_applied();
    if (size_changed) {
        int w, h;
        gfx_cfg_window_size(&w, &h);
        if (is_fullscreen()) SDL_SetWindowFullscreen(g_win, false);
        set_pixel_size(w, h);
        if (is_fullscreen() || (fs && !fs_changed)) SDL_SetWindowFullscreen(g_win, true);
    }
    if (fs_changed && fs != is_fullscreen()) set_fullscreen(fs);
}

void gfx_window_apply_settings(void) {
    if (g_win) post(EV_SETTINGS);
}

void gfx_window_request_close(void) {
    if (g_win) post(EV_CLOSE);
}

// ---- the display's refresh rate and size
static void refresh_rate_update(void) {
    const SDL_DisplayMode *m = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(g_win));
    double hz = 0;
    if (m && m->refresh_rate_denominator) hz = (double)m->refresh_rate_numerator / m->refresh_rate_denominator;
    else if (m) hz = m->refresh_rate;
    gfx_window_set_refresh(hz);
}

int gfx_window_monitor_size(int *w, int *h) {
    if (!g_win) return 0;
    const SDL_DisplayMode *m = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(g_win));
    if (!m) return 0;
    *w = (int)(m->w * m->pixel_density + 0.5f);
    *h = (int)(m->h * m->pixel_density + 0.5f);
    return 1;
}

static void density_update(void) {
    float d = SDL_GetWindowPixelDensity(g_win);
    __atomic_store_n(&g_density_milli, (int32_t)((d > 0 ? d : 1) * 1000 + 0.5f), __ATOMIC_RELAXED);
}

// ---- the window thread
static void handle(const SDL_Event *e) {
    if (e->type == g_ev_base + EV_RUN) {
        RunReq *r = e->user.data1;
        r->fn(r->arg);
        plat_event_set(&r->done);
        return;
    }
    if (e->type == g_ev_base + EV_SETTINGS) {
        if (g_win) apply_settings();
        return;
    }
    if (e->type == g_ev_base + EV_CLOSE || e->type == SDL_EVENT_QUIT) {
        gfx_window_closed();
        if (!g_win) return;
    }
    if (!g_win) return;
    input_sdl_event(e);
    menu_sdl_event(e);
    switch (e->type) {
    case SDL_EVENT_KEY_DOWN:
        if (e->key.scancode == SDL_SCANCODE_F11 && !e->key.repeat) {
            set_fullscreen(!is_fullscreen());
            cfg_set("XC2_FULLSCREEN", is_fullscreen() ? "1" : "0");   // the menu shows it; the next start keeps it
            cfg_save();
            note_applied();
        }
        break;
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
    case SDL_EVENT_WINDOW_RESIZED:
    case SDL_EVENT_WINDOW_RESTORED:
        density_update();
        gfx_window_mark_dirty();
        break;
    case SDL_EVENT_WINDOW_DISPLAY_CHANGED:
    case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
    case SDL_EVENT_DISPLAY_CURRENT_MODE_CHANGED:
        density_update();
        refresh_rate_update();
        gfx_window_mark_dirty();
        break;
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        gfx_window_focus_changed(e->type == SDL_EVENT_WINDOW_FOCUS_GAINED);
        break;
#ifdef __ANDROID__
    case SDL_EVENT_WILL_ENTER_BACKGROUND:
    case SDL_EVENT_DID_ENTER_BACKGROUND:
        gfx_window_focus_changed(0);
        break;
    case SDL_EVENT_DID_ENTER_FOREGROUND:
        density_update();
        gfx_window_mark_dirty();
        gfx_window_focus_changed(1);
        break;
#endif
    case SDL_EVENT_QUIT:
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
        gfx_window_closed();
        break;
    }
}

static void init_video_on_thread(void) {
    g_window_tid = plat_thread_id();
    SDL_SetHint(SDL_HINT_APP_ID, "xc2-recomp");   // Wayland's app id: the desktop file and the taskbar match it
    SDL_SetHint(SDL_HINT_VIDEO_ALLOW_SCREENSAVER, "0");
#ifdef __ANDROID__
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
    SDL_SetHint(SDL_HINT_TOUCH_MOUSE_EVENTS, "0");
#endif
    g_video_ok = SDL_Init(SDL_INIT_VIDEO);
    if (!g_video_ok) rt_log("gfx: SDL video init failed (%s)", SDL_GetError());
    // SDL's Vulkan queries need its Vulkan loader, which a window would load at creation: the instance and the device
    // are made before the window.
#ifdef __ANDROID__
    else {
        const char *drv = cfg_get("XC2_VK_DRIVER");
        if (drv && *drv) vk_loader_open(NULL);
        if (!vk_loader_is_custom()) {
            if (!SDL_Vulkan_LoadLibrary(NULL)) {
                rt_log("gfx: SDL cannot load Vulkan (%s)", SDL_GetError());
                g_video_ok = 0;
            } else {
                vk_loader_open((PFN_vkGetInstanceProcAddr)SDL_Vulkan_GetVkGetInstanceProcAddr());
            }
        }
        if (g_video_ok) {
            g_ev_base = SDL_RegisterEvents(EV_COUNT);
            rt_log("gfx: SDL %d.%d.%d, video driver %s", SDL_VERSIONNUM_MAJOR(SDL_GetVersion()),
                   SDL_VERSIONNUM_MINOR(SDL_GetVersion()), SDL_VERSIONNUM_MICRO(SDL_GetVersion()), SDL_GetCurrentVideoDriver());
        }
    }
#else
    else if (!SDL_Vulkan_LoadLibrary(NULL)) {
        rt_log("gfx: SDL cannot load Vulkan (%s)", SDL_GetError());
        g_video_ok = 0;
    } else {
        g_ev_base = SDL_RegisterEvents(EV_COUNT);
        rt_log("gfx: SDL %d.%d.%d, video driver %s", SDL_VERSIONNUM_MAJOR(SDL_GetVersion()),
               SDL_VERSIONNUM_MINOR(SDL_GetVersion()), SDL_VERSIONNUM_MICRO(SDL_GetVersion()), SDL_GetCurrentVideoDriver());
    }
#endif
}

static void window_main(void *arg) {
    (void)arg;
    init_video_on_thread();
    plat_event_set(&g_video_ready);
    if (!g_video_ok) return;
    for (;;) {
        SDL_Event e;
        if (SDL_WaitEventTimeout(&e, 100)) handle(&e);
        else input_sdl_idle();   // a cursor change requested from another thread meanwhile
    }
}

static void video_start(void *arg) {
    (void)arg;
#ifdef __ANDROID__
    if (g_android_cb_mode) return;
#endif
    plat_event_init(&g_video_ready, 1, 0);
    if (!plat_thread_spawn(window_main, NULL, "window", 0)) rt_fatal("gfx: cannot start the window thread");
    plat_event_wait(&g_video_ready);
}

#ifdef __ANDROID__
int gfx_window_sdl_android_init(void) {
    g_android_cb_mode = 1;
    init_video_on_thread();
    return g_video_ok;
}

void gfx_window_sdl_android_event(const SDL_Event *e) {
    if (g_video_ok && e) handle(e);
}

void gfx_window_sdl_android_iterate(void) {
    if (g_video_ok) input_sdl_idle();
}
#endif

static int video_up(void) {
    plat_once(&g_thread_once, video_start, NULL);
    return g_video_ok;
}

int gfx_window_vk_extensions(const char **out, int max) {
    if (!video_up()) return 0;
#ifdef __ANDROID__
    if (vk_loader_is_custom()) {
        int k = 0;
        if (k < max) out[k++] = VK_KHR_SURFACE_EXTENSION_NAME;
        if (k < max) out[k++] = "VK_KHR_android_surface";
        return k;
    }
#endif
    Uint32 n = 0;
    const char *const *ext = SDL_Vulkan_GetInstanceExtensions(&n);
    if (!ext) {
        rt_log("gfx: SDL_Vulkan_GetInstanceExtensions failed (%s)", SDL_GetError());
        return 0;
    }
    int k = 0;
    for (Uint32 i = 0; i < n && k < max; i++) out[k++] = ext[i];
    return k;
}

int gfx_window_vk_presentable(VkInstance inst, VkPhysicalDevice pd, uint32_t family) {
#ifdef __ANDROID__
    if (vk_loader_is_custom()) return video_up() != 0;
#endif
    return video_up() && SDL_Vulkan_GetPresentationSupport(inst, pd, family);
}

// ---- gfx_wsys.h
static int open_ok;

static void open_window(void *arg) {
    (void)arg;
    int w, h;
    gfx_cfg_window_size(&w, &h);
#ifdef __ANDROID__
    SDL_WindowFlags flags =
        (vk_loader_is_custom() ? 0 : SDL_WINDOW_VULKAN) | SDL_WINDOW_FULLSCREEN | SDL_WINDOW_HIGH_PIXEL_DENSITY;
#else
    SDL_WindowFlags flags = SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
#endif
    g_win = SDL_CreateWindow("XC2 Recomp", w, h, flags);
    if (!g_win) {
        rt_log("gfx: SDL_CreateWindow failed (%s)", SDL_GetError());
        return;
    }
#ifndef __ANDROID__
    set_pixel_size(w, h);   // w x h were window units; the setting is in pixels
#endif
    density_update();
    note_applied();
    refresh_rate_update();
    if (cfg_get("XC2_BACKGROUND_WINDOW")) rt_log("gfx: XC2_BACKGROUND_WINDOW has no effect with SDL windows");
#ifndef __ANDROID__
    if (gfx_cfg_fullscreen()) SDL_SetWindowFullscreen(g_win, true);
#endif
    SDL_SyncWindow(g_win);
    input_sdl_window(g_win);
    open_ok = 1;
}

int wsys_open(void) {
    if (!video_up()) return 0;
    if (!g_win) run_on_window(open_window, NULL);
    return open_ok;
}

typedef struct { VkInstance inst; VkSurfaceKHR surface; } SurfaceReq;

static void make_surface(void *arg) {
    SurfaceReq *r = arg;
#ifdef __ANDROID__
    if (vk_loader_is_custom()) {
        void *anw = SDL_GetPointerProperty(SDL_GetWindowProperties(g_win), SDL_PROP_WINDOW_ANDROID_WINDOW_POINTER, NULL);
        if (!vk_loader_create_android_surface(r->inst, anw, &r->surface)) {
            rt_log("gfx: custom driver vkCreateAndroidSurfaceKHR failed");
            r->surface = VK_NULL_HANDLE;
        }
        return;
    }
#endif
    if (!SDL_Vulkan_CreateSurface(g_win, r->inst, NULL, &r->surface)) {
        rt_log("gfx: SDL_Vulkan_CreateSurface failed (%s)", SDL_GetError());
        r->surface = VK_NULL_HANDLE;
    }
}

VkSurfaceKHR wsys_surface(VkInstance inst) {
    SurfaceReq r = {inst, VK_NULL_HANDLE};
    run_on_window(make_surface, &r);
    return r.surface;
}

void wsys_client_size(int *w, int *h) {
    *w = *h = 0;
    if (g_win) SDL_GetWindowSizeInPixels(g_win, w, h);
}

int wsys_minimized(void) { return g_win && (SDL_GetWindowFlags(g_win) & (SDL_WINDOW_MINIMIZED | SDL_WINDOW_HIDDEN)); }

static void resize_window(void *arg) {
    const int *wh = arg;
    set_pixel_size(wh[0], wh[1]);
    SDL_SyncWindow(g_win);
}

void wsys_resize(int w, int h) {
    int wh[2] = {w, h};
    run_on_window(resize_window, wh);
}

// ---- --probe-window: synthetic events through the event handler, on the window thread
static void probe_event(void *arg) { handle(arg); }

void wsys_probe_focus(int in) {
    SDL_Event e = {.window = {.type = in ? SDL_EVENT_WINDOW_FOCUS_GAINED : SDL_EVENT_WINDOW_FOCUS_LOST,
                              .windowID = SDL_GetWindowID(g_win)}};
    run_on_window(probe_event, &e);
}

void wsys_probe_escape(void) {
    SDL_Event e = {.key = {.type = SDL_EVENT_KEY_DOWN, .scancode = SDL_SCANCODE_ESCAPE, .key = SDLK_ESCAPE, .down = true,
                           .windowID = SDL_GetWindowID(g_win)}};
    run_on_window(probe_event, &e);
    e.key.type = SDL_EVENT_KEY_UP, e.key.down = false;
    run_on_window(probe_event, &e);
}

// SDL reports no activation by a title bar click: only the client click exists.
int wsys_probe_click(int where) {
    if (where == 2) return 0;
    SDL_Event e = {.button = {.type = SDL_EVENT_MOUSE_BUTTON_DOWN, .button = SDL_BUTTON_LEFT, .down = true,
                              .windowID = SDL_GetWindowID(g_win)}};
    run_on_window(probe_event, &e);
    e.button.type = SDL_EVENT_MOUSE_BUTTON_UP, e.button.down = false;
    run_on_window(probe_event, &e);
    return 1;
}
