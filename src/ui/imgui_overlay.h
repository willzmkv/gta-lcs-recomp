// Dear ImGui Overlay Manager for GTA: Liberty City Stories (PC)
// Integrates F2 Settings, F5 Trainer, FPS Counter, and Win32 input forwarding into D3D12/Vulkan.
#pragma once

#include <cstdint>

struct ID3D12Device;
struct ID3D12CommandQueue;
struct ID3D12GraphicsCommandList;

namespace lcs::ui_overlay {

bool init_d3d12(void* hwnd, ID3D12Device* device, ID3D12CommandQueue* command_queue, uint32_t num_frames_in_flight);
void shutdown();

// Returns true if ImGui consumed the Win32 message (or an in-game menu is open and blocking game input).
bool handle_win32_message(void* hwnd, uint32_t msg, uintptr_t wparam, intptr_t lparam);

// True while F2 Settings or F5 Trainer is open (blocks gameplay keyboard/mouse/gamepad input).
bool is_blocking_game_input();

// True if any ImGui element (F2, F5, notices, FPS counter) needs to be drawn this frame.
bool has_visible_overlay();

// Called right before Transition->PRESENT in D3D12 end_d3d12_frame_and_present().
void render_d3d12(ID3D12GraphicsCommandList* cmd_list);

} // namespace lcs::ui_overlay
