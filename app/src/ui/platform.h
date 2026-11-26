// Wisp: WPE's platform on the PS4 (WPEPlatform API: display, view, toplevel, screen, keymap, input
// method). The web process renders each page into shared-memory buffers (WebKit's SharedMemory
// swapchain: this display offers no DMA-BUF formats); a view keeps the newest one until the UI has
// shown it, and the UI draws it under its own toolbar on the CPU (ui/browser.cpp).
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

typedef struct _WPEDisplay WPEDisplay;
typedef struct _WPEView WPEView;

namespace wisp {

// The display, made once (and set as WPE's primary display)
WPEDisplay *platform_display();

// A view's newest frame: BGRA, premultiplied (WebKit's ARGB8888 in memory order)
struct Frame {
    const uint8_t *pixels = nullptr;
    int width = 0, height = 0, stride = 0;
    uint64_t serial = 0;   // grows with every new frame
};
// The frame to show for view; false if it has none yet
bool platform_frame(WPEView *view, Frame &frame);
// The rows [y0, y1) of view's frame that changed since the last call (false: none did); a new size
// counts as all of them
bool platform_take_damage(WPEView *view, int &y0, int &y1);
// The UI has shown view's newest frame: WebKit may draw the next one (frame pacing)
void platform_frame_shown(WPEView *view);
// A saved picture as view's frame until its page draws (a tab woken from sleep: the picture it had)
void platform_show_picture(WPEView *view, std::vector<uint8_t> &&pixels, int width, int height, int stride);

// The area the page gets on the screen (each view's toplevel is resized to it)
void platform_set_content_size(WPEView *view, int width, int height);
// The tab on show (active, focused, visible) or in the background
void platform_set_active(WPEView *view, bool active);
// (v00.36) A view's toplevel out of fullscreen (WebKit then takes its page out of its own fullscreen)
void platform_leave_fullscreen(WPEView *view);

// The PS4 keyboard for a text field: the input method calls this when a field gains focus; the UI
// opens the system keyboard and calls the callback with the text (or nothing when cancelled)
using TextRequest = std::function<void(const std::string &initial, bool password, bool url,
    std::function<void(bool done, const std::string &text)> finished)>;
void platform_set_text_request(TextRequest request);

// Something changed that the UI should redraw (a new frame, a cursor change)
void platform_set_redraw(std::function<void()> redraw);

// The pointer's shape the page asks for ("default", "pointer", "text", ...)
const std::string &platform_cursor_name();

// The USB keyboard's modifiers held now (WPEModifiers), for WebKit's asking the keymap
void platform_set_modifiers(uint32_t modifiers);

// The clipboard WebKit copies to and pastes from (WPE's own, in this process): text
std::string platform_clipboard_text();
void platform_set_clipboard_text(const std::string &text);

// Gamepads (v00.30): the controller as a page's gamepad, the Gamepad API's standard mapping, named as a
// PC browser names a DualShock 4 (settings: gamepad_name). WebKit asks the display for a gamepad manager
// when a page starts reading gamepads and lets it go when no page does: in_use(true / false). The UI
// gives the controller's state every frame (a neutral one while the browser has the controller); a
// page's vibration comes to rumble (strong and weak motors 0-1, for ms milliseconds; 0, 0, 0: stop).
struct GamepadHooks {
    std::function<void(bool in_use)> in_use;
    std::function<void(double strong, double weak, unsigned ms)> rumble;
};
void platform_set_gamepad_hooks(GamepadHooks hooks);
// Buttons 0-16 from 0 to 1: 0 Cross, 1 Circle, 2 Square, 3 Triangle, 4 L1, 5 R1, 6 L2, 7 R2 (analog),
// 8 Select (the touch pad's button), 9 Start (OPTIONS), 10 L3, 11 R3, 12-15 the D-pad's up, down,
// left, right, 16 Home (the PS button is the system's: never). Axes from -1 to 1: 0 and 1 the left
// stick's x and y, 2 and 3 the right stick's (up and left negative).
struct GamepadState {
    bool connected = false;
    double buttons[17] = {};
    double axes[4] = {};
};
void platform_gamepad_update(const GamepadState &state);
// Whether a page reads gamepads now
bool platform_gamepads_in_use();
// (v00.41) Says in ui.log whether WebKit finds a page for the controller's input (the tab's view expected)
void platform_gamepad_check(WPEView *expected);

} // namespace wisp
