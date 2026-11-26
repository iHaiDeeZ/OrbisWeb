// Wisp: browser extensions (ui/ext/extensions.h).
//
// Each installed extension is an unpacked folder /data/Wisp/extensions/<id>/ with its manifest.json,
// plus Wisp's own files there: .wisp-state (enabled), .wisp-storage-<area>.json (chrome.storage),
// .wisp-dnr.json (dynamic rules), .wisp-scripts.json (scripting.registerContentScripts).
//
// Who talks to whom: content scripts run in the world "wisp-ext-<id>" of every tab whose address
// their "matches" allow; extension pages (the background page, popups, options) are web views of their
// own showing chrome-extension://<id>/... All of them get data/ext/api.js first; its calls arrive in
// on_message() with the sender known from the content manager they came through (a tab's, or the
// extension view's), never from the message itself.
#include "extensions.h"

#include "../../common/wisp.h"
#include "package.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <map>
#include <memory>
#include <set>
#include <sys/stat.h>
#include <unistd.h>
#include <wpe/webkit.h>

#define RAPIDJSON_HAS_STDSTRING 1
#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

namespace wisp::ext {
namespace {

using rapidjson::Document;
using rapidjson::Value;

constexpr const char *ROOT = WISP_EXTENSIONS;
constexpr const char *HANDLER = "wispExt";
constexpr int CONTEXT_BACKGROUND = -2;
constexpr int CONTEXT_POPUP = -3;
constexpr unsigned PARSE = rapidjson::kParseCommentsFlag | rapidjson::kParseTrailingCommasFlag;

// --- small helpers -----------------------------------------------------------------------------------

std::string read_text(const std::string &path)
{
    FILE *f = fopen(path.c_str(), "rb");
    if (!f)
        return {};
    std::string text;
    char buffer[65536];
    size_t n;
    while ((n = fread(buffer, 1, sizeof buffer, f)) > 0)
        text.append(buffer, n);
    fclose(f);
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xef && static_cast<unsigned char>(text[1]) == 0xbb)
        text.erase(0, 3);   // a UTF-8 byte order mark
    return text;
}

bool write_text(const std::string &path, const std::string &text)
{
    const std::string temp = path + ".new";
    FILE *f = fopen(temp.c_str(), "wb");
    if (!f)
        return false;
    const bool ok = fwrite(text.data(), 1, text.size(), f) == text.size();
    fclose(f);
    return ok && rename(temp.c_str(), path.c_str()) == 0;
}

std::string to_json(const Value &value)
{
    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    value.Accept(writer);
    return buffer.GetString();
}

std::string json_string(const std::string &text)
{
    Value v(text.c_str(), static_cast<rapidjson::SizeType>(text.size()));
    return to_json(v);
}

std::string str(const Value &v, const char *key, const std::string &fallback = {})
{
    if (v.IsObject() && v.HasMember(key) && v[key].IsString())
        return v[key].GetString();
    return fallback;
}

void remove_tree(const std::string &path)
{
    DIR *dir = opendir(path.c_str());
    if (dir) {
        while (dirent *e = readdir(dir)) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
                continue;
            const std::string child = path + "/" + e->d_name;
            struct stat st;
            if (!lstat(child.c_str(), &st) && S_ISDIR(st.st_mode))
                remove_tree(child);
            else
                unlink(child.c_str());
        }
        closedir(dir);
    }
    rmdir(path.c_str());
}

// Chrome's match patterns as WebKit's allow lists already are (<all_urls> -> *://*/*)
std::vector<std::string> match_list(const Value &v, const char *key)
{
    std::vector<std::string> out;
    if (!v.IsObject() || !v.HasMember(key) || !v[key].IsArray())
        return out;
    for (auto &item : v[key].GetArray()) {
        if (!item.IsString())
            continue;
        std::string pattern = item.GetString();
        if (pattern == "<all_urls>") {
            out.push_back("http://*/*");
            out.push_back("https://*/*");
            out.push_back("file:///*");
        } else if (pattern.rfind("*://", 0) == 0) {
            out.push_back("http" + pattern.substr(1));
            out.push_back("https" + pattern.substr(1));
        } else {
            out.push_back(pattern);
        }
    }
    return out;
}

struct CStrings {
    std::vector<std::string> owned;
    std::vector<const char *> pointers;
    explicit CStrings(const std::vector<std::string> &list) : owned(list)
    {
        for (auto &s : owned)
            pointers.push_back(s.c_str());
        pointers.push_back(nullptr);
    }
    const char *const *get() const { return owned.empty() ? nullptr : pointers.data(); }
};

// --- the extensions ----------------------------------------------------------------------------------

struct Extension {
    std::string id, dir;
    Document manifest;
    std::string manifest_json;    // as api.js gets it
    std::string messages_json;    // _locales, merged (default locale over English)
    bool enabled = true;
    std::string error;
    WebKitWebView *background = nullptr;
    WebKitUserContentManager *background_ucm = nullptr;
    std::string badge_text;
    uint32_t badge_color = 0xff2530d9;
    std::string popup;            // the action's popup page (path)
    std::map<std::string, Document> storage;   // area -> object
    Document dynamic_rules{rapidjson::kArrayType};
    Document registered_scripts{rapidjson::kArrayType};
    std::set<std::string> disabled_rulesets, enabled_rulesets_extra;
};

Browser B;
std::vector<std::unique_ptr<Extension>> E;
std::map<int, WebKitUserContentManager *> tab_ucms;
WebKitUserContentFilterStore *filter_store = nullptr;
std::map<std::string, WebKitUserContentFilter *> filters;   // compiled content rule lists by id
bool adblock_on = true;
std::string api_template;

// replies awaiting a runtime.onMessage answer, and open ports
struct Pending {
    WebKitScriptMessageReply *reply = nullptr;
};
std::map<std::string, Pending> pending;
uint64_t next_reply = 1;
struct Port {
    std::string ext;
    int a_tab, b_tab;             // the two ends' contexts (tab id, or CONTEXT_*)
};
std::map<std::string, Port> port_table;
std::map<std::string, guint> alarms;   // ext + "\n" + name -> source id

Extension *find(const std::string &id)
{
    for (auto &e : E)
        if (e->id == id)
            return e.get();
    return nullptr;
}

std::string world_of(const Extension &e) { return "wisp-ext-" + e.id; }

std::string api_source(const Extension &e, const char *context, int tab)
{
    if (api_template.empty())
        api_template = read_text("/app0/wisp/ext/api.js");
    std::string s = api_template;
    auto replace = [&](const std::string &key, const std::string &value) {
        for (size_t at = s.find(key); at != std::string::npos; at = s.find(key, at + value.size()))
            s.replace(at, key.size(), value);
    };
    replace("__WISP_EXT_ID__", e.id);
    replace("__WISP_CONTEXT__", context);
    replace("__WISP_TAB_ID__", std::to_string(tab));
    replace("__WISP_MANIFEST__", e.manifest_json);
    replace("__WISP_MESSAGES__", e.messages_json);
    return s;
}

void load_messages(Extension &e)
{
    Document merged(rapidjson::kObjectType);
    auto &a = merged.GetAllocator();
    const std::string fallback = str(e.manifest, "default_locale", "en");
    for (const std::string &locale : { std::string("en"), fallback }) {
        Document d;
        const std::string text = read_text(e.dir + "/_locales/" + locale + "/messages.json");
        if (text.empty() || d.Parse<PARSE>(text.c_str()).HasParseError() || !d.IsObject())
            continue;
        for (auto &m : d.GetObject()) {
            std::string key = m.name.GetString();
            std::transform(key.begin(), key.end(), key.begin(), ::tolower);
            Value k(key.c_str(), a);
            if (merged.HasMember(k))
                merged.EraseMember(k);
            merged.AddMember(Value(key.c_str(), a), Value(m.value, a), a);
        }
    }
    e.messages_json = to_json(merged);
}

std::string localized(const Extension &e, const std::string &text)
{
    if (text.rfind("__MSG_", 0) != 0 || text.size() < 9)
        return text;
    std::string key = text.substr(6, text.size() - 8);
    std::transform(key.begin(), key.end(), key.begin(), ::tolower);
    Document d;
    if (d.Parse(e.messages_json.c_str()).HasParseError() || !d.HasMember(key.c_str()))
        return text;
    return str(d[key.c_str()], "message", text);
}

void load_state(Extension &e)
{
    const std::string state = read_text(e.dir + "/.wisp-state");
    e.enabled = state.find("enabled=0") == std::string::npos;
    for (const char *area : { "local", "sync", "managed" }) {
        Document d;
        const std::string text = read_text(e.dir + "/.wisp-storage-" + area + ".json");
        if (text.empty() || d.Parse(text.c_str()).HasParseError() || !d.IsObject())
            d.SetObject();
        e.storage[area] = std::move(d);
    }
    e.storage["session"].SetObject();
    const std::string dnr = read_text(e.dir + "/.wisp-dnr.json");
    if (dnr.empty() || e.dynamic_rules.Parse(dnr.c_str()).HasParseError() || !e.dynamic_rules.IsArray())
        e.dynamic_rules.SetArray();
    const std::string scripts = read_text(e.dir + "/.wisp-scripts.json");
    if (scripts.empty() || e.registered_scripts.Parse(scripts.c_str()).HasParseError() || !e.registered_scripts.IsArray())
        e.registered_scripts.SetArray();
}

void save_state(const Extension &e)
{
    write_text(e.dir + "/.wisp-state", std::string("enabled=") + (e.enabled ? "1" : "0") + "\n");
}

std::unique_ptr<Extension> load_extension(const std::string &dir, const std::string &id)
{
    auto e = std::make_unique<Extension>();
    e->id = id;
    e->dir = dir;
    const std::string text = read_text(dir + "/manifest.json");
    if (text.empty() || e->manifest.Parse<PARSE>(text.c_str()).HasParseError() || !e->manifest.IsObject())
        return nullptr;
    e->manifest_json = to_json(e->manifest);
    load_messages(*e);
    load_state(*e);
    const Value &m = e->manifest;
    const char *action_key = m.HasMember("action") ? "action" : m.HasMember("browser_action") ? "browser_action" : "page_action";
    if (m.HasMember(action_key) && m[action_key].IsObject())
        e->popup = str(m[action_key], "default_popup");
    return e;
}

// --- content rule lists (ad blocking, declarativeNetRequest) -------------------------------------------

void add_filter_everywhere(WebKitUserContentFilter *filter)
{
    for (auto &[tab, ucm] : tab_ucms)
        webkit_user_content_manager_add_filter(ucm, filter);
}

void remove_filter_everywhere(const std::string &id)
{
    for (auto &[tab, ucm] : tab_ucms)
        webkit_user_content_manager_remove_filter_by_id(ucm, id.c_str());
}

struct SaveRequest {
    std::string id;
    double started;
};

void filter_saved(GObject *store, GAsyncResult *result, gpointer data)
{
    std::unique_ptr<SaveRequest> request(static_cast<SaveRequest *>(data));
    GError *error = nullptr;
    WebKitUserContentFilter *filter = webkit_user_content_filter_store_save_finish(WEBKIT_USER_CONTENT_FILTER_STORE(store), result, &error);
    if (!filter) {
        wisp_log("ext: content rules %s did not compile: %s", request->id.c_str(), error ? error->message : "?");
        g_clear_error(&error);
        return;
    }
    wisp_log("ext: content rules %s ready (%.1f s)", request->id.c_str(), wisp_seconds() - request->started);
    if (filters.count(request->id)) {
        remove_filter_everywhere(request->id);
        webkit_user_content_filter_unref(filters[request->id]);
    }
    filters[request->id] = filter;
    const bool is_adblock = request->id.rfind("adblock-", 0) == 0;
    if (!is_adblock || adblock_on)
        add_filter_everywhere(filter);
}

// compiles the rules (WebKit's content blocker JSON) as id, unless the store already has them for
// this exact source (the store's copy is kept with a hash of what it was made from)
void compile_rules(const std::string &id, const std::string &json)
{
    const std::string stamp_path = std::string(WISP_PROFILE) + "/filters/" + id + ".stamp";
    const std::string stamp = std::to_string(std::hash<std::string>{}(json)) + ":" + std::to_string(json.size());
    if (read_text(stamp_path) == stamp) {
        // already compiled from this source: load it
        auto *request = new SaveRequest { id, wisp_seconds() };
        webkit_user_content_filter_store_load(filter_store, id.c_str(), nullptr, +[](GObject *store, GAsyncResult *result, gpointer data) {
            std::unique_ptr<SaveRequest> request(static_cast<SaveRequest *>(data));
            GError *error = nullptr;
            WebKitUserContentFilter *filter = webkit_user_content_filter_store_load_finish(WEBKIT_USER_CONTENT_FILTER_STORE(store), result, &error);
            if (!filter) {
                g_clear_error(&error);
                return;
            }
            filters[request->id] = filter;
            if (request->id.rfind("adblock-", 0) != 0 || adblock_on)
                add_filter_everywhere(filter);
            wisp_log("ext: content rules %s loaded", request->id.c_str());
        }, request);
        return;
    }
    GBytes *bytes = g_bytes_new(json.data(), json.size());
    webkit_user_content_filter_store_save(filter_store, id.c_str(), bytes, nullptr, filter_saved, new SaveRequest { id, wisp_seconds() });
    g_bytes_unref(bytes);
    write_text(stamp_path, stamp);
    wisp_log("ext: compiling content rules %s (%zu KiB)", id.c_str(), json.size() >> 10);
}

// declarativeNetRequest's urlFilter to WebKit's url-filter (the same language as data/adblock)
std::string url_filter_regex(std::string pattern)
{
    std::string regex;
    bool host = false, start = false, end = false;
    if (pattern.rfind("||", 0) == 0) {
        host = true;
        pattern.erase(0, 2);
    } else if (pattern.rfind("|", 0) == 0) {
        start = true;
        pattern.erase(0, 1);
    }
    if (!pattern.empty() && pattern.back() == '|') {
        end = true;
        pattern.pop_back();
    }
    for (size_t i = 0; i < pattern.size(); i++) {
        const char c = pattern[i];
        if (c == '*')
            regex += ".*";
        else if (c == '^')
            regex += i + 1 < pattern.size() ? "[^a-zA-Z0-9_.%-]" : "([^a-zA-Z0-9_.%-].*)?$";
        else if (strchr(".+?$(){}[]\\|", c))
            regex += std::string("\\") + c;
        else
            regex += c;
    }
    if (host)
        regex = "^[a-z][a-z0-9.+-]*://([^/:]*\\.)?" + regex;
    else if (start)
        regex = "^" + regex;
    if (end && (regex.empty() || regex.back() != '$'))
        regex += "$";
    return regex.empty() ? ".*" : regex;
}

// a DNR regexFilter WebKit can take, or "" (alternation and class escapes it lacks)
std::string regex_filter(const std::string &re)
{
    if (re.find('|') != std::string::npos || re.find("(?") != std::string::npos)
        return {};
    std::string out;
    for (size_t i = 0; i < re.size(); i++) {
        if (re[i] == '\\' && i + 1 < re.size()) {
            const char n = re[++i];
            if (n == 'd') out += "[0-9]";
            else if (n == 'w') out += "[A-Za-z0-9_]";
            else if (strchr("sSDWbB", n)) return {};
            else { out += '\\'; out += n; }
        } else {
            out += re[i];
        }
    }
    return out;
}

const char *webkit_type(const std::string &dnr)
{
    static const std::map<std::string, const char *> types = { { "main_frame", "document" }, { "sub_frame", "document" },
        { "stylesheet", "style-sheet" }, { "script", "script" }, { "image", "image" }, { "font", "font" },
        { "object", "other" }, { "xmlhttprequest", "fetch" }, { "ping", "ping" }, { "csp_report", "other" },
        { "media", "media" }, { "websocket", "websocket" }, { "webtransport", "other" }, { "webbundle", "other" },
        { "other", "other" } };
    auto it = types.find(dnr);
    return it == types.end() ? nullptr : it->second;
}

// appends the WebKit rules for DNR rules (blocks first, allows after: ignore-previous-rules) to out
void convert_dnr(const Value &rules, Value &blocks, Value &allows, Document::AllocatorType &a, int &skipped)
{
    if (!rules.IsArray())
        return;
    for (auto &rule : rules.GetArray()) {
        if (!rule.IsObject() || !rule.HasMember("action") || !rule.HasMember("condition"))
            continue;
        const std::string type = str(rule["action"], "type");
        const Value &c = rule["condition"];
        const char *action = type == "block" ? "block" : (type == "allow" || type == "allowAllRequests") ? "ignore-previous-rules"
            : type == "upgradeScheme" ? "make-https" : nullptr;
        if (!action) {
            skipped++;
            continue;
        }
        std::string regex;
        if (c.HasMember("regexFilter") && c["regexFilter"].IsString()) {
            regex = regex_filter(c["regexFilter"].GetString());
            if (regex.empty()) {
                skipped++;
                continue;
            }
        } else {
            regex = url_filter_regex(str(c, "urlFilter", "*"));
        }
        // requestDomains: one rule per domain (no alternation in WebKit's filters)
        std::vector<std::string> request_domains;
        for (const char *key : { "requestDomains", "domains" }) {
            if (!strcmp(key, "domains"))
                break;
            if (c.HasMember(key) && c[key].IsArray())
                for (auto &d : c[key].GetArray())
                    if (d.IsString())
                        request_domains.push_back(d.GetString());
        }
        if (request_domains.empty())
            request_domains.push_back({});
        for (const std::string &domain : request_domains) {
            Value trigger(rapidjson::kObjectType);
            std::string filter = regex;
            if (!domain.empty()) {
                std::string escaped;
                for (char ch : domain)
                    escaped += ch == '.' ? std::string("\\.") : std::string(1, ch);
                filter = "^[a-z][a-z0-9.+-]*://([^/:]*\\.)?" + escaped + "[:/]" + (regex == ".*" ? std::string() : ".*" + regex);
                if (regex == ".*")
                    filter += ".*";
            }
            trigger.AddMember("url-filter", Value(filter.c_str(), a), a);
            if (c.HasMember("isUrlFilterCaseSensitive") && c["isUrlFilterCaseSensitive"].IsBool())
                trigger.AddMember("url-filter-is-case-sensitive", c["isUrlFilterCaseSensitive"].GetBool(), a);
            Value types(rapidjson::kArrayType);
            if (c.HasMember("resourceTypes") && c["resourceTypes"].IsArray()) {
                std::set<std::string> seen;
                for (auto &t : c["resourceTypes"].GetArray())
                    if (t.IsString())
                        if (const char *w = webkit_type(t.GetString()); w && seen.insert(w).second)
                            types.PushBack(Value(w, a), a);
            } else if (type == "allowAllRequests") {
                types.PushBack(Value("document", a), a);
            }
            if (!types.Empty())
                trigger.AddMember("resource-type", types, a);
            const std::string domain_type = str(c, "domainType");
            if (!domain_type.empty()) {
                Value load(rapidjson::kArrayType);
                load.PushBack(Value(domain_type == "thirdParty" ? "third-party" : "first-party", a), a);
                trigger.AddMember("load-type", load, a);
            }
            for (const char *key : { "initiatorDomains", "excludedInitiatorDomains" }) {
                if (!c.HasMember(key) || !c[key].IsArray() || c[key].Empty())
                    continue;
                Value list(rapidjson::kArrayType);
                for (auto &d : c[key].GetArray())
                    if (d.IsString())
                        list.PushBack(Value(("*" + std::string(d.GetString())).c_str(), a), a);
                trigger.AddMember(Value(strcmp(key, "initiatorDomains") ? "unless-domain" : "if-domain", a), list, a);
                break;   // WebKit takes one of the two
            }
            Value act(rapidjson::kObjectType);
            act.AddMember("type", Value(action, a), a);
            Value r(rapidjson::kObjectType);
            r.AddMember("trigger", trigger, a);
            r.AddMember("action", act, a);
            (strcmp(action, "ignore-previous-rules") ? blocks : allows).PushBack(r, a);
        }
    }
}

// compiles an extension's enabled static rulesets and its dynamic rules into one content rule list
void compile_dnr(Extension &e)
{
    const Value &m = e.manifest;
    if (!m.HasMember("declarative_net_request") && e.dynamic_rules.Empty())
        return;
    Document out(rapidjson::kArrayType);
    auto &a = out.GetAllocator();
    Value blocks(rapidjson::kArrayType), allows(rapidjson::kArrayType);
    int skipped = 0;
    if (m.HasMember("declarative_net_request") && m["declarative_net_request"].HasMember("rule_resources")
        && m["declarative_net_request"]["rule_resources"].IsArray()) {
        for (auto &res : m["declarative_net_request"]["rule_resources"].GetArray()) {
            const std::string id = str(res, "id");
            bool on = res.HasMember("enabled") && res["enabled"].IsBool() && res["enabled"].GetBool();
            if (e.enabled_rulesets_extra.count(id))
                on = true;
            if (e.disabled_rulesets.count(id))
                on = false;
            if (!on)
                continue;
            Document rules;
            const std::string text = read_text(e.dir + "/" + str(res, "path"));
            if (text.empty() || rules.Parse<PARSE>(text.c_str()).HasParseError())
                continue;
            convert_dnr(rules, blocks, allows, a, skipped);
        }
    }
    convert_dnr(e.dynamic_rules, blocks, allows, a, skipped);
    for (auto &r : blocks.GetArray())
        out.PushBack(Value(r, a), a);
    for (auto &r : allows.GetArray())
        out.PushBack(Value(r, a), a);
    if (out.Empty()) {
        out.PushBack(Value(rapidjson::kObjectType).AddMember("trigger", Value(rapidjson::kObjectType).AddMember("url-filter", "^wisp-never:", a), a)
            .AddMember("action", Value(rapidjson::kObjectType).AddMember("type", "block", a), a), a);
    }
    wisp_log("ext %s: %u content rules from declarativeNetRequest (%d it cannot express)", e.id.c_str(), out.Size(), skipped);
    compile_rules("ext-" + e.id, to_json(out));
}

// --- extension pages and content scripts ----------------------------------------------------------------

gboolean on_message(WebKitUserContentManager *ucm, JSCValue *value, WebKitScriptMessageReply *reply, gpointer data);

void register_handler(WebKitUserContentManager *ucm, const char *world, int context)
{
    webkit_user_content_manager_register_script_message_handler_with_reply(ucm, HANDLER, world);
    // the content manager's signal is per manager; connect once per manager
    if (!g_object_get_data(G_OBJECT(ucm), "wisp-ext-connected")) {
        g_object_set_data(G_OBJECT(ucm), "wisp-ext-connected", GINT_TO_POINTER(1));
        g_object_set_data(G_OBJECT(ucm), "wisp-ext-context", GINT_TO_POINTER(context + 1000));
        g_signal_connect(ucm, "script-message-with-reply-received::wispExt", G_CALLBACK(on_message), nullptr);
    }
}

int context_of(WebKitUserContentManager *ucm)
{
    return GPOINTER_TO_INT(g_object_get_data(G_OBJECT(ucm), "wisp-ext-context")) - 1000;
}

void add_content_scripts(Extension &e, WebKitUserContentManager *ucm, int tab)
{
    const std::string world = world_of(e);
    register_handler(ucm, world.c_str(), tab);
    // the API first, everywhere the extension's scripts may run
    std::vector<Value *> scripts;
    Document &m = e.manifest;
    std::vector<std::string> all_matches;
    auto add_entry = [&](const Value &cs, bool registered) {
        std::vector<std::string> matches = match_list(cs, "matches");
        const std::vector<std::string> excludes = match_list(cs, registered ? "excludeMatches" : "exclude_matches");
        if (matches.empty())
            return;
        all_matches.insert(all_matches.end(), matches.begin(), matches.end());
        const CStrings allow(matches), block(excludes);
        const bool all_frames = (cs.HasMember(registered ? "allFrames" : "all_frames") && cs[registered ? "allFrames" : "all_frames"].IsBool()
            && cs[registered ? "allFrames" : "all_frames"].GetBool());
        const std::string run_at = str(cs, registered ? "runAt" : "run_at", "document_idle");
        const auto frames = all_frames ? WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES : WEBKIT_USER_CONTENT_INJECT_TOP_FRAME;
        const auto when = run_at == "document_start" ? WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START : WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_END;
        const bool main_world = str(cs, "world") == "MAIN";
        if (cs.HasMember("css") && cs["css"].IsArray()) {
            for (auto &file : cs["css"].GetArray()) {
                if (!file.IsString())
                    continue;
                const std::string css = read_text(e.dir + "/" + file.GetString());
                WebKitUserStyleSheet *sheet = webkit_user_style_sheet_new_for_world(css.c_str(), frames, WEBKIT_USER_STYLE_LEVEL_USER,
                    world.c_str(), allow.get(), block.get());
                webkit_user_content_manager_add_style_sheet(ucm, sheet);
                webkit_user_style_sheet_unref(sheet);
            }
        }
        if (cs.HasMember("js") && cs["js"].IsArray()) {
            std::string source;
            for (auto &file : cs["js"].GetArray())
                if (file.IsString())
                    source += read_text(e.dir + "/" + file.GetString()) + "\n;\n";
            WebKitUserScript *script = webkit_user_script_new_for_world(source.c_str(), frames, when,
                main_world ? nullptr : world.c_str(), allow.get(), block.get());
            webkit_user_content_manager_add_script(ucm, script);
            webkit_user_script_unref(script);
        }
    };
    // collect matches first (the API goes in before every script)
    if (m.HasMember("content_scripts") && m["content_scripts"].IsArray())
        for (auto &cs : m["content_scripts"].GetArray())
            for (auto &p : match_list(cs, "matches"))
                all_matches.push_back(p);
    for (auto &cs : e.registered_scripts.GetArray())
        for (auto &p : match_list(cs, "matches"))
            all_matches.push_back(p);
    if (all_matches.empty())
        all_matches = { "http://*/*", "https://*/*" };   // (for scripting.executeScript later)
    {
        const CStrings allow(all_matches);
        const std::string api = api_source(e, "content", tab);
        WebKitUserScript *script = webkit_user_script_new_for_world(api.c_str(), WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES,
            WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START, world.c_str(), allow.get(), nullptr);
        webkit_user_content_manager_add_script(ucm, script);
        webkit_user_script_unref(script);
    }
    all_matches.clear();
    if (m.HasMember("content_scripts") && m["content_scripts"].IsArray())
        for (auto &cs : m["content_scripts"].GetArray())
            add_entry(cs, false);
    for (auto &cs : e.registered_scripts.GetArray())
        add_entry(cs, true);
}

void attach_extensions(int tab, WebKitUserContentManager *ucm)
{
    webkit_user_content_manager_remove_all_scripts(ucm);
    webkit_user_content_manager_remove_all_style_sheets(ucm);
    // the browser's own first (v00.14-15 added them before this and lost them here: YouTube still met
    // no HTMLVideoElement)
    if (B.page_scripts)
        B.page_scripts(ucm);
    for (auto &e : E)
        if (e->enabled && e->error.empty())
            add_content_scripts(*e, ucm, tab);
}

void refresh_tabs()
{
    for (auto &[tab, ucm] : tab_ucms)
        attach_extensions(tab, ucm);
}

std::string background_page(const Extension &e)
{
    const Value &m = e.manifest;
    if (!m.HasMember("background") || !m["background"].IsObject())
        return {};
    const Value &bg = m["background"];
    if (bg.HasMember("page") && bg["page"].IsString())
        return "/" + std::string(bg["page"].GetString());
    if ((bg.HasMember("scripts") && bg["scripts"].IsArray()) || bg.HasMember("service_worker"))
        return "/_generated_background_page.html";
    return {};
}

std::string generated_background(const Extension &e)
{
    const Value &bg = e.manifest["background"];
    // a service worker's globals, enough for most extensions to run it as an event page
    std::string html = "<!doctype html><meta charset=utf-8><script>"
        "globalThis.importScripts=(...u)=>{for(const x of u){const r=new XMLHttpRequest();r.open('GET',x,false);r.send();(0,eval)(r.responseText);}};"
        "globalThis.clients=globalThis.clients||{matchAll:()=>Promise.resolve([]),claim:()=>Promise.resolve(),openWindow:u=>chrome.tabs.create({url:u})};"
        "globalThis.registration=globalThis.registration||{scope:location.origin+'/',unregister:()=>Promise.resolve(false)};"
        "globalThis.skipWaiting=()=>Promise.resolve();"
        "</script>";
    if (bg.HasMember("scripts") && bg["scripts"].IsArray())
        for (auto &s : bg["scripts"].GetArray())
            if (s.IsString())
                html += "<script src=\"/" + std::string(s.GetString()) + "\"></script>";
    if (bg.HasMember("service_worker") && bg["service_worker"].IsString())
        html += std::string("<script") + (str(bg, "type") == "module" ? " type=\"module\"" : "") + " src=\"/"
            + bg["service_worker"].GetString() + "\"></script>";
    return html;
}

WebKitUserContentManager *extension_page_ucm(Extension &e, int context)
{
    WebKitUserContentManager *ucm = webkit_user_content_manager_new();
    register_handler(ucm, nullptr, context);
    const std::string api = api_source(e, "extension", -1);
    const std::string origin = "chrome-extension://" + e.id + "/*";
    const char *const allow[] = { origin.c_str(), nullptr };
    WebKitUserScript *script = webkit_user_script_new(api.c_str(), WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES,
        WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START, allow, nullptr);
    webkit_user_content_manager_add_script(ucm, script);
    webkit_user_script_unref(script);
    g_object_set_data_full(G_OBJECT(ucm), "wisp-ext-id", g_strdup(e.id.c_str()), g_free);
    return ucm;
}

void start_background(Extension &e)
{
    const std::string page = background_page(e);
    if (page.empty() || e.background || !B.make_view)
        return;
    e.background_ucm = extension_page_ucm(e, CONTEXT_BACKGROUND);
    e.background = B.make_view(e.background_ucm);
    if (!e.background)
        return;
    const std::string uri = "chrome-extension://" + e.id + page;
    webkit_web_view_load_uri(e.background, uri.c_str());
    wisp_log("ext %s: background page %s", e.id.c_str(), uri.c_str());
}

void stop_background(Extension &e)
{
    if (e.background) {
        webkit_web_view_try_close(e.background);
        g_object_unref(e.background);
        e.background = nullptr;
    }
    if (e.background_ucm) {
        g_object_unref(e.background_ucm);
        e.background_ucm = nullptr;
    }
}

// --- events into the extension's contexts ----------------------------------------------------------

void eval_in(WebKitWebView *view, const char *world, const std::string &script)
{
    if (view)
        webkit_web_view_evaluate_javascript(view, script.c_str(), static_cast<gssize>(script.size()), world, nullptr, nullptr, nullptr, nullptr);
}

std::string dispatch_call(const std::string &event, const std::string &args_json, const std::string &extra_json = "null")
{
    return "globalThis.__wispExt&&globalThis.__wispExt.dispatch(" + json_string(event) + "," + args_json + "," + extra_json + ");";
}

// to the extension's pages (background, popup)
void to_pages(Extension &e, const std::string &script)
{
    eval_in(e.background, nullptr, script);
}

// to the extension's content scripts in a tab (every frame's world: the dispatch runs in each)
void to_tab(Extension &e, int tab, const std::string &script)
{
    if (B.tab_view)
        eval_in(B.tab_view(tab), world_of(e).c_str(), script);
}

void broadcast(const std::string &event, const std::string &args_json)
{
    const std::string script = dispatch_call(event, args_json);
    for (auto &e : E)
        if (e->enabled)
            to_pages(*e, script);
}

// --- the API --------------------------------------------------------------------------------------------

std::string tab_json(int tab)
{
    WebKitWebView *view = B.tab_view ? B.tab_view(tab) : nullptr;
    if (!view)
        return "null";
    const auto ids = B.tab_ids();
    const int index = static_cast<int>(std::find(ids.begin(), ids.end(), tab) - ids.begin());
    const char *uri = webkit_web_view_get_uri(view);
    const char *title = webkit_web_view_get_title(view);
    return "{\"id\":" + std::to_string(tab) + ",\"index\":" + std::to_string(index) + ",\"windowId\":1,\"active\":"
        + (B.active_tab() == tab ? "true" : "false") + ",\"highlighted\":" + (B.active_tab() == tab ? "true" : "false")
        + ",\"pinned\":false,\"incognito\":false,\"discarded\":false,\"autoDiscardable\":true,\"selected\":"
        + (B.active_tab() == tab ? "true" : "false") + ",\"status\":\"" + (webkit_web_view_is_loading(view) ? "loading" : "complete")
        + "\",\"url\":" + json_string(uri ? uri : "") + ",\"title\":" + json_string(title ? title : "") + ",\"favIconUrl\":\"\"}";
}

std::string sender_json(const Extension &e, int context, const std::string &url, bool top)
{
    std::string s = "{\"id\":" + json_string(e.id) + ",\"url\":" + json_string(url) + ",\"origin\":" + json_string(url);
    if (context >= 0)
        s += ",\"tab\":" + tab_json(context) + ",\"frameId\":" + (top ? "0" : "1");
    return s + "}";
}

void reply_json(WebKitScriptMessageReply *reply, JSCContext *ctx, const std::string &json)
{
    JSCValue *value = jsc_value_new_from_json(ctx, json.c_str());
    if (!value)
        value = jsc_value_new_null(ctx);
    webkit_script_message_reply_return_value(reply, value);
    g_object_unref(value);
}

void reply_error(WebKitScriptMessageReply *reply, JSCContext *ctx, const std::string &message)
{
    reply_json(reply, ctx, "{\"__wispError\":" + json_string(message) + "}");
}

std::string storage_get(Extension &e, const std::string &area, const Value &keys)
{
    Document &d = e.storage[area];
    Document out(rapidjson::kObjectType);
    auto &a = out.GetAllocator();
    if (keys.IsNull()) {
        out.CopyFrom(d, a);
    } else if (keys.IsString()) {
        if (d.HasMember(keys.GetString()))
            out.AddMember(Value(keys.GetString(), a), Value(d[keys.GetString()], a), a);
    } else if (keys.IsArray()) {
        for (auto &k : keys.GetArray())
            if (k.IsString() && d.HasMember(k.GetString()))
                out.AddMember(Value(k.GetString(), a), Value(d[k.GetString()], a), a);
    } else if (keys.IsObject()) {
        for (auto &m : keys.GetObject())
            out.AddMember(Value(m.name.GetString(), a), d.HasMember(m.name.GetString()) ? Value(d[m.name.GetString()], a) : Value(m.value, a), a);
    }
    return to_json(out);
}

void storage_changed(Extension &e, const std::string &area, const std::string &changes)
{
    if (area != "session")
        write_text(e.dir + "/.wisp-storage-" + area + ".json", to_json(e.storage[area]));
    const std::string args = "[" + changes + "," + json_string(area) + "]";
    const std::string script = dispatch_call("storage.onChanged", args) + dispatch_call("storage." + area + ".onChanged", "[" + changes + "]");
    to_pages(e, script);
    for (int tab : B.tab_ids())
        to_tab(e, tab, script);
}

void run_in_tab(Extension &e, int tab, const std::string &source, const std::string &world, bool all_frames,
    std::function<void(const std::string &)> done)
{
    WebKitWebView *view = B.tab_view ? B.tab_view(tab) : nullptr;
    if (!view) {
        done("null");
        return;
    }
    (void)all_frames;
    auto *callback = new std::function<void(const std::string &)>(std::move(done));
    webkit_web_view_evaluate_javascript(view, source.c_str(), static_cast<gssize>(source.size()),
        world.empty() ? nullptr : world.c_str(), nullptr, nullptr, +[](GObject *object, GAsyncResult *result, gpointer data) {
            std::unique_ptr<std::function<void(const std::string &)>> done(static_cast<std::function<void(const std::string &)> *>(data));
            GError *error = nullptr;
            JSCValue *value = webkit_web_view_evaluate_javascript_finish(WEBKIT_WEB_VIEW(object), result, &error);
            std::string json = "null";
            if (value) {
                if (!jsc_value_is_undefined(value)) {
                    char *text = jsc_value_to_json(value, 0);
                    if (text)
                        json = text;
                    g_free(text);
                }
                g_object_unref(value);
            }
            g_clear_error(&error);
            (*done)(json);
        }, callback);
    (void)e;
}

int target_tab(const Value &args0)
{
    if (args0.IsObject() && args0.HasMember("target") && args0["target"].IsObject() && args0["target"].HasMember("tabId")
        && args0["target"]["tabId"].IsInt())
        return args0["target"]["tabId"].GetInt();
    return B.active_tab ? B.active_tab() : -1;
}

gboolean on_message(WebKitUserContentManager *ucm, JSCValue *value, WebKitScriptMessageReply *reply, gpointer)
{
    JSCContext *ctx = jsc_value_get_context(value);
    char *text = jsc_value_to_json(value, 0);
    Document msg;
    const bool parsed = text && !msg.Parse(text).HasParseError() && msg.IsObject();
    g_free(text);
    if (!parsed) {
        reply_error(reply, ctx, "bad message");
        return TRUE;
    }
    // the sender: a tab's content manager says which tab; an extension view's says which extension
    const int context = context_of(ucm);
    std::string ext_id = str(msg, "ext");
    if (const char *owner = static_cast<const char *>(g_object_get_data(G_OBJECT(ucm), "wisp-ext-id")))
        ext_id = owner;
    Extension *e = find(ext_id);
    if (!e || !e->enabled) {
        reply_error(reply, ctx, "no such extension");
        return TRUE;
    }
    const std::string api = str(msg, "api"), method = str(msg, "method"), url = str(msg, "url");
    const bool top = msg.HasMember("top") && msg["top"].IsBool() && msg["top"].GetBool();
    Value empty(rapidjson::kArrayType);
    const Value &args = msg.HasMember("args") && msg["args"].IsArray() ? msg["args"] : empty;
    const Value null_value;
    auto arg = [&](unsigned i) -> const Value & { return i < args.Size() ? args[i] : null_value; };
    auto &alloc = msg.GetAllocator();

    if (api == "events")
        return reply_json(reply, ctx, "null"), TRUE;

    if (api == "storage") {
        const size_t dot = method.find('.');
        const std::string op = method.substr(0, dot), area = dot == std::string::npos ? "local" : method.substr(dot + 1);
        if (!e->storage.count(area))
            e->storage[area].SetObject();
        Document &d = e->storage[area];
        if (op == "get") {
            reply_json(reply, ctx, storage_get(*e, area, arg(0)));
        } else if (op == "set" && arg(0).IsObject()) {
            Document changes(rapidjson::kObjectType);
            auto &a = changes.GetAllocator();
            for (auto &m : arg(0).GetObject()) {
                Value change(rapidjson::kObjectType);
                if (d.HasMember(m.name.GetString())) {
                    change.AddMember("oldValue", Value(d[m.name.GetString()], a), a);
                    d.RemoveMember(m.name.GetString());
                }
                change.AddMember("newValue", Value(m.value, a), a);
                changes.AddMember(Value(m.name.GetString(), a), change, a);
                d.AddMember(Value(m.name.GetString(), d.GetAllocator()), Value(m.value, d.GetAllocator()), d.GetAllocator());
            }
            storage_changed(*e, area, to_json(changes));
            reply_json(reply, ctx, "null");
        } else if (op == "remove" || op == "clear") {
            Document changes(rapidjson::kObjectType);
            auto &a = changes.GetAllocator();
            std::vector<std::string> keys;
            if (op == "clear") {
                for (auto &m : d.GetObject())
                    keys.push_back(m.name.GetString());
            } else if (arg(0).IsString()) {
                keys.push_back(arg(0).GetString());
            } else if (arg(0).IsArray()) {
                for (auto &k : arg(0).GetArray())
                    if (k.IsString())
                        keys.push_back(k.GetString());
            }
            for (auto &k : keys) {
                if (!d.HasMember(k.c_str()))
                    continue;
                Value change(rapidjson::kObjectType);
                change.AddMember("oldValue", Value(d[k.c_str()], a), a);
                changes.AddMember(Value(k.c_str(), a), change, a);
                d.RemoveMember(k.c_str());
            }
            storage_changed(*e, area, to_json(changes));
            reply_json(reply, ctx, "null");
        } else if (op == "getBytesInUse") {
            reply_json(reply, ctx, std::to_string(to_json(d).size()));
        } else {
            reply_error(reply, ctx, "storage." + method + " is not supported");
        }
        return TRUE;
    }

    if (api == "runtime") {
        if (method == "sendMessage") {
            // to the extension's pages from a content script; between pages otherwise
            const std::string id = "r" + std::to_string(next_reply++);
            pending[id] = { webkit_script_message_reply_ref(reply) };
            const std::string args_json = "[" + to_json(arg(1)) + "," + sender_json(*e, context, url, top) + "]";
            if (!e->background)
                start_background(*e);
            to_pages(*e, dispatch_call("runtime.onMessage", args_json, json_string(id)));
            // nobody answers within 30 s: undefined
            g_timeout_add_seconds(30, +[](gpointer data) -> gboolean {
                std::unique_ptr<std::string> id(static_cast<std::string *>(data));
                auto it = pending.find(*id);
                if (it != pending.end()) {
                    JSCContext *c = jsc_context_new();
                    reply_json(it->second.reply, c, "null");
                    g_object_unref(c);
                    webkit_script_message_reply_unref(it->second.reply);
                    pending.erase(it);
                }
                return G_SOURCE_REMOVE;
            }, new std::string(id));
            return TRUE;
        }
        if (method == "respond") {
            const std::string id = arg(0).IsString() ? arg(0).GetString() : "";
            auto it = pending.find(id);
            if (it != pending.end()) {
                reply_json(it->second.reply, ctx, to_json(arg(1)));
                webkit_script_message_reply_unref(it->second.reply);
                pending.erase(it);
            }
            reply_json(reply, ctx, "null");
            return TRUE;
        }
        if (method == "connect" || method == "portPost" || method == "portClose") {
            const std::string port = arg(0).IsString() ? arg(0).GetString() : "";
            if (method == "connect") {
                port_table[port] = { e->id, context, CONTEXT_BACKGROUND };
                if (!e->background)
                    start_background(*e);
                const std::string name = arg(1).IsString() ? arg(1).GetString() : "";
                to_pages(*e, dispatch_call("runtime.onConnect", "[" + json_string(port) + "," + json_string(name) + ","
                    + sender_json(*e, context, url, top) + "]"));
            } else if (port_table.count(port)) {
                const Port p = port_table[port];
                const int other = context == p.a_tab ? p.b_tab : p.a_tab;
                const std::string script = method == "portPost" ? dispatch_call("port.message", "[" + json_string(port) + "," + to_json(arg(1)) + "]")
                    : dispatch_call("port.disconnect", "[" + json_string(port) + "]");
                if (other >= 0)
                    to_tab(*e, other, script);
                else
                    to_pages(*e, script);
                if (method == "portClose")
                    port_table.erase(port);
            }
            reply_json(reply, ctx, "null");
            return TRUE;
        }
        if (method == "openOptionsPage") {
            std::string page = str(e->manifest, "options_page");
            if (page.empty() && e->manifest.HasMember("options_ui"))
                page = str(e->manifest["options_ui"], "page");
            if (!page.empty() && B.open_tab)
                B.open_tab("chrome-extension://" + e->id + "/" + page, true);
            reply_json(reply, ctx, "null");
            return TRUE;
        }
        if (method == "reload") {
            stop_background(*e);
            start_background(*e);
            reply_json(reply, ctx, "null");
            return TRUE;
        }
        reply_json(reply, ctx, "[]");
        return TRUE;
    }

    if (api == "tabs") {
        if (method == "query") {
            const Value &q = arg(0);
            std::string out = "[";
            for (int tab : B.tab_ids()) {
                if (q.IsObject()) {
                    if (q.HasMember("active") && q["active"].IsBool() && q["active"].GetBool() != (B.active_tab() == tab))
                        continue;
                    if (q.HasMember("url") && q["url"].IsString()) {
                        WebKitWebView *v = B.tab_view(tab);
                        const char *u = v ? webkit_web_view_get_uri(v) : nullptr;
                        std::string pattern = q["url"].GetString();
                        if (pattern != "<all_urls>" && (!u || !g_pattern_match_simple(pattern.c_str(), u)))
                            continue;
                    }
                }
                if (out.size() > 1)
                    out += ",";
                out += tab_json(tab);
            }
            reply_json(reply, ctx, out + "]");
        } else if (method == "get") {
            reply_json(reply, ctx, tab_json(arg(0).IsInt() ? arg(0).GetInt() : -1));
        } else if (method == "create") {
            const std::string u = str(arg(0), "url", "about:blank");
            const bool active = !(arg(0).IsObject() && arg(0).HasMember("active") && arg(0)["active"].IsBool() && !arg(0)["active"].GetBool());
            std::string full = u;
            if (u.find(':') == std::string::npos)
                full = "chrome-extension://" + e->id + "/" + (u[0] == '/' ? u.substr(1) : u);
            const int tab = B.open_tab ? B.open_tab(full, active) : -1;
            reply_json(reply, ctx, tab_json(tab));
        } else if (method == "update") {
            const int tab = arg(0).IsInt() ? arg(0).GetInt() : B.active_tab();
            const Value &props = arg(0).IsInt() ? arg(1) : arg(0);
            WebKitWebView *v = B.tab_view(tab);
            if (v && props.IsObject() && props.HasMember("url") && props["url"].IsString())
                webkit_web_view_load_uri(v, props["url"].GetString());
            if (props.IsObject() && props.HasMember("active") && props["active"].IsBool() && props["active"].GetBool())
                B.activate_tab(tab);
            reply_json(reply, ctx, tab_json(tab));
        } else if (method == "remove") {
            if (arg(0).IsInt())
                B.close_tab(arg(0).GetInt());
            else if (arg(0).IsArray())
                for (auto &t : arg(0).GetArray())
                    if (t.IsInt())
                        B.close_tab(t.GetInt());
            reply_json(reply, ctx, "null");
        } else if (method == "reload" || method == "goBack" || method == "goForward") {
            const int tab = arg(0).IsInt() ? arg(0).GetInt() : B.active_tab();
            if (WebKitWebView *v = B.tab_view(tab))
                method == "reload" ? webkit_web_view_reload(v) : method == "goBack" ? webkit_web_view_go_back(v) : webkit_web_view_go_forward(v);
            reply_json(reply, ctx, "null");
        } else if (method == "sendMessage") {
            const int tab = arg(0).IsInt() ? arg(0).GetInt() : -1;
            const std::string id = "r" + std::to_string(next_reply++);
            pending[id] = { webkit_script_message_reply_ref(reply) };
            to_tab(*e, tab, dispatch_call("runtime.onMessage", "[" + to_json(arg(1)) + "," + sender_json(*e, context, url, top) + "]",
                json_string(id)));
            g_timeout_add_seconds(30, +[](gpointer data) -> gboolean {
                std::unique_ptr<std::string> id(static_cast<std::string *>(data));
                auto it = pending.find(*id);
                if (it != pending.end()) {
                    JSCContext *c = jsc_context_new();
                    reply_json(it->second.reply, c, "null");
                    g_object_unref(c);
                    webkit_script_message_reply_unref(it->second.reply);
                    pending.erase(it);
                }
                return G_SOURCE_REMOVE;
            }, new std::string(id));
        } else if (method == "connect") {
            const int tab = arg(0).IsInt() ? arg(0).GetInt() : -1;
            const std::string port = arg(1).IsString() ? arg(1).GetString() : "";
            port_table[port] = { e->id, context, tab };
            to_tab(*e, tab, dispatch_call("runtime.onConnect", "[" + json_string(port) + "," + to_json(arg(2)) + ","
                + sender_json(*e, context, url, top) + "]"));
            reply_json(reply, ctx, "null");
        } else if (method == "executeScript" || method == "insertCSS") {
            // MV2: tabs.executeScript(tabId?, {code|file})
            const int tab = arg(0).IsInt() ? arg(0).GetInt() : B.active_tab();
            const Value &details = arg(0).IsInt() ? arg(1) : arg(0);
            std::string source = str(details, "code");
            if (source.empty() && details.IsObject() && details.HasMember("file"))
                source = read_text(e->dir + "/" + str(details, "file"));
            if (method == "insertCSS")
                source = "(()=>{const s=document.createElement('style');s.textContent=" + json_string(source)
                    + ";(document.head||document.documentElement).appendChild(s);})()";
            WebKitScriptMessageReply *held = webkit_script_message_reply_ref(reply);
            JSCContext *held_ctx = JSC_CONTEXT(g_object_ref(ctx));
            run_in_tab(*e, tab, source, world_of(*e), false, [held, held_ctx](const std::string &result) {
                reply_json(held, held_ctx, "[" + result + "]");
                webkit_script_message_reply_unref(held);
                g_object_unref(held_ctx);
            });
        } else if (method == "getZoom") {
            WebKitWebView *v = B.tab_view(arg(0).IsInt() ? arg(0).GetInt() : B.active_tab());
            reply_json(reply, ctx, std::to_string(v ? webkit_web_view_get_zoom_level(v) : 1.0));
        } else if (method == "setZoom") {
            const int tab = arg(0).IsInt() ? arg(0).GetInt() : B.active_tab();
            const Value &level = arg(0).IsInt() ? arg(1) : arg(0);
            if (WebKitWebView *v = B.tab_view(tab); v && level.IsNumber())
                webkit_web_view_set_zoom_level(v, level.GetDouble() > 0 ? level.GetDouble() : 1.0);
            reply_json(reply, ctx, "null");
        } else {
            reply_json(reply, ctx, "null");
        }
        return TRUE;
    }

    if (api == "windows") {
        std::string tabs = "[";
        for (int tab : B.tab_ids())
            tabs += (tabs.size() > 1 ? "," : "") + tab_json(tab);
        tabs += "]";
        const std::string window = "{\"id\":1,\"focused\":true,\"top\":0,\"left\":0,\"width\":1920,\"height\":1080,\"incognito\":false,"
            "\"type\":\"normal\",\"state\":\"fullscreen\",\"alwaysOnTop\":false,\"tabs\":" + tabs + "}";
        reply_json(reply, ctx, method == "getAll" ? "[" + window + "]" : window);
        return TRUE;
    }

    if (api == "scripting") {
        const Value &d = arg(0);
        const int tab = target_tab(d);
        if (method == "executeScript") {
            std::string world = str(d, "world") == "MAIN" ? "" : world_of(*e);
            std::string source;
            if (d.HasMember("files") && d["files"].IsArray()) {
                for (auto &f : d["files"].GetArray())
                    if (f.IsString())
                        source += read_text(e->dir + "/" + std::string(f.GetString()[0] == '/' ? f.GetString() + 1 : f.GetString())) + "\n;\n";
            } else if (d.HasMember("funcSource") && d["funcSource"].IsString()) {
                const std::string fn_args = d.HasMember("args") ? to_json(d["args"]) : "[]";
                source = "(" + std::string(d["funcSource"].GetString()) + ")(..." + fn_args + ")";
            }
            WebKitScriptMessageReply *held = webkit_script_message_reply_ref(reply);
            JSCContext *held_ctx = JSC_CONTEXT(g_object_ref(ctx));
            run_in_tab(*e, tab, source, world, false, [held, held_ctx](const std::string &result) {
                reply_json(held, held_ctx, "[{\"frameId\":0,\"result\":" + result + "}]");
                webkit_script_message_reply_unref(held);
                g_object_unref(held_ctx);
            });
            return TRUE;
        }
        if (method == "insertCSS" || method == "removeCSS") {
            std::string css = str(d, "css");
            if (d.HasMember("files") && d["files"].IsArray())
                for (auto &f : d["files"].GetArray())
                    if (f.IsString())
                        css += read_text(e->dir + "/" + std::string(f.GetString())) + "\n";
            const std::string key = std::to_string(std::hash<std::string>{}(css));
            const std::string source = method == "insertCSS"
                ? "(()=>{const s=document.createElement('style');s.dataset.wisp=" + json_string(key) + ";s.textContent=" + json_string(css)
                    + ";(document.head||document.documentElement).appendChild(s);})()"
                : "document.querySelectorAll('style[data-wisp=\"" + key + "\"]').forEach(s=>s.remove())";
            run_in_tab(*e, tab, source, world_of(*e), false, [](const std::string &) {});
            reply_json(reply, ctx, "null");
            return TRUE;
        }
        if (method == "registerContentScripts" || method == "unregisterContentScripts" || method == "updateContentScripts") {
            Document &list = e->registered_scripts;
            auto &a = list.GetAllocator();
            std::set<std::string> ids;
            if (method != "registerContentScripts") {
                const Value &filter = arg(0);
                if (filter.IsObject() && filter.HasMember("ids") && filter["ids"].IsArray())
                    for (auto &id : filter["ids"].GetArray())
                        if (id.IsString())
                            ids.insert(id.GetString());
                if (method == "updateContentScripts" && arg(0).IsArray())
                    for (auto &s : arg(0).GetArray())
                        ids.insert(str(s, "id"));
                Document kept(rapidjson::kArrayType);
                for (auto &s : list.GetArray())
                    if (!(ids.empty() && method == "unregisterContentScripts") && !ids.count(str(s, "id")))
                        kept.PushBack(Value(s, kept.GetAllocator()), kept.GetAllocator());
                list.CopyFrom(kept, a);
            }
            if (method != "unregisterContentScripts" && arg(0).IsArray())
                for (auto &s : arg(0).GetArray())
                    list.PushBack(Value(s, a), a);
            write_text(e->dir + "/.wisp-scripts.json", to_json(list));
            refresh_tabs();
            reply_json(reply, ctx, "null");
            return TRUE;
        }
        if (method == "getRegisteredContentScripts") {
            reply_json(reply, ctx, to_json(e->registered_scripts));
            return TRUE;
        }
        reply_json(reply, ctx, "null");
        return TRUE;
    }

    if (api == "dnr") {
        if (method == "updateDynamicRules" || method == "updateSessionRules") {
            const Value &opts = arg(0);
            std::set<int> remove;
            if (opts.IsObject() && opts.HasMember("removeRuleIds") && opts["removeRuleIds"].IsArray())
                for (auto &id : opts["removeRuleIds"].GetArray())
                    if (id.IsInt())
                        remove.insert(id.GetInt());
            Document kept(rapidjson::kArrayType);
            for (auto &r : e->dynamic_rules.GetArray())
                if (!(r.HasMember("id") && r["id"].IsInt() && remove.count(r["id"].GetInt())))
                    kept.PushBack(Value(r, kept.GetAllocator()), kept.GetAllocator());
            if (opts.IsObject() && opts.HasMember("addRules") && opts["addRules"].IsArray())
                for (auto &r : opts["addRules"].GetArray())
                    kept.PushBack(Value(r, kept.GetAllocator()), kept.GetAllocator());
            e->dynamic_rules.CopyFrom(kept, e->dynamic_rules.GetAllocator());
            write_text(e->dir + "/.wisp-dnr.json", to_json(e->dynamic_rules));
            compile_dnr(*e);
            reply_json(reply, ctx, "null");
        } else if (method == "getDynamicRules" || method == "getSessionRules") {
            reply_json(reply, ctx, to_json(e->dynamic_rules));
        } else if (method == "updateEnabledRulesets") {
            const Value &opts = arg(0);
            for (const char *key : { "enableRulesetIds", "disableRulesetIds" })
                if (opts.IsObject() && opts.HasMember(key) && opts[key].IsArray())
                    for (auto &id : opts[key].GetArray()) {
                        if (!id.IsString())
                            continue;
                        if (!strcmp(key, "enableRulesetIds")) {
                            e->disabled_rulesets.erase(id.GetString());
                            e->enabled_rulesets_extra.insert(id.GetString());
                        } else {
                            e->enabled_rulesets_extra.erase(id.GetString());
                            e->disabled_rulesets.insert(id.GetString());
                        }
                    }
            compile_dnr(*e);
            reply_json(reply, ctx, "null");
        } else if (method == "getEnabledRulesets") {
            std::string out = "[";
            if (e->manifest.HasMember("declarative_net_request") && e->manifest["declarative_net_request"].HasMember("rule_resources"))
                for (auto &res : e->manifest["declarative_net_request"]["rule_resources"].GetArray()) {
                    const std::string id = str(res, "id");
                    bool on = (res.HasMember("enabled") && res["enabled"].IsBool() && res["enabled"].GetBool()) || e->enabled_rulesets_extra.count(id);
                    if (on && !e->disabled_rulesets.count(id))
                        out += (out.size() > 1 ? "," : "") + json_string(id);
                }
            reply_json(reply, ctx, out + "]");
        } else {
            reply_json(reply, ctx, "[]");
        }
        return TRUE;
    }

    if (api == "action") {
        if (method == "setBadgeText") {
            e->badge_text = str(arg(0), "text");
            if (B.redraw)
                B.redraw();
        } else if (method == "setBadgeBackgroundColor") {
            const Value &color = arg(0).IsObject() && arg(0).HasMember("color") ? arg(0)["color"] : null_value;
            if (color.IsArray() && color.Size() >= 3)
                e->badge_color = 0xff000000u | color[2].GetUint() << 16 | color[1].GetUint() << 8 | color[0].GetUint();
            if (B.redraw)
                B.redraw();
        } else if (method == "setPopup") {
            e->popup = str(arg(0), "popup");
        } else if (method == "getBadgeText") {
            reply_json(reply, ctx, json_string(e->badge_text));
            return TRUE;
        } else if (method == "getPopup") {
            reply_json(reply, ctx, json_string(e->popup.empty() ? "" : "chrome-extension://" + e->id + "/" + e->popup));
            return TRUE;
        }
        reply_json(reply, ctx, "null");
        return TRUE;
    }

    if (api == "alarms") {
        if (method == "create") {
            const std::string name = arg(0).IsString() ? arg(0).GetString() : "";
            const Value &info = arg(1);
            double delay = 60;
            if (info.IsObject() && info.HasMember("delayInMinutes") && info["delayInMinutes"].IsNumber())
                delay = info["delayInMinutes"].GetDouble() * 60;
            else if (info.IsObject() && info.HasMember("periodInMinutes") && info["periodInMinutes"].IsNumber())
                delay = info["periodInMinutes"].GetDouble() * 60;
            else if (info.IsObject() && info.HasMember("when") && info["when"].IsNumber())
                delay = std::max(0.0, (info["when"].GetDouble() - g_get_real_time() / 1000.0) / 1000.0);
            const double period = info.IsObject() && info.HasMember("periodInMinutes") && info["periodInMinutes"].IsNumber()
                ? info["periodInMinutes"].GetDouble() * 60 : 0;
            const std::string key = e->id + "\n" + name;
            if (alarms.count(key))
                g_source_remove(alarms[key]);
            struct Alarm { std::string key, ext, name; double period; };
            auto *alarm = new Alarm { key, e->id, name, period };
            alarms[key] = g_timeout_add_full(G_PRIORITY_DEFAULT, static_cast<guint>(std::max(1.0, delay) * 1000), +[](gpointer data) -> gboolean {
                auto *alarm = static_cast<Alarm *>(data);
                if (Extension *e = find(alarm->ext)) {
                    if (!e->background)
                        start_background(*e);
                    to_pages(*e, dispatch_call("alarms.onAlarm", "[{\"name\":" + json_string(alarm->name) + ",\"scheduledTime\":"
                        + std::to_string(g_get_real_time() / 1000) + "}]"));
                }
                if (alarm->period > 0) {
                    alarms[alarm->key] = g_timeout_add_full(G_PRIORITY_DEFAULT, static_cast<guint>(alarm->period * 1000), +[](gpointer d) -> gboolean {
                        auto *a = static_cast<Alarm *>(d);
                        if (Extension *e = find(a->ext))
                            to_pages(*e, dispatch_call("alarms.onAlarm", "[{\"name\":" + json_string(a->name) + "}]"));
                        return G_SOURCE_CONTINUE;
                    }, alarm, nullptr);
                    return G_SOURCE_REMOVE;
                }
                alarms.erase(alarm->key);
                delete alarm;
                return G_SOURCE_REMOVE;
            }, alarm, nullptr);
        } else if (method == "clear" || method == "clearAll") {
            for (auto it = alarms.begin(); it != alarms.end();) {
                if (it->first.rfind(e->id + "\n", 0) == 0 && (method == "clearAll" || it->first == e->id + "\n" + (arg(0).IsString() ? arg(0).GetString() : ""))) {
                    g_source_remove(it->second);
                    it = alarms.erase(it);
                } else {
                    ++it;
                }
            }
            reply_json(reply, ctx, "true");
            return TRUE;
        }
        reply_json(reply, ctx, method == "getAll" ? "[]" : "null");
        return TRUE;
    }

    if (api == "notifications" && method == "create") {
        const Value &o = arg(1);
        const std::string title = str(o, "title"), message = str(o, "message");
        if (B.toast)
            B.toast(localized(*e, str(e->manifest, "name")) + ": " + title + (message.empty() ? "" : " - " + message));
        reply_json(reply, ctx, json_string(arg(0).IsString() ? arg(0).GetString() : "n" + std::to_string(next_reply++)));
        return TRUE;
    }

    (void)alloc;
    reply_json(reply, ctx, "null");
    return TRUE;
}

// --- chrome-extension:// --------------------------------------------------------------------------------

const char *mime_type(const std::string &path)
{
    const size_t dot = path.find_last_of('.');
    const std::string ext = dot == std::string::npos ? "" : path.substr(dot + 1);
    static const std::map<std::string, const char *> types = { { "html", "text/html" }, { "htm", "text/html" },
        { "js", "text/javascript" }, { "mjs", "text/javascript" }, { "css", "text/css" }, { "json", "application/json" },
        { "png", "image/png" }, { "jpg", "image/jpeg" }, { "jpeg", "image/jpeg" }, { "gif", "image/gif" },
        { "svg", "image/svg+xml" }, { "webp", "image/webp" }, { "ico", "image/x-icon" }, { "woff", "font/woff" },
        { "woff2", "font/woff2" }, { "ttf", "font/ttf" }, { "otf", "font/otf" }, { "wasm", "application/wasm" },
        { "txt", "text/plain" }, { "xml", "application/xml" }, { "map", "application/json" } };
    auto it = types.find(ext);
    return it == types.end() ? "application/octet-stream" : it->second;
}

void serve(WebKitURISchemeRequest *request, gpointer)
{
    const char *uri = webkit_uri_scheme_request_get_uri(request);
    // chrome-extension://<id>/<path>
    std::string rest = uri + std::string(uri).find("://") + 3;
    const size_t slash = rest.find('/');
    const std::string id = rest.substr(0, slash);
    std::string path = slash == std::string::npos ? "/" : rest.substr(slash);
    const size_t query = path.find_first_of("?#");
    if (query != std::string::npos)
        path.erase(query);
    char *unescaped = g_uri_unescape_string(path.c_str(), nullptr);
    path = unescaped ? unescaped : path;
    g_free(unescaped);
    Extension *e = find(id);
    if (!e || path.find("..") != std::string::npos) {
        GError *error = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "no such extension file");
        webkit_uri_scheme_request_finish_error(request, error);
        g_error_free(error);
        return;
    }
    std::string body;
    if (path == "/_generated_background_page.html")
        body = generated_background(*e);
    else
        body = read_text(e->dir + (path == "/" ? "/index.html" : path));
    struct stat st;
    if (body.empty() && path != "/_generated_background_page.html" && stat((e->dir + path).c_str(), &st)) {
        GError *error = g_error_new(G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "%s not found in %s", path.c_str(), id.c_str());
        webkit_uri_scheme_request_finish_error(request, error);
        g_error_free(error);
        return;
    }
    GBytes *bytes = g_bytes_new(body.data(), body.size());
    GInputStream *stream = g_memory_input_stream_new_from_bytes(bytes);
    WebKitURISchemeResponse *response = webkit_uri_scheme_response_new(stream, static_cast<gint64>(body.size()));
    webkit_uri_scheme_response_set_content_type(response, mime_type(path));
    SoupMessageHeaders *headers = soup_message_headers_new(SOUP_MESSAGE_HEADERS_RESPONSE);
    soup_message_headers_append(headers, "Access-Control-Allow-Origin", "*");
    webkit_uri_scheme_response_set_http_headers(response, headers);
    webkit_uri_scheme_request_finish_with_response(request, response);
    g_object_unref(response);
    g_object_unref(stream);
    g_bytes_unref(bytes);
}

void load_all()
{
    mkdir(ROOT, 0777);
    DIR *dir = opendir(ROOT);
    if (!dir)
        return;
    std::vector<std::string> ids;
    while (dirent *d = readdir(dir))
        if (d->d_name[0] != '.')
            ids.push_back(d->d_name);
    closedir(dir);
    std::sort(ids.begin(), ids.end());
    for (const std::string &id : ids) {
        auto e = load_extension(std::string(ROOT) + "/" + id, id);
        if (!e) {
            wisp_log("ext: %s has no readable manifest", id.c_str());
            continue;
        }
        wisp_log("ext: %s \"%s\" %s (MV%d)%s", id.c_str(), localized(*e, str(e->manifest, "name")).c_str(),
            str(e->manifest, "version").c_str(), e->manifest.HasMember("manifest_version") && e->manifest["manifest_version"].IsInt()
                ? e->manifest["manifest_version"].GetInt() : 2, e->enabled ? "" : " (off)");
        E.push_back(std::move(e));
    }
}

void start(Extension &e)
{
    compile_dnr(e);
    start_background(e);
}

// the built-in ad blocker's lists (data/adblock: EasyList, EasyPrivacy), compiled once into the
// filter store; later starts load them
void compile_adblock()
{
    for (const char *list : { "easylist", "easyprivacy" }) {
        const std::string id = std::string("adblock-") + list;
        if (filters.count(id))
            continue;
        const std::string json = read_text(std::string("/app0/wisp/adblock/") + list + ".json");
        if (!json.empty())
            compile_rules(id, json);
    }
}

} // namespace

void init(const Browser &browser)
{
    B = browser;
    adblock_on = wisp_setting_int("adblock", 0);
    WebKitSecurityManager *security = webkit_web_context_get_security_manager(B.context);
    for (const char *scheme : { "chrome-extension", "moz-extension" }) {
        webkit_web_context_register_uri_scheme(B.context, scheme, serve, nullptr, nullptr);
        webkit_security_manager_register_uri_scheme_as_secure(security, scheme);
        webkit_security_manager_register_uri_scheme_as_cors_enabled(security, scheme);
    }
    mkdir(WISP_PROFILE "/filters", 0777);
    filter_store = webkit_user_content_filter_store_new(WISP_PROFILE "/filters");
    if (adblock_on)
        compile_adblock();
    load_all();
    for (auto &e : E)
        if (e->enabled)
            start(*e);
}

std::vector<Info> list()
{
    std::vector<Info> out;
    for (auto &e : E) {
        Info info;
        info.id = e->id;
        info.name = localized(*e, str(e->manifest, "name", e->id));
        info.version = str(e->manifest, "version");
        info.description = localized(*e, str(e->manifest, "description"));
        info.dir = e->dir;
        info.manifest_version = e->manifest.HasMember("manifest_version") && e->manifest["manifest_version"].IsInt()
            ? e->manifest["manifest_version"].GetInt() : 2;
        info.enabled = e->enabled;
        info.popup = e->popup;
        info.badge_text = e->badge_text;
        info.badge_color = e->badge_color;
        info.error = e->error;
        // the largest icon it has (icons, or the action's default_icon)
        int best = 0;
        auto consider = [&](const Value &icons) {
            if (icons.IsString()) {
                if (!best)
                    info.icon = e->dir + "/" + icons.GetString();
                return;
            }
            if (!icons.IsObject())
                return;
            for (auto &m : icons.GetObject()) {
                const int size = atoi(m.name.GetString());
                if (m.value.IsString() && size > best && size <= 128) {
                    best = size;
                    info.icon = e->dir + "/" + m.value.GetString();
                }
            }
        };
        if (e->manifest.HasMember("icons"))
            consider(e->manifest["icons"]);
        for (const char *key : { "action", "browser_action", "page_action" })
            if (e->manifest.HasMember(key) && e->manifest[key].IsObject() && e->manifest[key].HasMember("default_icon"))
                consider(e->manifest[key]["default_icon"]);
        if (e->manifest.HasMember("options_page"))
            info.options_page = str(e->manifest, "options_page");
        else if (e->manifest.HasMember("options_ui"))
            info.options_page = str(e->manifest["options_ui"], "page");
        out.push_back(info);
    }
    return out;
}

std::string install(const std::string &package_path)
{
    struct stat st;
    if (stat(package_path.c_str(), &st))
        return "the file is gone";
    const std::string temp = std::string(ROOT) + "/.installing";
    remove_tree(temp);
    std::string id;
    if (S_ISDIR(st.st_mode)) {
        return "install a packed extension (.crx, .nex, .xpi or .zip)";
    }
    const Unpacked unpacked = unpack_package(package_path, temp);
    if (!unpacked.ok) {
        remove_tree(temp);
        return unpacked.error;
    }
    Document manifest;
    const std::string text = read_text(temp + "/manifest.json");
    if (text.empty() || manifest.Parse<PARSE>(text.c_str()).HasParseError() || !manifest.IsObject()) {
        remove_tree(temp);
        return "no manifest.json in the package";
    }
    id = unpacked.crx_id;
    if (id.empty()) {
        // Firefox: its gecko id; otherwise the name and version
        std::string key = str(manifest, "name") + "@" + str(manifest, "version");
        for (const char *settings : { "browser_specific_settings", "applications" })
            if (manifest.HasMember(settings) && manifest[settings].IsObject() && manifest[settings].HasMember("gecko"))
                key = str(manifest[settings]["gecko"], "id", key);
        id = extension_id_from_bytes(std::vector<unsigned char>(key.begin(), key.end()));
    }
    const std::string dir = std::string(ROOT) + "/" + id;
    if (Extension *old = find(id)) {
        stop_background(*old);
        E.erase(std::remove_if(E.begin(), E.end(), [&](auto &e) { return e->id == id; }), E.end());
    }
    // keep its stored data across an update
    struct stat old_dir;
    if (!stat(dir.c_str(), &old_dir)) {
        for (const char *keep : { ".wisp-storage-local.json", ".wisp-storage-sync.json", ".wisp-dnr.json", ".wisp-scripts.json", ".wisp-state" })
            rename((dir + "/" + keep).c_str(), (temp + "/" + keep).c_str());
        remove_tree(dir);
    }
    if (rename(temp.c_str(), dir.c_str())) {
        remove_tree(temp);
        return "could not move it into place";
    }
    auto e = load_extension(dir, id);
    if (!e)
        return "its manifest could not be read";
    const std::string name = localized(*e, str(e->manifest, "name", id));
    wisp_log("ext: installed %s \"%s\" %s from %s (%zu files, %zu KiB)", id.c_str(), name.c_str(), str(e->manifest, "version").c_str(),
        package_path.c_str(), unpacked.files, unpacked.bytes >> 10);
    Extension &added = *e;
    E.push_back(std::move(e));
    save_state(added);
    start(added);
    refresh_tabs();
    to_pages(added, dispatch_call("runtime.onInstalled", "[{\"reason\":\"install\"}]"));
    return {};
}

void set_enabled(const std::string &id, bool enabled)
{
    Extension *e = find(id);
    if (!e || e->enabled == enabled)
        return;
    e->enabled = enabled;
    save_state(*e);
    if (enabled) {
        start(*e);
    } else {
        stop_background(*e);
        remove_filter_everywhere("ext-" + e->id);
    }
    refresh_tabs();
}

void uninstall(const std::string &id)
{
    Extension *e = find(id);
    if (!e)
        return;
    stop_background(*e);
    remove_filter_everywhere("ext-" + id);
    const std::string dir = e->dir;
    E.erase(std::remove_if(E.begin(), E.end(), [&](auto &x) { return x->id == id; }), E.end());
    remove_tree(dir);
    refresh_tabs();
    wisp_log("ext: removed %s", id.c_str());
}

void attach_tab(int tab, WebKitUserContentManager *ucm)
{
    tab_ucms[tab] = ucm;
    g_object_set_data(G_OBJECT(ucm), "wisp-ext-context", GINT_TO_POINTER(tab + 1000));
    attach_extensions(tab, ucm);
    for (auto &[id, filter] : filters)
        if (id.rfind("adblock-", 0) != 0 || adblock_on)
            webkit_user_content_manager_add_filter(ucm, filter);
}

void detach_tab(int tab)
{
    tab_ucms.erase(tab);
}

void tab_created(int tab)
{
    broadcast("tabs.onCreated", "[" + tab_json(tab) + "]");
}

void tab_updated(int tab, const std::string &url, const std::string &title, bool loading)
{
    std::string change = "{\"status\":\"" + std::string(loading ? "loading" : "complete") + "\"";
    if (!url.empty())
        change += ",\"url\":" + json_string(url);
    if (!title.empty())
        change += ",\"title\":" + json_string(title);
    change += "}";
    broadcast("tabs.onUpdated", "[" + std::to_string(tab) + "," + change + "," + tab_json(tab) + "]");
    if (!loading && !url.empty())
        broadcast("webNavigation.onCompleted", "[{\"tabId\":" + std::to_string(tab) + ",\"url\":" + json_string(url) + ",\"frameId\":0}]");
}

void tab_activated(int tab)
{
    broadcast("tabs.onActivated", "[{\"tabId\":" + std::to_string(tab) + ",\"windowId\":1}]");
}

void tab_removed(int tab)
{
    broadcast("tabs.onRemoved", "[" + std::to_string(tab) + ",{\"windowId\":1,\"isWindowClosing\":false}]");
    detach_tab(tab);
}

WebKitWebView *action_clicked(const std::string &id, int tab)
{
    Extension *e = find(id);
    if (!e)
        return nullptr;
    if (e->popup.empty()) {
        if (!e->background)
            start_background(*e);
        to_pages(*e, dispatch_call("action.onClicked", "[" + tab_json(tab) + "]"));
        return nullptr;
    }
    WebKitUserContentManager *ucm = extension_page_ucm(*e, CONTEXT_POPUP);
    WebKitWebView *view = B.make_view ? B.make_view(ucm) : nullptr;
    g_object_unref(ucm);
    if (view) {
        const std::string uri = "chrome-extension://" + e->id + "/" + e->popup;
        webkit_web_view_load_uri(view, uri.c_str());
    }
    return view;
}

void set_adblock(bool on)
{
    if (on == adblock_on)
        return;
    adblock_on = on;
    wisp_setting_set("adblock", on ? "1" : "0");
    if (on)
        compile_adblock();
    for (auto &[id, filter] : filters) {
        if (id.rfind("adblock-", 0) != 0)
            continue;
        if (on)
            add_filter_everywhere(filter);
        else
            remove_filter_everywhere(id);
    }
}

bool adblock() { return adblock_on; }

} // namespace wisp::ext
