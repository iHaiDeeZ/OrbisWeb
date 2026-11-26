// Wisp's download manager (ui/dm/dm.cpp): aria2 (libaria2, the engine of Motrix and many desktop
// download managers) on a thread of its own, and its page in a tab of its own (OPTIONS > Downloads).
//
// It takes what a page downloads (WebKit's "download-started": the link, the page's cookies for it,
// the page as Referer, Wisp's user agent) and links typed or pasted (Triangle / "Add a link"): HTTP,
// HTTPS and FTP with up to 8 connections per file (segmented, resumed after a stop or a restart of
// Wisp), BitTorrent (.torrent links and magnet: links, DHT), Metalink. What aria2 cannot fetch (blob:
// and data: links, a server that refuses it what it gave the page) goes to WebKit's own downloader,
// and those show in the same list.
//
// The controller on its page: Cross the selected download's first button (pause / resume; open a
// finished one), Square removes it from the list (the file stays), Triangle adds a link, the D-pad
// moves the selection, the right stick scrolls, Circle leaves the page (the tab stays). A mouse clicks
// the buttons; a USB keyboard: arrows, Space (pause / resume), Enter (open), Delete (remove), Ctrl+L
// (add a link).
//
// Where downloads go (v00.29): the page's second line names the folder, and Change the folder opens a
// chooser over the page (the places - Downloads, /data, the USB drives - then a folder's folders; Select
// this folder, New folder, Cancel). The choice is the setting download_folder (common/wisp.h:
// wisp_download_folder), which WebKit's downloader, Files' Downloads place and the player's screenshots
// follow too. Each download keeps the folder it started in (aria2's own dir option, in its session
// file); while the folder chosen can't be had (its USB drive out), downloads go to /data/Wisp/Downloads
// and the toasts say so.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "../shell/hwkeyboard.h"
#include "../shell/input.h"
#include "../shell/ps4_canvas.h"

namespace wisp::dm {

struct Host {
    std::string user_agent;   // the pages' (servers that check it see the same browser)
    std::function<void(const std::string &text)> toast;
    // Files at a downloaded file (selected), and a file opened as Files opens it (a tab, the player)
    std::function<void(const std::string &path)> show_in_files;
    std::function<void(const std::string &path)> open_file;
    // WebKit's own downloader for a link aria2 could not take
    std::function<void(const std::string &uri)> browser_download;
    // the PS4 keyboard (or the USB keyboard in place): title, starting text, then done(ok, text)
    std::function<void(const std::string &title, const std::string &initial,
                       std::function<void(bool, const std::string &)> done)> ask_text;
    // something changed that the browser should draw again (the toolbar's badge, the page)
    std::function<void()> redraw;
};

// The engine: aria2's session, with the downloads the last run left (its session file)
void start(const Host &host);
// Its session saved, the engine stopped (Restart / Leave Wisp)
void stop();
// Whether the engine runs (false: libaria2 failed to start; the browser's own downloader then does all)
bool running();

// Whether aria2 takes a link (http, https, ftp, magnet:; not blob:, data:, file:)
bool can_take(const std::string &uri);
// A download for aria2: its link, the request's headers ("Cookie: ...", "Referer: ..."), the file's
// name ("" for the server's / the link's)
void add(const std::string &uri, const std::vector<std::string> &headers, const std::string &file_name);

// Downloads WebKit's downloader runs, in the same list: their id
int external_start(const std::string &name, const std::string &path);
void external_progress(int id, double fraction, int64_t bytes);
void external_done(int id, bool ok, const std::string &error);

// The page (the Downloads tab): shown, hidden, its input and drawing (as Files')
void show();
void hide();
bool visible();
bool input(const shell::Input &in, const std::vector<shell::KeyEvent> &keys, int x, int y, int w, int h, float dt);
bool dirty();
void draw(ps4ui::Canvas &canvas);
// downloads running now (the toolbar's count)
int active_count();

} // namespace wisp::dm
