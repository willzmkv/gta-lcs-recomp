#include "window.h"
#include "input/input_system.h"

#include <iostream>
#include <algorithm>
#include <cmath>

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
#endif
    uint32_t width = 1280;
    uint32_t height = 720;
    bool open = false;
    ResizeCallback on_resize;
    bool pending_resize = false;
    uint32_t new_width = 1280;
    uint32_t new_height = 720;
};

WindowState g_win;

// Keyboard analog stick simulation state
static bool s_key_w = false;
static bool s_key_s = false;
static bool s_key_a = false;
static bool s_key_d = false;
static bool s_key_up = false;
static bool s_key_down = false;
static bool s_key_left = false;
static bool s_key_right = false;
static float s_prev_stick_x = 0.0f;
static float s_prev_stick_y = 0.0f;

static void update_keyboard_movement_axes() {
    float sx = 0.0f;
    float sy = 0.0f;
    if (s_key_d || s_key_right) sx += 1.0f;
    if (s_key_a || s_key_left)  sx -= 1.0f;
    if (s_key_s || s_key_down)  sy += 1.0f;
    if (s_key_w || s_key_up)    sy -= 1.0f;

    if (sx != s_prev_stick_x) {
        s_prev_stick_x = sx;
        lcs::input::InputSystem::instance().queue_joy_axis(0, 0, sx);
    }
    if (sy != s_prev_stick_y) {
        s_prev_stick_y = sy;
        lcs::input::InputSystem::instance().queue_joy_axis(0, 1, sy);
    }
}

#if defined(LCS_PLATFORM_WINDOWS)
LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
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
        case WM_LBUTTONDOWN: {
            int x = static_cast<short>(LOWORD(lparam));
            int y = static_cast<short>(HIWORD(lparam));
            float nx = (g_win.width > 0) ? static_cast<float>(x) / static_cast<float>(g_win.width) : 0.0f;
            float ny = (g_win.height > 0) ? static_cast<float>(y) / static_cast<float>(g_win.height) : 0.0f;
            SetCapture(hwnd);
            lcs::input::InputSystem::instance().queue_touch_down(0, nx, ny);
            // Also map left click to in-game attack/fire (Circle / Button 1)
            lcs::input::InputSystem::instance().queue_joy_button_down(0, 1);
            return 0;
        }
        case WM_LBUTTONUP: {
            int x = static_cast<short>(LOWORD(lparam));
            int y = static_cast<short>(HIWORD(lparam));
            float nx = (g_win.width > 0) ? static_cast<float>(x) / static_cast<float>(g_win.width) : 0.0f;
            float ny = (g_win.height > 0) ? static_cast<float>(y) / static_cast<float>(g_win.height) : 0.0f;
            ReleaseCapture();
            lcs::input::InputSystem::instance().queue_touch_up(0, nx, ny);
            lcs::input::InputSystem::instance().queue_joy_button_up(0, 1);
            return 0;
        }
        case WM_RBUTTONDOWN: {
            // Right-click: Target lock / Aim / Handbrake (R1 / Button 5)
            lcs::input::InputSystem::instance().queue_joy_button_down(0, 5);
            return 0;
        }
        case WM_RBUTTONUP: {
            lcs::input::InputSystem::instance().queue_joy_button_up(0, 5);
            return 0;
        }
        case WM_MOUSEWHEEL: {
            short delta = GET_WHEEL_DELTA_WPARAM(wparam);
            if (delta > 0) {
                // Next weapon / menu scroll up (D-Pad Right - button 13)
                lcs::input::InputSystem::instance().queue_joy_button_down(0, 13);
                lcs::input::InputSystem::instance().queue_joy_button_up(0, 13);
            } else if (delta < 0) {
                // Prev weapon / menu scroll down (D-Pad Left - button 12)
                lcs::input::InputSystem::instance().queue_joy_button_down(0, 12);
                lcs::input::InputSystem::instance().queue_joy_button_up(0, 12);
            }
            return 0;
        }
        case WM_MOUSEMOVE: {
            int x = static_cast<short>(LOWORD(lparam));
            int y = static_cast<short>(HIWORD(lparam));
            float nx = (g_win.width > 0) ? static_cast<float>(x) / static_cast<float>(g_win.width) : 0.0f;
            float ny = (g_win.height > 0) ? static_cast<float>(y) / static_cast<float>(g_win.height) : 0.0f;
            if (wparam & MK_LBUTTON) {
                lcs::input::InputSystem::instance().queue_touch_move(0, nx, ny);
            }
            // Mouse camera aim look: Right stick (Axis 2 = X, Axis 3 = Y)
            static int s_last_mouse_x = -1;
            static int s_last_mouse_y = -1;
            if (s_last_mouse_x >= 0 && s_last_mouse_y >= 0) {
                int dx = x - s_last_mouse_x;
                int dy = y - s_last_mouse_y;
                float cam_x = std::clamp(static_cast<float>(dx) * 0.05f, -1.0f, 1.0f);
                float cam_y = std::clamp(static_cast<float>(dy) * 0.05f, -1.0f, 1.0f);
                if (std::abs(cam_x) > 0.01f || std::abs(cam_y) > 0.01f) {
                    lcs::input::InputSystem::instance().queue_joy_axis(0, 2, cam_x);
                    lcs::input::InputSystem::instance().queue_joy_axis(0, 3, cam_y);
                }
            }
            s_last_mouse_x = x;
            s_last_mouse_y = y;
            return 0;
        }
        case WM_KEYDOWN: {
            if (lparam & (1 << 30)) return 0; // Ignore autorepeat
            switch (wparam) {
                case 'W':
                    s_key_w = true;
                    update_keyboard_movement_axes();
                    lcs::input::InputSystem::instance().queue_joy_button_down(0, 10); // Up
                    break;
                case 'S':
                    s_key_s = true;
                    update_keyboard_movement_axes();
                    lcs::input::InputSystem::instance().queue_joy_button_down(0, 11); // Down
                    break;
                case 'A':
                    s_key_a = true;
                    update_keyboard_movement_axes();
                    lcs::input::InputSystem::instance().queue_joy_button_down(0, 12); // Left
                    break;
                case 'D':
                    s_key_d = true;
                    update_keyboard_movement_axes();
                    lcs::input::InputSystem::instance().queue_joy_button_down(0, 13); // Right
                    break;
                case VK_UP:
                    s_key_up = true;
                    update_keyboard_movement_axes();
                    lcs::input::InputSystem::instance().queue_joy_button_down(0, 10);
                    break;
                case VK_DOWN:
                    s_key_down = true;
                    update_keyboard_movement_axes();
                    lcs::input::InputSystem::instance().queue_joy_button_down(0, 11);
                    break;
                case VK_LEFT:
                    s_key_left = true;
                    update_keyboard_movement_axes();
                    lcs::input::InputSystem::instance().queue_joy_button_down(0, 12);
                    break;
                case VK_RIGHT:
                    s_key_right = true;
                    update_keyboard_movement_axes();
                    lcs::input::InputSystem::instance().queue_joy_button_down(0, 13);
                    break;
                case VK_RETURN:
                case VK_SPACE:
                    // Menu select / Sprint / Accelerate (Cross / A - Button 0)
                    lcs::input::InputSystem::instance().queue_joy_button_down(0, 0);
                    break;
                case VK_ESCAPE:
                    lcs::input::InputSystem::instance().queue_back_button();
                    lcs::input::InputSystem::instance().queue_joy_button_down(0, 7); // Start
                    break;
                case VK_BACK:
                    lcs::input::InputSystem::instance().queue_joy_button_down(0, 1); // Circle / B
                    break;
                case VK_TAB:
                case 'M':
                    lcs::input::InputSystem::instance().queue_joy_button_down(0, 7); // Start / Map
                    break;
                case VK_SHIFT:
                    lcs::input::InputSystem::instance().queue_joy_button_down(0, 0); // Sprint (Cross)
                    break;
                case VK_CONTROL:
                    lcs::input::InputSystem::instance().queue_joy_button_down(0, 2); // Jump / Brake (Square)
                    break;
                case 'F':
                case 'E':
                    lcs::input::InputSystem::instance().queue_joy_button_down(0, 3); // Enter/Exit Vehicle (Triangle)
                    break;
                case 'C':
                    lcs::input::InputSystem::instance().queue_joy_button_down(0, 9); // Look Behind / Crouch (R3)
                    break;
                case 'R':
                    lcs::input::InputSystem::instance().queue_joy_button_down(0, 1); // Attack (Circle)
                    break;
                default:
                    break;
            }
            return 0;
        }
        case WM_KEYUP: {
            switch (wparam) {
                case 'W':
                    s_key_w = false;
                    update_keyboard_movement_axes();
                    lcs::input::InputSystem::instance().queue_joy_button_up(0, 10);
                    break;
                case 'S':
                    s_key_s = false;
                    update_keyboard_movement_axes();
                    lcs::input::InputSystem::instance().queue_joy_button_up(0, 11);
                    break;
                case 'A':
                    s_key_a = false;
                    update_keyboard_movement_axes();
                    lcs::input::InputSystem::instance().queue_joy_button_up(0, 12);
                    break;
                case 'D':
                    s_key_d = false;
                    update_keyboard_movement_axes();
                    lcs::input::InputSystem::instance().queue_joy_button_up(0, 13);
                    break;
                case VK_UP:
                    s_key_up = false;
                    update_keyboard_movement_axes();
                    lcs::input::InputSystem::instance().queue_joy_button_up(0, 10);
                    break;
                case VK_DOWN:
                    s_key_down = false;
                    update_keyboard_movement_axes();
                    lcs::input::InputSystem::instance().queue_joy_button_up(0, 11);
                    break;
                case VK_LEFT:
                    s_key_left = false;
                    update_keyboard_movement_axes();
                    lcs::input::InputSystem::instance().queue_joy_button_up(0, 12);
                    break;
                case VK_RIGHT:
                    s_key_right = false;
                    update_keyboard_movement_axes();
                    lcs::input::InputSystem::instance().queue_joy_button_up(0, 13);
                    break;
                case VK_RETURN:
                case VK_SPACE:
                    lcs::input::InputSystem::instance().queue_joy_button_up(0, 0);
                    break;
                case VK_BACK:
                    lcs::input::InputSystem::instance().queue_joy_button_up(0, 1);
                    break;
                case VK_TAB:
                case 'M':
                    lcs::input::InputSystem::instance().queue_joy_button_up(0, 7);
                    break;
                case VK_SHIFT:
                    lcs::input::InputSystem::instance().queue_joy_button_up(0, 0);
                    break;
                case VK_CONTROL:
                    lcs::input::InputSystem::instance().queue_joy_button_up(0, 2);
                    break;
                case 'F':
                case 'E':
                    lcs::input::InputSystem::instance().queue_joy_button_up(0, 3);
                    break;
                case 'C':
                    lcs::input::InputSystem::instance().queue_joy_button_up(0, 9);
                    break;
                case 'R':
                    lcs::input::InputSystem::instance().queue_joy_button_up(0, 1);
                    break;
                default:
                    break;
            }
            return 0;
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

bool create_game_window(int width, int height, const char* title) {
#if defined(LCS_PLATFORM_WINDOWS)
    if (g_win.hwnd) return true;

    // Enable DPI awareness so the swapchain matches native desktop pixels.
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

    RECT client{};
    GetClientRect(g_win.hwnd, &client);
    g_win.width = static_cast<uint32_t>(client.right - client.left);
    g_win.height = static_cast<uint32_t>(client.bottom - client.top);
    g_win.open = true;

    ShowWindow(g_win.hwnd, SW_SHOW);
    UpdateWindow(g_win.hwnd);
    SetForegroundWindow(g_win.hwnd);
    SetFocus(g_win.hwnd);

    std::cout << "[Window] Created native Win32 window (" << g_win.width << "x" << g_win.height << ")\n";
    return true;
#else
    (void)title;
    g_win.width = static_cast<uint32_t>(width);
    g_win.height = static_cast<uint32_t>(height);
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

bool poll_window_events() {
#if defined(LCS_PLATFORM_WINDOWS)
    MSG msg{};
    while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
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

} // namespace lcs::graphics
