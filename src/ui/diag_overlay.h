// F3 diagnostics panel (as VCS Recomp's F3 overlay): frame-time statistics and graph, texture
// pipeline and HD World counters, and the game's state (position, heading, cell, area, player,
// weapons, vehicle, clock, pool use). Ctrl+F3 copies "x, y, z, heading" to the clipboard and
// writes the same line to the log, so a spot can be reported or reproduced exactly.
#pragma once

namespace lcs::ui_diag {

bool is_open();
void toggle();
// Copies the player's position line to the clipboard (and stdout). `hwnd` is the game window.
void copy_position(void* hwnd);
// Records the frame time every frame (also while closed, so the graph is full when opened).
void record_frame(float delta_seconds);
void draw();

} // namespace lcs::ui_diag
