// Wisp: the browser (ui/browser.h).
//
// Layout (1920x1080): a toolbar of TOOLBAR_H pixels (the tab strip, then back / forward / reload /
// home, the address field and the menu button) over the page. The page is a WPE web view per tab; its
// frames arrive as shared-memory BGRA buffers (ui/platform.cpp) and are copied under the toolbar.
// Each frame is composed in ordinary memory and written to the screen's write-combined buffer once
// (reading that memory back is very slow).
//
// Controls: left stick / touch pad / USB mouse move the pointer; Cross (or R2) clicks; Square opens a
// link in a new tab; Circle goes back; Triangle edits the address; L1 / R1 switch tabs; OPTIONS opens
// the menu; the right stick and the D-pad's up/down scroll; D-pad left/right go back/forward.
// A USB mouse works as on a computer: left click, right click (the page's menu: open in a new tab,
// copy the link, save the image, ...), middle click (a link in a new tab), the side buttons (back,
// forward), the wheel (Shift: sideways; Ctrl: zoom) and its tilt. A USB keyboard types into the
// page's fields itself (no system keyboard over the page) and into the address field in place, with
// a desktop browser's shortcuts: Ctrl+L / F6 / Alt+D address, Ctrl+T new tab, Ctrl+W close,
// Ctrl+Tab / Ctrl+PageUp/PageDown / Ctrl+1-9 tabs, Alt+Left/Right back/forward, Alt+Home home,
// F5 / Ctrl+R reload (Ctrl+Shift+R without the cache), Esc stop, F11 full screen, Ctrl +/-/0 zoom.
// A page that reads gamepads (v00.30) gets the controller as a PC browser's standard gamepad: then the
// controller drives none of the above until the touch pad's button is held a second (gamepad_frame).
#include "browser.h"

#include "../common/wisp.h"
#include "cursor.h"
#include "dm/dm.h"
#include "player/player.h"
#include "ext/extensions.h"
#include "files/archives.h"
#include "files/files.h"
#include "pages.h"
#include "platform.h"
#include "process_manager.h"
#include "shell/hwkeyboard.h"
#include "shell/input.h"
#include "shell/keyboard.h"
#include "shell/ps4_canvas.h"
#include "shell/screen.h"

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <ps4_kit.h>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>
#include <wpe/webkit.h>

extern "C" void ps4_release_gpu_space(void);   // ui/gpu_space.c
// the system runs another program of the app in this one's place ("/app0/eboot.bin": a restart)
extern "C" int wisp_load_exec(const char *path, char *const argv[]) __asm__("sceSystemServiceLoadExec");

namespace wisp {
namespace {

using ps4ui::Canvas;
using ps4ui::Color;
using ps4ui::rgb;
using ps4ui::Weight;

constexpr int W = shell::SCREEN_W;
constexpr int H = shell::SCREEN_H;
constexpr int TAB_H = 46;
constexpr int TOOLBAR_H = 108;
constexpr int BUTTON = 52;
constexpr int ADDRESS_X = 290;
constexpr int MENU_X = W - 70;
constexpr int EXT_BUTTON = 48;     // an extension's toolbar button
constexpr int STORE_W = 216;       // "Add to OrbisWeb" on a store's page

// the dark theme
const Color BG = rgb(0x1f2023);
const Color TAB_IDLE = rgb(0x2b2c30);
const Color TAB_ACTIVE = rgb(0x3a3b40);
const Color FIELD = rgb(0x121316);
const Color TEXT = rgb(0xe8eaed);
const Color DIM = rgb(0x9aa0a6);
const Color ACCENT = rgb(0x8ab4f8);
const Color HOVER = rgb(0x45464c);

const char *const HOME_DEFAULT = "https://www.google.com";

// A sleeping tab's last picture (put_to_sleep): written to disk on a thread of its own, then let go
// here; wake() takes it from here if the write is still going, else from the file
struct Picture {
    std::mutex lock;
    std::vector<uint8_t> pixels;   // until on disk
    int width = 0, height = 0, stride = 0;
    bool on_disk = false;
    std::string path;
};

struct Tab {
    int id = 0;                   // stable (extensions' tab ids)
    WebKitWebView *view = nullptr;   // none for Files, or while the tab sleeps
    WPEView *wpe = nullptr;
    WebKitUserContentManager *ucm = nullptr;   // its own: content scripts know their tab
    std::string title, uri;
    double progress = 0;
    bool loading = false;
    bool crashed = false;
    bool files = false;           // Files (its own tab since v00.16): Files draws it, no web view
    bool downloads = false;       // the download manager's page (ui/dm): no web view either
    bool player = false;          // Wisp Player (ui/player): a video or music file playing
    // A tab hidden long enough, or when memory runs short, sleeps: its page closes (its memory in the
    // web process goes), its history (with scroll positions and form fields) and its last picture go
    // to disk; shown again, the picture is up at once while the page comes back (put_to_sleep, wake)
    bool asleep = false;
    double hidden_at = 0;         // when it last stopped being the tab on show
    GBytes *session = nullptr;    // (asleep) WebKit's session state
    std::shared_ptr<Picture> picture;
};

struct Download {
    std::string name, path;
    double progress = 0;
    bool done = false, failed = false;
    bool install = false;         // an extension to install when it is done
};

// an extension's toolbar button (and its popup, while open)
struct ExtButton {
    std::string id, name, badge;
    uint32_t badge_color = 0;
    ps4ui::Image icon;
};

enum class Overlay { None, Menu, Downloads, Extensions, Context, Addresses };

struct State {
    WebKitWebContext *context = nullptr;
    WebKitNetworkSession *session = nullptr;
    WebKitSettings *settings = nullptr;
    bool gpu_rendering = false;   // this run's web processes are wisp-web-gpu.bin (gpu=1 at the start)
    std::vector<Tab> tabs;
    int active = -1;
    bool fullscreen = false;
    // a page's own fullscreen (v00.36: a video's fullscreen button): its view, and F11's state before it
    WebKitWebView *page_fullscreen_view = nullptr;
    bool fullscreen_before_page = false;
    // the address bar's list (v00.36): addresses used before, under the field
    std::vector<pages::Suggestion> addr_list;
    std::string addr_for = "\x01";   // the text the list was made for ("\x01": none made)
    int addr_index = -1;              // the row the arrows chose (-1: none: the text typed)
    bool dirty = true;            // the screen needs composing
    bool toolbar_dirty = true;    // the toolbar picture needs drawing
    uint64_t shown_serial = 0;
    std::vector<uint32_t> compose;   // the frame, in ordinary memory (BGRA)
    std::vector<uint32_t> toolbar;   // the toolbar, BGRA
    std::string hover_link;
    std::string toast;
    double toast_until = 0;
    Overlay overlay = Overlay::None;
    int menu_index = 0;
    std::vector<Download> downloads;
    // the system keyboard: for the address field, or a page's text field
    bool keyboard_for_address = false;
    std::function<void(bool, const std::string &)> keyboard_done;
    bool pointer_down = false;
    bool right_down = false, middle_down = false;   // the mouse's other buttons, down on the page
    float last_x = -1, last_y = -1;
    float pointer_x = 960, pointer_y = 540;         // the pointer now
    double pointer_moved_at = 0;                    // (the player hides it when it stays still)
    uint32_t modifiers = 0;       // WPEModifiers from the USB keyboard
    bool mods_held[8] = {};       // left/right Shift, Ctrl, Alt, Windows
    bool caps = false;            // Caps Lock
    // the address typed in place with a USB keyboard: its text, the caret (a byte offset), all selected
    bool editing = false;
    std::string edit_text;
    size_t edit_caret = 0;
    bool edit_all = false;
    // a page's own menu (a right click): WebKit's items, referenced while it is open
    std::vector<WebKitContextMenuItem *> ctx_items;
    std::vector<std::string> ctx_labels;
    std::vector<bool> ctx_enabled;
    int ctx_x = 0, ctx_y = 0, ctx_index = -1;
    double last_frame = 0;
    // composing (compose()): each row's number of the compose that last changed it, the same for
    // what each of the two screen buffers holds, the rows overlays covered (last time, this time)
    uint32_t gen = 0;
    std::vector<uint32_t> row_gen;
    std::vector<uint32_t> buffer_gen[2];
    std::vector<uint8_t> overlay_last, overlay_now, restore;
    WPEView *composed_view = nullptr;   // whose frame the page's rows hold
    int composed_w = -1, composed_h = -1;
    bool composed_fullscreen = false;
    // pacing: frames are composed right after the TV's vblank (shell::screen_start_vblank), or as soon
    // as a new page frame comes while the screen is free; a 16 ms timer without vblank events
    bool vsync = false;
    double vsync_start = 0;       // when the vblank thread started
    uint32_t vblanks_seen = 0;
    bool compose_soon = false;
    shell::Input last_input;
    GMainLoop *loop = nullptr;
    int next_tab_id = 1;
    std::vector<ExtButton> ext_buttons;
    bool ext_buttons_dirty = true;
    int ext_index = 0;                 // the Extensions list's selection
    WebKitWebView *popup = nullptr;    // an extension's popup over the page
    WPEView *popup_wpe = nullptr;
    int popup_x = 0, popup_y = 0, popup_w = 420, popup_h = 600;
    std::string store_crx;             // "Add to Wisp": the package of the store page on show
    std::vector<uint32_t> files_pixels;   // Files' picture (BGRA), redrawn when it changes
    std::vector<uint32_t> dm_pixels;      // the download manager's page, the same
    // The web process (every tab's) not answering: since when (WebKit's responsiveness timer: 3 s
    // without an answer to an input event, a reload, a load), 0 while it answers; whether its threads'
    // stacks were asked for this time; a restart of it under way (hung_page_check, restart_web_process)
    double web_hung_since = 0;
    bool web_hung_dumped = false;
    bool web_restarting = false;
    WebKitWebView *web_restart_view = nullptr;   // the tab that reloads once it is started again
    // The controller as a page's gamepad (v00.30, gamepad_frame): the page has it (game mode); the touch
    // pad's button held since (0: up) and whether this hold switched already; the sites the user took
    // the controller back from (settings: gamepad_declined); the timer that stops the motors
    bool game_mode = false;
    double touchpad_down_at = 0;
    bool touchpad_switched = false;
    double shoulders_down_at = 0;   // (v00.34) L1 + L2 + R1 + R2 held: the same switch
    bool shoulders_switched = false;
    // (v00.42) game mode on a page that reads no gamepads: the controller is a keyboard (keys_frame), and the
    // keys it holds down now (PadKey bits)
    bool keys_mode = false;
    uint32_t keys_held = 0;
    std::vector<std::string> gamepad_declined;
    guint rumble_stop = 0;
    // The WebGPU self-test asked for at this start (v00.31): since when, until its result came (0: none)
    double webgpu_test_since = 0;
};

State S;

void set_game_mode(bool on, const char *why);   // (the controller as a page's gamepad: below)
void page_key(guint keycode, guint keyval, bool down, uint32_t without);   // (Input, below)

// (v00.42) The controller as a keyboard (game mode on a page that reads no gamepads): each key below as its
// button goes down and up; X11 key codes (evdev + 8: WebKit's KeyboardEvent.code) and keysyms
enum PadKey { K_UP, K_DOWN, K_LEFT, K_RIGHT, K_SPACE, K_ENTER, K_Z, K_X, K_SHIFT, K_CTRL, K_ESC, K_COUNT };
constexpr guint PAD_KEY_CODES[K_COUNT][2] = {
    { 111, 0xff52 }, { 116, 0xff54 }, { 113, 0xff51 }, { 114, 0xff53 },   // the arrows
    { 65, 0x0020 }, { 36, 0xff0d }, { 52, 0x007a }, { 53, 0x0078 },       // Space, Enter, z, x
    { 50, 0xffe1 }, { 37, 0xffe3 }, { 9, 0xff1b },                         // left Shift, left Ctrl, Escape
};

void release_pad_keys()
{
    for (int k = 0; k < K_COUNT; k++)
        if (S.keys_held & (1u << k))
            page_key(PAD_KEY_CODES[k][0], PAD_KEY_CODES[k][1], false, 0);
    S.keys_held = 0;
}

void keys_frame(const shell::PadState &pad)
{
    uint32_t want = 0;
    if (pad.connected) {
        const uint32_t b = pad.buttons;
        const auto stick = [](uint8_t v) { return (v - 127.5) / 127.5; };
        const bool held = S.shoulders_switched;   // (not the shoulders of the hold that switched)
        if ((b & shell::PAD_UP) || stick(pad.ly) < -0.5)
            want |= 1u << K_UP;
        if ((b & shell::PAD_DOWN) || stick(pad.ly) > 0.5)
            want |= 1u << K_DOWN;
        if ((b & shell::PAD_LEFT) || stick(pad.lx) < -0.5)
            want |= 1u << K_LEFT;
        if ((b & shell::PAD_RIGHT) || stick(pad.lx) > 0.5)
            want |= 1u << K_RIGHT;
        if (b & shell::PAD_CROSS)
            want |= 1u << K_SPACE;
        if (b & shell::PAD_CIRCLE)
            want |= 1u << K_ENTER;
        if (b & shell::PAD_SQUARE)
            want |= 1u << K_Z;
        if (b & shell::PAD_TRIANGLE)
            want |= 1u << K_X;
        if (!held && ((b & shell::PAD_L2) || pad.l2 > 128))
            want |= 1u << K_SHIFT;
        if (!held && ((b & shell::PAD_R2) || pad.r2 > 128))
            want |= 1u << K_CTRL;
        if (b & shell::PAD_OPTIONS)
            want |= 1u << K_ESC;
    }
    const uint32_t changed = want ^ S.keys_held;
    for (int k = 0; k < K_COUNT; k++)
        if (changed & (1u << k))
            page_key(PAD_KEY_CODES[k][0], PAD_KEY_CODES[k][1], (want & (1u << k)) != 0, 0);
    S.keys_held = want;
}

uint32_t now_ms() { return static_cast<uint32_t>(wisp_seconds() * 1000); }

void toast(const std::string &text)
{
    S.toast = text;
    S.toast_until = wisp_seconds() + 4;
    S.dirty = true;
    wisp_log("toast: %s", text.c_str());
}

// "; 171 MiB flexible memory left" after the tab lines in ui.log (Settings: memory_log=0 leaves it out)
std::string memory_note()
{
    if (!wisp_setting_int("memory_log", 1))
        return "";
    return "; " + std::to_string(wisp_flexible_free() >> 20) + " MiB flexible memory left";
}

// The tab on show (any kind) / the web page on show (nullptr while Files is: a tab shown is awake)
Tab *active_entry() { return S.active >= 0 && S.active < static_cast<int>(S.tabs.size()) ? &S.tabs[S.active] : nullptr; }
Tab *active_tab()
{
    Tab *tab = active_entry();
    return tab && tab->view ? tab : nullptr;
}
bool files_tab_active()
{
    const Tab *tab = active_entry();
    return tab && tab->files;
}
int content_y() { return S.fullscreen ? 0 : TOOLBAR_H; }
int content_h() { return H - content_y(); }

int tab_index(WebKitWebView *view)
{
    for (size_t i = 0; i < S.tabs.size(); i++)
        if (S.tabs[i].view == view)
            return static_cast<int>(i);
    return -1;
}

int index_of_id(int id)
{
    for (size_t i = 0; i < S.tabs.size(); i++)
        if (S.tabs[i].id == id)
            return static_cast<int>(i);
    return -1;
}

// The extension package behind a store's page, for "Add to Wisp": the Chrome Web Store
// (chromewebstore.google.com/detail/<name>/<id>), Opera add-ons (addons.opera.com/.../details/<slug>/)
// and Mozilla's (addons.mozilla.org/.../addon/<slug>/)
std::string store_package(const std::string &uri)
{
    auto after = [&](const std::string &marker) -> std::string {
        const size_t at = uri.find(marker);
        if (at == std::string::npos)
            return {};
        std::string rest = uri.substr(at + marker.size());
        return rest.substr(0, rest.find_first_of("/?#"));
    };
    if (uri.find("://chromewebstore.google.com/detail/") != std::string::npos || uri.find("://chrome.google.com/webstore/detail/") != std::string::npos) {
        std::string rest = uri.substr(uri.find("/detail/") + 8);
        const size_t slash = rest.find('/');
        std::string id = slash == std::string::npos ? rest : rest.substr(slash + 1);
        id = id.substr(0, id.find_first_of("/?#"));
        if (id.size() != 32)
            return {};
        return "https://clients2.google.com/service/update2/crx?response=redirect&prodversion=131.0.0.0"
            "&acceptformat=crx2,crx3&x=id%3D" + id + "%26installsource%3Dondemand%26uc";
    }
    if (uri.find("://addons.opera.com/") != std::string::npos && uri.find("/details/") != std::string::npos) {
        const std::string slug = after("/details/");
        return slug.empty() ? std::string() : "https://addons.opera.com/extensions/download/" + slug + "/";
    }
    if (uri.find("://addons.mozilla.org/") != std::string::npos && uri.find("/addon/") != std::string::npos) {
        const std::string slug = after("/addon/");
        return slug.empty() ? std::string() : "https://addons.mozilla.org/firefox/downloads/latest/" + slug + "/latest.xpi";
    }
    return {};
}

void tab_changed(int index)
{
    const Tab &tab = S.tabs[index];
    ext::tab_updated(tab.id, tab.uri, tab.title, tab.loading);
    if (index == S.active) {
        const std::string crx = store_package(tab.uri);
        if (crx != S.store_crx) {
            S.store_crx = crx;
            S.toolbar_dirty = S.dirty = true;
        }
    }
}

// --- Address handling ---------------------------------------------------------------------------------

std::string to_uri(const std::string &typed)
{
    std::string text = typed;
    while (!text.empty() && isspace(static_cast<unsigned char>(text.back())))
        text.pop_back();
    while (!text.empty() && isspace(static_cast<unsigned char>(text.front())))
        text.erase(text.begin());
    if (text.empty())
        return {};
    if (text == "about:orbisweb" || text == "about:wisp" || text == "about:version" || text == "about:about")
        return "orbisweb://about";
    // (v00.32: OrbisWeb's own pages, as a desktop browser's about: and chrome:// ones)
    for (const char *page : { "settings", "history", "bookmarks", "cookies" })
        if (text == std::string("about:") + page || text == std::string("chrome://") + page || text == std::string("orbisweb:") + page)
            return std::string("orbisweb://") + page;
    if (text.find("://") != std::string::npos || text.rfind("about:", 0) == 0 || text.rfind("file:", 0) == 0)
        return text;
    // a host-looking word (no spaces, a dot or localhost) is an address; anything else a search
    if (text.find(' ') == std::string::npos && (text.find('.') != std::string::npos || text.rfind("localhost", 0) == 0))
        return "https://" + text;
    char *escaped = g_uri_escape_string(text.c_str(), nullptr, FALSE);
    std::string uri = std::string(wisp_setting("search", "https://www.google.com/search?q=")) + escaped;
    g_free(escaped);
    return uri;
}

// --- About OrbisWeb (orbisweb://about; the menu's About OrbisWeb, about:orbisweb; v00.24's wisp://about too) -------

#ifndef WISP_HAVE_WASM
#define WISP_HAVE_WASM 0
#endif
// the web process linked with WebKit's JIT build (app/CMakeLists.txt, v00.27 on)
#ifndef WISP_HAVE_JIT
#define WISP_HAVE_JIT 0
#endif
// the web process's WebKit build has WebGL (app/CMakeLists.txt, v00.30 on)
#ifndef WISP_HAVE_WEBGL
#define WISP_HAVE_WEBGL 0
#endif
// WebGL for pages: in this build, with the GPU web process (CPU + GPU or GPU rendering), not turned off
// in the menu (settings: webgl)
bool webgl_on() { return WISP_HAVE_WEBGL && wisp_rendering() != WispRendering::Cpu && wisp_setting_int("webgl", 1) != 0; }
// the GPU web process has Dawn (WebGPU, v00.31 on: its self-test; app/CMakeLists.txt)
#ifndef WISP_HAVE_DAWN
#define WISP_HAVE_DAWN 0
#endif
// the web process's WebKit build has WebGPU for pages (v00.40 on: WebCore's WebGPU on Dawn, WebKit patch 0039)
#ifndef WISP_HAVE_WEBGPU
#define WISP_HAVE_WEBGPU 0
#endif
// WebGPU for pages: in this build, with the GPU web process (CPU + GPU or GPU rendering), not turned off in
// Settings (webgpu)
bool webgpu_on() { return WISP_HAVE_WEBGPU && wisp_rendering() != WispRendering::Cpu && wisp_setting_int("webgpu", 1) != 0; }
constexpr const char *WEBGPU_TEST_FILE = WISP_DATA "/run/webgpu-test.txt";

std::string webgpu_test_result();
void toast(const std::string &text);

// Each frame while a WebGPU self-test runs (every 2 s): its result as a toast once the file has it
void webgpu_test_check()
{
    if (!S.webgpu_test_since)
        return;
    static double last = 0;
    const double now = wisp_seconds();
    if (now - last < 2)
        return;
    last = now;
    const std::string result = webgpu_test_result();
    const bool done = result.rfind("RESULT: ", 0) == 0;
    if (done || (!result.empty() && now - S.webgpu_test_since > 90)) {
        toast("WebGPU test: " + (done ? result.substr(8) : result) + " (About OrbisWeb; " + WEBGPU_TEST_FILE + ")");
        S.webgpu_test_since = 0;
    } else if (now - S.webgpu_test_since > 120) {
        toast(std::string("WebGPU test: no result after 2 minutes (") + WEBGPU_TEST_FILE + ")");
        S.webgpu_test_since = 0;
    }
}

// The last WebGPU self-test's result line ("" when there was none)
std::string webgpu_test_result()
{
    std::string result;
    if (FILE *f = fopen(WEBGPU_TEST_FILE, "r")) {
        char line[1024];
        while (fgets(line, sizeof line, f)) {
            const char *at = strstr(line, "RESULT: ");
            if (!at)
                at = strstr(line, "FAIL");
            if (at) {
                result = at;
                while (!result.empty() && (result.back() == '\n' || result.back() == '\r'))
                    result.pop_back();
            }
        }
        fclose(f);
    }
    return result;
}

// What runs the pages' JavaScript: the web process writes it as it starts (child/child_main.cpp: the JIT
// when this console runs generated code and the menu leaves it on, else the LLInt interpreter)
std::string javascript_engine()
{
    if (FILE *f = fopen(WISP_DATA "/run/javascript.txt", "r")) {
        char line[256] = {};
        const bool got = fgets(line, sizeof line, f) != nullptr;
        fclose(f);
        std::string text = got ? line : "";
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
            text.pop_back();
        if (!text.empty())
            return "JavaScriptCore, " + text;
    }
    return WISP_HAVE_JIT && wisp_setting_int("jit", 1) ? "JavaScriptCore's JIT (Baseline and DFG tiers)" : "JavaScriptCore's interpreter (no JIT)";
}

const char *const WISP_AUTHOR = "iHaiDeeZ";
const char *const WISP_AUTHOR_GITHUB = "https://github.com/iHaiDeeZ";

std::string html_text(const std::string &text)
{
    std::string out;
    for (char c : text) {
        switch (c) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        default: out += c;
        }
    }
    return out;
}

// The page: who made OrbisWeb, this build and this console, and what it is built on (the copies in the package)
std::string about_html()
{
    char webkit[32];
    snprintf(webkit, sizeof webkit, "%u.%u.%u", webkit_get_major_version(), webkit_get_minor_version(), webkit_get_micro_version());
    const std::string github = WISP_AUTHOR_GITHUB;
    const std::string github_shown = github.substr(github.find("://") + 3);
    std::string html = R"HTML(<!doctype html><html lang="en"><head><meta charset="utf-8"><title>About OrbisWeb</title>
<style>
:root { color-scheme: dark; }
body { margin: 0; background: #202124; color: #e8eaed; font: 22px/1.5 Roboto, "Liberation Sans", sans-serif; }
main { max-width: 1100px; margin: 0 auto; padding: 56px 48px 72px; }
.mark { display: flex; align-items: center; gap: 32px; }
.logo { width: 144px; height: 144px; border-radius: 30px; flex: none; box-shadow: 0 10px 34px rgba(0, 0, 0, .5); }
h1 { font-size: 64px; margin: 0; line-height: 1.1; }
.tag { color: #9aa0a6; font-size: 26px; margin: 4px 0 0; }
.card { background: #2b2c30; border-radius: 18px; padding: 28px 34px; margin-top: 34px; }
.card h2 { font-size: 20px; letter-spacing: .08em; text-transform: uppercase; color: #9aa0a6; margin: 0 0 14px; }
.author { font-size: 30px; }
.author b { color: #fff; }
a { color: #8ab4f8; text-decoration: none; }
a:hover, a:focus { text-decoration: underline; }
table { border-collapse: collapse; width: 100%; }
td { padding: 7px 0; vertical-align: top; }
td:first-child { color: #9aa0a6; width: 42%; padding-right: 24px; }
.note { color: #9aa0a6; font-size: 18px; margin-top: 30px; }
</style></head><body><main>
<div class="mark"><img class="logo" src="orbisweb://about/icon.png" alt=""><div><h1>OrbisWeb</h1><p class="tag">A web browser for the PS4 &middot; version __VERSION__</p></div></div>
<div class="card"><h2>Made by</h2>
<p class="author"><b>__AUTHOR__</b></p>
<p>GitHub: <a href="__GITHUB__">__GITHUB_SHOWN__</a></p></div>
<div class="card"><h2>This console</h2><table>
<tr><td>Drawing</td><td>__RENDERING__</td></tr>
<tr><td>JavaScript</td><td>__JS____WASM__</td></tr>
<tr><td>WebGL</td><td>__WEBGL__</td></tr>
<tr><td>WebGPU</td><td>__WEBGPU__</td></tr>
<tr><td>Controller</td><td>A page that reads gamepads gets it as a standard gamepad (hold L1 + L2 + R1 + R2, or the touch pad's button: the pointer)</td></tr>
<tr><td>Memory</td><td>__DIRECT__ MiB of direct memory (heaps, GPU); __POOL__ MiB of the shared pool free</td></tr>
</table></div>
<div class="card"><h2>Built on</h2><table>
<tr><td>Engine</td><td>WPE WebKit __WEBKIT__ (LGPL 2.1, BSD), with Skia, ICU 77, HarfBuzz 11.2, FreeType 2.13, fontconfig 2.16</td></tr>
<tr><td>Network</td><td>libsoup 3.6, GLib 2.86, glib-networking 2.80, OpenSSL, nghttp2 1.65</td></tr>
<tr><td>Video and sound</td><td>GStreamer 1.26 and FFmpeg 8.0 (LGPL 2.1), Opus, Vorbis, Theora</td></tr>
<tr><td>Graphics</td><td>Mesa 26.0 (Zink on RADV, MIT), ANGLE for WebGL (BSD), Dawn for WebGPU (Apache 2.0)</td></tr>
<tr><td>Downloads</td><td>aria2 1.37 (GPL 2)</td></tr>
<tr><td>Archives</td><td>libarchive 3.8 (BSD)__UNRAR__, zlib, bzip2, xz, zstd, lz4</td></tr>
<tr><td>Ad blocking</td><td>EasyList and EasyPrivacy (GPL 3, CC BY-SA 3.0)</td></tr>
<tr><td>Fonts</td><td>Noto, Liberation, Roboto (OFL, Apache 2.0)</td></tr>
<tr><td>PS4 base</td><td>ps4-kit (MIT) on the OpenOrbis toolchain</td></tr>
<tr><td>OrbisWeb</td><td>GPL 2 or later, with its screen from Vita3K's PS4 port (GPL 2) and its shell from WinePS4 (LGPL 2.1): github.com/iHaiDeeZ/OrbisWeb</td></tr>
</table></div>
<p class="note">Each part keeps its own license. OrbisWeb runs on homebrew-enabled consoles.</p>
</main></body></html>)HTML";
    const auto put = [&](const char *name, const std::string &value) {
        for (size_t at = html.find(name); at != std::string::npos; at = html.find(name, at + value.size()))
            html.replace(at, strlen(name), value);
    };
    put("__VERSION__", html_text(WISP_VERSION_STRING));
#if defined(WISP_HAVE_UNRAR)
    put("__UNRAR__", ", UnRAR 7.3 (RARLAB's freeware license: wisp/licenses/unrar.txt)");
#else
    put("__UNRAR__", "");
#endif
    put("__AUTHOR__", html_text(WISP_AUTHOR));
    put("__GITHUB_SHOWN__", html_text(github_shown));
    put("__GITHUB__", html_text(github));
    put("__RENDERING__", html_text(wisp_rendering_name(wisp_rendering())));
    put("__JS__", html_text(javascript_engine()));
    put("__WASM__", WISP_HAVE_WASM && wisp_setting_int("wasm", 1) ? "; WebAssembly by its in-place interpreter" : "");
    {
        const std::string result = webgpu_test_result();
        const std::string test = result.empty() ? std::string() : "; the last test: " + html_text(result);
        put("__WEBGPU__", !WISP_HAVE_DAWN ? std::string("Not in this build")
            : !WISP_HAVE_WEBGPU ? (result.empty() ? std::string("In development: Settings &gt; WebGPU test checks Dawn on this console")
                : "In development" + test)
            : wisp_rendering() == WispRendering::Cpu ? "Off: CPU drawing has no GPU (Settings &gt; Drawing)" + test
            : webgpu_on() ? "WebGPU by Dawn on the GPU (Vulkan, Mesa's RADV)" + test : "Off (Settings &gt; WebGPU)" + test);
    }
    put("__WEBGL__", !WISP_HAVE_WEBGL ? "Not in this build" : wisp_rendering() == WispRendering::Cpu ? "Off: CPU drawing has no GPU (Settings &gt; Drawing)"
        : webgl_on() ? "WebGL 1 and 2, by ANGLE on Mesa's Zink" : "Off (Settings &gt; WebGL)");
    put("__DIRECT__", std::to_string(wisp_direct_memory_size() >> 20));
    put("__POOL__", std::to_string(wisp_flexible_free() >> 20));
    put("__WEBKIT__", webkit);
    return html;
}

// orbisweb:// pages (v00.24's wisp:// too): about, and the icon it shows (the package's own, sce_sys/icon0.png)
void serve_wisp_page(WebKitURISchemeRequest *request, gpointer)
{
    const std::string uri = webkit_uri_scheme_request_get_uri(request);
    const size_t sep = uri.find("://");
    std::string page = sep == std::string::npos ? std::string() : uri.substr(sep + 3);
    const size_t query = page.find_first_of("?#");
    std::string query_text;
    if (query != std::string::npos) {
        if (page[query] == '?')
            query_text = page.substr(query + 1, page.find('#', query) == std::string::npos ? std::string::npos : page.find('#', query) - query - 1);
        page.resize(query);
    }
    while (!page.empty() && page.back() == '/')
        page.pop_back();
    // Settings, History, Bookmarks, Cookies (v00.32, ui/pages.cpp)
    if (pages::serve(request, page, query_text))
        return;
    if (page == "about") {
        const std::string html = about_html();
        GBytes *bytes = g_bytes_new(html.data(), html.size());
        GInputStream *stream = g_memory_input_stream_new_from_bytes(bytes);
        webkit_uri_scheme_request_finish(request, stream, static_cast<gint64>(html.size()), "text/html");
        g_object_unref(stream);
        g_bytes_unref(bytes);
        return;
    }
    gchar *data = nullptr;
    gsize size = 0;
    if (page == "about/icon.png" && g_file_get_contents(WISP_APP "/sce_sys/icon0.png", &data, &size, nullptr)) {
        GBytes *bytes = g_bytes_new_take(data, size);
        GInputStream *stream = g_memory_input_stream_new_from_bytes(bytes);
        webkit_uri_scheme_request_finish(request, stream, static_cast<gint64>(size), "image/png");
        g_object_unref(stream);
        g_bytes_unref(bytes);
        return;
    }
    GError *error = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "No such OrbisWeb page");
    webkit_uri_scheme_request_finish_error(request, error);
    g_error_free(error);
}

bool is_about_page(const std::string &uri)
{
    return uri.rfind("orbisweb://about", 0) == 0 || uri.rfind("wisp://about", 0) == 0;
}

// --- Tabs ---------------------------------------------------------------------------------------------

void activate(int index);
WebKitWebView *new_tab(const std::string &uri, WebKitWebView *related, bool focus);
void forget_tab(int index);
void open_files_tab(const std::string &folder);
void open_local_file(const std::string &path);
void ask_text(const std::string &title, const std::string &initial, std::function<void(bool, const std::string &)> done);
void set_fullscreen(bool on);
void page_fullscreen(WebKitWebView *view, bool on);
void leave_page_fullscreen();
gboolean on_enter_fullscreen(WebKitWebView *view, gpointer);
gboolean on_leave_fullscreen(WebKitWebView *view, gpointer);
void open_address_keyboard();
void edit_stop();

void on_title(WebKitWebView *view, GParamSpec *, gpointer)
{
    int i = tab_index(view);
    if (i < 0)
        return;
    const char *title = webkit_web_view_get_title(view);
    S.tabs[i].title = title ? title : "";
    S.toolbar_dirty = S.dirty = true;
    tab_changed(i);
    if (title && webkit_web_view_get_uri(view))
        pages::note_title(webkit_web_view_get_uri(view), title);   // (the history's entry for the page)
}

void on_uri(WebKitWebView *view, GParamSpec *, gpointer)
{
    int i = tab_index(view);
    if (i < 0)
        return;
    const char *uri = webkit_web_view_get_uri(view);
    S.tabs[i].uri = uri ? uri : "";
    S.toolbar_dirty = S.dirty = true;
    tab_changed(i);
}

void on_progress(WebKitWebView *view, GParamSpec *, gpointer)
{
    int i = tab_index(view);
    if (i < 0)
        return;
    S.tabs[i].progress = webkit_web_view_get_estimated_load_progress(view);
    const bool was_loading = S.tabs[i].loading;
    S.tabs[i].loading = webkit_web_view_is_loading(view);
    S.toolbar_dirty = S.dirty = true;
    if (was_loading != S.tabs[i].loading)
        tab_changed(i);
}

void on_load_changed(WebKitWebView *view, WebKitLoadEvent event, gpointer)
{
    static const char *const names[] = { "started", "redirected", "committed", "finished" };
    wisp_log("load %s: %s", names[event], webkit_web_view_get_uri(view) ? webkit_web_view_get_uri(view) : "");
    // the page that had the controller goes: the browser has it again (the next page that reads
    // gamepads takes it, v00.30)
    if (event == WEBKIT_LOAD_STARTED && S.game_mode && active_tab() && active_tab()->view == view)
        set_game_mode(false, "the page loads another");
    // the history (v00.32): a tab's page committed (http and https; its title comes in on_title)
    if (event == WEBKIT_LOAD_COMMITTED && tab_index(view) >= 0 && webkit_web_view_get_uri(view))
        pages::note_visit(webkit_web_view_get_uri(view));
    on_progress(view, nullptr, nullptr);
}

gboolean on_load_failed(WebKitWebView *, WebKitLoadEvent, char *uri, GError *error, gpointer)
{
    wisp_log("load failed: %s: %s", uri, error ? error->message : "?");
    if (error && !g_error_matches(error, WEBKIT_NETWORK_ERROR, WEBKIT_NETWORK_ERROR_CANCELLED))
        toast(std::string("Could not load the page: ") + (error ? error->message : ""));
    return FALSE;   // WebKit's own error page
}

std::string html_escape(const std::string &text)
{
    std::string out;
    for (char c : text) {
        switch (c) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        default: out += c;
        }
    }
    return out;
}

// the TLS error flags in words
std::string tls_reasons(GTlsCertificateFlags flags)
{
    static const struct { GTlsCertificateFlags bit; const char *text; } reasons[] = {
        { G_TLS_CERTIFICATE_UNKNOWN_CA, "signed by an authority OrbisWeb does not know" },
        { G_TLS_CERTIFICATE_BAD_IDENTITY, "made out to another site" },
        { G_TLS_CERTIFICATE_NOT_ACTIVATED, "not valid yet" },
        { G_TLS_CERTIFICATE_EXPIRED, "expired" },
        { G_TLS_CERTIFICATE_REVOKED, "revoked" },
        { G_TLS_CERTIFICATE_INSECURE, "signed with a weak algorithm" },
        { G_TLS_CERTIFICATE_GENERIC_ERROR, "impossible to check" },
    };
    std::string out;
    for (const auto &reason : reasons) {
        if (flags & reason.bit) {
            if (!out.empty())
                out += ", ";
            out += reason.text;
        }
    }
    return out.empty() ? "not accepted" : out;
}

std::string certificate_time(GDateTime *when)
{
    char text[40] = "?";
    if (when) {
        wisp_format_utc(g_date_time_to_unix(when), text, sizeof text);
        g_date_time_unref(when);
    }
    return text;
}

// A certificate WebKit refuses: Wisp's own page in place of WebKit's ("Unacceptable TLS certificate"
// and nothing else), saying why, and when the console's clock is the reason, how to set it (v00.08 on
// .43: the clock read 1970, so every site's certificate was "not valid yet")
gboolean on_tls_error(WebKitWebView *view, char *uri, GTlsCertificate *cert, GTlsCertificateFlags flags, gpointer)
{
    char clock[40];
    wisp_format_utc(time(nullptr), clock, sizeof clock);
    const std::string from = certificate_time(cert ? g_tls_certificate_get_not_valid_before(cert) : nullptr);
    const std::string until = certificate_time(cert ? g_tls_certificate_get_not_valid_after(cert) : nullptr);
    const std::string reasons = tls_reasons(flags);
    std::string host = uri ? uri : "";
    if (GUri *parsed = uri ? g_uri_parse(uri, G_URI_FLAGS_NONE, nullptr) : nullptr) {
        if (g_uri_get_host(parsed))
            host = g_uri_get_host(parsed);
        g_uri_unref(parsed);
    }
    wisp_log("TLS error %#x (%s) for %s; certificate valid %s to %s; console clock %s", flags, reasons.c_str(),
        uri ? uri : "", from.c_str(), until.c_str(), clock);

    const bool clock_wrong = (flags & (G_TLS_CERTIFICATE_NOT_ACTIVATED | G_TLS_CERTIFICATE_EXPIRED)) && wisp_clock_behind();
    const std::string title = clock_wrong ? "The console's clock is wrong" : "This site's certificate is not trusted";
    std::string body;
    if (clock_wrong) {
        body = "<p>The console's clock says <b>" + html_escape(clock) + "</b>. This site's certificate is valid from <b>"
            + html_escape(from) + "</b>, so it looks not valid yet, and so does every other secure site's until the "
            "clock is right.</p><div class=\"steps\">On the PS4: <b>Settings &gt; System &gt; Date and Time &gt; Date and "
            "Time Settings</b>, then <b>Set Using Internet</b> or set the date and time by hand. Then reload this page."
            "</div>";
    } else {
        body = "<p>OrbisWeb could not confirm that this is the real <b>" + html_escape(host) + "</b>: its certificate is <b>"
            + html_escape(reasons) + "</b>.</p><p>Someone may be intercepting the connection, or the site is set up "
            "wrong, so OrbisWeb does not open it.</p>";
    }
    body += "<p class=\"dim\">" + html_escape(host) + " &middot; " + html_escape(reasons) + " &middot; certificate valid "
        + html_escape(from) + " to " + html_escape(until) + " &middot; console clock " + html_escape(clock) + "</p>";
    const std::string html = "<!doctype html><html><head><meta charset=\"utf-8\"><title>" + title + "</title><style>"
        "html{background:#1f2023;color:#e8eaed;font-family:sans-serif}"
        "body{max-width:60rem;margin:14vh auto 0;padding:0 2rem;font-size:1.4rem;line-height:1.5}"
        "h1{font-size:2.3rem;font-weight:600;margin:0 0 1.3rem}p{margin:0 0 1rem}b{color:#fff}"
        ".steps{background:#2b2c30;border-left:4px solid #8ab4f8;padding:1rem 1.4rem;margin:1.6rem 0}"
        ".dim{color:#9aa0a6;font-size:1.05rem;margin-top:2.4rem}"
        "</style></head><body><h1>" + title + "</h1>" + body + "</body></html>";
    webkit_web_view_load_alternate_html(view, html.c_str(), uri, nullptr);
    toast(clock_wrong ? "The console's clock is wrong: set the date and time, then reload" : "The site's certificate is not trusted");
    return TRUE;   // handled: no load-failed (and no WebKit error page) after this
}

// Whether the GPU web process's last crash (its kit crash log, written in the last minute) was WebKit
// stopping itself on its own check: CRASH() and a failed release assertion write to 0xbbadbeef. That
// is no fault of the GPU's (v00.30-31: a WebAssembly game's memory stopped bmalloc, WebKit patch 0032,
// and two of those took GPU drawing, and WebGL with it, away).
bool last_crash_was_webkit_check()
{
    constexpr const char *LOG = WISP_DATA "/kit-web-gpu/crash.log";
    struct stat st;
    if (stat(LOG, &st) != 0 || time(nullptr) - st.st_mtime > 60)
        return false;
    FILE *f = fopen(LOG, "r");
    if (!f)
        return false;
    std::string last;
    char line[512];
    while (fgets(line, sizeof line, f))
        if (strncmp(line, "=== signal", 10) == 0)
            last = line;
    fclose(f);
    return last.find("fault address 0xbbadbeef") != std::string::npos;
}

// Whether the web process that ran last had JavaScriptCore's concurrent collector on (v00.33: it says
// so in run/concurrent-gc.txt as it starts, child/child_main.cpp)
bool concurrent_gc_was_on()
{
    FILE *f = fopen(WISP_DATA "/run/concurrent-gc.txt", "r");
    if (!f)
        return false;
    const int c = fgetc(f);
    fclose(f);
    return c == '1';
}

// (v00.44) Whether two crashes, or two restarts of a page that stopped answering, turn the concurrent collector
// off for the rest of the run (v00.33). Off by default: the web process checks at its start that the collector can
// stop and resume its threads (child/child_main.cpp), and every hang and crash since came from memory, a page's
// own scripts or the GPU, never the collector; v00.43 took it away after two such ends (Hot Pursuit 2's memory,
// a Zink fault short of memory) and Star Fox 64 and Mortal Kombat then ran on stop-the-world collections.
bool gc_fallback_on()
{
    return wisp_setting_int("gc_fallback", 0) != 0;
}

// An automatic fallback: the setting for the rest of this run, and a mark that the next start takes it
// back (restore_fallbacks)
void set_fallback(const char *key, const char *value)
{
    wisp_setting_set(key, value);
    wisp_setting_set((std::string("fallback_") + key).c_str(), "1");
}

// (v00.33) The fallbacks below last one run: a start tries the GPU, the JIT and the concurrent collector
// again, as a desktop browser tries its GPU again at each start. v00.32 and older wrote rendering=cpu for
// good, and a console where that happened is taken back once (with a word); "gpu" is a key nothing reads
// since rendering came.
void restore_fallbacks()
{
    if (!wisp_setting_int("fallbacks_v33", 0)) {
        wisp_setting_set("fallbacks_v33", "1");
        if (wisp_rendering() == WispRendering::Cpu)
            wisp_setting_set("fallback_rendering", "1");
        wisp_setting_remove("gpu");
    }
    static const char *const KEYS[] = { "rendering", "jit", "concurrent_gc" };
    for (const char *key : KEYS) {
        const std::string mark = std::string("fallback_") + key;
        if (!wisp_setting_int(mark.c_str(), 0))
            continue;
        wisp_log("fallback: %s=%s was the last run's fallback: its default again", key, wisp_setting(key, "(default)"));
        wisp_setting_remove(key);
        wisp_setting_remove(mark.c_str());
        if (!strcmp(key, "rendering"))
            toast("Drawing is on the GPU again (CPU + GPU): it fell back to the CPU last time");
    }
}

void on_crashed(WebKitWebView *view, WebKitWebProcessTerminationReason reason, gpointer)
{
    int i = tab_index(view);
    wisp_log("web process ended (%s) under tab %d", reason == WEBKIT_WEB_PROCESS_CRASHED ? "crashed"
        : reason == WEBKIT_WEB_PROCESS_EXCEEDED_MEMORY_LIMIT ? "out of memory"
        : reason == WEBKIT_WEB_PROCESS_TERMINATED_BY_API ? "ended by OrbisWeb: it was not answering" : "terminated", i);
    S.web_hung_since = 0;   // (the next web process answers anew)
    // (a page in its own fullscreen: the toolbar comes back, and its view's toplevel leaves fullscreen)
    if (view == S.page_fullscreen_view) {
        platform_leave_fullscreen(webkit_web_view_get_wpe_view(view));
        page_fullscreen(nullptr, false);
    }
    if (reason == WEBKIT_WEB_PROCESS_TERMINATED_BY_API) {
        // restart_web_process: the tab it was asked in reloads once the new process is up; the others
        // show "stopped working" (Cross reloads them). Not a crash: the GPU fallback below does not count it.
        if (i >= 0 && view != S.web_restart_view)
            S.tabs[i].crashed = true;
        return;
    }
    if (i >= 0)
        S.tabs[i].crashed = true;
    toast("The page stopped working. Press Cross on it to reload.");
    // A web process that keeps ending (twice within a minute) takes one thing away for the rest of the
    // run, in turn: the concurrent collector (v00.33), GPU drawing (the next web process is wisp-web.bin:
    // the handoff reads the setting at each start, and it draws on the CPU whatever the settings say,
    // WebKit patch 0013), then the JIT. The next start has them back (restore_fallbacks). One web
    // process's end reaches every view: the calls within a second count once.
    static double last_end = -100, first_end = -100;
    static int ends = 0;
    static bool gc_switched = false, switched = false, jit_switched = false;
    // (v00.32) WebKit's own stop is no GPU fault: drawing stays on the GPU
    // (each tab hears of the same end: the answer is kept a second)
    static double checked_at = -100;
    static bool webkit_check = false;
    if (S.gpu_rendering) {
        if (wisp_seconds() - checked_at > 1.0) {
            checked_at = wisp_seconds();
            webkit_check = last_crash_was_webkit_check();
            if (webkit_check)
                wisp_log("gpu: the web process stopped on WebKit's own check (fault address 0xbbadbeef), not a GPU fault: drawing stays on the GPU");
        }
        if (webkit_check)
            return;
    }
    const double now = wisp_seconds();
    if (now - last_end > 1.0) {
        if (now - first_end > 60) {
            first_end = now;
            ends = 0;
        }
        ends++;
    }
    last_end = now;
    if (ends < 2)
        return;
    // (v00.44) the collector rung only when asked for (settings: gc_fallback): see gc_fallback_on()
    if (gc_fallback_on() && !gc_switched && concurrent_gc_was_on()) {
        gc_switched = true;
        ends = 0;   // (each turn below needs two more)
        set_fallback("concurrent_gc", "0");
        wisp_log("gc: the web process ended twice within a minute with the concurrent collector on: the next one collects on its JavaScript thread (until OrbisWeb starts again)");
        toast("The page stopped twice: the concurrent garbage collector is off until OrbisWeb starts again. Press Cross to reload.");
    } else if (S.gpu_rendering && !switched) {
        switched = true;
        ends = 0;
        set_fallback("rendering", "cpu");
        wisp_log("gpu: the web process ended twice within a minute: rendering=cpu, the next web process draws on the CPU (until OrbisWeb starts again)");
        toast("Drawing with the GPU stopped the page twice: CPU drawing until OrbisWeb starts again. Press Cross to reload.");
    } else if (!jit_switched && WISP_HAVE_JIT && wisp_setting_int("jit", 1)) {
        // then the JIT (v00.27): the next web process runs JavaScript on the LLInt (it reads jit as it starts)
        jit_switched = true;
        ends = 0;
        set_fallback("jit", "0");
        wisp_log("jit: the web process ended twice within a minute with the JIT on: jit=0, the next web process runs JavaScript on the LLInt (until OrbisWeb starts again)");
        toast("The page stopped twice with the JavaScript JIT on: the interpreter until OrbisWeb starts again. Press Cross to reload.");
    }
}

// --- A page that does not answer ----------------------------------------------------------------------
// WebKit's UI marks the web process unresponsive when an input event, a reload or a load has had no
// answer for 3 s. Every tab shares that process (one main thread, JavaScript interpreted): a heavy page
// can hold it a while, and v00.12-v00.14 lost IPC messages on the PS4's unix sockets (WebKit patch
// 0019), after which no click or reload reached the page again. ui.log says when it stops and starts
// answering; 5 s on, the web process writes every thread's stack to logs/web.hang.log (settings:
// watchdog=0 turns that off), and a reload, an address or Home starts the web process again, as a
// desktop browser offers for a page that hangs (settings: restart_hung_page=0 turns that off).

constexpr double HUNG_SECONDS = 5;

bool web_hung() { return S.web_hung_since > 0 && wisp_seconds() - S.web_hung_since >= HUNG_SECONDS; }
bool restart_hung_pages() { return wisp_setting_int("restart_hung_page", 1) != 0; }

void on_responsive(WebKitWebView *view, GParamSpec *, gpointer)
{
    // (each tab's view says it; the process is one)
    const bool answering = webkit_web_view_get_is_web_process_responsive(view);
    if (!answering && !S.web_hung_since) {
        S.web_hung_since = wisp_seconds();
        S.web_hung_dumped = false;
        wisp_log("the web process stopped answering (tab %d: WebKit had no answer for 3 s)", tab_index(view));
    } else if (answering && S.web_hung_since) {
        wisp_log("the web process answers again (after %.1f s more)", wisp_seconds() - S.web_hung_since);
        S.web_hung_since = 0;
    }
}

// From each frame: 5 s without an answer, the web process's threads' stacks (once each time) and a word
void hung_page_check()
{
    if (S.web_hung_dumped || !web_hung())
        return;
    S.web_hung_dumped = true;
    const int seconds = static_cast<int>(wisp_seconds() - S.web_hung_since) + 3;
    if (wisp_setting_int("watchdog", 1))
        wisp_ask_thread_dump(WispChild::Web, seconds);
    // (v00.34) a game's first load holds the page this long too (the kart game: 12 s and more), and a reload
    // then starts it all over: wait first
    if (restart_hung_pages())
        toast("The page is busy (a game loading?): give it time; Reload (F5) starts it again");
}

// The web process stopped answering: end it, wait until it is gone (two web processes would not fit in
// the memory pool the app's processes share), then reload view or load uri in it, in a new web process.
// The other tabs then show "stopped working" (Cross reloads them).
void restart_web_process(WebKitWebView *view, const std::string &uri)
{
    if (S.web_restarting)
        return;
    S.web_restarting = true;
    S.web_restart_view = view;
    wisp_log("restarting the web process: it has not answered for %.0f s", wisp_seconds() - S.web_hung_since + 3);
    // (v00.33) the second restart of a run with the concurrent collector on: off until the next start (a
    // collector that could not stop the JavaScript thread would hold every page for good)
    static int hung_with_gc = 0;
    if (gc_fallback_on() && concurrent_gc_was_on() && ++hung_with_gc >= 2 && !wisp_setting_int("fallback_concurrent_gc", 0)) {
        set_fallback("concurrent_gc", "0");
        wisp_log("gc: the web process stopped answering twice with the concurrent collector on: the next one collects on its JavaScript thread (until OrbisWeb starts again)");
    }
    toast("The page was not answering: starting it again");
    struct Pending {
        WebKitWebView *view;
        std::string uri;
        double since;
    };
    auto *pending = new Pending { WEBKIT_WEB_VIEW(g_object_ref(view)), uri, wisp_seconds() };
    webkit_web_view_terminate_web_process(view);
    g_timeout_add(100, +[](gpointer data) -> gboolean {
        auto *p = static_cast<Pending *>(data);
        // (common/wisp_handoff.cpp drops a local process when its control socket closes: it is gone)
        if (wisp_local_count_of(WispChild::Web) && wisp_seconds() - p->since < 5)
            return G_SOURCE_CONTINUE;
        S.web_restarting = false;
        S.web_restart_view = nullptr;
        S.web_hung_since = 0;
        const int i = tab_index(p->view);
        if (i >= 0) {
            S.tabs[i].crashed = false;
            wisp_log("the web process is gone (%.1f s): %s tab %d", wisp_seconds() - p->since,
                p->uri.empty() ? "reloading" : "loading the address in", i);
            if (p->uri.empty())
                webkit_web_view_reload(p->view);
            else
                webkit_web_view_load_uri(p->view, p->uri.c_str());
        }
        g_object_unref(p->view);
        delete p;
        S.dirty = S.toolbar_dirty = true;
        return G_SOURCE_REMOVE;
    }, pending);
}

// What the reload controls do (F5, Ctrl+R, the button, the menu): a web process that has stopped
// answering starts again instead (a reload it would never get)
void reload(Tab *tab, bool bypass_cache = false)
{
    if (!tab)
        return;
    if (S.web_restarting)
        return;
    if (web_hung() && restart_hung_pages()) {
        restart_web_process(tab->view, "");
        return;
    }
    if (bypass_cache)
        webkit_web_view_reload_bypass_cache(tab->view);
    else
        webkit_web_view_reload(tab->view);
}

// An address typed, Home: the same
void load(Tab *tab, const std::string &uri)
{
    if (!tab || uri.empty() || S.web_restarting)
        return;
    if (web_hung() && restart_hung_pages()) {
        restart_web_process(tab->view, uri);
        return;
    }
    webkit_web_view_load_uri(tab->view, uri.c_str());
}

void on_hover(WebKitWebView *, WebKitHitTestResult *hit, guint, gpointer)
{
    const char *link = webkit_hit_test_result_context_is_link(hit) ? webkit_hit_test_result_get_link_uri(hit) : nullptr;
    std::string text = link ? link : "";
    if (text != S.hover_link) {
        S.hover_link = text;
        S.dirty = true;
    }
}

WebKitWebView *on_create(WebKitWebView *view, WebKitNavigationAction *, gpointer)
{
    // window.open / target=_blank: a new tab, related to its opener (same web process, window.opener)
    return new_tab("", view, true);
}

void on_close(WebKitWebView *view, gpointer)
{
    int i = tab_index(view);
    if (i < 0)
        return;
    wisp_log("tab %d (id %d) closed%s", i, S.tabs[i].id, memory_note().c_str());
    ext::tab_removed(S.tabs[i].id);
    ext::detach_tab(S.tabs[i].id);   // (the extensions' layer kept the content manager freed below)
    // hidden first: activate() below only hides the tabs it still has, and a view that WebKit
    // thinks is on show draws on (its frames out of the shared pool) until it is gone
    platform_set_active(S.tabs[i].wpe, false);
    g_object_unref(S.tabs[i].view);
    if (S.tabs[i].ucm)
        g_object_unref(S.tabs[i].ucm);
    forget_tab(i);
}

gboolean on_decide_policy(WebKitWebView *view, WebKitPolicyDecision *decision, WebKitPolicyDecisionType type, gpointer)
{
    // a magnet link is the download manager's (aria2's BitTorrent), never a page
    if (type == WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION || type == WEBKIT_POLICY_DECISION_TYPE_NEW_WINDOW_ACTION) {
        WebKitNavigationAction *action = webkit_navigation_policy_decision_get_navigation_action(WEBKIT_NAVIGATION_POLICY_DECISION(decision));
        WebKitURIRequest *request = action ? webkit_navigation_action_get_request(action) : nullptr;
        const char *uri = request ? webkit_uri_request_get_uri(request) : nullptr;
        if (uri && g_ascii_strncasecmp(uri, "magnet:", 7) == 0) {
            webkit_policy_decision_ignore(decision);
            if (dm::running()) {
                dm::add(uri, {}, "");
                toast("Downloading a magnet link (OPTIONS > Downloads)");
            } else {
                toast("Magnet links need the download manager (it is off or did not start)");
            }
            return TRUE;
        }
    }
    if (type == WEBKIT_POLICY_DECISION_TYPE_RESPONSE) {
        auto *response = WEBKIT_RESPONSE_POLICY_DECISION(decision);
        if (!webkit_response_policy_decision_is_mime_type_supported(response)) {
            // a local file no page shows (a file:// address typed, or a link in a local page): a download
            // would only copy it into Downloads
            WebKitURIRequest *request = webkit_response_policy_decision_get_request(response);
            const char *uri = request ? webkit_uri_request_get_uri(request) : nullptr;
            if (uri && g_ascii_strncasecmp(uri, "file:", 5) == 0) {
                WebKitURIResponse *r = webkit_response_policy_decision_get_response(response);
                const char *mime = r ? webkit_uri_response_get_mime_type(r) : nullptr;
                wisp_log("files: %s: no page shows it (%s): not opened", uri, mime ? mime : "no type");
                webkit_policy_decision_ignore(decision);
                char *file = g_filename_from_uri(uri, nullptr, nullptr);
                std::string name = file ? file : uri;
                g_free(file);
                if (const size_t slash = name.rfind('/'); slash != std::string::npos)
                    name = name.substr(slash + 1);
                toast("OrbisWeb can't show " + name + " (it is no page, picture or text)");
                return TRUE;
            }
            webkit_policy_decision_download(decision);
            return TRUE;
        }
    }
    (void)view;
    return FALSE;
}

// --- The page's menu (a right click) -------------------------------------------------------------------
// WebKit proposes the items (what is under the pointer: a link, an image, a selection, a field); Wisp
// draws them. Returning TRUE from "context-menu" has WebKit connect each item's action to the page,
// then empty the menu: the items are kept (referenced) here and activated by their actions.

constexpr int CTX_W = 400, CTX_ROW = 50;

// the label an item shows; empty: not offered (spelling, fonts, the inspector, media controls)
std::string context_label(WebKitContextMenuItem *item)
{
    switch (webkit_context_menu_item_get_stock_action(item)) {
    case WEBKIT_CONTEXT_MENU_ACTION_OPEN_LINK: return "Open link";
    case WEBKIT_CONTEXT_MENU_ACTION_OPEN_LINK_IN_NEW_WINDOW: return "Open link in new tab";
    case WEBKIT_CONTEXT_MENU_ACTION_DOWNLOAD_LINK_TO_DISK: return "Download link";
    case WEBKIT_CONTEXT_MENU_ACTION_COPY_LINK_TO_CLIPBOARD: return "Copy link address";
    case WEBKIT_CONTEXT_MENU_ACTION_OPEN_IMAGE_IN_NEW_WINDOW: return "Open image in new tab";
    case WEBKIT_CONTEXT_MENU_ACTION_DOWNLOAD_IMAGE_TO_DISK: return "Save image";
    case WEBKIT_CONTEXT_MENU_ACTION_COPY_IMAGE_TO_CLIPBOARD: return "Copy image";
    case WEBKIT_CONTEXT_MENU_ACTION_OPEN_FRAME_IN_NEW_WINDOW: return "Open frame in new tab";
    case WEBKIT_CONTEXT_MENU_ACTION_GO_BACK: return "Back";
    case WEBKIT_CONTEXT_MENU_ACTION_GO_FORWARD: return "Forward";
    case WEBKIT_CONTEXT_MENU_ACTION_STOP: return "Stop";
    case WEBKIT_CONTEXT_MENU_ACTION_RELOAD: return "Reload";
    case WEBKIT_CONTEXT_MENU_ACTION_COPY: return "Copy";
    case WEBKIT_CONTEXT_MENU_ACTION_CUT: return "Cut";
    case WEBKIT_CONTEXT_MENU_ACTION_PASTE: return "Paste";
    case WEBKIT_CONTEXT_MENU_ACTION_NO_ACTION:
    case WEBKIT_CONTEXT_MENU_ACTION_SPELLING_GUESS:
    case WEBKIT_CONTEXT_MENU_ACTION_NO_GUESSES_FOUND:
    case WEBKIT_CONTEXT_MENU_ACTION_IGNORE_SPELLING:
    case WEBKIT_CONTEXT_MENU_ACTION_LEARN_SPELLING:
    case WEBKIT_CONTEXT_MENU_ACTION_IGNORE_GRAMMAR:
    case WEBKIT_CONTEXT_MENU_ACTION_FONT_MENU:
    case WEBKIT_CONTEXT_MENU_ACTION_BOLD:
    case WEBKIT_CONTEXT_MENU_ACTION_ITALIC:
    case WEBKIT_CONTEXT_MENU_ACTION_UNDERLINE:
    case WEBKIT_CONTEXT_MENU_ACTION_OUTLINE:
    case WEBKIT_CONTEXT_MENU_ACTION_INSPECT_ELEMENT:
    case WEBKIT_CONTEXT_MENU_ACTION_TOGGLE_MEDIA_CONTROLS:
    case WEBKIT_CONTEXT_MENU_ACTION_TOGGLE_MEDIA_LOOP:
    case WEBKIT_CONTEXT_MENU_ACTION_ENTER_VIDEO_FULLSCREEN:
        return {};
    default:
        break;
    }
    // anything else by WebKit's own title, without its mnemonic underscores
    const char *title = webkit_context_menu_item_get_title(item);
    std::string label;
    for (const char *p = title ? title : ""; *p; p++)
        if (*p != '_')
            label += *p;
    return label;
}

void context_close()
{
    for (WebKitContextMenuItem *item : S.ctx_items)
        g_object_unref(item);
    S.ctx_items.clear();
    S.ctx_labels.clear();
    S.ctx_enabled.clear();
    if (S.overlay == Overlay::Context)
        S.overlay = Overlay::None;
    S.dirty = true;
}

void context_activate(int i)
{
    if (i < 0 || i >= static_cast<int>(S.ctx_items.size()) || !S.ctx_enabled[i])
        return;
    WebKitContextMenuItem *item = WEBKIT_CONTEXT_MENU_ITEM(g_object_ref(S.ctx_items[i]));
    context_close();
    if (GAction *action = webkit_context_menu_item_get_gaction(item))
        g_action_activate(action, webkit_context_menu_item_get_gaction_target(item));
    g_object_unref(item);
}

int context_item_at(float x, float y)
{
    if (x < S.ctx_x || x >= S.ctx_x + CTX_W || y < S.ctx_y + 8)
        return -1;
    const int i = static_cast<int>(y - S.ctx_y - 8) / CTX_ROW;
    return i < static_cast<int>(S.ctx_items.size()) ? i : -1;
}

gboolean on_context_menu(WebKitWebView *, WebKitContextMenu *menu, WebKitHitTestResult *, gpointer)
{
    context_close();
    for (GList *l = webkit_context_menu_get_items(menu); l; l = l->next) {
        auto *item = WEBKIT_CONTEXT_MENU_ITEM(l->data);
        if (webkit_context_menu_item_is_separator(item) || webkit_context_menu_item_get_submenu(item))
            continue;
        const std::string label = context_label(item);
        if (label.empty() || std::find(S.ctx_labels.begin(), S.ctx_labels.end(), label) != S.ctx_labels.end())
            continue;
        GAction *action = webkit_context_menu_item_get_gaction(item);
        S.ctx_items.push_back(WEBKIT_CONTEXT_MENU_ITEM(g_object_ref(item)));
        S.ctx_labels.push_back(label);
        S.ctx_enabled.push_back(!action || g_action_get_enabled(action));
    }
    if (S.ctx_items.empty())
        return TRUE;
    const int h = static_cast<int>(S.ctx_items.size()) * CTX_ROW + 16;
    S.ctx_x = std::clamp(static_cast<int>(S.pointer_x) + 4, 8, W - CTX_W - 8);
    S.ctx_y = std::clamp(static_cast<int>(S.pointer_y) + 4, 8, std::max(8, H - h - 8));
    S.ctx_index = -1;
    S.overlay = Overlay::Context;
    S.dirty = true;
    return TRUE;
}

// A new tab costs the memory pool the app's processes share some 30 MiB until the tab it hides gives
// its frame buffers back (a second after): two 7.5 MiB buffers mapped in both the web process and the
// UI, a compositor thread, a GL context. With the pool nearly empty the app froze at the third tab
// (v00.13); now it says so instead. Settings: tab_memory_floor (MiB left needed; 0: no check).
bool tab_memory_ok()
{
    const int floor = wisp_setting_int("tab_memory_floor", 40);
    if (floor <= 0 || S.tabs.empty())
        return true;
    const size_t left = wisp_flexible_free() >> 20;
    if (left >= static_cast<size_t>(floor))
        return true;
    toast("Not enough memory for another tab (" + std::to_string(left) + " MiB left): close a tab first");
    return false;
}

// A WebKit without media (built with WISP_WEBKIT_MEDIA=0; every build before v00.17) has no
// HTMLMediaElement, HTMLVideoElement or HTMLAudioElement, and a page script that names one stops with a
// ReferenceError: YouTube's app script did, and its page stayed empty (v00.13). These stand-ins say
// what a browser that can play nothing says (canPlayType "", play() refused, no source), and
// document.createElement("video" / "audio") makes elements with them. With media (GStreamer, v00.17
// on) the classes are WebKit's own and this does nothing (its first line). Settings: media_stubs (0: off).
const char *const MEDIA_STUBS = R"JS((() => {
  'use strict';
  if (typeof window.HTMLMediaElement === 'function' || typeof window.HTMLElement !== 'function') return;
  const refuse = () => Promise.reject(new DOMException('This browser plays no audio or video', 'NotSupportedError'));
  class TimeRanges {
    get length() { return 0; }
    start() { throw new DOMException('No time ranges', 'IndexSizeError'); }
    end() { throw new DOMException('No time ranges', 'IndexSizeError'); }
  }
  class MediaError { constructor(code) { this.code = code; this.message = ''; } }
  Object.assign(MediaError, { MEDIA_ERR_ABORTED: 1, MEDIA_ERR_NETWORK: 2, MEDIA_ERR_DECODE: 3, MEDIA_ERR_SRC_NOT_SUPPORTED: 4 });
  class HTMLMediaElement extends HTMLElement {}
  Object.assign(HTMLMediaElement, { NETWORK_EMPTY: 0, NETWORK_IDLE: 1, NETWORK_LOADING: 2, NETWORK_NO_SOURCE: 3,
    HAVE_NOTHING: 0, HAVE_METADATA: 1, HAVE_CURRENT_DATA: 2, HAVE_FUTURE_DATA: 3, HAVE_ENOUGH_DATA: 4 });
  class HTMLVideoElement extends HTMLMediaElement {}
  class HTMLAudioElement extends HTMLMediaElement {}
  const define = (proto, name, get, set) => Object.defineProperty(proto, name, { configurable: true, enumerable: true, get, set });
  const state = new WeakMap();
  const settable = { currentTime: 0, volume: 1, muted: false, defaultMuted: false, playbackRate: 1,
    defaultPlaybackRate: 1, preservesPitch: true, preload: 'auto', crossOrigin: null, srcObject: null };
  const own = (el) => { let s = state.get(el); if (!s) { s = Object.assign({}, settable); state.set(el, s); } return s; };
  const m = HTMLMediaElement.prototype;
  for (const name of Object.keys(settable))
    define(m, name, function () { return own(this)[name]; }, function (v) { own(this)[name] = v; });
  const fixed = { paused: true, ended: false, seeking: false, duration: NaN, readyState: 0, networkState: 3,
    error: null, currentSrc: '' };
  for (const [name, value] of Object.entries(fixed))
    define(m, name, () => value);
  for (const name of ['buffered', 'played', 'seekable'])
    define(m, name, () => new TimeRanges());
  define(m, 'textTracks', () => Object.assign([], { getTrackById: () => null, addEventListener() {}, removeEventListener() {} }));
  const text = (proto, name, attribute) => define(proto, name, function () { return this.getAttribute(attribute) || ''; },
    function (v) { this.setAttribute(attribute, String(v)); });
  const flag = (proto, name, attribute) => define(proto, name, function () { return this.hasAttribute(attribute); },
    function (v) { if (v) this.setAttribute(attribute, ''); else this.removeAttribute(attribute); });
  text(m, 'src', 'src');
  for (const name of ['autoplay', 'loop', 'controls']) flag(m, name, name);
  m.canPlayType = () => '';
  m.load = () => {};
  m.play = refuse;
  m.pause = () => {};
  m.fastSeek = () => {};
  m.setSinkId = refuse;
  m.addTextTrack = () => ({ cues: [], activeCues: [], mode: 'disabled', addCue() {}, removeCue() {}, addEventListener() {}, removeEventListener() {} });
  const v = HTMLVideoElement.prototype;
  define(v, 'videoWidth', () => 0);
  define(v, 'videoHeight', () => 0);
  text(v, 'poster', 'poster');
  flag(v, 'playsInline', 'playsinline');
  for (const name of ['width', 'height'])
    define(v, name, function () { return parseInt(this.getAttribute(name), 10) || 0; }, function (n) { this.setAttribute(name, String(n)); });
  v.requestPictureInPicture = refuse;
  v.getVideoPlaybackQuality = () => ({ creationTime: performance.now(), totalVideoFrames: 0, droppedVideoFrames: 0, corruptedVideoFrames: 0 });
  v.requestVideoFrameCallback = () => 0;
  v.cancelVideoFrameCallback = () => {};
  const create = Document.prototype.createElement;
  Document.prototype.createElement = function createElement(name, options) {
    const element = create.call(this, name, options);
    const kind = String(name).toLowerCase();
    if (kind === 'video') Object.setPrototypeOf(element, v);
    else if (kind === 'audio') Object.setPrototypeOf(element, HTMLAudioElement.prototype);
    return element;
  };
  function Audio(src) {
    const element = document.createElement('audio');
    element.preload = 'auto';
    if (src !== undefined) element.src = src;
    return element;
  }
  Audio.prototype = HTMLAudioElement.prototype;
  for (const [name, value] of Object.entries({ HTMLMediaElement, HTMLVideoElement, HTMLAudioElement, MediaError, TimeRanges, Audio }))
    Object.defineProperty(window, name, { configurable: true, writable: true, value });
})();)JS";

// EmulatorJS (an N64 game page, v00.34) builds its settings menu at game start with
// MediaRecorder.isTypeSupported() and never checks that MediaRecorder exists. This WebKit has none (its
// GStreamer has no encoders), so the game stopped right there: "Failed to start game ReferenceError: Can't
// find variable: MediaRecorder". This one records nothing: no type is supported and the constructor refuses,
// which is also what a page that checks first is told. Settings: media_stubs (0: off).
const char *const MEDIA_RECORDER_STUB = R"JS((() => {
  'use strict';
  if (typeof window.MediaRecorder === 'function' || typeof window.EventTarget !== 'function') return;
  class MediaRecorder extends EventTarget {
    constructor() {
      super();
      throw new DOMException('This browser records no media', 'NotSupportedError');
    }
    static isTypeSupported() { return false; }
  }
  Object.defineProperty(window, 'MediaRecorder', { configurable: true, writable: true, value: MediaRecorder });
})();)JS";

// YouTube's ads with the ad blocker on (v00.34): an ad ends as it starts. EasyList blocks the requests
// YouTube's player makes while an ad plays (its stats and pings), not the ad's video, and the player then
// showed the ad with no Skip button ("i can't skip videos, the button is not appearing"). While the player
// says an ad is showing, its video is taken to its end, and a Skip button is pressed when one shows;
// the page's own video is left alone (an ad also needs its overlay, or a length under three minutes).
const char *const YOUTUBE_ADS = R"JS((() => {
  'use strict';
  if (window.__orbisAdSkip) return;
  window.__orbisAdSkip = true;
  const SKIP = '.ytp-skip-ad-button, .ytp-ad-skip-button, .ytp-ad-skip-button-modern, .ytp-ad-skip-button-slot button, '
    + '.ytp-ad-skip-button-container button, .ytp-ad-overlay-close-button';
  const AD_UI = '.ytp-ad-player-overlay, .ytp-ad-player-overlay-layout, .ytp-ad-preview-container, .ytp-ad-simple-ad-badge, '
    + '.ytp-ad-badge, .ytp-ad-duration-remaining, .video-ads .ytp-ad-module > *';
  const check = () => {
    for (const button of document.querySelectorAll(SKIP)) {
      if (button.offsetParent !== null) {
        try { button.click(); } catch (e) { }
      }
    }
    const player = document.querySelector('.html5-video-player.ad-showing, .html5-video-player.ad-interrupting');
    if (!player) return;
    const video = player.querySelector('video');
    if (!video) return;
    const length = video.duration;
    if (!isFinite(length) || length <= 0 || video.currentTime >= length - 0.25) return;
    if (!document.querySelector(AD_UI) && length > 180) return;
    try { video.currentTime = length; } catch (e) { }
  };
  setInterval(check, 300);
})();)JS";

// YouTube's player asks MediaSource.isTypeSupported which streams it can play (codec, size, frame rate
// in the type) and picks the best one: on the PS4's CPU the cheapest to decode is H.264, and 1080p or
// 60 frames a second are more than its 4 decoding threads keep up with. This says no to VP9, AV1 and
// streams over video_max_height (720) or 30 frames a second, as the h264ify extensions do on slow
// desktops (settings: video_h264_only=0 / video_max_height=0 lift them). Only on YouTube, and only when
// this WebKit plays video at all.
const char *const YOUTUBE_LIMITS = R"JS((() => {
  'use strict';
  if (typeof MediaSource === 'undefined' || typeof MediaSource.isTypeSupported !== 'function') return;
  const H264_ONLY = __H264_ONLY__, MAX_HEIGHT = __MAX_HEIGHT__;
  const refuse = (type) => {
    const t = String(type);
    if (H264_ONLY && /vp0?9|vp8|av01|hev1|hvc1/i.test(t)) return true;
    const height = /height=(\d+)/i.exec(t), framerate = /framerate=(\d+)/i.exec(t);
    if (MAX_HEIGHT && height && Number(height[1]) > MAX_HEIGHT) return true;
    if (MAX_HEIGHT && framerate && Number(framerate[1]) > 30) return true;
    return false;
  };
  const isTypeSupported = MediaSource.isTypeSupported.bind(MediaSource);
  MediaSource.isTypeSupported = (type) => !refuse(type) && isTypeSupported(type);
  const canPlayType = HTMLMediaElement.prototype.canPlayType;
  HTMLMediaElement.prototype.canPlayType = function (type) { return refuse(type) ? '' : canPlayType.call(this, type); };
  if (navigator.mediaCapabilities && navigator.mediaCapabilities.decodingInfo) {
    const decodingInfo = navigator.mediaCapabilities.decodingInfo.bind(navigator.mediaCapabilities);
    navigator.mediaCapabilities.decodingInfo = (config) => {
      const v = config && config.video;
      if (v && (refuse(v.contentType) || (MAX_HEIGHT && (v.height > MAX_HEIGHT || v.framerate > 30))))
        return Promise.resolve({ supported: false, smooth: false, powerEfficient: false });
      return decodingInfo(config);
    };
  }
})();)JS";

// (v00.43) What a page's own scripts see of the keyboard and of the focus, as page messages in web.log
// ("OrbisWeb keys: ..."; Page messages in the log must be on). Gold Miner (Ruffle 0.6 on a Flash game page)
// took no key from a USB keyboard in v00.42 although the keys reached WebKit: Ruffle takes keys only
// after a focusin on its player element. Each key down/up says where it went, what had the focus
// (through shadow roots), whether the page had it, and whether a script prevented its default; focus
// moves and pointer presses too; 300 lines a page at most. Settings: key_log (0: off).
const char *const KEY_LOG = R"JS((() => {
  'use strict';
  if (window.__orbisKeyLog) return;
  window.__orbisKeyLog = true;
  let lines = 0;
  const where = (self === top) ? '' : ' (frame ' + location.host + ')';
  const say = (text) => { if (lines++ < 300) console.log('OrbisWeb keys' + where + ': ' + text); };
  const name = (node) => {
    if (!node) return 'none';
    if (node === window) return 'window';
    if (node === document) return 'document';
    if (!node.tagName) return String(node.nodeName || node);
    let n = node.tagName.toLowerCase();
    if (node.id) n += '#' + node.id;
    else if (typeof node.className === 'string' && node.className.trim()) n += '.' + node.className.trim().split(/\s+/)[0];
    return n;
  };
  const first = (e) => name(e.composedPath ? e.composedPath()[0] : e.target);
  const focused = () => {
    let element = document.activeElement, path = name(element);
    while (element && element.shadowRoot && element.shadowRoot.activeElement) {
      element = element.shadowRoot.activeElement;
      path += ' > ' + name(element);
    }
    return path;
  };
  const key = (e) => {
    const at = first(e);
    // (after the whole dispatch: whether any script prevented the key's default)
    setTimeout(() => say(e.type + ' code ' + (e.code || '-') + ' key ' + e.key + ' keyCode ' + e.keyCode + (e.repeat ? ' (repeat)' : '')
      + ' at ' + at + '; focus ' + focused() + '; page focused ' + document.hasFocus() + '; default prevented ' + e.defaultPrevented), 0);
  };
  addEventListener('keydown', key, true);
  addEventListener('keyup', key, true);
  addEventListener('focusin', (e) => say('focusin ' + first(e) + ' (from ' + name(e.relatedTarget) + ')'), true);
  addEventListener('focusout', (e) => say('focusout ' + first(e) + ' (to ' + name(e.relatedTarget) + ')'), true);
  addEventListener('pointerdown', (e) => say('pointerdown ' + e.pointerType + ' button ' + e.button + ' at ' + first(e) + '; focus ' + focused()), true);
})();)JS";

// (v00.43) Flash games in Ruffle took no keys (Gold Miner on a Flash game page, v00.41 and v00.42). Ruffle 0.6
// handles a key only while its player "has focus", which it learns from a focusin on the player element
// (it listens from when its WebAssembly instance is ready). Its shadow tree holds a hidden text field
// (#virtual-keyboard) that can take the focus from the start, and pages call player.focus() early (that
// site 400 ms after making the player): on the PS4 Ruffle is not ready by then, so the focus went into
// the player unheard, and every click after it was a re-focus of an already-focused player (no focusin;
// the HTML focusing steps return there), so Ruffle never knew. On a player's first press this takes the
// focus out of it if it is already in, and Ruffle's own press handler (on the player, after this one in
// the capture phase) focuses it again, now with a focusin. Settings: ruffle_focus (0: off).
const char *const RUFFLE_FOCUS = R"JS((() => {
  'use strict';
  if (window.__orbisRuffleFocus) return;
  window.__orbisRuffleFocus = true;
  const seen = new WeakSet();
  const RUFFLE = /^RUFFLE-(PLAYER|EMBED|OBJECT)$/;
  addEventListener('pointerdown', (e) => {
    const path = e.composedPath ? e.composedPath() : [];
    const player = path.find((node) => node && node.tagName && RUFFLE.test(node.tagName));
    if (!player || seen.has(player)) return;
    seen.add(player);
    const root = player.getRootNode ? player.getRootNode() : document;
    if (root.activeElement === player) {
      try { player.blur(); } catch (err) { }
    }
  }, true);
})();)JS";

// (v00.44) navigator.deviceMemory, as Chrome gives it (gigabytes, a power of two): WebKit has none. A page that
// sizes itself by it picks its light mode on a console that leaves one page about 2 GB of the PS4's memory:
// Midnight Club 3's low-memory profile (1 GB of WebAssembly memory instead of its WebKit 2 GB, 8 game threads instead
// of 40, lighter defaults) is the one it runs on phones and TVs, where it "stayed black" otherwise; v00.43 ran it on
// the 2 GB profile and the web process ran out of memory twice. Settings: device_memory (GB; 0 leaves it out).
const char *const DEVICE_MEMORY = R"JS((() => {
  'use strict';
  if (typeof Navigator === 'undefined' || 'deviceMemory' in Navigator.prototype) return;
  const value = __DEVICE_MEMORY__;
  try {
    Object.defineProperty(Navigator.prototype, 'deviceMemory', {
      configurable: true, enumerable: true,
      get: function deviceMemory() { return value; },
    });
  } catch (e) { }
})();)JS";

// A tab's content manager gets the browser's own scripts (ext::Browser::page_scripts: the extensions'
// layer adds them back each time it sets a tab's scripts again). Not in sites' human checks: their
// fingerprinting tells a script's class from a built-in one, and takes the page for a doctored browser.
void add_page_scripts(WebKitUserContentManager *ucm)
{
    static const char *const human_checks[] = { "https://challenges.cloudflare.com/*", "https://www.google.com/recaptcha/*",
        "https://www.recaptcha.net/*", "https://*.hcaptcha.com/*", nullptr };
    // (v00.43) keys and focus as pages see them (settings: key_log=0 leaves it out)
    if (wisp_setting_int("key_log", 1)) {
        WebKitUserScript *keys = webkit_user_script_new(KEY_LOG, WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES,
            WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START, nullptr, human_checks);
        webkit_user_content_manager_add_script(ucm, keys);
        webkit_user_script_unref(keys);
    }
    // (v00.43) Ruffle's players learn they have the focus (settings: ruffle_focus=0 leaves it out)
    if (wisp_setting_int("ruffle_focus", 1)) {
        WebKitUserScript *ruffle = webkit_user_script_new(RUFFLE_FOCUS, WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES,
            WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START, nullptr, human_checks);
        webkit_user_content_manager_add_script(ucm, ruffle);
        webkit_user_script_unref(ruffle);
    }
    // (v00.44) navigator.deviceMemory (settings: device_memory, GB; 0 leaves it out)
    if (const int gigabytes = wisp_setting_int("device_memory", 2); gigabytes > 0) {
        std::string source = DEVICE_MEMORY;
        static const char marker[] = "__DEVICE_MEMORY__";
        const size_t at = source.find(marker);
        if (at != std::string::npos)
            source.replace(at, sizeof marker - 1, std::to_string(gigabytes));
        WebKitUserScript *memory = webkit_user_script_new(source.c_str(), WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES,
            WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START, nullptr, human_checks);
        webkit_user_content_manager_add_script(ucm, memory);
        webkit_user_script_unref(memory);
    }
    if (wisp_setting_int("video_h264_only", 1) || wisp_setting_int("video_max_height", 720) > 0) {
        std::string source = YOUTUBE_LIMITS;
        const auto put = [&](const char *name, const std::string &value) {
            const size_t at = source.find(name);
            if (at != std::string::npos)
                source.replace(at, strlen(name), value);
        };
        put("__H264_ONLY__", wisp_setting_int("video_h264_only", 1) ? "true" : "false");
        put("__MAX_HEIGHT__", std::to_string(std::max(0, wisp_setting_int("video_max_height", 720))));
        static const char *const youtube[] = { "https://*.youtube.com/*", "https://youtube.com/*", "https://*.youtube-nocookie.com/*", nullptr };
        WebKitUserScript *limits = webkit_user_script_new(source.c_str(), WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES,
            WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START, youtube, nullptr);
        webkit_user_content_manager_add_script(ucm, limits);
        webkit_user_script_unref(limits);
    }
    // YouTube's ads end as they start while the ad blocker is on (v00.34; settings: youtube_ad_skip=0 keeps
    // them, Skip buttons and all)
    if (ext::adblock() && wisp_setting_int("youtube_ad_skip", 1)) {
        static const char *const youtube[] = { "https://*.youtube.com/*", "https://youtube.com/*", nullptr };
        WebKitUserScript *ads = webkit_user_script_new(YOUTUBE_ADS, WEBKIT_USER_CONTENT_INJECT_TOP_FRAME,
            WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_END, youtube, nullptr);
        webkit_user_content_manager_add_script(ucm, ads);
        webkit_user_script_unref(ads);
    }
    if (!wisp_setting_int("media_stubs", 1))
        return;
    WebKitUserScript *stubs = webkit_user_script_new(MEDIA_STUBS, WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES,
        WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START, nullptr, human_checks);
    webkit_user_content_manager_add_script(ucm, stubs);
    webkit_user_script_unref(stubs);
    // (v00.35) a MediaRecorder that records nothing (EmulatorJS's games start with one)
    WebKitUserScript *recorder = webkit_user_script_new(MEDIA_RECORDER_STUB, WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES,
        WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START, nullptr, human_checks);
    webkit_user_content_manager_add_script(ucm, recorder);
    webkit_user_script_unref(recorder);
}

// A tab's web view (a new tab, or one woken from sleep: the same content manager again)
// Autoplay with sound (settings: autoplay=allow|muted|deny). WPE's own default lets a page start media
// only without sound, and YouTube's player starts its video from a script after the click, not from the
// click itself: it played muted (v00.22: "YouTube has no audio"). A console browser shows one page at a
// time, as the PS4's own does.
WebKitWebsitePolicies *website_policies()
{
    static WebKitWebsitePolicies *policies = [] {
        const std::string mode = wisp_setting("autoplay", "allow");
        const WebKitAutoplayPolicy policy = mode == "deny" ? WEBKIT_AUTOPLAY_DENY
            : mode == "muted" ? WEBKIT_AUTOPLAY_ALLOW_WITHOUT_SOUND : WEBKIT_AUTOPLAY_ALLOW;
        wisp_log("webkit: media autoplay %s (settings: autoplay)", mode == "deny" ? "off" : mode == "muted" ? "without sound" : "with sound");
        return webkit_website_policies_new_with_policies("autoplay", policy, nullptr);
    }();
    return policies;
}

void make_view(Tab &tab, WebKitWebView *related)
{
    WebKitWebView *view;
    if (related) {
        view = WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW, "related-view", related, "display", platform_display(),
            "settings", S.settings, "user-content-manager", tab.ucm, "website-policies", website_policies(), nullptr));
    } else {
        view = WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW, "display", platform_display(), "web-context", S.context,
            "network-session", S.session, "settings", S.settings, "user-content-manager", tab.ucm,
            "website-policies", website_policies(), nullptr));
    }
    // (the tab holds the one reference: WPE's WebKitWebView is a plain GObject, not a floating
    // GInitiallyUnowned as GTK's widget is, so a g_object_ref_sink here added a second one. Up to
    // v00.25 every tab's view kept that reference: a closed or sleeping tab's page never left the
    // web process, still drawing, running its scripts and holding its media's audio ports)
    tab.view = view;
    if (wisp_setting_int("memory_log", 1))
        g_object_weak_ref(G_OBJECT(view), [](gpointer id, GObject *) {
            wisp_log("tab id %d: its view is freed (WebKit closes its page in the web process)%s", GPOINTER_TO_INT(id),
                memory_note().c_str());
        }, GINT_TO_POINTER(tab.id));
    tab.wpe = webkit_web_view_get_wpe_view(view);
    g_signal_connect(view, "notify::title", G_CALLBACK(on_title), nullptr);
    g_signal_connect(view, "notify::uri", G_CALLBACK(on_uri), nullptr);
    g_signal_connect(view, "notify::estimated-load-progress", G_CALLBACK(on_progress), nullptr);
    g_signal_connect(view, "notify::is-loading", G_CALLBACK(on_progress), nullptr);
    g_signal_connect(view, "load-changed", G_CALLBACK(on_load_changed), nullptr);
    g_signal_connect(view, "load-failed", G_CALLBACK(on_load_failed), nullptr);
    g_signal_connect(view, "load-failed-with-tls-errors", G_CALLBACK(on_tls_error), nullptr);
    g_signal_connect(view, "web-process-terminated", G_CALLBACK(on_crashed), nullptr);
    g_signal_connect(view, "notify::is-web-process-responsive", G_CALLBACK(on_responsive), nullptr);
    g_signal_connect(view, "mouse-target-changed", G_CALLBACK(on_hover), nullptr);
    g_signal_connect(view, "create", G_CALLBACK(on_create), nullptr);
    g_signal_connect(view, "close", G_CALLBACK(on_close), nullptr);
    g_signal_connect(view, "decide-policy", G_CALLBACK(on_decide_policy), nullptr);
    g_signal_connect(view, "context-menu", G_CALLBACK(on_context_menu), nullptr);
    g_signal_connect(view, "enter-fullscreen", G_CALLBACK(on_enter_fullscreen), nullptr);
    g_signal_connect(view, "leave-fullscreen", G_CALLBACK(on_leave_fullscreen), nullptr);
    platform_set_content_size(tab.wpe, W, content_h());
}

WebKitWebView *new_tab(const std::string &uri, WebKitWebView *related, bool focus)
{
    if (!tab_memory_ok())
        return nullptr;
    // each tab has its own content manager: extensions' content scripts then know their tab (the
    // browser's own scripts go in with theirs: page_scripts)
    Tab tab;
    tab.ucm = webkit_user_content_manager_new();
    tab.id = S.next_tab_id++;
    tab.title = "New tab";
    ext::attach_tab(tab.id, tab.ucm);
    make_view(tab, related);
    const int id = tab.id;
    WebKitWebView *view = tab.view;
    S.tabs.push_back(tab);
    const int index = static_cast<int>(S.tabs.size()) - 1;
    if (focus || S.active < 0)
        activate(index);
    else {
        platform_set_active(S.tabs[index].wpe, false);
        S.tabs[index].hidden_at = wisp_seconds();
    }
    if (!uri.empty())
        webkit_web_view_load_uri(view, uri.c_str());
    ext::tab_created(id);
    wisp_log("tab %d (id %d) opened%s%s%s", index, id, uri.empty() ? "" : ": ", uri.c_str(), memory_note().c_str());
    return view;
}

// --- Sleeping tabs -----------------------------------------------------------------------------------
// Every tab's page lives in the one web process: its DOM, its scripts' objects (which each garbage
// collection walks, whatever tab they belong to) and its timers. A tab hidden for tab_sleep_after
// seconds (600; 0: never for that), or the longest-hidden one while the shared pool has less than
// tab_sleep_floor MiB left (40; 0: never for that) or the web process's heap holds more than
// tab_sleep_heap MiB (1200), sleeps; never for memory the page shown last while Files, Downloads or the
// player is in front (they use no page: v00.22 put YouTube to sleep a second after Downloads came up,
// with the pool at its usual 54 MiB under the old 64, and it loaded again each time it came back): WebKit's session state (its history with scroll positions and
// form fields) and its last picture go to /data/Wisp/run/tabs, and its web view goes (its page closes
// in the web process). Shown again, it wakes: a new view gets the session state back and goes to the
// page it was on (from the network cache where it can), with the saved picture on screen at once.

const char *const SLEEP_DIR = WISP_DATA "/run/tabs";

void picture_writer(std::shared_ptr<Picture> picture)
{
    std::vector<uint8_t> pixels;
    int header[3];
    {
        std::lock_guard<std::mutex> guard(picture->lock);
        pixels = picture->pixels;   // (a copy: wake() may take the original meanwhile)
        header[0] = picture->width, header[1] = picture->height, header[2] = picture->stride;
    }
    bool written = false;
    if (FILE *f = fopen(picture->path.c_str(), "wb")) {
        written = fwrite(header, sizeof header, 1, f) == 1 && fwrite(pixels.data(), 1, pixels.size(), f) == pixels.size();
        written = fclose(f) == 0 && written;
    }
    std::lock_guard<std::mutex> guard(picture->lock);
    if (written) {
        picture->on_disk = true;
        std::vector<uint8_t>().swap(picture->pixels);
    }
}

void put_to_sleep(int index, const char *why)
{
    Tab &tab = S.tabs[index];
    if (!tab.view || tab.files || tab.asleep || index == S.active)
        return;
    mkdir(WISP_DATA "/run", 0777);
    mkdir(SLEEP_DIR, 0777);
    WebKitWebViewSessionState *state = webkit_web_view_get_session_state(tab.view);
    tab.session = webkit_web_view_session_state_serialize(state);
    webkit_web_view_session_state_unref(state);
    const std::string base = std::string(SLEEP_DIR) + "/tab-" + std::to_string(tab.id);
    gsize session_size = 0;
    const void *session_data = g_bytes_get_data(tab.session, &session_size);
    if (FILE *f = fopen((base + ".session").c_str(), "wb")) {
        fwrite(session_data, 1, session_size, f);
        fclose(f);
    }
    Frame frame;
    if (platform_frame(tab.wpe, frame)) {
        auto picture = std::make_shared<Picture>();
        picture->pixels.assign(frame.pixels, frame.pixels + static_cast<size_t>(frame.stride) * frame.height);
        picture->width = frame.width, picture->height = frame.height, picture->stride = frame.stride;
        picture->path = base + ".picture";
        tab.picture = picture;
        std::thread(picture_writer, picture).detach();
    }
    ext::tab_removed(tab.id);   // (to extensions a sleeping tab is a discarded one: gone until it wakes)
    g_object_unref(tab.view);   // WebKit closes the page in the web process
    tab.view = nullptr;
    tab.wpe = nullptr;
    tab.asleep = true;
    tab.loading = false;
    wisp_log("tab %d (id %d) asleep (%s, hidden %.0f s): %s; %zu bytes of session state%s", index, tab.id, why,
        wisp_seconds() - tab.hidden_at, tab.uri.c_str(), static_cast<size_t>(session_size), memory_note().c_str());
    S.toolbar_dirty = S.dirty = true;
}

void wake(Tab &tab)
{
    if (!tab.asleep)
        return;
    const double t0 = wisp_seconds();
    make_view(tab, nullptr);
    tab.asleep = false;
    // the picture it had, until its page draws again
    if (auto picture = tab.picture) {
        std::vector<uint8_t> pixels;
        {
            std::lock_guard<std::mutex> guard(picture->lock);
            if (!picture->pixels.empty())
                pixels = std::move(picture->pixels);
        }
        if (pixels.empty() && picture->on_disk) {
            if (FILE *f = fopen(picture->path.c_str(), "rb")) {
                int header[3] = {};
                if (fread(header, sizeof header, 1, f) == 1 && header[1] > 0 && header[2] > 0 && header[1] <= 4096) {
                    pixels.resize(static_cast<size_t>(header[2]) * header[1]);
                    if (fread(pixels.data(), 1, pixels.size(), f) != pixels.size())
                        pixels.clear();
                }
                fclose(f);
            }
        }
        if (!pixels.empty())
            platform_show_picture(tab.wpe, std::move(pixels), picture->width, picture->height, picture->stride);
        tab.picture.reset();
    }
    bool restored = false;
    if (tab.session) {
        if (WebKitWebViewSessionState *state = webkit_web_view_session_state_new(tab.session)) {
            webkit_web_view_restore_session_state(tab.view, state);
            webkit_web_view_session_state_unref(state);
            WebKitBackForwardList *list = webkit_web_view_get_back_forward_list(tab.view);
            if (WebKitBackForwardListItem *item = list ? webkit_back_forward_list_get_current_item(list) : nullptr) {
                webkit_web_view_go_to_back_forward_list_item(tab.view, item);
                restored = true;
            }
        }
        g_bytes_unref(tab.session);
        tab.session = nullptr;
    }
    if (!restored && !tab.uri.empty())
        webkit_web_view_load_uri(tab.view, tab.uri.c_str());
    ext::tab_created(tab.id);
    wisp_log("tab id %d awake (%.0f ms): %s%s", tab.id, (wisp_seconds() - t0) * 1000, tab.uri.c_str(),
        restored ? " (its history back)" : "");
}

// once a second, from frame(): the tab that should sleep now, if one should
void sleep_check()
{
    static double last = 0;
    const double now = wisp_seconds();
    if (now - last < 1)
        return;
    last = now;
    const int after = wisp_setting_int("tab_sleep_after", 600);
    const int floor = wisp_setting_int("tab_sleep_floor", 40);
    const int heap_limit = wisp_setting_int("tab_sleep_heap", 1200);
    const size_t pool = wisp_flexible_free() >> 20;
    const int heap = wisp_child_heap_mib(WispChild::Web);
    const bool short_of_memory = (floor > 0 && pool < static_cast<size_t>(floor)) || (heap_limit > 0 && heap > heap_limit);
    static double last_pressed = 0;   // (under pressure one tab every 3 s: its memory takes a moment to go)
    // in front a tab with no page (Files, Downloads, the player): the page shown last stays for memory
    const bool page_in_front = active_tab() != nullptr;
    int oldest = -1, newest = -1;
    for (int i = 0; i < static_cast<int>(S.tabs.size()); i++) {
        const Tab &tab = S.tabs[i];
        if (i == S.active || tab.files || tab.asleep || !tab.view || tab.crashed || tab.hidden_at <= 0)
            continue;
        if (tab.uri.empty() || tab.uri.rfind("about:", 0) == 0)
            continue;
        if (after > 0 && now - tab.hidden_at >= after && !tab.loading) {
            put_to_sleep(i, "hidden long enough");
            return;
        }
        if (oldest < 0 || tab.hidden_at < S.tabs[oldest].hidden_at)
            oldest = i;
        if (newest < 0 || tab.hidden_at > S.tabs[newest].hidden_at)
            newest = i;
    }
    if (!page_in_front && oldest == newest)
        oldest = -1;   // (the longest-hidden page is the one shown last: it stays)
    if (short_of_memory && oldest >= 0 && now - last_pressed >= 3) {
        last_pressed = now;
        char why[96];
        snprintf(why, sizeof why, "memory short: %zu MiB of the pool left, web heap %d MiB", pool, heap);
        put_to_sleep(oldest, why);
    }
}

// --- The controller as a page's gamepad (v00.30) ----------------------------------------------------
// A page that reads gamepads (the Gamepad API: web games, emulators, cloud gaming) gets the controller
// as a PC browser gives it a DualShock 4 (ui/platform.cpp): the standard mapping, analog triggers,
// vibration. While the page has it (game mode) the controller drives nothing of the browser's: no
// pointer (a mouse still moves it), no Circle for Back, no OPTIONS menu, no L1 / R1 tabs. Holding the
// touch pad's button for a second, or L1 + L2 + R1 + R2 together for half a second (v00.34: no game
// presses all four at once), takes it back, and gives it to the page again. A page that starts reading
// gamepads gets it at once, unless the user took it back from that site before (settings:
// gamepad_declined); one that stops (closed, gone to another page) gives it back.

constexpr double TOUCHPAD_HOLD_S = 1.0;
constexpr double SHOULDERS_HOLD_S = 0.5;
constexpr const char *GIVE_BACK = "hold L1 + L2 + R1 + R2, or the touch pad's button";

std::string page_host(const std::string &uri)
{
    std::string host;
    if (GUri *parsed = g_uri_parse(uri.c_str(), G_URI_FLAGS_NONE, nullptr)) {
        if (g_uri_get_host(parsed))
            host = g_uri_get_host(parsed);
        g_uri_unref(parsed);
    }
    return host;
}

void load_gamepad_declined()
{
    S.gamepad_declined.clear();
    std::string list = wisp_setting("gamepad_declined", "");
    size_t start = 0;
    while (start < list.size()) {
        const size_t space = list.find(' ', start);
        const std::string host = list.substr(start, space == std::string::npos ? std::string::npos : space - start);
        if (!host.empty())
            S.gamepad_declined.push_back(host);
        if (space == std::string::npos)
            break;
        start = space + 1;
    }
}

bool gamepad_declined(const std::string &host)
{
    return !host.empty() && std::find(S.gamepad_declined.begin(), S.gamepad_declined.end(), host) != S.gamepad_declined.end();
}

void set_gamepad_declined(const std::string &host, bool declined)
{
    if (host.empty() || gamepad_declined(host) == declined)
        return;
    if (declined)
        S.gamepad_declined.push_back(host);
    else
        S.gamepad_declined.erase(std::find(S.gamepad_declined.begin(), S.gamepad_declined.end(), host));
    if (S.gamepad_declined.size() > 64)
        S.gamepad_declined.erase(S.gamepad_declined.begin());
    std::string list;
    for (const std::string &h : S.gamepad_declined)
        list += (list.empty() ? "" : " ") + h;
    wisp_setting_set("gamepad_declined", list.c_str());
}

void stop_rumble()
{
    if (S.rumble_stop) {
        g_source_remove(S.rumble_stop);
        S.rumble_stop = 0;
    }
    shell::input_pad_vibrate(0, 0);
}

void set_game_mode(bool on, const char *why)
{
    if (S.game_mode == on)
        return;
    S.game_mode = on;
    shell::input_pad_to_ui(!on);
    if (!on) {
        stop_rumble();
        if (S.keys_mode)
            release_pad_keys();
        S.keys_mode = false;
    }
    wisp_log("gamepad: %s (%s)", on ? "the page has the controller" : "the controller is the browser's again", why);
    if (on) {
        Tab *tab = active_tab();
        platform_gamepad_check(tab ? tab->wpe : nullptr);
    }
    S.dirty = S.toolbar_dirty = true;
}

// The touch pad's button held a second, or the four shoulder buttons half a second: the controller to the
// page, or back to the browser
void switch_game_mode_by_hand(const char *how)
{
    Tab *tab = active_tab();
    if (S.game_mode) {
        set_game_mode(false, how);
        if (tab)
            set_gamepad_declined(page_host(tab->uri), true);
        toast(std::string("The controller moves the pointer again (") + GIVE_BACK + " to give it back to the page)");
        return;
    }
    if (!tab) {
        toast("Only a web page can have the controller");
        return;
    }
    if (!platform_gamepads_in_use()) {
        // (v00.42) a page that reads no gamepads (Flash games in Ruffle, canvas games on the arrow keys): the
        // controller as a keyboard (keys_frame)
        set_game_mode(true, how);
        S.keys_mode = true;
        S.keys_held = 0;
        wisp_log("gamepad: the controller is a keyboard for the page (it reads no gamepads)");
        toast(std::string("The controller is a keyboard here: D-pad or left stick arrows, Cross Space, Circle Enter, Square Z, "
            "Triangle X, L2 Shift, R2 Ctrl, OPTIONS Esc (") + GIVE_BACK + " for the pointer)");
        return;
    }
    set_gamepad_declined(page_host(tab->uri), false);
    set_game_mode(true, how);
    toast(std::string("The page has the controller (") + GIVE_BACK + " for the pointer)");
}

// WebKit: a page started reading gamepads, or none reads them any more
void on_gamepads_in_use(bool in_use)
{
    if (!in_use) {
        if (S.game_mode && !S.keys_mode) {   // (the keyboard mode is for pages that read none)
            set_game_mode(false, "no page reads gamepads");
            toast("The controller moves the pointer again");
        }
        return;
    }
    Tab *tab = active_tab();
    if (!tab || S.game_mode)
        return;   // (Files, Downloads or the player on show: a page in the background reads them)
    if (gamepad_declined(page_host(tab->uri))) {
        toast(std::string("This page can take the controller: ") + GIVE_BACK);
        return;
    }
    set_game_mode(true, "a page reads gamepads");
    toast(std::string("The page has the controller (") + GIVE_BACK + " for the pointer)");
}

// A page's vibration (the Gamepad API's dual-rumble): only while it has the controller
void on_gamepad_rumble(double strong, double weak, unsigned ms)
{
    stop_rumble();
    if (!S.game_mode || (strong <= 0 && weak <= 0))
        return;
    shell::input_pad_vibrate(static_cast<uint8_t>(std::clamp(strong, 0.0, 1.0) * 255), static_cast<uint8_t>(std::clamp(weak, 0.0, 1.0) * 255));
    S.rumble_stop = g_timeout_add(std::clamp(ms ? ms : 5000u, 1u, 5000u), [](gpointer) -> gboolean {
        S.rumble_stop = 0;
        shell::input_pad_vibrate(0, 0);
        return G_SOURCE_REMOVE;
    }, nullptr);
}

// Each frame: the touch pad's hold, the four shoulders' hold, and the controller's state to the page's
// gamepad (a neutral one while the browser has the controller: the page sees nothing pressed)
void gamepad_frame()
{
    const shell::PadState &pad = shell::input_pad();
    const bool touch = pad.connected && (pad.buttons & shell::PAD_TOUCH_PAD);
    const double t = wisp_seconds();
    if (!touch) {
        S.touchpad_down_at = 0;
        S.touchpad_switched = false;
    } else if (!S.touchpad_down_at) {
        S.touchpad_down_at = t;
    } else if (!S.touchpad_switched && t - S.touchpad_down_at >= TOUCHPAD_HOLD_S && !shell::keyboard_active()) {
        S.touchpad_switched = true;
        switch_game_mode_by_hand("the touch pad held");
    }
    // (the triggers by their button bits, or pressed past half way)
    const bool shoulders = pad.connected && (pad.buttons & shell::PAD_L1) && (pad.buttons & shell::PAD_R1)
        && ((pad.buttons & shell::PAD_L2) || pad.l2 > 128) && ((pad.buttons & shell::PAD_R2) || pad.r2 > 128);
    if (!shoulders) {
        S.shoulders_down_at = 0;
        S.shoulders_switched = false;
    } else if (!S.shoulders_down_at) {
        S.shoulders_down_at = t;
    } else if (!S.shoulders_switched && t - S.shoulders_down_at >= SHOULDERS_HOLD_S && !shell::keyboard_active()) {
        S.shoulders_switched = true;
        switch_game_mode_by_hand("L1 + L2 + R1 + R2 held");
    }
    if (S.keys_mode)
        keys_frame(pad);
    GamepadState state;
    state.connected = pad.connected;
    if (S.game_mode && !S.keys_mode && pad.connected) {
        const uint32_t b = pad.buttons;
        const auto bit = [b](uint32_t mask) { return (b & mask) ? 1.0 : 0.0; };
        // the standard mapping (platform.h); the shoulders not while the hold that switched lasts
        const bool held = S.shoulders_switched;
        state.buttons[0] = bit(shell::PAD_CROSS);
        state.buttons[1] = bit(shell::PAD_CIRCLE);
        state.buttons[2] = bit(shell::PAD_SQUARE);
        state.buttons[3] = bit(shell::PAD_TRIANGLE);
        state.buttons[4] = held ? 0.0 : bit(shell::PAD_L1);
        state.buttons[5] = held ? 0.0 : bit(shell::PAD_R1);
        state.buttons[6] = held ? 0.0 : pad.l2 / 255.0;
        state.buttons[7] = held ? 0.0 : pad.r2 / 255.0;
        state.buttons[8] = S.touchpad_switched ? 0.0 : bit(shell::PAD_TOUCH_PAD);   // (Select; not the hold that switched)
        state.buttons[9] = bit(shell::PAD_OPTIONS);
        state.buttons[10] = bit(shell::PAD_L3);
        state.buttons[11] = bit(shell::PAD_R3);
        state.buttons[12] = bit(shell::PAD_UP);
        state.buttons[13] = bit(shell::PAD_DOWN);
        state.buttons[14] = bit(shell::PAD_LEFT);
        state.buttons[15] = bit(shell::PAD_RIGHT);
        // the sticks, -1 to 1 (up and left negative), still within 4% of the middle (the resting jitter)
        const auto axis = [](uint8_t v) {
            const double a = std::clamp((v - 127.5) / 127.5, -1.0, 1.0);
            return std::fabs(a) < 0.04 ? 0.0 : a;
        };
        state.axes[0] = axis(pad.lx);
        state.axes[1] = axis(pad.ly);
        state.axes[2] = axis(pad.rx);
        state.axes[3] = axis(pad.ry);
    }
    platform_gamepad_update(state);
}

// --- Files as a tab ------------------------------------------------------------------------------------

std::string files_folder_next;   // where the Files tab opens (open_files_tab)
bool files_started = false;      // Files has been shown since its tab was made

void activate(int index)
{
    if (index < 0 || index >= static_cast<int>(S.tabs.size()))
        return;
    if (index != S.active)
        leave_page_fullscreen();
    if (S.game_mode && index != S.active)
        set_game_mode(false, "another tab on show");
    if (S.active >= 0 && S.active < static_cast<int>(S.tabs.size()) && S.active != index)
        S.tabs[S.active].hidden_at = wisp_seconds();
    for (int i = 0; i < static_cast<int>(S.tabs.size()); i++)
        if (i != index)
            platform_set_active(S.tabs[i].wpe, false);
    S.active = index;
    Tab &tab = S.tabs[index];
    if (tab.files) {
        if (!files_started || !files_folder_next.empty())
            files::open(files_folder_next);
        else
            files::show();
        files_started = true;
        files_folder_next.clear();
    } else if (files::visible()) {
        files::hide();
    }
    if (tab.downloads)
        dm::show();
    else if (dm::visible())
        dm::hide();
    if (tab.player)
        player::show();
    else if (player::visible())
        player::hide();
    if (!tab.files && !tab.downloads && !tab.player) {
        wake(tab);
        platform_set_content_size(tab.wpe, W, content_h());
        platform_set_active(tab.wpe, true);
        ext::tab_activated(tab.id);
    }
    S.shown_serial = 0;
    S.toolbar_dirty = S.dirty = true;
    S.store_crx = store_package(tab.uri);
}

// The download manager's page, in its tab (one): made if there is none
void open_downloads_tab()
{
    for (int i = 0; i < static_cast<int>(S.tabs.size()); i++) {
        if (S.tabs[i].downloads) {
            activate(i);
            return;
        }
    }
    Tab tab;
    tab.id = S.next_tab_id++;
    tab.downloads = true;
    tab.title = "Downloads";
    S.tabs.push_back(tab);
    wisp_log("tab %d (id %d) opened: Downloads", static_cast<int>(S.tabs.size()) - 1, tab.id);
    activate(static_cast<int>(S.tabs.size()) - 1);
}

// Wisp Player, in its tab (one): path plays there
void open_player_tab(const std::string &path)
{
    int index = -1;
    for (int i = 0; i < static_cast<int>(S.tabs.size()); i++)
        if (S.tabs[i].player)
            index = i;
    if (!player::open(path))
        return;
    if (index < 0) {
        Tab tab;
        tab.id = S.next_tab_id++;
        tab.player = true;
        S.tabs.push_back(tab);
        index = static_cast<int>(S.tabs.size()) - 1;
        wisp_log("tab %d (id %d) opened: the player", index, tab.id);
    }
    S.tabs[index].title = player::title();
    S.tabs[index].uri = path;
    activate(index);
}

// Files, in its tab (one): made if there is none, at folder ("" Downloads, or where it was)
void open_files_tab(const std::string &folder)
{
    int index = -1;
    for (int i = 0; i < static_cast<int>(S.tabs.size()); i++)
        if (S.tabs[i].files)
            index = i;
    if (index < 0) {
        Tab tab;
        tab.id = S.next_tab_id++;
        tab.files = true;
        tab.title = "Files";
        S.tabs.push_back(tab);
        index = static_cast<int>(S.tabs.size()) - 1;
        files_started = false;
        wisp_log("tab %d (id %d) opened: Files", index, tab.id);
    }
    files_folder_next = folder.empty() && !files_started ? wisp_download_folder() : folder;
    if (index == S.active && files::visible() && !files_folder_next.empty()) {
        files::open(files_folder_next);   // (Files is up: it goes there)
        files_folder_next.clear();
        return;
    }
    activate(index);
}

// A tab gone from the list: the tab on show is its neighbour (a new home tab if it was the last)
void forget_tab(int index)
{
    if (S.tabs[index].view && S.tabs[index].view == S.page_fullscreen_view)
        page_fullscreen(nullptr, false);
    S.tabs.erase(S.tabs.begin() + index);
    S.composed_view = nullptr;   // (its view may be freed now, and a new one can take its address)
    if (S.tabs.empty()) {
        S.active = -1;
        new_tab(wisp_setting("home", HOME_DEFAULT), nullptr, true);
        return;
    }
    if (index < S.active || S.active >= static_cast<int>(S.tabs.size()))
        S.active--;
    activate(std::clamp(S.active, 0, static_cast<int>(S.tabs.size()) - 1));
}

void close_tab(int index)
{
    if (index < 0 || index >= static_cast<int>(S.tabs.size()))
        return;
    Tab &tab = S.tabs[index];
    if (tab.files) {
        files::close();
        files_started = false;
        wisp_log("tab %d (id %d) closed: Files", index, tab.id);
        forget_tab(index);
        return;
    }
    if (tab.downloads) {   // (the downloads go on: the page is all that closes)
        dm::hide();
        wisp_log("tab %d (id %d) closed: Downloads", index, tab.id);
        forget_tab(index);
        return;
    }
    if (tab.player) {
        player::close();
        if (S.fullscreen)
            set_fullscreen(false);
        wisp_log("tab %d (id %d) closed: the player", index, tab.id);
        forget_tab(index);
        return;
    }
    if (tab.asleep) {   // (no page to ask: gone with its saved state)
        wisp_log("tab %d (id %d) closed while asleep", index, tab.id);
        const std::string base = std::string(SLEEP_DIR) + "/tab-" + std::to_string(tab.id);
        unlink((base + ".session").c_str());
        unlink((base + ".picture").c_str());
        if (tab.session)
            g_bytes_unref(tab.session);
        if (tab.ucm)
            g_object_unref(tab.ucm);
        ext::detach_tab(tab.id);
        forget_tab(index);
        return;
    }
    webkit_web_view_try_close(tab.view);   // "close" follows (beforeunload may ask first)
}

void go_home()
{
    load(active_tab(), wisp_setting("home", HOME_DEFAULT));
}

void set_fullscreen(bool on)
{
    S.fullscreen = on;
    for (auto &tab : S.tabs)
        platform_set_content_size(tab.wpe, W, content_h());
    S.toolbar_dirty = S.dirty = true;
}

// A page's own fullscreen (v00.36; "Fix FullScreen of videos, it does not hide the browser elements"): a
// video's fullscreen button, requestFullscreen(). WebKit says so (enter-fullscreen) before it asks the view's
// toplevel (ui/platform.cpp), and the toplevel only took note: the toolbar stayed and the page kept its
// 1920x972. Now the toolbar goes and the page gets the whole screen, as F11 does; when the page leaves it
// (leave-fullscreen), what was on before comes back.
void page_fullscreen(WebKitWebView *view, bool on)
{
    if (on) {
        if (!S.page_fullscreen_view)
            S.fullscreen_before_page = S.fullscreen;
        S.page_fullscreen_view = view;
        set_fullscreen(true);
    } else if (S.page_fullscreen_view) {
        S.page_fullscreen_view = nullptr;
        set_fullscreen(S.fullscreen_before_page);
    }
}

// Out of the page's fullscreen from the browser's side (Circle, Escape, F11, the menu, another tab): the
// view's toplevel leaves fullscreen, WebKit takes the page out of it (its leave-fullscreen then finds the
// toolbar back already)
void leave_page_fullscreen()
{
    WebKitWebView *view = S.page_fullscreen_view;
    if (!view)
        return;
    platform_leave_fullscreen(webkit_web_view_get_wpe_view(view));
    page_fullscreen(nullptr, false);
}

gboolean on_enter_fullscreen(WebKitWebView *view, gpointer)
{
    const Tab *tab = active_tab();
    if (tab && tab->view == view) {
        wisp_log("a page went fullscreen: the toolbar steps aside");
        page_fullscreen(view, true);
        toast("Full screen: Circle or Esc leaves it");
    }
    return FALSE;   // (WebKit then asks the view's toplevel, which says it is fullscreen)
}

gboolean on_leave_fullscreen(WebKitWebView *view, gpointer)
{
    if (view == S.page_fullscreen_view) {
        wisp_log("the page left fullscreen");
        page_fullscreen(nullptr, false);
    }
    return FALSE;
}

// --- Downloads ----------------------------------------------------------------------------------------

// A free name for a download of WebKit's downloader in the download folder (the one chosen on the
// Downloads page, v00.29)
std::string unique_path(const std::string &name)
{
    std::string base = name.empty() ? "download" : name;
    for (char &c : base)
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|')
            c = '_';
    std::string stem = base, ext;
    const size_t dot = base.find_last_of('.');
    if (dot != std::string::npos && dot > 0) {
        stem = base.substr(0, dot);
        ext = base.substr(dot);
    }
    const std::string folder = wisp_download_folder();
    std::string path = folder + "/" + base;
    struct stat st;
    for (int n = 2; stat(path.c_str(), &st) == 0 && n < 1000; n++)
        path = folder + "/" + stem + " (" + std::to_string(n) + ")" + ext;
    return path;
}

// A page's download for the download manager (aria2): WebKit's own is cancelled, the link goes with
// the page's cookies for it and the page as its Referer (Wisp's user agent is aria2's own option), as
// desktop download managers take a browser's downloads
struct Handover {
    std::string uri, referer, name;
};

void hand_to_dm(WebKitDownload *download, const std::string &suggested)
{
    WebKitURIRequest *request = webkit_download_get_request(download);
    const char *uri = request ? webkit_uri_request_get_uri(request) : nullptr;
    auto *handover = new Handover { uri ? uri : "", "", suggested };
    if (WebKitWebView *view = webkit_download_get_web_view(download))
        if (const char *page = webkit_web_view_get_uri(view))
            handover->referer = page;
    g_object_set_data(G_OBJECT(download), "wisp-dm", GINT_TO_POINTER(1));
    webkit_download_cancel(download);
    WebKitCookieManager *cookies = webkit_network_session_get_cookie_manager(S.session);
    webkit_cookie_manager_get_cookies(cookies, handover->uri.c_str(), nullptr, +[](GObject *source, GAsyncResult *result, gpointer data) {
        auto *h = static_cast<Handover *>(data);
        GError *error = nullptr;
        GList *list = webkit_cookie_manager_get_cookies_finish(WEBKIT_COOKIE_MANAGER(source), result, &error);
        std::string cookie;
        for (GList *l = list; l; l = l->next) {
            auto *c = static_cast<SoupCookie *>(l->data);
            if (!cookie.empty())
                cookie += "; ";
            cookie += std::string(soup_cookie_get_name(c)) + "=" + soup_cookie_get_value(c);
        }
        g_list_free_full(list, reinterpret_cast<GDestroyNotify>(soup_cookie_free));
        g_clear_error(&error);
        std::vector<std::string> headers;
        if (!cookie.empty())
            headers.push_back("Cookie: " + cookie);
        if (!h->referer.empty())
            headers.push_back("Referer: " + h->referer);
        headers.push_back("Accept: */*");   // (headers mark a page's download: it may go back to WebKit)
        dm::add(h->uri, headers, h->name);
        wisp_log("download handed to the download manager: %s (%zu cookies' worth, referer %s)", h->uri.c_str(),
            cookie.size(), h->referer.c_str());
        const std::string note = wisp_download_fallback_note();
        toast("Downloading " + (h->name.empty() ? std::string("a file") : h->name) + (note.empty() ? " (OPTIONS > Downloads)" : note));
        delete h;
    }, handover);
}

gboolean on_decide_destination(WebKitDownload *download, char *suggested, gpointer)
{
    const bool install = g_object_get_data(G_OBJECT(download), "wisp-install") != nullptr;
    const bool by_browser = g_object_get_data(G_OBJECT(download), "wisp-browser") != nullptr;
    WebKitURIRequest *request = webkit_download_get_request(download);
    const char *uri = request ? webkit_uri_request_get_uri(request) : nullptr;
    if (!install && !by_browser && uri && dm::running() && dm::can_take(uri)) {
        hand_to_dm(download, suggested ? suggested : "");
        return TRUE;
    }
    // WebKit's own downloader (blob: and data: links, an extension's package, a server that refused
    // the download manager, the manager off): shown in the download manager's list too
    const std::string note = wisp_download_fallback_note();
    const std::string path = unique_path(suggested ? suggested : "");
    webkit_download_set_destination(download, path.c_str());
    Download entry;
    entry.path = path;
    entry.name = path.substr(path.find_last_of('/') + 1);
    entry.install = install;
    S.downloads.push_back(entry);
    g_object_set_data(G_OBJECT(download), "wisp-index", GINT_TO_POINTER(static_cast<int>(S.downloads.size())));
    g_object_set_data(G_OBJECT(download), "wisp-dm-id", GINT_TO_POINTER(dm::external_start(entry.name, path)));
    toast("Downloading " + entry.name + note);
    return TRUE;
}

int dm_id_of(WebKitDownload *download) { return GPOINTER_TO_INT(g_object_get_data(G_OBJECT(download), "wisp-dm-id")); }

Download *download_of(WebKitDownload *download)
{
    const int index = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(download), "wisp-index")) - 1;
    return index >= 0 && index < static_cast<int>(S.downloads.size()) ? &S.downloads[index] : nullptr;
}

void on_download_progress(WebKitDownload *download, GParamSpec *, gpointer)
{
    if (Download *d = download_of(download)) {
        d->progress = webkit_download_get_estimated_progress(download);
        S.dirty = true;
    }
    if (const int id = dm_id_of(download))
        dm::external_progress(id, webkit_download_get_estimated_progress(download),
            static_cast<int64_t>(webkit_download_get_received_data_length(download)));
}

void on_download_finished(WebKitDownload *download, gpointer)
{
    if (g_object_get_data(G_OBJECT(download), "wisp-dm"))
        return;   // (handed to the download manager: WebKit's was cancelled)
    Download *d = download_of(download);
    if (const int id = dm_id_of(download))
        dm::external_done(id, d && !d->failed, "");
    if (d && !d->failed) {
        d->done = true;
        if (d->install) {
            const std::string error = ext::install(d->path);
            toast(error.empty() ? "Extension installed: see OPTIONS > Extensions" : "The extension could not be installed: " + error);
            S.ext_buttons_dirty = S.toolbar_dirty = true;
        } else {
            toast("Downloaded " + d->name + " (OPTIONS > Files)");
        }
    }
}

// "Add to Wisp" on a store's page: the package downloads, then installs (on_download_finished)
void add_from_store()
{
    Tab *tab = active_tab();
    if (!tab || S.store_crx.empty())
        return;
    WebKitDownload *download = webkit_web_view_download_uri(tab->view, S.store_crx.c_str());
    if (download) {
        g_object_set_data(G_OBJECT(download), "wisp-install", GINT_TO_POINTER(1));
        g_object_unref(download);
        toast("Getting the extension...");
    }
}

void on_download_failed(WebKitDownload *download, GError *error, gpointer)
{
    if (g_object_get_data(G_OBJECT(download), "wisp-dm"))
        return;   // (its cancel: the download manager has it)
    if (Download *d = download_of(download)) {
        d->failed = true;
        toast("Download failed: " + d->name + (error ? std::string(" (") + error->message + ")" : std::string()));
    }
}

void on_download_started(WebKitNetworkSession *, WebKitDownload *download, gpointer)
{
    g_signal_connect(download, "decide-destination", G_CALLBACK(on_decide_destination), nullptr);
    g_signal_connect(download, "notify::estimated-progress", G_CALLBACK(on_download_progress), nullptr);
    g_signal_connect(download, "finished", G_CALLBACK(on_download_finished), nullptr);
    g_signal_connect(download, "failed", G_CALLBACK(on_download_failed), nullptr);
}

// --- Drawing ------------------------------------------------------------------------------------------

// RGBA (the canvas) to BGRA (the screen)
inline uint32_t swizzle(uint32_t rgba) { return (rgba & 0xff00ff00u) | ((rgba & 0xff) << 16) | ((rgba >> 16) & 0xff); }

// rows [y0, y1) of the screen, in a per-row list of flags
void mark_rows(std::vector<uint8_t> &rows, int y0, int y1)
{
    y0 = std::max(y0, 0);
    y1 = std::min(y1, H);
    if (y0 < y1)
        std::fill(rows.begin() + y0, rows.begin() + y1, 1);
}

// rows an overlay (a menu, a status line, the pointer, ...) covers in this compose (compose())
void overlay_rows(int y0, int y1) { mark_rows(S.overlay_now, y0, y1); }

void copy_canvas(const Canvas &canvas, int x, int y)
{
    overlay_rows(y, y + canvas.height());
    const uint32_t *src = canvas.data();
    for (int row = 0; row < canvas.height(); row++) {
        const int dy = y + row;
        if (dy < 0 || dy >= H)
            continue;
        uint32_t *dst = &S.compose[static_cast<size_t>(dy) * W];
        for (int col = 0; col < canvas.width(); col++) {
            const int dx = x + col;
            if (dx >= 0 && dx < W)
                dst[dx] = swizzle(src[static_cast<size_t>(row) * canvas.width() + col]);
        }
    }
}

void draw_icon_back(Canvas &c, int cx, int cy, Color col)
{
    c.line(cx + 8, cy, cx - 9, cy, 3.f, col);
    c.line(cx - 9, cy, cx - 1, cy - 8, 3.f, col);
    c.line(cx - 9, cy, cx - 1, cy + 8, 3.f, col);
}

void draw_icon_forward(Canvas &c, int cx, int cy, Color col)
{
    c.line(cx - 8, cy, cx + 9, cy, 3.f, col);
    c.line(cx + 9, cy, cx + 1, cy - 8, 3.f, col);
    c.line(cx + 9, cy, cx + 1, cy + 8, 3.f, col);
}

void draw_icon_reload(Canvas &c, int cx, int cy, Color col, bool stop)
{
    if (stop) {
        c.line(cx - 8, cy - 8, cx + 8, cy + 8, 3.f, col);
        c.line(cx + 8, cy - 8, cx - 8, cy + 8, 3.f, col);
        return;
    }
    c.ring(cx, cy, 10, 3, col);
    c.fill_rect(cx + 2, cy - 12, 12, 10, TAB_ACTIVE);   // a gap in the ring
    c.line(cx + 4, cy - 10, cx + 11, cy - 10, 3.f, col);
    c.line(cx + 11, cy - 10, cx + 11, cy - 3, 3.f, col);
}

void draw_icon_home(Canvas &c, int cx, int cy, Color col)
{
    c.line(cx - 11, cy - 1, cx, cy - 11, 3.f, col);
    c.line(cx, cy - 11, cx + 11, cy - 1, 3.f, col);
    c.line(cx - 8, cy - 3, cx - 8, cy + 10, 3.f, col);
    c.line(cx + 8, cy - 3, cx + 8, cy + 10, 3.f, col);
    c.line(cx - 8, cy + 10, cx + 8, cy + 10, 3.f, col);
}

void draw_icon_menu(Canvas &c, int cx, int cy, Color col)
{
    c.fill_circle(cx, cy - 9, 2.6f, col);
    c.fill_circle(cx, cy, 2.6f, col);
    c.fill_circle(cx, cy + 9, 2.6f, col);
}

// the extensions' toolbar buttons, from the installed and enabled ones
void refresh_ext_buttons()
{
    if (!S.ext_buttons_dirty)
        return;
    S.ext_buttons_dirty = false;
    std::vector<ExtButton> buttons;
    for (const ext::Info &info : ext::list()) {
        if (!info.enabled)
            continue;
        ExtButton b;
        b.id = info.id;
        b.name = info.name;
        b.badge = info.badge_text;
        b.badge_color = info.badge_color;
        // reuse the icon already loaded
        for (auto &old : S.ext_buttons)
            if (old.id == info.id)
                b.icon = old.icon;
        if (b.icon.empty() && !info.icon.empty()) {
            b.icon = ps4ui::load_image(info.icon, 30, 30);
            ps4ui::premultiply(b.icon);
        }
        buttons.push_back(std::move(b));
    }
    S.ext_buttons = std::move(buttons);
    S.toolbar_dirty = true;
}

int ext_area_w() { return static_cast<int>(S.ext_buttons.size()) * EXT_BUTTON + (S.store_crx.empty() ? 0 : STORE_W + 10); }
int address_w() { return MENU_X - 20 - ADDRESS_X - (ext_area_w() ? ext_area_w() + 10 : 0); }
int ext_x0() { return ADDRESS_X + address_w() + 10; }

// --- The memory meter: the tab strip's right end (settings: memory_meter=0 hides it) -------------------
// "RAM": what Wisp's three processes use, their heaps (direct memory: the UI's own figure, the web and
// network processes' every 2 s) plus what they take of the flexible memory pool they share (program
// images, stacks, shared buffers); the bar: that pool, used and left. The pool is what freezes the app
// when it runs out (a frame buffer or a thread it cannot get), so the bar turns amber under 96 MiB left
// and red under 48.
constexpr int METER_W = 330;
bool meter_on()
{
    static const bool on = wisp_setting_int("memory_meter", 1) != 0;
    return on;
}
int tab_width(int n) { return std::min(300, (W - 100 - (meter_on() ? METER_W : 0)) / std::max(1, n)); }

struct Meter {
    int ram = -1;            // MiB in use, all three processes
    int pool_left = -1;      // MiB
    int pool_total = 0;      // MiB: what was left at the start plus what the UI held then
    int direct_total = 0;    // MiB of direct memory the app has (the heaps, the GPU's)
} meter;

// (browser_run, before the web and network processes start)
void meter_start()
{
    WispMemoryCensus census;
    wisp_memory_census(census);
    meter.pool_total = static_cast<int>((wisp_flexible_free() + census.flexible) >> 20);
    meter.direct_total = static_cast<int>(wisp_direct_memory_size() >> 20);
}

// once a second, from frame(): the toolbar is drawn again when a figure changed
void meter_update()
{
    static double last = 0;
    const double now = wisp_seconds();
    if (now - last < 1)
        return;
    last = now;
    static int downloads = -1;   // (the Downloads tab's title says how many run)
    if (dm::active_count() != downloads) {
        downloads = dm::active_count();
        S.toolbar_dirty = S.dirty = true;
    }
    if (!meter_on())
        return;
    const int left = static_cast<int>(wisp_flexible_free() >> 20);
    meter.pool_total = std::max(meter.pool_total, left);   // (the pool grows when the system can spare it)
    int heaps = static_cast<int>(wisp_memory_footprint() >> 20);
    for (WispChild child : { WispChild::Web, WispChild::Network })
        heaps += std::max(0, wisp_child_heap_mib(child));
    const int ram = heaps + (meter.pool_total - left);
    if (ram != meter.ram || left != meter.pool_left) {
        meter.ram = ram;
        meter.pool_left = left;
        S.toolbar_dirty = S.dirty = true;
    }
}

// "RAM 782 MiB / 4.9 GiB": what the three processes use of all the app's memory (its direct memory,
// about 4.5 GiB, and the pool), the bar that much; then the pool the processes share (program images,
// frame buffers: a few hundred MiB, the PS4's to size), which is what freezes the app when it runs out,
// so it turns amber, then red, as it gets low (the bar too)
void draw_meter(Canvas &c)
{
    if (!meter_on() || meter.ram < 0)
        return;
    const int x0 = W - METER_W + 8, w = METER_W - 24;
    const int all = std::max(1, meter.direct_total + meter.pool_total);
    char ram[48], left[40];
    if (meter.ram >= 1024)
        snprintf(ram, sizeof ram, "RAM %.2f / %.1f GiB", meter.ram / 1024.0, all / 1024.0);
    else
        snprintf(ram, sizeof ram, "RAM %d MiB / %.1f GiB", meter.ram, all / 1024.0);
    snprintf(left, sizeof left, "pool %d MiB free", meter.pool_left);
    c.text(x0, 21, ram, 16, TEXT, Weight::Bold);
    const int lw = c.text_width(left, 14);
    const Color warn = meter.pool_left < 48 ? rgb(0xf28b82) : meter.pool_left < 96 ? rgb(0xfdd663) : rgb(0x81c995);
    c.text(x0 + w - lw, 21, left, 14, meter.pool_left < 96 ? warn : DIM);
    c.fill_round_rect(x0, 29, w, 9, 4, TAB_IDLE);
    const int used_w = std::clamp(static_cast<int>(static_cast<long long>(w) * meter.ram / all), 4, w);
    c.fill_round_rect(x0, 29, used_w, 9, 4, warn);
}

// which toolbar item the pointer is on: -1 none; 0 back, 1 forward, 2 reload, 3 home, 4 address,
// 5 menu, 100+i tab i, 200+i tab i's close button, 300 new tab, 400+i extension i, 500 "Add to Wisp"
int toolbar_item(float x, float y)
{
    if (S.fullscreen || y >= TOOLBAR_H)
        return -1;
    if (y < TAB_H) {
        const int n = static_cast<int>(S.tabs.size());
        const int tab_w = tab_width(n);
        const int i = static_cast<int>(x - 12) / tab_w;
        if (x >= 12 && i < n) {
            if (x >= 12 + (i + 1) * tab_w - 36)
                return 200 + i;
            return 100 + i;
        }
        if (x >= 12 + n * tab_w && x < 12 + n * tab_w + 46)
            return 300;
        return -1;
    }
    const int cy = TAB_H + (TOOLBAR_H - TAB_H) / 2;
    if (std::fabs(y - cy) > BUTTON / 2)
        return -1;
    for (int i = 0; i < 4; i++) {
        const int bx = 18 + i * (BUTTON + 14);
        if (x >= bx && x < bx + BUTTON)
            return i;
    }
    if (x >= ADDRESS_X && x < ADDRESS_X + address_w())
        return 4;
    int ex = ext_x0();
    if (!S.store_crx.empty()) {
        if (x >= ex && x < ex + STORE_W)
            return 500;
        ex += STORE_W + 10;
    }
    for (size_t i = 0; i < S.ext_buttons.size(); i++)
        if (x >= ex + static_cast<int>(i) * EXT_BUTTON && x < ex + static_cast<int>(i + 1) * EXT_BUTTON - 4)
            return 400 + static_cast<int>(i);
    if (x >= MENU_X && x < MENU_X + BUTTON)
        return 5;
    return -1;
}

void draw_toolbar(int hover)
{
    Canvas c(W, TOOLBAR_H);
    c.fill_rect(0, 0, W, TOOLBAR_H, BG);
    // tabs
    const int n = static_cast<int>(S.tabs.size());
    const int tab_w = tab_width(n);
    for (int i = 0; i < n; i++) {
        const int x = 12 + i * tab_w;
        const bool on = i == S.active;
        c.fill_round_rect(x, 6, tab_w - 6, TAB_H - 6 + (on ? 8 : 0), 10, on ? TAB_ACTIVE : (hover == 100 + i ? HOVER : TAB_IDLE));
        std::string title = S.tabs[i].title.empty() ? S.tabs[i].uri : S.tabs[i].title;
        if (title.empty())
            title = "New tab";
        if (S.tabs[i].downloads && dm::active_count() > 0)
            title += " (" + std::to_string(dm::active_count()) + ")";
        if (S.tabs[i].loading)
            c.fill_circle(x + 18, 6 + (TAB_H - 6) / 2, 5, ACCENT);
        else if (S.tabs[i].asleep)   // (a sleeping tab: a small moon, its title dimmer)
            c.fill_circle(x + 18, 6 + (TAB_H - 6) / 2, 5, rgb(0x5f6368));
        else if (S.tabs[i].files)
            c.fill_round_rect(x + 12, 6 + (TAB_H - 6) / 2 - 5, 13, 10, 2, rgb(0xfdd663));
        else if (S.tabs[i].player) {   // (a play triangle)
            const int ax = x + 14, ay = 6 + (TAB_H - 6) / 2;
            for (int k = 0; k < 7; k++)
                c.line(ax, ay - 7 + k * 2, ax + 12, ay, 1.6f, ACCENT);
        } else if (S.tabs[i].downloads) {   // (an arrow down)
            const int ax = x + 18, ay = 6 + (TAB_H - 6) / 2;
            c.line(ax, ay - 7, ax, ay + 5, 2.5f, ACCENT);
            c.line(ax - 5, ay, ax, ay + 6, 2.5f, ACCENT);
            c.line(ax + 5, ay, ax, ay + 6, 2.5f, ACCENT);
        }
        c.text(x + 32, 32, title, 19, on ? TEXT : S.tabs[i].asleep ? rgb(0x6f747a) : DIM, on ? Weight::Bold : Weight::Regular, tab_w - 80);
        const Color close = hover == 200 + i ? TEXT : DIM;
        const int cx = x + tab_w - 26, cy = 6 + (TAB_H - 6) / 2;
        c.line(cx - 6, cy - 6, cx + 6, cy + 6, 2.f, close);
        c.line(cx + 6, cy - 6, cx - 6, cy + 6, 2.f, close);
    }
    {
        const int x = 12 + n * tab_w + 6, cy = 6 + (TAB_H - 6) / 2;
        if (hover == 300)
            c.fill_round_rect(x, 8, 34, 34, 8, HOVER);
        c.line(x + 17, cy - 9, x + 17, cy + 9, 2.5f, TEXT);
        c.line(x + 8, cy, x + 26, cy, 2.5f, TEXT);
    }
    draw_meter(c);
    c.fill_rect(0, TAB_H, W, TOOLBAR_H - TAB_H, TAB_ACTIVE);
    const Tab *tab = active_tab();
    const int cy = TAB_H + (TOOLBAR_H - TAB_H) / 2;
    const bool can_back = tab && webkit_web_view_can_go_back(tab->view);
    const bool can_forward = tab && webkit_web_view_can_go_forward(tab->view);
    for (int i = 0; i < 4; i++) {
        const int bx = 18 + i * (BUTTON + 14), bcx = bx + BUTTON / 2;
        if (hover == i)
            c.fill_circle(bcx, cy, BUTTON / 2, HOVER);
        switch (i) {
        case 0: draw_icon_back(c, bcx, cy, can_back ? TEXT : DIM); break;
        case 1: draw_icon_forward(c, bcx, cy, can_forward ? TEXT : DIM); break;
        case 2: draw_icon_reload(c, bcx, cy, TEXT, tab && tab->loading); break;
        case 3: draw_icon_home(c, bcx, cy, TEXT); break;
        }
    }
    // the address field: typed in place with a USB keyboard (a caret, the text scrolled to it),
    // else the page's address
    const int aw = address_w();
    if (S.editing) {
        c.fill_round_rect(ADDRESS_X - 2, cy - 26, aw + 4, 52, 26, ACCENT);
        c.fill_round_rect(ADDRESS_X, cy - 24, aw, 48, 24, FIELD);
        const int tx = ADDRESS_X + 28, room = aw - 56;
        const std::string_view text = S.edit_text;
        const size_t caret = std::min(S.edit_caret, S.edit_text.size());
        size_t start = 0;   // the first byte shown: the caret stays in view
        if (c.text_width(text.substr(0, caret), 22) > room) {
            start = caret;
            while (start > 0) {
                size_t prev = start - 1;
                while (prev > 0 && (static_cast<unsigned char>(text[prev]) & 0xc0) == 0x80)
                    prev--;
                if (c.text_width(text.substr(prev, caret - prev), 22) > room)
                    break;
                start = prev;
            }
        }
        const std::string_view shown = text.substr(start);
        if (S.edit_all && !shown.empty())
            c.fill_rect(tx - 2, cy - 17, std::min(room, c.text_width(shown, 22)) + 4, 34, rgb(0x3b5ea8));
        if (shown.empty() && S.edit_all)
            c.text(tx, cy + 8, "Search or type an address", 22, DIM);
        else
            c.text(tx, cy + 8, shown, 22, TEXT, Weight::Regular, room);
        if (!S.edit_all)
            c.fill_rect(tx + c.text_width(text.substr(start, caret - start), 22), cy - 15, 2, 30, TEXT);
    } else {
        c.fill_round_rect(ADDRESS_X, cy - 24, aw, 48, 24, hover == 4 ? HOVER : FIELD);
        std::string address = tab ? tab->uri : "";
        const bool secure = address.rfind("https://", 0) == 0;
        if (address.empty())
            c.text(ADDRESS_X + 28, cy + 8, "Search or type an address (Triangle)", 22, DIM);
        else {
            if (secure)
                c.fill_circle(ADDRESS_X + 30, cy, 6, rgb(0x81c995));
            c.text(ADDRESS_X + (secure ? 48 : 28), cy + 8, address, 22, TEXT, Weight::Regular, aw - 80);
        }
    }
    if (tab && tab->loading && tab->progress > 0 && tab->progress < 1)
        c.fill_rect(ADDRESS_X + 20, cy + 20, static_cast<int>((aw - 40) * tab->progress), 3, ACCENT);
    // "Add to Wisp" on a store's page, then the extensions' buttons with their badges
    int ex = ext_x0();
    if (!S.store_crx.empty()) {
        c.fill_round_rect(ex, cy - 22, STORE_W, 44, 22, hover == 500 ? rgb(0xaecbfa) : ACCENT);
        c.text_centered(ex + STORE_W / 2, cy + 8, "Add to OrbisWeb", 21, rgb(0x0b1a33), Weight::Bold);
        ex += STORE_W + 10;
    }
    for (size_t i = 0; i < S.ext_buttons.size(); i++) {
        const ExtButton &b = S.ext_buttons[i];
        const int bx = ex + static_cast<int>(i) * EXT_BUTTON, bcx = bx + EXT_BUTTON / 2 - 2;
        if (hover == 400 + static_cast<int>(i))
            c.fill_circle(bcx, cy, 22, HOVER);
        if (!b.icon.empty())
            c.blit(b.icon, bcx - b.icon.width / 2, cy - b.icon.height / 2);
        else
            c.text_centered(bcx, cy + 8, b.name.substr(0, 1), 22, TEXT, Weight::Bold);
        if (!b.badge.empty()) {
            const int tw = std::max(18, c.text_width(b.badge, 14, Weight::Bold) + 8);
            const Color badge = rgb(((b.badge_color & 0xff) << 16) | (b.badge_color & 0xff00) | ((b.badge_color >> 16) & 0xff));
            c.fill_round_rect(bcx + 14 - tw / 2, cy + 6, tw, 18, 9, badge);
            c.text_centered(bcx + 14, cy + 20, b.badge, 14, rgb(0xffffff), Weight::Bold);
        }
    }
    if (hover == 5)
        c.fill_circle(MENU_X + BUTTON / 2, cy, BUTTON / 2, HOVER);
    draw_icon_menu(c, MENU_X + BUTTON / 2, cy, TEXT);

    S.toolbar.resize(static_cast<size_t>(W) * TOOLBAR_H);
    const uint32_t *src = c.data();
    for (size_t i = 0; i < S.toolbar.size(); i++)
        S.toolbar[i] = swizzle(src[i]);
    S.toolbar_dirty = false;
}

// (v00.32: the switches that were here - ad blocking, rendering, the JIT, WebAssembly, WebGL, the WebGPU
// test, page messages - are on the Settings page with every other setting, ui/pages.cpp)
const char *const MENU_ITEMS[] = { "New tab", "Close tab", "Reload", "Full screen", "Add bookmark", "Bookmarks", "History",
    "Downloads", "Files", "Extensions", "Settings", "About OrbisWeb", "Restart OrbisWeb", "Leave OrbisWeb" };
constexpr int MENU_COUNT = sizeof MENU_ITEMS / sizeof MENU_ITEMS[0];
constexpr int MENU_W = 520, MENU_ROW = 56;
enum MenuItem { M_NEW_TAB, M_CLOSE_TAB, M_RELOAD, M_FULLSCREEN, M_BOOKMARK, M_BOOKMARKS, M_HISTORY, M_DOWNLOADS, M_FILES,
    M_EXTENSIONS, M_SETTINGS, M_ABOUT, M_RESTART, M_LEAVE };
// WebKit built with WebAssembly (CMake: its ENABLE_WEBASSEMBLY; scripts/build-webkit.sh)
#ifndef WISP_HAVE_WASM
#define WISP_HAVE_WASM 0
#endif

void draw_menu()
{
    Canvas c(MENU_W, MENU_COUNT * MENU_ROW + 24);
    c.fill_rect(0, 0, c.width(), c.height(), rgb(0x2b2c30));
    for (int i = 0; i < MENU_COUNT; i++) {
        const int y = 12 + i * MENU_ROW;
        if (i == S.menu_index)
            c.fill_round_rect(10, y, MENU_W - 20, MENU_ROW - 6, 10, HOVER);
        const Tab *page = active_tab();
        const bool marked = i == M_BOOKMARK && page && pages::bookmarked(page->uri);
        c.text(32, y + 34, marked ? "Remove bookmark" : MENU_ITEMS[i], 23, TEXT);
        std::string value;
        if (i == M_FULLSCREEN)
            value = S.fullscreen ? "On" : "Off";
        else if (marked)
            value = "\xe2\x98\x85";   // (a star: this page is bookmarked)
        if (!value.empty())
            c.text(MENU_W - 28 - c.text_width(value, 21), y + 34, value, 21, ACCENT);
    }
    copy_canvas(c, W - MENU_W - 16, TOOLBAR_H - 4);
}

// OPTIONS > Extensions: the installed ones (Cross turns one on or off, Square removes it, Triangle
// opens its options), and how to get more
constexpr int EXT_LIST_W = 1100, EXT_ROW = 74;

void draw_extensions()
{
    const std::vector<ext::Info> list = ext::list();
    const int rows = std::max<int>(1, static_cast<int>(list.size()));
    Canvas c(EXT_LIST_W, 200 + std::min(rows, 9) * EXT_ROW);
    c.fill_rect(0, 0, c.width(), c.height(), rgb(0x2b2c30));
    c.text(32, 54, "Extensions", 30, TEXT, Weight::Bold);
    c.text(32, 92, "Chrome, Opera and Firefox extensions: open a store's page and press Add to OrbisWeb, or open a .crx, .nex,", 19, DIM);
    c.text(32, 118, ".xpi or .zip in Files. Cross: on/off   Square: remove   Triangle: options   Circle: close", 19, DIM);
    if (list.empty())
        c.text(32, 180, "None installed yet.", 22, DIM);
    int y = 150;
    S.ext_index = std::clamp(S.ext_index, 0, std::max(0, static_cast<int>(list.size()) - 1));
    for (size_t i = 0; i < list.size() && i < 9; i++) {
        const ext::Info &e = list[i];
        if (static_cast<int>(i) == S.ext_index)
            c.fill_round_rect(16, y, EXT_LIST_W - 32, EXT_ROW - 8, 12, HOVER);
        c.text(40, y + 30, e.name + "  " + e.version, 22, TEXT, Weight::Bold, 700);
        c.text(40, y + 56, e.description.empty() ? e.id : e.description, 17, DIM, Weight::Regular, 780);
        c.fill_round_rect(EXT_LIST_W - 150, y + 18, 100, 34, 17, e.enabled ? ACCENT : rgb(0x5f6368));
        c.text_centered(EXT_LIST_W - 100, y + 42, e.enabled ? "On" : "Off", 19, e.enabled ? rgb(0x0b1a33) : TEXT, Weight::Bold);
        y += EXT_ROW;
    }
    copy_canvas(c, (W - c.width()) / 2, TOOLBAR_H + 40);
}

void draw_status_line(const std::string &text, int y, Color color)
{
    Canvas c(std::min(W - 40, 40 + static_cast<int>(text.size()) * 12), 38);
    c.fill_rect(0, 0, c.width(), c.height(), rgb(0x2b2c30));
    c.text(16, 26, text, 19, color, Weight::Regular, c.width() - 28);
    copy_canvas(c, 12, y);
}

void draw_context_menu()
{
    const int n = static_cast<int>(S.ctx_labels.size());
    Canvas c(CTX_W, n * CTX_ROW + 16);
    c.fill_rect(0, 0, c.width(), c.height(), rgb(0x5f6368));
    c.fill_rect(1, 1, c.width() - 2, c.height() - 2, rgb(0x2b2c30));
    for (int i = 0; i < n; i++) {
        const int y = 8 + i * CTX_ROW;
        if (i == S.ctx_index && S.ctx_enabled[i])
            c.fill_round_rect(6, y, CTX_W - 12, CTX_ROW - 4, 8, HOVER);
        c.text(24, y + 32, S.ctx_labels[i], 21, S.ctx_enabled[i] ? TEXT : rgb(0x6f7378));
    }
    copy_canvas(c, S.ctx_x, S.ctx_y);
}

// --- The address bar's list (v00.36: "add drop down or list menu of addresses used before") ------------
//
// Under the address field: the addresses used before (pages::suggestions: bookmarks, then the history,
// newest first). Typed with a USB keyboard, it follows what is typed, and Up / Down choose a row for Enter.
// With the controller, Cross on the field opens it: its first row opens the PS4's keyboard, the others go
// to their page (Triangle still opens the keyboard at once). A click on a row goes there too.
constexpr int ADDR_ROW = 56, ADDR_ROWS = 8;

// (the controller's list has the keyboard's row first)
bool addresses_with_keyboard_row() { return S.overlay == Overlay::Addresses; }

int addresses_count() { return static_cast<int>(S.addr_list.size()) + (addresses_with_keyboard_row() ? 1 : 0); }

// The list for what is typed now (made again when that changed)
void addresses_refresh()
{
    const std::string typed = S.editing && !S.edit_all ? S.edit_text : std::string();
    if (typed == S.addr_for)
        return;
    S.addr_for = typed;
    S.addr_list = pages::suggestions(typed, ADDR_ROWS);
    S.addr_index = addresses_with_keyboard_row() ? 0 : -1;
}

bool addresses_shown() { return S.overlay == Overlay::Addresses || (S.editing && !S.addr_list.empty()); }

int address_row_at(float x, float y)
{
    if (!addresses_shown())
        return -1;
    const int x0 = ADDRESS_X, y0 = TOOLBAR_H - 2;
    if (x < x0 || x >= x0 + address_w() || y < y0)
        return -1;
    const int row = static_cast<int>(y - y0) / ADDR_ROW;
    return row < addresses_count() ? row : -1;
}

void draw_addresses()
{
    addresses_refresh();
    const int n = addresses_count();
    if (!addresses_shown() || n == 0)
        return;
    const int w = address_w();
    Canvas c(w, n * ADDR_ROW + 12);
    c.fill_rect(0, 0, c.width(), c.height(), rgb(0x5f6368));
    c.fill_rect(1, 0, c.width() - 2, c.height() - 1, rgb(0x2b2c30));
    for (int i = 0; i < n; i++) {
        const int y = 6 + i * ADDR_ROW;
        if (i == S.addr_index)
            c.fill_round_rect(8, y, w - 16, ADDR_ROW - 4, 10, HOVER);
        const int ty = y + ADDR_ROW / 2 + 7;
        if (addresses_with_keyboard_row() && i == 0) {
            c.text(28, ty, "Search or type an address", 21, TEXT, Weight::Bold);
            c.text(w - 40 - c.text_width("Triangle", 18), ty, "Triangle", 18, DIM);
            continue;
        }
        const pages::Suggestion &s = S.addr_list[static_cast<size_t>(i - (addresses_with_keyboard_row() ? 1 : 0))];
        std::string shown = s.uri;
        for (const char *scheme : {"https://", "http://"})
            if (shown.rfind(scheme, 0) == 0) {
                shown.erase(0, strlen(scheme));
                break;
            }
        // (a star: a bookmark)
        const int tx = s.bookmark ? 58 : 28;
        if (s.bookmark)
            c.text(26, ty, "\xe2\x98\x85", 20, ACCENT);
        const int address_room = std::min(w / 2, std::max(200, c.text_width(shown, 21)));
        c.text(tx, ty, shown, 21, TEXT, Weight::Regular, address_room);
        if (!s.title.empty())
            c.text(tx + address_room + 24, ty, s.title, 19, DIM, Weight::Regular, w - tx - address_room - 56);
    }
    copy_canvas(c, ADDRESS_X, TOOLBAR_H - 2);
}

// The list for the controller (Cross on the address field without a USB keyboard): straight to the
// PS4's keyboard when there is nothing to list
void open_addresses()
{
    context_close();
    S.overlay = Overlay::Addresses;
    S.addr_for = "\x01";
    addresses_refresh();
    if (S.addr_list.empty()) {
        S.overlay = Overlay::None;
        open_address_keyboard();
        return;
    }
    S.addr_index = 0;
    S.dirty = true;
}

// A row taken: its page in the tab on show (a new one when there is none), or the keyboard's row
void address_choose(int row)
{
    const bool keyboard_row = addresses_with_keyboard_row() && row == 0;
    const int i = row - (addresses_with_keyboard_row() ? 1 : 0);
    const std::string uri = !keyboard_row && i >= 0 && i < static_cast<int>(S.addr_list.size()) ? S.addr_list[static_cast<size_t>(i)].uri : "";
    S.overlay = Overlay::None;
    edit_stop();
    S.addr_for = "\x01";
    S.dirty = S.toolbar_dirty = true;
    if (keyboard_row) {
        open_address_keyboard();
        return;
    }
    if (uri.empty())
        return;
    if (Tab *tab = active_tab())
        load(tab, uri);
    else
        new_tab(uri, nullptr, true);
}

bool in_popup(float x, float y);

// the mouse pointer: the page's shape (a hand over links, a beam over text) over the page, the arrow
// over the browser's own parts
void draw_pointer(float px, float py)
{
    // a page with the controller (v00.30): no pointer, unless a mouse moves it
    if (S.game_mode && !S.last_input.mouse)
        return;
    // over a playing video, the pointer goes after 3 s still (as a desktop player's)
    if (player::visible() && S.overlay == Overlay::None && py >= content_y() && wisp_seconds() - S.pointer_moved_at > 3)
        return;
    const bool over_page = S.overlay == Overlay::None && !files::visible() && !dm::visible() && !player::visible()
        && (py >= content_y() || in_popup(px, py));
    const CursorShape shape = over_page ? cursor_shape_for(platform_cursor_name()) : CursorShape::Arrow;
    int y0, y1;
    cursor_rows(py, shape, y0, y1);
    overlay_rows(y0, y1);
    cursor_draw(S.compose.data(), W, H, px, py, shape);
}

// Row y of the picture under the overlays: the toolbar, the page (white past its frame), or the
// background while the tab has no frame yet.
void base_row(int y, const Frame *frame)
{
    uint32_t *dst = &S.compose[static_cast<size_t>(y) * W];
    const int top = content_y();
    if (y < top) {
        if (static_cast<size_t>(y + 1) * W <= S.toolbar.size())
            memcpy(dst, &S.toolbar[static_cast<size_t>(y) * W], static_cast<size_t>(W) * 4);
        else
            std::fill_n(dst, W, 0xff1f2023u);
        return;
    }
    if (!frame) {
        std::fill_n(dst, W, 0xff1f2023u);
        return;
    }
    const int row = y - top;
    if (row >= frame->height) {
        std::fill_n(dst, W, 0xffffffffu);
        return;
    }
    const int cols = std::min(frame->width, W);
    memcpy(dst, frame->pixels + static_cast<size_t>(row) * frame->stride, static_cast<size_t>(cols) * 4);
    if (cols < W)
        std::fill_n(dst + cols, W - cols, 0xffffffffu);
}

// S.compose holds the whole picture (toolbar, page, overlays, pointer) in ordinary memory. A compose
// rebuilds only the rows that changed: the page's damage (platform_take_damage), the toolbar when it
// was drawn again, and the rows the overlays and the pointer covered last time (drawn again on top).
// Each row carries the number of the compose that last changed it, and each of the two screen
// buffers gets just the rows it does not have yet: a still page under a moving pointer writes a few
// dozen rows to the write-combined screen, not all 8 MiB, and a scrolled page the rows it moved.
void compose(const shell::Input &in)
{
    const size_t pixels = static_cast<size_t>(W) * H;
    const bool first = S.compose.size() != pixels;
    if (first) {
        S.compose.assign(pixels, 0xff1f2023u);
        S.row_gen.assign(H, 0);
        for (auto &have : S.buffer_gen)
            have.assign(H, UINT32_MAX);
        S.overlay_last.assign(H, 0);
    }
    S.gen++;
    S.restore.assign(H, 0);
    for (int y = 0; y < H; y++)
        S.restore[y] = S.overlay_last[y];
    // (settings: partial_frames=0, every row every time)
    static const bool partial = wisp_setting_int("partial_frames", 1) != 0;
    if (!partial)
        mark_rows(S.restore, 0, H);
    const int y0 = content_y();
    if (S.toolbar_dirty && !S.fullscreen) {
        draw_toolbar(toolbar_item(in.x, in.y));
        mark_rows(S.restore, 0, y0);
    }
    Tab *tab = active_tab();
    Frame frame;
    const bool has_frame = tab && platform_frame(tab->wpe, frame);
    WPEView *view = has_frame ? tab->wpe : nullptr;
    int d0, d1;
    if (first || view != S.composed_view || S.fullscreen != S.composed_fullscreen
        || (has_frame && (frame.width != S.composed_w || frame.height != S.composed_h))) {
        // another tab, full screen on or off, a new size: all of it
        mark_rows(S.restore, 0, H);
        if (view)
            platform_take_damage(view, d0, d1);
        S.composed_view = view;
        S.composed_fullscreen = S.fullscreen;
        S.composed_w = has_frame ? frame.width : -1;
        S.composed_h = has_frame ? frame.height : -1;
    } else if (view && platform_take_damage(view, d0, d1)) {
        mark_rows(S.restore, y0 + d0, y0 + d1);
    }
    if (has_frame)
        S.shown_serial = frame.serial;
    for (int y = 0; y < H; y++) {
        if (S.restore[y]) {
            base_row(y, has_frame ? &frame : nullptr);
            S.row_gen[y] = S.gen;
        }
    }
    // the overlays and the pointer on top (copy_canvas, draw_pointer and the popup and Files below
    // note the rows they cover)
    S.overlay_now.assign(H, 0);
    if (tab && tab->crashed)
        draw_status_line("This page stopped working. Cross to reload it.", y0 + 20, rgb(0xf28b82));
    if (!S.hover_link.empty() && S.overlay == Overlay::None)
        draw_status_line(S.hover_link, H - 50, DIM);
    // an extension's popup, under its button, with a 2-pixel frame
    Frame popup;
    if (S.popup_wpe && platform_frame(S.popup_wpe, popup)) {
        const int x0 = S.popup_x, py0 = S.popup_y;
        overlay_rows(py0 - 2, py0 + S.popup_h + 2);
        for (int row = -2; row < S.popup_h + 2; row++) {
            const int y = py0 + row;
            if (y < 0 || y >= H)
                continue;
            uint32_t *dst = &S.compose[static_cast<size_t>(y) * W];
            if (row < 0 || row >= S.popup_h || row >= popup.height) {
                std::fill_n(dst + std::max(0, x0 - 2), std::min(W, x0 + S.popup_w + 2) - std::max(0, x0 - 2), 0xff5f6368u);
                continue;
            }
            dst[std::max(0, x0 - 2)] = dst[std::max(0, x0 - 1)] = 0xff5f6368u;
            const int cols = std::min({ popup.width, S.popup_w, W - x0 });
            memcpy(dst + x0, popup.pixels + static_cast<size_t>(row) * popup.stride, static_cast<size_t>(cols) * 4);
            if (x0 + S.popup_w + 1 < W)
                dst[x0 + S.popup_w] = dst[x0 + S.popup_w + 1] = 0xff5f6368u;
        }
    }
    // Files over the page, drawn again only when it changed
    if (files::visible()) {
        const int fy = content_y(), fh = content_h();
        if (files::dirty() || S.files_pixels.size() != static_cast<size_t>(W) * fh) {
            Canvas fc(W, fh);
            files::draw(fc);
            S.files_pixels.resize(static_cast<size_t>(W) * fh);
            const uint32_t *src = fc.data();
            for (size_t i = 0; i < S.files_pixels.size(); i++)
                S.files_pixels[i] = swizzle(src[i]);
        }
        memcpy(&S.compose[static_cast<size_t>(fy) * W], S.files_pixels.data(), S.files_pixels.size() * 4);
        overlay_rows(fy, fy + fh);
    }
    // the player (its tab): the video, its subtitles and bar, straight into the picture
    if (player::visible()) {
        player::paint(S.compose.data(), W, 0, content_y(), W, content_h());
        overlay_rows(content_y(), H);
    }
    // the download manager's page (its tab), the same way
    if (dm::visible()) {
        const int fy = content_y(), fh = content_h();
        if (dm::dirty() || S.dm_pixels.size() != static_cast<size_t>(W) * fh) {
            Canvas dc(W, fh);
            dm::draw(dc);
            S.dm_pixels.resize(static_cast<size_t>(W) * fh);
            const uint32_t *src = dc.data();
            for (size_t i = 0; i < S.dm_pixels.size(); i++)
                S.dm_pixels[i] = swizzle(src[i]);
        }
        memcpy(&S.compose[static_cast<size_t>(fy) * W], S.dm_pixels.data(), S.dm_pixels.size() * 4);
        overlay_rows(fy, fy + fh);
    }
    if (S.overlay == Overlay::Menu)
        draw_menu();
    else if (S.overlay == Overlay::Extensions)
        draw_extensions();
    else if (S.overlay == Overlay::Context)
        draw_context_menu();
    else if (S.overlay == Overlay::Addresses || S.editing)
        draw_addresses();
    if (!S.toast.empty() && wisp_seconds() < S.toast_until)
        draw_status_line(S.toast, H - 100, TEXT);
    if (in.pad || in.mouse)
        draw_pointer(in.x, in.y);
    for (int y = 0; y < H; y++)
        if (S.overlay_now[y])
            S.row_gen[y] = S.gen;
    S.overlay_last.swap(S.overlay_now);

    // the screen buffer: the runs of rows it does not have yet, in one copy each
    std::vector<uint32_t> &have = S.buffer_gen[shell::screen_back_index()];
    uint32_t *back = shell::screen_back_buffer();
    for (int y = 0; y < H;) {
        if (have[y] == S.row_gen[y]) {
            y++;
            continue;
        }
        int end = y;
        while (end < H && have[end] != S.row_gen[end]) {
            have[end] = S.row_gen[end];
            end++;
        }
        memcpy(back + static_cast<size_t>(y) * W, &S.compose[static_cast<size_t>(y) * W], static_cast<size_t>(end - y) * W * 4);
        y = end;
    }
    shell::screen_flip();
    if (tab)
        platform_frame_shown(tab->wpe);
    if (S.popup_wpe)
        platform_frame_shown(S.popup_wpe);
    S.dirty = false;
}

// --- Extensions' views ------------------------------------------------------------------------------

// a web view for an extension's own pages (its background page, a popup): never a tab
WebKitWebView *make_extension_view(WebKitUserContentManager *ucm)
{
    auto *view = WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW, "display", platform_display(), "web-context", S.context,
        "network-session", S.session, "settings", S.settings, "user-content-manager", ucm, nullptr));
    // (the caller holds the one reference: a plain GObject in WPE, see make_view)
    platform_set_content_size(webkit_web_view_get_wpe_view(view), 420, 600);
    platform_set_active(webkit_web_view_get_wpe_view(view), false);
    return view;
}

void close_popup()
{
    if (!S.popup)
        return;
    platform_set_active(S.popup_wpe, false);
    g_object_unref(S.popup);
    S.popup = nullptr;
    S.popup_wpe = nullptr;
    S.dirty = true;
}

void open_extension_popup(int button)
{
    if (button < 0 || button >= static_cast<int>(S.ext_buttons.size()))
        return;
    const bool was_open = S.popup != nullptr;
    close_popup();
    if (was_open)
        return;   // the button again: closed
    Tab *tab = active_tab();
    WebKitWebView *view = ext::action_clicked(S.ext_buttons[button].id, tab ? tab->id : -1);
    if (!view)
        return;
    S.popup = view;
    S.popup_wpe = webkit_web_view_get_wpe_view(view);
    S.popup_w = 420;
    S.popup_h = 600;
    const int ex = ext_x0() + (S.store_crx.empty() ? 0 : STORE_W + 10) + button * EXT_BUTTON;
    S.popup_x = std::clamp(ex + EXT_BUTTON - S.popup_w, 16, W - S.popup_w - 16);
    S.popup_y = TOOLBAR_H + 6;
    platform_set_content_size(S.popup_wpe, S.popup_w, S.popup_h);
    platform_set_active(S.popup_wpe, true);
    g_signal_connect(view, "close", G_CALLBACK(+[](WebKitWebView *, gpointer) { close_popup(); }), nullptr);
}

bool in_popup(float x, float y)
{
    return S.popup && x >= S.popup_x && x < S.popup_x + S.popup_w && y >= S.popup_y && y < S.popup_y + S.popup_h;
}

// --- Input --------------------------------------------------------------------------------------------

WPEModifiers modifiers(uint32_t buttons_held_mask = 0)
{
    return static_cast<WPEModifiers>(S.modifiers | buttons_held_mask);
}

// the view under (x, y): an open extension popup, else the tab's page; (ox, oy) its corner on screen
WPEView *target_view(float x, float y, double &ox, double &oy)
{
    if (in_popup(x, y)) {
        ox = S.popup_x;
        oy = S.popup_y;
        return S.popup_wpe;
    }
    Tab *tab = active_tab();
    ox = 0;
    oy = content_y();
    return tab ? tab->wpe : nullptr;
}

void page_pointer(WPEEventType type, float x, float y, guint button)
{
    double ox, oy;
    WPEView *view = target_view(x, y, ox, oy);
    if (!view)
        return;
    const double px = x - ox, py = y - oy;
    WPEEvent *event;
    if (type == WPE_EVENT_POINTER_MOVE) {
        const double dx = S.last_x < 0 ? 0 : x - S.last_x, dy = S.last_y < 0 ? 0 : y - S.last_y;
        event = wpe_event_pointer_move_new(type, view, WPE_INPUT_SOURCE_MOUSE, now_ms(),
            modifiers(S.pointer_down ? WPE_MODIFIER_POINTER_BUTTON1 : 0), px, py, dx, dy);
    } else {
        // the click count is for presses only: WPE refuses a release that has one, and gave no event
        // (v00.07: the UI then freed that missing event, and the app closed on the first click)
        const guint count = type == WPE_EVENT_POINTER_DOWN ? wpe_view_compute_press_count(view, px, py, button, now_ms()) : 0;
        const uint32_t mask = button == 1 ? WPE_MODIFIER_POINTER_BUTTON1 : button == 2 ? WPE_MODIFIER_POINTER_BUTTON2 : WPE_MODIFIER_POINTER_BUTTON3;
        // the buttons held after the event, as WPE's own seats give them (WPEWaylandSeat.cpp: a press has its
        // button, a release no longer): WebKit's MouseEvent.buttons and its pointer events come from them.
        // v00.40 and older sent them the other way round, as GDK does: a release still holding its button
        // became a pointermove (WebKit's chorded-button rule), the pointer stayed pressed, every later press
        // was a pointermove too, and pages that click by pointer events (Ruffle's Flash games) saw no click
        event = wpe_event_pointer_button_new(type, view, WPE_INPUT_SOURCE_MOUSE, now_ms(),
            modifiers(type == WPE_EVENT_POINTER_DOWN ? mask : 0), button, px, py, count);
    }
    if (!event)
        return;
    wpe_view_event(view, event);
    wpe_event_unref(event);
}

// dx, dy: positive scrolls right / down (the stick and the D-pad pushed that way, the wheel turned
// toward you). WebKit takes a WPE scroll's deltas as a wheel's (positive: left / up: it does not turn
// them over, WebEventFactoryWPE.cpp), so they go to it turned over (every version before v00.19 scrolled
// the wrong way)
void page_scroll(float x, float y, double dx, double dy)
{
    double ox, oy;
    WPEView *view = target_view(x, y, ox, oy);
    if (!view || (dx == 0 && dy == 0))
        return;
    WPEEvent *event = wpe_event_scroll_new(view, WPE_INPUT_SOURCE_MOUSE, now_ms(), modifiers(), -dx, -dy, TRUE, FALSE, x - ox, y - oy);
    if (!event)
        return;
    wpe_view_event(view, event);
    wpe_event_unref(event);
}

// keycode: the key's X11 hardware code (evdev + 8: WebKit makes KeyboardEvent.code of it), keyval
// its X keysym (KeyboardEvent.key and the text typed); without: modifiers the page should not see
// (AltGr's Alt on a character it typed)
void page_key(guint keycode, guint keyval, bool down, uint32_t without = 0)
{
    Tab *tab = active_tab();
    WPEView *view = S.popup_wpe ? S.popup_wpe : tab ? tab->wpe : nullptr;
    if (!view || !keyval)
        return;
    WPEEvent *event = wpe_event_keyboard_new(down ? WPE_EVENT_KEYBOARD_KEY_DOWN : WPE_EVENT_KEYBOARD_KEY_UP, view,
        WPE_INPUT_SOURCE_KEYBOARD, now_ms(), static_cast<WPEModifiers>(modifiers() & ~without), keycode ? keycode : keyval, keyval);
    if (!event)
        return;
    wpe_view_event(view, event);
    wpe_event_unref(event);
}

// The X11 hardware code of a key (shell/hwkeyboard.cpp gives Windows' set-1 scan codes): Linux's
// evdev codes are the set-1 codes for the plain keys; the E0 keys have their own; + 8 for X11
guint hardware_keycode(const shell::KeyEvent &key)
{
    if (!key.extended) {
        switch (key.scan) {
        case 0x45: return (key.vkey == 0x13 ? 119 : 69) + 8;   // Pause / Num Lock
        case 0x59: return 117 + 8;   // number pad =
        case 0x73: return 89 + 8;    // International1 (Ro)
        case 0x7e: return 121 + 8;   // Brazilian number pad .
        }
        return key.scan && key.scan < 0x59 ? key.scan + 8u : 0;
    }
    switch (key.scan) {
    case 0x1c: return 96 + 8;    // number pad Enter
    case 0x1d: return 97 + 8;    // right Ctrl
    case 0x20: return 113 + 8;   // Mute
    case 0x2e: return 114 + 8;   // Volume Down
    case 0x30: return 115 + 8;   // Volume Up
    case 0x35: return 98 + 8;    // number pad /
    case 0x36: return 54 + 8;    // right Shift (marked extended for Wine)
    case 0x37: return 99 + 8;    // Print Screen
    case 0x38: return 100 + 8;   // right Alt
    case 0x45: return 69 + 8;    // Num Lock (extended for Windows)
    case 0x46: return 119 + 8;   // Ctrl+Pause (Break)
    case 0x47: return 102 + 8;   // Home
    case 0x48: return 103 + 8;   // Up
    case 0x49: return 104 + 8;   // Page Up
    case 0x4b: return 105 + 8;   // Left
    case 0x4d: return 106 + 8;   // Right
    case 0x4f: return 107 + 8;   // End
    case 0x50: return 108 + 8;   // Down
    case 0x51: return 109 + 8;   // Page Down
    case 0x52: return 110 + 8;   // Insert
    case 0x53: return 111 + 8;   // Delete
    case 0x5b: return 125 + 8;   // left Windows
    case 0x5c: return 126 + 8;   // right Windows
    case 0x5d: return 127 + 8;   // Menu
    }
    return 0;
}

// Windows virtual keys (the USB keyboard layer, from WinePS4) to X keysyms (WPE's keyvals), US layout
guint keyval_for(uint16_t vk, bool shift, bool caps)
{
    if (vk >= 'A' && vk <= 'Z')
        return (shift != caps) ? vk : vk + 32;
    if (vk >= '0' && vk <= '9') {
        static const char shifted[] = ")!@#$%^&*(";
        return shift ? static_cast<guint>(shifted[vk - '0']) : vk;
    }
    struct Pair { uint16_t vk; char normal, shifted; };
    static const Pair punct[] = { { 0xBA, ';', ':' }, { 0xBB, '=', '+' }, { 0xBC, ',', '<' }, { 0xBD, '-', '_' },
        { 0xBE, '.', '>' }, { 0xBF, '/', '?' }, { 0xC0, '`', '~' }, { 0xDB, '[', '{' }, { 0xDC, '\\', '|' },
        { 0xDD, ']', '}' }, { 0xDE, '\'', '"' } };
    for (const Pair &p : punct)
        if (p.vk == vk)
            return static_cast<guint>(shift ? p.shifted : p.normal);
    if (vk >= 0x70 && vk <= 0x7B)
        return 0xffbe + (vk - 0x70);              // F1..F12
    if (vk >= 0x60 && vk <= 0x69)
        return 0xffb0 + (vk - 0x60);              // KP_0..KP_9
    switch (vk) {
    case 0x20: return ' ';
    case 0x08: return 0xff08;   // BackSpace
    case 0x09: return shift ? 0xfe20 : 0xff09;   // ISO_Left_Tab / Tab
    case 0x0D: return 0xff0d;   // Return
    case 0x1B: return 0xff1b;   // Escape
    case 0x2E: return 0xffff;   // Delete
    case 0x2D: return 0xff63;   // Insert
    case 0x24: return 0xff50;   // Home
    case 0x23: return 0xff57;   // End
    case 0x21: return 0xff55;   // Page_Up
    case 0x22: return 0xff56;   // Page_Down
    case 0x25: return 0xff51;   // Left
    case 0x26: return 0xff52;   // Up
    case 0x27: return 0xff53;   // Right
    case 0x28: return 0xff54;   // Down
    // (the USB keyboard layer sends the left/right virtual keys, 0xA0-0xA5, never 0x10-0x12)
    case 0x10: case 0xA0: return 0xffe1;   // Shift_L
    case 0xA1: return 0xffe2;              // Shift_R
    case 0x11: case 0xA2: return 0xffe3;   // Control_L
    case 0xA3: return 0xffe4;              // Control_R
    case 0x12: case 0xA4: return 0xffe9;   // Alt_L
    case 0xA5: return 0xffea;              // Alt_R
    case 0x5B: return 0xffeb;   // Super_L
    case 0x14: return 0xffe5;   // Caps_Lock
    case 0x6A: return 0xffaa;   // KP_Multiply
    case 0x6B: return 0xffab;   // KP_Add
    case 0x6D: return 0xffad;   // KP_Subtract
    case 0x6E: return 0xffae;   // KP_Decimal
    case 0x6F: return 0xffaf;   // KP_Divide
    case 0x5D: return 0xff67;   // Menu
    case 0x13: return 0xff13;   // Pause
    case 0x90: return 0xff7f;   // Num_Lock
    case 0x91: return 0xff14;   // Scroll_Lock
    case 0x2C: return 0xff61;   // Print
    }
    return 0;
}

constexpr uint32_t MOD_SHIFT = WPE_MODIFIER_KEYBOARD_SHIFT, MOD_CTRL = WPE_MODIFIER_KEYBOARD_CONTROL,
    MOD_ALT = WPE_MODIFIER_KEYBOARD_ALT, MOD_META = WPE_MODIFIER_KEYBOARD_META;

// The modifiers and Caps Lock, as each key comes (every reader of the keys sees the same)
void update_modifiers(const shell::KeyEvent &key)
{
    int slot = -1;
    switch (key.vkey) {
    case 0xA0: slot = 0; break;   // Shift
    case 0xA1: slot = 1; break;
    case 0xA2: slot = 2; break;   // Ctrl
    case 0xA3: slot = 3; break;
    case 0xA4: slot = 4; break;   // Alt
    case 0xA5: slot = 5; break;
    case 0x5B: slot = 6; break;   // Windows
    case 0x5C: slot = 7; break;
    case 0x14: if (key.down) S.caps = !S.caps; break;   // (locks never repeat: shell/hwkeyboard.cpp)
    }
    if (slot >= 0)
        S.mods_held[slot] = key.down;
    uint32_t m = 0;
    if (S.mods_held[0] || S.mods_held[1]) m |= MOD_SHIFT;
    if (S.mods_held[2] || S.mods_held[3]) m |= MOD_CTRL;
    if (S.mods_held[4] || S.mods_held[5]) m |= MOD_ALT;
    if (S.mods_held[6] || S.mods_held[7]) m |= MOD_META;
    if (S.caps) m |= WPE_MODIFIER_KEYBOARD_CAPS_LOCK;
    S.modifiers = m;
    platform_set_modifiers(m);
}

// Right Alt alone is AltGr on most layouts (German @, Polish ę, ...): its characters are typed, never
// Alt shortcuts
bool altgr() { return S.mods_held[5] && !S.mods_held[4] && !(S.modifiers & MOD_CTRL); }
bool alt_held() { return (S.modifiers & MOD_ALT) && !altgr(); }

// A key's keysym: the character the PS4's own keyboard layout gave it (libSceIme) when typed plainly
// (or with AltGr), else the US layout's (shortcuts take the letter, as a desktop has it)
guint key_keyval(const shell::KeyEvent &key)
{
    const bool plain = !(S.modifiers & (MOD_CTRL | MOD_META)) && !alt_held();
    const bool number_pad = key.vkey >= 0x60 && key.vkey <= 0x6F;
    if (plain && !number_pad && key.ch >= 0x20 && key.ch != 0x7f)
        return wpe_unicode_to_keyval(key.ch);
    return keyval_for(key.vkey, S.modifiers & MOD_SHIFT, S.caps);
}

// --- The address, typed in place (a USB keyboard) ----------------------------------------------------

void edit_start()
{
    // (on Files' tab: Files steps aside while the address is typed, and comes back if it is not used)
    if (files::visible())
        files::hide();
    Tab *tab = active_tab();
    context_close();
    S.overlay = Overlay::None;
    S.editing = true;
    S.edit_text = tab ? tab->uri : "";
    S.edit_caret = S.edit_text.size();
    S.edit_all = true;
    S.addr_for = "\x01";   // (the address bar's list, made afresh)
    S.addr_index = -1;
    S.toolbar_dirty = S.dirty = true;
}

void edit_stop()
{
    if (!S.editing)
        return;
    S.editing = false;
    S.toolbar_dirty = S.dirty = true;
    if (files_tab_active() && !files::visible())
        files::show();
}

size_t utf8_prev(const std::string &s, size_t i)
{
    if (i == 0)
        return 0;
    i--;
    while (i > 0 && (static_cast<unsigned char>(s[i]) & 0xc0) == 0x80)
        i--;
    return i;
}

size_t utf8_next(const std::string &s, size_t i)
{
    if (i >= s.size())
        return s.size();
    i++;
    while (i < s.size() && (static_cast<unsigned char>(s[i]) & 0xc0) == 0x80)
        i++;
    return i;
}

void edit_clear_if_all()
{
    if (S.edit_all) {
        S.edit_text.clear();
        S.edit_caret = 0;
        S.edit_all = false;
    }
}

void edit_insert(std::string text)
{
    text.erase(std::remove_if(text.begin(), text.end(), [](char c) { return c == '\n' || c == '\r' || c == '\t'; }), text.end());
    edit_clear_if_all();
    const size_t at = std::min(S.edit_caret, S.edit_text.size());
    S.edit_text.insert(at, text);
    S.edit_caret = at + text.size();
}

// A key for the address being typed; false: not its key (the browser's shortcuts get it)
bool edit_key(const shell::KeyEvent &key)
{
    if (!key.down)
        return true;
    const bool ctrl = S.modifiers & MOD_CTRL, alt = alt_held();
    switch (key.vkey) {
    case 0x28:   // Down / Up: a row of the list (v00.36), or back to the text typed
    case 0x26: {
        addresses_refresh();
        const int n = static_cast<int>(S.addr_list.size());
        if (n) {
            if (key.vkey == 0x28)
                S.addr_index = S.addr_index + 1 >= n ? -1 : S.addr_index + 1;
            else
                S.addr_index = S.addr_index < 0 ? n - 1 : S.addr_index - 1;
        }
        S.dirty = true;
        return true;
    }
    case 0x0D: {   // Enter: go there (the list's row, when the arrows chose one)
        if (S.addr_index >= 0 && S.addr_index < static_cast<int>(S.addr_list.size())) {
            address_choose(S.addr_index);
            return true;
        }
        const std::string uri = to_uri(S.edit_text);
        edit_stop();
        if (!uri.empty()) {
            if (Tab *tab = active_tab())
                load(tab, uri);
            else
                new_tab(uri, nullptr, true);
        }
        return true;
    }
    case 0x1B:   // Escape (a row the arrows chose goes first)
        if (S.addr_index >= 0) {
            S.addr_index = -1;
            S.dirty = true;
            return true;
        }
        edit_stop();
        return true;
    case 0x08:   // Backspace
        if (S.edit_all)
            edit_clear_if_all();
        else if (S.edit_caret) {
            const size_t p = utf8_prev(S.edit_text, S.edit_caret);
            S.edit_text.erase(p, S.edit_caret - p);
            S.edit_caret = p;
        }
        break;
    case 0x2E:   // Delete
        if (S.edit_all)
            edit_clear_if_all();
        else if (S.edit_caret < S.edit_text.size())
            S.edit_text.erase(S.edit_caret, utf8_next(S.edit_text, S.edit_caret) - S.edit_caret);
        break;
    case 0x25:   // Left
        S.edit_caret = S.edit_all ? 0 : utf8_prev(S.edit_text, S.edit_caret);
        S.edit_all = false;
        break;
    case 0x27:   // Right
        S.edit_caret = S.edit_all ? S.edit_text.size() : utf8_next(S.edit_text, S.edit_caret);
        S.edit_all = false;
        break;
    case 0x24:   // Home
        S.edit_caret = 0;
        S.edit_all = false;
        break;
    case 0x23:   // End
        S.edit_caret = S.edit_text.size();
        S.edit_all = false;
        break;
    case 0x09:   // Tab: stays here
        break;
    default:
        if (ctrl && !alt) {
            switch (key.vkey) {
            case 'A': S.edit_all = true; break;
            case 'C': platform_set_clipboard_text(S.edit_text); break;
            case 'X':
                platform_set_clipboard_text(S.edit_text);
                S.edit_all = true;
                edit_clear_if_all();
                break;
            case 'V': edit_insert(platform_clipboard_text()); break;
            default: return false;
            }
            break;
        }
        {
            const guint32 ch = wpe_keyval_to_unicode(key_keyval(key));
            if (ch < 0x20 || ch == 0x7f || alt)
                return false;
            char utf8[8];
            const int n = g_unichar_to_utf8(ch, utf8);
            edit_insert(std::string(utf8, static_cast<size_t>(n)));
        }
    }
    S.toolbar_dirty = S.dirty = true;
    return true;
}

// --- Zoom (Ctrl +/-/0, Ctrl+wheel) -----------------------------------------------------------------------

void zoom(int step)
{
    Tab *tab = active_tab();
    if (!tab)
        return;
    static const double levels[] = { 0.5, 0.67, 0.75, 0.8, 0.9, 1.0, 1.1, 1.25, 1.5, 1.75, 2.0, 2.5, 3.0 };
    constexpr int count = sizeof levels / sizeof levels[0];
    const double now = webkit_web_view_get_zoom_level(tab->view);
    double next = 1.0;
    if (step > 0) {
        next = levels[count - 1];
        for (double level : levels)
            if (level > now + 0.001) {
                next = level;
                break;
            }
    } else if (step < 0) {
        next = levels[0];
        for (int i = count - 1; i >= 0; i--)
            if (levels[i] < now - 0.001) {
                next = levels[i];
                break;
            }
    }
    webkit_web_view_set_zoom_level(tab->view, next);
    toast("Zoom " + std::to_string(static_cast<int>(std::lround(next * 100))) + "%");
}

// A desktop browser's shortcuts; true: taken (the page does not get the key)
bool shortcut(const shell::KeyEvent &key)
{
    if (!key.down)
        return false;
    const bool ctrl = S.modifiers & MOD_CTRL, shift = S.modifiers & MOD_SHIFT, alt = alt_held();
    Tab *tab = active_tab();
    const int n = static_cast<int>(S.tabs.size());
    if (ctrl && !alt) {
        switch (key.vkey) {
        case 'L': edit_start(); return true;
        case 'T': new_tab(wisp_setting("home", HOME_DEFAULT), nullptr, true); edit_start(); return true;
        case 'W': close_tab(S.active); return true;
        case 'R':
            reload(tab, shift);
            return true;
        case 0x09:   // Ctrl+Tab, Ctrl+Shift+Tab
            if (n > 1)
                activate(shift ? (S.active + n - 1) % n : (S.active + 1) % n);
            return true;
        case 0x21: if (n > 1) activate((S.active + n - 1) % n); return true;   // Ctrl+Page Up
        case 0x22: if (n > 1) activate((S.active + 1) % n); return true;       // Ctrl+Page Down
        case 0xBB: case 0x6B: zoom(+1); return true;   // Ctrl+= (+), number pad +
        case 0xBD: case 0x6D: zoom(-1); return true;   // Ctrl+-
        case '0': case 0x60: zoom(0); return true;
        }
        if (key.vkey >= '1' && key.vkey <= '9') {
            activate(key.vkey == '9' ? n - 1 : std::min(n - 1, key.vkey - '1'));
            return true;
        }
    }
    if (alt && !ctrl) {
        switch (key.vkey) {
        case 0x25: if (tab && webkit_web_view_can_go_back(tab->view)) webkit_web_view_go_back(tab->view); return true;
        case 0x27: if (tab && webkit_web_view_can_go_forward(tab->view)) webkit_web_view_go_forward(tab->view); return true;
        case 0x24: go_home(); return true;
        case 'D': edit_start(); return true;
        }
    }
    if (!ctrl && !alt) {
        switch (key.vkey) {
        case 0x74: reload(tab); return true;                                    // F5
        case 0x75: edit_start(); return true;                                   // F6
        case 0x7A:   // F11 (or out of a page's own fullscreen)
            if (S.page_fullscreen_view)
                leave_page_fullscreen();
            else
                set_fullscreen(!S.fullscreen);
            return true;
        case 0x1B:   // Esc: out of a page's fullscreen; else it stops a load, and the page gets it too
            if (S.page_fullscreen_view) {
                leave_page_fullscreen();
                return true;
            }
            if (tab && tab->loading)
                webkit_web_view_stop_loading(tab->view);
            return false;
        }
    }
    return false;
}

// How a run ended, for the next one to say (/data/Wisp/run/state): "running" from the start, "ended"
// when it leaves or restarts through the menu. Still "running" at a start: the last run was closed
// from the PS4's menu, crashed or stopped.
void run_state(const char *state)
{
    if (FILE *f = fopen(WISP_DATA "/run/state", "w")) {
        fprintf(f, "%s\n", state);
        fclose(f);
    }
}

void check_last_run()
{
    char state[16] = {};
    if (FILE *f = fopen(WISP_DATA "/run/state", "r")) {
        if (!fgets(state, sizeof state, f))
            state[0] = 0;
        fclose(f);
    }
    if (!strncmp(state, "running", 7))
        wisp_log("the last run did not end through the menu: closed from the PS4's menu, or it crashed or stopped (its logs: *.prev.log)");
    run_state("running");
}

// Starts Wisp again, as the GPU setting is read at the start: the web and network processes end first,
// then the system runs eboot.bin anew (WinePS4's restart). Tabs are not kept.
void restart()
{
    wisp_log("restarting (menu)");
    run_state("ended");
    dm::stop();   // (aria2's unfinished downloads into its session file: they go on after the restart)
    wisp_end_all_local();
    fflush(nullptr);
    const int result = wisp_load_exec("/app0/eboot.bin", nullptr);
    run_state("running");
    wisp_log("restart refused (%#x)", static_cast<unsigned>(result));
    toast("OrbisWeb could not restart itself: choose Leave OrbisWeb and start it again");
}

void open_address_keyboard()
{
    Tab *tab = active_tab();
    if (shell::keyboard_active())
        return;
    if (shell::keyboard_open("Search or type an address", tab ? tab->uri : "")) {
        S.keyboard_for_address = true;
        S.keyboard_done = nullptr;
    }
}

// One of OrbisWeb's own pages (About, Settings, History, Bookmarks): its tab when one shows it, else a new one
void open_own_page(const std::string &uri)
{
    for (int i = 0; i < static_cast<int>(S.tabs.size()); i++)
        if (S.tabs[i].view && (S.tabs[i].uri == uri || S.tabs[i].uri.rfind(uri + "?", 0) == 0 || S.tabs[i].uri.rfind(uri + "#", 0) == 0 ||
                               (uri == "orbisweb://about" && is_about_page(S.tabs[i].uri)))) {
            activate(i);
            return;
        }
    new_tab(uri, nullptr, true);
}

// Cookies sites may keep (v00.32, Settings > Privacy; settings: cookies=always|no_third_party|never)
void apply_cookie_policy()
{
    const std::string policy = wisp_setting("cookies", "no_third_party");
    webkit_cookie_manager_set_accept_policy(webkit_network_session_get_cookie_manager(S.session),
        policy == "always" ? WEBKIT_COOKIE_POLICY_ACCEPT_ALWAYS : policy == "never" ? WEBKIT_COOKIE_POLICY_ACCEPT_NEVER
                           : WEBKIT_COOKIE_POLICY_ACCEPT_NO_THIRD_PARTY);
}

// WebAssembly SIMD and relaxed SIMD (v00.32; WebKit's features, on by default): the web process takes
// them as it starts (JavaScriptCore's useWasmSIMD, which it keeps only on a CPU with AVX: its check came
// out without it on the PS4 until WebKit patch 0033 took AVX as present). WebAssembly threads are the web
// process's own environment (wasm_threads, child/child_main.cpp)
void apply_wasm_features()
{
    WebKitFeatureList *features = webkit_settings_get_all_features();
    for (const auto &[id, key] : { std::pair<const char *, const char *>{ "WasmSIMD", "wasm_simd" }, { "WasmRelaxedSIMD", "wasm_relaxed_simd" } }) {
        if (WebKitFeature *feature = webkit_feature_list_find(features, id))
            webkit_settings_set_feature_enabled(S.settings, feature, wisp_setting_int(key, 1) != 0);
        else
            wisp_log("webkit: no feature %s", id);
    }
    webkit_feature_list_unref(features);
}

// (v00.38) The pages' own storage: navigator.storage (persist, estimate) and its origin-private file system
// (getDirectory, file handles, the synchronous access handles Emscripten games keep their files with in a
// worker, writable streams). WebKit has them on Apple's systems only by default; Hot Pursuit 2 and Super
// Mario Sunshine (WebAssembly ports) stopped at "navigator.storage.getDirectory" being undefined. The files
// stay in the network process's website data (/data/Wisp/profile), cleared with the site's data.
// Settings: opfs (Pages and JavaScript). Returns how many of the features WebKit has.
int apply_storage_features()
{
    const bool on = wisp_setting_int("opfs", 1) != 0;
    WebKitFeatureList *features = webkit_settings_get_all_features();
    int found = 0;
    for (const char *id : { "StorageAPI", "StorageAPIEstimate", "FileSystem", "AccessHandle", "FileSystemWritableStream",
             "FileSystemHandleSerialization" }) {
        if (WebKitFeature *feature = webkit_feature_list_find(features, id)) {
            webkit_settings_set_feature_enabled(S.settings, feature, on);
            found++;
        } else {
            wisp_log("webkit: no feature %s", id);
        }
    }
    webkit_feature_list_unref(features);
    return found;
}

// (v00.40) WebGPU for pages: navigator.gpu and canvases' "webgpu" contexts, WebKit's feature (off by default
// off Apple's systems). WebCore's WebGPU on Dawn (WebKit patch 0039) runs in the GPU web process, on the kit's
// RADV beside Zink; a canvas's frames are read back and composited as images. Settings: webgpu (Pages and
// JavaScript). Returns whether WebKit has the feature.
bool apply_webgpu_feature()
{
    WebKitFeatureList *features = webkit_settings_get_all_features();
    WebKitFeature *feature = webkit_feature_list_find(features, "WebGPU");
    const bool found = feature != nullptr;
    if (feature)
        webkit_settings_set_feature_enabled(S.settings, feature, webgpu_on());
    else
        wisp_log("webkit: no feature WebGPU");
    webkit_feature_list_unref(features);
    return found;
}

// A setting the Settings page changed: what applies at once
void apply_setting(const std::string &key)
{
    if (key == "webgl")
        webkit_settings_set_enable_webgl(S.settings, webgl_on());
    else if (key == "console_log")
        webkit_settings_set_enable_write_console_messages_to_stdout(S.settings, wisp_setting_int("console_log", 0));
    else if (key == "smooth_scrolling")
        webkit_settings_set_enable_smooth_scrolling(S.settings, wisp_setting_int("smooth_scrolling", S.gpu_rendering ? 1 : 0) != 0);
    else if (key == "canvas_gpu")
        webkit_settings_set_enable_2d_canvas_acceleration(S.settings, wisp_setting_int("canvas_gpu", 0) != 0);
    else if (key == "cookies")
        apply_cookie_policy();
    else if (key == "wasm_simd" || key == "wasm_relaxed_simd")
        apply_wasm_features();
    else if (key == "opfs")
        apply_storage_features();
    else if (key == "webgpu")
        apply_webgpu_feature();
    else if (key == "gamepad_declined")
        load_gamepad_declined();
    S.dirty = S.toolbar_dirty = true;
}

// The WebGPU self-test (v00.31: WebGPU's first step, docs/WEBGPU.md): the next start's GPU web process runs
// Dawn beside Zink and writes /data/Wisp/run/webgpu-test.txt; a toast gives its result
void webgpu_test_restart()
{
    if (!WISP_HAVE_DAWN) {
        toast("This build of OrbisWeb has no WebGPU test");
        return;
    }
    if (wisp_rendering() == WispRendering::Cpu) {
        toast("The WebGPU test needs the GPU: Settings > Drawing: CPU + GPU or GPU first");
        return;
    }
    wisp_setting_set("webgpu_selftest", "1");
    wisp_log("webgpu test: asked for on the Settings page: restarting");
    restart();
}

void menu_choose(int item)
{
    S.overlay = Overlay::None;
    Tab *tab = active_tab();
    switch (item) {
    case M_NEW_TAB: new_tab(wisp_setting("home", HOME_DEFAULT), nullptr, true); break;
    case M_CLOSE_TAB: close_tab(S.active); break;
    case M_RELOAD: reload(tab); break;
    case M_FULLSCREEN:
        if (S.page_fullscreen_view)
            leave_page_fullscreen();
        else
            set_fullscreen(!S.fullscreen);
        break;
    case M_FILES: open_files_tab(""); break;
    case M_DOWNLOADS: open_downloads_tab(); break;
    case M_EXTENSIONS: S.overlay = Overlay::Extensions; S.ext_index = 0; break;
    case M_BOOKMARK: {
        // the page on show into the bookmarks, or out when it is in them (v00.32)
        const std::string uri = tab ? tab->uri : std::string();
        if (uri.rfind("http://", 0) != 0 && uri.rfind("https://", 0) != 0 && uri.rfind("file://", 0) != 0) {
            toast("Only a web page can be bookmarked");
            break;
        }
        const bool added = pages::toggle_bookmark(uri, tab->title);
        toast(added ? "Bookmarked: " + (tab->title.empty() ? uri : tab->title) + " (OPTIONS > Bookmarks)" : std::string("Removed from the bookmarks"));
        break;
    }
    case M_BOOKMARKS: open_own_page(pages::BOOKMARKS_URI); break;
    case M_HISTORY: open_own_page(pages::HISTORY_URI); break;
    case M_SETTINGS: open_own_page(pages::SETTINGS_URI); break;
    case M_RESTART:
        restart();
        break;
    case M_ABOUT: open_own_page("orbisweb://about"); break;
    case M_LEAVE:
        wisp_log("leaving (menu)");
        run_state("ended");
        dm::stop();
        ps4_kit_exit(0);
    }
    S.dirty = S.toolbar_dirty = true;
}

void toolbar_click(int item)
{
    Tab *tab = active_tab();
    if (item == 500) {
        add_from_store();
    } else if (item >= 400) {
        open_extension_popup(item - 400);
    } else if (item >= 300) {
        new_tab(wisp_setting("home", HOME_DEFAULT), nullptr, true);
    } else if (item >= 200) {
        close_tab(item - 200);
    } else if (item >= 100) {
        activate(item - 100);
    } else {
        // Back, Forward and Reload are a page's; the address, Home and the menu work on every tab, Files',
        // Downloads' and the player's too (through v00.21 nothing on the toolbar answered there: the menu's
        // three dots, an address typed)
        switch (item) {
        case 0: if (tab) webkit_web_view_go_back(tab->view); break;
        case 1: if (tab) webkit_web_view_go_forward(tab->view); break;
        case 2:   // stop while loading (unless the page stopped answering: a reload starts it again)
            if (!tab)
                break;
            if (tab->loading && !web_hung())
                webkit_web_view_stop_loading(tab->view);
            else
                reload(tab);
            break;
        case 3:   // (no page here: Home in a new tab)
            if (tab)
                go_home();
            else
                new_tab(wisp_setting("home", HOME_DEFAULT), nullptr, true);
            break;
        case 4:   // typed in place with a USB keyboard (the list under it), else the list of addresses used
                  // before, whose first row opens the system keyboard (v00.36; no page: a new tab)
            if (shell::hwkeyboard_connected())
                edit_start();
            else
                open_addresses();
            break;
        case 5: S.overlay = S.overlay == Overlay::Menu ? Overlay::None : Overlay::Menu; S.menu_index = 0; break;
        }
    }
    S.dirty = S.toolbar_dirty = true;
}

int menu_item_at(float x, float y)
{
    const int x0 = W - MENU_W - 16, y0 = TOOLBAR_H - 4 + 12;
    if (x < x0 || x >= x0 + MENU_W || y < y0)
        return -1;
    const int i = static_cast<int>(y - y0) / MENU_ROW;
    return i < MENU_COUNT ? i : -1;
}

// A list's keys on the USB keyboard (the OPTIONS menu, the page's menu, Extensions)
enum ListKey { LIST_NONE, LIST_UP, LIST_DOWN, LIST_TAKE, LIST_CLOSE };

ListKey list_key(const std::vector<shell::KeyEvent> &keys)
{
    ListKey result = LIST_NONE;
    for (const shell::KeyEvent &key : keys) {
        update_modifiers(key);
        if (!key.down)
            continue;
        switch (key.vkey) {
        case 0x26: result = LIST_UP; break;
        case 0x28: result = LIST_DOWN; break;
        case 0x0D: case 0x20: result = LIST_TAKE; break;
        case 0x1B: result = LIST_CLOSE; break;
        }
    }
    return result;
}

void handle_input(const shell::Input &in, float dt)
{
    S.pointer_x = in.x;
    S.pointer_y = in.y;
    if (in.moved)
        S.pointer_moved_at = wisp_seconds();
    // the system keyboard dialog owns the controller while it is up
    if (shell::keyboard_active()) {
        std::string text;
        const int result = shell::keyboard_poll(text);
        if (result) {
            shell::hwkeyboard_dialog_closed();
            if (S.keyboard_for_address) {
                S.keyboard_for_address = false;
                if (result > 0) {
                    const std::string uri = to_uri(text);
                    if (Tab *tab = active_tab())
                        load(tab, uri);
                    else if (!uri.empty())   // (typed on the Files tab: a new tab)
                        new_tab(uri, nullptr, true);
                }
            } else if (S.keyboard_done) {
                auto done = std::move(S.keyboard_done);
                S.keyboard_done = nullptr;
                done(result > 0, text);
            }
            S.dirty = true;
        }
        return;
    }

    static int last_hover = -2;
    const int hover = toolbar_item(in.x, in.y);
    if (hover != last_hover) {
        last_hover = hover;
        S.toolbar_dirty = S.dirty = true;
    }
    if (in.moved)
        S.dirty = true;

    // a button let go reaches the page it went down on, whatever opened since
    if ((in.released & shell::BTN_CLICK) && S.pointer_down) {
        S.pointer_down = false;
        page_pointer(WPE_EVENT_POINTER_UP, in.x, in.y, 1);
    }
    if ((in.released & shell::BTN_MOUSE_RIGHT) && S.right_down) {
        S.right_down = false;
        page_pointer(WPE_EVENT_POINTER_UP, in.x, in.y, 3);
    }
    if ((in.released & shell::BTN_MOUSE_MIDDLE) && S.middle_down) {
        S.middle_down = false;
        page_pointer(WPE_EVENT_POINTER_UP, in.x, in.y, 2);
    }

    // the USB keyboard's keys of this frame (Files, a list, the address field or the page gets them)
    const std::vector<shell::KeyEvent> keys = shell::hwkeyboard_poll(dt);

    // Files (WinePS4's file manager, in its own tab) has the controller while its tab is on show, below
    // the toolbar; the toolbar, OPTIONS, L1 and R1 stay the browser's
    if (files::visible() && S.overlay == Overlay::None) {
        for (const shell::KeyEvent &key : keys)
            update_modifiers(key);
        const bool on_toolbar = in.y < TOOLBAR_H && !S.fullscreen;
        const bool used = !(on_toolbar && (in.pressed & shell::BTN_CLICK)) && files::input(in, keys, 0, content_y(), W, content_h(), dt);
        if (!files::visible() && files_tab_active()) {
            // Files closed itself (Circle at This PS4, its Close button, Ctrl+W): its tab goes
            close_tab(S.active);
            return;
        }
        if (used) {
            S.dirty = true;
            return;
        }
        if (on_toolbar && (in.pressed & shell::BTN_CLICK)) {
            toolbar_click(hover);
            return;
        }
        if (in.pressed & shell::BTN_START) {
            S.overlay = Overlay::Menu;
            S.menu_index = 0;
            S.dirty = true;
        }
        if ((in.pressed & shell::BTN_PREV) && S.tabs.size() > 1)
            activate((S.active + static_cast<int>(S.tabs.size()) - 1) % static_cast<int>(S.tabs.size()));
        if ((in.pressed & shell::BTN_NEXT) && S.tabs.size() > 1)
            activate((S.active + 1) % static_cast<int>(S.tabs.size()));
        return;
    }

    // the player (its tab): the browser's shortcuts first (F11 is its full screen too), the rest of the
    // keys and the controller to the player; the toolbar, OPTIONS, L1 and R1 stay the browser's
    if (player::visible() && S.overlay == Overlay::None && !S.editing) {
        std::vector<shell::KeyEvent> rest;
        for (const shell::KeyEvent &key : keys) {
            update_modifiers(key);
            if (!shortcut(key))
                rest.push_back(key);
        }
        if (!player::visible())
            return;
        const bool on_toolbar = in.y < content_y();
        if (on_toolbar && (in.pressed & shell::BTN_CLICK)) {
            toolbar_click(hover);
            return;
        }
        if (in.pressed & shell::BTN_START) {
            S.overlay = Overlay::Menu;
            S.menu_index = 0;
            S.dirty = true;
            return;
        }
        if ((in.pressed & shell::BTN_PREV) && S.tabs.size() > 1) {
            activate((S.active + static_cast<int>(S.tabs.size()) - 1) % static_cast<int>(S.tabs.size()));
            return;
        }
        if ((in.pressed & shell::BTN_NEXT) && S.tabs.size() > 1) {
            activate((S.active + 1) % static_cast<int>(S.tabs.size()));
            return;
        }
        if (player::input(in, rest, 0, content_y(), W, content_h(), dt))
            S.dirty = true;
        return;
    }

    // the download manager's page (its tab): the browser's shortcuts first, the rest of the keys and the
    // controller to the page; the toolbar, OPTIONS, L1 and R1 stay the browser's
    if (dm::visible() && S.overlay == Overlay::None && !S.editing) {
        std::vector<shell::KeyEvent> rest;
        for (const shell::KeyEvent &key : keys) {
            update_modifiers(key);
            if (!shortcut(key))
                rest.push_back(key);
        }
        if (!dm::visible())   // (a shortcut went elsewhere: another tab, the address)
            return;
        const bool on_toolbar = in.y < TOOLBAR_H && !S.fullscreen;
        if (!(on_toolbar && (in.pressed & shell::BTN_CLICK)) && dm::input(in, rest, 0, content_y(), W, content_h(), dt)) {
            S.dirty = true;
            return;
        }
        if (on_toolbar && (in.pressed & shell::BTN_CLICK)) {
            toolbar_click(hover);
            return;
        }
        if (in.pressed & shell::BTN_START) {
            S.overlay = Overlay::Menu;
            S.menu_index = 0;
            S.dirty = true;
        }
        if ((in.pressed & shell::BTN_PREV) && S.tabs.size() > 1)
            activate((S.active + static_cast<int>(S.tabs.size()) - 1) % static_cast<int>(S.tabs.size()));
        if ((in.pressed & shell::BTN_NEXT) && S.tabs.size() > 1)
            activate((S.active + 1) % static_cast<int>(S.tabs.size()));
        return;
    }

    // the menus and lists take the controller (and the keyboard's arrows, Enter and Esc) while open
    if (S.overlay != Overlay::None) {
        const ListKey lkey = list_key(keys);
        const bool up = (in.repeat & shell::BTN_UP) || lkey == LIST_UP;
        const bool down = (in.repeat & shell::BTN_DOWN) || lkey == LIST_DOWN;
        const bool close = (in.pressed & (shell::BTN_BACK | shell::BTN_START)) || lkey == LIST_CLOSE;
        if (S.overlay == Overlay::Context) {
            // the page's menu: a click on an item takes it, a click (or a right click) anywhere else closes it
            const int at = context_item_at(in.x, in.y);
            const int n = static_cast<int>(S.ctx_items.size());
            if (in.moved && at != S.ctx_index)
                S.ctx_index = at, S.dirty = true;
            if (up && n)
                S.ctx_index = S.ctx_index <= 0 ? n - 1 : S.ctx_index - 1, S.dirty = true;
            if (down && n)
                S.ctx_index = (S.ctx_index + 1) % n, S.dirty = true;
            if (close || ((in.pressed & (shell::BTN_CLICK | shell::BTN_MOUSE_RIGHT)) && at < 0))
                context_close();
            else if (in.pressed & shell::BTN_CLICK)
                context_activate(at);
            else if (lkey == LIST_TAKE)
                context_activate(S.ctx_index);
            return;
        }
        if (close)
            S.overlay = Overlay::None, S.dirty = true;
        else if (S.overlay == Overlay::Addresses) {
            const int n = addresses_count();
            const int at = address_row_at(in.x, in.y);
            if (in.moved && at >= 0 && at != S.addr_index)
                S.addr_index = at, S.dirty = true;
            if (up && n)
                S.addr_index = S.addr_index <= 0 ? n - 1 : S.addr_index - 1, S.dirty = true;
            if (down && n)
                S.addr_index = (S.addr_index + 1) % n, S.dirty = true;
            if (in.pressed & shell::BTN_SWITCH) {
                S.overlay = Overlay::None, S.dirty = true;
                open_address_keyboard();
            } else if (in.pressed & shell::BTN_CLICK) {
                if (at >= 0)
                    address_choose(at);
                else if (hover != 4)   // (a click away from the list closes it; on the field, it takes the row)
                    S.overlay = Overlay::None, S.dirty = true;
                else
                    address_choose(S.addr_index);
            } else if (lkey == LIST_TAKE)
                address_choose(S.addr_index);
        } else if (S.overlay == Overlay::Menu) {
            const int at = menu_item_at(in.x, in.y);
            if (in.moved && at >= 0 && at != S.menu_index)
                S.menu_index = at, S.dirty = true;
            if (up)
                S.menu_index = (S.menu_index + MENU_COUNT - 1) % MENU_COUNT, S.dirty = true;
            if (down)
                S.menu_index = (S.menu_index + 1) % MENU_COUNT, S.dirty = true;
            if (in.pressed & shell::BTN_CLICK)
                menu_choose(at >= 0 ? at : (hover == 5 ? -1 : S.menu_index));
            else if (lkey == LIST_TAKE)
                menu_choose(S.menu_index);
        } else if (S.overlay == Overlay::Extensions) {
            const std::vector<ext::Info> list = ext::list();
            const int n = static_cast<int>(list.size());
            const int y0 = TOOLBAR_H + 40 + 150, x0 = (W - EXT_LIST_W) / 2;
            const int at = in.x >= x0 && in.x < x0 + EXT_LIST_W && in.y >= y0 ? static_cast<int>(in.y - y0) / EXT_ROW : -1;
            if (in.moved && at >= 0 && at < n && at != S.ext_index)
                S.ext_index = at, S.dirty = true;
            if (up && S.ext_index > 0)
                S.ext_index--, S.dirty = true;
            if (down && S.ext_index + 1 < n)
                S.ext_index++, S.dirty = true;
            if (S.ext_index < n) {
                const ext::Info &e = list[S.ext_index];
                if ((in.pressed & shell::BTN_CLICK) || lkey == LIST_TAKE) {
                    ext::set_enabled(e.id, !e.enabled);
                    toast(e.name + (e.enabled ? " turned off" : " turned on"));
                    S.ext_buttons_dirty = S.dirty = true;
                } else if ((in.pressed & shell::BTN_MENU) && !(in.pressed & shell::BTN_MOUSE_RIGHT)) {   // Square only
                    ext::uninstall(e.id);
                    toast(e.name + " removed");
                    S.ext_buttons_dirty = S.dirty = true;
                } else if ((in.pressed & shell::BTN_SWITCH) && !e.options_page.empty()) {
                    S.overlay = Overlay::None;
                    new_tab("chrome-extension://" + e.id + "/" + e.options_page, nullptr, true);
                }
            }
        } else if (in.pressed & shell::BTN_CLICK)
            S.overlay = Overlay::None, S.dirty = true;
        return;
    }

    // an extension's popup: Circle or a click outside closes it
    if (S.popup && ((in.pressed & shell::BTN_BACK) || ((in.pressed & shell::BTN_CLICK) && !in_popup(in.x, in.y) && hover < 400))) {
        close_popup();
        if (in.pressed & shell::BTN_BACK)
            return;
    }

    Tab *tab = active_tab();
    const bool over_page = in.y >= content_y() || in_popup(in.x, in.y);
    if (in.moved && over_page) {
        page_pointer(WPE_EVENT_POINTER_MOVE, in.x, in.y, 0);
        S.last_x = in.x;
        S.last_y = in.y;
    }
    // a row of the address bar's list (v00.36): over it the pointer chooses it, a click goes there
    if (S.editing && in.moved) {
        const int at = address_row_at(in.x, in.y);
        if (at >= 0 && at != S.addr_index)
            S.addr_index = at, S.dirty = true;
    }
    if (S.editing && (in.pressed & shell::BTN_CLICK)) {
        const int at = address_row_at(in.x, in.y);
        if (at >= 0) {
            address_choose(at);
            return;
        }
    }
    // a click away from the address being typed ends the typing
    if (S.editing && (in.pressed & (shell::BTN_CLICK | shell::BTN_MOUSE_RIGHT | shell::BTN_MOUSE_MIDDLE)) && hover != 4)
        edit_stop();
    if (in.pressed & shell::BTN_CLICK) {
        if (!over_page)
            toolbar_click(hover);
        else if (tab && tab->crashed) {
            if (!S.web_restarting) {   // (a restart reloads its tab itself, then Cross the others)
                tab->crashed = false;
                webkit_web_view_reload(tab->view);
            }
        } else {
            S.pointer_down = true;
            page_pointer(WPE_EVENT_POINTER_DOWN, in.x, in.y, 1);
        }
    }
    // the mouse's right button: the page's own menu (WebKit asks for it: on_context_menu); its middle
    // button: a link in a new tab, as on a computer
    if ((in.pressed & shell::BTN_MOUSE_RIGHT) && over_page) {
        S.right_down = true;
        page_pointer(WPE_EVENT_POINTER_DOWN, in.x, in.y, 3);
    }
    if ((in.pressed & shell::BTN_MOUSE_MIDDLE) && over_page) {
        S.middle_down = true;
        page_pointer(WPE_EVENT_POINTER_DOWN, in.x, in.y, 2);
    }
    if ((in.pressed & shell::BTN_MENU) && !(in.held & shell::BTN_MOUSE_RIGHT) && over_page) {   // Square: middle click
        page_pointer(WPE_EVENT_POINTER_DOWN, in.x, in.y, 2);
        page_pointer(WPE_EVENT_POINTER_UP, in.x, in.y, 2);
    }
    if ((in.pressed & shell::BTN_MOUSE_BACK) && tab && webkit_web_view_can_go_back(tab->view))
        webkit_web_view_go_back(tab->view);
    if ((in.pressed & shell::BTN_MOUSE_FORWARD) && tab && webkit_web_view_can_go_forward(tab->view))
        webkit_web_view_go_forward(tab->view);
    // Circle: Back, or out of a page's fullscreen (v00.36)
    if ((in.pressed & shell::BTN_BACK) && S.page_fullscreen_view)
        leave_page_fullscreen();
    else if (in.pressed & shell::BTN_BACK && tab && webkit_web_view_can_go_back(tab->view))
        webkit_web_view_go_back(tab->view);
    if (in.pressed & shell::BTN_SWITCH)
        open_address_keyboard();
    if (in.pressed & shell::BTN_START) {
        S.overlay = Overlay::Menu;
        S.menu_index = 0;
        S.dirty = true;
    }
    if ((in.pressed & shell::BTN_PREV) && S.tabs.size() > 1)
        activate((S.active + static_cast<int>(S.tabs.size()) - 1) % static_cast<int>(S.tabs.size()));
    if ((in.pressed & shell::BTN_NEXT) && S.tabs.size() > 1)
        activate((S.active + 1) % static_cast<int>(S.tabs.size()));
    if (in.pressed & shell::BTN_LEFT && tab && webkit_web_view_can_go_back(tab->view))
        webkit_web_view_go_back(tab->view);
    if (in.pressed & shell::BTN_RIGHT && tab && webkit_web_view_can_go_forward(tab->view))
        webkit_web_view_go_forward(tab->view);

    // scrolling: the right stick and the wheel (lines; Shift+wheel sideways, Ctrl+wheel zooms), the
    // wheel's tilt (columns), the D-pad (a third of the screen per press)
    double scroll = in.scroll * 48.0, scroll_x = in.scroll_x * 48.0;
    if ((S.modifiers & MOD_CTRL) && in.scroll != 0 && in.mouse) {
        zoom(in.scroll < 0 ? +1 : -1);
        scroll = 0;
    } else if ((S.modifiers & MOD_SHIFT) && scroll_x == 0) {
        scroll_x = scroll;
        scroll = 0;
    }
    if (in.repeat & shell::BTN_DOWN)
        scroll += content_h() / 3.0;
    if (in.repeat & shell::BTN_UP)
        scroll -= content_h() / 3.0;
    if ((scroll != 0 || scroll_x != 0) && over_page)
        page_scroll(in.x, in.y, scroll_x, scroll);
    else if (scroll != 0 || scroll_x != 0)
        page_scroll(W / 2.f, content_y() + content_h() / 2.f, scroll_x, scroll);

    // the USB keyboard: the address being typed, then the browser's shortcuts, then the page
    for (const shell::KeyEvent &key : keys) {
        update_modifiers(key);
        if (S.editing && edit_key(key))
            continue;
        if (shortcut(key) || S.editing)
            continue;
        const guint keyval = key_keyval(key);
        page_key(hardware_keycode(key), keyval, key.down, altgr() && wpe_keyval_to_unicode(keyval) >= 0x20 ? MOD_ALT : 0);
    }
}

gboolean frame(gpointer)
{
    const double t = wisp_seconds();
    const float dt = S.last_frame ? static_cast<float>(t - S.last_frame) : 1.f / 60;
    S.last_frame = t;
    const shell::Input &in = shell::input_poll(dt);
    S.last_input = in;
    gamepad_frame();
    webgpu_test_check();
    handle_input(in, dt);
    hung_page_check();
    sleep_check();
    meter_update();
    if (S.ext_buttons_dirty)
        refresh_ext_buttons();
    Tab *tab = active_tab();
    Frame frame;
    if (tab && platform_frame(tab->wpe, frame) && frame.serial != S.shown_serial)
        S.dirty = true;
    if (S.popup_wpe)
        S.dirty = true;   // (popups are small: redrawn every frame while open)
    if (files::visible() && files::dirty())
        S.dirty = true;
    if (dm::dirty())   // (every frame: the engine's news, its toasts, go out from here)
        S.dirty = true;
    if (player::dirty())   // (every frame: a picture whose time has come, the end of a file)
        S.dirty = true;
    if (!S.toast.empty() && t >= S.toast_until) {
        S.toast.clear();
        S.dirty = true;
    }
    // (a flip still waiting for its vblank: the next vblank composes this)
    if ((S.dirty || S.toolbar_dirty) && !shell::screen_flip_pending())
        compose(in);
    return G_SOURCE_CONTINUE;
}

// The frame source: ready at every vblank (input, then a compose when something changed), and when a
// new page frame came between two vblanks and the screen is free (composed at once: shown at the next
// vblank instead of the one after). The vblank thread only counts and wakes the main loop.
std::atomic<uint32_t> g_vblanks{0};

void on_vblank()
{
    g_vblanks.fetch_add(1, std::memory_order_release);
    g_main_context_wakeup(nullptr);
}

// (no vblank for 50 ms: a frame anyway, so the input never stops if the vblank events do)
bool frame_overdue() { return wisp_seconds() - S.last_frame > 0.05; }

bool frame_source_ready()
{
    if (g_vblanks.load(std::memory_order_acquire) != S.vblanks_seen || frame_overdue())
        return true;
    return S.compose_soon && (S.dirty || S.toolbar_dirty) && !shell::screen_flip_pending();
}

gboolean frame_prepare(GSource *, gint *timeout)
{
    *timeout = 20;
    return frame_source_ready();
}

gboolean frame_check(GSource *) { return frame_source_ready(); }

gboolean frame_dispatch(GSource *, GSourceFunc, gpointer)
{
    const uint32_t vblanks = g_vblanks.load(std::memory_order_acquire);
    S.compose_soon = false;
    if (vblanks != S.vblanks_seen || frame_overdue()) {
        // once, after the first seconds: how often the TV's vblanks came (from the vblank thread's start)
        static bool told = false;
        if (!told && S.vsync && wisp_seconds() - S.vsync_start > 10) {
            told = true;
            const double seconds = wisp_seconds() - S.vsync_start;
            wisp_log("screen: %u vblanks in %.1f s (%.1f a second)", vblanks, seconds, vblanks / seconds);
        }
        S.vblanks_seen = vblanks;
        frame(nullptr);
    } else if ((S.dirty || S.toolbar_dirty) && !shell::screen_flip_pending()) {
        compose(S.last_input);
    }
    return G_SOURCE_CONTINUE;
}

GSourceFuncs frame_source_funcs = { frame_prepare, frame_check, frame_dispatch, nullptr, nullptr, nullptr };

// a new frame of the page, a pointer shape: compose soon
void request_redraw()
{
    S.dirty = true;
    if (S.vsync)
        S.compose_soon = true;
}

void setup_webkit()
{
    // WebKit's IPC lines in ui.log (WebKit patch 0019: its sockets' room, messages that wait for room on
    // a busy socket, a message lost); settings: ipc_log=0 turns them off (the web and network processes
    // take the setting in child/child_main.cpp)
    setenv("WISP_IPC_LOG", wisp_setting_int("ipc_log", 1) ? "1" : "0", 1);
    mkdir(WISP_PROFILE, 0777);
    mkdir(WISP_PROFILE "/data", 0777);
    mkdir(WISP_PROFILE "/cache", 0777);
    mkdir(WISP_DOWNLOADS, 0777);
    process_manager_install();
    platform_display();

    S.session = webkit_network_session_new(WISP_PROFILE "/data", WISP_PROFILE "/cache");
    webkit_cookie_manager_set_persistent_storage(webkit_network_session_get_cookie_manager(S.session),
        WISP_PROFILE "/data/cookies.sqlite", WEBKIT_COOKIE_PERSISTENT_STORAGE_SQLITE);
    g_signal_connect(S.session, "download-started", G_CALLBACK(on_download_started), nullptr);

    // WebKit's memory monitor in the web process (WebKit patch 0025, v00.25: it never ran on the PS4, and closed tabs'
    // memory stayed): past 40% of memory_limit (the web process's heap, 1536 MiB) its caches let go of what no page
    // uses; past 60% it also collects garbage and drops compiled code and the back/forward cache. Checked every 15 s;
    // never killed (WebKit's own kill threshold would end the one process every tab shares).
    WebKitMemoryPressureSettings *pressure = webkit_memory_pressure_settings_new();
    webkit_memory_pressure_settings_set_memory_limit(pressure, static_cast<guint>(std::clamp(wisp_setting_int("memory_limit", 1536), 256, 1536)));
    webkit_memory_pressure_settings_set_conservative_threshold(pressure, 0.4);
    webkit_memory_pressure_settings_set_strict_threshold(pressure, 0.6);
    webkit_memory_pressure_settings_set_kill_threshold(pressure, 10.0);
    webkit_memory_pressure_settings_set_poll_interval(pressure, 15.0);
    S.context = WEBKIT_WEB_CONTEXT(g_object_new(WEBKIT_TYPE_WEB_CONTEXT, "memory-pressure-settings", pressure, nullptr));
    webkit_memory_pressure_settings_free(pressure);
    webkit_web_context_set_cache_model(S.context, WEBKIT_CACHE_MODEL_WEB_BROWSER);
    // OrbisWeb's own pages (orbisweb://about; wisp://about from v00.24's sessions and bookmarks)
    webkit_web_context_register_uri_scheme(S.context, "orbisweb", serve_wisp_page, nullptr, nullptr);
    webkit_web_context_register_uri_scheme(S.context, "wisp", serve_wisp_page, nullptr, nullptr);
    const char *const languages[] = { "en-US", "en", nullptr };
    webkit_web_context_set_preferred_languages(S.context, languages);

    // the last run's automatic fallbacks (GPU drawing, the JIT, the concurrent collector) back to their
    // defaults before anything reads them
    restore_fallbacks();
    // CPU rendering (Skia into shared memory, no GL) or composited with the GPU (CPU + GPU, GPU):
    // WebKitSettings reads WISP_CPU_RENDERING as it is made (patches/webkit: the policy setting is
    // GTK-only API)
    const WispRendering rendering = wisp_rendering();
    const bool gpu = rendering != WispRendering::Cpu;
    S.gpu_rendering = gpu;
    setenv("WISP_CPU_RENDERING", gpu ? "0" : "1", 1);
    S.settings = webkit_settings_new();
    // the developer extras (the inspector's hooks) only with the remote inspector on (settings:
    // inspector=1): with them on, pages' console calls are kept and formatted, and a site's challenge
    // (Cloudflare's Turnstile logs objects with invisible styles to see whether a console is open)
    // takes the page for one being debugged
    webkit_settings_set_enable_developer_extras(S.settings, wisp_setting_int("inspector", 0) != 0);
    // smooth scrolling animates each scroll over several frames: each of them is a whole new page
    // picture on the CPU, so off there unless asked for (settings: smooth_scrolling)
    webkit_settings_set_enable_smooth_scrolling(S.settings, wisp_setting_int("smooth_scrolling", gpu ? 1 : 0) != 0);
    webkit_settings_set_javascript_can_open_windows_automatically(S.settings, FALSE);
    // Safari's user agent: sites send WebKit browsers their Safari pages (the default names Linux/X11)
    webkit_settings_set_user_agent(S.settings, wisp_setting("user-agent",
        "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/18.6 Safari/605.1.15"));
    // the pages' console messages (script errors among them) into web.log (menu: Page messages in the
    // log): off unless asked for since v00.16. Each message's objects are turned into text on the page's
    // thread, which Cloudflare's challenge counts as a console being open (and it logged hundreds)
    webkit_settings_set_enable_write_console_messages_to_stdout(S.settings, wisp_setting_int("console_log", 0));
    // canvases drawn on the CPU (settings: canvas_gpu=1 for the GPU): an accelerated 2D canvas draws with
    // Skia on GL from the web process's main thread, and Zink fell over in zink_resource_memory_barrier
    // there (v00.18: the web process ended twice on pages whose scripts draw canvases, Google's and
    // sites' bot checks among them)
    webkit_settings_set_enable_2d_canvas_acceleration(S.settings, wisp_setting_int("canvas_gpu", 0) != 0);
    // WebGL 1 and 2 (v00.30): ANGLE in the GPU web process (CPU + GPU and GPU rendering), on its Mesa
    // (Zink on RADV; WebKit patch 0029); the CPU web process has no GL and turns WebGL off itself
    // (patch 0016). Menu: WebGL (settings: webgl)
    webkit_settings_set_enable_webgl(S.settings, webgl_on());
    // WebGPU (v00.40): Dawn in the GPU web process (WebKit patch 0039); the CPU web process has none and the
    // feature stays off there. Settings: webgpu
    const bool webgpu_feature = apply_webgpu_feature();
    // the WebGPU self-test asked for before this start (menu: WebGPU test, v00.31): the first GPU web
    // process runs it (WispStart flag), this once; webgpu_test_check toasts its result
    if (wisp_setting_int("webgpu_selftest", 0)) {
        wisp_setting_set("webgpu_selftest", "0");
        if (gpu && WISP_HAVE_DAWN) {
            unlink(WEBGPU_TEST_FILE);
            wisp_set_next_web_flags(WISP_CHILD_WEBGPU_SELFTEST);
            S.webgpu_test_since = wisp_seconds();
            wisp_log("webgpu test: asked for: the web process runs it 5 s after it starts");
        }
    }
    // no service workers or shared workers (settings: service_workers=1 brings them back): WebKit runs a
    // site's workers in a web process of that site, and a page process running workers is not given new
    // pages, while the PS4 cannot start a second web process (no room in the shared memory pool for its
    // program image: v00.20 ended at YouTube, whose COOP header asked WebKit for one). Pages go on
    // without them, as in private browsing.
    // (A feature's identifier is its preference's name without "Enabled": v00.21 looked for
    // "ServiceWorkersEnabled", found nothing, and YouTube's service worker asked for a process of its
    // own 225 times in 200 s.)
    if (!wisp_setting_int("service_workers", 0)) {
        WebKitFeatureList *features = webkit_settings_get_all_features();
        int found = 0;
        for (const char *id : { "ServiceWorkers", "SharedWorker" }) {
            if (WebKitFeature *feature = webkit_feature_list_find(features, id)) {
                webkit_settings_set_feature_enabled(S.settings, feature, FALSE);
                found++;
            } else {
                wisp_log("webkit: no feature %s: pages may still start workers in processes of their own", id);
            }
        }
        webkit_feature_list_unref(features);
        wisp_log("webkit: service workers and shared workers off (%d of 2 features; settings: service_workers=1)", found);
    }
    // WebAssembly SIMD and relaxed SIMD (v00.32; Settings > Pages and JavaScript)
    apply_wasm_features();
    // the pages' own storage and file system (v00.38; Settings > Pages and JavaScript: opfs)
    {
        const int storage = apply_storage_features();
        wisp_log("webkit: the pages' storage (navigator.storage, its file system, access handles) %s (%d of 6 features); "
            "COEP credentialless isolates %s; failed page loads in web.log %s",
            wisp_setting_int("opfs", 1) ? "on" : "off", storage, wisp_setting_int("coep_credentialless", 1) ? "on" : "off",
            wisp_setting_int("load_log", 1) ? "on" : "off");
    }
    wisp_log("webkit: WebAssembly SIMD %s, relaxed SIMD %s (AVX taken as present: WebKit patch 0033), threads and SharedArrayBuffer %s",
        wisp_setting_int("wasm_simd", 1) ? "on" : "off", wisp_setting_int("wasm_relaxed_simd", 1) ? "on" : "off",
        wisp_setting_int("wasm_threads", 1) ? "on" : "off");
    // (the web process says what runs JavaScript when it starts: web.log's "JavaScriptCore:" line)
    wisp_log("webkit: WPE WebKit %u.%u.%u, %s rendering, JavaScript by %s, user agent %s",
        webkit_get_major_version(), webkit_get_minor_version(), webkit_get_micro_version(),
        rendering == WispRendering::Hybrid ? "CPU + GPU (tiles drawn by Skia on the CPU, composited on Zink: wisp-web-gpu.bin)"
            : rendering == WispRendering::Gpu ? "GPU (Skia and compositing on Zink: wisp-web-gpu.bin)" : "CPU (Skia)",
        !WISP_HAVE_JIT ? "the LLInt interpreter (a web process built without the JIT)"
            : wisp_setting_int("jit", 1) ? "the JIT where this console runs generated code (see web.log)"
            : "the LLInt interpreter (JavaScript JIT off in Settings)",
        webkit_settings_get_user_agent(S.settings));
    wisp_log("webkit: WebGL 1 and 2 %s", !WISP_HAVE_WEBGL ? "not in this build" : !gpu ? "off (CPU rendering: no GL in the web process)"
        : webgl_on() ? "on (ANGLE on Zink in the GPU web process)" : "off (Settings: WebGL)");
    wisp_log("webkit: WebGPU %s", !WISP_HAVE_WEBGPU ? "not in this build" : !webgpu_feature ? "missing from WebKit's features"
        : !gpu ? "off (CPU rendering: no GPU in the web process)" : webgpu_on() ? "on (Dawn on RADV in the GPU web process)"
        : "off (Settings: WebGPU)");

    platform_set_redraw(request_redraw);
    platform_set_text_request([](const std::string &initial, bool password, bool, std::function<void(bool, const std::string &)> done) {
        // with a USB keyboard the field is typed into on the page itself: no system keyboard over it
        if (shell::hwkeyboard_connected()) {
            done(false, {});
            return;
        }
        if (shell::keyboard_active() || !shell::keyboard_open(password ? "Password" : "Text", initial, password)) {
            done(false, {});
            return;
        }
        S.keyboard_for_address = false;
        S.keyboard_done = std::move(done);
    });

    // extensions: what they need from the browser (tabs by their stable ids)
    ext::Browser hooks;
    hooks.context = S.context;
    hooks.session = S.session;
    hooks.make_view = make_extension_view;
    hooks.tab_ids = [] {
        // (web pages only: not Files, nor a sleeping tab, which extensions were told is gone)
        std::vector<int> ids;
        for (auto &t : S.tabs)
            if (t.view)
                ids.push_back(t.id);
        return ids;
    };
    hooks.tab_view = [](int id) -> WebKitWebView * {
        const int i = index_of_id(id);
        return i < 0 ? nullptr : S.tabs[i].view;
    };
    hooks.active_tab = [] { return active_tab() ? active_tab()->id : -1; };
    hooks.open_tab = [](const std::string &url, bool active) {
        WebKitWebView *view = new_tab(url, nullptr, active);
        const int i = tab_index(view);
        return i < 0 ? -1 : S.tabs[i].id;
    };
    hooks.close_tab = [](int id) { close_tab(index_of_id(id)); };
    hooks.activate_tab = [](int id) { activate(index_of_id(id)); };
    hooks.toast = [](const std::string &text) { toast(text); };
    hooks.redraw = [] { S.ext_buttons_dirty = S.toolbar_dirty = S.dirty = true; };
    hooks.page_scripts = add_page_scripts;
    ext::init(hooks);

    // Files: a file opened there shows in a tab, or installs when it is an extension's package
    files::Host files_host;
    files_host.open_file = open_local_file;
    files_host.ask_text = ask_text;
    files_host.toast = [](const std::string &text) { toast(text); };
    files::set_host(files_host);

    // the download manager (aria2): a page's downloads, links typed or pasted, magnet links; its page is
    // a tab (OPTIONS > Downloads)
    dm::Host dm_host;
    dm_host.user_agent = webkit_settings_get_user_agent(S.settings);
    dm_host.toast = [](const std::string &text) { toast(text); };
    dm_host.show_in_files = [](const std::string &path) { open_files_tab(path); };
    dm_host.open_file = open_local_file;
    dm_host.browser_download = [](const std::string &uri) {
        if (WebKitDownload *download = webkit_network_session_download_uri(S.session, uri.c_str())) {
            g_object_set_data(G_OBJECT(download), "wisp-browser", GINT_TO_POINTER(1));
            g_object_unref(download);
        }
    };
    dm_host.ask_text = ask_text;
    dm_host.redraw = [] { S.dirty = true; };
    dm::start(dm_host);

    // Wisp Player (video and music from Files and the downloads)
    player::Host player_host;
    player_host.toast = [](const std::string &text) { toast(text); };
    player_host.redraw = [] { S.dirty = true; };
    player_host.fullscreen = [](bool on) { set_fullscreen(on); };
    player_host.is_fullscreen = [] { return S.fullscreen; };
    player_host.closed = [] {
        for (int i = 0; i < static_cast<int>(S.tabs.size()); i++)
            if (S.tabs[i].player) {
                close_tab(i);
                return;
            }
    };
    player_host.ask_text = ask_text;
    player_host.open_tab = [](const std::string &url) { new_tab(url, nullptr, true); };
    player::set_host(player_host);

    // the controller as a page's gamepad (v00.30): WebKit's gamepad manager in ui/platform.cpp
    load_gamepad_declined();
    GamepadHooks gamepad_hooks;
    gamepad_hooks.in_use = on_gamepads_in_use;
    gamepad_hooks.rumble = on_gamepad_rumble;
    platform_set_gamepad_hooks(gamepad_hooks);

    // OrbisWeb's own pages (v00.32, ui/pages.cpp): Settings, History, Bookmarks, Cookies. (Restarts and tab
    // changes come after the page's request has been answered, not inside it.)
    pages::Host pages_host;
    pages_host.session = S.session;
    pages_host.toast = [](const std::string &text) { toast(text); };
    pages_host.setting_changed = apply_setting;
    pages_host.restart = [] { g_idle_add([](gpointer) -> gboolean { restart(); return G_SOURCE_REMOVE; }, nullptr); };
    pages_host.open_downloads = [] { g_idle_add([](gpointer) -> gboolean { open_downloads_tab(); return G_SOURCE_REMOVE; }, nullptr); };
    pages_host.webgpu_test = [] { g_idle_add([](gpointer) -> gboolean { webgpu_test_restart(); return G_SOURCE_REMOVE; }, nullptr); };
    pages_host.adblock = [] { return ext::adblock(); };
    pages_host.set_adblock = [](bool on) {
        ext::set_adblock(on);
        S.ext_buttons_dirty = S.toolbar_dirty = true;
    };
    pages::init(pages_host);
    apply_cookie_policy();
}

// Whether a page shows a local file: pictures and pages by their names, anything else when it is text (no
// NUL byte in its first 4 KiB: WebKit shows it as plain text). A binary file of another kind (a .pkg, an
// .exe) has no viewer here: in a tab it became a download of itself into Downloads, and v00.27's network
// process crashed on it (WebKit patch 0028). (lower: the path in lower case)
bool page_shows(const std::string &path, const std::string &lower)
{
    static const char *const shown[] = {".html", ".htm", ".xhtml", ".xht", ".shtml", ".svg", ".xml", ".png", ".jpg",
                                        ".jpeg", ".jpe", ".jfif", ".gif", ".webp", ".bmp", ".ico", ".pdf"};
    for (const char *suffix : shown) {
        const size_t n = strlen(suffix);
        if (lower.size() > n && lower.compare(lower.size() - n, n, suffix) == 0)
            return true;
    }
    const int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0)
        return true;  // (the tab says why it can't be read)
    char head[4096];
    const ssize_t n = read(fd, head, sizeof head);
    close(fd);
    return n <= 0 || std::memchr(head, 0, static_cast<size_t>(n)) == nullptr;
}

// A file to open (from Files, or a finished download): an extension's package installs, anything
// else a page shows opens in a tab of its own
void open_local_file(const std::string &path)
{
    // video and music: Wisp Player
    if (player::can_play(path)) {
        open_player_tab(path);
        return;
    }
    {
        std::string lower = path;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
        auto has = [&](const char *suffix) {
            const size_t n = strlen(suffix);
            return lower.size() > n && lower.compare(lower.size() - n, n, suffix) == 0;
        };
        if (has(".crx") || has(".nex") || has(".xpi") || has(".zip")) {
            const std::string error = ext::install(path);
            if (error.empty()) {
                toast("Extension installed: see OPTIONS > Extensions");
                S.ext_buttons_dirty = S.toolbar_dirty = true;
                return;
            }
            if (!has(".zip")) {
                toast("The extension could not be installed: " + error);
                return;
            }
        }
        // an archive (a .zip that is no extension among them): opened in Files as a folder
        if (archive::available() && archive::is_archive_name(path)) {
            open_files_tab(path + "/");
            return;
        }
        if (!page_shows(path, lower)) {
            const size_t dot = lower.find_last_of("./");
            const bool named = dot != std::string::npos && lower[dot] == '.' && lower.size() - dot <= 12;
            wisp_log("files: %s: not opened (binary, and no page shows its kind)", path.c_str());
            toast(named ? "OrbisWeb can't open " + lower.substr(dot) + " files" : "OrbisWeb can't open this file (it is no page, picture or text)");
            return;
        }
        // in a tab of its own, Files' tab kept (showing the new tab hides Files)
        char *uri = g_filename_to_uri(path.c_str(), nullptr, nullptr);
        if (uri) {
            new_tab(uri, nullptr, true);
            g_free(uri);
        }
    }
}

// Text from the PS4's keyboard (Files' names, a link for the download manager)
void ask_text(const std::string &title, const std::string &initial, std::function<void(bool, const std::string &)> done)
{
    if (shell::keyboard_active() || !shell::keyboard_open(title, initial)) {
        done(false, {});
        return;
    }
    S.keyboard_for_address = false;
    S.keyboard_done = std::move(done);
}

// One line about IPC on this console: WebKit's processes talk over SOCK_SEQPACKET socket pairs. How the
// kernel treats a pair whose receiver is behind: 4000-byte messages (WebKit's largest inline ones) sent
// until a send fails, with the default room and with WebKit's (patch 0019). Linux says EAGAIN ("try
// again"); FreeBSD 9's unix(4) shrinks the sender's room by what is unread and says EMSGSIZE, which
// WebKit took for an error and dropped the message (settings: ipc_log=0 leaves the test out).
void probe_ipc()
{
    struct Fill {
        int send_room = 0, receive_room = 0, sent = 0, error = 0;
    };
    const auto fill = [](int room, Fill &out) -> bool {
        int sv[2];
        if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv) != 0) {
            out.error = errno;
            return false;
        }
        if (room) {
            setsockopt(sv[0], SOL_SOCKET, SO_SNDBUF, &room, sizeof room);
            setsockopt(sv[1], SOL_SOCKET, SO_RCVBUF, &room, sizeof room);
        }
        socklen_t length = sizeof out.send_room;
        getsockopt(sv[0], SOL_SOCKET, SO_SNDBUF, &out.send_room, &length);
        length = sizeof out.receive_room;
        getsockopt(sv[1], SOL_SOCKET, SO_RCVBUF, &out.receive_room, &length);
        fcntl(sv[0], F_SETFL, fcntl(sv[0], F_GETFL) | O_NONBLOCK);
        static char message[4000];
        while (out.sent < 500) {
            if (send(sv[0], message, sizeof message, 0) < 0) {
                out.error = errno;
                break;
            }
            out.sent++;
        }
        close(sv[0]);
        close(sv[1]);
        return true;
    };
    const auto name = [](int error) -> std::string {
        if (!error)
            return "none failed";
        const char *what = error == EMSGSIZE ? "EMSGSIZE" : error == EAGAIN ? "EAGAIN" : error == ENOBUFS ? "ENOBUFS" : "";
        return std::string(what) + (what[0] ? " (" : "") + strerror(error) + (what[0] ? ")" : "");
    };
    Fill plain;
    if (!fill(0, plain)) {
        wisp_log("ipc: SOCK_SEQPACKET socket pair FAILED (%s): WebKit's processes cannot talk", strerror(plain.error));
        return;
    }
    if (!wisp_setting_int("ipc_log", 1)) {
        wisp_log("ipc: SOCK_SEQPACKET socket pairs work");
        return;
    }
    Fill wide;
    fill(256 * 1024, wide);
    wisp_log("ipc: SOCK_SEQPACKET socket pairs work; a receiver that reads nothing: %d bytes of room (send) / %d "
        "(receive) took %d messages of 4000 bytes, then %s; with %d / %d: %d, then %s", plain.send_room,
        plain.receive_room, plain.sent, name(plain.error).c_str(), wide.send_room, wide.receive_room, wide.sent,
        name(wide.error).c_str());
}

} // namespace

void browser_run()
{
    check_last_run();
    if (!shell::screen_open())
        wisp_log("no screen: VideoOut did not open");
    // the app's GPU address space goes to the web process (the UI draws on the CPU)
    ps4_release_gpu_space();
    shell::input_open();
    shell::hwkeyboard_open();
    probe_ipc();
    meter_start();
    // sleeping tabs' saved state from the last run (tabs are not kept across runs)
    if (DIR *dir = opendir(SLEEP_DIR)) {
        while (dirent *entry = readdir(dir))
            if (entry->d_name[0] != '.')
                unlink((std::string(SLEEP_DIR) + "/" + entry->d_name).c_str());
        closedir(dir);
    }
    setup_webkit();
    new_tab(wisp_setting("home", HOME_DEFAULT), nullptr, true);

    S.loop = g_main_loop_new(nullptr, FALSE);
    // frames after the TV's vblank (no waiting for flips on the main loop, which also carries WebKit's
    // messages and the input); a timer if the system gives no vblank events
    S.vsync_start = wisp_seconds();
    S.vsync = wisp_setting_int("vsync_pacing", 1) && shell::screen_start_vblank(on_vblank);
    if (S.vsync) {
        GSource *source = g_source_new(&frame_source_funcs, sizeof(GSource));
        // WebKit's own work on this loop (its messages from the web and network processes: frames,
        // answers to input events, loads) is G_PRIORITY_DEFAULT, and GLib runs only the highest
        // priority that is ready: at G_PRIORITY_HIGH, frames that took a whole vblank held all of it back
        g_source_set_priority(source, G_PRIORITY_DEFAULT);
        g_source_set_name(source, "wisp frames");
        g_source_attach(source, nullptr);
        g_source_unref(source);
    } else {
        g_timeout_add_full(G_PRIORITY_DEFAULT, 16, frame, nullptr, nullptr);
    }
    wisp_log("browser: running (frames %s)", S.vsync ? "after each vblank" : "on a 16 ms timer");
    wisp_watchdog_beat_start();
    g_main_loop_run(S.loop);
}

} // namespace wisp
