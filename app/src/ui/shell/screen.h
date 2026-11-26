// Wisp: the frames on the TV (from WinePS4 shell/screen.cpp), in BGRA as WebKit draws them. Two linear 1920x1080 buffers in VideoOut's
// app slots 12-13 (ps4-kit keeps 0-11 for Vulkan swapchains: DXVK's, later), flipped at vsync.
// From HENkaku's launcher (ps4_launcher.cpp: setup_buffers, wait_flipped, present).
#pragma once

#include <cstdint>

namespace shell {

constexpr int SCREEN_W = 1920;
constexpr int SCREEN_H = 1080;

// Opens the display; false (logged) if it can't
bool screen_open();
// The TV's vertical blank: callback (on a thread of its own, at every vblank) from now on; false
// (logged) without vblank events, and the caller paces itself
bool screen_start_vblank(void (*callback)());
// The last flip is not on the screen yet: the back buffer is still to be shown, don't write it
bool screen_flip_pending();
// The buffer to draw the next frame into (RGBA, SCREEN_W per row): waits while a flip is pending
// (callers that check screen_flip_pending first never wait)
uint32_t *screen_back_buffer();
// Which of the two that is (0 or 1), for what the caller keeps of each
int screen_back_index();
// Shows it at the next vsync
void screen_flip();

} // namespace shell
