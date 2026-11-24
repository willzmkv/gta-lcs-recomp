#include "window.h"
#include "render_bridge.h"
#include "game/lcs_prompts.h"
#include "input/input_system.h"
#include "input/mouse_aim.h"
#include "input/pc_controls.h"
#include "os/jni_bridge.h"
#include "ui/imgui_overlay.h"

#include <iostream>
#include <fstream>
#include <vector>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <thread>

#if defined(LCS_PLATFORM_WINDOWS)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace lcs::graphics {

namespace {

constexpr const char* kWindowClassName = "GTALCSNativeWindowClass";

struct WindowState {
#if defined(LCS_PLATFORM_WINDOWS)
    HWND hwnd = nullptr;
    HINSTANCE hinstance = nullptr;
    bool class_registered = false;
    WINDOWPLACEMENT saved_placement{sizeof(WINDOWPLACEMENT)};
    DWORD saved_style = WS_OVERLAPPEDWINDOW;
    DWORD saved_ex_style = WS_EX_APPWINDOW;
    bool has_saved_placement = false;
    std::thread::id owner_thread_id{};
    POINT last_warp_client{-1, -1};
    bool warped_this_frame = false;
    bool in_loading_pump = false;
    bool raw_mouse_registered = false;
    bool got_raw_rel_this_frame = false;
    bool menu_touch_down = false;
    float last_menu_nx = 0.0f;
    float last_menu_ny = 0.0f;
#endif
    uint32_t width = 1280;
    uint32_t height = 720;
    bool open = false;
    bool fullscreen = false;
    ResizeCallback on_resize;
    FullscreenCallback on_fullscreen_changed;
    bool pending_resize = false;
    uint32_t new_width = 1280;
    uint32_t new_height = 720;
};

WindowState g_win;

static bool s_cursor_locked = false;
static bool s_os_cursor_hidden = false;
static bool s_reset_mouse_wake = false;

#if defined(LCS_PLATFORM_WINDOWS)
static void set_os_cursor_visible(bool visible) {
    if (visible) {
        if (s_os_cursor_hidden) {
            s_os_cursor_hidden = false;
            while (ShowCursor(TRUE) < 0) {}
        }
        if (g_win.hwnd && g_win.hwnd == GetForegroundWindow()) {
            SetCursor(LoadCursorA(nullptr, IDC_ARROW));
        }
    } else {
        const bool was_hidden = s_os_cursor_hidden;
        s_os_cursor_hidden = true;
        if (!was_hidden) {
            s_reset_mouse_wake = true;
        }
        CURSORINFO ci{sizeof(ci)};
        if (!was_hidden || (GetCursorInfo(&ci) && (ci.flags & CURSOR_SHOWING))) {
            while (ShowCursor(FALSE) >= 0) {}
        }
        if (GetCursor() != nullptr) {
            SetCursor(nullptr);
        }
    }
}
#endif

#if defined(LCS_PLATFORM_WINDOWS)
static int normalize_win32_vk(WPARAM vk, LPARAM lparam) {
    UINT sc = static_cast<UINT>((lparam >> 16) & 0xff);
    bool ext = (lparam & (1 << 24)) != 0;
    switch (vk) {
        case VK_SHIFT:
            return static_cast<int>(MapVirtualKeyA(sc, MAPVK_VSC_TO_VK_EX));
        case VK_CONTROL:
            return ext ? VK_RCONTROL : VK_LCONTROL;
        case VK_MENU:
            return ext ? VK_RMENU : VK_LMENU;
        default:
            return static_cast<int>(vk);
    }
}
#endif

#if defined(LCS_PLATFORM_WINDOWS)
static void lock_cursor_to_window() {
    if (!g_win.hwnd || g_win.hwnd != GetForegroundWindow()) return;
    if (!s_cursor_locked) {
        s_cursor_locked = true;
    }
    set_os_cursor_visible(false);
    RECT rc{};
    if (GetClientRect(g_win.hwnd, &rc) && rc.right > rc.left && rc.bottom > rc.top) {
        POINT tl{rc.left, rc.top};
        POINT br{rc.right, rc.bottom};
        ClientToScreen(g_win.hwnd, &tl);
        ClientToScreen(g_win.hwnd, &br);
        RECT clip{tl.x, tl.y, br.x, br.y};
        ClipCursor(&clip);
        POINT center{(tl.x + br.x) / 2, (tl.y + br.y) / 2};
        g_win.last_warp_client = {
            static_cast<LONG>((rc.right - rc.left) / 2),
            static_cast<LONG>((rc.bottom - rc.top) / 2)
        };
        g_win.warped_this_frame = true;
        SetCursorPos(center.x, center.y);
    }
    lcs::input::MouseAim::instance().reset();
}

static void unlock_cursor_from_window() {
    if (s_cursor_locked) {
        s_cursor_locked = false;
        ClipCursor(nullptr);
    }
    const bool pad_active =
        (lcs::game::LCSPrompts::instance().active_device() != lcs::game::PromptDevice::KeyboardMouse);
    set_os_cursor_visible(!pad_active);
    lcs::input::MouseAim::instance().reset();
}

static void normalize_client_menu_pos(int x, int y, float& nx, float& ny) {
    float rx = 0.0f, ry = 0.0f, rw = 0.0f, rh = 0.0f;
    RenderBridge::instance().get_content_rect(rx, ry, rw, rh);
    if (rw <= 0.0f || rh <= 0.0f) {
        rw = static_cast<float>(g_win.width);
        rh = static_cast<float>(g_win.height);
    }
    nx = (rw > 0.0f) ? std::clamp((static_cast<float>(x) - rx) / rw, 0.0f, 1.0f) : 0.0f;
    ny = (rh > 0.0f) ? std::clamp((static_cast<float>(y) - ry) / rh, 0.0f, 1.0f) : 0.0f;
}

// LCS_HIDDEN_WINDOW=1: create the window and swapchain but never show them (automated
// verification runs in the background without taking over the user's screen).
static bool hidden_window_mode() {
    static const bool s_hidden = [] {
        const char* e = std::getenv("LCS_HIDDEN_WINDOW");
        return e && e[0] == '1';
    }();
    return s_hidden;
}

static bool enter_win32_borderless_fullscreen() {
    if (!g_win.hwnd || hidden_window_mode()) return false;
    g_win.saved_style = static_cast<DWORD>(GetWindowLongPtrA(g_win.hwnd, GWL_STYLE));
    g_win.saved_ex_style = static_cast<DWORD>(GetWindowLongPtrA(g_win.hwnd, GWL_EXSTYLE));
    g_win.saved_placement = WINDOWPLACEMENT{sizeof(WINDOWPLACEMENT)};
    if (GetWindowPlacement(g_win.hwnd, &g_win.saved_placement)) {
        if (g_win.saved_placement.showCmd == SW_HIDE) {
            g_win.saved_placement.showCmd = SW_SHOWNORMAL;
        }
        g_win.has_saved_placement = true;
    }

    HMONITOR mon = MonitorFromWindow(g_win.hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(MONITORINFO);
    if (!GetMonitorInfoA(mon, &mi)) {
        return false;
    }

    DWORD fs_style = (g_win.saved_style & ~WS_OVERLAPPEDWINDOW) | WS_POPUP | WS_VISIBLE;
    SetWindowLongPtrA(g_win.hwnd, GWL_STYLE, static_cast<LONG_PTR>(fs_style));
    SetWindowPos(
        g_win.hwnd, HWND_TOP,
        mi.rcMonitor.left, mi.rcMonitor.top,
        mi.rcMonitor.right - mi.rcMonitor.left,
        mi.rcMonitor.bottom - mi.rcMonitor.top,
        SWP_NOOWNERZORDER | SWP_FRAMECHANGED | SWP_SHOWWINDOW
    );
    return true;
}
#endif

static void reset_keyboard_state() {
    lcs::input::PCControls::instance().reset_all();
    float rx = 0.0f, ry = 0.0f;
    if (lcs::input::MouseAim::instance().consume_axes(true, false, 1.0f / 60.0f, rx, ry)) {
        lcs::input::InputSystem::instance().queue_joy_axis(0, 2, rx);
        lcs::input::InputSystem::instance().queue_joy_axis(0, 3, ry);
    }
#if defined(LCS_PLATFORM_WINDOWS)
    unlock_cursor_from_window();
#endif
}

#if defined(LCS_PLATFORM_WINDOWS)
LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    if (g_win.in_loading_pump) {
        switch (msg) {
            case WM_LBUTTONDOWN:
            case WM_RBUTTONDOWN:
            case WM_MBUTTONDOWN:
            case WM_MOUSEWHEEL:
            case WM_MOUSEMOVE:
            case WM_KEYDOWN:
            case WM_SYSKEYDOWN:
                return 0;
            default:
                break;
        }
    }
    // Always allow Alt+Enter & F11 fullscreen toggles even when Main Menu / Global Menu blocks game input
    if (msg == WM_KEYDOWN && wparam == VK_F11 && (lparam & (1 << 30)) == 0) {
        toggle_fullscreen();
        return 0;
    }
    if (msg == WM_SYSKEYDOWN && wparam == VK_RETURN && (lparam & (1 << 29)) != 0 && (lparam & (1 << 30)) == 0) {
        toggle_fullscreen();
        return 0;
    }

    // Notify prompt detector on real keyboard/mouse activity even when menus consume input
    if (msg == WM_KEYDOWN || msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN || msg == WM_MBUTTONDOWN || msg == WM_MOUSEWHEEL) {
        lcs::game::LCSPrompts::instance().notify_keyboard_mouse();
        const bool in_menu_now = lcs::input::PCControls::instance().in_menu() || lcs::ui_overlay::is_blocking_game_input();
        if (in_menu_now && !s_cursor_locked &&
            (msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN || msg == WM_MBUTTONDOWN || msg == WM_MOUSEWHEEL)) {
            set_os_cursor_visible(true);
        }
    } else if (msg == WM_MOUSEMOVE) {
        static int s_last_pmx = -1, s_last_pmy = -1;
        const int px = static_cast<short>(LOWORD(lparam));
        const int py = static_cast<short>(HIWORD(lparam));
        if (s_reset_mouse_wake) {
            s_reset_mouse_wake = false;
            s_last_pmx = px;
            s_last_pmy = py;
        } else if (!g_win.warped_this_frame && !(px == g_win.last_warp_client.x && py == g_win.last_warp_client.y)) {
            if (s_last_pmx < 0) {
                s_last_pmx = px;
                s_last_pmy = py;
            } else if ((std::abs(px - s_last_pmx) + std::abs(py - s_last_pmy)) >= 18) {
                s_last_pmx = px;
                s_last_pmy = py;
                lcs::game::LCSPrompts::instance().notify_keyboard_mouse();
                if (!s_cursor_locked && (lcs::input::PCControls::instance().in_menu() || lcs::ui_overlay::is_blocking_game_input())) {
                    set_os_cursor_visible(true);
                }
            }
        } else {
            s_last_pmx = px;
            s_last_pmy = py;
        }
    }

    // Forward to Dear ImGui (Main Menu / Global Menu / Settings / Trainer); block only input messages while open
    const bool is_input_msg =
        (msg == WM_KEYDOWN || msg == WM_KEYUP || msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP ||
         msg == WM_CHAR || msg == WM_LBUTTONDOWN || msg == WM_LBUTTONUP ||
         msg == WM_RBUTTONDOWN || msg == WM_RBUTTONUP || msg == WM_MBUTTONDOWN || msg == WM_MBUTTONUP ||
         msg == WM_MOUSEWHEEL || msg == WM_MOUSEMOVE || msg == WM_INPUT);
    if (msg != WM_SIZE && msg != WM_CLOSE && msg != WM_DESTROY && msg != WM_SETFOCUS && msg != WM_KILLFOCUS) {
        if (lcs::ui_overlay::handle_win32_message(hwnd, static_cast<uint32_t>(msg),
                                                  static_cast<uintptr_t>(wparam), static_cast<intptr_t>(lparam))) {
            if (is_input_msg) {
                // Always deliver key/button release events to PCControls so keys released after opening
                // a menu never stay latched inside m_vk_held.
                auto& rel_ctrl = lcs::input::PCControls::instance();
                if (msg == WM_KEYUP || msg == WM_SYSKEYUP) {
                    rel_ctrl.on_key_event(normalize_win32_vk(wparam, lparam), false);
                } else if (msg == WM_LBUTTONUP) {
                    rel_ctrl.on_mouse_button(VK_LBUTTON, false);
                } else if (msg == WM_RBUTTONUP) {
                    rel_ctrl.on_mouse_button(VK_RBUTTON, false);
                } else if (msg == WM_MBUTTONUP) {
                    rel_ctrl.on_mouse_button(VK_MBUTTON, false);
                }
                return 0;
            }
        }
    }

    auto& controls = lcs::input::PCControls::instance();
    const bool in_menu = controls.in_menu() || lcs::ui_overlay::is_blocking_game_input();

    switch (msg) {
        case WM_SIZE: {
            if (wparam != SIZE_MINIMIZED) {
                uint32_t new_w = static_cast<uint32_t>(LOWORD(lparam));
                uint32_t new_h = static_cast<uint32_t>(HIWORD(lparam));
                if (new_w > 0 && new_h > 0 && (new_w != g_win.width || new_h != g_win.height)) {
                    g_win.width = new_w;
                    g_win.height = new_h;
                    g_win.pending_resize = true;
                    g_win.new_width = new_w;
                    g_win.new_height = new_h;
                    if (g_win.on_resize) {
                        g_win.on_resize(new_w, new_h);
                    }
                }
            }
            return 0;
        }
        case WM_SETCURSOR: {
            if (LOWORD(lparam) == HTCLIENT && s_os_cursor_hidden) {
                SetCursor(nullptr);
                return TRUE;
            }
            return DefWindowProcA(hwnd, msg, wparam, lparam);
        }
        case WM_SETFOCUS: {
            if (!in_menu) {
                lock_cursor_to_window();
            }
            return 0;
        }
        case WM_KILLFOCUS: {
            if (g_win.menu_touch_down) {
                g_win.menu_touch_down = false;
                ReleaseCapture();
                lcs::input::InputSystem::instance().queue_touch_up(0, g_win.last_menu_nx, g_win.last_menu_ny);
            }
            reset_keyboard_state();
            return 0;
        }
        case WM_LBUTTONDOWN: {
            if (lcs::os::JNIBridge::instance().is_video_playing()) {
                lcs::os::JNIBridge::instance().skip_video();
                return 0;
            }
            if (in_menu) {
                int x = static_cast<short>(LOWORD(lparam));
                int y = static_cast<short>(HIWORD(lparam));
                float nx = 0.0f, ny = 0.0f;
                normalize_client_menu_pos(x, y, nx, ny);
                g_win.menu_touch_down = true;
                g_win.last_menu_nx = nx;
                g_win.last_menu_ny = ny;
                SetCapture(hwnd);
                lcs::input::InputSystem::instance().queue_touch_down(0, nx, ny);
            } else {
                controls.on_mouse_button(VK_LBUTTON, true);
            }
            return 0;
        }
        case WM_LBUTTONUP: {
            if (g_win.menu_touch_down || in_menu) {
                int x = static_cast<short>(LOWORD(lparam));
                int y = static_cast<short>(HIWORD(lparam));
                float nx = g_win.last_menu_nx, ny = g_win.last_menu_ny;
                if (in_menu) {
                    normalize_client_menu_pos(x, y, nx, ny);
                }
                if (g_win.menu_touch_down) {
                    g_win.menu_touch_down = false;
                    ReleaseCapture();
                }
                lcs::input::InputSystem::instance().queue_touch_up(0, nx, ny);
            } else {
                controls.on_mouse_button(VK_LBUTTON, false);
            }
            return 0;
        }
        case WM_RBUTTONDOWN: {
            controls.on_mouse_button(VK_RBUTTON, true);
            return 0;
        }
        case WM_RBUTTONUP: {
            controls.on_mouse_button(VK_RBUTTON, false);
            return 0;
        }
        case WM_MBUTTONDOWN: {
            controls.on_mouse_button(VK_MBUTTON, true);
            return 0;
        }
        case WM_MBUTTONUP: {
            controls.on_mouse_button(VK_MBUTTON, false);
            return 0;
        }
        case WM_MOUSEWHEEL: {
            controls.on_mouse_wheel(GET_WHEEL_DELTA_WPARAM(wparam));
            return 0;
        }
        case WM_INPUT: {
            if (!in_menu && g_win.hwnd && g_win.hwnd == GetForegroundWindow()) {
                if (!s_cursor_locked) {
                    lock_cursor_to_window();
                } else {
                    RAWINPUT raw{};
                    UINT size = sizeof(raw);
                    if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lparam), RID_INPUT,
                                        &raw, &size, sizeof(RAWINPUTHEADER)) != static_cast<UINT>(-1)) {
                        if (raw.header.dwType == RIM_TYPEMOUSE &&
                            (raw.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) == 0) {
                            const int dx = static_cast<int>(raw.data.mouse.lLastX);
                            const int dy = static_cast<int>(raw.data.mouse.lLastY);
                            if (dx != 0 || dy != 0) {
                                g_win.got_raw_rel_this_frame = true;
                                lcs::input::MouseAim::instance().add_delta(dx, dy);
                            }
                        }
                    }
                }
            }
            return DefWindowProcA(hwnd, msg, wparam, lparam);
        }
        case WM_MOUSEMOVE: {
            int x = static_cast<short>(LOWORD(lparam));
            int y = static_cast<short>(HIWORD(lparam));
            if (in_menu) {
                if (wparam & MK_LBUTTON) {
                    float nx = 0.0f, ny = 0.0f;
                    normalize_client_menu_pos(x, y, nx, ny);
                    lcs::input::InputSystem::instance().queue_touch_move(0, nx, ny);
                }
            } else if (g_win.hwnd && g_win.hwnd == GetForegroundWindow()) {
                if (!s_cursor_locked) {
                    lock_cursor_to_window();
                    return 0;
                }
                if (x == g_win.last_warp_client.x && y == g_win.last_warp_client.y) {
                    return 0;
                }
                int cx = static_cast<int>(g_win.width) / 2;
                int cy = static_cast<int>(g_win.height) / 2;
                int dx = x - cx;
                int dy = y - cy;
                if (dx != 0 || dy != 0) {
                    if (!g_win.got_raw_rel_this_frame && !g_win.warped_this_frame) {
                        lcs::input::MouseAim::instance().add_delta(dx, dy);
                    }
                    if (!g_win.warped_this_frame) {
                        g_win.last_warp_client = {cx, cy};
                        g_win.warped_this_frame = true;
                        POINT pt = {cx, cy};
                        ClientToScreen(hwnd, &pt);
                        SetCursorPos(pt.x, pt.y);
                    }
                }
            }
            return 0;
        }
        case WM_KEYDOWN: {
            if (lparam & (1 << 30)) return 0; // Ignore autorepeat
            if (lcs::os::JNIBridge::instance().is_video_playing() &&
                wparam != VK_SHIFT && wparam != VK_CONTROL && wparam != VK_MENU) {
                lcs::os::JNIBridge::instance().skip_video();
                return 0;
            }
            controls.on_key_event(normalize_win32_vk(wparam, lparam), true);
            return 0;
        }
        case WM_KEYUP: {
            controls.on_key_event(normalize_win32_vk(wparam, lparam), false);
            return 0;
        }
        case WM_SYSKEYDOWN: {
            if (wparam == VK_RETURN && (lparam & (1 << 29)) != 0 && (lparam & (1 << 30)) == 0) {
                toggle_fullscreen();
                return 0;
            }
            if (wparam == VK_MENU && (lparam & (1 << 30)) == 0) {
                controls.on_key_event(normalize_win32_vk(wparam, lparam), true);
                return 0; // Handle Left Alt (Walk) cleanly without triggering window sysmenu
            }
            return DefWindowProcA(hwnd, msg, wparam, lparam);
        }
        case WM_SYSKEYUP: {
            controls.on_key_event(normalize_win32_vk(wparam, lparam), false);
            if (wparam == VK_MENU) {
                return 0;
            }
            return DefWindowProcA(hwnd, msg, wparam, lparam);
        }
        case WM_SYSCHAR: {
            if ((wparam == VK_RETURN || wparam == VK_MENU) && (lparam & (1 << 29)) != 0) {
                return 0; // Suppress default Windows Alt+Enter / Alt beep
            }
            return DefWindowProcA(hwnd, msg, wparam, lparam);
        }
        case WM_CLOSE:
            g_win.open = false;
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            g_win.open = false;
            g_win.hwnd = nullptr;
            PostQuitMessage(0);
            return 0;
        default:
            return DefWindowProcA(hwnd, msg, wparam, lparam);
    }
}
#endif

} // namespace

void set_menu_active(bool in_menu) {
    set_gameplay_context(in_menu, false, false);
}

void set_gameplay_context(bool in_menu, bool in_vehicle, bool in_cutscene) {
    const bool effective_menu = in_menu || lcs::ui_overlay::is_blocking_game_input();
    auto& controls = lcs::input::PCControls::instance();
    const bool prev_menu = controls.in_menu();
    controls.set_context(effective_menu, in_vehicle, in_cutscene);
    if (effective_menu) {
        if (s_cursor_locked) {
            unlock_cursor_from_window();
            reset_keyboard_state();
        }
    } else if (prev_menu != effective_menu) {
#if defined(LCS_PLATFORM_WINDOWS)
        if (g_win.menu_touch_down) {
            g_win.menu_touch_down = false;
            ReleaseCapture();
            lcs::input::InputSystem::instance().queue_touch_up(0, g_win.last_menu_nx, g_win.last_menu_ny);
        }
        lock_cursor_to_window();
#endif
    }
}

bool is_menu_active() {
    return lcs::input::PCControls::instance().in_menu();
}

void sync_menu_cursor_visibility() {
#if defined(LCS_PLATFORM_WINDOWS)
    if (g_win.hwnd && g_win.hwnd == GetForegroundWindow()) {
        const bool in_menu = lcs::input::PCControls::instance().in_menu() || lcs::ui_overlay::is_blocking_game_input();
        if (in_menu && !s_cursor_locked) {
            const bool pad_active =
                (lcs::game::LCSPrompts::instance().active_device() != lcs::game::PromptDevice::KeyboardMouse);
            set_os_cursor_visible(!pad_active);
        } else if (!in_menu) {
            set_os_cursor_visible(false);
        }
    }
#endif
}

#if defined(LCS_PLATFORM_WINDOWS)
static void ensure_dpi_awareness() {
    static bool s_done = false;
    if (s_done) return;
    s_done = true;
    HMODULE user32 = GetModuleHandleA("user32.dll");
    if (user32) {
        using SetDpiCtxFn = BOOL(WINAPI*)(HANDLE);
        auto fn = reinterpret_cast<SetDpiCtxFn>(GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
        if (fn) {
            // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 = (HANDLE)-4
            fn(reinterpret_cast<HANDLE>(static_cast<intptr_t>(-4)));
        } else {
            SetProcessDPIAware();
        }
    }
}
#endif

bool create_game_window(int width, int height, const char* title, bool fullscreen) {
#if defined(LCS_PLATFORM_WINDOWS)
    if (g_win.hwnd) return true;

    // Enable DPI awareness so the swapchain matches native desktop pixels.
    ensure_dpi_awareness();

    g_win.hinstance = GetModuleHandleA(nullptr);
    WNDCLASSEXA wc{};
    wc.cbSize = sizeof(WNDCLASSEXA);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = g_win.hinstance;
    wc.hCursor = LoadCursorA(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    wc.lpszClassName = kWindowClassName;

    if (!RegisterClassExA(&wc)) {
        DWORD err = GetLastError();
        if (err != ERROR_CLASS_ALREADY_EXISTS) {
            std::cerr << "[Window] RegisterClassExA failed (error " << err << ").\n";
            return false;
        }
    }
    g_win.class_registered = true;

    DWORD style = WS_OVERLAPPEDWINDOW;
    DWORD ex_style = WS_EX_APPWINDOW;
    RECT rect{0, 0, width, height};
    AdjustWindowRectEx(&rect, style, FALSE, ex_style);

    int win_w = rect.right - rect.left;
    int win_h = rect.bottom - rect.top;

    // Center window inside primary monitor work area, clamping to fit if larger.
    RECT work_area{};
    int pos_x = CW_USEDEFAULT;
    int pos_y = CW_USEDEFAULT;
    if (SystemParametersInfoA(SPI_GETWORKAREA, 0, &work_area, 0)) {
        int area_w = work_area.right - work_area.left;
        int area_h = work_area.bottom - work_area.top;
        if (win_w > area_w) win_w = area_w;
        if (win_h > area_h) win_h = area_h;
        pos_x = work_area.left + (area_w - win_w) / 2;
        pos_y = work_area.top + (area_h - win_h) / 2;
    }

    g_win.hwnd = CreateWindowExA(
        ex_style,
        kWindowClassName,
        title ? title : "GTA: Liberty City Stories",
        style,
        pos_x, pos_y,
        win_w, win_h,
        nullptr, nullptr, g_win.hinstance, nullptr
    );

    if (!g_win.hwnd) {
        std::cerr << "[Window] CreateWindowExA failed (error " << GetLastError() << ").\n";
        return false;
    }

    g_win.open = true;
    g_win.owner_thread_id = std::this_thread::get_id();
    g_win.fullscreen = false;
    if (hidden_window_mode()) {
        std::cout << "[Window] LCS_HIDDEN_WINDOW=1: window stays hidden.\n";
    } else if (fullscreen && enter_win32_borderless_fullscreen()) {
        g_win.fullscreen = true;
    } else {
        ShowWindow(g_win.hwnd, SW_SHOW);
    }

    RECT client{};
    GetClientRect(g_win.hwnd, &client);
    g_win.width = static_cast<uint32_t>(std::max<LONG>(1, client.right - client.left));
    g_win.height = static_cast<uint32_t>(std::max<LONG>(1, client.bottom - client.top));
    g_win.new_width = g_win.width;
    g_win.new_height = g_win.height;
    g_win.pending_resize = false;

    if (!hidden_window_mode()) {
        UpdateWindow(g_win.hwnd);
        SetForegroundWindow(g_win.hwnd);
        SetFocus(g_win.hwnd);
    }

    RAWINPUTDEVICE rid{};
    rid.usUsagePage = 0x01; // Generic desktop controls
    rid.usUsage = 0x02;     // Mouse
    rid.dwFlags = 0;        // Foreground-only hardware deltas
    rid.hwndTarget = g_win.hwnd;
    g_win.raw_mouse_registered = (RegisterRawInputDevices(&rid, 1, sizeof(rid)) == TRUE);

    std::cout << "[Window] Created native Win32 window (" << g_win.width << "x" << g_win.height
              << (g_win.fullscreen ? ", borderless fullscreen" : ", windowed") << ")\n";
    return true;
#else
    (void)title;
    g_win.width = static_cast<uint32_t>(width);
    g_win.height = static_cast<uint32_t>(height);
    g_win.fullscreen = fullscreen;
    g_win.open = true;
    return true;
#endif
}

void destroy_game_window() {
#if defined(LCS_PLATFORM_WINDOWS)
    if (g_win.hwnd) {
        DestroyWindow(g_win.hwnd);
        g_win.hwnd = nullptr;
    }
    if (g_win.class_registered && g_win.hinstance) {
        UnregisterClassA(kWindowClassName, g_win.hinstance);
        g_win.class_registered = false;
    }
#endif
    g_win.open = false;
}

#ifndef PW_RENDERFULLCONTENT
#define PW_RENDERFULLCONTENT 0x00000002
#endif

bool capture_window_bmp(const char* filepath) {
#if defined(LCS_PLATFORM_WINDOWS)
    if (!g_win.hwnd || !filepath) return false;
    RECT rc{};
    if (!GetClientRect(g_win.hwnd, &rc)) return false;
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return false;

    HDC hdcScreen = GetDC(g_win.hwnd);
    HDC hdcMem = CreateCompatibleDC(hdcScreen);
    HBITMAP hbm = CreateCompatibleBitmap(hdcScreen, w, h);
    HGDIOBJ old = SelectObject(hdcMem, hbm);

    // PW_RENDERFULLCONTENT works on DWM flip-model D3D12 swapchains on Windows 10/11
    RECT wr{};
    GetWindowRect(g_win.hwnd, &wr);
    POINT pt{0, 0};
    ClientToScreen(g_win.hwnd, &pt);
    int dx = pt.x - wr.left;
    int dy = pt.y - wr.top;
    int ww = wr.right - wr.left;
    int wh = wr.bottom - wr.top;

    HDC hdcWin = CreateCompatibleDC(hdcScreen);
    HBITMAP hbmWin = CreateCompatibleBitmap(hdcScreen, ww, wh);
    HGDIOBJ oldWin = SelectObject(hdcWin, hbmWin);
    bool ok = PrintWindow(g_win.hwnd, hdcWin, PW_RENDERFULLCONTENT) != FALSE;
    if (ok) {
        BitBlt(hdcMem, 0, 0, w, h, hdcWin, dx, dy, SRCCOPY);
    }
    SelectObject(hdcWin, oldWin);
    DeleteObject(hbmWin);
    DeleteDC(hdcWin);

    BITMAPINFOHEADER bi{};
    bi.biSize = sizeof(BITMAPINFOHEADER);
    bi.biWidth = w;
    bi.biHeight = h; // bottom-up
    bi.biPlanes = 1;
    bi.biBitCount = 32;
    bi.biCompression = BI_RGB;
    size_t rowBytes = static_cast<size_t>(w) * 4;
    std::vector<uint8_t> pixels(rowBytes * static_cast<size_t>(h));
    GetDIBits(hdcMem, hbm, 0, static_cast<UINT>(h), pixels.data(), reinterpret_cast<BITMAPINFO*>(&bi), DIB_RGB_COLORS);

    SelectObject(hdcMem, old);
    DeleteObject(hbm);
    DeleteDC(hdcMem);
    ReleaseDC(g_win.hwnd, hdcScreen);

    if (!ok) return false;
    BITMAPFILEHEADER bmf{};
    bmf.bfType = 0x4D42;
    bmf.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    bmf.bfSize = bmf.bfOffBits + static_cast<uint32_t>(pixels.size());
    std::ofstream ofs(filepath, std::ios::binary);
    if (!ofs) return false;
    ofs.write(reinterpret_cast<const char*>(&bmf), sizeof(bmf));
    ofs.write(reinterpret_cast<const char*>(&bi), sizeof(bi));
    ofs.write(reinterpret_cast<const char*>(pixels.data()), pixels.size());
    return true;
#else
    (void)filepath;
    return false;
#endif
}

bool poll_window_events() {
#if defined(LCS_PLATFORM_WINDOWS)
    auto& controls = lcs::input::PCControls::instance();
    controls.tick_frame();
    g_win.warped_this_frame = false;
    g_win.got_raw_rel_this_frame = false;

    MSG msg{};
    int processed = 0;
    while (processed++ < 256 && PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) {
            g_win.open = false;
            return false;
        }
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }

    if (g_win.hwnd && g_win.hwnd == GetForegroundWindow()) {
        const bool in_menu = controls.in_menu() || lcs::ui_overlay::is_blocking_game_input();
        if (in_menu && !s_cursor_locked) {
            const bool pad_active =
                (lcs::game::LCSPrompts::instance().active_device() != lcs::game::PromptDevice::KeyboardMouse);
            set_os_cursor_visible(!pad_active);
        } else if (!in_menu) {
            set_os_cursor_visible(false);
        }
    }

    return g_win.open;
#else
    return g_win.open;
#endif
}

bool pump_loading_events() {
#if defined(LCS_PLATFORM_WINDOWS)
    if (!g_win.hwnd || std::this_thread::get_id() != g_win.owner_thread_id) {
        return g_win.open;
    }
    if (g_win.in_loading_pump) {
        return g_win.open;
    }
    struct PumpScope {
        bool& flag;
        explicit PumpScope(bool& f) : flag(f) { flag = true; }
        ~PumpScope() { flag = false; }
    } scope(g_win.in_loading_pump);

    MSG msg{};
    int processed = 0;
    while (processed++ < 128 && PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) {
            g_win.open = false;
            return false;
        }
        TranslateMessage(&msg);
        DispatchMessageA(&msg);
    }
    return g_win.open;
#else
    return g_win.open;
#endif
}

void* get_native_window_handle() {
#if defined(LCS_PLATFORM_WINDOWS)
    return static_cast<void*>(g_win.hwnd);
#else
    return nullptr;
#endif
}

void get_window_size(uint32_t& width, uint32_t& height) {
    width = g_win.width;
    height = g_win.height;
}

void get_desktop_resolution(uint32_t& width, uint32_t& height) {
#if defined(LCS_PLATFORM_WINDOWS)
    ensure_dpi_awareness();
    HMONITOR mon = g_win.hwnd
        ? MonitorFromWindow(g_win.hwnd, MONITOR_DEFAULTTONEAREST)
        : MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoA(mon, &mi)) {
        width = static_cast<uint32_t>(std::max<LONG>(1, mi.rcMonitor.right - mi.rcMonitor.left));
        height = static_cast<uint32_t>(std::max<LONG>(1, mi.rcMonitor.bottom - mi.rcMonitor.top));
        return;
    }
    width = static_cast<uint32_t>(std::max(1, GetSystemMetrics(SM_CXSCREEN)));
    height = static_cast<uint32_t>(std::max(1, GetSystemMetrics(SM_CYSCREEN)));
#else
    width = 1920;
    height = 1080;
#endif
}

void set_window_size(uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) return;
#if defined(LCS_PLATFORM_WINDOWS)
    if (!g_win.hwnd) {
        g_win.width = width;
        g_win.height = height;
        return;
    }
    DWORD style = g_win.fullscreen
        ? ((g_win.saved_style & ~WS_POPUP) | WS_OVERLAPPEDWINDOW)
        : static_cast<DWORD>(GetWindowLongPtrA(g_win.hwnd, GWL_STYLE));
    DWORD ex_style = g_win.fullscreen
        ? g_win.saved_ex_style
        : static_cast<DWORD>(GetWindowLongPtrA(g_win.hwnd, GWL_EXSTYLE));

    RECT rect{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
    AdjustWindowRectEx(&rect, style, FALSE, ex_style);
    int win_w = rect.right - rect.left;
    int win_h = rect.bottom - rect.top;

    HMONITOR mon = MonitorFromWindow(g_win.hwnd, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{};
    mi.cbSize = sizeof(mi);
    int pos_x = 100;
    int pos_y = 100;
    if (GetMonitorInfoA(mon, &mi)) {
        int area_w = mi.rcWork.right - mi.rcWork.left;
        int area_h = mi.rcWork.bottom - mi.rcWork.top;
        if (win_w > area_w) win_w = area_w;
        if (win_h > area_h) win_h = area_h;
        pos_x = mi.rcWork.left + (area_w - win_w) / 2;
        pos_y = mi.rcWork.top + (area_h - win_h) / 2;
    }

    if (g_win.fullscreen) {
        // Defer applying the windowed bounds until leaving fullscreen.
        g_win.saved_placement.length = sizeof(WINDOWPLACEMENT);
        g_win.saved_placement.showCmd = SW_SHOWNORMAL;
        g_win.saved_placement.rcNormalPosition = RECT{pos_x, pos_y, pos_x + win_w, pos_y + win_h};
        g_win.has_saved_placement = true;
    } else {
        SetWindowPos(
            g_win.hwnd, nullptr, pos_x, pos_y, win_w, win_h,
            SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED |
                (hidden_window_mode() ? 0u : SWP_SHOWWINDOW)
        );
        RECT client{};
        if (GetClientRect(g_win.hwnd, &client)) {
            uint32_t cw = static_cast<uint32_t>(std::max<LONG>(1, client.right - client.left));
            uint32_t ch = static_cast<uint32_t>(std::max<LONG>(1, client.bottom - client.top));
            g_win.width = cw;
            g_win.height = ch;
            g_win.new_width = cw;
            g_win.new_height = ch;
            g_win.pending_resize = true;
        }
    }
#else
    g_win.width = width;
    g_win.height = height;
#endif
}

void set_window_resize_callback(ResizeCallback cb) {
    g_win.on_resize = std::move(cb);
}

void set_window_title(const char* title) {
#if defined(LCS_PLATFORM_WINDOWS)
    if (g_win.hwnd && title) {
        SetWindowTextA(g_win.hwnd, title);
    }
#else
    (void)title;
#endif
}

bool get_pending_resize(uint32_t& out_w, uint32_t& out_h) {
    if (g_win.pending_resize) {
        out_w = g_win.new_width;
        out_h = g_win.new_height;
        g_win.pending_resize = false;
        return true;
    }
    return false;
}

bool is_window_open() {
    return g_win.open;
}

void set_fullscreen(bool fullscreen) {
    if (g_win.fullscreen == fullscreen || hidden_window_mode()) {
        return;
    }
#if defined(LCS_PLATFORM_WINDOWS)
    if (g_win.hwnd) {
        if (fullscreen) {
            if (enter_win32_borderless_fullscreen()) {
                g_win.fullscreen = true;
            }
        } else {
            DWORD win_style = (g_win.saved_style & ~WS_POPUP) | WS_OVERLAPPEDWINDOW | WS_VISIBLE;
            SetWindowLongPtrA(g_win.hwnd, GWL_STYLE, static_cast<LONG_PTR>(win_style));
            if (g_win.has_saved_placement) {
                SetWindowPlacement(g_win.hwnd, &g_win.saved_placement);
                SetWindowPos(
                    g_win.hwnd, nullptr, 0, 0, 0, 0,
                    SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED | SWP_SHOWWINDOW
                );
            } else {
                SetWindowPos(
                    g_win.hwnd, nullptr, 100, 100, 1280, 720,
                    SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED | SWP_SHOWWINDOW
                );
            }
            g_win.fullscreen = false;
        }

        RECT client{};
        if (GetClientRect(g_win.hwnd, &client)) {
            uint32_t cw = static_cast<uint32_t>(std::max<LONG>(1, client.right - client.left));
            uint32_t ch = static_cast<uint32_t>(std::max<LONG>(1, client.bottom - client.top));
            if (cw != g_win.width || ch != g_win.height) {
                g_win.width = cw;
                g_win.height = ch;
                g_win.new_width = cw;
                g_win.new_height = ch;
                g_win.pending_resize = true;
            }
        }
        if (!lcs::input::PCControls::instance().in_menu() && g_win.hwnd == GetForegroundWindow()) {
            lock_cursor_to_window();
        }
    } else {
        g_win.fullscreen = fullscreen;
    }
#else
    g_win.fullscreen = fullscreen;
#endif
    std::cout << "[Window] Mode -> " << (g_win.fullscreen ? "Fullscreen" : "Windowed")
              << " (" << g_win.width << "x" << g_win.height << ")\n" << std::flush;
    if (g_win.on_fullscreen_changed) {
        g_win.on_fullscreen_changed(g_win.fullscreen);
    }
}

bool is_fullscreen() {
    return g_win.fullscreen;
}

void toggle_fullscreen() {
    set_fullscreen(!g_win.fullscreen);
}

void set_fullscreen_callback(FullscreenCallback cb) {
    g_win.on_fullscreen_changed = std::move(cb);
}

} // namespace lcs::graphics
