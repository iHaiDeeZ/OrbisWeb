// Wisp's download manager (ui/dm/dm.h): libaria2 on a thread of its own, its page in a tab.
//
// libaria2 is not thread-safe: every call is made on the engine's thread, which runs aria2's event
// loop one turn at a time (aria2::run RUN_ONCE: up to a second each), takes the page's commands between
// turns and twice a second copies what each download is doing into a snapshot the page draws from.
// The engine's state survives a restart of Wisp: aria2's session file (unfinished downloads, with
// their options) and its control files beside each file (what parts are done); the finished ones are
// in a history file of Wisp's own.
#include "dm.h"

#include "../../common/wisp.h"

#if defined(WISP_HAVE_ARIA2)
#include <aria2/aria2.h>
#endif
#include <glib.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <fcntl.h>
#include <map>
#include <mutex>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>

namespace wisp::dm {
namespace {

using ps4ui::Canvas;
using ps4ui::Color;
using ps4ui::rgb;
using ps4ui::Weight;

constexpr const char *ARIA2_DIR = WISP_DATA "/aria2";
constexpr const char *SESSION_FILE = WISP_DATA "/aria2/session.txt";
constexpr const char *HISTORY_FILE = WISP_DATA "/aria2/history.tsv";

// the dark theme (the browser's)
const Color BG = rgb(0x1f2023);
const Color ROW = rgb(0x2b2c30);
const Color ROW_ON = rgb(0x3a3b40);
const Color TEXT = rgb(0xe8eaed);
const Color DIM = rgb(0x9aa0a6);
const Color ACCENT = rgb(0x8ab4f8);
const Color HOVER = rgb(0x45464c);
const Color GOOD = rgb(0x81c995);
const Color BAD = rgb(0xf28b82);
const Color WARN = rgb(0xfdd663);

enum class State { Active, Waiting, Paused, Complete, Error, Removed };

struct Item {
    uint64_t gid = 0;        // aria2's; 0 for WebKit's downloads and the history's
    int external = 0;        // WebKit's download (external_start); 0 otherwise
    std::string name, path, uri;
    int64_t total = 0, done = 0;
    int down = 0, up = 0, connections = 0;
    State state = State::Waiting;
    int error = 0;
    bool torrent = false;
    bool hidden = false;     // a .torrent / .metalink file another download took over
    double added = 0;
    time_t finished_at = 0;
};

// --- The engine's side (its thread) --------------------------------------------------------------------

struct Command {
    enum Kind { Add, Pause, Resume, Remove, PauseAll, ResumeAll, Forget, Stop } kind;
    uint64_t gid = 0;
    std::string uri, name;
    std::vector<std::string> headers;
    std::string dir;   // Add: the folder it goes to (v00.29: the one chosen then; "" WISP_DOWNLOADS)
};

// what the engine remembers of each download it was given (aria2's handles say the rest)
struct Meta {
    std::string uri, name;
    std::vector<std::string> headers;
    bool from_page = false;
    double added = 0;
    bool handed_over = false;   // given to WebKit's downloader after aria2 failed it
    bool reported = false;      // its end said (toast, history)
};

Host H;
std::mutex lock;                       // guards everything below it shared by the two threads
std::vector<Command> commands;
std::vector<Item> snapshot;            // the engine's downloads, as of its last look
int speed_down = 0, speed_up = 0;
std::vector<std::string> notices;      // toasts for the UI thread
std::vector<std::string> fallbacks;    // links for WebKit's downloader (UI thread)
bool snapshot_new = false;
std::atomic<bool> engine_up{false}, engine_failed{false}, stopping{false};
pthread_t engine_thread;

[[maybe_unused]] std::map<uint64_t, Meta> metas;   // (the engine's thread only)

std::string base_name(const std::string &path)
{
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

[[maybe_unused]] bool exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

std::string name_from_uri(const std::string &uri)
{
    if (uri.rfind("magnet:", 0) == 0) {
        const size_t dn = uri.find("dn=");
        if (dn != std::string::npos) {
            std::string name = uri.substr(dn + 3, uri.find('&', dn) - dn - 3);
            char *plain = g_uri_unescape_string(name.c_str(), nullptr);
            if (plain) {
                name = plain;
                g_free(plain);
            }
            for (char &c : name)
                if (c == '+')
                    c = ' ';
            return name;
        }
        return "Magnet link";
    }
    std::string path = uri.substr(0, uri.find_first_of("?#"));
    std::string name = base_name(path);
    char *plain = g_uri_unescape_string(name.c_str(), nullptr);
    if (plain) {
        name = plain;
        g_free(plain);
    }
    return name.empty() ? uri : name;
}

void history_append(const Item &item)
{
    static std::mutex file_lock;   // (the engine's thread and the UI's both add lines)
    std::lock_guard<std::mutex> guard(file_lock);
    mkdir(ARIA2_DIR, 0777);
    if (FILE *f = fopen(HISTORY_FILE, "a")) {
        fprintf(f, "%s\t%s\t%lld\t%s\t%lld\n", item.name.c_str(), item.path.c_str(), static_cast<long long>(item.total),
            item.state == State::Complete ? "done" : "error", static_cast<long long>(time(nullptr)));
        fclose(f);
    }
}

#if defined(WISP_HAVE_ARIA2)
int on_event(aria2::Session *, aria2::DownloadEvent event, aria2::A2Gid gid, void *)
{
    // (a download the last run left, restored from the session file, is seen here first)
    if (event == aria2::EVENT_ON_DOWNLOAD_START && !metas.count(gid))
        metas[gid].added = wisp_seconds();
    return 0;
}
#endif

const char *error_words(int code)
{
    switch (code) {
    case 2: return "timed out";
    case 3: return "not found on the server";
    case 5: return "too slow";
    case 6: return "network problem";
    case 7: return "unfinished when OrbisWeb stopped";
    case 8: return "the server cannot resume";
    case 9: return "not enough space";
    case 13: return "the file is there already";
    case 16: case 17: case 18: return "could not write the file";
    case 19: return "the server's name could not be found";
    case 22: return "the server refused it";
    case 23: return "too many redirections";
    case 24: return "the server asked for a password";
    case 26: case 25: return "the torrent is damaged";
    case 27: return "a bad magnet link";
    case 29: return "the server is busy";
    case 32: return "it came damaged (checksum)";
    default: return "failed";
    }
}

#if defined(WISP_HAVE_ARIA2)
void engine_snapshot(aria2::Session *session)
{
    // every download aria2 has now, and the ones it was given (finished ones stay with it)
    for (aria2::A2Gid gid : aria2::getActiveDownload(session))
        if (!metas.count(gid))
            metas[gid].added = wisp_seconds();
    std::vector<Item> items;
    std::vector<std::string> said, handed;
    std::vector<uint64_t> gone;
    std::vector<std::pair<uint64_t, std::string>> follow;
    for (auto &[gid, meta] : metas) {
        aria2::DownloadHandle *handle = aria2::getDownloadHandle(session, gid);
        if (!handle) {
            gone.push_back(gid);
            continue;
        }
        Item item;
        item.gid = gid;
        item.uri = meta.uri;
        item.added = meta.added;
        item.total = handle->getTotalLength();
        item.done = handle->getCompletedLength();
        item.down = handle->getDownloadSpeed();
        item.up = handle->getUploadSpeed();
        item.connections = handle->getConnections();
        item.error = handle->getErrorCode();
        item.torrent = !handle->getInfoHash().empty();
        switch (handle->getStatus()) {
        case aria2::DOWNLOAD_ACTIVE: item.state = State::Active; break;
        case aria2::DOWNLOAD_WAITING: item.state = State::Waiting; break;
        case aria2::DOWNLOAD_PAUSED: item.state = State::Paused; break;
        case aria2::DOWNLOAD_COMPLETE: item.state = State::Complete; break;
        case aria2::DOWNLOAD_ERROR: item.state = State::Error; break;
        case aria2::DOWNLOAD_REMOVED: item.state = State::Removed; break;
        }
        // a .torrent or .metalink file whose download went on as another one: that one is shown (added
        // to the map after this walk: the map must not change while it is walked)
        const std::vector<aria2::A2Gid> &followed = handle->getFollowedBy();
        for (aria2::A2Gid next : followed)
            if (!metas.count(next))
                follow.emplace_back(next, meta.uri);
        item.hidden = !followed.empty();
        if (item.torrent) {
            const std::string name = handle->getBtMetaInfo().name;
            item.name = name.empty() ? (meta.name.empty() ? name_from_uri(meta.uri) : meta.name) : name;
            item.path = handle->getDir() + "/" + item.name;
        } else {
            const std::vector<aria2::FileData> files = handle->getFiles();
            if (!files.empty() && !files[0].path.empty())
                item.path = files[0].path;
            // (v00.33) a download from the last run's session has no record of this run's: its address from aria2
            if (item.uri.empty() && !files.empty() && !files[0].uris.empty())
                item.uri = files[0].uris[0].uri;
            item.name = !item.path.empty() ? base_name(item.path) : (meta.name.empty() ? name_from_uri(item.uri) : meta.name);
        }
        aria2::deleteDownloadHandle(handle);
        // its end, once: a toast, the history; a page's download aria2 could not have goes to WebKit
        if (!meta.reported && !item.hidden && (item.state == State::Complete || item.state == State::Error)) {
            meta.reported = true;
            if (item.state == State::Complete) {
                said.push_back("Downloaded " + item.name);
                item.finished_at = time(nullptr);
                history_append(item);
            } else if (meta.from_page && !item.done && (item.error == 22 || item.error == 24 || item.error == 3) && !meta.handed_over) {
                meta.handed_over = true;
                handed.push_back(meta.uri);
                said.push_back(item.name + ": the server refused the download manager; the browser downloads it");
            } else if (meta.uri.empty()) {
                // (v00.33) one the last run left, failing again as it comes back: the Downloads list says
                // so; v00.32 toasted each at every start, without a name
                wisp_log("dm: %s, from the last run, failed again (%s)", item.name.empty() ? "a download" : item.name.c_str(),
                    error_words(item.error));
            } else {
                said.push_back("Download failed: " + item.name + " (" + error_words(item.error) + ")");
            }
        }
        if (!meta.handed_over)
            items.push_back(item);
    }
    for (uint64_t gid : gone)
        metas.erase(gid);
    for (auto &[gid, uri] : follow) {
        Meta &meta = metas[gid];
        meta.added = wisp_seconds();
        meta.uri = uri;
    }
    const std::vector<std::string> &links = handed;
    const aria2::GlobalStat stat = aria2::getGlobalStat(session);
    std::lock_guard<std::mutex> guard(lock);
    snapshot = std::move(items);
    speed_down = stat.downloadSpeed;
    speed_up = stat.uploadSpeed;
    notices.insert(notices.end(), said.begin(), said.end());
    fallbacks.insert(fallbacks.end(), links.begin(), links.end());
    snapshot_new = true;
}

void engine_apply(aria2::Session *session, std::vector<Command> &todo)
{
    for (Command &command : todo) {
        switch (command.kind) {
        case Command::Add: {
            aria2::KeyVals options;
            // its own folder (aria2's session file keeps it: a download the last run left goes on
            // where it started, whatever folder is chosen now; the global dir stays Downloads for the
            // ones v00.28 and before left)
            options.emplace_back("dir", command.dir.empty() ? std::string(WISP_DOWNLOADS) : command.dir);
            for (const std::string &header : command.headers)
                options.emplace_back("header", header);
            if (!command.name.empty())
                options.emplace_back("out", command.name);
            aria2::A2Gid gid = 0;
            const int result = aria2::addUri(session, &gid, { command.uri }, options);
            if (result == 0) {
                Meta &meta = metas[gid];
                meta.uri = command.uri;
                meta.name = command.name;
                meta.headers = command.headers;
                meta.from_page = !command.headers.empty();
                meta.added = wisp_seconds();
                wisp_log("dm: added %s (gid %s, into %s)", command.uri.c_str(), aria2::gidToHex(gid).c_str(),
                    command.dir.empty() ? WISP_DOWNLOADS : command.dir.c_str());
            } else {
                wisp_log("dm: aria2 refused %s (%d)", command.uri.c_str(), result);
                std::lock_guard<std::mutex> guard(lock);
                notices.push_back("The download manager cannot take this link");
                if (!command.headers.empty())
                    fallbacks.push_back(command.uri);
            }
            break;
        }
        case Command::Pause:
            aria2::pauseDownload(session, command.gid, false);
            break;
        case Command::Resume:
            aria2::unpauseDownload(session, command.gid);
            break;
        case Command::Remove: {
            auto it = metas.find(command.gid);
            aria2::removeDownload(session, command.gid, true);
            if (it != metas.end())
                metas.erase(it);
            break;
        }
        case Command::PauseAll:
            for (auto &[gid, meta] : metas)
                aria2::pauseDownload(session, gid, false);
            break;
        case Command::ResumeAll:
            for (auto &[gid, meta] : metas)
                aria2::unpauseDownload(session, gid);
            break;
        case Command::Forget: {   // the finished ones out of the list
            std::vector<uint64_t> done;
            for (auto &[gid, meta] : metas)
                if (meta.reported)
                    done.push_back(gid);
            for (uint64_t gid : done)
                metas.erase(gid);
            break;
        }
        case Command::Stop:
            break;
        }
    }
}

void *engine_main(void *)
{
    mkdir(WISP_DATA, 0777);
    mkdir(WISP_DOWNLOADS, 0777);
    mkdir(ARIA2_DIR, 0777);
    if (aria2::libraryInit() != 0) {
        wisp_log("dm: aria2's library did not start: the browser's own downloader takes every download");
        engine_failed = true;
        return nullptr;
    }
    const int split = std::clamp(wisp_setting_int("dm_connections", 8), 1, 16);
    aria2::KeyVals options = {
        { "dir", WISP_DOWNLOADS },
        { "no-conf", "true" },                  // (no $HOME/.aria2/aria2.conf: HOME is the browser's profile)
        { "disable-ipv6", "true" },             // (the kit's resolver answers IPv4 only)
        { "file-allocation", "none" },          // (the PS4's C library has no fallocate)
        { "continue", "true" },
        { "max-concurrent-downloads", std::to_string(std::clamp(wisp_setting_int("dm_parallel", 3), 1, 8)) },
        { "split", std::to_string(split) },
        { "max-connection-per-server", std::to_string(split) },
        { "min-split-size", "1M" },
        { "user-agent", H.user_agent },
        { "ca-certificate", WISP_APP "/wisp/cacert.pem" },
        { "check-certificate", "true" },
        { "auto-file-renaming", "true" },
        { "allow-overwrite", "false" },
        { "max-tries", "5" },
        { "retry-wait", "3" },
        { "connect-timeout", "20" },
        { "timeout", "40" },
        { "disk-cache", "16M" },
        { "summary-interval", "0" },
        { "console-log-level", "error" },
        { "log", WISP_LOGS "/aria2.log" },
        { "log-level", wisp_setting_int("dm_log", 1) ? "notice" : "error" },
        // BitTorrent and magnet links: the DHT (IPv4: the console's network), no seeding after the
        // download unless asked (settings: dm_seed_minutes)
        { "enable-dht", "true" },
        { "enable-dht6", "false" },
        { "dht-file-path", WISP_DATA "/aria2/dht.dat" },
        // (a node to start from while dht.dat is empty: a magnet link without trackers finds no peers
        // otherwise)
        { "dht-entry-point", "dht.transmissionbt.com:6881" },
        { "bt-enable-lpd", "false" },
        // every peer, connection and open file is a descriptor of the ~250 the app's processes have
        { "bt-max-peers", "30" },
        { "bt-max-open-files", "16" },
        { "enable-peer-exchange", "true" },
        { "follow-torrent", "mem" },
        { "follow-metalink", "mem" },
        { "bt-save-metadata", "true" },
        { "seed-time", std::to_string(std::max(0, wisp_setting_int("dm_seed_minutes", 0))) },
        { "max-overall-upload-limit", "1M" },
        { "listen-port", "6881-6889" },
        { "dht-listen-port", "6881-6889" },
        // unfinished downloads (and their options) in a session file, for the next run
        { "save-session", SESSION_FILE },
        { "save-session-interval", "20" },
        { "auto-save-interval", "20" },
        { "force-save", "false" },
    };
    if (exists(SESSION_FILE))
        options.emplace_back("input-file", SESSION_FILE);
    aria2::SessionConfig config;
    config.keepRunning = true;
    config.useSignalHandler = false;
    config.downloadEventCallback = on_event;
    aria2::Session *session = aria2::sessionNew(options, config);
    if (!session) {
        wisp_log("dm: aria2 refused its options: the browser's own downloader takes every download");
        aria2::libraryDeinit();
        engine_failed = true;
        return nullptr;
    }
    engine_up = true;
    wisp_log("dm: aria2 running (%s; %d connections a file)", exists(SESSION_FILE) ? "the last run's downloads back" : "no downloads left from before", split);
    double last_look = 0;
    for (;;) {
        aria2::run(session, aria2::RUN_ONCE);
        std::vector<Command> todo;
        {
            std::lock_guard<std::mutex> guard(lock);
            todo.swap(commands);
        }
        engine_apply(session, todo);
        if (stopping)
            break;
        const double now = wisp_seconds();
        if (now - last_look >= 0.5) {
            last_look = now;
            engine_snapshot(session);
        }
    }
    // what is unfinished goes to the session file as aria2 ends (forced: no waiting on trackers, which
    // would hold Leave / Restart for seconds; aria2 picks the downloads up from the file next time)
    aria2::shutdown(session, true);
    for (int turns = 0; turns < 5 && aria2::run(session, aria2::RUN_ONCE) == 1; turns++) { }
    aria2::sessionFinal(session);
    aria2::libraryDeinit();
    engine_up = false;
    wisp_log("dm: aria2 stopped (its session saved)");
    return nullptr;
}
#else
void *engine_main(void *)
{
    wisp_log("dm: built without aria2: the browser's own downloader takes every download");
    engine_failed = true;
    return nullptr;
}
#endif

void post(Command command)
{
    std::lock_guard<std::mutex> guard(lock);
    commands.push_back(std::move(command));
}

// --- The page's side (the UI thread) ---------------------------------------------------------------------

void chooser_open();   // (Change the folder: below)

struct Page {
    bool visible = false;
    bool dirty = true;
    std::vector<Item> items;       // what is drawn: the engine's, WebKit's, the history's
    std::vector<Item> externals;   // WebKit's downloads
    std::vector<Item> history;     // finished before this run
    int next_external = 1;
    int selected = 0;
    float scroll = 0;
    int x = 0, y = 0, w = 1920, h = 972;
    int hover_row = -1, hover_button = -1;   // the row and the button (0-2) under the pointer, -1 none
    int hover_bar = -1;                      // the top bar's button under the pointer
    int down = 0, up = 0;
    double last_click = 0;
    int last_click_row = -1;
} P;

constexpr int BAR_H = 140, ROW_H = 92, ROW_GAP = 8, LIST_X = 40;
// the top bar's buttons: on its first line, then (v00.29) Change the folder on its second, after the
// folder downloads go to
const char *const BAR_BUTTONS[] = { "Add a link", "Pause all", "Resume all", "Clear finished", "Open the folder", "Change the folder" };
constexpr int BAR_COUNT = 6, BAR_LINE1 = 5;
constexpr int BAR2_Y = 88, BAR2_H = 38;

void load_history()
{
    P.history.clear();
    FILE *f = fopen(HISTORY_FILE, "r");
    if (!f)
        return;
    char line[4096];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\n")] = 0;
        char *fields[5] = {};
        int n = 0;
        for (char *p = line; n < 5; n++) {
            fields[n] = p;
            char *tab = strchr(p, '\t');
            if (!tab) {
                n++;
                break;
            }
            *tab = 0;
            p = tab + 1;
        }
        if (n < 5)
            continue;
        Item item;
        item.name = fields[0];
        item.path = fields[1];
        item.total = item.done = atoll(fields[2]);
        item.state = !strcmp(fields[3], "done") ? State::Complete : State::Error;
        item.finished_at = static_cast<time_t>(atoll(fields[4]));
        P.history.push_back(item);
    }
    fclose(f);
    if (P.history.size() > 200)
        P.history.erase(P.history.begin(), P.history.end() - 200);
}

// the list: running and waiting first (newest first), then the finished (newest first)
void rebuild()
{
    std::vector<Item> engine;
    {
        std::lock_guard<std::mutex> guard(lock);
        engine = snapshot;
        P.down = speed_down;
        P.up = speed_up;
    }
    std::vector<Item> live, done;
    for (Item &item : engine) {
        if (item.hidden || item.state == State::Removed)
            continue;
        (item.state == State::Complete || item.state == State::Error ? done : live).push_back(item);
    }
    for (const Item &item : P.externals)
        (item.state == State::Complete || item.state == State::Error ? done : live).push_back(item);
    std::sort(live.begin(), live.end(), [](const Item &a, const Item &b) { return a.added > b.added; });
    std::sort(done.begin(), done.end(), [](const Item &a, const Item &b) { return a.finished_at > b.finished_at; });
    P.items = std::move(live);
    P.items.insert(P.items.end(), done.begin(), done.end());
    // the history's, unless this run has them already (a path finished again)
    for (auto it = P.history.rbegin(); it != P.history.rend(); ++it) {
        const bool listed = std::any_of(P.items.begin(), P.items.end(), [&](const Item &i) { return !i.path.empty() && i.path == it->path; });
        if (!listed)
            P.items.push_back(*it);
    }
    P.selected = std::clamp(P.selected, 0, std::max(0, static_cast<int>(P.items.size()) - 1));
    P.dirty = true;
}

std::string size_text(int64_t bytes)
{
    char out[32];
    if (bytes >= (static_cast<int64_t>(1) << 30))
        snprintf(out, sizeof out, "%.2f GB", bytes / 1073741824.0);
    else if (bytes >= (1 << 20))
        snprintf(out, sizeof out, "%.1f MB", bytes / 1048576.0);
    else if (bytes >= 1024)
        snprintf(out, sizeof out, "%.0f KB", bytes / 1024.0);
    else
        snprintf(out, sizeof out, "%lld B", static_cast<long long>(bytes));
    return out;
}

std::string speed_text(int bytes_per_second) { return size_text(bytes_per_second) + "/s"; }

std::string time_text(double seconds)
{
    char out[32];
    const long s = static_cast<long>(seconds);
    if (s >= 3600)
        snprintf(out, sizeof out, "%ld h %02ld min", s / 3600, (s / 60) % 60);
    else
        snprintf(out, sizeof out, "%ld:%02ld", s / 60, s % 60);
    return out;
}

std::string details(const Item &item)
{
    std::string text;
    switch (item.state) {
    case State::Active:
        text = (item.total > 0 ? size_text(item.done) + " of " + size_text(item.total) : size_text(item.done)) + " · " + speed_text(item.down);
        if (item.total > 0 && item.down > 0)
            text += " · " + time_text(static_cast<double>(item.total - item.done) / item.down) + " left";
        if (item.connections > 0)
            text += " · " + std::to_string(item.connections) + (item.torrent ? " peers" : " connections");
        if (item.torrent && item.up > 0)
            text += " · up " + speed_text(item.up);
        if (item.external)
            text += " · the browser's downloader";
        break;
    case State::Waiting:
        text = item.total > 0 ? "Waiting · " + size_text(item.done) + " of " + size_text(item.total) : "Waiting";
        break;
    case State::Paused:
        text = "Paused · " + (item.total > 0 ? size_text(item.done) + " of " + size_text(item.total) : size_text(item.done));
        break;
    case State::Complete:
        text = "Done · " + size_text(item.total > 0 ? item.total : item.done);
        break;
    case State::Error:
        text = std::string("Failed: ") + (item.error ? error_words(item.error) : "the download stopped");
        break;
    case State::Removed:
        text = "Removed";
        break;
    }
    return text;
}

// a row's buttons, right to left: 0 pause / resume / open, 1 remove, 2 its folder
const char *button_label(const Item &item, int button)
{
    if (button == 0) {
        if (item.state == State::Complete)
            return "Open";
        if (item.state == State::Paused || item.state == State::Error)
            return item.gid ? "Resume" : "";
        return item.gid ? "Pause" : "";
    }
    if (button == 1)
        return item.state == State::Active || item.state == State::Waiting || item.state == State::Paused ? "Cancel" : "Remove";
    return item.path.empty() ? "" : "Folder";
}

int row_top(int i) { return BAR_H + 12 + i * (ROW_H + ROW_GAP) - static_cast<int>(P.scroll); }
int button_x(int button) { return P.w - 40 - (button + 1) * 130; }

void act(int row, int button)
{
    if (row < 0 || row >= static_cast<int>(P.items.size()))
        return;
    const Item item = P.items[row];
    if (button == 0) {
        if (item.state == State::Complete) {
            if (H.open_file && !item.path.empty())
                H.open_file(item.path);
        } else if (item.gid && (item.state == State::Paused)) {
            post({ Command::Resume, item.gid });
        } else if (item.gid && item.state == State::Error) {
            post({ Command::Remove, item.gid });
            Command again { Command::Add };
            again.uri = item.uri;
            // (in its folder again, where what it got so far is: aria2 goes on from that; that folder
            // gone, a USB drive out: the one downloads go to now)
            const size_t slash = item.path.find_last_of('/');
            const std::string was = slash != std::string::npos && slash > 0 ? item.path.substr(0, slash) : std::string();
            struct stat st;
            again.dir = !was.empty() && stat(was.c_str(), &st) == 0 && S_ISDIR(st.st_mode) ? was : wisp_download_folder();
            post(again);
        } else if (item.gid) {
            post({ Command::Pause, item.gid });
        }
    } else if (button == 1) {
        if (item.gid) {
            post({ Command::Remove, item.gid });
        } else if (item.external) {
            P.externals.erase(std::remove_if(P.externals.begin(), P.externals.end(), [&](const Item &i) { return i.external == item.external; }), P.externals.end());
        } else {
            P.history.erase(std::remove_if(P.history.begin(), P.history.end(), [&](const Item &i) { return i.path == item.path && i.finished_at == item.finished_at; }), P.history.end());
        }
        P.items.erase(P.items.begin() + row);
    } else if (button == 2) {
        if (H.show_in_files && !item.path.empty())
            H.show_in_files(item.path);
    }
    P.dirty = true;
}

void bar_action(int button)
{
    switch (button) {
    case 0:
        if (H.ask_text)
            H.ask_text("A link to download (http, https, ftp, magnet, .torrent)", "", [](bool ok, const std::string &text) {
                std::string uri = text;
                while (!uri.empty() && isspace(static_cast<unsigned char>(uri.back())))
                    uri.pop_back();
                while (!uri.empty() && isspace(static_cast<unsigned char>(uri.front())))
                    uri.erase(uri.begin());
                if (!ok || uri.empty())
                    return;
                if (uri.find("://") == std::string::npos && uri.rfind("magnet:", 0) != 0)
                    uri = "https://" + uri;
                if (!can_take(uri)) {
                    if (H.toast)
                        H.toast("The download manager takes http, https, ftp and magnet links");
                    return;
                }
                add(uri, {}, "");
                if (H.toast)
                    H.toast("Downloading " + name_from_uri(uri) + wisp_download_fallback_note());
            });
        break;
    case 1: post({ Command::PauseAll }); break;
    case 2: post({ Command::ResumeAll }); break;
    case 3:
        post({ Command::Forget });
        P.externals.erase(std::remove_if(P.externals.begin(), P.externals.end(), [](const Item &i) { return i.state == State::Complete || i.state == State::Error; }), P.externals.end());
        P.history.clear();
        unlink(HISTORY_FILE);
        break;
    case 4:
        if (H.show_in_files)
            H.show_in_files(wisp_download_folder());
        break;
    case 5:
        chooser_open();
        break;
    }
    rebuild();
}

int bar_button_x(Canvas *c, int i, int &width)
{
    int x = 330;
    for (int k = 0; k <= i; k++) {
        const int tw = (c ? c->text_width(BAR_BUTTONS[k], 19, Weight::Bold) : static_cast<int>(strlen(BAR_BUTTONS[k])) * 10) + 40;
        if (k == i) {
            width = tw;
            return x;
        }
        x += tw + 14;
    }
    return x;
}

int bar_widths[BAR_COUNT];
int bar_xs[BAR_COUNT];

void clamp_scroll()
{
    const int content = static_cast<int>(P.items.size()) * (ROW_H + ROW_GAP) + 24;
    const int room = P.h - BAR_H - 12;
    P.scroll = std::clamp(P.scroll, 0.f, static_cast<float>(std::max(0, content - room)));
}

// --- Change the folder (v00.29): where downloads go, chosen with the controller --------------------------
// A panel over the page, as the player's Open file: the places (Downloads, /data, the USB drives), then a
// folder's folders and Up a folder; along its foot Select this folder (the folder shown), New folder and
// Cancel. Cross opens a folder, Circle goes up (closes at the places), Square selects, Triangle makes a
// folder, OPTIONS cancels; a mouse clicks, a USB keyboard has arrows, Enter, Backspace and Escape.

struct ChooserEntry {
    std::string name, path;
    bool up = false;
};

struct Chooser {
    bool open = false;
    std::string folder;                  // "" the places
    std::vector<ChooserEntry> entries;
    int index = 0, scroll = 0, rows = 1;
    int x = 0, y = 0, w = 0, h = 0, list_y = 0;
    int bx[3] = {}, bw[3] = {}, by = 0;  // the foot's buttons as drawn last: Select this folder, New folder, Cancel
    int hover_button = -1;
    std::string message;                 // why the folder shown can't be chosen, what went wrong
} C;

constexpr int CHOOSER_ROW_H = 46, CHOOSER_FOOT_H = 150;
const char *const CHOOSER_BUTTONS[] = { "Select this folder", "New folder", "Cancel" };

bool within(const std::string &path, const std::string &root)
{
    return path == root || path.rfind(root + "/", 0) == 0;
}

bool is_folder(const std::string &path)
{
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

// /data or a USB drive's own folder: Up a folder goes to the places from there
bool is_drive_root(const std::string &path)
{
    return path == "/data" || (path.rfind("/mnt/usb", 0) == 0 && path.find('/', 8) == std::string::npos);
}

// Why downloads can't go to path ("" they can)
std::string folder_problem(const std::string &path)
{
    if (path.empty())
        return "Open a place first, then select a folder in it (or the place itself)";
    if (!within(path, "/data") && path.rfind("/mnt/usb", 0) != 0)
        return "Downloads can go to a folder in /data or on a USB drive";
    if (within(path, WISP_DATA) && !within(path, WISP_DOWNLOADS))
        return "OrbisWeb keeps its own files there: choose another folder";
    if (!is_folder(path))
        return path + " isn't there any more";
    // (the folder's permissions say little here: a file made and deleted tells)
    const std::string probe = path + "/.orbisweb-write-test";
    const int fd = open(probe.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0)
        return "OrbisWeb can't write in " + path + " (" + strerror(errno) + ")";
    close(fd);
    unlink(probe.c_str());
    return {};
}

// The chooser shows folder ("" the places), select's entry chosen
void chooser_list(const std::string &folder, const std::string &select = {})
{
    C.folder = folder;
    C.entries.clear();
    C.index = C.scroll = 0;
    C.message.clear();
    P.dirty = true;
    if (folder.empty()) {
        C.entries.push_back({ "Downloads (" WISP_DOWNLOADS ")", WISP_DOWNLOADS });
        C.entries.push_back({ "Data (/data)", "/data" });
        for (int i = 0; i < 8; i++) {
            const std::string usb = "/mnt/usb" + std::to_string(i);
            if (is_folder(usb))
                C.entries.push_back({ "USB drive " + std::to_string(i + 1) + " (" + usb + ")", usb });
        }
    } else {
        C.entries.push_back({ "Up a folder", "", true });
        std::vector<ChooserEntry> folders;
        if (DIR *dir = opendir(folder.c_str())) {
            while (dirent *d = readdir(dir)) {
                if (d->d_name[0] == '.')
                    continue;   // (hidden ones: a USB drive's Recycle Bin among them)
                const std::string path = folder + "/" + d->d_name;
                if (is_folder(path))
                    folders.push_back({ d->d_name, path });
            }
            closedir(dir);
        }
        std::sort(folders.begin(), folders.end(), [](const ChooserEntry &a, const ChooserEntry &b) {
            return g_ascii_strcasecmp(a.name.c_str(), b.name.c_str()) < 0;
        });
        C.entries.insert(C.entries.end(), folders.begin(), folders.end());
    }
    for (int i = 0; i < static_cast<int>(C.entries.size()); i++)
        if (!select.empty() && C.entries[i].path == select)
            C.index = i;
}

// Opened at the folder downloads go to now
void chooser_open()
{
    C = Chooser{};
    C.open = true;
    chooser_list(wisp_download_folder());
}

void chooser_close()
{
    C.open = false;
    P.dirty = true;
}

void chooser_up()
{
    if (C.folder.empty()) {
        chooser_close();
        return;
    }
    const std::string from = C.folder;
    const size_t slash = from.find_last_of('/');
    chooser_list(is_drive_root(from) || slash == std::string::npos || slash == 0 ? std::string() : from.substr(0, slash), from);
}

void chooser_enter(int i)
{
    if (i < 0 || i >= static_cast<int>(C.entries.size()))
        return;
    if (C.entries[i].up)
        chooser_up();
    else
        chooser_list(C.entries[i].path);
}

void chooser_select()
{
    const std::string path = C.folder;
    const std::string problem = folder_problem(path);
    if (!problem.empty()) {
        C.message = problem;
        P.dirty = true;
        return;
    }
    const std::string before = wisp_download_folder();
    wisp_setting_set("download_folder", path == WISP_DOWNLOADS ? "" : path.c_str());
    chooser_close();
    wisp_log("dm: downloads go to %s now (were going to %s)", path.c_str(), before.c_str());
    if (H.toast)
        H.toast(path == before ? "Downloads go to " + path + ", as before"
            : "Downloads go to " + path + " now" + (active_count() ? " (the ones running finish where they are)" : ""));
}

void chooser_new_folder()
{
    if (C.folder.empty()) {
        C.message = "Open a place first: the new folder goes in it";
        P.dirty = true;
        return;
    }
    if (!H.ask_text)
        return;
    const std::string in = C.folder;
    H.ask_text("A name for the new folder in " + in, "New folder", [in](bool ok, const std::string &typed) {
        std::string name = typed;
        while (!name.empty() && isspace(static_cast<unsigned char>(name.back())))
            name.pop_back();
        while (!name.empty() && isspace(static_cast<unsigned char>(name.front())))
            name.erase(name.begin());
        if (!ok || name.empty() || !C.open || C.folder != in)
            return;   // (cancelled, or the chooser moved on meanwhile)
        if (name == "." || name == ".." || name.find('/') != std::string::npos) {
            C.message = "A folder's name can't have a / in it, or be . or ..";
        } else {
            const std::string path = in + "/" + name;
            if (mkdir(path.c_str(), 0777) != 0 && errno != EEXIST)
                C.message = "The folder could not be made (" + std::string(strerror(errno)) + ")";
            else if (!is_folder(path))
                C.message = "A file named " + name + " is there already";
            else
                chooser_list(path);   // (in it: Select this folder takes it)
        }
        P.dirty = true;
        if (H.redraw)
            H.redraw();
    });
}

void draw_chooser(Canvas &c)
{
    C.w = std::min(1280, P.w - 80);
    C.h = std::min(P.h - 40, 860);
    C.x = (P.w - C.w) / 2;
    C.y = (P.h - C.h) / 2;
    c.fill_rect(0, 0, P.w, P.h, rgb(0x000000, 0x90));
    c.fill_round_rect(C.x, C.y, C.w, C.h, 16, rgb(0x2b2c30));
    c.text(C.x + 32, C.y + 52, "Where downloads go", 25, TEXT, Weight::Bold, C.w - 64);
    c.text(C.x + 32, C.y + 90, C.folder.empty() ? "The places: open one, then choose a folder in it (or the place itself)" : C.folder,
        19, C.folder.empty() ? DIM : ACCENT, Weight::Regular, C.w - 64);
    C.list_y = 112;
    const int foot = C.h - CHOOSER_FOOT_H;
    C.rows = std::max(3, (foot - C.list_y - 8) / CHOOSER_ROW_H);
    const int n = static_cast<int>(C.entries.size());
    C.index = std::clamp(C.index, 0, std::max(0, n - 1));
    if (C.index < C.scroll)
        C.scroll = C.index;
    if (C.index >= C.scroll + C.rows)
        C.scroll = C.index - C.rows + 1;
    C.scroll = std::clamp(C.scroll, 0, std::max(0, n - C.rows));
    for (int r = 0; r < C.rows && C.scroll + r < n; r++) {
        const int i = C.scroll + r;
        const ChooserEntry &e = C.entries[i];
        const int top = C.y + C.list_y + r * CHOOSER_ROW_H;
        if (i == C.index)
            c.fill_round_rect(C.x + 16, top + 2, C.w - 32, CHOOSER_ROW_H - 4, 9, HOVER);
        if (e.up) {   // an arrow up
            c.line(C.x + 40, top + 30, C.x + 47, top + 16, 3, DIM);
            c.line(C.x + 47, top + 16, C.x + 54, top + 30, 3, DIM);
        } else {      // a folder
            const Color amber = rgb(0xf6c344);
            c.fill_round_rect(C.x + 34, top + 14, 12, 6, 2, amber);
            c.fill_round_rect(C.x + 34, top + 17, 26, 17, 3, amber);
        }
        c.text(C.x + 76, top + CHOOSER_ROW_H / 2 + 8, e.name, 21, e.up ? DIM : TEXT, Weight::Regular, C.w - 76 - 48);
    }
    if (!C.folder.empty() && n <= 1)
        c.text(C.x + 76, C.y + C.list_y + CHOOSER_ROW_H + 30, "No folders in here: Select this folder takes it as it is", 19, DIM);
    // the foot: what went wrong (else where downloads go now), the buttons, the controller's keys
    c.fill_rect(C.x + 16, C.y + foot, C.w - 32, 1, rgb(0x3c4043));
    if (!C.message.empty())
        c.text(C.x + 32, C.y + foot + 32, C.message, 18, BAD, Weight::Bold, C.w - 64);
    else
        c.text(C.x + 32, C.y + foot + 32, "Downloads go to " + wisp_download_folder() + " now", 18, DIM, Weight::Regular, C.w - 64);
    C.by = C.y + foot + 50;
    int bx = C.x + 32;
    for (int b = 0; b < 3; b++) {
        const bool enabled = b == 2 || !C.folder.empty();
        C.bw[b] = c.text_width(CHOOSER_BUTTONS[b], 19, Weight::Bold) + 48;
        C.bx[b] = bx;
        const bool hot = C.hover_button == b && enabled;
        c.fill_round_rect(bx, C.by, C.bw[b], 44, 22, b == 0 && enabled ? (hot ? rgb(0xaecbfa) : ACCENT) : (hot ? HOVER : ROW_ON));
        c.text_centered(bx + C.bw[b] / 2, C.by + 29, CHOOSER_BUTTONS[b], 19, !enabled ? rgb(0x5f6368) : b == 0 ? rgb(0x0b1a33) : TEXT, Weight::Bold);
        bx += C.bw[b] + 14;
    }
    c.text(C.x + 32, C.y + C.h - 18, "Cross: open   Circle: up a folder   Square: select this folder   Triangle: new folder   OPTIONS: cancel",
        17, DIM, Weight::Regular, C.w - 64);
}

// The page's input while the chooser is open: all of it is the chooser's
bool chooser_input(const shell::Input &in, const std::vector<shell::KeyEvent> &keys, float px, float py)
{
    const int n = static_cast<int>(C.entries.size());
    int row = -1, button = -1;
    const int list_top = C.y + C.list_y;
    if (px >= C.x + 16 && px < C.x + C.w - 16 && py >= list_top && py < list_top + C.rows * CHOOSER_ROW_H) {
        const int i = C.scroll + static_cast<int>((py - list_top) / CHOOSER_ROW_H);
        if (i < n)
            row = i;
    }
    for (int b = 0; b < 3; b++)
        if (C.bw[b] && px >= C.bx[b] && px < C.bx[b] + C.bw[b] && py >= C.by && py < C.by + 44)
            button = b;
    if (button != C.hover_button) {
        C.hover_button = button;
        P.dirty = true;
    }
    if (in.moved && row >= 0 && row != C.index) {
        C.index = row;
        P.dirty = true;
    }
    if (in.scroll != 0) {
        C.scroll = std::clamp(C.scroll + (in.scroll > 0 ? 3 : -3), 0, std::max(0, n - C.rows));
        C.index = std::clamp(C.index, C.scroll, std::max(C.scroll, std::min(n - 1, C.scroll + C.rows - 1)));
        P.dirty = true;
    }
    if (in.pressed & shell::BTN_CLICK) {
        const bool outside = px < C.x || px >= C.x + C.w || py < C.y || py >= C.y + C.h;
        if (button == 0)
            chooser_select();
        else if (button == 1)
            chooser_new_folder();
        else if (button == 2)
            chooser_close();
        else if (row >= 0)
            chooser_enter(row);
        else if (outside && in.mouse)
            chooser_close();   // (a click away from it, as the player's Open file)
        else if (outside || !in.mouse)
            chooser_enter(C.index);   // (Cross with the pointer elsewhere: the folder chosen in the list)
        P.dirty = true;
        return true;
    }
    if (in.pressed & shell::BTN_BACK)
        chooser_up();
    if ((in.pressed & shell::BTN_MENU) && !(in.pressed & shell::BTN_MOUSE_RIGHT))   // Square
        chooser_select();
    if (in.pressed & shell::BTN_SWITCH)   // Triangle
        chooser_new_folder();
    if (in.pressed & shell::BTN_START)
        chooser_close();
    if (!C.open)
        return true;
    if (in.repeat & shell::BTN_UP)
        C.index = std::max(0, C.index - 1);
    if (in.repeat & shell::BTN_DOWN)
        C.index = std::min(n - 1, C.index + 1);
    if (in.repeat & shell::BTN_LEFT)
        C.index = std::max(0, C.index - C.rows);
    if (in.repeat & shell::BTN_RIGHT)
        C.index = std::min(n - 1, C.index + C.rows);
    for (const shell::KeyEvent &key : keys) {
        if (!key.down)
            continue;
        switch (key.vkey) {
        case 0x26: C.index = std::max(0, C.index - 1); break;          // Up
        case 0x28: C.index = std::min(n - 1, C.index + 1); break;      // Down
        case 0x21: C.index = std::max(0, C.index - C.rows); break;     // Page Up
        case 0x22: C.index = std::min(n - 1, C.index + C.rows); break; // Page Down
        case 0x24: C.index = 0; break;                                 // Home
        case 0x23: C.index = n - 1; break;                             // End
        case 0x0D: chooser_enter(C.index); break;                      // Enter
        case 0x08: chooser_up(); break;                                // Backspace
        case 0x1B: chooser_close(); break;                             // Escape
        }
        if (!C.open)
            break;
    }
    P.dirty = true;
    return true;
}

void keep_selected_in_view()
{
    const int top = BAR_H + 12 + P.selected * (ROW_H + ROW_GAP);
    if (top - P.scroll < BAR_H + 12)
        P.scroll = static_cast<float>(top - BAR_H - 12);
    if (top + ROW_H - P.scroll > P.h - 12)
        P.scroll = static_cast<float>(top + ROW_H - P.h + 12);
    clamp_scroll();
}

} // namespace

// --- Public -------------------------------------------------------------------------------------------

void start(const Host &host)
{
    H = host;
    load_history();
    if (!wisp_setting_int("download_manager", 1)) {
        wisp_log("dm: off (download_manager=0): the browser's own downloader takes every download");
        engine_failed = true;
        return;
    }
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, static_cast<size_t>(1) << 20);
    if (pthread_create(&engine_thread, &attr, engine_main, nullptr) != 0) {
        wisp_log("dm: the engine's thread did not start");
        engine_failed = true;
    }
    pthread_attr_destroy(&attr);
}

void stop()
{
    if (!engine_up)
        return;
    stopping = true;
    pthread_join(engine_thread, nullptr);
}

bool running() { return engine_up && !engine_failed; }

bool can_take(const std::string &uri)
{
    if (engine_failed)
        return false;
    for (const char *scheme : { "http://", "https://", "ftp://", "sftp://", "magnet:" })
        if (g_ascii_strncasecmp(uri.c_str(), scheme, strlen(scheme)) == 0)
            return strcmp(scheme, "sftp://") != 0;   // (no libssh2 in this aria2)
    return false;
}

void add(const std::string &uri, const std::vector<std::string> &headers, const std::string &file_name)
{
    Command command { Command::Add };
    command.uri = uri;
    command.headers = headers;
    command.name = file_name;
    command.dir = wisp_download_folder();   // (the one chosen now; it stays the download's)
    post(command);
    P.dirty = true;
}

int external_start(const std::string &name, const std::string &path)
{
    Item item;
    item.external = P.next_external++;
    item.name = name;
    item.path = path;
    item.state = State::Active;
    item.added = wisp_seconds();
    P.externals.push_back(item);
    rebuild();
    return item.external;
}

void external_progress(int id, double fraction, int64_t bytes)
{
    for (Item &item : P.externals) {
        if (item.external != id)
            continue;
        item.done = bytes;
        if (fraction > 0.001 && bytes > 0)
            item.total = static_cast<int64_t>(bytes / fraction);
        P.dirty = true;
    }
}

void external_done(int id, bool ok, const std::string &)
{
    for (Item &item : P.externals) {
        if (item.external != id)
            continue;
        item.state = ok ? State::Complete : State::Error;
        item.finished_at = time(nullptr);
        if (ok) {
            item.total = std::max(item.total, item.done);
            history_append(item);
        }
    }
    rebuild();
}

void show()
{
    P.visible = true;
    rebuild();
}

void hide()
{
    P.visible = false;
    C.open = false;   // (a folder not chosen: as Cancel)
}
bool visible() { return P.visible; }

int active_count()
{
    std::lock_guard<std::mutex> guard(lock);
    int n = 0;
    for (const Item &item : snapshot)
        n += !item.hidden && (item.state == State::Active || item.state == State::Waiting);
    for (const Item &item : P.externals)
        n += item.state == State::Active;
    return n;
}

bool dirty()
{
    // (called every frame, visible or not: the engine's news goes out from here, on the UI thread)
    std::vector<std::string> said, links;
    bool fresh = false;
    {
        std::lock_guard<std::mutex> guard(lock);
        said.swap(notices);
        links.swap(fallbacks);
        fresh = snapshot_new;
        snapshot_new = false;
    }
    for (const std::string &text : said)
        if (H.toast)
            H.toast(text);
    for (const std::string &uri : links)
        if (H.browser_download)
            H.browser_download(uri);
    if (fresh) {
        rebuild();
        if (H.redraw)
            H.redraw();
    }
    return P.visible && P.dirty;
}

bool input(const shell::Input &in, const std::vector<shell::KeyEvent> &keys, int x, int y, int w, int h, float)
{
    if (!P.visible)
        return false;
    if (P.x != x || P.y != y || P.w != w || P.h != h) {
        P.x = x, P.y = y, P.w = w, P.h = h;
        P.dirty = true;
    }
    const float px = in.x - x, py = in.y - y;
    const bool inside = px >= 0 && py >= 0 && px < w && py < h;
    // what the pointer is over
    int row = -1, button = -1, bar = -1;
    if (C.open)
        return chooser_input(in, keys, px, py);
    if (inside && py < BAR_H) {
        for (int i = 0; i < BAR_COUNT; i++) {
            const int top = i < BAR_LINE1 ? 28 : BAR2_Y, bottom = i < BAR_LINE1 ? 72 : BAR2_Y + BAR2_H;
            if (px >= bar_xs[i] && px < bar_xs[i] + bar_widths[i] && py >= top && py < bottom)
                bar = i;
        }
    } else if (inside) {
        for (int i = 0; i < static_cast<int>(P.items.size()); i++) {
            const int top = row_top(i);
            if (py >= top && py < top + ROW_H) {
                row = i;
                for (int b = 0; b < 3; b++)
                    if (*button_label(P.items[i], b) && px >= button_x(b) && px < button_x(b) + 116 && py >= top + 24 && py < top + 68)
                        button = b;
            }
        }
    }
    if (row != P.hover_row || button != P.hover_button || bar != P.hover_bar) {
        P.hover_row = row, P.hover_button = button, P.hover_bar = bar;
        P.dirty = true;
    }
    bool used = inside && (in.moved || in.pressed || in.scroll != 0);
    if (in.pressed & shell::BTN_CLICK) {
        if (bar >= 0) {
            bar_action(bar);
        } else if (row >= 0) {
            P.selected = row;
            if (button >= 0) {
                act(row, button);
            } else {
                // twice on a finished one opens it
                const double now = wisp_seconds();
                if (row == P.last_click_row && now - P.last_click < 0.45 && P.items[row].state == State::Complete)
                    act(row, 0);
                P.last_click = now;
                P.last_click_row = row;
            }
            P.dirty = true;
        } else if (!inside) {
            used = false;
        } else {
            // (Cross with no pointer on a row: the selected one's first button)
            act(P.selected, 0);
        }
    }
    const int n = static_cast<int>(P.items.size());
    if ((in.repeat & shell::BTN_DOWN) && n) {
        P.selected = std::min(n - 1, P.selected + 1);
        keep_selected_in_view();
        P.dirty = used = true;
    }
    if ((in.repeat & shell::BTN_UP) && n) {
        P.selected = std::max(0, P.selected - 1);
        keep_selected_in_view();
        P.dirty = used = true;
    }
    if ((in.pressed & shell::BTN_MENU) && !(in.pressed & shell::BTN_MOUSE_RIGHT) && n) {   // Square
        act(P.selected, 1);
        used = true;
    }
    if (in.pressed & shell::BTN_SWITCH) {   // Triangle
        bar_action(0);
        used = true;
    }
    if (in.scroll != 0 && inside) {
        P.scroll += in.scroll * 48.f;
        clamp_scroll();
        P.dirty = true;
    }
    for (const shell::KeyEvent &key : keys) {
        if (!key.down)
            continue;
        used = true;
        switch (key.vkey) {
        case 0x28: if (n) P.selected = std::min(n - 1, P.selected + 1), keep_selected_in_view(); break;   // Down
        case 0x26: if (n) P.selected = std::max(0, P.selected - 1), keep_selected_in_view(); break;       // Up
        case 0x20: act(P.selected, 0); break;    // Space
        case 0x0D: act(P.selected, 0); break;    // Enter
        case 0x2E: act(P.selected, 1); break;    // Delete
        case 'L': bar_action(0); break;          // (Ctrl+L comes as L with Ctrl: either adds a link)
        default: used = false;
        }
        P.dirty = true;
    }
    return used;
}

void draw(Canvas &c)
{
    P.dirty = false;
    c.fill_rect(0, 0, P.w, P.h, BG);
    // the top bar: the title, the totals, the buttons
    c.text(LIST_X, 62, "Downloads", 30, TEXT, Weight::Bold);
    for (int i = 0; i < BAR_LINE1; i++) {
        bar_xs[i] = bar_button_x(&c, i, bar_widths[i]);
        c.fill_round_rect(bar_xs[i], 28, bar_widths[i], 44, 22, P.hover_bar == i ? HOVER : ROW_ON);
        c.text_centered(bar_xs[i] + bar_widths[i] / 2, 57, BAR_BUTTONS[i], 19, i == 0 ? ACCENT : TEXT, Weight::Bold);
    }
    // the second line: where downloads go (the folder chosen; Downloads while that one can't be had),
    // and Change the folder
    {
        bool missing = false;
        const std::string folder = wisp_download_folder(&missing);
        const std::string where = missing ? wisp_download_folder_chosen() + " isn't there (a USB drive out?): they go to " + folder : folder;
        const int label_w = c.text(LIST_X, BAR2_Y + 26, "Downloads go to", 18, DIM);
        const int where_x = LIST_X + label_w + 10;
        bar_widths[BAR_LINE1] = c.text_width(BAR_BUTTONS[BAR_LINE1], 17, Weight::Bold) + 36;
        const int room = std::max(100, P.w - where_x - bar_widths[BAR_LINE1] - 64);
        const int where_w = c.text(where_x, BAR2_Y + 26, where, 18, missing ? WARN : TEXT, Weight::Bold, room);
        bar_xs[BAR_LINE1] = where_x + std::min(where_w, room) + 20;
        c.fill_round_rect(bar_xs[BAR_LINE1], BAR2_Y, bar_widths[BAR_LINE1], BAR2_H, BAR2_H / 2, P.hover_bar == BAR_LINE1 ? HOVER : ROW_ON);
        c.text_centered(bar_xs[BAR_LINE1] + bar_widths[BAR_LINE1] / 2, BAR2_Y + 25, BAR_BUTTONS[BAR_LINE1], 17, ACCENT, Weight::Bold);
    }
    std::string totals = running() ? "down " + speed_text(P.down) + "  ·  up " + speed_text(P.up)
        : engine_failed ? "aria2 is not running: the browser downloads" : "starting aria2...";
    const int tw = c.text_width(totals, 18);
    c.text(P.w - 40 - tw, 57, totals, 18, DIM);
    c.fill_rect(0, BAR_H - 2, P.w, 2, ROW);
    if (P.items.empty()) {
        c.text_centered(P.w / 2, P.h / 2 - 10, "No downloads yet", 26, DIM, Weight::Bold);
        c.text_centered(P.w / 2, P.h / 2 + 30, "What a page downloads comes here; Triangle (or Add a link) takes a link or a magnet link.", 18, DIM);
        if (C.open)
            draw_chooser(c);
        return;
    }
    for (int i = 0; i < static_cast<int>(P.items.size()); i++) {
        const int top = row_top(i);
        if (top + ROW_H < BAR_H || top > P.h)
            continue;
        const Item &item = P.items[i];
        const bool on = i == P.selected;
        c.fill_round_rect(LIST_X - 16, top, P.w - 2 * LIST_X + 32, ROW_H, 14, on ? ROW_ON : (i == P.hover_row ? HOVER : ROW));
        // the kind's badge: the file's extension
        std::string ext = item.torrent ? "BT" : "";
        if (ext.empty()) {
            const size_t dot = item.name.find_last_of('.');
            ext = dot != std::string::npos && item.name.size() - dot <= 5 ? item.name.substr(dot + 1) : "file";
            for (char &ch : ext)
                ch = static_cast<char>(toupper(static_cast<unsigned char>(ch)));
        }
        const Color badge = item.state == State::Complete ? GOOD : item.state == State::Error ? BAD : item.state == State::Paused ? WARN : ACCENT;
        c.fill_round_rect(LIST_X, top + 20, 56, 52, 10, rgb(0x121316));
        c.text_centered(LIST_X + 28, top + 52, ext, 15, badge, Weight::Bold);
        const int text_x = LIST_X + 80, text_w = button_x(2) - text_x - 20;
        c.text(text_x, top + 36, item.name, 21, TEXT, Weight::Bold, text_w);
        c.text(text_x, top + 64, details(item), 16, item.state == State::Error ? BAD : DIM, Weight::Regular, text_w);
        if (item.state != State::Complete && item.state != State::Error) {
            c.fill_round_rect(text_x, top + 75, text_w, 6, 3, rgb(0x121316));
            if (item.total > 0)
                c.fill_round_rect(text_x, top + 75, std::max(6, static_cast<int>(text_w * (static_cast<double>(item.done) / item.total))), 6, 3, badge);
        }
        for (int b = 0; b < 3; b++) {
            const char *label = button_label(item, b);
            if (!*label)
                continue;
            const int bx = button_x(b);
            const bool hot = i == P.hover_row && b == P.hover_button;
            c.fill_round_rect(bx, top + 24, 116, 44, 22, hot ? HOVER : rgb(0x202124));
            c.text_centered(bx + 58, top + 53, label, 18, b == 0 ? ACCENT : TEXT, Weight::Bold);
        }
    }
    if (C.open)
        draw_chooser(c);
}

} // namespace wisp::dm
