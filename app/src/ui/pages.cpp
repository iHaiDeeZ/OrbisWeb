// Wisp: OrbisWeb's own pages (ui/pages.h): Settings, History, Bookmarks, Cookies.
//
// History and bookmarks are OrbisWeb's own files (/data/Wisp/history.tsv, bookmarks.tsv: one line each,
// tab-separated); cookies and other site data are WebKit's (its cookie and website data managers).
// Every setting the app reads is on the Settings page, in groups, with when a change applies: at once,
// to pages loaded from then on, or after a restart.
#include "pages.h"

#include "../common/wisp.h"

#include <wpe/webkit.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <map>
#include <string>
#include <sys/stat.h>
#include <vector>

#ifndef WISP_HAVE_JIT
#define WISP_HAVE_JIT 0
#endif
#ifndef WISP_HAVE_WASM
#define WISP_HAVE_WASM 0
#endif
#ifndef WISP_HAVE_WEBGL
#define WISP_HAVE_WEBGL 0
#endif
#ifndef WISP_HAVE_DAWN
#define WISP_HAVE_DAWN 0
#endif
#ifndef WISP_HAVE_WEBGPU
#define WISP_HAVE_WEBGPU 0
#endif
#ifndef WISP_HAVE_WASM_JIT
#define WISP_HAVE_WASM_JIT 0
#endif

namespace wisp::pages {
namespace {

Host H;
std::string g_token;   // (init: the actions' token, new at each start)
bool g_restart_wanted = false;   // a setting that needs a restart changed this run

// --- Text and links -----------------------------------------------------------------------------------

std::string esc(const std::string &text)
{
    std::string out;
    out.reserve(text.size());
    for (char c : text) {
        switch (c) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        case '\'': out += "&#39;"; break;
        default: out += c;
        }
    }
    return out;
}

std::string url_escape(const std::string &text)
{
    gchar *escaped = g_uri_escape_string(text.c_str(), nullptr, FALSE);
    std::string out = escaped ? escaped : "";
    g_free(escaped);
    return out;
}

std::map<std::string, std::string> parse_query(const std::string &query)
{
    std::map<std::string, std::string> out;
    size_t start = 0;
    while (start <= query.size()) {
        size_t end = query.find('&', start);
        if (end == std::string::npos)
            end = query.size();
        std::string part = query.substr(start, end - start);
        std::replace(part.begin(), part.end(), '+', ' ');
        const size_t eq = part.find('=');
        const std::string key = part.substr(0, eq);
        const std::string raw = eq == std::string::npos ? std::string() : part.substr(eq + 1);
        gchar *value = g_uri_unescape_string(raw.c_str(), nullptr);
        if (!key.empty())
            out[key] = value ? value : raw;
        g_free(value);
        start = end + 1;
    }
    return out;
}

// A link that acts on one of these pages: the token in it
std::string act(const char *page, const std::string &params)
{
    return std::string("orbisweb://") + page + "?" + params + "&t=" + g_token;
}

// A setting's single line in the settings file (no line breaks, at most 2 KiB)
std::string one_line(std::string value)
{
    for (char &c : value)
        if (c == '\r' || c == '\n' || c == '\t')
            c = ' ';
    if (value.size() > 2048)
        value.resize(2048);
    return value;
}

std::string day_of(time_t t)
{
    tm local {};
    localtime_r(&t, &local);
    char text[32];
    strftime(text, sizeof text, "%Y-%m-%d", &local);
    return text;
}

std::string clock_of(time_t t)
{
    tm local {};
    localtime_r(&t, &local);
    char text[16];
    strftime(text, sizeof text, "%H:%M", &local);
    return text;
}

std::string host_of(const std::string &uri)
{
    std::string host;
    if (GUri *parsed = g_uri_parse(uri.c_str(), G_URI_FLAGS_NONE, nullptr)) {
        if (g_uri_get_host(parsed))
            host = g_uri_get_host(parsed);
        g_uri_unref(parsed);
    }
    return host;
}

void finish(WebKitURISchemeRequest *request, const std::string &html)
{
    GBytes *bytes = g_bytes_new(html.data(), html.size());
    GInputStream *stream = g_memory_input_stream_new_from_bytes(bytes);
    webkit_uri_scheme_request_finish(request, stream, static_cast<gint64>(html.size()), "text/html");
    g_object_unref(stream);
    g_bytes_unref(bytes);
}

// The page around a body: the style (About's), the bar of OrbisWeb's pages, a notice (what an action
// did), and, after an action, the address put back without it (a reload or Back then does not act
// again)
std::string page_html(const char *page, const std::string &title, const std::string &body, const std::string &notice, bool acted)
{
    static const char *const NAV[][2] = {
        {"settings", "Settings"}, {"history", "History"}, {"bookmarks", "Bookmarks"}, {"cookies", "Cookies"}, {"about", "About"}};
    std::string nav;
    for (const auto &item : NAV)
        nav += std::string("<a class=\"tab") + (strcmp(item[0], page) == 0 ? " on" : "") + "\" href=\"orbisweb://" + item[0] + "\">" + item[1] + "</a>";
    std::string html = R"HTML(<!doctype html><html lang="en"><head><meta charset="utf-8"><title>)HTML" + esc(title) + R"HTML(</title>
<style>
:root { color-scheme: dark; }
body { margin: 0; background: #202124; color: #e8eaed; font: 22px/1.45 Roboto, "Liberation Sans", sans-serif; }
main { max-width: 1240px; margin: 0 auto; padding: 28px 48px 80px; }
nav { display: flex; gap: 10px; flex-wrap: wrap; margin-bottom: 26px; }
nav .tab { padding: 10px 24px; border-radius: 24px; background: #2b2c30; color: #e8eaed; }
nav .tab.on { background: #8ab4f8; color: #0b1a33; font-weight: bold; }
h1 { font-size: 46px; margin: 6px 0 18px; }
h2 { font-size: 19px; letter-spacing: .08em; text-transform: uppercase; color: #9aa0a6; margin: 34px 0 12px; }
a { color: #8ab4f8; text-decoration: none; }
a:hover, a:focus { text-decoration: underline; }
.card { background: #2b2c30; border-radius: 18px; padding: 8px 30px; }
.row { display: flex; align-items: center; gap: 22px; padding: 16px 0; border-bottom: 1px solid #3c4043; }
.row:last-child { border-bottom: 0; }
.what { flex: 1; min-width: 0; }
.name { font-size: 23px; }
.desc, .dim { color: #9aa0a6; font-size: 18px; }
.when { color: #fdd663; font-size: 16px; }
.btns { display: flex; gap: 8px; flex-wrap: wrap; justify-content: flex-end; max-width: 640px; }
.btn { display: inline-block; padding: 9px 20px; border-radius: 22px; background: #3c4043; color: #e8eaed; font-size: 19px; border: 0; font-family: inherit; }
.btn.on { background: #8ab4f8; color: #0b1a33; font-weight: bold; }
.btn.danger { background: #5c2b29; color: #f28b82; }
.btn.off { opacity: .45; }
a.btn:hover, a.btn:focus, button.btn:hover { text-decoration: none; outline: 3px solid #8ab4f8; }
input[type=text], input[type=search], input[type=url] { font: inherit; font-size: 20px; padding: 9px 16px; border-radius: 12px; border: 1px solid #5f6368; background: #202124; color: #e8eaed; width: 460px; max-width: 100%; }
form { display: flex; gap: 10px; flex-wrap: wrap; align-items: center; margin: 0; }
.notice { background: #283a2b; color: #a8dab5; border-radius: 14px; padding: 14px 24px; margin-bottom: 18px; }
.notice.warn { background: #3e3420; color: #fdd663; }
.entry { display: flex; align-items: center; gap: 18px; padding: 11px 0; border-bottom: 1px solid #3c4043; }
.entry .time { color: #9aa0a6; width: 70px; flex: none; font-size: 18px; }
.entry .title { flex: 1; min-width: 0; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.entry .host { color: #9aa0a6; font-size: 17px; max-width: 360px; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
.x { color: #9aa0a6; padding: 2px 14px; border-radius: 16px; }
.x:hover, .x:focus { background: #5c2b29; color: #f28b82; text-decoration: none; }
.badge { display: inline-block; font-size: 15px; padding: 2px 10px; border-radius: 10px; background: #3c4043; color: #c8c8c8; margin-right: 6px; }
.empty { color: #9aa0a6; padding: 30px 0; }
</style></head><body><main><nav>)HTML" + nav + "</nav><h1>" + esc(title) + "</h1>";
    if (!notice.empty())
        html += notice;
    html += body;
    html += "</main>";
    if (acted)
        html += std::string("<script>history.replaceState(null, '', 'orbisweb://") + page + "' + location.hash);</script>";
    html += "</body></html>";
    return html;
}

std::string notice_html(const std::string &text, bool warn = false)
{
    return text.empty() ? std::string() : std::string("<div class=\"notice") + (warn ? " warn" : "") + "\">" + text + "</div>";
}

// --- Settings -----------------------------------------------------------------------------------------

enum class Kind { Toggle, Choice, Text };
enum class Applies { Now, NewPages, Restart };

struct Choice {
    const char *value, *label;
};

struct Entry {
    const char *section, *key, *label, *description;
    Kind kind;
    const char *fallback;
    std::vector<Choice> choices;
    Applies applies;
};

const std::vector<Entry> &entries()
{
    static const std::vector<Entry> list = {
        {"General", "home", "Home page", "What a new tab and Home open", Kind::Text, "https://www.google.com", {}, Applies::Now},
        {"General", "search", "Search engine", "Words typed in the address bar search here", Kind::Choice, "https://www.google.com/search?q=",
         {{"https://www.google.com/search?q=", "Google"}, {"https://duckduckgo.com/?q=", "DuckDuckGo"}, {"https://www.bing.com/search?q=", "Bing"},
          {"https://search.brave.com/search?q=", "Brave"}, {"https://www.startpage.com/do/search?q=", "Startpage"}}, Applies::Now},

        {"Privacy", "cookies", "Cookies", "What cookies sites may keep (the Cookies page lists them)", Kind::Choice, "no_third_party",
         {{"always", "Allow all"}, {"no_third_party", "Block third-party"}, {"never", "Block all"}}, Applies::Now},
        {"Privacy", "adblock", "Ad blocking", "EasyList and EasyPrivacy (they compile for a minute the first time)", Kind::Toggle, "0", {}, Applies::Now},
        {"Privacy", "youtube_ad_skip", "Skip YouTube's ads", "With ad blocking on, an ad YouTube still plays ends as it starts", Kind::Toggle, "1", {}, Applies::NewPages},

        {"Pages and JavaScript", "jit", "JavaScript JIT", "Faster JavaScript: the Baseline and DFG tiers. Off: the interpreter (the switch to try when a page crashes)",
         Kind::Toggle, "1", {}, Applies::Restart},
        {"Pages and JavaScript", "wasm", "WebAssembly", "Games, emulators and apps compiled for the web", Kind::Toggle, "1", {}, Applies::Restart},
        {"Pages and JavaScript", "wasm_jit", "WebAssembly JIT",
         "Compiles a game's WebAssembly once the interpreter has run it a while (BBQ), then its hottest code again, optimized (OMG): emulators run much faster. BBQ alone: less memory. Off: the interpreter alone",
         Kind::Choice, "omg", {{"omg", "BBQ + OMG"}, {"bbq", "BBQ"}, {"off", "Off"}}, Applies::Restart},
        {"Pages and JavaScript", "wasm_omg_max", "Largest WebAssembly function OMG optimizes",
         "Larger ones stay on BBQ. Higher: faster games with big functions, more memory and time while they compile",
         Kind::Choice, "256000", {{"50000", "50 KB"}, {"100000", "100 KB"}, {"256000", "256 KB"}, {"512000", "512 KB"}}, Applies::Restart},
        {"Pages and JavaScript", "compiler_priority", "Compiler threads give way to games",
         "WebKit's compiler threads run below the pages' own threads, on the cores they leave free",
         Kind::Choice, "720", {{"720", "Yes"}, {"0", "No (same priority)"}}, Applies::Restart},
        {"Pages and JavaScript", "jit_memory_mib", "Memory for compiled code", "Where JavaScript's and WebAssembly's compiled code goes; a game's may need a lot",
         Kind::Choice, "256", {{"64", "64 MiB"}, {"128", "128 MiB"}, {"256", "256 MiB"}, {"384", "384 MiB"}}, Applies::Restart},
        {"Pages and JavaScript", "js_ftl", "JavaScript FTL tier",
         "JavaScriptCore's optimizing tier for the hottest scripts: faster games and pages (the largest functions stay in the tiers below)", Kind::Toggle, "1", {}, Applies::Restart},
        {"Pages and JavaScript", "wasm_simd", "WebAssembly SIMD", "128-bit vector instructions, on the console's AVX: emulators and games built with them need it",
         Kind::Toggle, "1", {}, Applies::Restart},
        {"Pages and JavaScript", "wasm_relaxed_simd", "WebAssembly relaxed SIMD", "The faster, less strict vector instructions some builds use", Kind::Toggle, "1", {}, Applies::Restart},
        {"Pages and JavaScript", "wasm_threads", "WebAssembly threads",
         "SharedArrayBuffer and shared memories, for games and emulators that run on several cores. Every page gets them: one process serves them all",
         Kind::Toggle, "1", {}, Applies::Restart},
        {"Pages and JavaScript", "coep_credentialless", "Cross-origin isolation for \"credentialless\" sites",
         "Sites that ask for it with COEP credentialless (games with threads) get crossOriginIsolated. Their files from other sites then need a CORP header",
         Kind::Toggle, "1", {}, Applies::Restart},
        {"Pages and JavaScript", "opfs", "Pages' own files", "navigator.storage and its private file system, where web games keep their saves",
         Kind::Toggle, "1", {}, Applies::NewPages},
        {"Pages and JavaScript", "concurrent_gc", "Concurrent garbage collection",
         "Frees JavaScript's memory beside the page instead of stopping it: fewer hitches. Automatic: on when the console passes a check as the page process starts",
         Kind::Choice, "auto", {{"auto", "Automatic"}, {"1", "On"}, {"0", "Off"}}, Applies::Restart},
        {"Pages and JavaScript", "gc_fallback", "Turn the concurrent collector off after two crashes",
         "Until OrbisWeb starts again. Off: pages that stop for other reasons (memory, the GPU) keep it",
         Kind::Toggle, "0", {}, Applies::Now},
        {"Pages and JavaScript", "webgl", "WebGL 1 and 2", "3D graphics for pages: ANGLE on the GPU (needs CPU + GPU or GPU drawing)", Kind::Toggle, "1", {}, Applies::NewPages},
        {"Pages and JavaScript", "webgpu", "WebGPU", "Newer 3D graphics and GPU computing for pages: Dawn on the GPU (needs CPU + GPU or GPU drawing)",
         Kind::Toggle, "1", {}, Applies::NewPages},
        {"Pages and JavaScript", "autoplay", "Videos that start by themselves", "Media a page starts without a click", Kind::Choice, "allow",
         {{"allow", "Allow"}, {"muted", "Muted only"}, {"deny", "Never"}}, Applies::Restart},
        {"Pages and JavaScript", "audio_out", "Pages' sound plays from",
         "OrbisWeb's own process, or the page process (nothing was heard from it up to v00.34)", Kind::Choice, "ui",
         {{"ui", "OrbisWeb"}, {"web", "The page process"}}, Applies::Restart},
        {"Pages and JavaScript", "video_max_height", "Video quality cap", "The largest video a page gets offered (the console decodes in software)", Kind::Choice, "720",
         {{"480", "480p"}, {"720", "720p"}, {"1080", "1080p"}, {"0", "No cap"}}, Applies::NewPages},
        {"Pages and JavaScript", "video_h264_only", "H.264 video only", "Sites that offer several kinds (YouTube) send H.264, the lightest to decode", Kind::Toggle, "1", {}, Applies::NewPages},
        {"Pages and JavaScript", "service_workers", "Service workers", "Off by default: a site's workers ask for a web process of their own, which the PS4 can't start",
         Kind::Toggle, "0", {}, Applies::Restart},
        {"Pages and JavaScript", "device_memory", "Memory pages are told about",
         "navigator.deviceMemory: games that read it pick their light mode on 2 GB (Midnight Club 3: half the memory, a fifth of the threads)",
         Kind::Choice, "2", {{"1", "1 GB"}, {"2", "2 GB"}, {"4", "4 GB"}, {"8", "8 GB"}, {"0", "Not told"}}, Applies::NewPages},
        {"Pages and JavaScript", "ruffle_focus", "Keys for Flash games (Ruffle)",
         "Ruffle takes keys only after it sees its player get the focus; on the PS4 pages focus the player before Ruffle is ready",
         Kind::Toggle, "1", {}, Applies::NewPages},
        {"Pages and JavaScript", "smooth_scrolling", "Smooth scrolling", "Scrolls animated over several frames", Kind::Toggle, "1", {}, Applies::Now},

        {"Drawing and memory", "rendering", "Drawing", "Who draws pages. CPU + GPU: the CPU paints, the GPU composites and scrolls", Kind::Choice, "hybrid",
         {{"cpu", "CPU"}, {"hybrid", "CPU + GPU"}, {"gpu", "GPU"}}, Applies::Restart},
        {"Drawing and memory", "canvas_gpu", "Canvases on the GPU", "2D canvases drawn by Skia on the GPU", Kind::Toggle, "0", {}, Applies::NewPages},
        {"Drawing and memory", "paint_threads", "Painting threads", "Threads that paint pages on the CPU", Kind::Choice, "4",
         {{"1", "1"}, {"2", "2"}, {"4", "4"}, {"6", "6"}}, Applies::Restart},
        {"Drawing and memory", "tab_sleep_after", "A hidden tab sleeps after", "Its page saved and let go, to free memory; it comes back when shown", Kind::Choice, "600",
         {{"120", "2 min"}, {"300", "5 min"}, {"600", "10 min"}, {"1800", "30 min"}, {"0", "Never"}}, Applies::Now},
        {"Drawing and memory", "memory_limit", "The web process's heap", "MiB of memory for every page together", Kind::Choice, "1536",
         {{"1024", "1 GiB"}, {"1536", "1.5 GiB"}, {"2048", "2 GiB"}}, Applies::Restart},
        {"Drawing and memory", "memory_meter", "Memory meter", "The memory left, on the toolbar", Kind::Toggle, "1", {}, Applies::Restart},

        {"Controller", "pointer_size", "Pointer size", "", Kind::Choice, "160", {{"100", "Small"}, {"160", "Medium"}, {"220", "Large"}, {"300", "Huge"}}, Applies::Restart},
        {"Controller", "gamepad_name", "The controller's name for pages", "What a game's Gamepad API reads (Gamepad.id)", Kind::Text,
         "Wireless Controller (STANDARD GAMEPAD Vendor: 054c Product: 09cc)", {}, Applies::NewPages},

        {"Downloads", "download_manager", "Download manager", "aria2: up to 16 connections a file, resumes, torrents. Off: WebKit's own downloader",
         Kind::Toggle, "1", {}, Applies::Restart},
        {"Downloads", "dm_connections", "Connections a file", "", Kind::Choice, "8", {{"1", "1"}, {"4", "4"}, {"8", "8"}, {"16", "16"}}, Applies::Restart},
        {"Downloads", "dm_parallel", "Files at once", "", Kind::Choice, "3", {{"1", "1"}, {"2", "2"}, {"3", "3"}, {"5", "5"}}, Applies::Restart},
        {"Downloads", "dm_seed_minutes", "Seed finished torrents", "", Kind::Choice, "0", {{"0", "No"}, {"10", "10 min"}, {"30", "30 min"}, {"60", "1 hour"}}, Applies::Restart},

        {"Player", "player_threads", "Video decoding threads", "", Kind::Choice, "4", {{"1", "1"}, {"2", "2"}, {"4", "4"}, {"6", "6"}}, Applies::Now},
        {"Player", "player_next", "Play the next file", "At a file's end, the folder's next one plays", Kind::Toggle, "1", {}, Applies::Now},

        {"Diagnostics", "console_log", "Page messages in the log", "Pages' console messages (script errors among them) into logs/web.log", Kind::Toggle, "0", {}, Applies::Now},
        {"Diagnostics", "memory_log", "Memory lines in the log", "", Kind::Toggle, "1", {}, Applies::Now},
        {"Diagnostics", "gpu_log", "GPU messages in the log", "Mesa's EGL and Zink messages", Kind::Toggle, "1", {}, Applies::Restart},
        {"Diagnostics", "webgpu_log", "WebGPU errors in the log", "Dawn's errors a page does not catch, and a GPU device it lost, in logs/web.log",
         Kind::Toggle, "1", {}, Applies::Restart},
        {"Diagnostics", "kobj_census", "Kernel-object census in the log",
         "Which calls use up the PS4's internal memory (threads, locks) in the page process, in logs/web.log (slows the page process a little)", Kind::Toggle, "0", {}, Applies::Restart},
        {"Diagnostics", "ipc_log", "WebKit's IPC lines in the log", "", Kind::Toggle, "1", {}, Applies::Restart},
        {"Diagnostics", "dm_log", "aria2's log", "logs/aria2.log", Kind::Toggle, "1", {}, Applies::Restart},
        {"Diagnostics", "watchdog", "Thread dumps of a page that stops answering", "logs/web.hang.log", Kind::Toggle, "1", {}, Applies::Restart},
        {"Diagnostics", "stall_log", "Page stalls and frames in the log",
         "How often and how long the pages' main thread was held, and how many frames the page, the compositor and WebGPU canvases made, in logs/web.log",
         Kind::Toggle, "1", {}, Applies::Restart},
        {"Diagnostics", "profiler", "Hot-spot profile",
         "Every 10 s, each busy thread's busiest code, in kit-web-gpu/profile.log and kit-ui/profile.log (what holds a slow game back; costs some CPU)",
         Kind::Toggle, "0", {}, Applies::Restart},
        {"Diagnostics", "audio_log", "Sound lines in the log", "Each player's sound, where the PS4 plays it and how loud, in logs/web.log", Kind::Toggle, "1", {}, Applies::Restart},
        {"Diagnostics", "key_log", "Keys and focus in the log",
         "What pages see of the keyboard and the focus, as page messages in logs/web.log (needs Page messages in the log)", Kind::Toggle, "1", {}, Applies::NewPages},
        {"Diagnostics", "load_log", "Failed page loads in the log", "Every file a page could not load (an HTTP error or a failed connection), with its address, in logs/web.log",
         Kind::Toggle, "1", {}, Applies::Restart},
        {"Diagnostics", "restart_hung_page", "Restart a page that stops answering", "A reload or an address then starts the web process again", Kind::Toggle, "1", {}, Applies::Now},
        {"Diagnostics", "inspector", "Remote Web Inspector", "", Kind::Toggle, "0", {}, Applies::Restart},
    };
    return list;
}

const Entry *entry_for(const std::string &key)
{
    for (const Entry &e : entries())
        if (key == e.key)
            return &e;
    return nullptr;
}

std::string value_of(const Entry &e)
{
    if (!strcmp(e.key, "adblock") && H.adblock)
        return H.adblock() ? "1" : "0";
    if (!strcmp(e.key, "smooth_scrolling"))   // (the browser's default: on with the GPU, off on the CPU)
        return wisp_setting(e.key, wisp_rendering() != WispRendering::Cpu ? "1" : "0");
    return wisp_setting(e.key, e.fallback);
}

// Why a setting can't change in this build or this mode ("" it can)
std::string unavailable(const Entry &e)
{
    const std::string key = e.key;
    if (key == "jit" && !WISP_HAVE_JIT)
        return "Not in this build";
    if ((key == "wasm_jit" || key == "js_ftl") && !WISP_HAVE_WASM_JIT)
        return "Not in this build";
    if ((key == "wasm" || key == "wasm_simd" || key == "wasm_relaxed_simd" || key == "wasm_threads") && !WISP_HAVE_WASM)
        return "Not in this build";
    if (key == "webgl" && !WISP_HAVE_WEBGL)
        return "Not in this build";
    if (key == "webgl" && wisp_rendering() == WispRendering::Cpu)
        return "Needs CPU + GPU or GPU drawing";
    if ((key == "webgpu" || key == "webgpu_log") && !WISP_HAVE_WEBGPU)
        return "Not in this build";
    if (key == "webgpu" && wisp_rendering() == WispRendering::Cpu)
        return "Needs CPU + GPU or GPU drawing";
    return {};
}

const char *applies_text(Applies applies)
{
    return applies == Applies::Now ? "applies at once" : applies == Applies::NewPages ? "applies to pages loaded from now on (reload)" : "applies after a restart";
}

// A setting changed through a page's link or form: false (and why) when the key or value isn't one of
// the page's
bool set_setting(const std::string &key, const std::string &value, std::string &said)
{
    const Entry *e = entry_for(key);
    if (key == "gamepad_declined") {   // (the Controller group's list: only cleared)
        wisp_setting_set("gamepad_declined", "");
        if (H.setting_changed)
            H.setting_changed("gamepad_declined");
        said = "The sites you took the controller back from are forgotten: each gets it again when it reads gamepads.";
        return true;
    }
    if (!e) {
        said = "No such setting.";
        return false;
    }
    const std::string why = unavailable(*e);
    if (!why.empty()) {
        said = esc(e->label) + ": " + esc(why) + ".";
        return false;
    }
    std::string v = one_line(value);
    std::string shown = v;
    if (e->kind == Kind::Toggle) {
        if (v != "0" && v != "1") {
            said = "Not a value for this setting.";
            return false;
        }
        shown = v == "1" ? "on" : "off";
    } else if (e->kind == Kind::Choice) {
        const auto it = std::find_if(e->choices.begin(), e->choices.end(), [&](const Choice &c) { return v == c.value; });
        if (it == e->choices.end()) {
            said = "Not a value for this setting.";
            return false;
        }
        shown = it->label;
    } else if (v.empty()) {
        v = e->fallback;
        shown = v;
    }
    if (!strcmp(e->key, "adblock") && H.set_adblock)
        H.set_adblock(v == "1");
    else
        wisp_setting_set(e->key, v.c_str());
    wisp_log("settings: %s = %s (the Settings page)", e->key, v.c_str());
    if (H.setting_changed)
        H.setting_changed(e->key);
    if (e->applies == Applies::Restart)
        g_restart_wanted = true;
    said = esc(e->label) + ": " + esc(shown) + ", " + applies_text(e->applies) + ".";
    if (e->applies == Applies::Restart)
        said += " <a class=\"btn\" href=\"" + act("settings", "do=restart") + "\">Restart OrbisWeb</a>";
    return true;
}

// The JavaScript that tests WebAssembly's features in the page itself (what a game's check sees)
const char *WASM_CHECK = R"JS(<script>
(function () {
  function has(bytes) { try { return WebAssembly.validate(new Uint8Array(bytes)); } catch (e) { return false; } }
  var out = [];
  if (typeof WebAssembly !== 'object') { out.push('no WebAssembly'); }
  else {
    out.push('WebAssembly ' + (has([0,97,115,109,1,0,0,0]) ? 'yes' : 'no'));
    out.push('SIMD ' + (has([0,97,115,109,1,0,0,0,1,5,1,96,0,1,123,3,2,1,0,10,10,1,8,0,65,0,253,15,253,98,11]) ? 'yes' : 'no'));
    out.push('relaxed SIMD ' + (has([0,97,115,109,1,0,0,0,1,5,1,96,0,1,123,3,2,1,0,10,15,1,13,0,65,1,253,15,65,2,253,15,253,128,2,11]) ? 'yes' : 'no'));
    out.push('threads ' + (typeof SharedArrayBuffer === 'function' && has([0,97,115,109,1,0,0,0,5,4,1,3,1,1]) ? 'yes' : 'no'));
  }
  var cell = document.getElementById('wasm-now');
  if (cell) cell.textContent = out.join(' · ');
  // the sound test: 440 Hz for a second through Web Audio (what web.log's "audio:" lines then describe)
  var button = document.getElementById('tone-btn'), said = document.getElementById('sound-now');
  if (button) button.addEventListener('click', function (event) {
    event.preventDefault();
    try {
      var C = window.AudioContext || window.webkitAudioContext;
      var context = new C(), tone = context.createOscillator(), gain = context.createGain();
      tone.frequency.value = 440;
      gain.gain.value = 0.3;
      tone.connect(gain);
      gain.connect(context.destination);
      context.resume().then(function () {
        tone.start();
        tone.stop(context.currentTime + 1);
        if (said) said.textContent = 'Playing a second at ' + context.sampleRate + ' Hz (' + context.state + ')';
        setTimeout(function () {
          context.close();
          if (said) said.textContent = 'Played. Heard nothing? Try OrbisWeb\'s tone; the "audio:" lines in logs/web.log and logs/ui.log say what the PS4 did with it';
        }, 1500);
      });
    } catch (e) {
      if (said) said.textContent = 'No Web Audio here: ' + e;
    }
  });
})();
</script>)JS";

std::string settings_html(const std::string &notice, bool acted)
{
    std::string body;
    const char *section = "";
    const auto open_section = [&](const char *name) {
        if (*section)
            body += "</div>";
        section = name;
        body += std::string("<h2>") + esc(name) + "</h2><div class=\"card\">";
    };
    const auto row = [&](const std::string &id, const std::string &name, const std::string &desc, const std::string &controls) {
        body += "<div class=\"row\" id=\"" + esc(id) + "\"><div class=\"what\"><div class=\"name\">" + name + "</div>";
        if (!desc.empty())
            body += "<div class=\"desc\">" + desc + "</div>";
        body += "</div><div class=\"btns\">" + controls + "</div></div>";
    };
    for (const Entry &e : entries()) {
        if (strcmp(section, e.section) != 0) {
            open_section(e.section);
            // the group's own rows, first
            if (!strcmp(e.section, "General")) {
                const std::string folder = wisp_download_folder();
                row("download_folder", "Downloads go to", "<span class=\"dim\">" + esc(folder) + "</span>",
                    "<a class=\"btn\" href=\"" + act("settings", "do=downloads") + "\">Change the folder</a>");
            } else if (!strcmp(e.section, "Privacy")) {
                row("data", "History, cookies and site data", "",
                    "<a class=\"btn\" href=\"orbisweb://history\">History</a><a class=\"btn\" href=\"orbisweb://cookies\">Cookies and site data</a>"
                    "<a class=\"btn danger\" href=\"" + act("settings", "do=ask_clear") + "\">Clear browsing data</a>");
            } else if (!strcmp(e.section, "Pages and JavaScript")) {
                row("wasm_now", "WebAssembly on this console", "<span id=\"wasm-now\" class=\"dim\">(checking)</span>", "");
                // (v00.34) a second's tone through Web Audio: the page's sound path, the audio sink and the PS4's;
                // (v00.35) and one from OrbisWeb itself, no page in it: which of the two is heard says where sound stops
                row("sound_test", "Sound test",
                    "<span id=\"sound-now\" class=\"dim\">A second's tone through a page's sound (Web Audio), or from OrbisWeb itself</span>",
                    "<a class=\"btn\" id=\"tone-btn\" href=\"#sound_test\">Page's tone</a><a class=\"btn\" href=\"" + act("settings", "do=tone") +
                        "#sound_test\">OrbisWeb's tone</a>");
            }
        }
        const std::string value = value_of(e);
        const std::string why = unavailable(e);
        std::string controls;
        if (e.kind == Kind::Toggle) {
            for (const char *v : {"1", "0"})
                controls += std::string("<a class=\"btn") + (value == v ? " on" : "") + (why.empty() ? "" : " off") + "\" href=\"" +
                            act("settings", std::string("do=set&key=") + e.key + "&value=" + v) + "#" + e.key + "\">" + (*v == '1' ? "On" : "Off") + "</a>";
        } else if (e.kind == Kind::Choice) {
            for (const Choice &c : e.choices)
                controls += std::string("<a class=\"btn") + (value == c.value ? " on" : "") + (why.empty() ? "" : " off") + "\" href=\"" +
                            act("settings", std::string("do=set&key=") + e.key + "&value=" + url_escape(c.value)) + "#" + e.key + "\">" + esc(c.label) + "</a>";
        } else {
            controls = "<form action=\"orbisweb://settings\" method=\"get\"><input type=\"hidden\" name=\"do\" value=\"set\"><input type=\"hidden\" name=\"key\" value=\"" +
                       std::string(e.key) + "\"><input type=\"hidden\" name=\"t\" value=\"" + g_token + "\"><input type=\"text\" name=\"value\" value=\"" + esc(value) +
                       "\"><button class=\"btn\" type=\"submit\">Save</button></form>";
        }
        std::string desc = esc(e.description);
        if (!why.empty())
            desc += std::string(desc.empty() ? "" : " &middot; ") + "<span class=\"when\">" + esc(why) + "</span>";
        else if (e.applies != Applies::Now)
            desc += std::string(desc.empty() ? "" : " &middot; ") + "<span class=\"when\">" + (e.applies == Applies::Restart ? "after a restart" : "pages loaded from now on") + "</span>";
        row(e.key, esc(e.label), desc, controls);
        // the Controller group's list of sites, after its last setting
        if (!strcmp(e.key, "gamepad_name")) {
            const std::string declined = wisp_setting("gamepad_declined", "");
            row("gamepad_declined", "Sites you took the controller back from",
                declined.empty() ? std::string("<span class=\"dim\">None: every page that reads gamepads gets the controller</span>")
                                 : "<span class=\"dim\">" + esc(declined) + "</span>",
                declined.empty() ? std::string() : "<a class=\"btn\" href=\"" + act("settings", "do=set&key=gamepad_declined&value=") + "\">Forget them</a>");
        }
    }
    if (*section)
        body += "</div>";
    body += "<h2>OrbisWeb</h2><div class=\"card\">";
    row("webgpu_test", "WebGPU test", "Dawn, the engine of pages' WebGPU, on this console's GPU (OrbisWeb restarts; the result comes in a toast and on About)",
        WISP_HAVE_DAWN ? "<a class=\"btn\" href=\"" + act("settings", "do=webgpu_test") + "\">Run it</a>" : std::string("<span class=\"dim\">Not in this build</span>"));
    row("restart", "Restart OrbisWeb", g_restart_wanted ? "<span class=\"when\">Settings changed that apply after a restart</span>" : "",
        "<a class=\"btn" + std::string(g_restart_wanted ? " on" : "") + "\" href=\"" + act("settings", "do=restart") + "\">Restart</a>");
    body += "</div>";
    body += WASM_CHECK;
    return page_html("settings", "Settings", body, notice, acted);
}

// --- History --------------------------------------------------------------------------------------------

struct Visit {
    time_t when = 0;
    std::string uri, title;
};

constexpr const char *HISTORY_FILE = WISP_DATA "/history.tsv";
constexpr size_t HISTORY_MAX = 5000;
std::vector<Visit> history;   // oldest first
bool history_loaded = false;
guint history_save = 0;

void load_history()
{
    if (history_loaded)
        return;
    history_loaded = true;
    FILE *f = fopen(HISTORY_FILE, "r");
    if (!f)
        return;
    char line[8192];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *uri = strchr(line, '\t');
        if (!uri)
            continue;
        *uri++ = 0;
        char *title = strchr(uri, '\t');
        if (title)
            *title++ = 0;
        history.push_back({static_cast<time_t>(atoll(line)), uri, title ? title : ""});
    }
    fclose(f);
    if (history.size() > HISTORY_MAX)
        history.erase(history.begin(), history.end() - HISTORY_MAX);
}

bool save_history_now()
{
    const std::string temp = std::string(HISTORY_FILE) + ".new";
    if (FILE *f = fopen(temp.c_str(), "w")) {
        for (const Visit &v : history)
            fprintf(f, "%lld\t%s\t%s\n", static_cast<long long>(v.when), v.uri.c_str(), v.title.c_str());
        fclose(f);
        rename(temp.c_str(), HISTORY_FILE);
    }
    return false;
}

void save_history_soon()
{
    if (history_save)
        return;
    history_save = g_timeout_add_seconds(2, [](gpointer) -> gboolean {
        history_save = 0;
        save_history_now();
        return G_SOURCE_REMOVE;
    }, nullptr);
}

std::string history_html(const std::map<std::string, std::string> &q, const std::string &notice, bool acted)
{
    load_history();
    const auto get = [&](const char *k) { const auto it = q.find(k); return it == q.end() ? std::string() : it->second; };
    const std::string search = get("q");
    const long long asked = atoll(get("limit").c_str());
    const size_t limit = asked > 0 ? static_cast<size_t>(std::min(asked, 100000LL)) : 300;
    std::string lower = search;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    std::string body = "<form action=\"orbisweb://history\" method=\"get\"><input type=\"search\" name=\"q\" placeholder=\"Search history\" value=\"" + esc(search) +
                       "\"><button class=\"btn\" type=\"submit\">Search</button>" + (search.empty() ? "" : "<a class=\"btn\" href=\"orbisweb://history\">All</a>") +
                       "<a class=\"btn danger\" href=\"" + act("history", "do=ask_clear") + "\">Clear browsing history</a></form>";
    const std::string today = day_of(time(nullptr)), yesterday = day_of(time(nullptr) - 86400);
    std::string day;
    size_t shown = 0, matched = 0;
    for (auto it = history.rbegin(); it != history.rend(); ++it) {
        const Visit &v = *it;
        if (!lower.empty()) {
            std::string hay = v.title + " " + v.uri;
            std::transform(hay.begin(), hay.end(), hay.begin(), ::tolower);
            if (hay.find(lower) == std::string::npos)
                continue;
        }
        matched++;
        if (shown >= limit)
            continue;
        const std::string d = day_of(v.when);
        if (d != day) {
            day = d;
            body += "<h2>" + esc(d == today ? "Today" : d == yesterday ? "Yesterday" : d) + "</h2>";
        }
        body += "<div class=\"entry\"><span class=\"time\">" + clock_of(v.when) + "</span><a class=\"title\" href=\"" + esc(v.uri) + "\">" +
                esc(v.title.empty() ? v.uri : v.title) + "</a><span class=\"host\">" + esc(host_of(v.uri)) + "</span><a class=\"x\" title=\"Remove\" href=\"" +
                esc(act("history", "do=remove&when=" + std::to_string(static_cast<long long>(v.when)) + "&uri=" + url_escape(v.uri))) + "\">&times;</a></div>";
        shown++;
    }
    if (!matched)
        body += std::string("<p class=\"empty\">") + (search.empty() ? "No history yet: the pages you visit are listed here." : "Nothing matches.") + "</p>";
    else if (shown < matched)
        body += "<p><a class=\"btn\" href=\"orbisweb://history?limit=" + std::to_string(limit + 700) + (search.empty() ? "" : "&q=" + url_escape(search)) +
                "\">Show more (" + std::to_string(matched - shown) + " more)</a></p>";
    return page_html("history", "History", body, notice, acted);
}

// --- Bookmarks ----------------------------------------------------------------------------------------

struct Bookmark {
    std::string uri, title;
    time_t when = 0;
};

constexpr const char *BOOKMARKS_FILE = WISP_DATA "/bookmarks.tsv";
std::vector<Bookmark> bookmarks;   // in the order they were added
bool bookmarks_loaded = false;

void load_bookmarks()
{
    if (bookmarks_loaded)
        return;
    bookmarks_loaded = true;
    FILE *f = fopen(BOOKMARKS_FILE, "r");
    if (!f)
        return;
    char line[8192];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *title = strchr(line, '\t');
        if (!title)
            continue;
        *title++ = 0;
        char *when = strchr(title, '\t');
        if (when)
            *when++ = 0;
        bookmarks.push_back({line, title, when ? static_cast<time_t>(atoll(when)) : 0});
    }
    fclose(f);
}

void save_bookmarks()
{
    const std::string temp = std::string(BOOKMARKS_FILE) + ".new";
    if (FILE *f = fopen(temp.c_str(), "w")) {
        for (const Bookmark &b : bookmarks)
            fprintf(f, "%s\t%s\t%lld\n", b.uri.c_str(), b.title.c_str(), static_cast<long long>(b.when));
        fclose(f);
        rename(temp.c_str(), BOOKMARKS_FILE);
    }
}

std::string bookmarks_html(const std::string &notice, bool acted)
{
    load_bookmarks();
    std::string body = "<form action=\"orbisweb://bookmarks\" method=\"get\"><input type=\"hidden\" name=\"do\" value=\"add\"><input type=\"hidden\" name=\"t\" value=\"" +
                       g_token + "\"><input type=\"url\" name=\"uri\" placeholder=\"An address to bookmark\"><input type=\"text\" name=\"title\" placeholder=\"Its name\" style=\"width:300px\">"
                       "<button class=\"btn\" type=\"submit\">Add</button></form>";
    body += "<p class=\"dim\">OPTIONS &gt; Add bookmark adds the page on show.</p>";
    if (bookmarks.empty())
        body += "<p class=\"empty\">No bookmarks yet.</p>";
    else
        body += "<div class=\"card\">";
    for (auto it = bookmarks.rbegin(); it != bookmarks.rend(); ++it)
        body += "<div class=\"entry\"><a class=\"title\" href=\"" + esc(it->uri) + "\">" + esc(it->title.empty() ? it->uri : it->title) +
                "</a><span class=\"host\">" + esc(host_of(it->uri)) + "</span><a class=\"x\" title=\"Remove\" href=\"" +
                esc(act("bookmarks", "do=remove&uri=" + url_escape(it->uri))) + "\">&times;</a></div>";
    if (!bookmarks.empty())
        body += "</div>";
    return page_html("bookmarks", "Bookmarks", body, notice, acted);
}

// --- Cookies and site data (WebKit's managers: answered when their data has come) -------------------

struct CookiesJob {
    WebKitURISchemeRequest *request = nullptr;
    std::map<std::string, std::string> q;
    std::string notice;
    bool acted = false;
    GList *sites = nullptr;     // WebKitWebsiteData (fetched)
};

WebKitWebsiteDataManager *data_manager() { return webkit_network_session_get_website_data_manager(H.session); }
WebKitCookieManager *cookie_manager() { return webkit_network_session_get_cookie_manager(H.session); }

constexpr WebKitWebsiteDataTypes SITE_DATA = static_cast<WebKitWebsiteDataTypes>(
    WEBKIT_WEBSITE_DATA_COOKIES | WEBKIT_WEBSITE_DATA_LOCAL_STORAGE | WEBKIT_WEBSITE_DATA_SESSION_STORAGE | WEBKIT_WEBSITE_DATA_INDEXEDDB_DATABASES |
    WEBKIT_WEBSITE_DATA_DISK_CACHE | WEBKIT_WEBSITE_DATA_SERVICE_WORKER_REGISTRATIONS | WEBKIT_WEBSITE_DATA_DOM_CACHE | WEBKIT_WEBSITE_DATA_HSTS_CACHE);

void cookies_render(CookiesJob *job, GList *cookies)
{
    std::string body;
    const std::string policy = wisp_setting("cookies", "no_third_party");
    std::string buttons;
    for (const Choice &c : entry_for("cookies")->choices)
        buttons += std::string("<a class=\"btn") + (policy == c.value ? " on" : "") + "\" href=\"" + act("cookies", std::string("do=set&value=") + c.value) +
                   "\">" + esc(c.label) + "</a>";
    body += "<div class=\"card\"><div class=\"row\"><div class=\"what\"><div class=\"name\">Cookies sites may keep</div></div><div class=\"btns\">" + buttons +
            "</div></div><div class=\"row\"><div class=\"what\"><div class=\"name\">Everything below</div></div><div class=\"btns\"><a class=\"btn danger\" href=\"" +
            act("cookies", "do=ask_clear&what=cookies") + "\">Remove all cookies</a><a class=\"btn danger\" href=\"" + act("cookies", "do=ask_clear&what=all") +
            "\">Remove all site data and the cache</a></div></div></div>";
    // the cookies by site: the site whose name ends the cookie's domain
    std::map<std::string, std::vector<std::string>> names;
    for (GList *l = cookies; l; l = l->next) {
        auto *cookie = static_cast<SoupCookie *>(l->data);
        std::string domain = soup_cookie_get_domain(cookie) ? soup_cookie_get_domain(cookie) : "";
        while (!domain.empty() && domain[0] == '.')
            domain.erase(domain.begin());
        std::string site = domain;
        for (GList *s = job->sites; s; s = s->next) {
            const std::string name = webkit_website_data_get_name(static_cast<WebKitWebsiteData *>(s->data));
            if (domain == name || (domain.size() > name.size() && domain.compare(domain.size() - name.size(), name.size(), name) == 0 && domain[domain.size() - name.size() - 1] == '.'))
                site = name;
        }
        names[site].push_back(soup_cookie_get_name(cookie) ? soup_cookie_get_name(cookie) : "");
    }
    std::vector<WebKitWebsiteData *> sites;
    for (GList *s = job->sites; s; s = s->next)
        sites.push_back(static_cast<WebKitWebsiteData *>(s->data));
    std::sort(sites.begin(), sites.end(), [](WebKitWebsiteData *a, WebKitWebsiteData *b) {
        return g_ascii_strcasecmp(webkit_website_data_get_name(a), webkit_website_data_get_name(b)) < 0;
    });
    body += "<h2>" + std::to_string(sites.size()) + (sites.size() == 1 ? " site" : " sites") + " with cookies or data</h2>";
    if (sites.empty())
        body += "<p class=\"empty\">No site keeps cookies or data now.</p>";
    else
        body += "<div class=\"card\">";
    for (WebKitWebsiteData *data : sites) {
        const std::string name = webkit_website_data_get_name(data);
        const WebKitWebsiteDataTypes types = webkit_website_data_get_types(data);
        std::string badges;
        const auto found = names.find(name);
        if (types & WEBKIT_WEBSITE_DATA_COOKIES)
            badges += "<span class=\"badge\">" + (found == names.end() ? std::string("Cookies") : std::to_string(found->second.size()) + (found->second.size() == 1 ? " cookie" : " cookies")) + "</span>";
        if (types & (WEBKIT_WEBSITE_DATA_LOCAL_STORAGE | WEBKIT_WEBSITE_DATA_SESSION_STORAGE))
            badges += "<span class=\"badge\">Storage</span>";
        if (types & WEBKIT_WEBSITE_DATA_INDEXEDDB_DATABASES)
            badges += "<span class=\"badge\">Databases</span>";
        if (types & (WEBKIT_WEBSITE_DATA_DISK_CACHE | WEBKIT_WEBSITE_DATA_DOM_CACHE))
            badges += "<span class=\"badge\">Cache</span>";
        if (types & WEBKIT_WEBSITE_DATA_SERVICE_WORKER_REGISTRATIONS)
            badges += "<span class=\"badge\">Service worker</span>";
        std::string cookie_names;
        if (found != names.end()) {
            for (size_t i = 0; i < found->second.size() && i < 6; i++)
                cookie_names += (i ? ", " : "") + esc(found->second[i]);
            if (found->second.size() > 6)
                cookie_names += ", ...";
        }
        body += "<div class=\"row\"><div class=\"what\"><div class=\"name\">" + esc(name) + "</div><div>" + badges + "</div>" +
                (cookie_names.empty() ? std::string() : "<div class=\"desc\">" + cookie_names + "</div>") + "</div><div class=\"btns\"><a class=\"btn\" href=\"" +
                esc(act("cookies", "do=remove&site=" + url_escape(name))) + "\">Remove</a></div></div>";
    }
    if (!sites.empty())
        body += "</div>";
    finish(job->request, page_html("cookies", "Cookies and site data", body, job->notice, job->acted));
}

void cookies_done(CookiesJob *job)
{
    g_list_free_full(job->sites, reinterpret_cast<GDestroyNotify>(webkit_website_data_unref));
    g_object_unref(job->request);
    delete job;
}

// the list: the sites' data, then every cookie (for their names)
void cookies_fetch(CookiesJob *job)
{
    webkit_website_data_manager_fetch(data_manager(), SITE_DATA, nullptr, +[](GObject *source, GAsyncResult *result, gpointer data) {
        auto *job = static_cast<CookiesJob *>(data);
        job->sites = webkit_website_data_manager_fetch_finish(WEBKIT_WEBSITE_DATA_MANAGER(source), result, nullptr);
        webkit_cookie_manager_get_all_cookies(cookie_manager(), nullptr, +[](GObject *source, GAsyncResult *result, gpointer data) {
            auto *job = static_cast<CookiesJob *>(data);
            GList *cookies = webkit_cookie_manager_get_all_cookies_finish(WEBKIT_COOKIE_MANAGER(source), result, nullptr);
            cookies_render(job, cookies);
            g_list_free_full(cookies, reinterpret_cast<GDestroyNotify>(soup_cookie_free));
            cookies_done(job);
        }, job);
    }, job);
}

void cookies_serve(WebKitURISchemeRequest *request, const std::map<std::string, std::string> &q, bool token_ok)
{
    auto *job = new CookiesJob;
    job->request = WEBKIT_URI_SCHEME_REQUEST(g_object_ref(request));
    job->q = q;
    const auto get = [&](const char *k) { const auto it = q.find(k); return it == q.end() ? std::string() : it->second; };
    const std::string what = get("do");
    if (!token_ok || what.empty()) {
        cookies_fetch(job);
        return;
    }
    job->acted = true;
    if (what == "set") {
        std::string said;
        set_setting("cookies", get("value"), said);
        job->notice = notice_html(said);
        cookies_fetch(job);
    } else if (what == "ask_clear") {
        const bool all = get("what") == "all";
        job->acted = false;   // (the question's own address: its answer acts)
        job->notice = notice_html(std::string(all ? "Remove every site's cookies, storage, databases and the cache?" : "Remove every site's cookies?") +
                                  " Sites you're signed in to will sign you out. <a class=\"btn danger\" href=\"" + act("cookies", std::string("do=clear&what=") + (all ? "all" : "cookies")) +
                                  "\">Remove</a> <a class=\"btn\" href=\"orbisweb://cookies\">Cancel</a>", true);
        cookies_fetch(job);
    } else if (what == "clear") {
        const bool all = get("what") == "all";
        job->notice = notice_html(all ? "Every site's cookies, data and the cache are removed." : "Every cookie is removed.");
        wisp_log("cookies: %s removed (the Cookies page)", all ? "all site data" : "all cookies");
        webkit_website_data_manager_clear(data_manager(), all ? WEBKIT_WEBSITE_DATA_ALL : WEBKIT_WEBSITE_DATA_COOKIES, 0, nullptr,
            +[](GObject *source, GAsyncResult *result, gpointer data) {
                webkit_website_data_manager_clear_finish(WEBKIT_WEBSITE_DATA_MANAGER(source), result, nullptr);
                cookies_fetch(static_cast<CookiesJob *>(data));
            }, job);
    } else if (what == "remove") {
        // the site's data as WebKit lists it (removal takes those objects), then removed
        webkit_website_data_manager_fetch(data_manager(), SITE_DATA, nullptr, +[](GObject *source, GAsyncResult *result, gpointer data) {
            auto *job = static_cast<CookiesJob *>(data);
            GList *all = webkit_website_data_manager_fetch_finish(WEBKIT_WEBSITE_DATA_MANAGER(source), result, nullptr);
            const std::string site = job->q.count("site") ? job->q.at("site") : "";
            GList *chosen = nullptr;
            for (GList *l = all; l; l = l->next)
                if (site == webkit_website_data_get_name(static_cast<WebKitWebsiteData *>(l->data)))
                    chosen = g_list_prepend(chosen, l->data);
            job->notice = notice_html(chosen ? esc(site) + ": its cookies and data are removed." : esc(site) + " keeps nothing now.");
            if (!chosen) {
                g_list_free_full(all, reinterpret_cast<GDestroyNotify>(webkit_website_data_unref));
                cookies_fetch(job);
                return;
            }
            wisp_log("cookies: %s's data removed (the Cookies page)", site.c_str());
            // (all keeps the references until the removal has ended)
            g_object_set_data_full(G_OBJECT(job->request), "wisp-sites", all, [](gpointer list) {
                g_list_free_full(static_cast<GList *>(list), reinterpret_cast<GDestroyNotify>(webkit_website_data_unref));
            });
            webkit_website_data_manager_remove(WEBKIT_WEBSITE_DATA_MANAGER(source), WEBKIT_WEBSITE_DATA_ALL, chosen, nullptr,
                +[](GObject *source, GAsyncResult *result, gpointer data) {
                    webkit_website_data_manager_remove_finish(WEBKIT_WEBSITE_DATA_MANAGER(source), result, nullptr);
                    cookies_fetch(static_cast<CookiesJob *>(data));
                }, job);
            g_list_free(chosen);
        }, job);
    } else {
        cookies_fetch(job);
    }
}

} // namespace

// --- Public ---------------------------------------------------------------------------------------------

void init(const Host &host)
{
    H = host;
    char token[33];
    snprintf(token, sizeof token, "%08x%08x%08x%08x", g_random_int(), g_random_int(), g_random_int(), static_cast<unsigned>(time(nullptr)) ^ g_random_int());
    g_token = token;
}

void note_visit(const std::string &uri)
{
    if (uri.rfind("http://", 0) != 0 && uri.rfind("https://", 0) != 0)
        return;
    load_history();
    const time_t now = time(nullptr);
    // (the same page again within a minute, a reload or a redirect back: one entry)
    if (!history.empty() && history.back().uri == uri && now - history.back().when < 60) {
        history.back().when = now;
    } else {
        history.push_back({now, one_line(uri), ""});
        if (history.size() > HISTORY_MAX)
            history.erase(history.begin(), history.begin() + (history.size() - HISTORY_MAX));
    }
    save_history_soon();
}

void note_title(const std::string &uri, const std::string &title)
{
    if (title.empty() || !history_loaded)
        return;
    for (size_t n = 0; n < history.size() && n < 50; n++) {
        Visit &v = history[history.size() - 1 - n];
        if (v.uri == uri) {
            if (v.title != title) {
                v.title = one_line(title);
                save_history_soon();
            }
            return;
        }
    }
}

std::vector<Suggestion> suggestions(const std::string &typed, size_t max)
{
    load_history();
    load_bookmarks();
    const auto lower = [](std::string s) {
        for (char &c : s)
            c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
        return s;
    };
    const std::string want = lower(typed);
    // an address as the list compares it: no scheme, no "www.", no trailing slash ("youtube" finds
    // https://www.youtube.com/ by its start too)
    const auto bare = [](const std::string &uri) {
        std::string s = uri;
        for (const char *scheme : {"https://", "http://"})
            if (s.rfind(scheme, 0) == 0) {
                s.erase(0, strlen(scheme));
                break;
            }
        if (s.rfind("www.", 0) == 0)
            s.erase(0, 4);
        if (!s.empty() && s.back() == '/')
            s.pop_back();
        return s;
    };
    const auto matches = [&](const std::string &uri, const std::string &title) {
        return want.empty() || lower(uri).find(want) != std::string::npos || lower(title).find(want) != std::string::npos;
    };
    std::vector<Suggestion> list;
    std::vector<std::string> seen;   // (bare addresses already in the list)
    const auto add = [&](const std::string &uri, const std::string &title, bool bookmark) {
        const std::string key = bare(uri);
        if (key.empty() || std::find(seen.begin(), seen.end(), key) != seen.end())
            return;
        seen.push_back(key);
        list.push_back({uri, title, bookmark});
    };
    // bookmarks: the ones typed for (a few, so the history still shows), or none before anything is typed
    if (!want.empty())
        for (const Bookmark &b : bookmarks)
            if (list.size() < std::min<size_t>(max, 3) && matches(b.uri, b.title))
                add(b.uri, b.title, true);
    // the history, newest first: an address whose start is what was typed before one that only has it inside
    std::vector<const Visit *> later;
    for (auto it = history.rbegin(); it != history.rend() && list.size() < max; ++it) {
        if (!matches(it->uri, it->title))
            continue;
        if (!want.empty() && lower(bare(it->uri)).rfind(want, 0) != 0) {
            if (later.size() < max)
                later.push_back(&*it);
            continue;
        }
        add(it->uri, it->title, false);
    }
    for (const Visit *v : later) {
        if (list.size() >= max)
            break;
        add(v->uri, v->title, false);
    }
    return list;
}

bool bookmarked(const std::string &uri)
{
    load_bookmarks();
    return std::any_of(bookmarks.begin(), bookmarks.end(), [&](const Bookmark &b) { return b.uri == uri; });
}

bool toggle_bookmark(const std::string &uri, const std::string &title)
{
    load_bookmarks();
    const auto it = std::find_if(bookmarks.begin(), bookmarks.end(), [&](const Bookmark &b) { return b.uri == uri; });
    const bool add = it == bookmarks.end();
    if (add)
        bookmarks.push_back({one_line(uri), one_line(title), time(nullptr)});
    else
        bookmarks.erase(it);
    save_bookmarks();
    return add;
}

bool serve(WebKitURISchemeRequest *request, const std::string &page, const std::string &query)
{
    if (page != "settings" && page != "history" && page != "bookmarks" && page != "cookies")
        return false;
    const std::map<std::string, std::string> q = parse_query(query);
    const auto get = [&](const char *k) { const auto it = q.find(k); return it == q.end() ? std::string() : it->second; };
    const bool token_ok = !get("t").empty() && get("t") == g_token;
    const std::string what = token_ok ? get("do") : std::string();
    if (!get("do").empty() && !token_ok)
        wisp_log("pages: %s?do=%s without this run's token: shown, not done", page.c_str(), get("do").c_str());

    if (page == "cookies") {
        cookies_serve(request, q, token_ok);
        return true;
    }
    std::string notice;
    bool acted = !what.empty();
    if (page == "settings") {
        if (what == "set") {
            std::string said;
            const bool ok = set_setting(get("key"), get("value"), said);
            notice = notice_html(said, !ok);
        } else if (what == "restart" && H.restart) {
            H.restart();
            notice = notice_html("Restarting...");
        } else if (what == "downloads" && H.open_downloads) {
            H.open_downloads();
            notice = notice_html("The Downloads tab is open: its Change the folder chooses where downloads go.");
        } else if (what == "tone") {
            // (v00.35) the UI's own tone: the PS4's audio out from the app's own process, no page in it
            wisp_audio_tone(1.0);
            notice = notice_html("A second's tone from OrbisWeb itself. Heard this one and not the page's? Then a page's sound stops on its "
                                 "way here: the \"audio:\" lines in logs/web.log and logs/ui.log say where.");
        } else if (what == "webgpu_test" && H.webgpu_test) {
            H.webgpu_test();
            notice = notice_html("Restarting for the WebGPU test...");
        } else if (what == "ask_clear") {
            acted = false;
            notice = notice_html("Clear the history, every cookie, every site's data and the cache? Sites you're signed in to will sign you out. <a class=\"btn danger\" href=\"" +
                                 act("settings", "do=clear") + "\">Clear</a> <a class=\"btn\" href=\"orbisweb://settings\">Cancel</a>", true);
        } else if (what == "clear") {
            load_history();
            history.clear();
            save_history_now();
            webkit_website_data_manager_clear(data_manager(), WEBKIT_WEBSITE_DATA_ALL, 0, nullptr, nullptr, nullptr);
            wisp_log("pages: browsing data cleared (history, cookies, site data, cache)");
            notice = notice_html("The history, cookies, site data and the cache are cleared.");
        }
        finish(request, settings_html(notice, acted));
        return true;
    }
    if (page == "history") {
        if (what == "ask_clear") {
            acted = false;
            notice = notice_html("Clear the whole browsing history? <a class=\"btn danger\" href=\"" + act("history", "do=clear") +
                                 "\">Clear</a> <a class=\"btn\" href=\"orbisweb://history\">Cancel</a>", true);
        } else if (what == "clear") {
            load_history();
            history.clear();
            save_history_now();
            wisp_log("pages: history cleared");
            notice = notice_html("The browsing history is cleared.");
        } else if (what == "remove") {
            load_history();
            const time_t when = static_cast<time_t>(atoll(get("when").c_str()));
            const std::string uri = get("uri");
            history.erase(std::remove_if(history.begin(), history.end(), [&](const Visit &v) { return v.when == when && v.uri == uri; }), history.end());
            save_history_soon();
            notice = notice_html("Removed from the history.");
        }
        finish(request, history_html(q, notice, acted));
        return true;
    }
    // bookmarks
    if (what == "add") {
        std::string uri = get("uri");
        while (!uri.empty() && isspace(static_cast<unsigned char>(uri.back())))
            uri.pop_back();
        if (!uri.empty() && uri.find("://") == std::string::npos)
            uri = "https://" + uri;
        if (uri.empty())
            notice = notice_html("Type an address to bookmark.", true);
        else if (bookmarked(uri))
            notice = notice_html("That address is bookmarked already.");
        else {
            toggle_bookmark(uri, get("title"));
            notice = notice_html("Bookmarked.");
        }
    } else if (what == "remove") {
        if (bookmarked(get("uri")))
            toggle_bookmark(get("uri"), "");
        notice = notice_html("Removed from the bookmarks.");
    }
    finish(request, bookmarks_html(notice, acted));
    return true;
}

} // namespace wisp::pages
