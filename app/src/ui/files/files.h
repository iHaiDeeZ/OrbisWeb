// Wisp's Files (ui/files/files.cpp): the console's files in the browser - Downloads, Wisp's own data,
// /data, USB drives, the app's read-only files and the Recycle Bin. A list with Name, Date modified,
// Type and Size (folders first, sorted by a column's header), the folder's parts in an address bar, a
// pane of places, a command bar and a menu for what's under the pointer. Copies, moves, deletes and
// archives' jobs run on a thread of their own (shell/fileops.h), their progress on a card with Cancel.
// Delete moves things to the Recycle Bin (Restore puts them back); Shift+Delete, "Delete for good" and
// anything with no bin ask first and delete for good. Archives (files/archives.h) open as folders, as
// Windows opens a compressed folder: Extract All, Extract here, Compress to ZIP / 7z.
//
// The controller (the browser draws the pointer):
//   Cross               clicks; twice on an entry opens it: a folder here, an archive as a folder, a
//                       file through open_file (one inside an archive taken out first)
//   Square              the menu for what's under the pointer (Open, Extract All / Extract here, Cut,
//                       Copy, Paste into folder, Compress to ZIP / 7z, Rename, Delete, Delete for good,
//                       Properties; on empty space Sort by, Refresh, Paste, New folder, New text
//                       document, Select all / none, Invert selection, Properties; in the Recycle Bin
//                       Restore, Delete for good, Restore all items, Empty Recycle Bin)
//   Circle              closes the dialog or the menu, else ends typing in place, else clears a
//                       search, else goes up a folder; at This PS4 (the list of places: the top) it
//                       closes Files
//   Triangle            the address bar: a folder to go to, typed on the PS4's keyboard
//   D-pad up / down     moves the selection and the pointer with it (Cross twice then opens it); in
//                       the menu or a dialog, steps through its items
//   D-pad left / right  Back and Forward
//   right stick, wheel  scroll the list
//   OPTIONS, L1, R1     never used by Files: left to the browser
// A USB keyboard works as in Explorer: arrows, Page Up/Down, Home, End, Enter, Backspace (Back),
// Delete, F2 (rename), F3 or Ctrl+F (search), F5, Escape, Ctrl+A/C/X/V, Ctrl+Shift+N (new folder),
// Ctrl+L or Alt+D (address), Alt+Left/Right/Up, the menu key, Ctrl+W or Alt+F4 (closes Files); letters
// jump to names. While one is connected, names, the address and searches are typed in place instead
// of on the PS4's keyboard.
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "../shell/hwkeyboard.h"
#include "../shell/input.h"
#include "../shell/ps4_canvas.h"

namespace wisp::files {

struct Host {
    // the browser opens a file the user activated (Cross twice / Enter / "Open"): a tab, or an extension install
    std::function<void(const std::string &path)> open_file;
    // the PS4 keyboard: title, starting text, then done(true, text) or done(false, "") later
    std::function<void(const std::string &title, const std::string &initial,
                       std::function<void(bool, const std::string &)> done)> ask_text;
    std::function<void(const std::string &text)> toast;
};

void set_host(Host host);
// Shows Files at folder (Downloads when empty). A file's path shows its folder with it selected; a
// folder that isn't there shows Downloads (and says so). Called while Files is up, it goes there (Back
// returns).
void open(const std::string &folder);
void close();
// Files in a tab: hide() when another tab is shown, show() when its tab is again (it comes back where
// it was: folder, Back/Forward, selection; the folder read again)
void hide();
void show();
// Files closes itself too (Circle at This PS4, its Close button, Ctrl+W): check this after input()
bool visible();
// one frame of input while visible; the area is x, y, w, h in screen pixels (the browser gives
// everything under its toolbar). Returns true when Files used the input (the page must not get it):
// what it acted on (Cross, Square, Circle, Triangle, the D-pad, scrolling, every key of a USB
// keyboard), and anything of the pointer's over its area. False when all of it was elsewhere (a
// click on the toolbar, OPTIONS, L1, R1), for the browser.
bool input(const shell::Input &in, const std::vector<shell::KeyEvent> &keys, int x, int y, int w, int h, float dt);
// whether it must be drawn again (something changed, or a job is running: its progress 4 times a
// second). False while Files is closed; called then too, a copy that ends meanwhile gets its toast at
// once (else when Files opens again).
bool dirty();
// draws Files into canvas (canvas is w x h; draw at 0,0)
void draw(ps4ui::Canvas &canvas);

} // namespace wisp::files
