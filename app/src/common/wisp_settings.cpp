// Wisp: /data/Wisp/settings.txt, "key=value" lines (common/wisp.h). Read once per process; a set
// rewrites the file. The UI changes settings; the children only read them at start.
#include "wisp.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <sys/stat.h>

namespace {

std::mutex lock;
std::map<std::string, std::string> values;
bool loaded = false;

void load()
{
    if (loaded)
        return;
    loaded = true;
    FILE *f = fopen(WISP_SETTINGS, "r");
    if (!f)
        return;
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        size_t len = strlen(line);
        while (len && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = 0;
        if (!len || line[0] == '#')
            continue;
        char *eq = strchr(line, '=');
        if (!eq)
            continue;
        *eq = 0;
        values[line] = eq + 1;
    }
    fclose(f);
}

} // namespace

const char *wisp_setting(const char *key, const char *fallback)
{
    std::lock_guard<std::mutex> guard(lock);
    load();
    auto it = values.find(key);
    return it == values.end() ? fallback : it->second.c_str();
}

int wisp_setting_int(const char *key, int fallback)
{
    const char *value = wisp_setting(key, nullptr);
    return value && *value ? atoi(value) : fallback;
}

WispRendering wisp_rendering()
{
    const char *value = wisp_setting("rendering", "hybrid");
    if (!strcmp(value, "cpu"))
        return WispRendering::Cpu;
    if (!strcmp(value, "gpu"))
        return WispRendering::Gpu;
    return WispRendering::Hybrid;
}

void wisp_set_rendering(WispRendering mode)
{
    wisp_setting_set("rendering", mode == WispRendering::Cpu ? "cpu" : mode == WispRendering::Gpu ? "gpu" : "hybrid");
}

const char *wisp_rendering_name(WispRendering mode)
{
    return mode == WispRendering::Cpu ? "CPU" : mode == WispRendering::Gpu ? "GPU" : "CPU + GPU";
}

namespace {

// (with the lock held)
void save()
{
    const std::string temp = std::string(WISP_SETTINGS) + ".new";
    FILE *f = fopen(temp.c_str(), "w");
    if (!f)
        return;
    fputs("# OrbisWeb settings (key=value); the browser rewrites this file\n", f);
    for (auto &[k, v] : values)
        fprintf(f, "%s=%s\n", k.c_str(), v.c_str());
    fclose(f);
    rename(temp.c_str(), WISP_SETTINGS);
}

} // namespace

void wisp_setting_set(const char *key, const char *value)
{
    std::lock_guard<std::mutex> guard(lock);
    load();
    values[key] = value;
    save();
}

// The key out of the file: its default applies again
void wisp_setting_remove(const char *key)
{
    std::lock_guard<std::mutex> guard(lock);
    load();
    if (values.erase(key))
        save();
}

// --- Where downloads go ------------------------------------------------------------------------------

namespace {

bool is_folder(const std::string &path)
{
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

// The drive the folder is on is there: /data always, a USB drive while it is plugged in (its
// /mnt/usbN). (Nothing is made where no drive is: a mkdir of /mnt/usb0 with the drive out would make
// a folder on the console's own file system.)
bool drive_there(const std::string &path)
{
    if (path.rfind("/mnt/usb", 0) == 0)
        return is_folder(path.substr(0, path.find('/', 8)));
    return path == "/data" || path.rfind("/data/", 0) == 0;
}

bool make_folders(const std::string &path)
{
    if (path.empty() || path == "/")
        return false;
    if (is_folder(path))
        return true;
    const size_t slash = path.find_last_of('/');
    if (slash != std::string::npos && slash > 0 && !make_folders(path.substr(0, slash)))
        return false;
    return mkdir(path.c_str(), 0777) == 0 || errno == EEXIST;
}

} // namespace

std::string wisp_download_folder_chosen()
{
    std::string chosen = wisp_setting("download_folder", "");
    while (chosen.size() > 1 && chosen.back() == '/')
        chosen.pop_back();
    return chosen == WISP_DOWNLOADS ? std::string() : chosen;
}

std::string wisp_download_folder(bool *missing)
{
    if (missing)
        *missing = false;
    const std::string chosen = wisp_download_folder_chosen();
    if (!chosen.empty()) {
        if (is_folder(chosen) || (drive_there(chosen) && make_folders(chosen)))
            return chosen;
        if (missing)
            *missing = true;
    }
    mkdir(WISP_DATA, 0777);
    mkdir(WISP_DOWNLOADS, 0777);
    return WISP_DOWNLOADS;
}

std::string wisp_download_fallback_note()
{
    bool missing = false;
    wisp_download_folder(&missing);
    return missing ? " (in Downloads: " + wisp_download_folder_chosen() + " isn't there)" : std::string();
}
