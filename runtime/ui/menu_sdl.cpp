// SDL3 adapter of the in-game menu: the window's events to menu events (menu.h), as ui/menu_win.cpp does for Win32.
// Window thread (gfx_window_sdl.c's event loop, after input_sdl_event).
//
// F1 opens and closes the menu, Esc opens it (with the menu open, Esc goes to the menu, which closes). Esc also releases
// the cursor (input_sdl.c sees the event first; closing the menu gives the capture back). While the menu is open, its
// keys, the mouse and focus changes go to it; the game reads the keyboard through input_sdl.c and gets nothing while
// the menu is open (input_menu). Mouse positions come in window units and go to the menu in pixels.
#include <SDL3/SDL.h>
#include <math.h>

#include "imgui.h"

extern "C" {
#include "input.h"
#include "input_sdl.h"
#include "platform/platform.h"
#include "ui/menu.h"
}

namespace {
struct FingerSlot {
    SDL_FingerID id;
    float x0, y0;
    bool active;
};
FingerSlot g_fingers[10];
int g_touch_active;
int g_touch_peak;
uint64_t g_touch_t0_ns;
float g_touch_max_delta;
bool g_touch_hold_fired;

void touch_event(const SDL_Event *e) {
    const SDL_TouchFingerEvent &tf = e->tfinger;
    if (e->type == SDL_EVENT_FINGER_DOWN) {
        if (g_touch_active == 0) {
            g_touch_peak = 0;
            g_touch_t0_ns = plat_now_ns();
            g_touch_max_delta = 0.0f;
            g_touch_hold_fired = false;
        }
        int slot = -1;
        for (int i = 0; i < 10; i++) {
            if (g_fingers[i].active && g_fingers[i].id == tf.fingerID) { slot = i; break; }
            if (!g_fingers[i].active && slot < 0) slot = i;
        }
        if (slot >= 0) {
            if (!g_fingers[slot].active) g_touch_active++;
            g_fingers[slot] = {tf.fingerID, tf.x, tf.y, true};
            if (g_touch_active > g_touch_peak) g_touch_peak = g_touch_active;
        }
        if (menu_is_open() && g_touch_peak == 1) {
            int pw = 0, ph = 0;
            if (SDL_Window *w = gfx_window_sdl()) SDL_GetWindowSizeInPixels(w, &pw, &ph);
            if (pw > 0 && ph > 0) {
                menu_event_mouse_pos(tf.x * (float)pw, tf.y * (float)ph);
                menu_event_mouse_button(0, 1);
            }
        }
        return;
    }
    if (e->type == SDL_EVENT_FINGER_MOTION) {
        for (int i = 0; i < 10; i++) {
            if (g_fingers[i].active && g_fingers[i].id == tf.fingerID) {
                float dx = tf.x - g_fingers[i].x0, dy = tf.y - g_fingers[i].y0;
                float dist = sqrtf(dx * dx + dy * dy);
                if (dist > g_touch_max_delta) g_touch_max_delta = dist;
                break;
            }
        }
        if (!g_touch_hold_fired && g_touch_active == 3 && g_touch_peak == 3 && g_touch_max_delta < 0.08f &&
            plat_now_ns() - g_touch_t0_ns >= 650000000ull) {
            g_touch_hold_fired = true;
            menu_perf_cycle();   // 3-finger hold toggles the F10 performance overlay
        }
        if (menu_is_open() && g_touch_peak == 1) {
            int pw = 0, ph = 0;
            if (SDL_Window *w = gfx_window_sdl()) SDL_GetWindowSizeInPixels(w, &pw, &ph);
            if (pw > 0 && ph > 0) menu_event_mouse_pos(tf.x * (float)pw, tf.y * (float)ph);
        }
        return;
    }
    if (e->type == SDL_EVENT_FINGER_UP || e->type == SDL_EVENT_FINGER_CANCELED) {
        for (int i = 0; i < 10; i++) {
            if (g_fingers[i].active && g_fingers[i].id == tf.fingerID) {
                float dx = tf.x - g_fingers[i].x0, dy = tf.y - g_fingers[i].y0;
                float dist = sqrtf(dx * dx + dy * dy);
                if (dist > g_touch_max_delta) g_touch_max_delta = dist;
                g_fingers[i].active = false;
                if (g_touch_active > 0) g_touch_active--;
                break;
            }
        }
        if (menu_is_open() && g_touch_peak == 1) {
            int pw = 0, ph = 0;
            if (SDL_Window *w = gfx_window_sdl()) SDL_GetWindowSizeInPixels(w, &pw, &ph);
            if (pw > 0 && ph > 0) menu_event_mouse_pos(tf.x * (float)pw, tf.y * (float)ph);
            menu_event_mouse_button(0, 0);
        }
        if (g_touch_active == 0 && e->type == SDL_EVENT_FINGER_UP && !g_touch_hold_fired) {
            uint64_t dt_ns = plat_now_ns() - g_touch_t0_ns;
            if (dt_ns <= 500000000ull && g_touch_max_delta < 0.08f) {
                if (g_touch_peak == 3) menu_toggle();          // 3-finger tap -> F1 settings menu
                else if (g_touch_peak >= 4) menu_perf_cycle(); // 4-finger tap -> F10 performance overlay
            }
        }
        if (g_touch_active == 0) g_touch_peak = 0;
    }
}
} // namespace

static ImGuiKey key_of(SDL_Scancode sc) {
    switch (sc) {
    case SDL_SCANCODE_UP: return ImGuiKey_UpArrow;
    case SDL_SCANCODE_DOWN: return ImGuiKey_DownArrow;
    case SDL_SCANCODE_LEFT: return ImGuiKey_LeftArrow;
    case SDL_SCANCODE_RIGHT: return ImGuiKey_RightArrow;
    case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: return ImGuiKey_Enter;
    case SDL_SCANCODE_SPACE: return ImGuiKey_Space;
    case SDL_SCANCODE_ESCAPE: return ImGuiKey_Escape;
    case SDL_SCANCODE_TAB: return ImGuiKey_Tab;
    case SDL_SCANCODE_BACKSPACE: return ImGuiKey_Backspace;
    case SDL_SCANCODE_DELETE: return ImGuiKey_Delete;   // clears a cell of the remapping rows
    case SDL_SCANCODE_HOME: return ImGuiKey_Home;
    case SDL_SCANCODE_END: return ImGuiKey_End;
    case SDL_SCANCODE_PAGEUP: return ImGuiKey_PageUp;
    case SDL_SCANCODE_PAGEDOWN: return ImGuiKey_PageDown;
    case SDL_SCANCODE_LSHIFT: case SDL_SCANCODE_RSHIFT: return ImGuiKey_LeftShift;
    case SDL_SCANCODE_LCTRL: case SDL_SCANCODE_RCTRL: return ImGuiKey_LeftCtrl;
    case SDL_SCANCODE_Q: return ImGuiKey_Q;   // previous tab
    case SDL_SCANCODE_E: return ImGuiKey_E;   // next tab
    default: return ImGuiKey_None;
    }
}

extern "C" void menu_sdl_event(const SDL_Event *e) {
    float d = (float)gfx_window_sdl_density();
    switch (e->type) {
    case SDL_EVENT_KEY_DOWN:
        // Esc with the menu open (opened by F1 too) is not a toggle here: it goes to the menu, which closes itself
        if (!e->key.repeat && (e->key.scancode == SDL_SCANCODE_F1 ||
#ifdef __ANDROID__
                               (e->key.scancode == SDL_SCANCODE_AC_BACK && !menu_is_open()) ||
#endif
                               (e->key.scancode == SDL_SCANCODE_ESCAPE && !menu_is_open()))) {
            menu_toggle();
            return;
        }
#ifdef __ANDROID__
        if (e->key.scancode == SDL_SCANCODE_AC_BACK && menu_is_open()) {
            if (!e->key.repeat) menu_event_key(ImGuiKey_Escape, 1);
            return;
        }
#endif
        if (e->key.scancode == SDL_SCANCODE_F10) {
            if (!e->key.repeat) menu_perf_cycle();
            return;
        }
        [[fallthrough]];
    case SDL_EVENT_KEY_UP:
        if (!menu_is_open()) return;
#ifdef __ANDROID__
        if (e->key.scancode == SDL_SCANCODE_AC_BACK) {
            menu_event_key(ImGuiKey_Escape, 0);
            return;
        }
#endif
        if (ImGuiKey k = key_of(e->key.scancode)) menu_event_key(k, e->type == SDL_EVENT_KEY_DOWN);
        return;
    case SDL_EVENT_FINGER_DOWN:
    case SDL_EVENT_FINGER_MOTION:
    case SDL_EVENT_FINGER_UP:
    case SDL_EVENT_FINGER_CANCELED:
        touch_event(e);
        return;
    case SDL_EVENT_MOUSE_MOTION:
        if (menu_is_open()) menu_event_mouse_pos(e->motion.x * d, e->motion.y * d);
        return;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
        if (!menu_is_open()) return;
        int b = e->button.button == SDL_BUTTON_LEFT ? 0 : e->button.button == SDL_BUTTON_RIGHT ? 1
              : e->button.button == SDL_BUTTON_MIDDLE ? 2 : -1;
        if (b < 0) return;
        menu_event_mouse_pos(e->button.x * d, e->button.y * d);
        menu_event_mouse_button(b, e->type == SDL_EVENT_MOUSE_BUTTON_DOWN);
        return;
    }
    case SDL_EVENT_MOUSE_WHEEL:
        if (menu_is_open()) menu_event_wheel(e->wheel.x, e->wheel.y);
        return;
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        menu_event_focus(e->type == SDL_EVENT_WINDOW_FOCUS_GAINED);
        return;
    }
}
