// Wisp: the browser (ui/browser.cpp): tabs of WPE web views under a toolbar drawn on the CPU, the
// controller as a pointer, the PS4 keyboard for text.
#pragma once

namespace wisp {

// Runs the browser until the user leaves the app (GLib's main loop: WebKit's UI side runs on it)
void browser_run();

} // namespace wisp
