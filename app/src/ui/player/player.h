// Wisp Player (ui/player/player.cpp): video and music files from Files or a finished download, in a tab
// of their own, played with FFmpeg (the decoders behind MPC-HC's LAV Filters) on the console's CPU, with
// MPC-HC's keys where a console has them.
//
// Decoding runs on a thread of its own (FFmpeg's decoders use 4 more), which also turns each picture
// into the screen's format at the size it is shown; the sound goes to the PS4's audio out on a thread
// of its own, and its clock is the one pictures follow (a late picture is skipped, never waited for).
// Subtitles: the file's own (text and pictures), a .srt / .ass / .vtt file of the same name beside it,
// or any file chosen (Load subtitle file: folders, USB drives, and zip / rar / 7z archives of them),
// read in any encoding (UTF-8, UTF-16, Arabic Windows-1256 and the other code pages: Automatic tells
// UTF-8 from Arabic and Western), with a delay, a size, a position and a box; Find subtitles online opens
// a subtitles site (the server chosen) searched for the video's name. Where a file was left is kept
// (/data/Wisp/player/resume.tsv) and played from there next time; the recent files too.
//
// MPC-HC's menu (Square or the mouse's right button): Play / Pause, Stop, Open file, Recent files;
// Navigate (Go to a time, Chapters, the folder's files); Play (Speed 0.25x-4x, When it ends: the next
// file, again, stop, close); Audio (tracks, delay, a boost to 400%, mute); Subtitles (tracks, Load
// subtitle file, Find subtitles online, Encoding, Delay, Size, Position, a box); Video (Aspect ratio
// 4:3 ... 2.39:1, Video frame: fit / fill / stretch / original size, Zoom, Brightness, Contrast,
// Saturation); Save image (Downloads/Screenshots); Information (MPC-HC's Statistics); Full screen.
//
// The controller: Cross pause / play, D-pad left / right 10 s back / on, D-pad up / down volume,
// Square the menu, Triangle the next subtitles (or none), L1 / R1 the browser's tabs, Circle closes the
// player. A USB keyboard: Space pause, Left / Right 5 s (, and . a minute), Up / Down volume, M mute,
// F (or F11, or a double click) full screen, A audio track, S subtitles, R aspect ratio (Shift+R the
// frame), + / - audio delay, F1 / F2 subtitles delay, numpad + / - / 5 zoom, Tab information, Ctrl+O
// open, Ctrl+L load subtitles, Ctrl+G go to, Ctrl+S save image, Ctrl+Up / Down / R speed, Home the start,
// Page Up / Page Down the folder's previous / next file, the menu key the menu, Esc closes. A mouse: a
// click pauses, a click on the bar seeks there, the wheel is the volume, the right button the menu.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "../shell/hwkeyboard.h"
#include "../shell/input.h"

namespace wisp::player {

struct Host {
    std::function<void(const std::string &text)> toast;
    std::function<void()> redraw;              // a new picture or a change of the bar: compose soon
    std::function<void(bool on)> fullscreen;   // the browser's full screen (no toolbar)
    std::function<bool()> is_fullscreen;
    std::function<void()> closed;              // the player closed itself (Circle, Esc, the end of a list)
    // text typed (Go to): the keyboard with title and initial; done(ok, text)
    std::function<void(const std::string &title, const std::string &initial,
        std::function<void(bool, const std::string &)> done)> ask_text;
    std::function<void(const std::string &url)> open_tab;   // a page in a tab of its own (subtitles sites)
};

void set_host(Host host);
// A file the player takes (by its extension: video and audio)
bool can_play(const std::string &path);
// Plays path (from where it was left); false and a toast when it cannot be opened
bool open(const std::string &path);
// Its tab shown / hidden (hidden: a video pauses, music goes on)
void show();
void hide();
bool visible();
// Stops and lets everything go (its tab closed)
void close();
// One frame of input while visible (the area is the page's); true: used
bool input(const shell::Input &in, const std::vector<shell::KeyEvent> &keys, int x, int y, int w, int h, float dt);
// A new picture or a change to draw
bool dirty();
// Paints the area [x, x+w) x [y, y+h) of the screen picture (BGRA, stride pixels a row): the video,
// letterboxed, its subtitles, and the bar while it shows
void paint(uint32_t *bgra, int stride, int x, int y, int w, int h);
// The file playing ("" none) and its name for the tab
const std::string &path();
std::string title();

} // namespace wisp::player
