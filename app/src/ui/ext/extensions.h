// Wisp: browser extensions (WebExtensions: Chrome MV2/MV3, Opera, Firefox) on WPE's public API
// (ui/ext/extensions.cpp).
//
//   chrome-extension://<id>/...  a URI scheme serving the unpacked extension's files
//   content scripts, CSS         user scripts / style sheets in a script world per extension
//   chrome.* / browser.*         data/ext/api.js in those worlds and in extension pages, talking to
//                                this module through the "wispExt" script message handler
//   background page / worker     a hidden web view per extension (MV3's worker runs as an event page)
//   declarativeNetRequest        rules compiled to WebKit content rule lists (WebKitUserContentFilterStore)
//   built-in ad blocking         EasyList + EasyPrivacy as content rule lists (Settings)
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

typedef struct _WebKitUserContentManager WebKitUserContentManager;
typedef struct _WebKitWebView WebKitWebView;
typedef struct _WebKitWebContext WebKitWebContext;
typedef struct _WebKitNetworkSession WebKitNetworkSession;

namespace wisp::ext {

// What the extensions need from the browser
struct Browser {
    WebKitWebContext *context = nullptr;
    WebKitNetworkSession *session = nullptr;
    // makes a web view for an extension's own pages (background, popup) with this content manager
    std::function<WebKitWebView *(WebKitUserContentManager *ucm)> make_view;
    // tabs: their ids, views and state
    std::function<std::vector<int>()> tab_ids;
    std::function<WebKitWebView *(int tab)> tab_view;
    std::function<int()> active_tab;
    std::function<int(const std::string &url, bool active)> open_tab;   // its id
    std::function<void(int tab)> close_tab;
    std::function<void(int tab)> activate_tab;
    std::function<void(const std::string &text)> toast;
    std::function<void()> redraw;
    // the browser's own scripts for a tab's content manager (the media stand-ins), added again each
    // time the extensions' scripts are (they start by removing every script)
    std::function<void(WebKitUserContentManager *ucm)> page_scripts;
};

struct Info {
    std::string id, name, version, description, dir, icon, popup, options_page;
    int manifest_version = 2;
    bool enabled = true;
    std::string badge_text;
    uint32_t badge_color = 0xffd93025;   // RGBA
    std::string error;                   // why it isn't running, if it isn't
};

// Once, after the web context exists: the scheme, the stores, every installed extension
void init(const Browser &browser);
// The installed extensions, in install order
std::vector<Info> list();

// Installs a .crx / .nex / .xpi / .zip (or an unpacked folder): "" on success, else why not
std::string install(const std::string &package_path);
void set_enabled(const std::string &id, bool enabled);
void uninstall(const std::string &id);

// A tab's content manager: content scripts, the API, the content rule lists (ad blocking, DNR)
void attach_tab(int tab, WebKitUserContentManager *ucm);
void detach_tab(int tab);
// Tab events for tabs.on*/webNavigation (url, title, status)
void tab_created(int tab);
void tab_updated(int tab, const std::string &url, const std::string &title, bool loading);
void tab_activated(int tab);
void tab_removed(int tab);

// The toolbar's extension button was pressed: its popup's view (shown by the browser over the page),
// or nullptr when the extension takes the click itself (action.onClicked)
WebKitWebView *action_clicked(const std::string &id, int tab);

// Built-in ad blocking (EasyList + EasyPrivacy as content rule lists), on by default
void set_adblock(bool on);
bool adblock();

} // namespace wisp::ext
