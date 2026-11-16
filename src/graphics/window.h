#pragma once

#include <cstdint>
#include <functional>

namespace lcs::graphics {

using ResizeCallback = std::function<void(uint32_t width, uint32_t height)>;

// Creates the native desktop window with the specified client area dimensions.
bool create_game_window(int width, int height, const char* title);

// Destroys the native window and unregisters its window class.
void destroy_game_window();

// Processes pending OS window messages. Returns false when the user closes the window.
bool poll_window_events();

// Returns the platform-native window handle (HWND on Windows).
void* get_native_window_handle();

// Retrieves the current client area size in pixels.
void get_window_size(uint32_t& width, uint32_t& height);

// Registers a callback invoked when the client area is resized (excluding minimized state).
void set_window_resize_callback(ResizeCallback cb);

// Updates the window title bar text.
void set_window_title(const char* title);

// Retrieves pending resize dimensions if a window resize occurred, clearing the pending flag.
bool get_pending_resize(uint32_t& out_w, uint32_t& out_h);

// Returns true if the window is currently alive and not closed.
bool is_window_open();

} // namespace lcs::graphics
