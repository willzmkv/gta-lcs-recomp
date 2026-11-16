#include "window.h"

#include <iostream>

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
};

WindowState g_win;

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
                    if (g_win.on_resize) {
                        g_win.on_resize(new_w, new_h);
                    }
                }
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

bool is_window_open() {
    return g_win.open;
}

} // namespace lcs::graphics
