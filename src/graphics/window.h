#pragma once

#include <cstdint>
#include <functional>

namespace lcs::graphics {

using ResizeCallback = std::function<void(uint32_t width, uint32_t height)>;
using FullscreenCallback = std::function<void(bool fullscreen)>;

// Creates the native desktop window with the specified client area dimensions.
bool create_game_window(int width, int height, const char* title, bool fullscreen = false);

// Destroys the native window and unregisters its window class.
void destroy_game_window();

// Processes pending OS window messages. Returns false when the user closes the window.
bool poll_window_events();

// Non-blocking message pump for long synchronous loads so Windows DWM does not mark the window hung.
bool pump_loading_events();

// Returns the platform-native window handle (HWND on Windows).
void* get_native_window_handle();

// Retrieves the current client area size in pixels.
void get_window_size(uint32_t& width, uint32_t& height);

// Resizes the windowed client area (or updates the saved windowed placement if fullscreen).
void set_window_size(uint32_t width, uint32_t height);

// Retrieves the primary/current monitor resolution in physical pixels.
void get_desktop_resolution(uint32_t& width, uint32_t& height);

// Registers a callback invoked when the client area is resized (excluding minimized state).
void set_window_resize_callback(ResizeCallback cb);

// Updates the window title bar text.
void set_window_title(const char* title);

// Retrieves pending resize dimensions if a window resize occurred, clearing the pending flag.
bool get_pending_resize(uint32_t& out_w, uint32_t& out_h);

// Returns true if the window is currently alive and not closed.
bool is_window_open();

// Switches between borderless fullscreen and windowed mode, invoking any registered callback.
void set_fullscreen(bool fullscreen);
bool is_fullscreen();
void toggle_fullscreen();
void set_fullscreen_callback(FullscreenCallback cb);

// Sets whether the game is currently showing a menu/pause screen (unlocking the mouse cursor)
void set_menu_active(bool in_menu);
void set_gameplay_context(bool in_menu, bool in_vehicle, bool in_cutscene);
void sync_menu_cursor_visibility();
bool is_menu_active();

// Captures the current window client area to an uncompressed 24/32-bpp BMP file.
bool capture_window_bmp(const char* filepath);

} // namespace lcs::graphics
