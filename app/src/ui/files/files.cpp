// Wisp's Files (files.h), from WinePS4's Files window (WinePS4 shell/desktop.cpp).
//
// Drawn on the CPU with HENkaku's canvas into the area the browser gives it (everything under its
// toolbar), as one window filling it: a title bar with Close, the command bar, the address row (Back,
// Forward, Up, Refresh, the folder's parts, the search box), the pane of places, the list with its
// header, the status bar. The browser draws the pointer. Copies, moves and deletes are fileops'
// (a thread of their own, shell/fileops.cpp); names, the address and searches are typed on the PS4's
// keyboard (Host::ask_text), or in place on a USB keyboard.
#include "files.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <dirent.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include "../../common/wisp.h"
#include "../shell/fileops.h"
#include "archives.h"

namespace wisp::files {
namespace {

using ps4ui::Canvas;
using ps4ui::Color;
using ps4ui::Image;
using ps4ui::rgb;
using ps4ui::Weight;
using shell::KeyEvent;

// --- the look ------------------------------------------------------------------------------------

constexpr int TITLE_H = 40;  // the title bar: the folder's name, Close
constexpr int RADIUS = 8;    // panels' corners
constexpr int SHADOW = 28;   // how far a shadow reaches

// the dark theme
constexpr Color TEXT = rgb(0xffffff), TEXT2 = rgb(0xc8c8c8), TEXT3 = rgb(0x8a8a8a);
constexpr Color ACCENT = rgb(0x4cc2ff), ACCENT_HOT = rgb(0x6dd0ff), ACCENT_BORDER = rgb(0x3aa6e0), ACCENT_TEXT = rgb(0x000000);
constexpr Color WINDOW_BG = rgb(0x202020), CONTENT_BG = rgb(0x1b1b1b), BORDER = rgb(0x3a3a3a), BORDER_ACTIVE = rgb(0x4a4a4a);
constexpr Color CONTROL = rgb(0x2d2d2d), HOVER = rgb(0xffffff, 0x12), SELECTED = rgb(0xffffff, 0x1c), LINE = rgb(0xffffff, 0x16);
constexpr Color DISABLED = rgb(0x6e6e6e), SCROLLBAR = rgb(0xffffff, 0x50), TRACK = rgb(0xffffff, 0x20), ICON_HOLE = rgb(0x202020);
constexpr Color POPUP_BG = rgb(0x2c2c2c), POPUP_BORDER = rgb(0x454545), CARD = rgb(0x262626);
constexpr Color DIALOG_BG = rgb(0x2b2b2b), DIALOG_FOOT = rgb(0x202020);
constexpr Color BUTTON = rgb(0x333333), BUTTON_HOT = rgb(0x3d3d3d), BUTTON_BORDER = rgb(0x454545), CLOSE_RED = rgb(0xc42b1c);

// --- small things --------------------------------------------------------------------------------

double now_seconds() {
    timespec t{};
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

std::string base_name(const std::string &path) {
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string folder_of(const std::string &path) {
    const size_t slash = path.find_last_of('/');
    return slash == 0 || slash == std::string::npos ? "/" : path.substr(0, slash);
}

// dir/name (the root's names have one slash)
std::string join(const std::string &dir, const std::string &name) { return dir == "/" ? "/" + name : dir + "/" + name; }

std::string lower(std::string s) {
    for (char &c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// (suffix in lower case; s in any)
bool ends_with(const std::string &s, const char *suffix) {
    const size_t n = std::strlen(suffix);
    return s.size() >= n && lower(s.substr(s.size() - n)) == suffix;
}

bool is_dir(const std::string &path) {
    struct stat st{};
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool is_file(const std::string &path) {
    struct stat st{};
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

// path is root, or inside it
bool within(const std::string &path, const std::string &root) {
    return path.rfind(root, 0) == 0 && (path.size() == root.size() || root == "/" || path[root.size()] == '/');
}

// An archive opened as a folder: its path is the archive file's, and its folders' paths go on from it
// ("/data/Downloads/photos.zip/2024/june"). True for such a path: the archive file and the path inside
// ("" its top). (The last answer is kept: this is asked for every frame's buttons.)
bool in_archive(const std::string &path, std::string &file, std::string &inside) {
    static std::string last_path, last_file, last_inside;
    static bool last = false;
    if (path.empty() || path[0] != '/' || !archive::available())
        return false;
    if (path != last_path) {
        last_path = path;
        last = false;
        for (size_t at = path.find('/', 1);; at = path.find('/', at + 1)) {
            const std::string prefix = at == std::string::npos ? path : path.substr(0, at);
            struct stat st{};
            if (stat(prefix.c_str(), &st) != 0)
                break;  // (nothing real there)
            if (S_ISREG(st.st_mode)) {
                if (archive::is_archive_name(base_name(prefix))) {
                    last = true;
                    last_file = prefix;
                    last_inside = at == std::string::npos ? std::string() : path.substr(at + 1);
                }
                break;
            }
            if (at == std::string::npos)
                break;  // (a real folder)
        }
    }
    if (last) {
        file = last_file;
        inside = last_inside;
    }
    return last;
}

bool in_archive(const std::string &path) {
    std::string file, inside;
    return in_archive(path, file, inside);
}

// path is in an archive: a folder or file in one, not the archive file itself. For an entry of a real
// folder (in_archive says yes for an archive's own path too: its top, as a folder). Through v00.21
// open, Extract All, paste and drop asked in_archive of a chosen archive and did nothing.
bool inside_archive(const std::string &path) {
    std::string file, inside;
    return in_archive(path, file, inside) && !inside.empty();
}

// The app's own files (/app0) can't change: nothing is made, moved, renamed or deleted there; nor
// inside an archive (copying out takes files out of it)
bool read_only(const std::string &path) { return !path.empty() && (within(path, WISP_APP) || in_archive(path)); }

bool same_drive(const std::string &a, const std::string &b) {
    struct stat x{}, y{};
    return stat(a.c_str(), &x) == 0 && stat(b.c_str(), &y) == 0 && x.st_dev == y.st_dev;
}

int number_after(const std::string &key, const char *prefix) { return std::atoi(key.c_str() + std::strlen(prefix)); }

// file:///data/My%20files -> /data/My files
std::string from_file_url(const std::string &url) {
    std::string out;
    for (size_t i = 7; i < url.size(); i++) {
        if (url[i] == '%' && i + 2 < url.size() && std::isxdigit(static_cast<unsigned char>(url[i + 1])) &&
            std::isxdigit(static_cast<unsigned char>(url[i + 2]))) {
            out += static_cast<char>(std::strtol(url.substr(i + 1, 2).c_str(), nullptr, 16));
            i += 2;
        } else {
            out += url[i];
        }
    }
    return out;
}

// --- icons (drawn, not pictures) -----------------------------------------------------------------

enum class Icon { Search, Folder, Computer, Drive, Usb, File, Picture, Package, Music, Video, Download, Notepad,
                  Back, Forward, Up, Refresh, Cut, Copy, Paste, Rename, Trash, Plus, Sort, Info, Chevron, Check };

void arc(Canvas &c, float cx, float cy, float r, float from, float to, float t, Color col) {
    const int steps = 24;
    for (int i = 0; i < steps; i++) {
        const float a0 = from + (to - from) * i / steps, a1 = from + (to - from) * (i + 1) / steps;
        c.line(cx + std::cos(a0) * r, cy + std::sin(a0) * r, cx + std::cos(a1) * r, cy + std::sin(a1) * r, t, col);
    }
}

void draw_icon(Canvas &c, Icon icon, float cx, float cy, float s) {
    const int x = static_cast<int>(cx - s / 2), y = static_cast<int>(cy - s / 2), is = static_cast<int>(s);
    switch (icon) {
    case Icon::Search:
        c.ring(cx - s * 0.08f, cy - s * 0.08f, s * 0.27f, s * 0.09f, TEXT);
        c.line(cx + s * 0.13f, cy + s * 0.13f, cx + s * 0.36f, cy + s * 0.36f, s * 0.1f, TEXT);
        break;
    case Icon::Folder:
        c.fill_round_rect(x + is * 6 / 100, y + is * 16 / 100, is * 40 / 100, is * 20 / 100, is * 6 / 100, rgb(0xd29a25));
        c.fill_round_rect(x + is * 6 / 100, y + is * 24 / 100, is * 88 / 100, is * 62 / 100, is * 8 / 100, rgb(0xf2c14b));
        c.fill_rect(x + is * 6 / 100, y + is * 34 / 100, is * 88 / 100, is * 3 / 100, rgb(0xe3ad34));
        break;
    case Icon::Computer:
        c.fill_round_rect(x + is * 6 / 100, y + is * 12 / 100, is * 88 / 100, is * 58 / 100, is * 7 / 100, rgb(0x9aa4b2));
        c.fill_round_rect(x + is * 10 / 100, y + is * 16 / 100, is * 80 / 100, is * 50 / 100, is * 4 / 100, rgb(0x2f7fd6));
        c.fill_rect(x + is * 44 / 100, y + is * 70 / 100, is * 12 / 100, is * 12 / 100, rgb(0x9aa4b2));
        c.fill_round_rect(x + is * 28 / 100, y + is * 82 / 100, is * 44 / 100, is * 8 / 100, is * 4 / 100, rgb(0x9aa4b2));
        break;
    case Icon::Drive:
        c.fill_round_rect(x + is * 6 / 100, y + is * 30 / 100, is * 88 / 100, is * 42 / 100, is * 8 / 100, rgb(0x9aa4b2));
        c.fill_round_rect(x + is * 10 / 100, y + is * 34 / 100, is * 80 / 100, is * 22 / 100, is * 5 / 100, rgb(0xc9d0da));
        c.fill_rect(x + is * 66 / 100, y + is * 60 / 100, is * 18 / 100, is * 5 / 100, rgb(0x52d16b));
        break;
    case Icon::Usb:
        c.fill_round_rect(x + is * 26 / 100, y + is * 30 / 100, is * 48 / 100, is * 62 / 100, is * 8 / 100, rgb(0x6f7b8c));
        c.fill_rect(x + is * 34 / 100, y + is * 8 / 100, is * 32 / 100, is * 24 / 100, rgb(0xc9d0da));
        c.fill_rect(x + is * 40 / 100, y + is * 14 / 100, is * 6 / 100, is * 6 / 100, rgb(0x6f7b8c));
        c.fill_rect(x + is * 54 / 100, y + is * 14 / 100, is * 6 / 100, is * 6 / 100, rgb(0x6f7b8c));
        break;
    case Icon::File:
        c.fill_round_rect(x + is * 20 / 100, y + is * 8 / 100, is * 60 / 100, is * 84 / 100, is * 6 / 100, rgb(0xd5dbe4));
        c.fill_rect(x + is * 30 / 100, y + is * 36 / 100, is * 40 / 100, is * 5 / 100, rgb(0x8f99a8));
        c.fill_rect(x + is * 30 / 100, y + is * 50 / 100, is * 40 / 100, is * 5 / 100, rgb(0x8f99a8));
        c.fill_rect(x + is * 30 / 100, y + is * 64 / 100, is * 26 / 100, is * 5 / 100, rgb(0x8f99a8));
        break;
    case Icon::Picture:  // a framed landscape
        c.fill_round_rect(x + is * 8 / 100, y + is * 16 / 100, is * 84 / 100, is * 68 / 100, is * 8 / 100, rgb(0x5fa8e8));
        c.fill_circle(x + is * 68 / 100, y + is * 34 / 100, is * 8 / 100, rgb(0xffe08a));
        c.line(x + is * 14 / 100, y + is * 76 / 100, x + is * 40 / 100, y + is * 44 / 100, is * 10 / 100, rgb(0x3d8a4f));
        c.line(x + is * 40 / 100, y + is * 44 / 100, x + is * 58 / 100, y + is * 66 / 100, is * 10 / 100, rgb(0x3d8a4f));
        c.line(x + is * 52 / 100, y + is * 72 / 100, x + is * 70 / 100, y + is * 54 / 100, is * 10 / 100, rgb(0x2f6e3e));
        c.line(x + is * 70 / 100, y + is * 54 / 100, x + is * 86 / 100, y + is * 74 / 100, is * 10 / 100, rgb(0x2f6e3e));
        break;
    case Icon::Package:  // a box
        c.fill_round_rect(x + is * 12 / 100, y + is * 22 / 100, is * 76 / 100, is * 64 / 100, is * 8 / 100, rgb(0xb98a52));
        c.fill_rect(x + is * 12 / 100, y + is * 40 / 100, is * 76 / 100, is * 6 / 100, rgb(0x8f6638));
        c.fill_rect(x + is * 44 / 100, y + is * 22 / 100, is * 12 / 100, is * 64 / 100, rgb(0xd9b07a));
        break;
    case Icon::Music:  // a note
        c.fill_circle(cx - s * 0.14f, cy + s * 0.22f, s * 0.14f, rgb(0xe8508a));
        c.line(cx - s * 0.01f, cy + s * 0.22f, cx - s * 0.01f, cy - s * 0.34f, s * 0.08f, rgb(0xe8508a));
        c.line(cx - s * 0.01f, cy - s * 0.34f, cx + s * 0.26f, cy - s * 0.22f, s * 0.08f, rgb(0xe8508a));
        break;
    case Icon::Video:  // a film frame with a play mark
        c.fill_round_rect(x + is * 8 / 100, y + is * 18 / 100, is * 84 / 100, is * 64 / 100, is * 10 / 100, rgb(0x8a5cd6));
        c.line(cx - s * 0.08f, cy - s * 0.16f, cx - s * 0.08f, cy + s * 0.16f, s * 0.07f, rgb(0xffffff));
        c.line(cx - s * 0.08f, cy - s * 0.16f, cx + s * 0.16f, cy, s * 0.07f, rgb(0xffffff));
        c.line(cx - s * 0.08f, cy + s * 0.16f, cx + s * 0.16f, cy, s * 0.07f, rgb(0xffffff));
        break;
    case Icon::Download:  // an arrow down onto a tray
        c.line(cx, cy - s * 0.34f, cx, cy + s * 0.14f, s * 0.1f, ACCENT);
        c.line(cx, cy + s * 0.14f, cx - s * 0.2f, cy - s * 0.06f, s * 0.1f, ACCENT);
        c.line(cx, cy + s * 0.14f, cx + s * 0.2f, cy - s * 0.06f, s * 0.1f, ACCENT);
        c.fill_round_rect(x + is * 14 / 100, y + is * 78 / 100, is * 72 / 100, is * 9 / 100, is * 4 / 100, TEXT2);
        break;
    case Icon::Notepad:  // text: a page with lines and a binding along the top
        c.fill_round_rect(x + is * 18 / 100, y + is * 10 / 100, is * 64 / 100, is * 82 / 100, is * 7 / 100, rgb(0xf4f6f9));
        c.fill_round_rect(x + is * 18 / 100, y + is * 10 / 100, is * 64 / 100, is * 14 / 100, is * 7 / 100, rgb(0x3b8ee8));
        for (int k = 0; k < 4; k++)
            c.fill_rect(x + is * 28 / 100, y + is * (36 + k * 13) / 100, is * 44 / 100, is * 4 / 100, rgb(0x8f99a8));
        break;
    // the bars' buttons: line drawings in the text colour
    case Icon::Back:
    case Icon::Forward: {
        const float d = icon == Icon::Back ? -1.f : 1.f;
        c.line(cx - s * 0.3f * d, cy, cx + s * 0.3f * d, cy, s * 0.08f, TEXT);
        c.line(cx + s * 0.3f * d, cy, cx + s * 0.06f * d, cy - s * 0.24f, s * 0.08f, TEXT);
        c.line(cx + s * 0.3f * d, cy, cx + s * 0.06f * d, cy + s * 0.24f, s * 0.08f, TEXT);
        break;
    }
    case Icon::Up:
        c.line(cx, cy - s * 0.3f, cx, cy + s * 0.3f, s * 0.09f, TEXT);
        c.line(cx, cy - s * 0.3f, cx - s * 0.24f, cy - s * 0.06f, s * 0.09f, TEXT);
        c.line(cx, cy - s * 0.3f, cx + s * 0.24f, cy - s * 0.06f, s * 0.09f, TEXT);
        break;
    case Icon::Refresh:
        arc(c, cx, cy, s * 0.28f, -0.4f, 4.4f, s * 0.08f, TEXT);
        c.line(cx + s * 0.26f, cy - s * 0.1f, cx + s * 0.32f, cy - s * 0.34f, s * 0.08f, TEXT);
        c.line(cx + s * 0.26f, cy - s * 0.1f, cx + s * 0.04f, cy - s * 0.16f, s * 0.08f, TEXT);
        break;
    case Icon::Cut:  // scissors
        c.ring(cx - s * 0.17f, cy + s * 0.22f, s * 0.12f, s * 0.06f, TEXT);
        c.ring(cx + s * 0.17f, cy + s * 0.22f, s * 0.12f, s * 0.06f, TEXT);
        c.line(cx - s * 0.1f, cy + s * 0.12f, cx + s * 0.16f, cy - s * 0.38f, s * 0.06f, TEXT);
        c.line(cx + s * 0.1f, cy + s * 0.12f, cx - s * 0.16f, cy - s * 0.38f, s * 0.06f, TEXT);
        break;
    case Icon::Copy:  // two pages
        c.fill_round_rect(x + is * 30 / 100, y + is * 8 / 100, is * 52 / 100, is * 62 / 100, is * 8 / 100, TEXT);
        c.fill_round_rect(x + is * 35 / 100, y + is * 13 / 100, is * 42 / 100, is * 52 / 100, is * 5 / 100, ICON_HOLE);
        c.fill_round_rect(x + is * 14 / 100, y + is * 28 / 100, is * 52 / 100, is * 62 / 100, is * 8 / 100, TEXT);
        c.fill_round_rect(x + is * 19 / 100, y + is * 33 / 100, is * 42 / 100, is * 52 / 100, is * 5 / 100, ICON_HOLE);
        break;
    case Icon::Paste:  // a clipboard
        c.fill_round_rect(x + is * 16 / 100, y + is * 14 / 100, is * 68 / 100, is * 78 / 100, is * 8 / 100, TEXT);
        c.fill_round_rect(x + is * 21 / 100, y + is * 19 / 100, is * 58 / 100, is * 68 / 100, is * 5 / 100, ICON_HOLE);
        c.fill_round_rect(x + is * 34 / 100, y + is * 6 / 100, is * 32 / 100, is * 16 / 100, is * 5 / 100, TEXT);
        break;
    case Icon::Rename:  // a text box with a cursor
        c.fill_round_rect(x + is * 6 / 100, y + is * 30 / 100, is * 88 / 100, is * 40 / 100, is * 8 / 100, TEXT);
        c.fill_round_rect(x + is * 11 / 100, y + is * 35 / 100, is * 78 / 100, is * 30 / 100, is * 5 / 100, ICON_HOLE);
        c.line(cx + s * 0.12f, cy - s * 0.34f, cx + s * 0.12f, cy + s * 0.34f, s * 0.06f, TEXT);
        c.fill_rect(x + is * 20 / 100, y + is * 47 / 100, is * 22 / 100, is * 6 / 100, TEXT);
        break;
    case Icon::Trash:
        c.fill_round_rect(x + is * 14 / 100, y + is * 18 / 100, is * 72 / 100, is * 8 / 100, is * 4 / 100, TEXT);
        c.fill_round_rect(x + is * 38 / 100, y + is * 8 / 100, is * 24 / 100, is * 12 / 100, is * 4 / 100, TEXT);
        c.fill_round_rect(x + is * 22 / 100, y + is * 30 / 100, is * 56 / 100, is * 62 / 100, is * 8 / 100, TEXT);
        c.fill_round_rect(x + is * 27 / 100, y + is * 30 / 100, is * 46 / 100, is * 56 / 100, is * 5 / 100, ICON_HOLE);
        c.fill_rect(x + is * 40 / 100, y + is * 40 / 100, is * 5 / 100, is * 38 / 100, TEXT);
        c.fill_rect(x + is * 55 / 100, y + is * 40 / 100, is * 5 / 100, is * 38 / 100, TEXT);
        break;
    case Icon::Plus:
        c.line(cx - s * 0.3f, cy, cx + s * 0.3f, cy, s * 0.09f, ACCENT);
        c.line(cx, cy - s * 0.3f, cx, cy + s * 0.3f, s * 0.09f, ACCENT);
        break;
    case Icon::Sort:  // arrows up and down
        c.line(cx - s * 0.16f, cy - s * 0.32f, cx - s * 0.16f, cy + s * 0.32f, s * 0.07f, TEXT);
        c.line(cx - s * 0.16f, cy - s * 0.32f, cx - s * 0.32f, cy - s * 0.14f, s * 0.07f, TEXT);
        c.line(cx + s * 0.16f, cy - s * 0.32f, cx + s * 0.16f, cy + s * 0.32f, s * 0.07f, TEXT);
        c.line(cx + s * 0.16f, cy + s * 0.32f, cx + s * 0.32f, cy + s * 0.14f, s * 0.07f, TEXT);
        break;
    case Icon::Info:
        c.ring(cx, cy, s * 0.36f, s * 0.07f, TEXT);
        c.fill_circle(cx, cy - s * 0.16f, s * 0.05f, TEXT);
        c.line(cx, cy - s * 0.04f, cx, cy + s * 0.2f, s * 0.08f, TEXT);
        break;
    case Icon::Chevron:  // >
        c.line(cx - s * 0.12f, cy - s * 0.28f, cx + s * 0.14f, cy, s * 0.09f, TEXT2);
        c.line(cx + s * 0.14f, cy, cx - s * 0.12f, cy + s * 0.28f, s * 0.09f, TEXT2);
        break;
    case Icon::Check:
        c.line(cx - s * 0.3f, cy, cx - s * 0.08f, cy + s * 0.24f, s * 0.11f, ACCENT_TEXT);
        c.line(cx - s * 0.08f, cy + s * 0.24f, cx + s * 0.32f, cy - s * 0.24f, s * 0.11f, ACCENT_TEXT);
        break;
    }
}

// --- the folder's entries --------------------------------------------------------------------------

struct Entry {
    std::string name;
    std::string path;  // (a place's root in This PS4's list)
    bool dir = false;
    long long size = -1;
    std::time_t modified = 0;
    Icon icon = Icon::File;
    std::string note;  // a drive's free space (This PS4's list)
};

bool is_text_name(const std::string &name) {
    static const char *const exts[] = {".txt", ".ini", ".cfg", ".conf", ".log", ".xml", ".json", ".md", ".csv", ".yml", ".yaml",
                                       ".lua", ".py", ".js", ".c", ".h", ".cpp", ".hpp", ".sh", ".nfo", ".htm", ".html", ".css",
                                       ".toml", ".properties", ".gitignore"};
    return std::any_of(std::begin(exts), std::end(exts), [&](const char *e) { return ends_with(name, e); });
}

bool is_picture_name(const std::string &name) {
    static const char *const exts[] = {".jpg", ".jpeg", ".jpe", ".jfif", ".png", ".bmp", ".gif", ".tga", ".psd", ".hdr", ".ppm",
                                       ".pgm", ".pnm", ".pbm", ".webp", ".heic", ".heif", ".tif", ".tiff", ".jp2", ".j2k",
                                       ".qoi", ".dds", ".sgi", ".pcx", ".pic", ".svg", ".avif", ".ico"};
    return std::any_of(std::begin(exts), std::end(exts), [&](const char *e) { return ends_with(name, e); });
}

Icon icon_for(const std::string &name, bool dir) {
    if (dir)
        return Icon::Folder;
    if (is_picture_name(name))
        return Icon::Picture;
    if (ends_with(name, ".mp3") || ends_with(name, ".wav") || ends_with(name, ".ogg") || ends_with(name, ".flac") ||
        ends_with(name, ".at9") || ends_with(name, ".m4a") || ends_with(name, ".opus"))
        return Icon::Music;
    if (ends_with(name, ".mp4") || ends_with(name, ".mkv") || ends_with(name, ".avi") || ends_with(name, ".webm") ||
        ends_with(name, ".mov"))
        return Icon::Video;
    // archives and packages (a browser's extensions among them)
    if (ends_with(name, ".pkg") || ends_with(name, ".crx") || ends_with(name, ".xpi") || archive::is_archive_name(name))
        return Icon::Package;
    if (is_text_name(name))
        return Icon::Notepad;
    return Icon::File;
}

std::string gib(double bytes) {
    char s[32];
    std::snprintf(s, sizeof s, bytes >= 1e12 ? "%.1f TB" : "%.1f GB", bytes >= 1e12 ? bytes / 1099511627776.0 : bytes / 1073741824.0);
    return s;
}

// a drive's (or a folder's file system's) room: "12.3 GB free of 100 GB"
std::string space_note(const std::string &path) {
    struct statvfs fs{};
    if (statvfs(path.c_str(), &fs) != 0 || !fs.f_blocks)
        return {};
    const double unit = fs.f_frsize ? fs.f_frsize : fs.f_bsize;
    return gib(unit * fs.f_bavail) + " free of " + gib(unit * fs.f_blocks);
}

// (the status bar's, looked at again every 2 s rather than every frame)
std::string space_cached(const std::string &path) {
    static std::string for_path, note;
    static double when = -10;
    const double t = now_seconds();
    if (path != for_path || t - when > 2) {
        note = space_note(path);
        for_path = path;
        when = t;
    }
    return note;
}

// --- the Recycle Bin -----------------------------------------------------------------------------------
//
// Delete moves things to their drive's bin (a rename: /data's is Wisp data/Recycle Bin, a USB drive's
// .wisp-recycle at its top), each beside a record of where it was and when ("<id>.info"). The Recycle
// Bin place lists every bin's by their own names; Restore puts them back where they were (the folder
// made again if it went; "name (2)" if the name is taken), Delete or Empty Recycle Bin deletes them for
// good. Shift+Delete and "Delete for good" skip the bin, and so does anything with no bin (outside /data
// and the USB drives) or holding one: those are asked about first.

const std::string RECYCLE = "recycle:";  // the Recycle Bin place's path
constexpr const char *USB_BIN = ".wisp-recycle";
std::string data_bin() { return std::string(WISP_DATA) + "/Recycle Bin"; }

// The bin a path's things go to ("" none)
std::string bin_of(const std::string &path) {
    for (int i = 0; i < 8; i++) {
        const std::string usb = "/mnt/usb" + std::to_string(i);
        if (within(path, usb))
            return usb + "/" + USB_BIN;
    }
    return within(path, "/data") ? data_bin() : std::string();
}

std::vector<std::string> all_bins() {
    std::vector<std::string> list{data_bin()};
    for (int i = 0; i < 8; i++) {
        const std::string usb = "/mnt/usb" + std::to_string(i);
        if (is_dir(usb))
            list.push_back(usb + "/" + USB_BIN);
    }
    return list;
}

// a bin's own folder (not listed where it is: the Recycle Bin place shows what it holds)
bool is_bin_folder(const std::string &path) {
    // (a USB drive's: at its top, /mnt/usbN)
    return path == data_bin() || (path.rfind("/mnt/usb", 0) == 0 && base_name(path) == USB_BIN && folder_of(path).find('/', 5) == std::string::npos);
}

struct Binned {
    std::string original;  // where it was
    std::time_t when = 0;  // when it was deleted
};

// An item's record (item: its path in a bin)
bool read_binned(const std::string &item, Binned &out) {
    FILE *f = std::fopen((item + ".info").c_str(), "r");
    if (!f)
        return false;
    char line[4096];
    while (std::fgets(line, sizeof line, f)) {
        std::string s = line;
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r'))
            s.pop_back();
        if (s.rfind("path=", 0) == 0)
            out.original = s.substr(5);
        else if (s.rfind("time=", 0) == 0)
            out.when = static_cast<std::time_t>(std::atoll(s.c_str() + 5));
    }
    std::fclose(f);
    return !out.original.empty();
}

// Every bin's items, by the names they had (path: where they are in their bin; note: where they were)
std::vector<Entry> bin_entries() {
    std::vector<Entry> list;
    for (const std::string &bin : all_bins()) {
        DIR *dir = opendir(bin.c_str());
        if (!dir)
            continue;
        while (dirent *d = readdir(dir)) {
            const std::string name = d->d_name;
            if (name == "." || name == ".." || ends_with(name, ".info"))
                continue;
            Binned b;
            if (!read_binned(bin + "/" + name, b))
                continue;  // (no record: not one of the bin's)
            Entry e;
            e.name = base_name(b.original);
            e.path = bin + "/" + name;
            struct stat st{};
            if (lstat(e.path.c_str(), &st) == 0) {
                e.dir = S_ISDIR(st.st_mode);
                e.size = e.dir ? -1 : static_cast<long long>(st.st_size);
            }
            e.modified = b.when;  // (the date column: when it was deleted)
            e.icon = icon_for(e.name, e.dir);
            e.note = folder_of(b.original);
            list.push_back(e);
        }
        closedir(dir);
    }
    return list;
}

bool make_dirs(const std::string &path) {
    if (path.empty() || path == "/" || is_dir(path))
        return true;
    if (!make_dirs(folder_of(path)))
        return false;
    return mkdir(path.c_str(), 0777) == 0 || errno == EEXIST;
}

// The places, top to bottom: Wisp's own folders (QUICK of them), then the drives - the console's data,
// the USB drives plugged in, the app's own files (read-only) - and the Recycle Bin
constexpr int QUICK = 2;

std::vector<Entry> find_places() {
    std::vector<Entry> list;
    list.push_back({"Downloads", wisp_download_folder(), true, -1, 0, Icon::Download, {}});  // (the folder chosen, v00.29)
    list.push_back({"OrbisWeb data", WISP_DATA, true, -1, 0, Icon::Folder, {}});
    list.push_back({"Data (/data)", "/data", true, -1, 0, Icon::Drive, {}});
    for (int i = 0; i < 8; i++) {
        const std::string usb = "/mnt/usb" + std::to_string(i);
        if (is_dir(usb))
            list.push_back({"USB drive " + std::to_string(i + 1), usb, true, -1, 0, Icon::Usb, {}});
    }
    list.push_back({"OrbisWeb app (/app0)", WISP_APP, true, -1, 0, Icon::Package, {}});
    list.push_back({"Recycle Bin", RECYCLE, true, -1, 0, Icon::Trash, {}});
    for (Entry &e : list)
        if (e.icon == Icon::Drive || e.icon == Icon::Usb)
            e.note = space_note(e.path);
    return list;
}

std::vector<Entry> place_list;
double places_when = -10;

// The places looked at again every 2 s, not every frame (a USB drive plugged in or out, the room
// left): true when they changed
bool update_places(bool force = false) {
    const double t = now_seconds();
    if (!force && !place_list.empty() && t - places_when < 2)
        return false;
    places_when = t;
    std::vector<Entry> list = find_places();
    bool changed = list.size() != place_list.size();
    for (size_t i = 0; !changed && i < list.size(); i++)
        changed = list[i].path != place_list[i].path || list[i].note != place_list[i].note;
    place_list.swap(list);
    return changed;
}

const std::vector<Entry> &places() {
    if (place_list.empty())
        update_places(true);
    return place_list;
}

// the pane's items: This PS4 (the places' list), then the places
std::vector<Entry> nav_items() {
    std::vector<Entry> list{{"This PS4", "", true, -1, 0, Icon::Computer, {}}};
    const std::vector<Entry> &all = places();
    list.insert(list.end(), all.begin(), all.end());
    return list;
}

// The place holding path (the longest that matches: Downloads before Wisp data before /data), -1 for none
int place_of(const std::string &path) {
    const std::vector<Entry> &list = places();
    int best = -1;
    size_t best_size = 0;
    for (size_t i = 0; i < list.size(); i++)
        if (!path.empty() && within(path, list[i].path) && list[i].path.size() > best_size)
            best = static_cast<int>(i), best_size = list[i].path.size();
    return best;
}

// A folder's name for its title: a place's own, else its last part
std::string place_title(const std::string &path) {
    if (path.empty())
        return "This PS4";
    for (const Entry &p : places())
        if (p.path == path)
            return p.name;
    return path == "/" ? "PS4 (/)" : base_name(path);
}

std::string shown_path(const std::string &path) { return path.empty() ? "This PS4" : path; }

// --- archives opened as folders ----------------------------------------------------------------------

std::map<std::string, std::string> archive_passwords;  // typed this run, by archive
std::string archive_note;        // what the list says instead of entries ("Reading photos.zip...", why it can't open)
std::string password_asked_for;  // (a locked list's password is asked once, again when it was wrong)

std::string password_for(const std::string &file) {
    const auto it = archive_passwords.find(file);
    return it == archive_passwords.end() ? std::string() : it->second;
}

// The entries of the folder inside at path (archive::index_of reads the list on a thread: empty, with
// the note, until it has)
std::vector<Entry> archive_entries(const std::string &path, const std::string &file, const std::string &inside) {
    std::vector<Entry> list;
    const auto index = archive::index_of(file, password_for(file));
    if (!index) {
        archive_note = "Reading " + base_name(file) + "...";
        return list;
    }
    archive_note.clear();
    if (!index->error.empty() && index->items.empty())
        archive_note = index->wrong_password    ? "That isn't this archive's password."
                       : index->needs_password ? "This archive's list is locked with a password."
                                               : base_name(file) + " can't be opened: " + index->error;
    const std::string prefix = inside.empty() ? std::string() : inside + "/";
    for (const archive::Item &item : index->items) {
        if (item.path.size() <= prefix.size() || item.path.compare(0, prefix.size(), prefix) != 0)
            continue;
        const std::string name = item.path.substr(prefix.size());
        if (name.find('/') != std::string::npos)
            continue;  // (further down)
        Entry e;
        e.name = name;
        e.path = join(path, name);
        e.dir = item.dir;
        e.size = item.dir ? -1 : item.size;
        e.modified = item.modified;
        e.icon = icon_for(name, item.dir);
        list.push_back(e);
    }
    return list;
}

std::vector<Entry> list_dir(const std::string &path) {
    if (path.empty())  // This PS4: the places
        return places();
    if (path == RECYCLE)
        return bin_entries();
    std::string file, inside;
    if (in_archive(path, file, inside))
        return archive_entries(path, file, inside);
    std::vector<Entry> list;
    DIR *dir = opendir(path.c_str());
    if (!dir)
        return list;
    while (dirent *e = readdir(dir)) {
        const std::string name = e->d_name;
        if (name == "." || name == "..")
            continue;
        Entry entry;
        entry.name = name;
        entry.path = join(path, name);
        if (is_bin_folder(entry.path))
            continue;  // (the Recycle Bin place shows what it holds)
        struct stat st{};
        if (stat(entry.path.c_str(), &st) == 0) {
            entry.dir = S_ISDIR(st.st_mode);
            entry.size = entry.dir ? -1 : static_cast<long long>(st.st_size);
            entry.modified = st.st_mtime;
        }
        entry.icon = icon_for(name, entry.dir);
        list.push_back(entry);
    }
    closedir(dir);
    return list;
}

std::string size_text(long long size) {
    if (size < 0)
        return "";
    char s[32];
    if (size < 1024)
        std::snprintf(s, sizeof s, "%lld bytes", size);
    else if (size < 1024 * 1024)
        std::snprintf(s, sizeof s, "%lld KB", (size + 1023) / 1024);
    else if (size < 1024ll * 1024 * 1024)
        std::snprintf(s, sizeof s, "%.1f MB", size / 1048576.0);
    else
        std::snprintf(s, sizeof s, "%.2f GB", size / 1073741824.0);
    return s;
}

std::string type_text(const Entry &e) {
    if (e.dir)
        return e.icon == Icon::Drive || e.icon == Icon::Usb ? "Drive" : e.icon == Icon::Folder ? "File folder" : "System folder";
    if (ends_with(e.name, ".txt"))
        return "Text Document";
    const size_t dot = e.name.find_last_of('.');
    if (dot == std::string::npos || dot == 0)
        return "File";
    std::string ext = e.name.substr(dot + 1);
    for (char &ch : ext)
        ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    return ext + " File";
}

std::string date_text(std::time_t t) {
    if (!t)
        return {};
    char when[32];
    std::tm local{};
    localtime_r(&t, &local);
    std::strftime(when, sizeof when, "%d/%m/%Y %H:%M", &local);
    return when;
}

// The order: folders first, then by name, date modified, type or size (as the header's columns)
enum SortBy { SORT_NAME, SORT_DATE, SORT_TYPE, SORT_SIZE };
void sort_entries(std::vector<Entry> &list, int by, bool down) {
    std::stable_sort(list.begin(), list.end(), [&](const Entry &a, const Entry &b) {
        if (a.dir != b.dir)
            return a.dir;
        int order = 0;
        if (by == SORT_DATE)
            order = a.modified < b.modified ? -1 : a.modified > b.modified;
        else if (by == SORT_TYPE)
            order = lower(type_text(a)).compare(lower(type_text(b)));
        else if (by == SORT_SIZE)
            order = a.size < b.size ? -1 : a.size > b.size;
        if (order == 0)
            order = lower(a.name).compare(lower(b.name));
        return down ? order > 0 : order < 0;
    });
}

// --- the state -----------------------------------------------------------------------------------

struct Rect {
    int x = 0, y = 0, w = 0, h = 0;
    bool contains(float px, float py) const { return px >= x && py >= y && px < x + w && py < y + h; }
};

// A line of text being typed on a USB keyboard (a name, the address, the search): the cursor and the
// selection's other end, in bytes at characters' starts
struct LineEdit {
    bool active = false;
    std::string text;
    size_t cursor = 0, anchor = 0;
};

// The menu for what's under the pointer (Square, the mouse's right button), and the command bar's
// drop-down (Sort)
struct MenuItem {
    std::string label;
    std::string action;  // "" for a separator
    bool enabled = true;
};

struct Menu {
    bool open = false;
    int x = 0, y = 0;
    std::vector<MenuItem> items;
    std::string target;  // what it is for (a hit key)
};

enum class DialogKind { Message, Delete };

struct Dialog {
    bool open = false;
    DialogKind kind = DialogKind::Message;  // [OK], or [Delete] [Cancel]
    std::string title, text;                // text: lines split by '\n'
    std::vector<std::string> paths;         // what Delete deletes
    // Properties: folders' sizes are counted on a thread (count_sizes): the text is head, the sizes
    // ("Counting..." until they come), tail
    unsigned counting = 0;                  // the count it waits for (0: none)
    bool folder = false;                    // one folder (it's among the items counted), else several items
    std::string head, tail;
    int buttons() const { return kind == DialogKind::Message ? 1 : 2; }
};

// Cut or Copy, for Paste
struct Clipboard {
    std::vector<std::string> paths;
    bool cut = false;
};

// Entries dragged with the pointer: onto a folder, a place or a part of the address
struct DraggedFiles {
    bool pending = false;  // pressed on an entry: a drag once the pointer has moved far enough
    bool active = false;
    float x0 = 0, y0 = 0;  // where it was pressed
    std::vector<std::string> paths;
    std::string label;
    Icon icon = Icon::File;
    int click_row = -1;  // let go without a drag: this row chosen alone (it was one of several)
};

// The browser's side (set_host): kept apart from the state, which a fresh Files replaces (one of these
// may be running then: open_file can close Files and open it again)
Host browser;

// An archive's job (files/archives.h): what was asked (kept to ask again with a password) and what to
// do when it has ended
struct ArchiveJob {
    std::string file;                // the archive files come out of
    std::vector<std::string> items;  // their paths inside (none: all of it)
    std::string base, folder, title, open;
    std::string then_go;             // the folder to show when it ended well (Extract All)
    std::string then_select;         // the entry to choose then (an archive just made)
};

struct State {
    bool visible = false;
    bool dirty = true;
    int x = 0, y = 108, w = 1920, h = 972;  // the area on the screen (input's; draw's canvas is as big)
    float px = -1, py = -1;                  // the pointer, in the area
    std::string path;                        // the folder ("" for This PS4: the places)
    std::vector<Entry> entries;              // as shown: sorted, and only the search's matches
    std::set<std::string> chosen;            // the selected entries (paths)
    int cursor = -1, anchor = -1;            // the keyboard's entry; the other end of Shift's range
    std::vector<std::string> back, forward;
    int sort_by = SORT_NAME;
    bool sort_down = false;
    float scroll = 0;                        // rows
    LineEdit rename, address, search;        // typed in place on a USB keyboard
    std::string rename_path;                 // the entry rename's text is for
    std::string filter;                      // the search's text, applied
    bool banding = false;                    // a selection rectangle being drawn, from:
    float band_x = 0, band_y = 0;            // (band_y: in the list's rows, scroll included)
    std::vector<Rect> crumb_rects;           // the address's parts, as drawn last
    Menu menu;
    Dialog dialog;
    Clipboard clipboard;
    DraggedFiles dragged;
    std::string hover;                       // what the pointer is over (a hit key)
    std::string last_click_key;              // (Cross twice: the same thing within half a second)
    double last_click_time = 0;
    bool held_back = false;                  // after the PS4's keyboard: until the buttons that closed it are let go
    bool key_ctrl = false, key_shift = false, key_alt = false, caps_lock = false;  // a USB keyboard's
    std::string text_clipboard;              // text cut or copied in the boxes, for their Ctrl+V
    std::string typed;                       // letters typed to jump to a name, and when
    double typed_at = 0;
    double job_drawn = 0;                    // when a running job's progress was drawn last
    ArchiveJob archive_job;                  // the archive job running (archive_job_running), for its end
    bool archive_job_running = false;
};

State S;

void toast(const std::string &text) {
    if (browser.toast)
        browser.toast(text);
}

void show_message(const std::string &title, const std::string &text) {
    S.dialog = Dialog{};
    S.dialog.open = true;
    S.dialog.title = title;
    S.dialog.text = text;
    S.menu.open = false;
    S.dirty = true;
}

// What went wrong: in a dialog, or (Files closed meanwhile) a toast
void report(const std::string &title, const std::string &text) {
    if (S.visible)
        show_message(title, text);
    else
        toast(title + ": " + text);
}

// The PS4's keyboard (the browser's): then(text) once the user has typed it
void ask(const std::string &title, const std::string &initial, std::function<void(const std::string &)> then) {
    if (!browser.ask_text)
        return;
    browser.ask_text(title, initial, [then = std::move(then)](bool ok, const std::string &text) {
        S.held_back = true;  // (the buttons that closed the keyboard are no click here)
        S.dirty = true;
        if (ok)
            then(text);
    });
}

// The pointer to (x, y) in the area: the D-pad steps it through a menu, the list
void warp(float x, float y) { shell::input_warp(S.x + x, S.y + y); }

// --- archives: taking files out, making them ---------------------------------------------------------

bool start_extract(const ArchiveJob &job) {
    if (!archive::job_extract(job.file, job.items, job.base, job.folder, password_for(job.file), job.title, job.open)) {
        show_message(job.title, "Wait for the job that is running to finish.");
        return false;
    }
    S.archive_job = job;
    S.archive_job_running = true;
    S.job_drawn = 0;
    S.dirty = true;
    return true;
}

// Entries inside one archive (paths Files shows them at) taken out into folder
void extract_entries(const std::vector<std::string> &paths, const std::string &folder) {
    std::string file, inside;
    if (paths.empty() || !in_archive(paths[0], file, inside))
        return;
    ArchiveJob job;
    job.file = file;
    for (const std::string &p : paths) {
        std::string f, i;
        if (in_archive(p, f, i) && f == file && !i.empty())
            job.items.push_back(i);
    }
    if (job.items.empty())
        return;
    // (they were chosen in one folder inside: their paths go on from it)
    const size_t slash = job.items[0].find_last_of('/');
    job.base = slash == std::string::npos ? std::string() : job.items[0].substr(0, slash);
    job.folder = folder;
    job.title = "Extracting " + (paths.size() == 1 ? base_name(paths[0]) : std::to_string(paths.size()) + " items");
    start_extract(job);
}

// A file inside an archive opened: taken out to the app's own folder for them (once a run), then
// opened as any file is
void open_in_archive(const std::string &file, const std::string &inside) {
    const std::string folder = archive::open_folder(file);
    if (is_file(folder + "/" + inside)) {
        if (browser.open_file)
            browser.open_file(folder + "/" + inside);
        return;
    }
    ArchiveJob job;
    job.file = file;
    job.items = {inside};
    job.folder = folder;
    job.title = "Opening " + base_name(inside);
    job.open = inside;
    start_extract(job);
}

// --- layout ----------------------------------------------------------------------------------------

// As Windows 11's File Explorer: the command bar, the address row (Back, Forward, Up, Refresh, the
// address, the search box), the places' pane, the list with its header, the status bar
constexpr int FILES_NAV_W = 240, FILES_CMD_H = 50, FILES_ADDR_H = 48, FILES_HEADER_H = 32, ROW_H = 34, FILES_STATUS_H = 28;
constexpr int NAV_ROW_H = 32, NAV_GAP = 10, SEARCH_W = 250, CHECK_X = 12;
constexpr int MENU_W = 280, MENU_ITEM_H = 38, MENU_SEPARATOR_H = 9;

// the command bar's buttons: key (files_command's, or "sort": its drop-down), label (none: an icon only)
struct CmdButton {
    const char *key;
    const char *label;
    Icon icon;
    int w;  // a separator when key is ""
};
const CmdButton FILES_CMDS[] = {{"new-folder", "New folder", Icon::Plus, 132}, {"", nullptr, Icon::File, 14},
                                {"cut", nullptr, Icon::Cut, 46}, {"copy", nullptr, Icon::Copy, 46}, {"paste", nullptr, Icon::Paste, 46},
                                {"rename", nullptr, Icon::Rename, 46}, {"delete", nullptr, Icon::Trash, 46}, {"", nullptr, Icon::File, 14},
                                {"sort", "Sort", Icon::Sort, 96}, {"", nullptr, Icon::File, 14},
                                {"properties", "Properties", Icon::Info, 132}};
constexpr int CMD_COUNT = static_cast<int>(sizeof FILES_CMDS / sizeof FILES_CMDS[0]);

Rect close_button() { return Rect{S.w - 46, 0, 46, TITLE_H}; }
Rect files_cmd_rect(int i) {
    int x = 12;
    for (int k = 0; k < i && k < CMD_COUNT; k++)
        x += FILES_CMDS[k].w;
    return Rect{x, TITLE_H + 7, i < CMD_COUNT ? FILES_CMDS[i].w - 4 : 0, FILES_CMD_H - 14};
}
// Back, Forward, Up, Refresh
Rect files_nav_button(int i) { return Rect{10 + i * 42, TITLE_H + FILES_CMD_H + 6, 38, 36}; }
Rect files_address() {
    const int x = 10 + 4 * 42 + 6;
    return Rect{x, TITLE_H + FILES_CMD_H + 6, S.w - 16 - SEARCH_W - 10 - x, 36};
}
Rect files_search() { return Rect{S.w - 16 - SEARCH_W, TITLE_H + FILES_CMD_H + 6, SEARCH_W, 36}; }
int files_top() { return TITLE_H + FILES_CMD_H + FILES_ADDR_H; }
int files_bottom() { return S.h - FILES_STATUS_H; }
// (a gap between Wisp's own folders and the drives)
Rect files_nav_item(int i) {
    const int y = files_top() + 8 + i * NAV_ROW_H + (i > QUICK ? NAV_GAP : 0);
    return Rect{8, y, FILES_NAV_W - 16, NAV_ROW_H - 2};
}
Rect files_header() { return Rect{FILES_NAV_W, files_top(), S.w - FILES_NAV_W - 1, FILES_HEADER_H}; }
Rect files_list() {
    const int top = files_top() + FILES_HEADER_H;
    return Rect{FILES_NAV_W, top, S.w - FILES_NAV_W - 1, files_bottom() - top};
}
int files_rows() { return std::max(1, files_list().h / ROW_H); }
// the columns: Name, Date modified, Type, Size (This PS4: Type, then the free space)
int files_column_x(int i) {
    const Rect l = files_list();
    static const int at[] = {0, 47, 67, 85};
    return l.x + (i == 0 ? 44 : l.w * at[i] / 100);
}

void clamp_scroll() {
    S.scroll = std::clamp(S.scroll, 0.f, static_cast<float>(std::max(0, static_cast<int>(S.entries.size()) - files_rows())));
}

// --- the list and the selection --------------------------------------------------------------------

// The folder's list again: sorted as chosen, only what matches the search
void load_entries() {
    std::vector<Entry> list = list_dir(S.path);
    if (!S.path.empty())
        sort_entries(list, S.sort_by, S.sort_down);
    if (!S.filter.empty()) {
        const std::string f = lower(S.filter);
        list.erase(std::remove_if(list.begin(), list.end(), [&](const Entry &e) { return lower(e.name).find(f) == std::string::npos; }),
                   list.end());
    }
    S.entries.swap(list);
    S.dirty = true;
}

// To another folder ("" This PS4); remember: Back comes back here
void navigate(const std::string &path, bool remember = true) {
    if (remember && path != S.path) {
        S.back.push_back(S.path);
        if (S.back.size() > 64)
            S.back.erase(S.back.begin());
        S.forward.clear();
    }
    S.path = path;
    S.filter.clear();
    S.search = S.rename = S.address = LineEdit{};
    load_entries();
    S.chosen.clear();
    S.cursor = S.anchor = -1;
    S.scroll = 0;
    S.banding = false;
}

std::vector<std::string> chosen_paths() {  // in the list's order
    std::vector<std::string> out;
    for (const Entry &e : S.entries)
        if (S.chosen.count(e.path))
            out.push_back(e.path);
    return out;
}

void keep_in_view(int i) {
    const int rows = files_rows();
    if (i < S.scroll)
        S.scroll = static_cast<float>(i);
    else if (i >= S.scroll + rows)
        S.scroll = static_cast<float>(i - rows + 1);
}

void choose_only(int i) {
    S.chosen.clear();
    if (i >= 0 && i < static_cast<int>(S.entries.size()))
        S.chosen.insert(S.entries[i].path);
    S.cursor = S.anchor = i;
}

// from the anchor to i (Shift)
void choose_range(int i) {
    if (S.anchor < 0)
        S.anchor = S.cursor >= 0 ? S.cursor : i;
    S.chosen.clear();
    for (int k = std::min(S.anchor, i); k <= std::max(S.anchor, i); k++)
        if (k >= 0 && k < static_cast<int>(S.entries.size()))
            S.chosen.insert(S.entries[k].path);
    S.cursor = i;
}

void choose_toggle(int i) {  // (Ctrl, or the row's check box)
    if (i < 0 || i >= static_cast<int>(S.entries.size()))
        return;
    const std::string &p = S.entries[i].path;
    if (!S.chosen.erase(p))
        S.chosen.insert(p);
    S.cursor = S.anchor = i;
}

void choose_all() {
    for (const Entry &e : S.entries)
        S.chosen.insert(e.path);
}

// The entries a command is for: the chosen ones (the one the pointer is on when it isn't chosen: the
// menu chooses it alone first)
std::vector<std::string> subject(int row) {
    if (row >= 0 && row < static_cast<int>(S.entries.size()) && !S.chosen.count(S.entries[row].path))
        choose_only(row);
    return chosen_paths();
}

// The list again (keeping the selection, by path): after a change in the folder; select: the entry to
// have chosen alone
void refresh(const std::string &select = std::string()) {
    const std::string focus = !select.empty() ? select
                            : S.cursor >= 0 && S.cursor < static_cast<int>(S.entries.size()) ? S.entries[S.cursor].path : std::string();
    const float scroll = S.scroll;
    load_entries();
    S.scroll = scroll;
    std::set<std::string> still;
    for (const Entry &e : S.entries)
        if (select.empty() ? S.chosen.count(e.path) > 0 : e.path == select)
            still.insert(e.path);
    S.chosen.swap(still);
    S.cursor = S.anchor = -1;
    for (int i = 0; i < static_cast<int>(S.entries.size()); i++)
        if (S.entries[i].path == focus) {
            S.cursor = S.anchor = i;
            keep_in_view(i);
        }
    clamp_scroll();
}

// --- typing on a USB keyboard: characters, edit boxes ----------------------------------------------

// Virtual keys (Windows' VK_*, as shell/hwkeyboard.h gives them)
enum : uint16_t {
    VK_BACK = 0x08, VK_RETURN = 0x0d, VK_SHIFT = 0x10, VK_CONTROL = 0x11, VK_MENU = 0x12, VK_CAPITAL = 0x14, VK_ESCAPE = 0x1b,
    VK_SPACE = 0x20, VK_PRIOR = 0x21, VK_NEXT = 0x22, VK_END = 0x23, VK_HOME = 0x24, VK_LEFT = 0x25, VK_UP = 0x26,
    VK_RIGHT = 0x27, VK_DOWN = 0x28, VK_DELETE = 0x2e, VK_APPS = 0x5d, VK_F2 = 0x71, VK_F3 = 0x72, VK_F4 = 0x73, VK_F5 = 0x74,
    VK_LSHIFT = 0xa0, VK_RSHIFT = 0xa1, VK_LCONTROL = 0xa2, VK_RCONTROL = 0xa3, VK_LMENU = 0xa4, VK_RMENU = 0xa5,
};

// The character a key types on a US keyboard layout (0: none)
char key_char(uint16_t vk, bool shift) {
    const bool upper = shift != S.caps_lock;
    if (vk >= 'A' && vk <= 'Z')
        return static_cast<char>(upper ? vk : vk + 32);
    if (vk >= '0' && vk <= '9')
        return shift ? ")!@#$%^&*("[vk - '0'] : static_cast<char>(vk);
    if (vk >= 0x60 && vk <= 0x69)  // the number pad's
        return static_cast<char>('0' + vk - 0x60);
    switch (vk) {
    case VK_SPACE: return ' ';
    case 0x6a: return '*';
    case 0x6b: return '+';
    case 0x6d: return '-';
    case 0x6e: return '.';
    case 0x6f: return '/';
    case 0xba: return shift ? ':' : ';';
    case 0xbb: return shift ? '+' : '=';
    case 0xbc: return shift ? '<' : ',';
    case 0xbd: return shift ? '_' : '-';
    case 0xbe: return shift ? '>' : '.';
    case 0xbf: return shift ? '?' : '/';
    case 0xc0: return shift ? '~' : '`';
    case 0xdb: return shift ? '{' : '[';
    case 0xdc: return shift ? '|' : '\\';
    case 0xdd: return shift ? '}' : ']';
    case 0xde: return shift ? '"' : '\'';
    }
    return 0;
}

// UTF-8: the start of the character after / before byte at
size_t next_char(const std::string &s, size_t at) {
    if (at >= s.size())
        return s.size();
    at++;
    while (at < s.size() && (static_cast<unsigned char>(s[at]) & 0xc0) == 0x80)
        at++;
    return at;
}

size_t prev_char(const std::string &s, size_t at) {
    if (at == 0)
        return 0;
    at--;
    while (at > 0 && (static_cast<unsigned char>(s[at]) & 0xc0) == 0x80)
        at--;
    return at;
}

// A key for an edit box; false when it isn't one (Enter, Escape: the caller's)
bool edit_key(LineEdit &e, const KeyEvent &k) {
    const size_t from = std::min(e.cursor, e.anchor), to = std::max(e.cursor, e.anchor);
    const auto erase_selection = [&] {
        e.text.erase(from, to - from);
        e.cursor = e.anchor = from;
    };
    const auto move = [&](size_t at) {
        e.cursor = at;
        if (!S.key_shift)
            e.anchor = at;
    };
    if (S.key_ctrl) {
        if (k.vkey == 'A') {
            e.anchor = 0;
            e.cursor = e.text.size();
        } else if (k.vkey == 'C' || k.vkey == 'X') {
            if (to > from)
                S.text_clipboard = e.text.substr(from, to - from);
            if (k.vkey == 'X')
                erase_selection();
        } else if (k.vkey == 'V') {
            const std::string paste = S.text_clipboard.substr(0, S.text_clipboard.find('\n'));
            erase_selection();
            e.text.insert(e.cursor, paste);
            e.cursor = e.anchor = e.cursor + paste.size();
        } else {
            return false;
        }
        return true;
    }
    switch (k.vkey) {
    case VK_LEFT: move(from != to && !S.key_shift ? from : prev_char(e.text, e.cursor)); return true;
    case VK_RIGHT: move(from != to && !S.key_shift ? to : next_char(e.text, e.cursor)); return true;
    case VK_HOME: move(0); return true;
    case VK_END: move(e.text.size()); return true;
    case VK_BACK:
        if (to > from) {
            erase_selection();
        } else if (e.cursor > 0) {
            const size_t p = prev_char(e.text, e.cursor);
            e.text.erase(p, e.cursor - p);
            e.cursor = e.anchor = p;
        }
        return true;
    case VK_DELETE:
        if (to > from)
            erase_selection();
        else if (e.cursor < e.text.size())
            e.text.erase(e.cursor, next_char(e.text, e.cursor) - e.cursor);
        return true;
    }
    if (const char ch = key_char(k.vkey, S.key_shift)) {
        erase_selection();
        e.text.insert(e.cursor, 1, ch);
        e.cursor = e.anchor = e.cursor + 1;
        return true;
    }
    return false;
}

// An edit box drawn in r: its text, the selection, the cursor
void draw_edit(Canvas &c, const Rect &r, const LineEdit &e, int px) {
    const int tx = r.x + 10, base = r.y + r.h / 2 + px / 2 - 1;
    const size_t from = std::min(e.cursor, e.anchor), to = std::max(e.cursor, e.anchor);
    // (a long text: its end, where the cursor is, stays in the box)
    int shift = 0;
    const int cursor_x = c.text_width(e.text.substr(0, e.cursor), px);
    if (cursor_x > r.w - 24)
        shift = cursor_x - (r.w - 24);
    size_t first = 0;
    while (first < e.text.size() && c.text_width(e.text.substr(0, first), px) < shift)
        first = next_char(e.text, first);
    const int skipped = c.text_width(e.text.substr(0, first), px);
    if (to > from) {
        const int sx = std::max(0, c.text_width(e.text.substr(0, from), px) - skipped);
        const int ex = std::min(r.w - 16, c.text_width(e.text.substr(0, to), px) - skipped);
        if (ex > sx)
            c.fill_rect(tx + sx, r.y + 6, ex - sx, r.h - 12, rgb(0x0078d4, 0xa0));
    }
    c.text(tx, base, e.text.substr(first), px, TEXT, Weight::Regular, r.w - 20);
    c.fill_rect(tx + cursor_x - skipped, r.y + 7, 2, r.h - 14, TEXT);
}

// --- Properties ------------------------------------------------------------------------------------

// Folders' sizes, counted on a thread of their own (a big folder takes a while, and the browser's
// thread must not stand still): the dialog shows "Counting..." until they come
struct Count {
    unsigned serial = 0;
    long long bytes = 0, items = 0;
};
std::mutex count_lock;
Count count_done;         // the newest count finished (count_lock)
unsigned count_serial = 0;

unsigned count_sizes(std::vector<std::string> paths) {
    const unsigned serial = ++count_serial;
    std::thread([paths = std::move(paths), serial] {
        long long items = 0, bytes = 0;
        for (const std::string &p : paths)
            bytes += shell::folder_size(p, items);
        std::lock_guard<std::mutex> guard(count_lock);
        if (serial > count_done.serial)
            count_done = Count{serial, bytes, items};
    }).detach();
    return serial;
}

std::string size_lines(long long bytes, long long items, bool folder) {
    return "\nSize: " + size_text(bytes) + " (" + std::to_string(bytes) + " bytes)\nContains: " + std::to_string(std::max(0ll, items)) +
           (folder ? " items" : " items in all");
}

const char *const COUNTING = "\nSize: Counting...\nContains: Counting...";

// The count the dialog waits for, once it's done
void poll_count() {
    if (!S.dialog.open || !S.dialog.counting)
        return;
    Count done;
    {
        std::lock_guard<std::mutex> guard(count_lock);
        done = count_done;
    }
    if (done.serial != S.dialog.counting)
        return;
    S.dialog.text = S.dialog.head + size_lines(done.bytes, S.dialog.folder ? done.items - 1 : done.items, S.dialog.folder) + S.dialog.tail;
    S.dialog.counting = 0;
    S.dirty = true;
}

std::string mode_text(mode_t m) {
    std::string s = S_ISDIR(m) ? "d" : "-";
    const char *rwx = "rwxrwxrwx";
    for (int i = 0; i < 9; i++)
        s += (m & (0400 >> i)) ? rwx[i] : '-';
    return s;
}

// A place (This PS4's list): what it is and its drive's room; no size (all of /data would take long)
void show_place_properties(const Entry &place) {
    if (place.path == RECYCLE) {
        const std::vector<Entry> held = bin_entries();
        long long bytes = 0;
        for (const Entry &e : held)
            bytes += std::max(0ll, e.size);
        show_message("Recycle Bin properties", "Holds: " + std::to_string(held.size()) + (held.size() == 1 ? " item" : " items") +
                                                   (bytes ? " (" + size_text(bytes) + " in files, folders not counted)" : std::string()) +
                                                   "\nWhere: " + data_bin() + ", and " + USB_BIN + " at a USB drive's top" +
                                                   "\nEmptied by: Empty Recycle Bin (its menu)");
        return;
    }
    std::string text = "Type: " + type_text(place) + "\nLocation: " + place.path;
    const std::string room = space_note(place.path);
    if (!room.empty())
        text += "\nDrive: " + room;
    if (read_only(place.path))
        text += "\nRead-only: the app's own files";
    show_message(place.name + " properties", text);
}

// An entry inside an archive (or the archive's top): what the archive's list says of it
void show_archive_properties(const std::string &path, const std::string &file, const std::string &inside) {
    const auto index = archive::index_of(file, password_for(file));
    std::string text = "Location: " + shown_path(folder_of(path)) + "\nIn: " + base_name(file);
    if (index && !index->format.empty())
        text += " (" + index->format + ")";
    if (index && inside.empty()) {
        long long files = 0, folders = 0;
        for (const archive::Item &it : index->items)
            (it.dir ? folders : files)++;
        text += "\nContains: " + std::to_string(files) + (files == 1 ? " file, " : " files, ") + std::to_string(folders) +
                (folders == 1 ? " folder" : " folders") + "\nSize unpacked: " + size_text(index->unpacked);
    } else if (index) {
        for (const archive::Item &it : index->items) {
            if (it.path != inside)
                continue;
            if (!it.dir && it.size >= 0)
                text += "\nSize: " + size_text(it.size) + " (" + std::to_string(it.size) + " bytes)";
            if (it.modified)
                text += "\nModified: " + date_text(it.modified);
            if (it.encrypted)
                text += "\nLocked with a password";
            break;
        }
    }
    show_message(base_name(path) + " properties", text);
}

void show_properties(const std::vector<std::string> &paths) {
    if (paths.empty())
        return;
    std::string archive_file, inside;
    if (paths.size() == 1 && in_archive(paths[0], archive_file, inside)) {
        show_archive_properties(paths[0], archive_file, inside);
        return;
    }
    Binned binned;
    if (paths.size() == 1 && S.path == RECYCLE && read_binned(paths[0], binned)) {  // in the Recycle Bin
        Entry e;
        e.name = base_name(binned.original);
        e.dir = is_dir(paths[0]);
        e.icon = icon_for(e.name, e.dir);
        const std::string head = "Type: " + type_text(e) + "\nOriginal location: " + folder_of(binned.original) + "\nDeleted: " + date_text(binned.when);
        show_message(e.name + " properties", "");
        if (e.dir) {
            S.dialog.head = head;
            S.dialog.folder = true;
            S.dialog.text = head + COUNTING;
            S.dialog.counting = count_sizes({paths[0]});
        } else {
            struct stat st{};
            S.dialog.text = head + (lstat(paths[0].c_str(), &st) == 0 ? "\nSize: " + size_text(st.st_size) : std::string());
        }
        return;
    }
    if (paths.size() > 1) {
        show_message(std::to_string(paths.size()) + " items", "");
        S.dialog.head = "Location: " + shown_path(folder_of(paths[0]));
        S.dialog.text = S.dialog.head + COUNTING;
        S.dialog.counting = count_sizes(paths);
        return;
    }
    const std::string path = paths[0];
    struct stat st{};
    if (stat(path.c_str(), &st) != 0) {
        show_message("Properties", "It can't be read: " + std::string(std::strerror(errno)));
        return;
    }
    Entry e;
    e.name = base_name(path);
    e.dir = S_ISDIR(st.st_mode);
    e.icon = icon_for(e.name, e.dir);
    const std::string head = "Type: " + type_text(e) + "\nLocation: " + shown_path(folder_of(path));
    std::string tail;
    if (e.dir) {
        const std::string room = space_note(path);
        if (!room.empty())
            tail += "\nDrive: " + room;
    }
    tail += "\nModified: " + date_text(st.st_mtime) + "\nAccessed: " + date_text(st.st_atime);
    char owner[96];
    std::snprintf(owner, sizeof owner, "\nPermissions: %s (%o), owner %u, group %u", mode_text(st.st_mode).c_str(),
                  static_cast<unsigned>(st.st_mode & 07777), static_cast<unsigned>(st.st_uid), static_cast<unsigned>(st.st_gid));
    tail += owner;
    show_message(e.name + " properties", "");
    if (e.dir) {
        S.dialog.head = head;
        S.dialog.tail = tail;
        S.dialog.folder = true;
        S.dialog.text = head + COUNTING + tail;
        S.dialog.counting = count_sizes({path});
    } else {
        S.dialog.text = head + "\nSize: " + size_text(st.st_size) + " (" + std::to_string(static_cast<long long>(st.st_size)) + " bytes)" + tail;
    }
}

// --- commands --------------------------------------------------------------------------------------

// The name typed for path: renamed, and still chosen
void finish_rename(const std::string &path, std::string name) {
    while (!name.empty() && name.back() == ' ')
        name.pop_back();
    while (!name.empty() && name.front() == ' ')
        name.erase(name.begin());
    if (name.empty() || name == base_name(path))
        return;
    if (name.find('/') != std::string::npos || name == "." || name == "..") {
        report("Rename", "A name can't contain / or be . or ..");
        return;
    }
    const std::string to = join(folder_of(path), name);
    const std::string error = shell::rename_entry(path, to);
    if (!error.empty()) {
        report("Rename", error);
        return;
    }
    for (std::string &p : S.clipboard.paths)  // (what Cut or Copy holds goes with it)
        if (p == path)
            p = to;
    if (S.visible && S.path == folder_of(path))
        refresh(to);
}

// F2 / Rename: typed in place on a USB keyboard (the name without its extension chosen, as Windows
// does), else on the PS4's keyboard
void start_rename(const std::string &path, const std::string &title) {
    if (shell::hwkeyboard_connected()) {
        const std::string name = base_name(path);
        const size_t dot = is_dir(path) ? std::string::npos : name.find_last_of('.');
        S.rename = LineEdit{true, name, dot == std::string::npos || dot == 0 ? name.size() : dot, 0};
        S.rename_path = path;
        S.address.active = S.search.active = false;
        S.dirty = true;
        return;
    }
    ask(title, base_name(path), [path](const std::string &text) { finish_rename(path, text); });
}

void commit_rename() {
    if (!S.rename.active)
        return;
    S.rename.active = false;
    const std::string path = S.rename_path, text = S.rename.text;
    finish_rename(path, text);
}

// New folder: made at once, then named (as Windows does)
void make_new_folder() {
    if (S.path.empty() || read_only(S.path) || S.path == RECYCLE)
        return;
    const std::string path = join(S.path, shell::unique_name(S.path, "New folder", ""));
    const std::string error = shell::make_folder(path);
    if (!error.empty()) {
        show_message("New folder", error);
        return;
    }
    refresh(path);
    start_rename(path, "Name the new folder");
}

void cut_copy(const std::vector<std::string> &paths, bool cut) {
    if (paths.empty())
        return;
    S.clipboard = Clipboard{paths, cut};
    const std::string what = paths.size() == 1 ? base_name(paths[0]) : std::to_string(paths.size()) + " items";
    toast(std::string(cut ? "Cut " : "Copied ") + what + ": open a folder and Paste");
    S.dirty = true;
}

void paste_into(const std::string &folder) {
    if (S.clipboard.paths.empty() || folder.empty())
        return;
    if (folder == RECYCLE) {
        toast("Delete moves things to the Recycle Bin");
        return;
    }
    if (read_only(folder)) {
        toast(in_archive(folder) ? "Nothing can be put into an archive here: make a new one with Compress"
                                 : "The app's own files can't change");
        return;
    }
    // copied inside an archive: taken out of it (an archive itself is copied as any file)
    if (inside_archive(S.clipboard.paths[0])) {
        extract_entries(S.clipboard.paths, folder);
        return;
    }
    if (!shell::job_paste(S.clipboard.paths, S.clipboard.cut, folder)) {
        show_message("Paste", "Wait for the copy that is running to finish.");
        return;
    }
    if (S.clipboard.cut)
        S.clipboard = Clipboard{};  // moved: nothing left to paste
    S.job_drawn = 0;
    S.dirty = true;
}

// Delete: for good (there is no recycle bin), after a question
void ask_delete(const std::vector<std::string> &paths) {
    if (paths.empty())
        return;
    const bool one = paths.size() == 1;
    show_message(one ? "Delete " + base_name(paths[0]) + "?" : "Delete these " + std::to_string(paths.size()) + " items?",
                 one && is_dir(paths[0]) ? "The folder and everything in it are deleted for good."
                 : one                   ? "It is deleted for good."
                                         : "They are deleted for good.");
    S.dialog.kind = DialogKind::Delete;
    S.dialog.paths = paths;
}

// --- the Recycle Bin's jobs ------------------------------------------------------------------------------

std::string items_text(size_t n) { return std::to_string(n) + (n == 1 ? " item" : " items"); }

// paths to their drives' bins, each with its record (a rename each)
bool recycle(const std::vector<std::string> &paths) {
    std::vector<std::string> touched{RECYCLE};
    for (const std::string &p : paths)
        touched.push_back(folder_of(p));
    const std::string title = "Moving " + (paths.size() == 1 ? base_name(paths[0]) : items_text(paths.size())) + " to the Recycle Bin";
    return shell::job_run(title, [paths](shell::JobControl &job) -> std::string {
        job.set_counts_items(true);
        job.set_total(static_cast<long long>(paths.size()));
        static unsigned serial = 0;
        for (const std::string &p : paths) {
            if (job.cancelled())
                return {};
            job.set_current(base_name(p));
            const std::string bin = bin_of(p);
            if (!make_dirs(bin))
                return "Could not make the Recycle Bin's folder: " + std::string(std::strerror(errno));
            std::string to;
            do {
                char id[48];
                std::snprintf(id, sizeof id, "R%llx-%u", static_cast<unsigned long long>(std::time(nullptr)), ++serial);
                to = bin + "/" + id;
            } while (access(to.c_str(), F_OK) == 0);
            FILE *f = std::fopen((to + ".info").c_str(), "w");
            if (!f)
                return "Could not write the Recycle Bin's record: " + std::string(std::strerror(errno));
            std::fprintf(f, "path=%s\ntime=%lld\n", p.c_str(), static_cast<long long>(std::time(nullptr)));
            std::fclose(f);
            if (rename(p.c_str(), to.c_str()) != 0) {
                const std::string why = std::strerror(errno);
                unlink((to + ".info").c_str());
                return "Could not move " + base_name(p) + " to the Recycle Bin: " + why;
            }
            job.add_done(1);
        }
        return {};
    }, touched);
}

// Items in a bin back where they were
bool restore(const std::vector<std::string> &items) {
    std::vector<std::string> touched{RECYCLE};
    for (const std::string &item : items) {
        Binned b;
        if (read_binned(item, b))
            touched.push_back(folder_of(b.original));
    }
    return shell::job_run("Restoring " + items_text(items.size()), [items](shell::JobControl &job) -> std::string {
        job.set_counts_items(true);
        job.set_total(static_cast<long long>(items.size()));
        for (const std::string &item : items) {
            if (job.cancelled())
                return {};
            Binned b;
            if (!read_binned(item, b))
                continue;
            job.set_current(base_name(b.original));
            const std::string folder = folder_of(b.original);
            if (!make_dirs(folder))
                return "Could not make " + folder + " again: " + std::strerror(errno);
            std::string to = b.original;
            struct stat st{};
            if (lstat(to.c_str(), &st) == 0) {  // (the name is taken there now: "name (2)")
                const std::string name = base_name(to);
                const size_t dot = is_dir(item) ? std::string::npos : name.find_last_of('.');
                const bool split = dot != std::string::npos && dot > 0;
                to = join(folder, shell::unique_name(folder, split ? name.substr(0, dot) : name, split ? name.substr(dot) : std::string()));
            }
            if (rename(item.c_str(), to.c_str()) != 0)
                return "Could not restore " + base_name(b.original) + ": " + std::strerror(errno);
            unlink((item + ".info").c_str());
            job.add_done(1);
        }
        return {};
    }, touched);
}

// Items in a bin (all: the bin emptied) deleted for good with their records, after a question
void ask_delete_binned(const std::vector<std::string> &items, bool all) {
    if (items.empty())
        return;
    std::vector<std::string> paths;
    for (const std::string &item : items) {
        paths.push_back(item);
        paths.push_back(item + ".info");
    }
    Binned b;
    const bool one = items.size() == 1;
    const std::string name = one && read_binned(items[0], b) ? base_name(b.original) : base_name(items[0]);
    show_message(all ? "Empty the Recycle Bin?" : one ? "Delete " + name + " for good?" : "Delete these " + items_text(items.size()) + " for good?",
                 all ? "The " + items_text(items.size()) + " in it are deleted for good." : one ? "It is deleted for good." : "They are deleted for good.");
    S.dialog.kind = DialogKind::Delete;
    S.dialog.paths = paths;
}

// Delete: to the Recycle Bin when everything chosen has one (and is no bin, nor holds one); else, and
// for good (Shift+Delete, "Delete for good"), deleted for good after a question. In the bin: for good.
void delete_paths(const std::vector<std::string> &paths, bool for_good) {
    if (paths.empty())
        return;
    if (S.path == RECYCLE) {
        ask_delete_binned(paths, false);
        return;
    }
    bool to_bin = !for_good;
    for (const std::string &p : paths) {
        const std::string bin = bin_of(p);
        if (bin.empty() || within(p, bin) || within(bin, p))
            to_bin = false;
    }
    if (!to_bin) {
        ask_delete(paths);
        return;
    }
    if (!recycle(paths))
        show_message("Delete", "Wait for the job that is running to finish.");
    else
        S.job_drawn = 0;
}

// New text document: made at once, then named (as Windows does)
void make_new_text() {
    if (S.path.empty() || read_only(S.path) || S.path == RECYCLE)
        return;
    const std::string path = join(S.path, shell::unique_name(S.path, "New Text Document", ".txt"));
    const std::string error = shell::make_file(path);
    if (!error.empty()) {
        show_message("New text document", error);
        return;
    }
    refresh(path);
    start_rename(path, "Name the new text document");
}

// --- the address bar's parts -------------------------------------------------------------------------

struct Crumb {
    std::string label, path;
};

// This PS4, the place holding the folder (else the console's root), then the folder's parts below it
std::vector<Crumb> crumbs(const std::string &path) {
    std::vector<Crumb> list{{"This PS4", ""}};
    if (path.empty())
        return list;
    std::string at, rest;
    const int p = place_of(path);
    if (p >= 0) {
        const Entry &root = places()[p];
        list.push_back({root.name, root.path});
        at = root.path;
        rest = path.substr(root.path.size());
    } else {
        list.push_back({"PS4 (/)", "/"});
        rest = path == "/" ? std::string() : path;
    }
    for (size_t i = 1; i <= rest.size() && !rest.empty();) {
        const size_t end = std::min(rest.find('/', i), rest.size());
        if (end > i) {
            at += "/" + rest.substr(i, end - i);
            list.push_back({rest.substr(i, end - i), at});
        }
        i = end + 1;
    }
    return list;
}

// The file at path, or the one whose names match it but for case (typed: the console's file systems
// mind case, "/data/wisp" is /data/Wisp). Names there as typed are taken as they are: only a folder
// holding a name that isn't is read (the app may not be let read / itself).
std::string existing_path(const std::string &path) {
    struct stat st{};
    if (path.empty() || path[0] != '/' || stat(path.c_str(), &st) == 0)
        return path;
    std::string found;
    size_t at = 1;
    while (at <= path.size()) {
        const size_t end = std::min(path.find('/', at), path.size());
        const std::string name = path.substr(at, end - at), dir = found.empty() ? "/" : found;
        at = end + 1;
        if (name.empty())
            continue;  // (a doubled slash)
        if (stat((found + "/" + name).c_str(), &st) == 0) {
            found += "/" + name;
            continue;
        }
        std::string match;
        if (DIR *d = opendir(dir.c_str())) {
            while (dirent *e = readdir(d))
                if (lower(e->d_name) == lower(name)) {
                    match = e->d_name;
                    break;
                }
            closedir(d);
        }
        if (match.empty())
            return path;
        found += "/" + match;
    }
    return found.empty() ? path : found;
}

// The address typed: a folder goes there, a file opens its folder with it chosen
void go_to_address(const std::string &typed) {
    std::string t = typed;
    while (!t.empty() && t.back() == ' ')
        t.pop_back();
    while (!t.empty() && t.front() == ' ')
        t.erase(t.begin());
    if (t.rfind("file://", 0) == 0)
        t = from_file_url(t);
    if (t.empty() || lower(t) == "this ps4") {
        navigate("");
        return;
    }
    if (lower(t) == "recycle bin" || t == RECYCLE) {
        navigate(RECYCLE);
        return;
    }
    if (t[0] != '/')  // (a name in the folder shown)
        t = join(S.path.empty() ? "/" : S.path, t);
    while (t.size() > 1 && t.back() == '/')
        t.pop_back();
    if (in_archive(t)) {  // (a folder inside an archive, or the archive itself: opened as a folder)
        navigate(t);
        return;
    }
    const std::string path = existing_path(t);
    if (is_dir(path)) {
        navigate(path);
    } else if (is_file(path)) {
        navigate(folder_of(path));
        refresh(path);
    } else {
        show_message("Files", "OrbisWeb can't find '" + typed + "'. Check the spelling and try again.");
    }
}

// Up: a place's own folder goes to This PS4 (the places), as the address bar has it
void go_up() {
    if (S.path.empty())
        return;
    for (const Entry &p : places())
        if (S.path == p.path) {
            navigate("");
            return;
        }
    navigate(S.path == "/" ? std::string() : folder_of(S.path));
}

void go_back() {
    if (S.back.empty())
        return;
    const std::string to = S.back.back();
    S.back.pop_back();
    S.forward.push_back(S.path);
    navigate(to, false);
}

void go_forward() {
    if (S.forward.empty())
        return;
    const std::string to = S.forward.back();
    S.forward.pop_back();
    S.back.push_back(S.path);
    navigate(to, false);
}

// An entry opened: a folder here, an archive as a folder (as Windows opens one), a file inside an
// archive taken out then opened, any other file by the browser (a copy of it: the browser may change Files)
void open_entry(Entry e) {
    if (S.path == RECYCLE) {  // (in the Recycle Bin: its properties, as Windows does; Restore brings it back)
        show_properties({e.path});
        return;
    }
    if (e.dir) {
        navigate(e.path);
        return;
    }
    std::string file, inside;
    if (in_archive(e.path, file, inside) && !inside.empty()) {
        open_in_archive(file, inside);
        return;
    }
    if (archive::available() && archive::is_archive_name(e.name)) {
        navigate(e.path);
        return;
    }
    if (browser.open_file)
        browser.open_file(e.path);
}

void close_files() {
    if (!S.visible)
        return;
    commit_rename();  // (the name typed is kept, as a click elsewhere keeps it)
    S.visible = false;
    S.menu.open = false;
    S.dialog = Dialog{};
    S.dragged = DraggedFiles{};
    S.banding = false;
    S.address.active = S.search.active = false;
    S.hover.clear();
    S.dirty = true;
}

// The address bar to type into: on a USB keyboard there, else on the PS4's keyboard
void start_address() {
    commit_rename();
    if (shell::hwkeyboard_connected()) {
        S.address = LineEdit{true, S.path, S.path.size(), 0};
        S.search.active = false;
        S.dirty = true;
        return;
    }
    ask("Go to (/data, /mnt/usb0, ...)", S.path, [](const std::string &text) {
        if (S.visible)
            go_to_address(text);
    });
}

void start_search() {
    commit_rename();
    if (shell::hwkeyboard_connected()) {
        S.search.active = true;
        S.search.cursor = S.search.text.size();
        S.search.anchor = 0;
        S.address.active = false;
        S.dirty = true;
        return;
    }
    ask("Search " + place_title(S.path), S.filter, [](const std::string &text) {
        if (!S.visible)
            return;
        S.filter = S.search.text = text;
        load_entries();
        S.scroll = 0;
        S.chosen.clear();
        S.cursor = S.anchor = -1;
    });
}

// --- the commands: the command bar, the menu and the keyboard ----------------------------------------

// (the command bar's buttons drawn faded do nothing)
bool command_enabled(const std::string &key) {
    const bool place = S.path.empty(), ro = read_only(S.path), any = !S.chosen.empty(), bin = S.path == RECYCLE;
    if (key == "new-folder")
        return !place && !ro && !bin;
    if (key == "delete")
        return any && !place && !ro;
    if (key == "cut")
        return any && !place && !ro && !bin;
    if (key == "copy")
        return any && !place && !bin;
    if (key == "rename")
        return S.chosen.size() == 1 && !place && !ro && !bin;
    if (key == "paste")
        return !S.clipboard.paths.empty() && !place && !ro && !bin;
    if (key == "sort")
        return !place;
    return true;
}

void files_command(const std::string &cmd, int row = -1) {
    const bool place = S.path.empty(), ro = read_only(S.path);
    const Entry *e = row >= 0 && row < static_cast<int>(S.entries.size()) ? &S.entries[row] : nullptr;
    if (!e && (cmd == "open" || (cmd == "properties" && place)) && S.cursor >= 0 && S.cursor < static_cast<int>(S.entries.size()))
        e = &S.entries[S.cursor];
    if (cmd == "open") {
        if (e)
            open_entry(*e);
    } else if (cmd == "cut" || cmd == "copy" || cmd == "rename" || cmd == "delete" || cmd == "delete-for-good" || cmd == "paste" ||
               cmd == "new-folder" || cmd == "new-text") {
        if (place)
            return;
        if (S.path == RECYCLE && cmd != "delete" && cmd != "delete-for-good") {
            toast("In the Recycle Bin: Restore puts things back, Delete deletes them for good");
            return;
        }
        if (ro && cmd != "copy") {
            toast(in_archive(S.path) ? "Inside an archive nothing changes: copy out, or Extract All" : "The app's own files can't change");
            return;
        }
        if (cmd == "cut" || cmd == "copy") {
            cut_copy(subject(row), cmd == "cut");
        } else if (cmd == "rename") {
            const std::vector<std::string> list = subject(row);
            if (!list.empty())
                start_rename(list[0], "Rename " + base_name(list[0]));
        } else if (cmd == "delete" || cmd == "delete-for-good") {
            delete_paths(subject(row), cmd == "delete-for-good");
        } else if (cmd == "new-text") {
            make_new_text();
        } else if (cmd == "paste") {
            paste_into(S.path);
        } else {
            make_new_folder();
        }
    } else if (cmd == "paste-into") {
        if (e && e->dir)
            paste_into(e->path);
    } else if (cmd == "properties") {
        if (place) {
            if (e)
                show_place_properties(*e);
        } else if (e || !S.chosen.empty()) {
            show_properties(subject(row));
        } else {
            show_properties({S.path});
        }
    } else if (cmd == "restore" || cmd == "restore-all") {
        if (S.path != RECYCLE)
            return;
        std::vector<std::string> list;
        if (cmd == "restore")
            list = subject(row);
        else
            for (const Entry &x : S.entries)
                list.push_back(x.path);
        if (!list.empty() && !restore(list))
            show_message("Restore", "Wait for the job that is running to finish.");
        S.job_drawn = 0;
    } else if (cmd == "empty-bin") {
        std::vector<std::string> list;
        for (const Entry &x : bin_entries())
            list.push_back(x.path);
        if (list.empty())
            toast("The Recycle Bin is empty");
        else
            ask_delete_binned(list, true);
    } else if (cmd == "select-none") {
        S.chosen.clear();
    } else if (cmd == "invert-selection") {
        std::set<std::string> inverted;
        for (const Entry &x : S.entries)
            if (!S.chosen.count(x.path))
                inverted.insert(x.path);
        S.chosen.swap(inverted);
    } else if (cmd == "extract-all" || cmd == "extract-here") {
        // the archive: the entry's, or (inside one) the archive shown; Extract All into a new folder
        // named after it (then shown, as Windows does), Extract here next to it
        std::string file, inside;
        if (e && !e->dir && !inside_archive(e->path) && archive::is_archive_name(e->name))
            file = e->path;
        else if (!in_archive(S.path, file, inside))
            return;
        const std::string dir = folder_of(file);
        if (read_only(dir)) {
            toast("The app's own files can't change: copy the archive somewhere else first");
            return;
        }
        ArchiveJob job;
        job.file = file;
        job.title = "Extracting " + base_name(file);
        if (cmd == "extract-all") {
            job.folder = join(dir, shell::unique_name(dir, archive::stem_of(file), ""));
            job.then_go = job.folder;
        } else {
            job.folder = dir;
        }
        start_extract(job);
    } else if (cmd == "extract-beside") {
        // entries chosen inside an archive, into the folder the archive is in
        std::string file, inside;
        if (!in_archive(S.path, file, inside))
            return;
        if (read_only(folder_of(file))) {
            toast("The app's own files can't change: copy what you chose somewhere else");
            return;
        }
        extract_entries(subject(row), folder_of(file));
    } else if (cmd == "compress-zip" || cmd == "compress-7z") {
        if (place || ro)
            return;
        const std::vector<std::string> list = subject(row);
        if (list.empty())
            return;
        const archive::Format format = cmd == "compress-zip" ? archive::Format::Zip : archive::Format::SevenZip;
        // named after the entry (a file's name without its extension), or the folder for several
        std::string stem = list.size() == 1 ? base_name(list[0]) : place_title(S.path);
        if (list.size() == 1 && !is_dir(list[0])) {
            const size_t dot = stem.find_last_of('.');
            if (dot != std::string::npos && dot > 0)
                stem = stem.substr(0, dot);
        }
        const std::string target = join(S.path, shell::unique_name(S.path, stem, archive::extension_of(format)));
        if (!archive::job_compress(list, target, format)) {
            show_message("Compress", "Wait for the job that is running to finish.");
            return;
        }
        S.archive_job = ArchiveJob{};
        S.archive_job.then_select = target;
        S.archive_job_running = true;
        S.job_drawn = 0;
    } else if (cmd.size() == 5 && cmd.rfind("sort", 0) == 0) {
        if (place)
            return;
        const int by = cmd[4] - '0';
        S.sort_down = S.sort_by == by ? !S.sort_down : by == SORT_DATE || by == SORT_SIZE;
        S.sort_by = by;
        refresh();
    } else if (cmd == "refresh") {
        refresh();
    } else if (cmd == "select-all") {
        choose_all();
    } else if (cmd == "up") {
        go_up();
    } else if (cmd == "back") {
        go_back();
    } else if (cmd == "forward") {
        go_forward();
    }
    S.dirty = true;
}

// --- dragging entries: onto a folder, a place or a part of the address ------------------------------

// The folder a drop at key goes into ("" for none): a folder's row, a place, a part of the address,
// the folder shown
std::string drop_folder(const std::string &key) {
    if (key.rfind("row", 0) == 0 || key.rfind("check", 0) == 0) {
        const int row = number_after(key, key[0] == 'r' ? "row" : "check");
        if (row >= 0 && row < static_cast<int>(S.entries.size()) && S.entries[row].dir)
            return S.entries[row].path;
    }
    if (key.rfind("nav", 0) == 0) {
        const std::vector<Entry> list = nav_items();
        const int i = number_after(key, "nav");
        return i >= 0 && i < static_cast<int>(list.size()) ? list[i].path : std::string();
    }
    if (key.rfind("crumb", 0) == 0) {
        const std::vector<Crumb> list = crumbs(S.path);
        const int i = number_after(key, "crumb");
        return i >= 0 && i < static_cast<int>(list.size()) ? list[i].path : std::string();
    }
    if (key.empty())
        return {};  // (outside Files)
    return S.path;  // ("" for This PS4: nothing)
}

// What a drop into folder does: 0 nothing, 1 move, 2 copy (a move on the same drive, a copy onto
// another; Ctrl copies, Shift moves; the app's own files are only copied)
int drop_effect(const std::string &folder) {
    const std::vector<std::string> &paths = S.dragged.paths;
    if (paths.empty() || S.path == RECYCLE)
        return 0;  // (out of the Recycle Bin: Restore)
    if (folder == RECYCLE)  // onto the Recycle Bin: deleted (the app's own files and an archive's can't be)
        return std::any_of(paths.begin(), paths.end(), read_only) ? 0 : 1;
    if (folder.empty() || read_only(folder))
        return 0;
    for (const std::string &p : paths)
        if (folder == p || folder.rfind(p + "/", 0) == 0)
            return 0;  // (onto itself, into itself)
    if (std::all_of(paths.begin(), paths.end(), [&](const std::string &p) { return folder_of(p) == folder; }))
        return 0;  // (where they are already)
    if (S.key_ctrl || std::any_of(paths.begin(), paths.end(), read_only))
        return 2;
    if (S.key_shift)
        return 1;
    return same_drive(paths[0], folder) ? 1 : 2;
}

// Let go over key
void drop_dragged(const std::string &key) {
    const std::string folder = drop_folder(key);
    const int effect = drop_effect(folder);
    if (!effect)
        return;
    if (folder == RECYCLE) {
        delete_paths(S.dragged.paths, false);
        return;
    }
    if (inside_archive(S.dragged.paths[0])) {  // dragged out of an archive: taken out of it (an archive itself moves as any file)
        extract_entries(S.dragged.paths, folder);
        return;
    }
    if (!shell::job_paste(S.dragged.paths, effect == 1, folder))
        show_message(effect == 1 ? "Move" : "Copy", "Wait for the copy that is running to finish.");
    else
        S.job_drawn = 0;
}

// --- the menu ----------------------------------------------------------------------------------------

void open_menu(const std::string &target, float px, float py) {
    if (target.empty())
        return;
    std::vector<MenuItem> items;
    const auto sep = [&] { items.push_back({"", "", false}); };
    const bool place = S.path.empty(), ro = read_only(S.path);
    static const char *const sorts[] = {"Name", "Date modified", "Type", "Size"};
    static const char *const bin_sorts[] = {"Name", "Date deleted", "Type", "Size"};
    const auto sort_items = [&](const char *prefix) {
        for (int i = 0; i < 4; i++)
            items.push_back({std::string(S.sort_by == i ? (S.sort_down ? "\xe2\x86\x93 " : "\xe2\x86\x91 ") : "    ") + prefix +
                                 (S.path == RECYCLE ? bin_sorts : sorts)[i],
                             "sort" + std::to_string(i), !place});
    };
    if (target == "close" || target == "title") {
        items.push_back({"Close Files", "close"});
    } else if (target == "menusort") {
        sort_items("");
    } else if ((target.rfind("row", 0) == 0 || target.rfind("check", 0) == 0) && S.path == RECYCLE) {  // what is in the Recycle Bin
        const int row = number_after(target, target[0] == 'r' ? "row" : "check");
        if (row < 0 || row >= static_cast<int>(S.entries.size()))
            return;
        const bool many = subject(row).size() > 1;
        items.push_back({"Restore", "restore"});
        sep();
        items.push_back({"Delete for good", "delete"});
        sep();
        items.push_back({"Properties", "properties", !many});
    } else if (target.rfind("row", 0) == 0 || target.rfind("check", 0) == 0) {
        const int row = number_after(target, target[0] == 'r' ? "row" : "check");
        if (row < 0 || row >= static_cast<int>(S.entries.size()))
            return;
        const std::vector<std::string> list = subject(row);
        const Entry &e = S.entries[row];
        const bool many = list.size() > 1;
        std::string archive_file, inside;
        const bool in_arch = in_archive(S.path, archive_file, inside);
        const bool an_archive = !e.dir && !many && !in_arch && archive::available() && archive::is_archive_name(e.name);
        if (!many)
            items.push_back({"Open", "open"});
        if (an_archive) {
            items.push_back({"Extract All", "extract-all", !ro});
            items.push_back({"Extract here", "extract-here", !ro});
        }
        if (in_arch)
            items.push_back({"Extract to " + place_title(folder_of(archive_file)), "extract-beside", !read_only(folder_of(archive_file))});
        if (!place) {
            if (!many)
                sep();
            items.push_back({"Cut", "cut", !ro});
            items.push_back({"Copy", "copy"});
            if (e.dir && !many && !in_arch)
                items.push_back({"Paste into folder", "paste-into", !S.clipboard.paths.empty() && !ro});
            sep();
            if (!in_arch && archive::available()) {
                items.push_back({"Compress to ZIP file", "compress-zip", !ro});
                items.push_back({"Compress to 7z file", "compress-7z", !ro});
                sep();
            }
            items.push_back({"Rename", "rename", !many && !ro});
            items.push_back({"Delete", "delete", !ro});
            items.push_back({"Delete for good", "delete-for-good", !ro});
        }
        sep();
        items.push_back({"Properties", "properties"});
    } else if (S.path == RECYCLE) {  // the Recycle Bin's own
        sort_items("Sort by ");
        items.push_back({"Refresh", "refresh"});
        sep();
        items.push_back({"Restore all items", "restore-all", !S.entries.empty()});
        items.push_back({"Empty Recycle Bin", "empty-bin", !S.entries.empty()});
        sep();
        items.push_back({"Select all", "select-all"});
    } else {  // the folder's own (empty space, the bars)
        sort_items("Sort by ");
        items.push_back({"Refresh", "refresh"});
        std::string archive_file, inside;
        if (in_archive(S.path, archive_file, inside)) {
            sep();
            items.push_back({"Extract All", "extract-all", !read_only(folder_of(archive_file))});
        }
        if (!place) {
            sep();
            items.push_back({"Paste", "paste", !S.clipboard.paths.empty() && !ro});
            sep();
            items.push_back({"New folder", "new-folder", !ro});
            items.push_back({"New text document", "new-text", !ro});
            sep();
            items.push_back({"Select all", "select-all"});
            items.push_back({"Select none", "select-none", !S.chosen.empty()});
            items.push_back({"Invert selection", "invert-selection"});
            items.push_back({"Properties", "properties"});
        }
    }
    int h = 8;
    for (const MenuItem &it : items)
        h += it.action.empty() ? MENU_SEPARATOR_H : MENU_ITEM_H;
    S.menu.open = true;
    S.menu.items = std::move(items);
    S.menu.target = target;
    // at the pointer, inside Files
    S.menu.x = std::clamp(static_cast<int>(px), 8, std::max(8, S.w - MENU_W - 8));
    S.menu.y = static_cast<int>(py) + h > S.h - 4 ? std::max(8, static_cast<int>(py) - h) : static_cast<int>(py);
    S.dirty = true;
}

void menu_action(const std::string &action) {
    const std::string target = S.menu.target;
    S.menu.open = false;
    S.dirty = true;
    if (action == "close") {
        close_files();
        return;
    }
    const bool row_part = target.rfind("row", 0) == 0 || target.rfind("check", 0) == 0;
    files_command(action, row_part ? number_after(target, target[0] == 'r' ? "row" : "check") : -1);
}

// --- hit testing and clicks --------------------------------------------------------------------------

// The dialog: centred in Files, its buttons along its foot (0 the primary: OK, Delete)
int dialog_lines() { return 1 + static_cast<int>(std::count(S.dialog.text.begin(), S.dialog.text.end(), '\n')); }
Rect dialog_rect() {
    const int h = 200 + 26 * std::max(0, dialog_lines() - 1) + (S.dialog.text.size() > 60 && dialog_lines() == 1 ? 22 : 0);
    return Rect{S.w / 2 - 300, S.h / 2 - h / 2, 600, h};
}
Rect dialog_button(int i) {
    const Rect d = dialog_rect();
    const int n = S.dialog.buttons(), bw = (540 - (n - 1) * 16) / n;
    return Rect{d.x + 30 + i * (bw + 16), d.y + d.h - 64, bw, 44};
}

// The menu's items (separators are thinner)
Rect menu_item_rect(int i) {
    int y = S.menu.y + 4;
    for (int k = 0; k < i; k++)
        y += S.menu.items[k].action.empty() ? MENU_SEPARATOR_H : MENU_ITEM_H;
    return Rect{S.menu.x + 4, y, MENU_W - 8, S.menu.items[i].action.empty() ? MENU_SEPARATOR_H : MENU_ITEM_H};
}
Rect menu_rect() {
    int h = 8;
    for (const MenuItem &it : S.menu.items)
        h += it.action.empty() ? MENU_SEPARATOR_H : MENU_ITEM_H;
    return Rect{S.menu.x, S.menu.y, MENU_W, h};
}

// What's at (px, py) in Files, as a key: hover changes redraw, clicks compare it for Cross twice
// A running job's card (bottom right of the list) and its Cancel button
Rect job_card() { return Rect{S.w - 440 - 16, files_bottom() - 16 - 104, 440, 104}; }
Rect job_cancel_rect() {
    const Rect card = job_card();
    return Rect{card.x + card.w - 22 - 88, card.y + 14, 88, 30};
}

std::string files_hit(float px, float py) {
    if (close_button().contains(px, py))
        return "close";
    if (shell::job_status().running && job_card().contains(px, py))
        return job_cancel_rect().contains(px, py) ? "job-cancel" : "job";
    if (py < TITLE_H)
        return "title";
    for (int i = 0; i < CMD_COUNT; i++)
        if (*FILES_CMDS[i].key && files_cmd_rect(i).contains(px, py))
            return std::string("cmd:") + FILES_CMDS[i].key;
    static const char *const nav_buttons[] = {"back", "forward", "up", "refresh"};
    for (int i = 0; i < 4; i++)
        if (files_nav_button(i).contains(px, py))
            return nav_buttons[i];
    if (files_address().contains(px, py)) {
        if (!S.address.active)
            for (size_t i = 0; i < S.crumb_rects.size(); i++)
                if (S.crumb_rects[i].contains(px, py))
                    return "crumb" + std::to_string(i);
        return "addr";
    }
    if (files_search().contains(px, py))
        return "search";
    const int nav_count = static_cast<int>(places().size()) + 1;
    for (int i = 0; i < nav_count; i++) {
        const Rect r = files_nav_item(i);
        if (r.y + r.h > files_bottom() - 4)
            break;
        if (r.contains(px, py))
            return "nav" + std::to_string(i);
    }
    if (files_header().contains(px, py)) {
        for (int i = 3; i > 0; i--)
            if (px >= files_column_x(i) - 10)
                return "col" + std::to_string(i);
        return "col0";
    }
    const Rect list = files_list();
    if (list.contains(px, py)) {
        const int row = static_cast<int>((py - list.y) / ROW_H) + static_cast<int>(S.scroll);
        if (row < static_cast<int>(S.entries.size()))
            return (px < list.x + 36 ? "check" : "row") + std::to_string(row);
        return "list";
    }
    return "body";
}

std::string hit(float px, float py) {
    if (S.dialog.open) {
        for (int i = 0; i < S.dialog.buttons(); i++)
            if (dialog_button(i).contains(px, py))
                return "dialog:" + std::to_string(i);
        return "dialog";
    }
    if (S.menu.open) {
        if (menu_rect().contains(px, py)) {
            for (size_t i = 0; i < S.menu.items.size(); i++)
                if (!S.menu.items[i].action.empty() && menu_item_rect(static_cast<int>(i)).contains(px, py))
                    return "menu:" + std::to_string(i);
            return "menu";
        }
        return "outside-menu";
    }
    if (px < 0 || py < 0 || px >= S.w || py >= S.h)
        return {};
    return files_hit(px, py);
}

void dialog_click(int i) {
    const Dialog d = S.dialog;
    S.dialog.open = false;
    S.dialog.counting = 0;
    S.dirty = true;
    if (i == 0 && d.kind == DialogKind::Delete) {
        if (!shell::job_delete(d.paths))
            show_message("Delete", "Wait for the copy that is running to finish.");
        else
            S.job_drawn = 0;
    }
}

void files_click(const std::string &part, bool twice, float px, float py) {
    // a click elsewhere ends typing in a box (the name typed is kept, as Windows does)
    if (S.rename.active && part.rfind("row", 0) != 0)
        commit_rename();
    if (part != "addr")
        S.address.active = false;
    if (part != "search")
        S.search.active = false;
    const int n = static_cast<int>(S.entries.size());
    if (part == "job-cancel") {
        shell::job_cancel();
        S.dirty = true;
        return;
    }
    if (part == "job")
        return;  // (the card: no click goes through it to the list)
    if (part.rfind("cmd:", 0) == 0) {
        const std::string cmd = part.substr(4);
        if (!command_enabled(cmd))
            return;
        if (cmd == "sort") {  // its drop-down menu
            for (int i = 0; i < CMD_COUNT; i++)
                if (cmd == FILES_CMDS[i].key) {
                    const Rect r = files_cmd_rect(i);
                    open_menu("menusort", static_cast<float>(r.x), static_cast<float>(r.y + r.h + 4));
                }
            return;
        }
        files_command(cmd);
    } else if (part == "back" || part == "forward" || part == "up" || part == "refresh") {
        files_command(part);
    } else if (part.rfind("crumb", 0) == 0) {
        const std::vector<Crumb> list = crumbs(S.path);
        const int i = number_after(part, "crumb");
        if (i >= 0 && i < static_cast<int>(list.size()))
            navigate(list[i].path);
    } else if (part == "addr") {
        if (!S.address.active)
            start_address();
    } else if (part == "search") {
        if (!S.search.active)
            start_search();
    } else if (part.rfind("nav", 0) == 0) {
        const std::vector<Entry> list = nav_items();
        const int i = number_after(part, "nav");
        if (i >= 0 && i < static_cast<int>(list.size()))
            navigate(list[i].path);
    } else if (part.rfind("col", 0) == 0) {
        if (!S.path.empty())
            files_command("sort" + std::to_string(number_after(part, "col")));
    } else if (part.rfind("check", 0) == 0) {
        choose_toggle(number_after(part, "check"));
    } else if (part.rfind("row", 0) == 0) {
        int row = number_after(part, "row");
        if (row < 0 || row >= n)
            return;
        const Entry e = S.entries[row];
        if (S.rename.active && S.rename_path == e.path)
            return;  // (a click in the name being typed)
        if (S.rename.active) {
            // the name typed for another entry, kept: the list sorted again, the row clicked is found again
            commit_rename();
            const auto it = std::find_if(S.entries.begin(), S.entries.end(), [&](const Entry &x) { return x.path == e.path; });
            if (it == S.entries.end())
                return;
            row = static_cast<int>(it - S.entries.begin());
        }
        if (twice && S.chosen.count(e.path) && !S.key_ctrl && !S.key_shift) {
            open_entry(e);
            return;
        }
        int click_row = -1;
        if (S.key_ctrl)
            choose_toggle(row);
        else if (S.key_shift)
            choose_range(row);
        else if (!S.chosen.count(e.path))
            choose_only(row);
        else  // (one of several chosen: they drag together; let go without a drag, it's chosen alone)
            S.cursor = S.anchor = row, click_row = row;
        if (!S.path.empty() && S.chosen.count(e.path)) {
            const std::vector<std::string> list = chosen_paths();
            S.dragged = DraggedFiles{true, false, px, py, list, list.size() == 1 ? e.name : std::to_string(list.size()) + " items",
                                     e.icon, click_row};
        }
    } else if (part == "list") {
        // empty space: the selection goes (not with Ctrl), and a selection rectangle starts
        if (!S.key_ctrl)
            S.chosen.clear();
        S.cursor = -1;
        const Rect list = files_list();
        S.banding = true;
        S.band_x = px;
        S.band_y = (py - list.y) / ROW_H + S.scroll;
    }
}

void click(const std::string &key, bool twice, float px, float py) {
    if (key.rfind("dialog:", 0) == 0) {
        dialog_click(number_after(key, "dialog:"));
        return;
    }
    if (key == "dialog")
        return;
    if (key.rfind("menu:", 0) == 0) {
        const int i = number_after(key, "menu:");
        if (i >= 0 && i < static_cast<int>(S.menu.items.size()) && S.menu.items[i].enabled) {
            const std::string action = S.menu.items[i].action;
            menu_action(action);
        }
        return;
    }
    if (key == "menu")
        return;
    if (key == "outside-menu") {
        S.menu.open = false;  // (a click away from a menu only closes it, as on Windows)
        return;
    }
    if (key == "close") {
        close_files();
        return;
    }
    files_click(key, twice, px, py);
}

// --- the D-pad: the pointer goes where it steps ------------------------------------------------------

void step_menu(int step) {
    const int n = static_cast<int>(S.menu.items.size());
    int at = S.hover.rfind("menu:", 0) == 0 ? number_after(S.hover, "menu:") : -1;
    for (int k = 0; k < n; k++) {
        at = at < 0 ? (step > 0 ? 0 : n - 1) : (at + step + n) % n;
        if (!S.menu.items[at].action.empty() && S.menu.items[at].enabled)
            break;
    }
    if (at >= 0) {
        const Rect r = menu_item_rect(at);
        warp(r.x + 40.f, r.y + r.h / 2.f);
    }
}

void step_dialog() {
    int at = S.hover.rfind("dialog:", 0) == 0 ? number_after(S.hover, "dialog:") : -1;
    at = (at + 1) % S.dialog.buttons();
    const Rect r = dialog_button(at);
    warp(r.x + r.w / 2.f, r.y + r.h / 2.f);
}

// The pointer onto entry i (moved there by the D-pad): Cross then acts on it
void point_at_row(int i) {
    const Rect list = files_list();
    const float y = list.y + (i - static_cast<int>(S.scroll)) * ROW_H + ROW_H / 2.f;
    float x = S.px;
    if (x < list.x + 40 || x > list.x + list.w - 20)
        x = list.x + 150.f;
    warp(x, y);
}

// --- a USB keyboard --------------------------------------------------------------------------------

void files_key(const KeyEvent &k) {
    // typing in one of the boxes
    if (S.rename.active) {
        if (k.vkey == VK_RETURN)
            commit_rename();
        else if (k.vkey == VK_ESCAPE)
            S.rename.active = false;
        else
            edit_key(S.rename, k);
        return;
    }
    if (S.address.active) {
        if (k.vkey == VK_RETURN) {
            S.address.active = false;
            go_to_address(S.address.text);
        } else if (k.vkey == VK_ESCAPE) {
            S.address.active = false;
        } else {
            edit_key(S.address, k);
        }
        return;
    }
    if (S.search.active) {
        if (k.vkey == VK_ESCAPE) {
            S.search = LineEdit{};
            S.filter.clear();
            refresh();
        } else if (k.vkey == VK_RETURN || k.vkey == VK_DOWN) {
            S.search.active = false;
            if (!S.entries.empty())
                choose_only(0);
        } else if (edit_key(S.search, k)) {
            S.filter = S.search.text;
            load_entries();
            S.scroll = 0;
            S.chosen.clear();
            S.cursor = S.anchor = -1;
        }
        return;
    }
    const int n = static_cast<int>(S.entries.size());
    const auto go = [&](int i) {
        if (n == 0)
            return;
        i = std::clamp(i, 0, n - 1);
        if (S.key_shift)
            choose_range(i);
        else if (S.key_ctrl)
            S.cursor = i;  // (Ctrl moves the focus only; Ctrl+Space chooses)
        else
            choose_only(i);
        keep_in_view(i);
    };
    if (S.key_alt) {
        if (k.vkey == VK_LEFT)
            go_back();
        else if (k.vkey == VK_RIGHT)
            go_forward();
        else if (k.vkey == VK_UP)
            go_up();
        else if (k.vkey == VK_RETURN)
            files_command("properties", S.cursor);
        else if (k.vkey == 'D')
            start_address();
        return;
    }
    if (S.key_ctrl) {
        switch (k.vkey) {
        case 'A': choose_all(); return;
        case 'C': files_command("copy"); return;
        case 'X': files_command("cut"); return;
        case 'V': files_command("paste"); return;
        case 'N':
            if (S.key_shift)
                files_command("new-folder");
            return;
        case 'W': close_files(); return;
        case 'F':
        case 'E': start_search(); return;
        case 'L': start_address(); return;
        case VK_SPACE: choose_toggle(S.cursor); return;
        }
    }
    switch (k.vkey) {
    case VK_UP: go(S.cursor < 0 ? 0 : S.cursor - 1); return;
    case VK_DOWN: go(S.cursor < 0 ? 0 : S.cursor + 1); return;
    case VK_PRIOR: go(S.cursor - files_rows()); return;
    case VK_NEXT: go(S.cursor + files_rows()); return;
    case VK_HOME: go(0); return;
    case VK_END: go(n - 1); return;
    case VK_RETURN:
        if (S.cursor >= 0 && S.cursor < n)
            open_entry(S.entries[S.cursor]);
        return;
    case VK_BACK: go_back(); return;
    case VK_DELETE: files_command(S.key_shift ? "delete-for-good" : "delete"); return;
    case VK_F2: files_command("rename", S.cursor); return;
    case VK_F3: start_search(); return;
    case VK_F5: refresh(); return;
    case VK_ESCAPE: S.chosen.clear(); return;
    case VK_APPS: {  // the menu key: the entry's menu, by it
        const Rect list = files_list();
        const int row = S.cursor;
        const float y = row >= 0 ? list.y + (row - static_cast<int>(S.scroll)) * ROW_H + ROW_H : list.y + 20.f;
        open_menu(row >= 0 ? "row" + std::to_string(row) : std::string("list"), static_cast<float>(list.x + 80), y);
        return;
    }
    }
    // letters: the next entry whose name starts with what was typed (as Explorer's)
    if (const char ch = key_char(k.vkey, false); ch && ch != ' ' && n > 0) {
        const double t = now_seconds();
        if (t - S.typed_at > 1.0)
            S.typed.clear();
        S.typed_at = t;
        S.typed += static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        const int from = S.cursor < 0 ? 0 : S.cursor + (S.typed.size() == 1 ? 1 : 0);
        for (int k2 = 0; k2 < n; k2++) {
            const int i = (from + k2) % n;
            if (lower(S.entries[i].name).rfind(S.typed, 0) == 0) {
                choose_only(i);
                keep_in_view(i);
                break;
            }
        }
    }
}

// A key for Files, its menu or its dialog
void key(const KeyEvent &k) {
    if (S.dialog.open) {
        if (k.vkey == VK_RETURN)
            dialog_click(0);
        else if (k.vkey == VK_ESCAPE)
            S.dialog.open = false;
        return;
    }
    if (S.menu.open) {
        if (k.vkey == VK_ESCAPE) {
            S.menu.open = false;
        } else if (k.vkey == VK_RETURN && S.hover.rfind("menu:", 0) == 0) {
            const std::string target = S.hover;
            click(target, false, S.px, S.py);
        } else if (k.vkey == VK_UP || k.vkey == VK_DOWN) {
            step_menu(k.vkey == VK_DOWN ? 1 : -1);
        }
        return;
    }
    if (S.key_alt && k.vkey == VK_F4) {
        close_files();
        return;
    }
    files_key(k);
}

// --- a copy, move or delete ----------------------------------------------------------------------------

// One that ended: the list again, and how it went. True while one runs.
bool job_tick() {
    const shell::JobStatus job = shell::job_status();
    if (job.finished) {
        shell::job_acknowledge();
        const bool was_archive = S.archive_job_running;
        const ArchiveJob archive_job = S.archive_job;
        S.archive_job_running = false;
        if (S.visible && (S.path.empty() || S.path == RECYCLE || std::find(job.touched.begin(), job.touched.end(), S.path) != job.touched.end()))
            refresh();
        if (job.password_needed && was_archive && !archive_job.file.empty()) {
            // a ZIP's files locked with a password: asked for, then taken out again (kept for this run)
            ask(job.wrong_password ? "Wrong password - try again: " + base_name(archive_job.file) : "Password for " + base_name(archive_job.file),
                "", [archive_job](const std::string &text) {
                    archive_passwords[archive_job.file] = text;
                    start_extract(archive_job);
                });
        } else if (job.cancelled) {
            toast(job.title + ": cancelled");
        } else if (!job.error.empty()) {
            report(job.title + " stopped", job.error);
        } else {
            if (!job.open_after.empty() && browser.open_file)
                browser.open_file(job.open_after);
            else
                toast(job.title + ": done");
            if (was_archive && S.visible && !archive_job.then_go.empty())
                navigate(archive_job.then_go);
            else if (was_archive && S.visible && !archive_job.then_select.empty() && folder_of(archive_job.then_select) == S.path)
                refresh(archive_job.then_select);
        }
        S.dirty = true;
    }
    return job.running;
}

// An archive's list read meanwhile: the folder shown again when it is in that archive; a list locked
// with a password asks for one (once; again when it was wrong)
void archive_list_came() {
    std::string file, inside;
    if (!S.visible || !in_archive(S.path, file, inside))
        return;
    refresh();
    const auto index = archive::index_of(file, password_for(file));
    if (!index || !index->needs_password || !index->items.empty())
        return;
    const std::string key = file + (index->wrong_password ? "\n" + password_for(file) : std::string());
    if (password_asked_for == key)
        return;
    password_asked_for = key;
    ask(index->wrong_password ? "Wrong password - try again: " + base_name(file) : "Password for " + base_name(file), "",
        [file](const std::string &text) {
            archive_passwords[file] = text;
            if (S.visible)
                refresh();
        });
}

// --- drawing -----------------------------------------------------------------------------------------

// a rounded rectangle with a 1 px border
void panel(Canvas &c, int x, int y, int w, int h, int radius, Color fill, Color border) {
    c.fill_round_rect(x, y, w, h, radius, border);
    c.fill_round_rect(x + 1, y + 1, w - 2, h - 2, std::max(radius - 1, 0), fill);
}

// a disabled button: its colours faded into the bar's
void fade(Canvas &c, const Rect &r, Color bg) { c.fill_rect(r.x, r.y, r.w, r.h, Color{bg.r, bg.g, bg.b, 0xa8}); }

// A panel's soft shadow (one sprite per size), lit from above
Image make_shadow(int w, int h, int alpha) {
    Image s;
    s.width = w + 2 * SHADOW;
    s.height = h + 2 * SHADOW;
    s.pixels.assign(static_cast<size_t>(s.width) * s.height, 0);
    for (int y = 0; y < s.height; y++)
        for (int x = 0; x < s.width; x++) {
            // distance outside the panel (moved down a little)
            const float px = x + 0.5f - SHADOW, py = y + 0.5f - SHADOW - 8.f;
            const float hw = w * 0.5f, hh = h * 0.5f;
            const float qx = std::fabs(px - hw) - (hw - RADIUS), qy = std::fabs(py - hh) - (hh - RADIUS);
            const float d = std::sqrt(std::max(qx, 0.f) * std::max(qx, 0.f) + std::max(qy, 0.f) * std::max(qy, 0.f)) - RADIUS;
            const float a = d <= 0 ? 1.f : std::exp(-(d * d) / 220.f);
            s.pixels[static_cast<size_t>(y) * s.width + x] = static_cast<uint32_t>(a * alpha) << 24;
        }
    return s;
}

const Image &shadow_for(int w, int h) {
    static std::map<std::pair<int, int>, Image> cache;
    const auto key = std::make_pair(w, h);
    auto it = cache.find(key);
    if (it == cache.end()) {
        if (cache.size() > 24)
            cache.clear();
        it = cache.emplace(key, make_shadow(w, h, 150)).first;
    }
    return it->second;
}

void draw_files(Canvas &c) {
    const bool place = S.path.empty(), ro = read_only(S.path);
    const std::string drop = S.dragged.active ? drop_folder(S.hover) : std::string();
    const bool drop_ok = S.dragged.active && drop_effect(drop) != 0;
    const int n = static_cast<int>(S.entries.size());
    const bool any = !S.chosen.empty();

    // the frame, and the title bar: the folder's name, Close
    c.fill_rect(0, 0, S.w, S.h, WINDOW_BG);
    c.fill_rect(0, 0, S.w, 1, BORDER_ACTIVE);
    c.fill_rect(0, S.h - 1, S.w, 1, BORDER_ACTIVE);
    c.fill_rect(0, 0, 1, S.h, BORDER_ACTIVE);
    c.fill_rect(S.w - 1, 0, 1, S.h, BORDER_ACTIVE);
    draw_icon(c, Icon::Folder, 26, TITLE_H / 2.f, 20);
    c.text(46, 26, place_title(S.path), 15, TEXT, Weight::Regular, S.w - 46 - 46 - 10);
    const Rect cb = close_button();
    if (S.hover == "close")
        c.fill_round_rect(cb.x, cb.y + 1, cb.w - 1, cb.h - 1, 6, CLOSE_RED);
    const float ccx = cb.x + cb.w / 2.f, ccy = cb.y + cb.h / 2.f;
    c.line(ccx - 6, ccy - 6, ccx + 6, ccy + 6, 1.4f, TEXT);
    c.line(ccx - 6, ccy + 6, ccx + 6, ccy - 6, 1.4f, TEXT);

    // the command bar
    for (int i = 0; i < CMD_COUNT; i++) {
        const CmdButton &b = FILES_CMDS[i];
        const Rect r = files_cmd_rect(i);
        if (!*b.key) {
            c.fill_rect(r.x + r.w / 2 + 2, r.y + 6, 1, r.h - 12, BORDER);
            continue;
        }
        const bool enabled = command_enabled(b.key);
        if (enabled && S.hover == std::string("cmd:") + b.key)
            c.fill_round_rect(r.x, r.y, r.w, r.h, 5, HOVER);
        draw_icon(c, b.icon, r.x + (b.label ? 20.f : r.w / 2.f), r.y + r.h / 2.f, 18);
        if (b.label)
            c.text(r.x + 38, r.y + r.h / 2 + 5, b.label, 14, TEXT);
        if (!enabled)
            fade(c, r, WINDOW_BG);
    }

    // the address row: Back, Forward, Up, Refresh, the address, the search box
    const int ay = TITLE_H + FILES_CMD_H;
    c.fill_rect(1, ay + FILES_ADDR_H - 1, S.w - 2, 1, BORDER);
    const Icon nav_icons[] = {Icon::Back, Icon::Forward, Icon::Up, Icon::Refresh};
    const bool nav_on[] = {!S.back.empty(), !S.forward.empty(), !place, true};
    static const char *const nav_keys[] = {"back", "forward", "up", "refresh"};
    for (int i = 0; i < 4; i++) {
        const Rect r = files_nav_button(i);
        if (nav_on[i] && S.hover == nav_keys[i])
            c.fill_round_rect(r.x, r.y, r.w, r.h, 5, HOVER);
        draw_icon(c, nav_icons[i], r.x + r.w / 2.f, r.y + r.h / 2.f, 18);
        if (!nav_on[i])
            fade(c, r, WINDOW_BG);
    }
    const Rect a = files_address();
    panel(c, a.x, a.y, a.w, a.h, 5, CONTROL, S.address.active ? ACCENT : BORDER);
    S.crumb_rects.clear();
    if (S.address.active) {
        draw_edit(c, a, S.address, 15);
    } else {
        // the folder's icon, then its path's parts (the first ones left out when they don't fit)
        draw_icon(c, place ? Icon::Computer : Icon::Folder, a.x + 20, a.y + a.h / 2.f, 18);
        const std::vector<Crumb> list = crumbs(S.path);
        std::vector<int> widths;
        int total = 0;
        for (const Crumb &cr : list) {
            widths.push_back(c.text_width(cr.label, 14) + 16);
            total += widths.back() + 18;
        }
        int first = 0;
        while (first < static_cast<int>(list.size()) - 1 && total > a.w - 50) {
            total -= widths[first] + 18;
            first++;
        }
        S.crumb_rects.assign(list.size(), Rect{});
        int x = a.x + 36;
        for (int i = first; i < static_cast<int>(list.size()); i++) {
            const Rect r{x, a.y + 4, std::min(widths[i], a.x + a.w - 4 - x), a.h - 8};
            S.crumb_rects[i] = r;
            if (S.hover == "crumb" + std::to_string(i))
                c.fill_round_rect(r.x, r.y, r.w, r.h, 4, drop_ok ? SELECTED : HOVER);
            c.text(r.x + 8, a.y + a.h / 2 + 5, list[i].label, 14, TEXT, Weight::Regular, std::max(1, r.w - 8));
            x += r.w;
            if (i + 1 < static_cast<int>(list.size())) {
                draw_icon(c, Icon::Chevron, x + 9.f, a.y + a.h / 2.f, 12);
                x += 18;
            }
        }
    }
    const Rect sr = files_search();
    panel(c, sr.x, sr.y, sr.w, sr.h, 5, CONTROL, S.search.active ? ACCENT : BORDER);
    draw_icon(c, Icon::Search, sr.x + 18.f, sr.y + sr.h / 2.f, 14);
    if (S.search.active)
        draw_edit(c, Rect{sr.x + 26, sr.y, sr.w - 26, sr.h}, S.search, 14);
    else
        c.text(sr.x + 36, sr.y + sr.h / 2 + 5, S.filter.empty() ? "Search " + place_title(S.path) : S.filter, 14,
               S.filter.empty() ? TEXT3 : TEXT, Weight::Regular, sr.w - 46);

    // the places' pane: the place holding the folder marked (the longest path that matches)
    const int top = files_top(), bottom = files_bottom();
    c.fill_rect(FILES_NAV_W - 1, top, 1, bottom - top, BORDER);
    const std::vector<Entry> nav = nav_items();
    const int holder = place_of(S.path);
    const int current = place ? 0 : holder >= 0 ? holder + 1 : -1;
    for (int i = 0; i < static_cast<int>(nav.size()); i++) {
        const Rect r = files_nav_item(i);
        if (r.y + r.h > bottom - 4)
            break;
        const std::string key = "nav" + std::to_string(i);
        if (i == current || (S.hover == key && drop_ok))
            c.fill_round_rect(r.x, r.y, r.w, r.h, 5, SELECTED);
        else if (S.hover == key)
            c.fill_round_rect(r.x, r.y, r.w, r.h, 5, HOVER);
        if (i == current)
            c.fill_round_rect(r.x, r.y + 8, 3, r.h - 16, 2, ACCENT);
        draw_icon(c, nav[i].icon, r.x + 24.f, r.y + r.h / 2.f, 18);
        c.text(r.x + 44, r.y + r.h / 2 + 5, nav[i].name, 14, TEXT, Weight::Regular, r.w - 52);
        if (i == QUICK)  // (between Wisp's own folders and the drives)
            c.fill_rect(r.x + 10, r.y + r.h + NAV_GAP / 2, r.w - 20, 1, BORDER);
    }

    // the list's header: its columns, the one it is sorted by with an arrow
    const Rect hd = files_header();
    c.fill_rect(hd.x, hd.y, hd.w, hd.h, CONTENT_BG);
    const char *const titles[] = {"Name", place ? "Type" : S.path == RECYCLE ? "Date deleted" : "Date modified", place ? "Free space" : "Type",
                                  place ? "" : "Size"};
    for (int i = 0; i < 4; i++) {
        const int x = files_column_x(i), next = i < 3 ? files_column_x(i + 1) : hd.x + hd.w;
        if (!place && S.hover == "col" + std::to_string(i))
            c.fill_rect(x - 10, hd.y + 2, next - x, hd.h - 4, HOVER);
        if (i > 0)
            c.fill_rect(x - 10, hd.y + 8, 1, hd.h - 16, BORDER);
        c.text(x, hd.y + 21, titles[i], 13, TEXT2);
        if (!place && S.sort_by == i) {  // a small arrow, up for A to Z
            const float tx = x + c.text_width(titles[i], 13) + 12.f, ty = hd.y + hd.h / 2.f, d = S.sort_down ? 3.f : -3.f;
            c.line(tx - 4, ty - d, tx, ty + d, 1.4f, TEXT3);
            c.line(tx, ty + d, tx + 4, ty - d, 1.4f, TEXT3);
        }
    }
    c.fill_rect(hd.x + 8, hd.y + hd.h - 1, hd.w - 16, 1, BORDER);

    // the rows
    const Rect list = files_list();
    c.fill_rect(list.x, list.y, list.w, list.h, CONTENT_BG);
    const int shown = files_rows();
    clamp_scroll();
    const int first = static_cast<int>(S.scroll);
    if (!n) {
        const std::string why = !S.filter.empty() ? "No items match your search."
                              : place             ? ""
                              : S.path == RECYCLE  ? "The Recycle Bin is empty."
                              : in_archive(S.path) ? (archive_note.empty() ? "This folder is empty." : archive_note)
                              : access(S.path.c_str(), R_OK | X_OK) != 0 ? "This folder can't be opened: the PS4 doesn't let the app read it."
                                                                         : "This folder is empty.";
        c.text_centered(list.x + list.w / 2, list.y + 60, why.c_str(), 15, TEXT3);
    }
    const bool keyboard = shell::hwkeyboard_connected();
    const int col1 = files_column_x(1), col2 = files_column_x(2), col3 = files_column_x(3), nx = list.x + 78;
    for (int i = first; i < n && i < first + shown; i++) {
        const Entry &e = S.entries[i];
        const int ry = list.y + (i - first) * ROW_H;
        const std::string row_key = "row" + std::to_string(i), check_key = "check" + std::to_string(i);
        const bool chosen = S.chosen.count(e.path) > 0, hot = S.hover == row_key || S.hover == check_key;
        const bool target = drop_ok && e.dir && drop == e.path && hot;
        if (chosen || target)
            c.fill_round_rect(list.x + 6, ry + 1, list.w - 14, ROW_H - 2, 5, SELECTED);
        else if (hot)
            c.fill_round_rect(list.x + 6, ry + 1, list.w - 14, ROW_H - 2, 5, HOVER);
        if (i == S.cursor && !chosen && keyboard) {  // the keyboard's place, outlined
            c.fill_rect(list.x + 8, ry + 2, list.w - 18, 1, TEXT3);
            c.fill_rect(list.x + 8, ry + ROW_H - 3, list.w - 18, 1, TEXT3);
        }
        // the check box: on the row under the pointer, and on all once one is chosen
        if (chosen || hot || any) {
            const int bx = list.x + CHECK_X, by = ry + ROW_H / 2 - 9;
            if (chosen) {
                c.fill_round_rect(bx, by, 18, 18, 4, ACCENT);
                draw_icon(c, Icon::Check, bx + 9.f, by + 9.f, 14);
            } else {
                c.fill_round_rect(bx, by, 18, 18, 4, TEXT3);
                c.fill_round_rect(bx + 1, by + 1, 16, 16, 3, CONTENT_BG);
            }
        }
        draw_icon(c, e.icon, list.x + 58.f, ry + ROW_H / 2.f, 22);
        // (a cut entry is shown faded until it is pasted)
        const bool cut = S.clipboard.cut && std::find(S.clipboard.paths.begin(), S.clipboard.paths.end(), e.path) != S.clipboard.paths.end();
        if (cut)
            c.fill_rect(list.x + 44, ry + 5, 28, ROW_H - 10, Color{CONTENT_BG.r, CONTENT_BG.g, CONTENT_BG.b, 0x90});
        if (S.rename.active && S.rename_path == e.path) {
            const Rect box{nx - 6, ry + 3, col1 - nx - 4, ROW_H - 6};
            panel(c, box.x, box.y, box.w, box.h, 4, CONTROL, ACCENT);
            draw_edit(c, box, S.rename, 15);
        } else {
            c.text(nx, ry + 22, e.name, 15, cut ? TEXT3 : TEXT, Weight::Regular, col1 - nx - 14);
        }
        if (place) {
            c.text(col1, ry + 22, type_text(e), 14, TEXT2, Weight::Regular, col2 - col1 - 14);
            c.text(col2, ry + 22, e.note, 14, TEXT2, Weight::Regular, list.x + list.w - col2 - 16);
            continue;
        }
        c.text(col1, ry + 22, date_text(e.modified), 14, TEXT2, Weight::Regular, col2 - col1 - 14);
        c.text(col2, ry + 22, type_text(e), 14, TEXT2, Weight::Regular, col3 - col2 - 14);
        if (!e.dir) {  // (sizes to the right)
            const std::string size = size_text(e.size);
            c.text(list.x + list.w - 22 - c.text_width(size, 14), ry + 22, size, 14, TEXT2);
        }
    }
    // a selection rectangle being drawn
    if (S.banding) {
        const float y0 = list.y + (S.band_y - S.scroll) * ROW_H;
        const float lx0 = static_cast<float>(list.x), lx1 = static_cast<float>(list.x + list.w);
        const float ly0 = static_cast<float>(list.y), ly1 = static_cast<float>(list.y + list.h);
        const int x0 = static_cast<int>(std::clamp(std::min(S.band_x, S.px), lx0, lx1));
        const int x1 = static_cast<int>(std::clamp(std::max(S.band_x, S.px), lx0, lx1));
        const int y_0 = static_cast<int>(std::clamp(std::min(y0, S.py), ly0, ly1));
        const int y_1 = static_cast<int>(std::clamp(std::max(y0, S.py), ly0, ly1));
        if (x1 - x0 > 3 && y_1 - y_0 > 3) {
            c.fill_rect(x0, y_0, x1 - x0, y_1 - y_0, Color{ACCENT.r, ACCENT.g, ACCENT.b, 0x30});
            c.fill_rect(x0, y_0, x1 - x0, 1, ACCENT);
            c.fill_rect(x0, y_1 - 1, x1 - x0, 1, ACCENT);
            c.fill_rect(x0, y_0, 1, y_1 - y_0, ACCENT);
            c.fill_rect(x1 - 1, y_0, 1, y_1 - y_0, ACCENT);
        }
    }
    // a drop into the folder itself: the list outlined
    if (drop_ok && drop == S.path && !S.hover.empty()) {
        c.fill_rect(list.x + 2, list.y, list.w - 4, 2, ACCENT);
        c.fill_rect(list.x + 2, list.y + list.h - 2, list.w - 4, 2, ACCENT);
        c.fill_rect(list.x + 2, list.y, 2, list.h, ACCENT);
        c.fill_rect(list.x + list.w - 4, list.y, 2, list.h, ACCENT);
    }
    // the scroll bar
    if (n > shown) {
        const float total = static_cast<float>(n);
        const int bh = std::max(30, static_cast<int>(list.h * shown / total));
        const int by = list.y + static_cast<int>((list.h - bh) * (S.scroll / (total - shown)));
        c.fill_round_rect(list.x + list.w - 8, by, 4, bh, 2, SCROLLBAR);
    }

    // the status bar: how many, what's chosen, the drive's room
    const int sy = files_bottom();
    c.fill_rect(1, sy, S.w - 2, 1, BORDER);
    std::string status = std::to_string(n) + (n == 1 ? " item" : " items");
    if (any) {
        long long bytes = 0;
        bool files_only = true;
        for (const Entry &e : S.entries)
            if (S.chosen.count(e.path)) {
                if (e.dir)
                    files_only = false;
                else
                    bytes += std::max(0ll, e.size);
            }
        status += "      " + std::to_string(S.chosen.size()) + (S.chosen.size() == 1 ? " item selected" : " items selected") +
                  (files_only && bytes ? "  " + size_text(bytes) : std::string());
    }
    c.text(16, sy + 19, status, 13, TEXT2);
    if (!place) {
        std::string archive_file, inside;
        const std::string room = in_archive(S.path, archive_file, inside) ? "Inside " + base_name(archive_file) + ": copy out or Extract All"
                               : ro                                       ? "Read-only: the app's own files"
                                                                          : space_cached(S.path);
        c.text(S.w - 16 - c.text_width(room, 13), sy + 19, room, 13, TEXT2);
    }
}

// The entries being dragged, by the pointer: what letting go does
void draw_dragged(Canvas &c) {
    const std::string folder = drop_folder(S.hover);
    const int effect = drop_effect(folder);
    const int x = static_cast<int>(S.px) + 18, y = static_cast<int>(S.py) + 14;
    const std::string where = place_title(folder);
    const std::string what = effect == 1 ? "Move to " + where : effect == 2 ? "+ Copy to " + where : S.dragged.label;
    const int w = std::min(560, c.text_width(what, 14) + 64);
    panel(c, x, y, w, 40, 6, POPUP_BG, POPUP_BORDER);
    draw_icon(c, S.dragged.icon, x + 22.f, y + 20.f, 22);
    if (S.dragged.paths.size() > 1) {  // how many, as a badge
        const std::string count = std::to_string(S.dragged.paths.size());
        const int bw = std::max(20, c.text_width(count, 12, Weight::Bold) + 10);
        c.fill_round_rect(x + 26, y + 2, bw, 18, 9, ACCENT);
        c.text_centered(x + 26 + bw / 2, y + 16, count, 12, ACCENT_TEXT, Weight::Bold);
    }
    c.text(x + 44, y + 26, what, 14, TEXT, Weight::Regular, w - 52);
}

// A copy, move, delete or an archive's job running: a card in the corner with its progress and Cancel
void draw_job(Canvas &c) {
    const shell::JobStatus s = shell::job_status();
    if (!s.running)
        return;
    const Rect card = job_card();
    const int w = card.w, h = card.h, x = card.x, y = card.y;
    c.blit(shadow_for(w, h), x - SHADOW, y - SHADOW);
    panel(c, x, y, w, h, RADIUS, CARD, POPUP_BORDER);
    const Rect b = job_cancel_rect();
    c.text(x + 22, y + 32, s.title, 16, TEXT, Weight::Bold, b.x - x - 32);
    panel(c, b.x, b.y, b.w, b.h, 4, !s.cancelling && S.hover == "job-cancel" ? BUTTON_HOT : BUTTON, BUTTON_BORDER);
    c.text_centered(b.x + b.w / 2, b.y + b.h / 2 + 5, s.cancelling ? "Stopping" : "Cancel", 13, s.cancelling ? TEXT3 : TEXT);
    c.text(x + 22, y + 56, s.current, 14, TEXT2, Weight::Regular, w - 44);
    const float f = s.total > 0 ? std::clamp(static_cast<float>(s.done) / static_cast<float>(s.total), 0.f, 1.f) : 0.f;
    c.fill_round_rect(x + 22, y + 74, w - 44, 6, 3, TRACK);
    c.fill_round_rect(x + 22, y + 74, std::max(6, static_cast<int>((w - 44) * f)), 6, 3, ACCENT);
    const std::string amount = s.counts_items ? std::to_string(s.done) + " of " + std::to_string(s.total)
                                              : size_text(s.done) + " of " + size_text(s.total);
    c.text(x + 22, y + 98, amount, 13, TEXT3);
}

void draw_menu(Canvas &c) {
    const Rect m = menu_rect();
    c.blit(shadow_for(m.w, m.h), m.x - SHADOW, m.y - SHADOW);
    panel(c, m.x, m.y, m.w, m.h, RADIUS, POPUP_BG, POPUP_BORDER);
    for (int i = 0; i < static_cast<int>(S.menu.items.size()); i++) {
        const MenuItem &it = S.menu.items[i];
        const Rect r = menu_item_rect(i);
        if (it.action.empty()) {
            c.fill_rect(r.x + 8, r.y + r.h / 2, r.w - 16, 1, LINE);
            continue;
        }
        if (it.enabled && S.hover == "menu:" + std::to_string(i))
            c.fill_round_rect(r.x, r.y + 1, r.w, r.h - 2, 5, HOVER);
        c.text(r.x + 16, r.y + 25, it.label, 15, it.enabled ? TEXT : DISABLED);
    }
}

void draw_dialog(Canvas &c) {
    c.fill_rect(0, 0, S.w, S.h, rgb(0x000000, 0x60));
    const Rect d = dialog_rect();
    c.blit(shadow_for(d.w, d.h), d.x - SHADOW, d.y - SHADOW);
    panel(c, d.x, d.y, d.w, d.h, RADIUS, DIALOG_BG, POPUP_BORDER);
    c.fill_round_rect(d.x + 1, d.y + d.h - 88, d.w - 2, 87, RADIUS - 1, DIALOG_FOOT);
    c.fill_rect(d.x + 1, d.y + d.h - 88, d.w - 2, 10, DIALOG_FOOT);
    c.text(d.x + 30, d.y + 48, S.dialog.title, 20, TEXT, Weight::Bold, d.w - 60);
    if (dialog_lines() == 1) {
        c.text_wrapped(d.x + 30, d.y + 84, S.dialog.text, 15, TEXT2, d.w - 60, 22);
    } else {
        size_t start = 0;
        for (int line = 0; start <= S.dialog.text.size(); line++) {
            const size_t end = S.dialog.text.find('\n', start);
            const std::string piece = S.dialog.text.substr(start, end == std::string::npos ? std::string::npos : end - start);
            c.text(d.x + 30, d.y + 84 + line * 26, piece, 15, TEXT2, Weight::Regular, d.w - 60);
            if (end == std::string::npos)
                break;
            start = end + 1;
        }
    }
    for (int i = 0; i < S.dialog.buttons(); i++) {
        const Rect b = dialog_button(i);
        const bool primary = i == 0;
        const bool hot = S.hover == "dialog:" + std::to_string(i);
        const bool danger = primary && S.dialog.kind == DialogKind::Delete;
        const Color fill = danger ? (hot ? rgb(0xe0453a) : CLOSE_RED) : primary ? (hot ? ACCENT_HOT : ACCENT) : (hot ? BUTTON_HOT : BUTTON);
        panel(c, b.x, b.y, b.w, b.h, 5, fill, danger ? rgb(0xa32317) : primary ? ACCENT_BORDER : BUTTON_BORDER);
        const char *label = i == S.dialog.buttons() - 1 && i > 0 ? "Cancel" : S.dialog.kind == DialogKind::Delete ? "Delete" : "OK";
        c.text_centered(b.x + b.w / 2, b.y + 28, label, 15, danger ? rgb(0xffffff) : primary ? ACCENT_TEXT : TEXT);
    }
}

// --- opening -----------------------------------------------------------------------------------------

void open_at(const std::string &folder) {
    const bool was_open = S.visible;
    if (!was_open) {
        // a fresh Files: it keeps what Cut or Copy holds, the order and the area
        State fresh;
        fresh.clipboard = std::move(S.clipboard);
        fresh.text_clipboard = std::move(S.text_clipboard);
        fresh.sort_by = S.sort_by;
        fresh.sort_down = S.sort_down;
        fresh.x = S.x, fresh.y = S.y, fresh.w = S.w, fresh.h = S.h;
        fresh.archive_job = std::move(S.archive_job);  // (one may be running: its end is handled as asked)
        fresh.archive_job_running = S.archive_job_running;
        S = std::move(fresh);
        S.visible = true;
    }
    update_places(true);  // (a USB drive plugged in since)
    std::string path = folder;
    if (path.rfind("file://", 0) == 0)
        path = from_file_url(path);
    // an archive's path ending in '/': opened as a folder (the browser opening a downloaded archive);
    // without it, its folder with it chosen (Show in folder)
    if (path.size() > 1 && path.back() == '/') {
        std::string bare = path;
        while (bare.size() > 1 && bare.back() == '/')
            bare.pop_back();
        if (in_archive(bare)) {
            navigate(bare, was_open);
            job_tick();
            S.dirty = true;
            return;
        }
    }
    while (path.size() > 1 && path.back() == '/')
        path.pop_back();
    if (path.empty())
        path = wisp_download_folder();
    if (is_dir(path)) {
        navigate(path, was_open);
    } else if (is_file(path)) {
        navigate(folder_of(path), was_open);
        refresh(path);
    } else {
        navigate(wisp_download_folder(), was_open);
        if (!folder.empty())
            toast(folder + " isn't there: Files shows Downloads");
    }
    job_tick();  // (a copy that ended while Files was closed)
    S.dirty = true;
}

} // namespace

void set_host(Host host) { browser = std::move(host); }

void open(const std::string &folder) { open_at(folder); }

void close() { close_files(); }

// Files in a tab of its own: another tab shown hides it, its tab shown again brings it back where it was
// (the folder, Back and Forward, the selection); the folder is read again (a download may have ended)
void hide() { close_files(); }

void show() {
    if (S.visible)
        return;
    S.visible = true;
    update_places(true);
    refresh();
    S.dirty = true;
}

bool visible() { return S.visible; }

bool input(const shell::Input &in, const std::vector<shell::KeyEvent> &keys, int x, int y, int w, int h, float dt) {
    if (!S.visible)
        return false;
    if (S.x != x || S.y != y || S.w != w || S.h != h) {
        S.x = x, S.y = y, S.w = w, S.h = h;
        clamp_scroll();
        S.dirty = true;
    }
    const double t = now_seconds();
    if (update_places()) {  // (a USB drive plugged in or out)
        if (S.path.empty())
            refresh();
        S.dirty = true;
    }
    job_tick();
    poll_count();
    // after the PS4's keyboard: nothing is pressed until the buttons that closed it are let go (and
    // the browser doesn't get those presses either)
    if (S.held_back && !in.held)
        S.held_back = false;
    const bool live = !S.held_back;
    bool acted = !live && in.pressed != 0;
    const float px = in.x - x, py = in.y - y;
    const bool inside = px >= 0 && py >= 0 && px < w && py < h;
    S.px = px, S.py = py;
    if (in.moved && (S.dragged.active || S.banding))
        S.dirty = true;

    // a USB keyboard: all of its keys are Files' while it's up
    for (const KeyEvent &k : keys) {
        if (k.vkey == VK_LCONTROL || k.vkey == VK_RCONTROL || k.vkey == VK_CONTROL)
            S.key_ctrl = k.down;
        if (k.vkey == VK_LSHIFT || k.vkey == VK_RSHIFT || k.vkey == VK_SHIFT)
            S.key_shift = k.down;
        if (k.vkey == VK_LMENU || k.vkey == VK_RMENU || k.vkey == VK_MENU)
            S.key_alt = k.down;
        if (k.vkey == VK_CAPITAL && k.down)
            S.caps_lock = !S.caps_lock;
        if (!k.down)
            continue;
        acted = true;
        if (!live)
            continue;
        key(k);
        S.dirty = true;
        if (!S.visible)
            return true;  // (Ctrl+W, Alt+F4, or the browser closed it)
    }

    const std::string now_hover = inside || S.menu.open || S.dialog.open ? hit(px, py) : std::string();
    if (now_hover != S.hover) {
        S.hover = now_hover;
        S.dirty = true;
    }
    const bool modal = S.menu.open || S.dialog.open;  // (they take the buttons wherever the pointer is)
    if (live && (in.pressed & shell::BTN_CLICK) && (inside || modal)) {
        const bool twice = S.hover == S.last_click_key && t - S.last_click_time < 0.5;
        S.last_click_key = twice ? std::string() : S.hover;
        S.last_click_time = t;
        const std::string target = S.hover;
        click(target, twice, px, py);
        S.dirty = true;
        acted = true;
        if (!S.visible)
            return true;
    }
    if (live && (in.pressed & shell::BTN_MENU) && (inside || modal)) {
        // Square, the mouse's right button: the menu for what's under the pointer (not while entries
        // are dragged or a selection rectangle is drawn)
        if (S.menu.open)
            S.menu.open = false;
        else if (!S.dialog.open && !S.dragged.active && !S.banding)
            open_menu(S.hover, px, py);
        S.dirty = true;
        acted = true;
    }

    // entries dragged: once the pointer has gone far enough, then dropped where it's let go
    if (S.dragged.pending && !S.dragged.active && (in.held & shell::BTN_CLICK) && std::hypot(px - S.dragged.x0, py - S.dragged.y0) > 14) {
        S.dragged.active = true;
        S.dirty = true;
    }
    if (S.dragged.active) {
        acted = true;
        // (held at the list's top or bottom: it scrolls)
        const Rect list = files_list();
        if (px >= list.x && px < list.x + list.w && py < list.y + 24 && py >= list.y - 8)
            S.scroll = std::max(0.f, S.scroll - dt * 12), S.dirty = true;
        else if (px >= list.x && px < list.x + list.w && py > list.y + list.h - 24 && py < list.y + list.h + 8)
            S.scroll += dt * 12, S.dirty = true;
        clamp_scroll();
    }
    // a selection rectangle: the rows it covers, chosen (the list scrolls past its ends)
    if (S.banding) {
        acted = true;
        if (!(in.held & shell::BTN_CLICK)) {
            S.banding = false;
            S.dirty = true;
        } else {
            const Rect list = files_list();
            const float before = S.scroll;
            if (py < list.y)
                S.scroll -= dt * 12;
            else if (py > list.y + list.h)
                S.scroll += dt * 12;
            clamp_scroll();
            if (S.scroll != before)
                S.dirty = true;
            const float row = (std::clamp(py, static_cast<float>(list.y), static_cast<float>(list.y + list.h - 1)) - list.y) / ROW_H + S.scroll;
            if (std::fabs(row - S.band_y) * ROW_H > 4 || std::fabs(px - S.band_x) > 4) {
                const int from = static_cast<int>(std::floor(std::min(S.band_y, row))), to = static_cast<int>(std::floor(std::max(S.band_y, row)));
                std::set<std::string> covered;
                for (int i = std::max(0, from); i <= to && i < static_cast<int>(S.entries.size()); i++)
                    covered.insert(S.entries[i].path);
                if (covered != S.chosen) {
                    S.chosen.swap(covered);
                    S.dirty = true;
                }
            }
        }
    }
    if (in.released & shell::BTN_CLICK) {
        if (S.dragged.active && live)
            drop_dragged(hit(px, py));
        else if (S.dragged.pending && S.dragged.click_row >= 0)
            choose_only(S.dragged.click_row);
        if (S.dragged.pending || S.dragged.active) {
            S.dirty = true;
            acted = true;
        }
        S.dragged = DraggedFiles{};
    }

    // Circle: the dialog, the menu, typing in place, a search, then up a folder; at the top, Files closes
    if (live && (in.pressed & shell::BTN_BACK)) {
        acted = true;
        S.dirty = true;
        if (S.dialog.open) {
            S.dialog.open = false;
        } else if (S.menu.open) {
            S.menu.open = false;
        } else if (S.dragged.active || S.banding) {
            S.dragged = DraggedFiles{};
            S.banding = false;
        } else if (S.rename.active || S.address.active) {
            S.rename.active = S.address.active = false;
        } else if (S.search.active || !S.filter.empty()) {
            S.search = LineEdit{};
            S.filter.clear();
            refresh();
        } else if (!S.path.empty()) {
            go_up();
        } else {
            close_files();
            return true;
        }
    }
    // Triangle: the address bar
    if (live && (in.pressed & shell::BTN_SWITCH)) {
        acted = true;
        if (!modal)
            start_address();
        S.dirty = true;
    }
    // the D-pad: steps through the menu or the dialog (the pointer goes there), else up and down move
    // the selection (the pointer follows it, for Cross), left and right are Back and Forward
    const uint32_t dpad = in.repeat & (shell::BTN_UP | shell::BTN_DOWN | shell::BTN_LEFT | shell::BTN_RIGHT);
    if (live && dpad) {
        acted = true;
        S.dirty = true;
        const bool typing = S.rename.active || S.address.active || S.search.active;
        if (S.dialog.open) {
            step_dialog();
        } else if (S.menu.open) {
            if (dpad & (shell::BTN_UP | shell::BTN_DOWN))
                step_menu((dpad & shell::BTN_DOWN) ? 1 : -1);
        } else {
            if (dpad & shell::BTN_UP)
                files_key(KeyEvent{VK_UP, 0, true, true});
            if (dpad & shell::BTN_DOWN)
                files_key(KeyEvent{VK_DOWN, 0, true, true});
            if (!typing && (dpad & (shell::BTN_UP | shell::BTN_DOWN)) && S.cursor >= 0)
                point_at_row(S.cursor);
            if (!typing && (dpad & shell::BTN_LEFT))
                go_back();
            else if (!typing && (dpad & shell::BTN_RIGHT))
                go_forward();
        }
    }
    // the right stick, the wheel
    if (in.scroll != 0 && !modal) {
        S.scroll += in.scroll;
        clamp_scroll();
        S.dirty = true;
        acted = true;
    }
    return acted || (inside && (in.moved || ((in.pressed | in.released) & (shell::BTN_CLICK | shell::BTN_MENU)) != 0));
}

bool dirty() {
    const bool running = job_tick();
    if (archive::list_ready())
        archive_list_came();
    if (!S.visible)
        return false;
    poll_count();
    if (running && now_seconds() - S.job_drawn > 0.25)
        S.dirty = true;  // (its progress, 4 times a second)
    return S.dirty;
}

void draw(ps4ui::Canvas &canvas) {
    S.w = canvas.width();
    S.h = canvas.height();
    draw_files(canvas);
    if (S.dragged.active)
        draw_dragged(canvas);
    draw_job(canvas);
    if (S.menu.open)
        draw_menu(canvas);
    if (S.dialog.open)
        draw_dialog(canvas);
    S.dirty = false;
    S.job_drawn = now_seconds();
}

} // namespace wisp::files
