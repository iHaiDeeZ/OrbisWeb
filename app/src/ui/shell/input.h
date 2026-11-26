// WinePS4's input for the desktop: the controller (pad, touch pad) and a USB mouse, as one pointer
// and a few buttons. Keyboard text comes from the system keyboard for now.
#pragma once

#include <cstdint>

namespace shell {

enum Button : uint32_t {
    BTN_CLICK = 1 << 0,    // left button: Cross, R2, mouse left
    BTN_MENU = 1 << 1,     // right button: Square, mouse right
    BTN_BACK = 1 << 2,     // Circle: close a menu, go up a folder
    BTN_START = 1 << 3,    // OPTIONS: the Start menu
    BTN_SWITCH = 1 << 4,   // Triangle: the next window
    BTN_PREV = 1 << 5,     // L1
    BTN_NEXT = 1 << 6,     // R1
    BTN_UP = 1 << 7,       // D-pad
    BTN_DOWN = 1 << 8,
    BTN_LEFT = 1 << 9,
    BTN_RIGHT = 1 << 10,
    // the mouse's own buttons (its right button is BTN_MENU as well, for the lists that take Square)
    BTN_MOUSE_RIGHT = 1 << 11,
    BTN_MOUSE_MIDDLE = 1 << 12,
    BTN_MOUSE_BACK = 1 << 13,      // the side buttons
    BTN_MOUSE_FORWARD = 1 << 14,
};

struct Input {
    float x = 960, y = 540;     // the pointer, in screen pixels
    bool moved = false;         // the pointer moved since the last poll
    uint32_t held = 0;          // Button bits held now
    uint32_t pressed = 0;       // pressed since the last poll
    uint32_t released = 0;      // released since the last poll
    uint32_t repeat = 0;        // D-pad and the pressed keys again while held (lists)
    float scroll = 0;           // lines (positive: down), from the right stick and the wheel
    float scroll_x = 0;         // columns (positive: right), from the wheel's tilt
    bool mouse = false;         // a mouse is connected (the pointer follows it)
    bool pad = false;           // a controller is connected
};

void input_open();
// Once per frame, dt in seconds
const Input &input_poll(float dt);
// The mouse's speed, 1-20 (Settings > Bluetooth & devices; 10 by default)
void input_set_pointer_speed(int speed);
// Puts the pointer at (x, y): the D-pad steps it through a menu's items
void input_warp(float x, float y);

// The controller as input_poll read it (OrbisWeb v00.30: a page's gamepad). No Share or PS button:
// those stay the system's. Its button bits, as libScePad's (orbis/_types/pad.h):
enum PadButton : uint32_t {
    PAD_L3 = 0x0002, PAD_R3 = 0x0004, PAD_OPTIONS = 0x0008,
    PAD_UP = 0x0010, PAD_RIGHT = 0x0020, PAD_DOWN = 0x0040, PAD_LEFT = 0x0080,
    PAD_L2 = 0x0100, PAD_R2 = 0x0200, PAD_L1 = 0x0400, PAD_R1 = 0x0800,
    PAD_TRIANGLE = 0x1000, PAD_CIRCLE = 0x2000, PAD_CROSS = 0x4000, PAD_SQUARE = 0x8000,
    PAD_TOUCH_PAD = 0x100000,
};
struct PadState {
    bool connected = false;
    uint32_t buttons = 0;                             // ORBIS_PAD_BUTTON_* bits
    uint8_t lx = 128, ly = 128, rx = 128, ry = 128;   // the sticks, 0-255 (128 the middle, up and left low)
    uint8_t l2 = 0, r2 = 0;                           // how far the triggers are pressed, 0-255
};
const PadState &input_pad();
// The controller's motors, 0-255 each (0, 0: still)
void input_pad_vibrate(uint8_t large, uint8_t small);
// Whether the controller moves the pointer and presses the buttons of Input (true, at start). False while
// a page has it as a gamepad: input_pad() still reads it, and a mouse still moves the pointer.
void input_pad_to_ui(bool on);

} // namespace shell
