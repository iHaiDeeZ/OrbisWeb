// OrbisWeb's own pages (ui/pages.cpp, v00.32): orbisweb://settings (every setting in one page),
// orbisweb://history, orbisweb://bookmarks and orbisweb://cookies, next to orbisweb://about
// (browser.cpp). HTML the browser's orbisweb:// scheme serves (serve_wisp_page); what they change comes
// back as their own links and forms (?do=...&t=<token>). The token is made at each start and only these
// pages have it: no web page can act through them, a link to orbisweb://cookies?do=clear included.
#pragma once

#include <functional>
#include <string>
#include <vector>

typedef struct _WebKitURISchemeRequest WebKitURISchemeRequest;
typedef struct _WebKitNetworkSession WebKitNetworkSession;

namespace wisp::pages {

struct Host {
    WebKitNetworkSession *session = nullptr;   // cookies and other site data
    std::function<void(const std::string &text)> toast;
    // a setting the Settings page changed: the browser applies what applies at once (WebGL, ad
    // blocking, cookies, ...); the rest the page says needs a restart
    std::function<void(const std::string &key)> setting_changed;
    std::function<void()> restart;            // Restart OrbisWeb
    std::function<void()> open_downloads;     // the Downloads tab (its Change the folder)
    std::function<void()> webgpu_test;        // the WebGPU self-test (sets it up and restarts)
    std::function<bool()> adblock;            // ad blocking on (the extensions layer keeps it)
    std::function<void(bool)> set_adblock;
};
void init(const Host &host);

// History: a page a tab went to (its main frame committed: http and https only), and its title later
void note_visit(const std::string &uri);
void note_title(const std::string &uri, const std::string &title);

// The address bar's list (v00.36): addresses used before whose address or title has `typed` in it (letter
// case aside; every one when nothing is typed): bookmarks first, then the history, newest first, each
// address once; at most `max`
struct Suggestion {
    std::string uri, title;
    bool bookmark = false;
};
std::vector<Suggestion> suggestions(const std::string &typed, size_t max);

// Bookmarks
bool bookmarked(const std::string &uri);
// Adds it (true), or removes it when it is bookmarked already (false)
bool toggle_bookmark(const std::string &uri, const std::string &title);

// orbisweb://<page>?<query>: true when page is one of these (served now, or once its data has come)
bool serve(WebKitURISchemeRequest *request, const std::string &page, const std::string &query);

constexpr const char *SETTINGS_URI = "orbisweb://settings";
constexpr const char *HISTORY_URI = "orbisweb://history";
constexpr const char *BOOKMARKS_URI = "orbisweb://bookmarks";
constexpr const char *COOKIES_URI = "orbisweb://cookies";

} // namespace wisp::pages
