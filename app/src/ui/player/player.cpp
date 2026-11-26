// Wisp Player (ui/player/player.h): FFmpeg on a decoding thread, the PS4's audio out on another, the
// pictures painted into the browser's screen picture by the UI thread.
#include "player.h"

#include "../../common/wisp.h"
#include "../shell/ps4_canvas.h"

#if defined(WISP_HAVE_FFMPEG)
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include <orbis/AudioOut.h>
#include <orbis/UserService.h>
#include <ps4_kit.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <deque>
#include <dirent.h>
#include <glib.h>
#include <iconv.h>
#include <map>
#include <mutex>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>

#include <jpeglib.h>
#if defined(WISP_HAVE_LIBARCHIVE)
#include <archive.h>
#include <archive_entry.h>
#endif

namespace wisp::player {
namespace {

using ps4ui::Canvas;
using ps4ui::Color;
using ps4ui::rgb;
using ps4ui::Weight;

constexpr int RATE = 48000;          // the audio out's
constexpr int BLOCK = 1024;          // frames per sceAudioOutOutput (21.3 ms)
constexpr int MAX_FRAMES = 3;        // pictures made ahead
constexpr double AUDIO_AHEAD = 0.6;  // seconds of sound decoded ahead
constexpr const char *RESUME_FILE = WISP_DATA "/player/resume.tsv";
constexpr const char *RECENT_FILE = WISP_DATA "/player/recent.txt";

Host H;

// --- How the picture is shown (MPC-HC's Video Frame and Pan & Scan) --------------------------------------
struct Aspect {
    const char *name;
    double ratio;   // 0: the video's own
};
const Aspect ASPECTS[] = { { "The video's own", 0 }, { "4:3", 4.0 / 3 }, { "5:4", 1.25 }, { "16:9", 16.0 / 9 },
    { "1.85:1 (cinema)", 1.85 }, { "2.35:1 (CinemaScope)", 2.35 }, { "2.39:1", 2.39 }, { "21:9", 21.0 / 9 }, { "1:1", 1.0 } };
constexpr int ASPECT_COUNT = sizeof ASPECTS / sizeof ASPECTS[0];
const char *const FRAME_MODES[] = { "Fit inside (letterbox)", "Fill the screen (crop the edges)", "Stretch to the screen", "Original size" };
constexpr int FRAME_MODE_COUNT = 4;
const double SPEEDS[] = { 0.25, 0.5, 0.75, 1.0, 1.25, 1.5, 1.75, 2.0, 3.0, 4.0 };
constexpr int SPEED_COUNT = sizeof SPEEDS / sizeof SPEEDS[0];

struct View {
    int aspect = 0;          // ASPECTS
    int frame = 0;           // FRAME_MODES
    double zoom = 1.0;       // on top of the frame
    int brightness = 0, contrast = 0, saturation = 0;   // -100 ... 100
    bool operator==(const View &o) const
    {
        return aspect == o.aspect && frame == o.frame && zoom == o.zoom && brightness == o.brightness && contrast == o.contrast
            && saturation == o.saturation;
    }
};

// Subtitles' text encodings for files that are not UTF-8 (iconv names: the console's C library has them)
struct Encoding {
    const char *name;
    const char *iconv;   // nullptr: decide (auto)
};
const Encoding ENCODINGS[] = { { "Automatic (UTF-8, else Arabic or Western)", nullptr }, { "UTF-8", "UTF-8" },
    { "Arabic (Windows-1256)", "WINDOWS-1256" }, { "Arabic (ISO-8859-6)", "ISO-8859-6" }, { "Western (Windows-1252)", "WINDOWS-1252" },
    { "Central European (Windows-1250)", "WINDOWS-1250" }, { "Cyrillic (Windows-1251)", "WINDOWS-1251" },
    { "Greek (Windows-1253)", "WINDOWS-1253" }, { "Turkish (Windows-1254)", "WINDOWS-1254" }, { "Hebrew (Windows-1255)", "WINDOWS-1255" },
    { "Baltic (Windows-1257)", "WINDOWS-1257" }, { "Vietnamese (Windows-1258)", "WINDOWS-1258" }, { "Persian / Urdu (Windows-1256)", "WINDOWS-1256" },
    { "Chinese simplified (GB18030)", "GB18030" }, { "Chinese traditional (Big5)", "BIG5" }, { "Japanese (Shift_JIS)", "SHIFT_JIS" },
    { "Korean (EUC-KR)", "EUC-KR" } };
constexpr int ENCODING_COUNT = sizeof ENCODINGS / sizeof ENCODINGS[0];

// Where subtitles are looked for online (MPC-HC's subtitle database servers): the site opens in a tab of
// its own, searched for the video's name; what it downloads goes to Downloads, Load subtitle file takes it
struct Server {
    const char *name;
    const char *url;   // the search, %s the name
};
const Server SERVERS[] = { { "OpenSubtitles.org", "https://www.opensubtitles.org/en/search2/sublanguageid-all/moviename-%s" },
    { "Podnapisi", "https://www.podnapisi.net/subtitles/search/?keywords=%s" }, { "SubDL", "https://subdl.com/search/%s" },
    { "Addic7ed (TV shows)", "https://www.addic7ed.com/search.php?search=%s" },
    { "Search the web", "https://www.google.com/search?q=%s+subtitles" } };
constexpr int SERVER_COUNT = sizeof SERVERS / sizeof SERVERS[0];

struct Picture {
    std::vector<uint32_t> pixels;   // BGRA, w x h
    int w = 0, h = 0;
    double pts = 0;
};

struct Subtitle {
    double start = 0, end = 0;
    std::string text;               // a text subtitle (ASS tags taken out)
    std::vector<uint32_t> bitmap;   // a picture subtitle: premultiplied BGRA, at the video's own size
    int x = 0, y = 0, w = 0, h = 0, frame_w = 0, frame_h = 0;
};

struct Chunk {
    std::vector<int16_t> samples;   // stereo, interleaved
    size_t used = 0;                // frames played from it
    double pts = 0;                 // of its first frame
    double step = 1.0 / RATE;       // the file's seconds a frame of it plays (the speed / 48000)
};

struct Chapter {
    double start = 0;
    std::string title;
};

// --- What the threads share -----------------------------------------------------------------------------
struct Shared {
    std::mutex lock;
    std::condition_variable wake;
    // asked of the decoding thread
    bool quit = false;
    double seek_to = -1;
    int want_audio = -1, want_sub = -2;    // a track to switch to (index into the lists; sub -1: none)
    int area_w = 1920, area_h = 972;       // what pictures are made for
    // made by it
    std::deque<Picture> pictures;
    std::vector<Picture> spare;
    std::deque<Chunk> sound;
    double sound_seconds = 0;              // queued
    std::vector<Subtitle> subtitles;       // the current track's, as they come (an external file's at once)
    bool at_end = false;                   // nothing more to read
    std::string error;
    // the file
    double duration = 0;
    bool has_video = false, has_audio = false;
    int video_w = 0, video_h = 0;
    std::string video_codec, audio_codec;
    std::vector<std::string> audio_names, sub_names;   // tracks; the subtitles' last may be the file beside it
    int audio_now = -1, sub_now = -1;
    // the clock: the sound's when there is sound, else the wall's
    double clock_pts = 0, clock_at = 0;
    bool paused = false;
    int volume = 100;
    bool muted = false;
    int boost = 100;                       // the sound made louder, in percent (clipped)
    double speed = 1.0;                    // the rate (the sound's pitch moves with it)
    double audio_delay = 0;                // seconds the sound comes later than the picture
    View view;                             // how pictures are made (the decoding thread reads it)
    // given to the decoding thread: a subtitles file's lines for the "File:" track (Load subtitle file)
    bool external_ready = false;
    std::vector<Subtitle> external;
    std::string external_name;
    int sub_encoding = 0;                  // how the file beside the video is read (ENCODINGS)
    std::string sub_file_beside;           // that file, when the decoding thread found one
    // what the file is (the information panel)
    std::vector<Chapter> chapters;
    std::string container, pixel_format;
    double fps = 0;
    int64_t bitrate = 0;
    int audio_rate = 0, audio_channels = 0;
    uint64_t shown_frames = 0, dropped_frames = 0;
};
Shared X;

pthread_t decode_thread, audio_thread;
bool threads_up = false;

double now_s() { return wisp_seconds(); }

// the clock now (any thread; X.lock held)
double clock_locked()
{
    if (X.paused)
        return X.clock_pts;
    return X.clock_pts + std::min(now_s() - X.clock_at, X.has_audio ? 0.25 : 1e9) * X.speed;
}

// --- The page's state (UI thread) -----------------------------------------------------------------------
struct Page {
    bool visible = false;
    std::string path, name;
    Picture shown;                  // the picture on the screen
    bool shown_new = false;
    double osd_until = 0;
    double last_click = 0;
    int x = 0, y = 0, w = 1920, h = 972;
    bool dirty = true;
    std::string message;            // a word over the picture ("Paused", "Volume 80%"), until message_until
    double message_until = 0;
    double last_saved = 0;
    bool closing = false;
    int seek_x = 0, seek_w = 0;     // the bar's seek line as last drawn (in the area)
    bool info = false;              // the information panel (MPC-HC's Statistics and Properties)
    // subtitles: how they look and when (settings: player_sub_size, player_sub_lift, player_sub_box)
    int sub_size = 100;             // percent
    int sub_lift = 0;               // pixels above where they would be
    bool sub_box = false;           // a dark box behind them
    double sub_delay = 0;           // seconds they come later
    int encoding = 0;               // ENCODINGS: how a subtitles file that is not UTF-8 is read
    std::string sub_file;           // the subtitles file loaded (to read it again in another encoding)
    std::string sub_inside;         // an entry of an archive: sub_file is the archive
    // at the end: 0 the folder's next file, 1 this one again, 2 stop there, 3 close (settings: player_after)
    int after = 0;
    bool ctrl = false, shift = false;   // the USB keyboard's modifiers
} P;

// --- The player's menu (MPC-HC's right-click menu) --------------------------------------------------------
struct Item {
    enum Kind { Action, Check, Radio, Sub, Separator };
    std::string label, hint;        // hint: its key, or a value, at the right
    Kind kind = Action;
    bool on = false, enabled = true;
    bool keep_open = false;         // a step (+ / -) that is taken again and again: the menu stays
    std::function<void()> act;
    std::function<std::vector<Item>()> sub;
};
struct Level {
    std::vector<Item> items;
    int index = 0, scroll = 0;
    int x = 0, y = 0, w = 0, h = 0;   // where it was drawn (in the area)
    int rows = 0;                     // the rows shown (a long list scrolls)
};
struct Menu {
    bool open = false;
    std::vector<Level> levels;
    float at_x = 0, at_y = 0;         // where the root opened (in the area)
} M;

// --- A file to open (Load subtitle file, Open file): the folders, archives as folders ---------------------
struct PickEntry {
    std::string name, path;           // path: a folder or file; inside an archive, the archive's
    std::string inside;               // an archive's entry
    bool dir = false, archive = false, up = false, place = false;
    int64_t size = 0;
};
struct Picker {
    bool open = false;
    bool subtitles = false;           // what it takes: subtitles (and archives of them), else video and music
    std::string title;
    std::string folder;               // "" the places; an archive's path while its list shows
    bool in_archive = false;
    std::vector<PickEntry> entries;
    int index = 0, scroll = 0, rows = 0;
    int x = 0, y = 0, w = 0, h = 0, list_y = 0;
} K;

void say(const std::string &text)
{
    P.message = text;
    P.message_until = now_s() + 1.6;
    P.dirty = true;
}

std::string clock_text(double seconds)
{
    if (!(seconds >= 0))
        seconds = 0;
    const long s = static_cast<long>(seconds);
    char out[24];
    if (s >= 3600)
        snprintf(out, sizeof out, "%ld:%02ld:%02ld", s / 3600, (s / 60) % 60, s % 60);
    else
        snprintf(out, sizeof out, "%ld:%02ld", s / 60, s % 60);
    return out;
}

std::string lower(std::string s)
{
    for (char &c : s)
        c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string extension(const std::string &path)
{
    const size_t dot = path.find_last_of('.');
    const size_t slash = path.find_last_of('/');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash))
        return "";
    return lower(path.substr(dot + 1));
}

// --- Resume positions -----------------------------------------------------------------------------------
std::map<std::string, double> resume_read()
{
    std::map<std::string, double> out;
    if (FILE *f = fopen(RESUME_FILE, "r")) {
        char line[4096];
        while (fgets(line, sizeof line, f)) {
            line[strcspn(line, "\n")] = 0;
            char *tab = strrchr(line, '\t');
            if (!tab)
                continue;
            *tab = 0;
            out[line] = atof(tab + 1);
        }
        fclose(f);
    }
    return out;
}

void resume_write(const std::string &path, double seconds)
{
    std::map<std::string, double> all = resume_read();
    if (seconds > 0)
        all[path] = seconds;
    else
        all.erase(path);
    mkdir(WISP_DATA, 0777);
    mkdir(WISP_DATA "/player", 0777);
    if (FILE *f = fopen(RESUME_FILE, "w")) {
        int n = 0;
        for (auto &[p, s] : all)
            if (n++ < 500)
                fprintf(f, "%s\t%.1f\n", p.c_str(), s);
        fclose(f);
    }
}

// --- Subtitles ------------------------------------------------------------------------------------------
// An ASS event's text: after its 8 fields, override tags ({...}) out, \N a new line
std::string ass_text(const char *ass)
{
    std::string line = ass ? ass : "";
    int commas = 0;
    size_t start = 0;
    for (size_t i = 0; i < line.size() && commas < 8; i++)
        if (line[i] == ',' && ++commas == 8)
            start = i + 1;
    if (commas == 8)
        line = line.substr(start);
    std::string out;
    bool tag = false;
    for (size_t i = 0; i < line.size(); i++) {
        const char c = line[i];
        if (c == '{') {
            tag = true;
            continue;
        }
        if (c == '}') {
            tag = false;
            continue;
        }
        if (tag)
            continue;
        if (c == '\\' && i + 1 < line.size() && (line[i + 1] == 'N' || line[i + 1] == 'n')) {
            out += '\n';
            i++;
            continue;
        }
        if (c == '\\' && i + 1 < line.size() && line[i + 1] == 'h') {
            out += ' ';
            i++;
            continue;
        }
        out += c;
    }
    return out;
}

void take_subtitle(const AVSubtitle &sub, double pts, int frame_w, int frame_h, std::vector<Subtitle> &into)
{
    const double start = pts + sub.start_display_time / 1000.0;
    double end = pts + (sub.end_display_time ? sub.end_display_time / 1000.0 : 5.0);
    for (unsigned i = 0; i < sub.num_rects; i++) {
        const AVSubtitleRect *rect = sub.rects[i];
        Subtitle s;
        s.start = start;
        s.end = end;
        if (rect->type == SUBTITLE_ASS || rect->type == SUBTITLE_TEXT) {
            s.text = rect->type == SUBTITLE_ASS ? ass_text(rect->ass) : (rect->text ? rect->text : "");
            if (s.text.empty())
                continue;
        } else if (rect->type == SUBTITLE_BITMAP && rect->w > 0 && rect->h > 0 && rect->data[0] && rect->data[1]) {
            s.x = rect->x, s.y = rect->y, s.w = rect->w, s.h = rect->h;
            s.frame_w = frame_w, s.frame_h = frame_h;
            s.bitmap.resize(static_cast<size_t>(rect->w) * rect->h);
            const uint32_t *palette = reinterpret_cast<const uint32_t *>(rect->data[1]);   // ARGB
            for (int yy = 0; yy < rect->h; yy++) {
                for (int xx = 0; xx < rect->w; xx++) {
                    const uint32_t argb = palette[rect->data[0][yy * rect->linesize[0] + xx]];
                    const uint32_t a = argb >> 24;
                    const uint32_t r = ((argb >> 16) & 0xff) * a / 255, g = ((argb >> 8) & 0xff) * a / 255, b = (argb & 0xff) * a / 255;
                    s.bitmap[static_cast<size_t>(yy) * rect->w + xx] = a << 24 | r << 16 | g << 8 | b;
                }
            }
        } else {
            continue;
        }
        into.push_back(std::move(s));
    }
}

// --- Subtitles files: any text encoding, from an archive too ------------------------------------------------

bool valid_utf8(const std::string &s)
{
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        const int extra = c < 0x80 ? 0 : (c >> 5) == 6 ? 1 : (c >> 4) == 14 ? 2 : (c >> 3) == 30 ? 3 : -1;
        if (extra < 0 || i + extra >= s.size() + (extra ? 0 : 1))
            return false;
        for (int k = 1; k <= extra; k++)
            if ((static_cast<unsigned char>(s[i + k]) >> 6) != 2)
                return false;
        i += static_cast<size_t>(extra) + 1;
    }
    return true;
}

// from `from` to UTF-8 (iconv, the console's C library's); a byte it cannot read becomes U+FFFD
bool iconv_to_utf8(const std::string &in, const char *from, std::string &out)
{
    iconv_t cd = iconv_open("UTF-8", from);
    if (cd == reinterpret_cast<iconv_t>(-1))
        return false;
    out.clear();
    out.reserve(in.size() * 2);
    char *src = const_cast<char *>(in.data());
    size_t left = in.size();
    char buffer[4096];
    while (left > 0) {
        char *dst = buffer;
        size_t room = sizeof buffer;
        const size_t result = iconv(cd, &src, &left, &dst, &room);
        out.append(buffer, static_cast<size_t>(dst - buffer));
        if (result == static_cast<size_t>(-1) && errno != E2BIG) {
            out += "\xEF\xBF\xBD";
            src++;
            left--;
        }
    }
    iconv_close(cd);
    return true;
}

// What Automatic reads text that is not UTF-8 as: Arabic (Windows-1256) when most of its letters are high
// bytes (an Arabic text's letters all are; a Western text has a few accented ones), else Western
const char *guess_encoding(const std::string &bytes)
{
    size_t high = 0, letters = 0;
    for (const char ch : bytes) {
        const unsigned char c = static_cast<unsigned char>(ch);
        high += c >= 0x80;
        letters += c >= 0x80 || isalpha(c);
    }
    return letters && high * 100 / letters >= 30 ? "WINDOWS-1256" : "WINDOWS-1252";
}

// A subtitles file's text as UTF-8 (encoding: ENCODINGS). UTF-16 by its byte-order mark; Automatic takes
// UTF-8 as it is. used: what it was read as.
std::string subtitles_utf8(const std::string &bytes, int encoding, std::string &used)
{
    const auto b = [&](size_t i) { return i < bytes.size() ? static_cast<unsigned char>(bytes[i]) : 0u; };
    std::string out;
    if (b(0) == 0xFF && b(1) == 0xFE && iconv_to_utf8(bytes.substr(2), "UTF-16LE", out)) {
        used = "UTF-16";
        return out;
    }
    if (b(0) == 0xFE && b(1) == 0xFF && iconv_to_utf8(bytes.substr(2), "UTF-16BE", out)) {
        used = "UTF-16";
        return out;
    }
    const std::string text = b(0) == 0xEF && b(1) == 0xBB && b(2) == 0xBF ? bytes.substr(3) : bytes;
    const char *from = ENCODINGS[std::clamp(encoding, 0, ENCODING_COUNT - 1)].iconv;
    if (!from) {
        if (valid_utf8(text)) {
            used = "UTF-8";
            return text;
        }
        from = guess_encoding(text);
    }
    if (!strcmp(from, "UTF-8")) {
        used = "UTF-8";
        return text;
    }
    if (iconv_to_utf8(text, from, out)) {
        used = from;
        return out;
    }
    used = std::string("as it is (no ") + from + " here)";
    return text;
}

// The subtitles files FFmpeg (this build's) reads: SubRip, ASS / SSA, WebVTT
bool is_subtitle_name(const std::string &name)
{
    const std::string ext = extension(name);
    return ext == "srt" || ext == "ass" || ext == "ssa" || ext == "vtt";
}

bool read_whole(const std::string &path, std::string &out, size_t limit = static_cast<size_t>(16) << 20)
{
    FILE *f = fopen(path.c_str(), "rb");
    if (!f)
        return false;
    out.clear();
    char buffer[65536];
    size_t got;
    while ((got = fread(buffer, 1, sizeof buffer, f)) > 0 && out.size() < limit)
        out.append(buffer, got);
    fclose(f);
    return true;
}

#if defined(WISP_HAVE_LIBARCHIVE)
// An archive's subtitles files (zip, rar, 7z: what subtitle sites give), or one of them whole
bool archive_walk(const std::string &file, const std::string &want, std::vector<std::string> *names, std::string *bytes)
{
    struct archive *a = archive_read_new();
    archive_read_support_filter_all(a);
    archive_read_support_format_all(a);
    archive_read_set_options(a, "zip:hdrcharset=CP437,rar:hdrcharset=CP437");
    if (archive_read_open_filename(a, file.c_str(), 65536) != ARCHIVE_OK) {
        archive_read_free(a);
        return false;
    }
    bool found = false;
    struct archive_entry *entry;
    while (archive_read_next_header(a, &entry) == ARCHIVE_OK) {
        const char *pathname = archive_entry_pathname_utf8(entry);
        if (!pathname)
            pathname = archive_entry_pathname(entry);
        if (!pathname || archive_entry_filetype(entry) != AE_IFREG || !is_subtitle_name(pathname))
            continue;
        if (names)
            names->push_back(pathname);
        if (bytes && want == pathname) {
            bytes->clear();
            char buffer[65536];
            la_ssize_t got;
            while ((got = archive_read_data(a, buffer, sizeof buffer)) > 0 && bytes->size() < (static_cast<size_t>(16) << 20))
                bytes->append(buffer, static_cast<size_t>(got));
            found = true;
            break;
        }
    }
    archive_read_free(a);
    return names ? !names->empty() : found;
}
#endif

// Subtitles text (UTF-8, as a .srt / .ass / .vtt has it) into lines, through FFmpeg, which reads it from a
// file of the app's own
bool parse_subtitles(const std::string &utf8, std::string ext, std::vector<Subtitle> &into)
{
    static std::atomic<int> n{0};
    if (ext == "ssa")
        ext = "ass";
    mkdir(WISP_DATA "/tmp", 0777);
    const std::string path = std::string(WISP_DATA "/tmp/player-subtitles-") + std::to_string(n++ % 8) + "." + ext;
    if (FILE *f = fopen(path.c_str(), "wb")) {
        fwrite(utf8.data(), 1, utf8.size(), f);
        fclose(f);
    } else {
        return false;
    }
    {
        AVFormatContext *fmt = nullptr;
        if (avformat_open_input(&fmt, path.c_str(), nullptr, nullptr) < 0) {
            unlink(path.c_str());
            return false;
        }
        avformat_find_stream_info(fmt, nullptr);
        const int stream = av_find_best_stream(fmt, AVMEDIA_TYPE_SUBTITLE, -1, -1, nullptr, 0);
        if (stream >= 0) {
            const AVCodec *codec = avcodec_find_decoder(fmt->streams[stream]->codecpar->codec_id);
            AVCodecContext *ctx = codec ? avcodec_alloc_context3(codec) : nullptr;
            if (ctx && avcodec_parameters_to_context(ctx, fmt->streams[stream]->codecpar) >= 0 && avcodec_open2(ctx, codec, nullptr) >= 0) {
                AVPacket *packet = av_packet_alloc();
                const AVRational tb = fmt->streams[stream]->time_base;
                while (av_read_frame(fmt, packet) >= 0) {
                    if (packet->stream_index == stream) {
                        AVSubtitle sub;
                        int got = 0;
                        if (avcodec_decode_subtitle2(ctx, &sub, &got, packet) >= 0 && got) {
                            const double pts = packet->pts != AV_NOPTS_VALUE ? packet->pts * av_q2d(tb) : 0;
                            if (!sub.end_display_time && packet->duration > 0)
                                sub.end_display_time = static_cast<uint32_t>(packet->duration * av_q2d(tb) * 1000);
                            take_subtitle(sub, pts, 0, 0, into);
                            avsubtitle_free(&sub);
                        }
                    }
                    av_packet_unref(packet);
                }
                av_packet_free(&packet);
            }
            if (ctx)
                avcodec_free_context(&ctx);
        }
        avformat_close_input(&fmt);
    }
    unlink(path.c_str());
    std::sort(into.begin(), into.end(), [](const Subtitle &a, const Subtitle &b) { return a.start < b.start; });
    return !into.empty();
}

// A subtitles file, or an entry of an archive (inside), in an encoding (ENCODINGS): its lines
bool load_subtitles(const std::string &file, const std::string &inside, int encoding, std::vector<Subtitle> &into, std::string &used)
{
    std::string bytes;
    if (!inside.empty()) {
#if defined(WISP_HAVE_LIBARCHIVE)
        if (!archive_walk(file, inside, nullptr, &bytes))
            return false;
#else
        return false;
#endif
    } else if (!read_whole(file, bytes)) {
        return false;
    }
    const std::string &name = inside.empty() ? file : inside;
    if (!is_subtitle_name(name))
        return false;
    return parse_subtitles(subtitles_utf8(bytes, encoding, used), extension(name), into);
}

// A subtitles file beside the video with its name (.srt, .ass, .ssa, .vtt): all of it, at once
bool read_external_subtitles(const std::string &video, int encoding, std::vector<Subtitle> &into, std::string &which, std::string &file)
{
    const size_t dot = video.find_last_of('.');
    const std::string stem = dot == std::string::npos ? video : video.substr(0, dot);
    for (const char *ext : { ".srt", ".ass", ".ssa", ".vtt", ".SRT", ".ASS", ".VTT" }) {
        const std::string path = stem + ext;
        struct stat st;
        std::string used;
        if (stat(path.c_str(), &st) != 0 || !load_subtitles(path, "", encoding, into, used))
            continue;
        which = path.substr(path.find_last_of('/') + 1);
        file = path;
        return true;
    }
    return false;
}

// --- The decoding thread ----------------------------------------------------------------------------------
struct Decoder {
    AVFormatContext *fmt = nullptr;
    int video = -1, audio = -1, sub = -1;
    AVCodecContext *vctx = nullptr, *actx = nullptr, *sctx = nullptr;
    std::vector<int> audio_streams, sub_streams;
    SwsContext *sws = nullptr;
    SwsContext *sws_colored = nullptr;   // the context the color controls were set on, and for which view
    View sws_view;
    SwrContext *swr = nullptr;
    int swr_rate = 0, swr_channels = 0, swr_out_rate = 0;
    AVSampleFormat swr_format = AV_SAMPLE_FMT_NONE;
    AVPacket *packet = nullptr;
    AVFrame *frame = nullptr;
    std::vector<Subtitle> external;
    bool external_track = false;         // the subtitles' last track is a file's
    bool external_on = false;
    bool eof = false;
    int late = 0;   // pictures that came too late in a row (then reference-less ones are skipped)
};

AVCodecContext *open_decoder(AVStream *stream, int threads)
{
    const AVCodec *codec = avcodec_find_decoder(stream->codecpar->codec_id);
    if (!codec)
        return nullptr;
    AVCodecContext *ctx = avcodec_alloc_context3(codec);
    if (!ctx || avcodec_parameters_to_context(ctx, stream->codecpar) < 0) {
        avcodec_free_context(&ctx);
        return nullptr;
    }
    ctx->pkt_timebase = stream->time_base;
    if (threads > 1) {
        ctx->thread_count = threads;
        ctx->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
    }
    // (FFmpeg's decoding threads start here, with the default stack: 256 KiB in this app, short for
    // some decoders)
    ps4_kit_set_default_thread_stack(static_cast<size_t>(1) << 20);
    const int opened = avcodec_open2(ctx, codec, nullptr);
    ps4_kit_set_default_thread_stack(static_cast<size_t>(256) << 10);
    if (opened < 0) {
        avcodec_free_context(&ctx);
        return nullptr;
    }
    return ctx;
}

std::string stream_name(AVStream *stream, int n)
{
    std::string name = "Track " + std::to_string(n);
    if (const AVDictionaryEntry *lang = av_dict_get(stream->metadata, "language", nullptr, 0))
        name += " · " + std::string(lang->value);
    if (const AVDictionaryEntry *title = av_dict_get(stream->metadata, "title", nullptr, 0))
        name += " · " + std::string(title->value);
    if (const AVCodecDescriptor *desc = avcodec_descriptor_get(stream->codecpar->codec_id))
        name += " (" + std::string(desc->name) + ")";
    return name;
}

void switch_audio(Decoder &d, int index)
{
    if (index < 0 || index >= static_cast<int>(d.audio_streams.size()))
        return;
    if (d.actx)
        avcodec_free_context(&d.actx);
    d.audio = d.audio_streams[index];
    d.actx = open_decoder(d.fmt->streams[d.audio], 1);
    std::lock_guard<std::mutex> guard(X.lock);
    X.audio_now = index;
    X.audio_codec = d.actx ? d.actx->codec->name : "";
    X.audio_rate = d.actx ? d.actx->sample_rate : 0;
    X.audio_channels = d.actx ? d.actx->ch_layout.nb_channels : 0;
    X.sound.clear();
    X.sound_seconds = 0;
}

void switch_sub(Decoder &d, int index)
{
    if (d.sctx)
        avcodec_free_context(&d.sctx);
    d.sub = -1;
    d.external_on = false;
    std::lock_guard<std::mutex> guard(X.lock);
    X.subtitles.clear();
    X.sub_now = index;
    if (index >= 0 && index < static_cast<int>(d.sub_streams.size())) {
        d.sub = d.sub_streams[index];
        d.sctx = open_decoder(d.fmt->streams[d.sub], 1);
    } else if (index == static_cast<int>(d.sub_streams.size()) && !d.external.empty()) {
        d.external_on = true;
        X.subtitles = d.external;
    } else {
        X.sub_now = -1;
    }
}

void picture_out(Decoder &d, AVFrame *frame)
{
    int area_w, area_h;
    View view;
    {
        std::lock_guard<std::mutex> guard(X.lock);
        area_w = X.area_w;
        area_h = X.area_h;
        view = X.view;
    }
    // the shape it shows in: the one chosen, else the video's own (its pixels' shape counted)
    double aspect = static_cast<double>(frame->width) / std::max(1, frame->height);
    if (frame->sample_aspect_ratio.num > 0 && frame->sample_aspect_ratio.den > 0)
        aspect *= av_q2d(frame->sample_aspect_ratio);
    if (view.aspect > 0 && view.aspect < ASPECT_COUNT)
        aspect = ASPECTS[view.aspect].ratio;
    // its whole size on the screen (MPC-HC's video frame), then the zoom
    double dw = area_w, dh = area_w / aspect;
    switch (view.frame) {
    case 1:   // fill: no bars, the edges cut off
        if (dh < area_h) {
            dh = area_h;
            dw = area_h * aspect;
        }
        break;
    case 2:   // stretched to the area
        dw = area_w;
        dh = area_h;
        break;
    case 3:   // original size: its own height in pixels
        dh = frame->height;
        dw = dh * aspect;
        break;
    default:  // fit: bars where the shapes differ
        if (dh > area_h) {
            dh = area_h;
            dw = area_h * aspect;
        }
    }
    dw *= view.zoom;
    dh *= view.zoom;
    // what of it shows (the area's part of it), and the part of the frame that is: only that is scaled
    const int w = static_cast<int>(std::min<double>(area_w, dw)) & ~1, h = static_cast<int>(std::min<double>(area_h, dh)) & ~1;
    if (w < 2 || h < 2 || dw < 2 || dh < 2)
        return;
    const int crop_w = std::clamp(static_cast<int>(frame->width * (w / dw) + 0.5), 2, frame->width) & ~1;
    const int crop_h = std::clamp(static_cast<int>(frame->height * (h / dh) + 0.5), 2, frame->height) & ~1;
    if (crop_w < frame->width || crop_h < frame->height) {
        frame->crop_left = static_cast<size_t>(((frame->width - crop_w) / 2) & ~1);
        frame->crop_right = static_cast<size_t>(frame->width - crop_w) - frame->crop_left;
        frame->crop_top = static_cast<size_t>(((frame->height - crop_h) / 2) & ~1);
        frame->crop_bottom = static_cast<size_t>(frame->height - crop_h) - frame->crop_top;
        if (av_frame_apply_cropping(frame, AV_FRAME_CROP_UNALIGNED) < 0)
            return;
    }
    const bool same = w == frame->width && h == frame->height;
    d.sws = sws_getCachedContext(d.sws, frame->width, frame->height, static_cast<AVPixelFormat>(frame->format), w, h,
        AV_PIX_FMT_BGRA, same ? SWS_POINT : SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
    if (!d.sws)
        return;
    // brightness, contrast, saturation (MPC-HC's color controls): in the conversion from YUV
    if (d.sws != d.sws_colored || !(view == d.sws_view)) {
        int *inv_table = nullptr, *table = nullptr, src_range = 0, dst_range = 0, brightness = 0, contrast = 0, saturation = 0;
        if (sws_getColorspaceDetails(d.sws, &inv_table, &src_range, &table, &dst_range, &brightness, &contrast, &saturation) >= 0)
            sws_setColorspaceDetails(d.sws, inv_table, src_range, table, dst_range, (view.brightness * 65536 + 50) / 100,
                ((view.contrast + 100) * 65536 + 50) / 100, ((view.saturation + 100) * 65536 + 50) / 100);
        d.sws_colored = d.sws;
        d.sws_view = view;
    }
    Picture picture;
    {
        std::lock_guard<std::mutex> guard(X.lock);
        if (!X.spare.empty()) {
            picture = std::move(X.spare.back());
            X.spare.pop_back();
        }
    }
    picture.w = w;
    picture.h = h;
    picture.pixels.resize(static_cast<size_t>(w) * h);
    uint8_t *planes[4] = { reinterpret_cast<uint8_t *>(picture.pixels.data()), nullptr, nullptr, nullptr };
    int strides[4] = { w * 4, 0, 0, 0 };
    sws_scale(d.sws, frame->data, frame->linesize, 0, frame->height, planes, strides);
    const int64_t ts = frame->best_effort_timestamp != AV_NOPTS_VALUE ? frame->best_effort_timestamp : frame->pts;
    picture.pts = ts != AV_NOPTS_VALUE ? ts * av_q2d(d.fmt->streams[d.video]->time_base) : 0;
    std::lock_guard<std::mutex> guard(X.lock);
    // too late already (the decoding falls behind): not shown, and reference-less pictures skipped for a while
    if (picture.pts < clock_locked() - 0.3 && !X.paused) {
        if (++d.late > 8 && d.vctx)
            d.vctx->skip_frame = AVDISCARD_NONREF;
        X.spare.push_back(std::move(picture));
        X.dropped_frames++;
        return;
    }
    if (d.late > 0 && --d.late == 0 && d.vctx)
        d.vctx->skip_frame = AVDISCARD_DEFAULT;
    X.pictures.push_back(std::move(picture));
    X.shown_frames++;
}

void sound_out(Decoder &d, AVFrame *frame)
{
    // another speed: the sound made for a rate of 48 kHz / speed and played at 48 kHz (faster and higher,
    // or slower and lower, as a tape)
    double speed, delay;
    {
        std::lock_guard<std::mutex> guard(X.lock);
        speed = X.speed;
        delay = X.audio_delay;
    }
    const int out_rate = std::max(8000, static_cast<int>(RATE / speed + 0.5));
    const int channels = frame->ch_layout.nb_channels;
    if (!d.swr || d.swr_rate != frame->sample_rate || d.swr_channels != channels || d.swr_format != frame->format || d.swr_out_rate != out_rate) {
        swr_free(&d.swr);
        AVChannelLayout out = AV_CHANNEL_LAYOUT_STEREO;
        if (swr_alloc_set_opts2(&d.swr, &out, AV_SAMPLE_FMT_S16, out_rate, &frame->ch_layout, static_cast<AVSampleFormat>(frame->format),
                frame->sample_rate, 0, nullptr) < 0 || swr_init(d.swr) < 0) {
            swr_free(&d.swr);
            return;
        }
        d.swr_rate = frame->sample_rate;
        d.swr_channels = channels;
        d.swr_format = static_cast<AVSampleFormat>(frame->format);
        d.swr_out_rate = out_rate;
    }
    Chunk chunk;
    chunk.step = 1.0 / out_rate;
    const int room = static_cast<int>(av_rescale_rnd(swr_get_delay(d.swr, frame->sample_rate) + frame->nb_samples, out_rate, frame->sample_rate, AV_ROUND_UP));
    chunk.samples.resize(static_cast<size_t>(room) * 2);
    uint8_t *out[1] = { reinterpret_cast<uint8_t *>(chunk.samples.data()) };
    const int got = swr_convert(d.swr, out, room, const_cast<const uint8_t **>(frame->extended_data), frame->nb_samples);
    if (got <= 0)
        return;
    chunk.samples.resize(static_cast<size_t>(got) * 2);
    const int64_t ts = frame->best_effort_timestamp != AV_NOPTS_VALUE ? frame->best_effort_timestamp : frame->pts;
    // (the audio delay: the sound's times later, so the pictures, which follow it, come earlier)
    chunk.pts = ts != AV_NOPTS_VALUE ? ts * av_q2d(d.fmt->streams[d.audio]->time_base) + delay : -1;
    std::lock_guard<std::mutex> guard(X.lock);
    if (chunk.pts < 0)
        chunk.pts = X.sound.empty() ? clock_locked() : X.sound.back().pts + (X.sound.back().samples.size() / 2.0) * X.sound.back().step;
    X.sound_seconds += got / static_cast<double>(RATE);
    X.sound.push_back(std::move(chunk));
}

void drain(Decoder &d, AVCodecContext *ctx, bool video)
{
    while (avcodec_receive_frame(ctx, d.frame) == 0) {
        if (video)
            picture_out(d, d.frame);
        else
            sound_out(d, d.frame);
        av_frame_unref(d.frame);
    }
}

void *decode_main(void *arg)
{
    std::string path = static_cast<std::string *>(arg)->c_str();
    delete static_cast<std::string *>(arg);
    Decoder d;
    auto fail = [&](const std::string &why) {
        std::lock_guard<std::mutex> guard(X.lock);
        X.error = why;
        X.at_end = true;
    };
    if (avformat_open_input(&d.fmt, path.c_str(), nullptr, nullptr) < 0) {
        fail("the file could not be opened");
        return nullptr;
    }
    if (avformat_find_stream_info(d.fmt, nullptr) < 0) {
        fail("the file's streams could not be read");
        avformat_close_input(&d.fmt);
        return nullptr;
    }
    d.video = av_find_best_stream(d.fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (d.video >= 0 && (d.fmt->streams[d.video]->disposition & AV_DISPOSITION_ATTACHED_PIC))
        d.video = -1;   // (a music file's cover)
    const int best_audio = av_find_best_stream(d.fmt, AVMEDIA_TYPE_AUDIO, -1, d.video, nullptr, 0);
    std::vector<std::string> audio_names, sub_names;
    for (unsigned i = 0; i < d.fmt->nb_streams; i++) {
        AVStream *stream = d.fmt->streams[i];
        if (stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            d.audio_streams.push_back(static_cast<int>(i));
            audio_names.push_back(stream_name(stream, static_cast<int>(d.audio_streams.size())));
        } else if (stream->codecpar->codec_type == AVMEDIA_TYPE_SUBTITLE) {
            d.sub_streams.push_back(static_cast<int>(i));
            sub_names.push_back(stream_name(stream, static_cast<int>(d.sub_streams.size())));
        }
    }
    std::string external_name, external_file;
    int encoding = 0;
    {
        std::lock_guard<std::mutex> guard(X.lock);
        encoding = X.sub_encoding;
    }
    if (read_external_subtitles(path, encoding, d.external, external_name, external_file)) {
        sub_names.push_back("File: " + external_name);
        d.external_track = true;
    }
    if (d.video >= 0)
        d.vctx = open_decoder(d.fmt->streams[d.video], std::clamp(wisp_setting_int("player_threads", 4), 1, 6));
    if (d.video >= 0 && !d.vctx)
        d.video = -1;
    int audio_index = -1;
    for (size_t i = 0; i < d.audio_streams.size(); i++)
        if (d.audio_streams[i] == best_audio)
            audio_index = static_cast<int>(i);
    if (audio_index < 0 && !d.audio_streams.empty())
        audio_index = 0;
    if (d.video < 0 && audio_index < 0) {
        fail("no picture or sound FFmpeg can decode in it");
        avformat_close_input(&d.fmt);
        return nullptr;
    }
    {
        std::lock_guard<std::mutex> guard(X.lock);
        X.duration = d.fmt->duration > 0 ? d.fmt->duration / static_cast<double>(AV_TIME_BASE) : 0;
        X.has_video = d.video >= 0;
        X.video_w = d.vctx ? d.vctx->width : 0;
        X.video_h = d.vctx ? d.vctx->height : 0;
        X.video_codec = d.vctx ? d.vctx->codec->name : "";
        X.audio_names = audio_names;
        X.sub_names = sub_names;
        X.sub_file_beside = external_file;
        // what the information panel shows, and the chapters (Navigate > Chapters)
        X.chapters.clear();
        for (unsigned i = 0; i < d.fmt->nb_chapters; i++) {
            const AVChapter *chapter = d.fmt->chapters[i];
            Chapter c;
            c.start = chapter->start * av_q2d(chapter->time_base);
            const AVDictionaryEntry *title = av_dict_get(chapter->metadata, "title", nullptr, 0);
            c.title = title && title->value[0] ? title->value : "Chapter " + std::to_string(i + 1);
            X.chapters.push_back(c);
        }
        X.container = d.fmt->iformat ? (d.fmt->iformat->long_name ? d.fmt->iformat->long_name : d.fmt->iformat->name) : "";
        X.bitrate = d.fmt->bit_rate;
        X.fps = 0;
        X.pixel_format.clear();
        if (d.vctx) {
            const AVStream *stream = d.fmt->streams[d.video];
            X.fps = stream->avg_frame_rate.num > 0 ? av_q2d(stream->avg_frame_rate) : stream->r_frame_rate.num > 0 ? av_q2d(stream->r_frame_rate) : 0;
            if (const char *name = av_get_pix_fmt_name(d.vctx->pix_fmt))
                X.pixel_format = name;
        }
        X.shown_frames = X.dropped_frames = 0;
    }
    if (audio_index >= 0)
        switch_audio(d, audio_index);
    {
        std::lock_guard<std::mutex> guard(X.lock);
        X.has_audio = d.actx != nullptr;
    }
    // the file's first subtitles track, if it has one, else the file beside it
    if (!d.sub_streams.empty())
        switch_sub(d, 0);
    else if (!d.external.empty())
        switch_sub(d, 0);
    wisp_log("player: %s: %s%dx%d, %s, %zu audio, %zu subtitles, %.0f s", path.c_str(), d.vctx ? "" : "(no video) ",
        d.vctx ? d.vctx->width : 0, d.vctx ? d.vctx->height : 0, d.vctx ? d.vctx->codec->name : "-", d.audio_streams.size(),
        sub_names.size(), d.fmt->duration > 0 ? d.fmt->duration / static_cast<double>(AV_TIME_BASE) : 0.0);
    d.packet = av_packet_alloc();
    d.frame = av_frame_alloc();
    for (;;) {
        double seek = -1;
        int want_audio = -1, want_sub = -2;
        bool external_ready = false;
        std::vector<Subtitle> external;
        std::string external_label;
        {
            std::unique_lock<std::mutex> guard(X.lock);
            // room to read more: few pictures made ahead, little sound queued (or the other is missing)
            // (at the end there is nothing to read: it looks every 20 ms whether the queues ran dry)
            X.wake.wait_for(guard, std::chrono::milliseconds(20), [&] {
                const bool pictures_full = !X.has_video || static_cast<int>(X.pictures.size()) >= MAX_FRAMES;
                const bool sound_full = !X.has_audio || X.sound_seconds >= AUDIO_AHEAD;
                return X.quit || X.seek_to >= 0 || X.want_audio >= 0 || X.want_sub != -2 || X.external_ready
                    || (!d.eof && !(pictures_full && sound_full));
            });
            if (X.quit)
                break;
            // a subtitles file loaded (Load subtitle file, another encoding): the "File:" track, now
            if (X.external_ready) {
                X.external_ready = false;
                external_ready = true;
                external = std::move(X.external);
                X.external.clear();
                external_label = "File: " + X.external_name;
                if (d.external_track)
                    X.sub_names.back() = external_label;
                else
                    X.sub_names.push_back(external_label);
            }
        }
        if (external_ready) {
            d.external = std::move(external);
            d.external_track = true;
            switch_sub(d, static_cast<int>(d.sub_streams.size()));
            continue;
        }
        {
            std::unique_lock<std::mutex> guard(X.lock);
            seek = X.seek_to;
            X.seek_to = -1;
            want_audio = X.want_audio;
            X.want_audio = -1;
            want_sub = X.want_sub;
            X.want_sub = -2;
            const bool pictures_full = !X.has_video || static_cast<int>(X.pictures.size()) >= MAX_FRAMES;
            const bool sound_full = !X.has_audio || X.sound_seconds >= AUDIO_AHEAD;
            if (seek < 0 && want_audio < 0 && want_sub == -2 && pictures_full && sound_full)
                continue;
            if (d.eof && seek < 0 && want_audio < 0 && want_sub == -2) {
                if (X.pictures.empty() && X.sound.empty())
                    X.at_end = true;
                continue;
            }
        }
        if (want_audio >= 0)
            switch_audio(d, want_audio);
        if (want_sub != -2)
            switch_sub(d, want_sub);
        if (seek >= 0) {
            const int64_t target = static_cast<int64_t>(seek * AV_TIME_BASE);
            avformat_seek_file(d.fmt, -1, INT64_MIN, target, target, 0);
            if (d.vctx)
                avcodec_flush_buffers(d.vctx);
            if (d.actx)
                avcodec_flush_buffers(d.actx);
            if (d.sctx)
                avcodec_flush_buffers(d.sctx);
            d.eof = false;
            d.late = 0;
            if (d.vctx)
                d.vctx->skip_frame = AVDISCARD_DEFAULT;
            std::lock_guard<std::mutex> guard(X.lock);
            for (Picture &p : X.pictures)
                X.spare.push_back(std::move(p));
            X.pictures.clear();
            X.sound.clear();
            X.sound_seconds = 0;
            if (!d.external_on)
                X.subtitles.clear();
            X.clock_pts = seek;
            X.clock_at = now_s();
            X.at_end = false;
            continue;
        }
        const int read = av_read_frame(d.fmt, d.packet);
        if (read < 0) {
            // the end: what the decoders hold, out
            if (d.vctx) {
                avcodec_send_packet(d.vctx, nullptr);
                drain(d, d.vctx, true);
            }
            if (d.actx) {
                avcodec_send_packet(d.actx, nullptr);
                drain(d, d.actx, false);
            }
            d.eof = true;
            continue;
        }
        if (d.packet->stream_index == d.video && d.vctx) {
            if (avcodec_send_packet(d.vctx, d.packet) >= 0)
                drain(d, d.vctx, true);
        } else if (d.packet->stream_index == d.audio && d.actx) {
            if (avcodec_send_packet(d.actx, d.packet) >= 0)
                drain(d, d.actx, false);
        } else if (d.packet->stream_index == d.sub && d.sctx) {
            AVSubtitle sub;
            int got = 0;
            if (avcodec_decode_subtitle2(d.sctx, &sub, &got, d.packet) >= 0 && got) {
                const AVRational tb = d.fmt->streams[d.sub]->time_base;
                const double pts = d.packet->pts != AV_NOPTS_VALUE ? d.packet->pts * av_q2d(tb) : 0;
                if (!sub.end_display_time && d.packet->duration > 0)
                    sub.end_display_time = static_cast<uint32_t>(d.packet->duration * av_q2d(tb) * 1000);
                std::vector<Subtitle> lines;
                take_subtitle(sub, pts, d.sctx->width ? d.sctx->width : (d.vctx ? d.vctx->width : 0),
                    d.sctx->height ? d.sctx->height : (d.vctx ? d.vctx->height : 0), lines);
                avsubtitle_free(&sub);
                std::lock_guard<std::mutex> guard(X.lock);
                for (Subtitle &s : lines)
                    X.subtitles.push_back(std::move(s));
                if (X.subtitles.size() > 400)
                    X.subtitles.erase(X.subtitles.begin(), X.subtitles.begin() + 100);
            }
        }
        av_packet_unref(d.packet);
    }
    av_packet_free(&d.packet);
    av_frame_free(&d.frame);
    sws_freeContext(d.sws);
    swr_free(&d.swr);
    if (d.vctx)
        avcodec_free_context(&d.vctx);
    if (d.actx)
        avcodec_free_context(&d.actx);
    if (d.sctx)
        avcodec_free_context(&d.sctx);
    avformat_close_input(&d.fmt);
    return nullptr;
}

// --- The audio out's thread -------------------------------------------------------------------------------
void *audio_main(void *)
{
    static bool initialized = false;
    if (!initialized) {
        sceAudioOutInit();   // (an error when done before: harmless)
        initialized = true;
    }
    // the user who started the app; the system's own id (255) when the user service has none (a main
    // port opens for either)
    OrbisUserServiceUserId user = -1;
    int port = -1;
    if (!sceUserServiceGetInitialUser(&user) && user >= 0)
        port = sceAudioOutOpen(user, ORBIS_AUDIO_OUT_PORT_TYPE_MAIN, 0, BLOCK, RATE, ORBIS_AUDIO_OUT_PARAM_FORMAT_S16_STEREO);
    if (port < 0)
        port = sceAudioOutOpen(255, ORBIS_AUDIO_OUT_PORT_TYPE_MAIN, 0, BLOCK, RATE, ORBIS_AUDIO_OUT_PARAM_FORMAT_S16_STEREO);
    if (port < 0)
        wisp_log("player: the audio out did not open (0x%08x): pictures only", static_cast<unsigned>(port));
    std::vector<int16_t> block(static_cast<size_t>(BLOCK) * 2);
    for (;;) {
        double block_pts = -1, block_step = 1.0 / RATE;
        int gain = 100;
        bool quit = false, playing = false;
        {
            std::lock_guard<std::mutex> guard(X.lock);
            quit = X.quit;
            gain = X.muted ? 0 : X.volume * X.boost / 100;
            playing = !X.paused && X.has_audio;
            std::fill(block.begin(), block.end(), 0);
            size_t filled = 0;
            while (playing && filled < static_cast<size_t>(BLOCK) && !X.sound.empty()) {
                Chunk &chunk = X.sound.front();
                if (block_pts < 0) {
                    block_pts = chunk.pts + chunk.used * chunk.step;
                    block_step = chunk.step;
                }
                const size_t frames = chunk.samples.size() / 2;
                const size_t take = std::min(frames - chunk.used, static_cast<size_t>(BLOCK) - filled);
                memcpy(block.data() + filled * 2, chunk.samples.data() + chunk.used * 2, take * 4);
                chunk.used += take;
                filled += take;
                X.sound_seconds -= take / static_cast<double>(RATE);
                if (chunk.used >= frames)
                    X.sound.pop_front();
            }
            if (block_pts >= 0) {
                // what is heard now: this block's start, a block (the one queued before it) earlier
                X.clock_pts = block_pts - BLOCK * block_step;
                X.clock_at = now_s();
            }
        }
        X.wake.notify_all();
        if (quit)
            break;
        // the volume, and a boost over 100% (clipped at full scale)
        if (gain != 100)
            for (int16_t &s : block)
                s = static_cast<int16_t>(std::clamp(s * gain / 100, -32768, 32767));
        if (port >= 0)
            sceAudioOutOutput(port, block.data());   // (blocks until the one before has started)
        else {
            const timespec pause = { 0, 21000000 };
            nanosleep(&pause, nullptr);
        }
    }
    if (port >= 0) {
        sceAudioOutOutput(port, nullptr);
        sceAudioOutClose(port);
    }
    return nullptr;
}

void stop_threads()
{
    if (!threads_up)
        return;
    {
        std::lock_guard<std::mutex> guard(X.lock);
        X.quit = true;
    }
    X.wake.notify_all();
    pthread_join(decode_thread, nullptr);
    pthread_join(audio_thread, nullptr);
    threads_up = false;
}

double position()
{
    std::lock_guard<std::mutex> guard(X.lock);
    return clock_locked();
}

void save_position()
{
    if (P.path.empty())
        return;
    double duration;
    {
        std::lock_guard<std::mutex> guard(X.lock);
        duration = X.duration;
    }
    const double at = position();
    resume_write(P.path, at > 10 && (duration <= 0 || at < duration - 30) ? at : 0);
}

void seek_to(double seconds)
{
    std::lock_guard<std::mutex> guard(X.lock);
    double end = X.duration > 0 ? X.duration - 1 : 1e12;
    X.seek_to = std::clamp(seconds, 0.0, std::max(0.0, end));
    X.at_end = false;      // (until the decoding thread finds the end again)
    P.closing = false;
    X.wake.notify_all();
}

void seek_by(double delta)
{
    seek_to(position() + delta);
    P.osd_until = now_s() + 3;
    P.dirty = true;
}

// The folder's files the player takes, in name order: the one before / after this one
std::string neighbour(int step)
{
    const size_t slash = P.path.find_last_of('/');
    if (slash == std::string::npos)
        return "";
    const std::string folder = P.path.substr(0, slash);
    std::vector<std::string> names;
    if (DIR *dir = opendir(folder.c_str())) {
        while (dirent *e = readdir(dir))
            if (e->d_name[0] != '.' && can_play(e->d_name))
                names.push_back(e->d_name);
        closedir(dir);
    }
    std::sort(names.begin(), names.end());
    const std::string me = P.path.substr(slash + 1);
    for (size_t i = 0; i < names.size(); i++) {
        if (names[i] != me)
            continue;
        const long next = static_cast<long>(i) + step;
        if (next >= 0 && next < static_cast<long>(names.size()))
            return folder + "/" + names[next];
    }
    return "";
}

void blend_sprite(uint32_t *bgra, int stride, int clip_x, int clip_y, int clip_w, int clip_h, const ps4ui::Image &sprite, int x, int y)
{
    // sprite: premultiplied RGBA in memory; the screen BGRA
    for (int row = 0; row < sprite.height; row++) {
        const int dy = y + row;
        if (dy < clip_y || dy >= clip_y + clip_h)
            continue;
        uint32_t *dst = bgra + static_cast<size_t>(dy) * stride;
        const uint32_t *src = sprite.pixels.data() + static_cast<size_t>(row) * sprite.width;
        for (int col = 0; col < sprite.width; col++) {
            const int dx = x + col;
            if (dx < clip_x || dx >= clip_x + clip_w)
                continue;
            const uint32_t p = src[col], a = p >> 24;
            if (!a)
                continue;
            const uint32_t r = p & 0xff, g = (p >> 8) & 0xff, b = (p >> 16) & 0xff;
            const uint32_t d = dst[dx], keep = 255 - a;
            const uint32_t db = d & 0xff, dg = (d >> 8) & 0xff, dr = (d >> 16) & 0xff;
            dst[dx] = 0xff000000u | std::min(255u, r + (dr * keep + 127) / 255) << 16 | std::min(255u, g + (dg * keep + 127) / 255) << 8
                | std::min(255u, b + (db * keep + 127) / 255);
        }
    }
}

// a canvas (RGBA, opaque where drawn) onto the screen picture at (x, y): every pixel whose alpha is not 0
void put_canvas(uint32_t *bgra, int stride, const Canvas &c, int x, int y, uint8_t alpha)
{
    const uint32_t *src = c.data();
    for (int row = 0; row < c.height(); row++) {
        uint32_t *dst = bgra + static_cast<size_t>(y + row) * stride + x;
        for (int col = 0; col < c.width(); col++) {
            const uint32_t p = src[static_cast<size_t>(row) * c.width() + col];
            const uint32_t pa = ((p >> 24) * alpha) / 255;
            if (!pa)
                continue;
            const uint32_t r = p & 0xff, g = (p >> 8) & 0xff, b = (p >> 16) & 0xff, keep = 255 - pa;
            const uint32_t d = dst[col];
            dst[col] = 0xff000000u | ((r * pa + ((d >> 16) & 0xff) * keep) / 255) << 16 | ((g * pa + ((d >> 8) & 0xff) * keep) / 255) << 8
                | ((b * pa + (d & 0xff) * keep) / 255);
        }
    }
}

// --- What the player keeps (settings.txt) ------------------------------------------------------------------
void load_settings()
{
    P.sub_size = std::clamp(wisp_setting_int("player_sub_size", 100), 50, 250);
    P.sub_lift = std::clamp(wisp_setting_int("player_sub_lift", 0), -300, 700);
    P.sub_box = wisp_setting_int("player_sub_box", 0) != 0;
    P.encoding = std::clamp(wisp_setting_int("player_sub_encoding", 0), 0, ENCODING_COUNT - 1);
    P.after = std::clamp(wisp_setting_int("player_after", wisp_setting_int("player_next", 1) ? 0 : 3), 0, 3);
}

void keep(const char *key, int value) { wisp_setting_set(key, std::to_string(value).c_str()); }

std::vector<std::string> recent_read()
{
    std::vector<std::string> list;
    if (FILE *f = fopen(RECENT_FILE, "r")) {
        char line[4096];
        while (fgets(line, sizeof line, f) && list.size() < 15) {
            line[strcspn(line, "\n")] = 0;
            if (line[0])
                list.push_back(line);
        }
        fclose(f);
    }
    return list;
}

void recent_add(const std::string &file)
{
    std::vector<std::string> list = recent_read();
    list.erase(std::remove(list.begin(), list.end(), file), list.end());
    list.insert(list.begin(), file);
    mkdir(WISP_DATA "/player", 0777);
    if (FILE *f = fopen(RECENT_FILE, "w")) {
        for (size_t i = 0; i < list.size() && i < 15; i++)
            fprintf(f, "%s\n", list[i].c_str());
        fclose(f);
    }
}

std::string base_of(const std::string &path) { return path.substr(path.find_last_of('/') + 1); }

std::string signed_seconds(double s)
{
    char out[24];
    if (std::fabs(s) < 0.0005)
        return "0 s";
    snprintf(out, sizeof out, "%+.1f s", s);
    return out;
}

std::string speed_text(double s)
{
    char out[16];
    snprintf(out, sizeof out, s == std::floor(s) ? "%.0f×" : "%.2g×", s);
    return out;
}

// --- What the menu does ---------------------------------------------------------------------------------------
void set_speed(double speed)
{
    speed = std::clamp(speed, SPEEDS[0], SPEEDS[SPEED_COUNT - 1]);
    {
        std::lock_guard<std::mutex> guard(X.lock);
        X.clock_pts = clock_locked();
        X.clock_at = now_s();
        X.speed = speed;
    }
    // the sound queued was made for the old speed: from here again
    seek_to(position());
    say(speed == 1.0 ? "Speed: normal" : "Speed: " + speed_text(speed));
}

void speed_step(int step)
{
    double now;
    {
        std::lock_guard<std::mutex> guard(X.lock);
        now = X.speed;
    }
    int at = 3;
    for (int i = 0; i < SPEED_COUNT; i++)
        if (std::fabs(SPEEDS[i] - now) < 0.01)
            at = i;
    set_speed(SPEEDS[std::clamp(at + step, 0, SPEED_COUNT - 1)]);
}

void set_audio_delay(double delay)
{
    delay = std::clamp(delay, -10.0, 10.0);
    std::lock_guard<std::mutex> guard(X.lock);
    const double change = delay - X.audio_delay;
    X.audio_delay = delay;
    for (Chunk &c : X.sound)
        c.pts += change;
    say("Audio delay: " + signed_seconds(delay));
}

double audio_delay()
{
    std::lock_guard<std::mutex> guard(X.lock);
    return X.audio_delay;
}

void set_sub_delay(double delay)
{
    P.sub_delay = std::clamp(delay, -60.0, 60.0);
    say("Subtitles delay: " + signed_seconds(P.sub_delay));
}

View view_now()
{
    std::lock_guard<std::mutex> guard(X.lock);
    return X.view;
}

void change_view(const std::function<void(View &)> &change, const std::string &said)
{
    {
        std::lock_guard<std::mutex> guard(X.lock);
        change(X.view);
        X.view.zoom = std::clamp(X.view.zoom, 0.25, 4.0);
        X.view.brightness = std::clamp(X.view.brightness, -100, 100);
        X.view.contrast = std::clamp(X.view.contrast, -100, 100);
        X.view.saturation = std::clamp(X.view.saturation, -100, 100);
    }
    say(said);
}

void set_volume_boost(int boost)
{
    {
        std::lock_guard<std::mutex> guard(X.lock);
        X.boost = std::clamp(boost, 100, 400);
    }
    keep("player_boost", boost);
    say(boost == 100 ? "Volume boost off" : "Volume boost " + std::to_string(boost) + "%");
}

// A subtitles file (or an archive's entry), read in the encoding chosen: the "File:" track
void use_subtitle_file(const std::string &file, const std::string &inside)
{
    std::vector<Subtitle> lines;
    std::string used;
    const std::string name = base_of(inside.empty() ? file : inside);
    if (!load_subtitles(file, inside, P.encoding, lines, used) || lines.empty()) {
        say("No subtitles could be read from " + name);
        return;
    }
    P.sub_file = file;
    P.sub_inside = inside;
    {
        std::lock_guard<std::mutex> guard(X.lock);
        X.external = std::move(lines);
        X.external_name = name;
        X.external_ready = true;
    }
    X.wake.notify_all();
    say("Subtitles: " + name + " (read as " + used + ")");
}

// Another encoding: the subtitles file read again in it (the one loaded, else the one beside the video)
void set_encoding(int encoding)
{
    P.encoding = std::clamp(encoding, 0, ENCODING_COUNT - 1);
    keep("player_sub_encoding", P.encoding);
    std::string file = P.sub_file, inside = P.sub_inside;
    if (file.empty()) {
        std::lock_guard<std::mutex> guard(X.lock);
        file = X.sub_file_beside;
        X.sub_encoding = P.encoding;
    }
    if (file.empty())
        say(std::string("Subtitles files: ") + ENCODINGS[P.encoding].name);
    else
        use_subtitle_file(file, inside);
}

// The video's name as subtitle sites want it: dots and underscores as spaces, up to its year or its
// episode (S01E02), without the release's tags (1080p, x264, BluRay, ...)
std::string search_name()
{
    std::string name = P.name;
    const size_t dot = name.find_last_of('.');
    if (dot != std::string::npos && dot > 0)
        name = name.substr(0, dot);
    for (char &c : name)
        if (c == '.' || c == '_' || c == '[' || c == ']' || c == '(' || c == ')')
            c = ' ';
    static const char *const tags[] = { "1080p", "720p", "2160p", "480p", "576p", "4k", "x264", "x265", "h264", "h265", "hevc",
        "avc", "bluray", "blu-ray", "brrip", "bdrip", "webrip", "web-dl", "webdl", "web", "dvdrip", "hdtv", "hdrip", "remux",
        "10bit", "aac", "ac3", "dts", "yify", "yts", "rarbg", "proper", "repack", "extended", "unrated", "imax", "multi", "subs" };
    std::string out;
    size_t start = 0;
    while (start < name.size()) {
        const size_t space = name.find(' ', start);
        const std::string word = name.substr(start, space == std::string::npos ? std::string::npos : space - start);
        start = space == std::string::npos ? name.size() : space + 1;
        if (word.empty())
            continue;
        const std::string low = lower(word);
        bool tag = false;
        for (const char *t : tags)
            tag = tag || low == t;
        if (tag)
            break;
        if (!out.empty())
            out += ' ';
        out += word;
        const bool year = word.size() == 4 && (word.rfind("19", 0) == 0 || word.rfind("20", 0) == 0)
            && std::all_of(word.begin(), word.end(), ::isdigit) && out.size() > 4;
        const bool episode = low.size() >= 6 && low[0] == 's' && isdigit(static_cast<unsigned char>(low[1])) && low.find('e') != std::string::npos;
        if (year || episode)
            break;
    }
    return out.empty() ? P.name : out;
}

void find_online(int server)
{
    char *query = g_uri_escape_string(search_name().c_str(), nullptr, FALSE);
    std::string url = SERVERS[server].url;
    const size_t at = url.find("%s");
    if (at != std::string::npos)
        url.replace(at, 2, query);
    g_free(query);
    if (H.open_tab)
        H.open_tab(url);
    if (H.toast)
        H.toast(std::string(SERVERS[server].name) + ": what it downloads goes to Downloads; then Subtitles > Load subtitle file (zip files too)");
}

// h:mm:ss, m:ss or seconds (a decimal point too); -1 for anything else
double parse_time(const std::string &text)
{
    double total = 0, part = 0;
    bool any = false;
    std::string number;
    for (size_t i = 0; i <= text.size(); i++) {
        const char c = i < text.size() ? text[i] : ':';
        if (isdigit(static_cast<unsigned char>(c)) || c == '.') {
            number += c;
            continue;
        }
        if (c == ':' || c == ' ' || i == text.size()) {
            if (number.empty())
                continue;
            part = atof(number.c_str());
            total = total * 60 + part;
            number.clear();
            any = true;
            continue;
        }
        return -1;
    }
    return any ? total : -1;
}

void go_to_time()
{
    if (!H.ask_text) {
        say("Go to: no keyboard here");
        return;
    }
    H.ask_text("Go to (h:mm:ss, m:ss or seconds)", clock_text(position()), [](bool ok, const std::string &text) {
        if (!ok)
            return;
        const double t = parse_time(text);
        if (t < 0) {
            say("Not a time: " + text);
            return;
        }
        seek_to(t);
        say("Go to " + clock_text(t));
    });
}

void toggle_pause_now()
{
    std::lock_guard<std::mutex> guard(X.lock);
    X.clock_pts = clock_locked();
    X.clock_at = now_s();
    X.paused = !X.paused;
    say(X.paused ? "Paused" : "Playing");
}

void stop_playing()
{
    seek_to(0);
    std::lock_guard<std::mutex> guard(X.lock);
    X.paused = true;
    X.clock_pts = 0;
    say("Stopped");
}

// The picture on the screen as a JPEG in Downloads/Screenshots (MPC-HC's Save Image): the download
// folder chosen (v00.29), Screenshots in it
void save_image()
{
    const Picture &pic = P.shown;
    if (pic.pixels.empty() || pic.w < 2 || pic.h < 2) {
        say("No picture to save yet");
        return;
    }
    const std::string folder = wisp_download_folder() + "/Screenshots";
    mkdir(folder.c_str(), 0777);
    std::string stem = P.name;
    const size_t dot = stem.find_last_of('.');
    if (dot != std::string::npos && dot > 0)
        stem = stem.substr(0, dot);
    std::string when = clock_text(position());
    std::replace(when.begin(), when.end(), ':', '.');
    const std::string name = stem + " " + when + ".jpg", path = folder + "/" + name;
    FILE *f = fopen(path.c_str(), "wb");
    if (!f) {
        say("Could not save it: " + std::string(strerror(errno)));
        return;
    }
    jpeg_compress_struct cinfo;
    jpeg_error_mgr jerr;
    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_compress(&cinfo);
    jpeg_stdio_dest(&cinfo, f);
    cinfo.image_width = static_cast<JDIMENSION>(pic.w);
    cinfo.image_height = static_cast<JDIMENSION>(pic.h);
    cinfo.input_components = 3;
    cinfo.in_color_space = JCS_RGB;
    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, 92, TRUE);
    jpeg_start_compress(&cinfo, TRUE);
    std::vector<uint8_t> row(static_cast<size_t>(pic.w) * 3);
    while (cinfo.next_scanline < cinfo.image_height) {
        const uint32_t *src = pic.pixels.data() + static_cast<size_t>(cinfo.next_scanline) * pic.w;
        for (int x = 0; x < pic.w; x++) {
            row[x * 3] = (src[x] >> 16) & 0xff;   // (BGRA in a word: red in bits 16-23)
            row[x * 3 + 1] = (src[x] >> 8) & 0xff;
            row[x * 3 + 2] = src[x] & 0xff;
        }
        JSAMPROW rows[1] = { row.data() };
        jpeg_write_scanlines(&cinfo, rows, 1);
    }
    jpeg_finish_compress(&cinfo);
    jpeg_destroy_compress(&cinfo);
    fclose(f);
    say("Saved: Downloads/Screenshots/" + name);
}

void toggle_fullscreen()
{
    if (H.fullscreen && H.is_fullscreen)
        H.fullscreen(!H.is_fullscreen());
}

void go_file(int step)
{
    const std::string next = neighbour(step);
    if (next.empty())
        say(step > 0 ? "The last file in the folder" : "The first file in the folder");
    else
        open(next);
}

void open_picker(bool subtitles);

// --- The menu: what it holds, made afresh each time it opens (its values are the moment's) --------------------
Item action(const std::string &label, std::function<void()> act, const std::string &hint = "", bool enabled = true)
{
    Item it;
    it.label = label;
    it.hint = hint;
    it.act = std::move(act);
    it.enabled = enabled;
    return it;
}

Item check(const std::string &label, bool on, std::function<void()> act, const std::string &hint = "")
{
    Item it = action(label, std::move(act), hint);
    it.kind = Item::Check;
    it.on = on;
    return it;
}

Item radio(const std::string &label, bool on, std::function<void()> act, const std::string &hint = "")
{
    Item it = action(label, std::move(act), hint);
    it.kind = Item::Radio;
    it.on = on;
    return it;
}

Item step(const std::string &label, std::function<void()> act, const std::string &hint = "")
{
    Item it = action(label, std::move(act), hint);
    it.keep_open = true;
    return it;
}

Item submenu(const std::string &label, std::function<std::vector<Item>()> sub, const std::string &hint = "", bool enabled = true)
{
    Item it;
    it.label = label;
    it.hint = hint;
    it.kind = Item::Sub;
    it.sub = std::move(sub);
    it.enabled = enabled;
    return it;
}

Item separator()
{
    Item it;
    it.kind = Item::Separator;
    it.enabled = false;
    return it;
}

std::vector<Item> audio_menu()
{
    std::vector<std::string> names;
    int now = -1, boost = 100;
    bool muted = false;
    {
        std::lock_guard<std::mutex> guard(X.lock);
        names = X.audio_names;
        now = X.audio_now;
        boost = X.boost;
        muted = X.muted;
    }
    std::vector<Item> items;
    for (int i = 0; i < static_cast<int>(names.size()); i++)
        items.push_back(radio(names[i], i == now, [i] {
            std::lock_guard<std::mutex> guard(X.lock);
            X.want_audio = i;
            X.wake.notify_all();
        }));
    if (names.empty())
        items.push_back(action("No audio", nullptr, "", false));
    items.push_back(separator());
    const double delay = audio_delay();
    items.push_back(submenu("Audio delay", [] {
        return std::vector<Item> { step("Later by 0.5 s", [] { set_audio_delay(audio_delay() + 0.5); }),
            step("Later by 0.1 s", [] { set_audio_delay(audio_delay() + 0.1); }, "+"),
            step("Sooner by 0.1 s", [] { set_audio_delay(audio_delay() - 0.1); }, "-"),
            step("Sooner by 0.5 s", [] { set_audio_delay(audio_delay() - 0.5); }), separator(),
            action("No delay", [] { set_audio_delay(0); }) };
    }, signed_seconds(delay)));
    items.push_back(submenu("Volume boost", [boost] {
        std::vector<Item> list;
        for (int b : { 100, 150, 200, 300, 400 })
            list.push_back(radio(b == 100 ? "Off (100%)" : std::to_string(b) + "%", boost == b, [b] { set_volume_boost(b); }));
        return list;
    }, boost == 100 ? "" : std::to_string(boost) + "%"));
    items.push_back(check("Mute", muted, [] {
        std::lock_guard<std::mutex> guard(X.lock);
        X.muted = !X.muted;
        say(X.muted ? "Muted" : "Sound on");
    }, "M"));
    return items;
}

std::vector<Item> subtitles_menu()
{
    std::vector<std::string> names;
    int now = -1;
    {
        std::lock_guard<std::mutex> guard(X.lock);
        names = X.sub_names;
        now = X.sub_now;
    }
    std::vector<Item> items;
    items.push_back(radio("Off", now < 0, [] {
        std::lock_guard<std::mutex> guard(X.lock);
        X.want_sub = -1;
        X.wake.notify_all();
        say("Subtitles off");
    }));
    for (int i = 0; i < static_cast<int>(names.size()); i++)
        items.push_back(radio(names[i], i == now, [i] {
            std::lock_guard<std::mutex> guard(X.lock);
            X.want_sub = i;
            X.wake.notify_all();
        }));
    items.push_back(separator());
    items.push_back(action("Load subtitle file...", [] { open_picker(true); }, "Ctrl+L"));
    items.push_back(submenu("Find subtitles online", [] {
        std::vector<Item> list;
        for (int i = 0; i < SERVER_COUNT; i++)
            list.push_back(action(SERVERS[i].name, [i] { find_online(i); }));
        return list;
    }, "servers"));
    items.push_back(separator());
    items.push_back(submenu("Encoding of files", [] {
        std::vector<Item> list;
        for (int i = 0; i < ENCODING_COUNT; i++)
            list.push_back(radio(ENCODINGS[i].name, P.encoding == i, [i] { set_encoding(i); }));
        return list;
    }, P.encoding ? ENCODINGS[P.encoding].iconv : "Auto"));
    items.push_back(submenu("Delay", [] {
        return std::vector<Item> { step("Later by 1 s", [] { set_sub_delay(P.sub_delay + 1); }),
            step("Later by 0.1 s", [] { set_sub_delay(P.sub_delay + 0.1); }, "F2"),
            step("Sooner by 0.1 s", [] { set_sub_delay(P.sub_delay - 0.1); }, "F1"),
            step("Sooner by 1 s", [] { set_sub_delay(P.sub_delay - 1); }), separator(), action("No delay", [] { set_sub_delay(0); }) };
    }, signed_seconds(P.sub_delay)));
    items.push_back(submenu("Size", [] {
        return std::vector<Item> { step("Larger", [] {
            P.sub_size = std::min(250, P.sub_size + 10);
            keep("player_sub_size", P.sub_size);
            say("Subtitles " + std::to_string(P.sub_size) + "%");
        }), step("Smaller", [] {
            P.sub_size = std::max(50, P.sub_size - 10);
            keep("player_sub_size", P.sub_size);
            say("Subtitles " + std::to_string(P.sub_size) + "%");
        }), separator(), action("Normal (100%)", [] {
            P.sub_size = 100;
            keep("player_sub_size", 100);
            say("Subtitles 100%");
        }) };
    }, std::to_string(P.sub_size) + "%"));
    items.push_back(submenu("Position", [] {
        return std::vector<Item> { step("Higher", [] {
            P.sub_lift = std::min(700, P.sub_lift + 20);
            keep("player_sub_lift", P.sub_lift);
            say("Subtitles higher");
        }), step("Lower", [] {
            P.sub_lift = std::max(-300, P.sub_lift - 20);
            keep("player_sub_lift", P.sub_lift);
            say("Subtitles lower");
        }), separator(), action("Where they were", [] {
            P.sub_lift = 0;
            keep("player_sub_lift", 0);
            say("Subtitles where they were");
        }) };
    }));
    items.push_back(check("A box behind them", P.sub_box, [] {
        P.sub_box = !P.sub_box;
        keep("player_sub_box", P.sub_box ? 1 : 0);
    }));
    return items;
}

std::vector<Item> picture_step_menu(const char *what, int View::*field)
{
    return std::vector<Item> { step("More", [field, what] {
        change_view([field](View &v) { v.*field += 5; }, std::string(what) + " " + std::to_string(view_now().*field + 5));
    }), step("Less", [field, what] {
        change_view([field](View &v) { v.*field -= 5; }, std::string(what) + " " + std::to_string(view_now().*field - 5));
    }), separator(), action("Normal", [field, what] { change_view([field](View &v) { v.*field = 0; }, std::string(what) + " normal"); }) };
}

std::vector<Item> video_menu()
{
    const View v = view_now();
    std::vector<Item> items;
    items.push_back(submenu("Aspect ratio", [v] {
        std::vector<Item> list;
        for (int i = 0; i < ASPECT_COUNT; i++)
            list.push_back(radio(ASPECTS[i].name, v.aspect == i, [i] {
                change_view([i](View &w) { w.aspect = i; }, std::string("Aspect ratio: ") + ASPECTS[i].name);
            }));
        return list;
    }, v.aspect ? ASPECTS[v.aspect].name : "", true));
    items.push_back(submenu("Video frame", [v] {
        std::vector<Item> list;
        for (int i = 0; i < FRAME_MODE_COUNT; i++)
            list.push_back(radio(FRAME_MODES[i], v.frame == i, [i] {
                change_view([i](View &w) { w.frame = i; }, FRAME_MODES[i]);
            }));
        return list;
    }));
    char zoom[16];
    snprintf(zoom, sizeof zoom, "%d%%", static_cast<int>(std::lround(v.zoom * 100)));
    items.push_back(submenu("Zoom", [] {
        return std::vector<Item> { step("Zoom in", [] {
            change_view([](View &w) { w.zoom *= 1.1; }, "Zoom " + std::to_string(static_cast<int>(std::lround(std::min(4.0, view_now().zoom * 1.1) * 100))) + "%");
        }, "Num +"), step("Zoom out", [] {
            change_view([](View &w) { w.zoom /= 1.1; }, "Zoom " + std::to_string(static_cast<int>(std::lround(std::max(0.25, view_now().zoom / 1.1) * 100))) + "%");
        }, "Num -"), separator(), action("100%", [] { change_view([](View &w) { w.zoom = 1; }, "Zoom 100%"); }, "Num 5") };
    }, zoom));
    items.push_back(separator());
    items.push_back(submenu("Brightness", [] { return picture_step_menu("Brightness", &View::brightness); }, v.brightness ? std::to_string(v.brightness) : ""));
    items.push_back(submenu("Contrast", [] { return picture_step_menu("Contrast", &View::contrast); }, v.contrast ? std::to_string(v.contrast) : ""));
    items.push_back(submenu("Saturation", [] { return picture_step_menu("Saturation", &View::saturation); }, v.saturation ? std::to_string(v.saturation) : ""));
    items.push_back(separator());
    items.push_back(action("Reset the picture", [] { change_view([](View &w) { w = View(); }, "The picture as it was"); }));
    return items;
}

std::vector<Item> play_menu()
{
    double speed;
    {
        std::lock_guard<std::mutex> guard(X.lock);
        speed = X.speed;
    }
    std::vector<Item> items;
    items.push_back(submenu("Speed", [speed] {
        std::vector<Item> list { step("Faster", [] { speed_step(+1); }, "Ctrl+Up"), step("Slower", [] { speed_step(-1); }, "Ctrl+Down"),
            separator() };
        for (int i = 0; i < SPEED_COUNT; i++)
            list.push_back(radio(i == 3 ? "Normal" : speed_text(SPEEDS[i]), std::fabs(SPEEDS[i] - speed) < 0.01, [i] { set_speed(SPEEDS[i]); },
                i == 3 ? "Ctrl+R" : ""));
        return list;
    }, speed_text(speed)));
    items.push_back(submenu("When it ends", [] {
        static const char *const names[] = { "Play the folder's next file", "Play it again", "Stop there", "Close the player" };
        std::vector<Item> list;
        for (int i = 0; i < 4; i++)
            list.push_back(radio(names[i], P.after == i, [i] {
                P.after = i;
                keep("player_after", i);
            }));
        return list;
    }));
    return items;
}

std::vector<Item> navigate_menu()
{
    std::vector<Chapter> chapters;
    {
        std::lock_guard<std::mutex> guard(X.lock);
        chapters = X.chapters;
    }
    std::vector<Item> items;
    items.push_back(action("Go to...", go_to_time, "Ctrl+G"));
    items.push_back(submenu("Chapters", [chapters] {
        std::vector<Item> list;
        const double now = position();
        for (size_t i = 0; i < chapters.size(); i++) {
            const bool current = now >= chapters[i].start && (i + 1 >= chapters.size() || now < chapters[i + 1].start);
            const double at = chapters[i].start;
            list.push_back(radio(chapters[i].title, current, [at] { seek_to(at); }, clock_text(at)));
        }
        return list;
    }, chapters.empty() ? "none" : std::to_string(chapters.size()), !chapters.empty()));
    items.push_back(separator());
    items.push_back(action("Previous file", [] { go_file(-1); }, "Page Up"));
    items.push_back(action("Next file", [] { go_file(+1); }, "Page Down"));
    items.push_back(submenu("Files in this folder", [] {
        std::vector<Item> list;
        const size_t slash = P.path.find_last_of('/');
        if (slash == std::string::npos)
            return list;
        const std::string folder = P.path.substr(0, slash);
        std::vector<std::string> names;
        if (DIR *dir = opendir(folder.c_str())) {
            while (dirent *e = readdir(dir))
                if (e->d_name[0] != '.' && can_play(e->d_name))
                    names.push_back(e->d_name);
            closedir(dir);
        }
        std::sort(names.begin(), names.end());
        for (const std::string &n : names) {
            const std::string path = folder + "/" + n;
            list.push_back(radio(n, path == P.path, [path] { open(path); }));
        }
        return list;
    }));
    return items;
}

std::vector<Item> root_menu()
{
    bool paused, muted;
    {
        std::lock_guard<std::mutex> guard(X.lock);
        paused = X.paused;
        muted = X.muted;
    }
    (void)muted;
    std::vector<Item> items;
    items.push_back(action(paused ? "Play" : "Pause", toggle_pause_now, "Space"));
    items.push_back(action("Stop", stop_playing));
    items.push_back(separator());
    items.push_back(action("Open file...", [] { open_picker(false); }, "Ctrl+O"));
    const std::vector<std::string> recent = recent_read();
    items.push_back(submenu("Recent files", [recent] {
        std::vector<Item> list;
        for (const std::string &path : recent)
            list.push_back(radio(base_of(path), path == P.path, [path] {
                struct stat st;
                if (stat(path.c_str(), &st) == 0)
                    open(path);
                else
                    say("Gone: " + base_of(path));
            }));
        return list;
    }, "", !recent.empty()));
    items.push_back(separator());
    items.push_back(submenu("Navigate", navigate_menu));
    items.push_back(submenu("Play", play_menu));
    items.push_back(submenu("Audio", audio_menu));
    items.push_back(submenu("Subtitles", subtitles_menu));
    items.push_back(submenu("Video", video_menu));
    items.push_back(separator());
    items.push_back(action("Save image", save_image, "Ctrl+S"));
    items.push_back(check("Information", P.info, [] { P.info = !P.info; }, "Tab"));
    items.push_back(check("Full screen", H.is_fullscreen && H.is_fullscreen(), toggle_fullscreen, "F"));
    items.push_back(separator());
    items.push_back(action("Close", [] {
        if (H.closed)
            H.closed();
    }, "Esc"));
    return items;
}

// --- The menu: drawn and driven -------------------------------------------------------------------------------
constexpr int MENU_ROW_H = 40, MENU_SEP_H = 13, MENU_PAD = 8, MENU_MIN_W = 300;
const Color MENU_BG = rgb(0x2b2c30, 248), MENU_HOVER = rgb(0x3c4043), MENU_TEXT = rgb(0xe8eaed), MENU_DIM = rgb(0x8e9196),
    MENU_ACCENT = rgb(0x8ab4f8);

Canvas &measurer()
{
    static Canvas c(1, 1);
    return c;
}

bool selectable(const Item &it) { return it.kind != Item::Separator && it.enabled; }

void level_layout(Level &l, int area_w, int area_h, int x, int y)
{
    int text_w = 0, hint_w = 0;
    for (const Item &it : l.items) {
        text_w = std::max(text_w, measurer().text_width(it.label, 21));
        if (!it.hint.empty())
            hint_w = std::max(hint_w, measurer().text_width(it.hint, 18));
    }
    l.w = std::clamp(48 + text_w + (hint_w ? hint_w + 34 : 0) + 40, MENU_MIN_W, std::max(MENU_MIN_W, area_w - 40));
    // a list longer than the screen scrolls: then a separator is a row like the rest
    int natural = 2 * MENU_PAD;
    for (const Item &it : l.items)
        natural += it.kind == Item::Separator ? MENU_SEP_H : MENU_ROW_H;
    const int max_rows = std::max(3, (area_h - 40 - 2 * MENU_PAD) / MENU_ROW_H);
    if (natural <= area_h - 40) {
        l.rows = static_cast<int>(l.items.size());
        l.h = natural;
        l.scroll = 0;
    } else {
        l.rows = max_rows;
        l.h = 2 * MENU_PAD + max_rows * MENU_ROW_H;
        if (l.index >= 0) {
            if (l.index < l.scroll)
                l.scroll = l.index;
            if (l.index >= l.scroll + l.rows)
                l.scroll = l.index - l.rows + 1;
        }
        l.scroll = std::clamp(l.scroll, 0, std::max(0, static_cast<int>(l.items.size()) - l.rows));
    }
    l.x = std::clamp(x, 10, std::max(10, area_w - l.w - 10));
    l.y = std::clamp(y, 10, std::max(10, area_h - l.h - 10));
}

bool scrolling(const Level &l) { return l.rows < static_cast<int>(l.items.size()); }

// the rows' tops (in the level) for the items shown: -1 for those scrolled out of sight
int row_top(const Level &l, int i)
{
    if (scrolling(l))
        return i < l.scroll || i >= l.scroll + l.rows ? -1 : MENU_PAD + (i - l.scroll) * MENU_ROW_H;
    int y = MENU_PAD;
    for (int k = 0; k < i; k++)
        y += l.items[k].kind == Item::Separator ? MENU_SEP_H : MENU_ROW_H;
    return y;
}

int row_height(const Level &l, int i) { return scrolling(l) || l.items[i].kind != Item::Separator ? MENU_ROW_H : MENU_SEP_H; }

int row_at(const Level &l, float px, float py)
{
    if (px < l.x || px >= l.x + l.w || py < l.y || py >= l.y + l.h)
        return -1;
    for (int i = 0; i < static_cast<int>(l.items.size()); i++) {
        const int top = row_top(l, i);
        if (top >= 0 && py >= l.y + top && py < l.y + top + row_height(l, i))
            return i;
    }
    return -1;
}

void menu_close()
{
    M.open = false;
    M.levels.clear();
    P.dirty = true;
}

void menu_open_at(float x, float y)
{
    M.open = true;
    M.levels.clear();
    M.at_x = x;
    M.at_y = y;
    Level root;
    root.items = root_menu();
    root.index = -1;
    for (int i = 0; i < static_cast<int>(root.items.size()); i++)
        if (selectable(root.items[i])) {
            root.index = i;
            break;
        }
    M.levels.push_back(std::move(root));
    P.dirty = true;
}

// a level's submenu (its item index) opened to its right
void menu_open_sub(size_t level, int index, bool select_first)
{
    M.levels.resize(level + 1);
    Level &parent = M.levels[level];
    if (index < 0 || index >= static_cast<int>(parent.items.size()) || parent.items[index].kind != Item::Sub || !parent.items[index].enabled)
        return;
    Level sub;
    sub.items = parent.items[index].sub ? parent.items[index].sub() : std::vector<Item>();
    sub.index = -1;
    if (select_first)
        for (int i = 0; i < static_cast<int>(sub.items.size()); i++)
            if (selectable(sub.items[i])) {
                sub.index = i;
                break;
            }
    // (it shows its chosen one: a radio's)
    for (int i = 0; i < static_cast<int>(sub.items.size()) && select_first; i++)
        if (sub.items[i].kind == Item::Radio && sub.items[i].on)
            sub.index = i;
    M.levels.push_back(std::move(sub));
    P.dirty = true;
}

// After a step (+ / -): every level made again with today's values, where they were
void menu_refresh()
{
    if (!M.open || M.levels.empty())
        return;
    std::vector<int> chosen;
    for (const Level &l : M.levels)
        chosen.push_back(l.index);
    const size_t depth = M.levels.size();
    std::vector<int> scrolls;
    for (const Level &l : M.levels)
        scrolls.push_back(l.scroll);
    M.levels.clear();
    Level root;
    root.items = root_menu();
    root.index = chosen[0];
    root.scroll = scrolls[0];
    M.levels.push_back(std::move(root));
    for (size_t k = 1; k < depth; k++) {
        const Level &parent = M.levels[k - 1];
        const int at = chosen[k - 1];
        if (at < 0 || at >= static_cast<int>(parent.items.size()) || parent.items[at].kind != Item::Sub || !parent.items[at].sub)
            break;
        Level sub;
        sub.items = parent.items[at].sub();
        sub.index = std::min(chosen[k], static_cast<int>(sub.items.size()) - 1);
        sub.scroll = scrolls[k];
        M.levels.push_back(std::move(sub));
    }
    P.dirty = true;
}

void menu_activate(size_t level, int index)
{
    if (level >= M.levels.size())
        return;
    Level &l = M.levels[level];
    if (index < 0 || index >= static_cast<int>(l.items.size()) || !selectable(l.items[index]))
        return;
    l.index = index;
    const Item it = l.items[index];
    if (it.kind == Item::Sub) {
        menu_open_sub(level, index, true);
        return;
    }
    if (!it.keep_open)
        menu_close();
    if (it.act)
        it.act();
    if (it.keep_open)
        menu_refresh();
    P.osd_until = now_s() + 2;
    P.dirty = true;
}

void menu_move(int step)
{
    if (M.levels.empty())
        return;
    Level &l = M.levels.back();
    const int n = static_cast<int>(l.items.size());
    if (!n)
        return;
    int i = l.index;
    for (int tries = 0; tries < n; tries++) {
        i = (i + step + n) % n;
        if (selectable(l.items[i]))
            break;
    }
    l.index = i;
    P.dirty = true;
}

void draw_menu(uint32_t *bgra, int stride, int x0, int y0, int area_w, int area_h)
{
    // the levels laid out from the root: each submenu at its parent's chosen row, right of it (left when
    // there is no room)
    for (size_t k = 0; k < M.levels.size(); k++) {
        Level &l = M.levels[k];
        if (k == 0) {
            level_layout(l, area_w, area_h, static_cast<int>(M.at_x), static_cast<int>(M.at_y));
            continue;
        }
        const Level &parent = M.levels[k - 1];
        const int top = std::max(0, row_top(parent, std::max(0, parent.index)));
        level_layout(l, area_w, area_h, parent.x + parent.w - 6, parent.y + top - MENU_PAD);
        if (parent.x + parent.w - 6 + l.w > area_w - 10)
            l.x = std::max(10, parent.x - l.w + 6);
    }
    for (const Level &l : M.levels) {
        Canvas c(l.w, l.h);
        c.fill_round_rect(0, 0, l.w, l.h, 12, MENU_BG);
        for (int i = 0; i < static_cast<int>(l.items.size()); i++) {
            const int top = row_top(l, i);
            if (top < 0)
                continue;
            const Item &it = l.items[i];
            if (it.kind == Item::Separator) {
                c.fill_rect(16, top + row_height(l, i) / 2, l.w - 32, 1, rgb(0x46484d));
                continue;
            }
            if (i == l.index && it.enabled)
                c.fill_round_rect(6, top + 2, l.w - 12, MENU_ROW_H - 4, 8, MENU_HOVER);
            const Color text = it.enabled ? MENU_TEXT : MENU_DIM;
            const int base = top + MENU_ROW_H / 2 + 8;
            if (it.kind == Item::Check && it.on) {
                c.line(18, top + 20, 24, top + 27, 3, MENU_ACCENT);
                c.line(24, top + 27, 34, top + 13, 3, MENU_ACCENT);
            } else if (it.kind == Item::Radio && it.on) {
                c.fill_circle(26, top + MENU_ROW_H / 2.0f, 6, MENU_ACCENT);
            }
            c.text(48, base, it.label, 21, text, Weight::Regular, l.w - 48 - 40);
            if (!it.hint.empty()) {
                const int hw = c.text_width(it.hint, 18);
                c.text(l.w - hw - (it.kind == Item::Sub ? 40 : 18), base - 1, it.hint, 18, MENU_DIM);
            }
            if (it.kind == Item::Sub) {
                c.line(l.w - 24, top + 13, l.w - 17, top + MENU_ROW_H / 2.0f, 2.5f, text);
                c.line(l.w - 17, top + MENU_ROW_H / 2.0f, l.w - 24, top + MENU_ROW_H - 13, 2.5f, text);
            }
        }
        if (scrolling(l)) {
            // where the list is: a thin bar at the right
            const int track = l.h - 2 * MENU_PAD, n = static_cast<int>(l.items.size());
            const int bar_h = std::max(30, track * l.rows / std::max(1, n));
            const int bar_y = MENU_PAD + (track - bar_h) * l.scroll / std::max(1, n - l.rows);
            c.fill_round_rect(l.w - 7, bar_y, 4, bar_h, 2, rgb(0x6b6e73));
        }
        put_canvas(bgra, stride, c, x0 + l.x, y0 + l.y, 255);
    }
}

bool menu_input(const shell::Input &in, const std::vector<shell::KeyEvent> &keys, float px, float py)
{
    // the pointer: the deepest level under it takes it (a submenu opens as it is pointed at)
    if (in.moved && in.mouse) {
        for (int k = static_cast<int>(M.levels.size()) - 1; k >= 0; k--) {
            const int row = row_at(M.levels[k], px, py);
            if (row < 0)
                continue;
            Level &l = M.levels[k];
            if (l.index != row || static_cast<int>(M.levels.size()) > k + 1) {
                l.index = row;
                M.levels.resize(static_cast<size_t>(k) + 1);
                if (selectable(l.items[row]) && l.items[row].kind == Item::Sub)
                    menu_open_sub(static_cast<size_t>(k), row, false);
                P.dirty = true;
            }
            break;
        }
    }
    if (in.scroll != 0 && !M.levels.empty()) {
        Level &l = M.levels.back();
        if (scrolling(l)) {
            l.scroll = std::clamp(l.scroll + (in.scroll > 0 ? 3 : -3), 0, static_cast<int>(l.items.size()) - l.rows);
            P.dirty = true;
        }
    }
    if (in.pressed & shell::BTN_CLICK) {
        if (in.mouse) {
            for (int k = static_cast<int>(M.levels.size()) - 1; k >= 0; k--) {
                const int row = row_at(M.levels[k], px, py);
                if (row >= 0) {
                    menu_activate(static_cast<size_t>(k), row);
                    return true;
                }
            }
            menu_close();   // (a click elsewhere)
            return true;
        }
        if (!M.levels.empty())
            menu_activate(M.levels.size() - 1, M.levels.back().index);
        return true;
    }
    if (in.pressed & shell::BTN_MOUSE_RIGHT) {
        menu_open_at(px, py);
        return true;
    }
    if ((in.pressed & shell::BTN_MENU) || (in.pressed & shell::BTN_START)) {
        menu_close();
        return true;
    }
    if (in.repeat & shell::BTN_UP)
        menu_move(-1);
    if (in.repeat & shell::BTN_DOWN)
        menu_move(+1);
    if ((in.repeat & shell::BTN_RIGHT) && !M.levels.empty()) {
        const Level &l = M.levels.back();
        if (l.index >= 0 && l.items[l.index].kind == Item::Sub)
            menu_open_sub(M.levels.size() - 1, l.index, true);
    }
    if ((in.repeat & shell::BTN_LEFT) || (in.pressed & shell::BTN_BACK)) {
        if (M.levels.size() > 1) {
            M.levels.pop_back();
            P.dirty = true;
        } else {
            menu_close();
        }
    }
    for (const shell::KeyEvent &key : keys) {
        if (!key.down)
            continue;
        switch (key.vkey) {
        case 0x26: menu_move(-1); break;
        case 0x28: menu_move(+1); break;
        case 0x27:
            if (!M.levels.empty() && M.levels.back().index >= 0 && M.levels.back().items[M.levels.back().index].kind == Item::Sub)
                menu_open_sub(M.levels.size() - 1, M.levels.back().index, true);
            break;
        case 0x25: case 0x08:
            if (M.levels.size() > 1) {
                M.levels.pop_back();
                P.dirty = true;
            } else {
                menu_close();
            }
            break;
        case 0x0D: case 0x20:
            if (!M.levels.empty())
                menu_activate(M.levels.size() - 1, M.levels.back().index);
            break;
        case 0x1B: case 0x5D: menu_close(); break;
        }
    }
    return true;
}

// --- The picker (Load subtitle file, Open file) ---------------------------------------------------------------
bool is_archive_file(const std::string &name)
{
    const std::string ext = extension(name);
    return ext == "zip" || ext == "rar" || ext == "7z";
}

bool picker_takes(const std::string &name)
{
    if (K.subtitles)
        return is_subtitle_name(name)
#if defined(WISP_HAVE_LIBARCHIVE)
            || is_archive_file(name)
#endif
            ;
    return can_play(name);
}

void picker_list(const std::string &folder, bool archive)
{
    K.entries.clear();
    K.index = 0;
    K.scroll = 0;
    K.folder = folder;
    K.in_archive = archive;
    P.dirty = true;
    if (folder.empty()) {
        // the places: the video's folder, Downloads, the console's data, USB drives
        const size_t slash = P.path.find_last_of('/');
        std::vector<std::pair<std::string, std::string>> places;
        if (slash != std::string::npos && slash > 0)
            places.push_back({ "This video's folder", P.path.substr(0, slash) });
        places.push_back({ "Downloads", wisp_download_folder() });
        places.push_back({ "Data (/data)", "/data" });
        for (int i = 0; i < 8; i++) {
            const std::string usb = "/mnt/usb" + std::to_string(i);
            struct stat st;
            if (stat(usb.c_str(), &st) == 0 && S_ISDIR(st.st_mode))
                places.push_back({ "USB drive " + std::to_string(i) + " (" + usb + ")", usb });
        }
        for (auto &[name, path] : places) {
            PickEntry e;
            e.name = name;
            e.path = path;
            e.dir = true;
            e.place = true;
            K.entries.push_back(e);
        }
        return;
    }
    PickEntry up;
    up.name = "..";
    up.up = true;
    up.dir = true;
    K.entries.push_back(up);
#if defined(WISP_HAVE_LIBARCHIVE)
    if (archive) {
        std::vector<std::string> names;
        archive_walk(folder, "", &names, nullptr);
        std::sort(names.begin(), names.end());
        for (const std::string &n : names) {
            PickEntry e;
            e.name = n;
            e.path = folder;
            e.inside = n;
            K.entries.push_back(e);
        }
        return;
    }
#endif
    std::vector<PickEntry> dirs, files;
    if (DIR *dir = opendir(folder.c_str())) {
        while (dirent *d = readdir(dir)) {
            if (d->d_name[0] == '.')
                continue;
            const std::string path = folder + "/" + d->d_name;
            struct stat st;
            if (stat(path.c_str(), &st) != 0)
                continue;
            PickEntry e;
            e.name = d->d_name;
            e.path = path;
            if (S_ISDIR(st.st_mode)) {
                e.dir = true;
                dirs.push_back(e);
            } else if (picker_takes(e.name)) {
                e.size = st.st_size;
                e.archive = K.subtitles && is_archive_file(e.name);
                files.push_back(e);
            }
        }
        closedir(dir);
    }
    const auto by_name = [](const PickEntry &a, const PickEntry &b) { return lower(a.name) < lower(b.name); };
    std::sort(dirs.begin(), dirs.end(), by_name);
    std::sort(files.begin(), files.end(), by_name);
    K.entries.insert(K.entries.end(), dirs.begin(), dirs.end());
    K.entries.insert(K.entries.end(), files.begin(), files.end());
    // the video's own name first among subtitles (the likeliest one)
    if (K.subtitles && K.entries.size() > 1)
        K.index = 1;
}

void open_picker(bool subtitles)
{
    K = Picker();
    K.open = true;
    K.subtitles = subtitles;
    K.title = subtitles ? "Load subtitle file (.srt, .ass, .ssa, .vtt; or a zip, rar or 7z of them)" : "Open file";
    const size_t slash = P.path.find_last_of('/');
    picker_list(slash != std::string::npos && slash > 0 ? P.path.substr(0, slash) : wisp_download_folder(), false);
    menu_close();
}

void picker_close()
{
    K.open = false;
    K.entries.clear();
    P.dirty = true;
}

void picker_up()
{
    if (K.folder.empty()) {
        picker_close();
        return;
    }
    if (K.in_archive) {
        const std::string archive = K.folder;
        const size_t slash = archive.find_last_of('/');
        picker_list(slash != std::string::npos && slash > 0 ? archive.substr(0, slash) : "/", false);
        return;
    }
    if (K.folder == "/data" || K.folder == "/" || (K.folder.rfind("/mnt/usb", 0) == 0 && K.folder.find('/', 5) == std::string::npos)) {
        picker_list("", false);
        return;
    }
    const std::string from = K.folder;
    const size_t slash = from.find_last_of('/');
    picker_list(slash != std::string::npos && slash > 0 ? from.substr(0, slash) : "/", false);
    // (the folder it came from chosen)
    for (int i = 0; i < static_cast<int>(K.entries.size()); i++)
        if (K.entries[i].path == from)
            K.index = i;
}

void picker_choose(int index)
{
    if (index < 0 || index >= static_cast<int>(K.entries.size()))
        return;
    const PickEntry e = K.entries[index];
    if (e.up) {
        picker_up();
        return;
    }
    if (e.dir) {
        picker_list(e.path, false);
        return;
    }
    if (e.archive) {
        picker_list(e.path, true);
        if (K.entries.size() <= 1)
            say("No subtitles files in " + e.name);
        else if (K.entries.size() == 2)
            K.index = 1;
        return;
    }
    const bool subtitles = K.subtitles;
    picker_close();
    if (subtitles)
        use_subtitle_file(e.path, e.inside);
    else
        open(e.path);
}

constexpr int PICK_ROW_H = 46;

void draw_picker(uint32_t *bgra, int stride, int x0, int y0, int area_w, int area_h)
{
    K.w = std::min(1280, area_w - 80);
    K.h = std::min(area_h - 60, 900);
    K.x = (area_w - K.w) / 2;
    K.y = (area_h - K.h) / 2;
    Canvas c(K.w, K.h);
    c.fill_round_rect(0, 0, K.w, K.h, 16, rgb(0x232427, 250));
    c.text(32, 52, K.title, 25, MENU_TEXT, Weight::Bold, K.w - 64);
    const std::string where = K.folder.empty() ? "Places" : K.in_archive ? K.folder + " (an archive)" : K.folder;
    c.text(32, 90, where, 19, MENU_DIM, Weight::Regular, K.w - 64);
    K.list_y = 112;
    const int footer = 52;
    K.rows = std::max(3, (K.h - K.list_y - footer) / PICK_ROW_H);
    if (K.index < K.scroll)
        K.scroll = K.index;
    if (K.index >= K.scroll + K.rows)
        K.scroll = K.index - K.rows + 1;
    K.scroll = std::clamp(K.scroll, 0, std::max(0, static_cast<int>(K.entries.size()) - K.rows));
    for (int r = 0; r < K.rows; r++) {
        const int i = K.scroll + r;
        if (i >= static_cast<int>(K.entries.size()))
            break;
        const PickEntry &e = K.entries[i];
        const int top = K.list_y + r * PICK_ROW_H;
        if (i == K.index)
            c.fill_round_rect(16, top + 2, K.w - 32, PICK_ROW_H - 4, 9, MENU_HOVER);
        // an icon: a folder (amber), an archive (violet), a file (grey)
        const Color icon = e.up ? MENU_DIM : e.dir ? rgb(0xf6c344) : e.archive ? rgb(0xb388ff) : !e.inside.empty() || is_subtitle_name(e.name) ? rgb(0x8ab4f8) : rgb(0x9aa0a6);
        if (e.dir && !e.up) {
            c.fill_round_rect(34, top + 14, 12, 6, 2, icon);
            c.fill_round_rect(34, top + 17, 26, 17, 3, icon);
        } else if (e.up) {
            c.line(40, top + 30, 47, top + 16, 3, icon);
            c.line(47, top + 16, 54, top + 30, 3, icon);
        } else {
            c.fill_round_rect(38, top + 11, 20, 25, 3, icon);
            if (e.archive)
                for (int k = 0; k < 4; k++)
                    c.fill_rect(46, top + 13 + k * 6, 4, 3, rgb(0x232427));
        }
        c.text(76, top + PICK_ROW_H / 2 + 8, e.up ? "Up a folder" : e.name, 21, MENU_TEXT, Weight::Regular, K.w - 76 - 170);
        if (e.size > 0) {
            char size[32];
            if (e.size >= (1 << 20))
                snprintf(size, sizeof size, "%.1f MB", e.size / 1048576.0);
            else
                snprintf(size, sizeof size, "%lld KB", static_cast<long long>((e.size + 1023) / 1024));
            c.text(K.w - 32 - c.text_width(size, 18), top + PICK_ROW_H / 2 + 7, size, 18, MENU_DIM);
        }
    }
    if (K.entries.empty() || (K.entries.size() == 1 && K.entries[0].up))
        c.text(76, K.list_y + PICK_ROW_H + 30, K.subtitles ? "No subtitles files here." : "No video or music files here.", 20, MENU_DIM);
    c.text(32, K.h - 20, "Cross: open   Circle: up a folder   Square: close   (Places: Circle at the top)", 18, MENU_DIM, Weight::Regular, K.w - 64);
    put_canvas(bgra, stride, c, x0 + K.x, y0 + K.y, 255);
}

bool picker_input(const shell::Input &in, const std::vector<shell::KeyEvent> &keys, float px, float py)
{
    const int n = static_cast<int>(K.entries.size());
    auto row_under = [&](float x, float y) -> int {
        if (x < K.x + 16 || x >= K.x + K.w - 16 || y < K.y + K.list_y)
            return -1;
        const int r = static_cast<int>((y - K.y - K.list_y) / PICK_ROW_H);
        const int i = K.scroll + r;
        return r < K.rows && i < n ? i : -1;
    };
    if (in.moved && in.mouse) {
        const int i = row_under(px, py);
        if (i >= 0 && i != K.index) {
            K.index = i;
            P.dirty = true;
        }
    }
    if (in.scroll != 0) {
        K.scroll = std::clamp(K.scroll + (in.scroll > 0 ? 3 : -3), 0, std::max(0, n - K.rows));
        K.index = std::clamp(K.index, K.scroll, std::max(K.scroll, std::min(n - 1, K.scroll + K.rows - 1)));
        P.dirty = true;
    }
    if (in.pressed & shell::BTN_CLICK) {
        if (in.mouse) {
            const int i = row_under(px, py);
            if (i >= 0)
                picker_choose(i);
            else if (px < K.x || px >= K.x + K.w || py < K.y || py >= K.y + K.h)
                picker_close();
        } else {
            picker_choose(K.index);
        }
        return true;
    }
    if (in.pressed & shell::BTN_BACK) {
        picker_up();
        return true;
    }
    if (in.pressed & (shell::BTN_MENU | shell::BTN_START)) {
        picker_close();
        return true;
    }
    if (in.repeat & shell::BTN_UP) {
        K.index = std::max(0, K.index - 1);
        P.dirty = true;
    }
    if (in.repeat & shell::BTN_DOWN) {
        K.index = std::min(n - 1, K.index + 1);
        P.dirty = true;
    }
    if (in.repeat & shell::BTN_LEFT) {
        K.index = std::max(0, K.index - K.rows);
        P.dirty = true;
    }
    if (in.repeat & shell::BTN_RIGHT) {
        K.index = std::min(n - 1, K.index + K.rows);
        P.dirty = true;
    }
    for (const shell::KeyEvent &key : keys) {
        if (!key.down)
            continue;
        switch (key.vkey) {
        case 0x26: K.index = std::max(0, K.index - 1); break;
        case 0x28: K.index = std::min(n - 1, K.index + 1); break;
        case 0x21: K.index = std::max(0, K.index - K.rows); break;
        case 0x22: K.index = std::min(n - 1, K.index + K.rows); break;
        case 0x24: K.index = 0; break;
        case 0x23: K.index = n - 1; break;
        case 0x0D: picker_choose(K.index); break;
        case 0x08: picker_up(); break;
        case 0x1B: picker_close(); break;
        }
        P.dirty = true;
    }
    return true;
}

// --- The information panel (MPC-HC's Statistics and Properties) ------------------------------------------------
void draw_info(uint32_t *bgra, int stride, int x0, int y0, int area_w)
{
    std::vector<std::pair<std::string, std::string>> rows;
    {
        std::lock_guard<std::mutex> guard(X.lock);
        char text[256];
        rows.push_back({ "File", P.name });
        if (!X.container.empty())
            rows.push_back({ "Container", X.container });
        snprintf(text, sizeof text, "%s of %s", clock_text(clock_locked()).c_str(), X.duration > 0 ? clock_text(X.duration).c_str() : "?");
        rows.push_back({ "Time", text });
        if (X.bitrate > 0) {
            snprintf(text, sizeof text, "%lld kb/s", static_cast<long long>(X.bitrate / 1000));
            rows.push_back({ "Bit rate", text });
        }
        if (X.has_video) {
            snprintf(text, sizeof text, "%s, %dx%d, %s, %.3g frames a second", X.video_codec.c_str(), X.video_w, X.video_h,
                X.pixel_format.c_str(), X.fps);
            rows.push_back({ "Video", text });
            snprintf(text, sizeof text, "%llu shown, %llu late (dropped)", static_cast<unsigned long long>(X.shown_frames),
                static_cast<unsigned long long>(X.dropped_frames));
            rows.push_back({ "Frames", text });
            snprintf(text, sizeof text, "%s, %s, zoom %d%%", X.view.aspect ? ASPECTS[X.view.aspect].name : "the video's shape",
                FRAME_MODES[X.view.frame], static_cast<int>(std::lround(X.view.zoom * 100)));
            rows.push_back({ "Picture", text });
        }
        if (X.has_audio) {
            snprintf(text, sizeof text, "%s, %d Hz, %d channels (to stereo), delay %s, speed %s", X.audio_codec.c_str(), X.audio_rate,
                X.audio_channels, signed_seconds(X.audio_delay).c_str(), speed_text(X.speed).c_str());
            rows.push_back({ "Audio", text });
            if (X.audio_now >= 0 && X.audio_now < static_cast<int>(X.audio_names.size()))
                rows.push_back({ "Audio track", X.audio_names[X.audio_now] });
        }
        rows.push_back({ "Subtitles", X.sub_now >= 0 && X.sub_now < static_cast<int>(X.sub_names.size()) ? X.sub_names[X.sub_now] + ", delay " + signed_seconds(P.sub_delay) : "off" });
        snprintf(text, sizeof text, "%.2f s of sound ready, %zu pictures ready", X.sound_seconds, X.pictures.size());
        rows.push_back({ "Buffers", text });
    }
    const int w = std::min(1100, area_w - 80), row = 32, h = 30 + static_cast<int>(rows.size()) * row;
    Canvas c(w, h);
    c.fill_round_rect(0, 0, w, h, 14, rgb(0x101114, 215));
    int y = 38;
    for (auto &[name, value] : rows) {
        c.text(24, y, name, 18, MENU_DIM, Weight::Bold);
        c.text(200, y, value, 18, MENU_TEXT, Weight::Regular, w - 224);
        y += row;
    }
    put_canvas(bgra, stride, c, x0 + 40, y0 + 40, 255);
}

// darkens a rectangle of the screen picture (subtitles' box)
void shade(uint32_t *bgra, int stride, int clip_x, int clip_y, int clip_w, int clip_h, int x, int y, int w, int h, int keep)
{
    const int x1 = std::max(x, clip_x), y1 = std::max(y, clip_y), x2 = std::min(x + w, clip_x + clip_w), y2 = std::min(y + h, clip_y + clip_h);
    for (int yy = y1; yy < y2; yy++) {
        uint32_t *row = bgra + static_cast<size_t>(yy) * stride;
        for (int xx = x1; xx < x2; xx++) {
            const uint32_t p = row[xx];
            row[xx] = 0xff000000u | ((((p >> 16) & 0xff) * keep / 255) << 16) | ((((p >> 8) & 0xff) * keep / 255) << 8) | ((p & 0xff) * keep / 255);
        }
    }
}

} // namespace

// --- Public -------------------------------------------------------------------------------------------------

void set_host(Host host) { H = std::move(host); }

bool can_play(const std::string &path)
{
    static const char *const kinds[] = { "mp4", "m4v", "mkv", "webm", "avi", "mov", "ts", "m2ts", "mts", "mpg", "mpeg", "vob",
        "flv", "ogv", "wmv", "3gp", "divx", "mp3", "m4a", "aac", "flac", "ogg", "oga", "opus", "wav", "wma", "ac3", "mka", "dts" };
    const std::string ext = extension(path);
    for (const char *kind : kinds)
        if (ext == kind)
            return true;
    return false;
}

bool open(const std::string &file)
{
    if (P.visible || threads_up)
        close();
    {
        std::lock_guard<std::mutex> guard(X.lock);
        X.quit = false;
        X.seek_to = -1;
        X.want_audio = -1;
        X.want_sub = -2;
        X.pictures.clear();
        X.spare.clear();
        X.sound.clear();
        X.sound_seconds = 0;
        X.subtitles.clear();
        X.at_end = false;
        X.error.clear();
        X.duration = 0;
        X.has_video = X.has_audio = false;
        X.audio_names.clear();
        X.sub_names.clear();
        X.audio_now = X.sub_now = -1;
        X.clock_pts = 0;
        X.clock_at = now_s();
        X.paused = false;
        X.area_w = P.w;
        X.area_h = P.h;
        X.volume = std::clamp(wisp_setting_int("player_volume", 100), 0, 100);
        X.boost = std::clamp(wisp_setting_int("player_boost", 100), 100, 400);
        X.speed = 1.0;
        X.audio_delay = 0;
        X.external_ready = false;
        X.external.clear();
        X.chapters.clear();
        X.sub_file_beside.clear();
        X.shown_frames = X.dropped_frames = 0;
    }
    const int area_w = P.w, area_h = P.h;
    const bool info = P.info;
    P = Page { true, file, file.substr(file.find_last_of('/') + 1) };
    P.w = area_w, P.h = area_h;
    P.info = info;
    load_settings();
    {
        std::lock_guard<std::mutex> guard(X.lock);
        X.sub_encoding = P.encoding;
    }
    menu_close();
    K = Picker();
    recent_add(file);
    P.osd_until = now_s() + 4;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, static_cast<size_t>(1) << 20);
    const bool decode = pthread_create(&decode_thread, &attr, decode_main, new std::string(file)) == 0;
    pthread_attr_setstacksize(&attr, static_cast<size_t>(256) << 10);
    const bool audio = decode && pthread_create(&audio_thread, &attr, audio_main, nullptr) == 0;
    pthread_attr_destroy(&attr);
    if (!decode || !audio) {
        if (decode) {
            std::lock_guard<std::mutex> guard(X.lock);
            X.quit = true;
        }
        if (decode)
            pthread_join(decode_thread, nullptr);
        P.visible = false;
        if (H.toast)
            H.toast("The player could not start");
        return false;
    }
    threads_up = true;
    // where it was left
    const std::map<std::string, double> resume = resume_read();
    auto it = resume.find(file);
    if (it != resume.end() && it->second > 10) {
        seek_to(it->second);
        if (H.toast)
            H.toast("Resumed at " + clock_text(it->second) + " (Home: from the start)");
    }
    wisp_log("player: playing %s", file.c_str());
    return true;
}

void show()
{
    P.visible = true;
    P.dirty = true;
    P.osd_until = now_s() + 3;
}

void hide()
{
    // a video pauses while its tab is hidden; music goes on
    std::lock_guard<std::mutex> guard(X.lock);
    if (X.has_video && !X.paused) {
        X.clock_pts = clock_locked();
        X.paused = true;
    }
    P.visible = false;
}

bool visible() { return P.visible; }

void close()
{
    save_position();
    stop_threads();
    P.visible = false;
    P.shown = Picture();
    P.path.clear();
    P.name.clear();
}

const std::string &path() { return P.path; }

std::string title() { return P.name.empty() ? std::string("Player") : P.name; }

bool dirty()
{
    if (!threads_up)
        return false;
    const double t = now_s();
    bool due = false;
    std::string error;
    bool ended = false;
    {
        std::lock_guard<std::mutex> guard(X.lock);
        const double clock = clock_locked();
        // the newest picture whose time has come; the older ones go back
        while (!X.pictures.empty() && X.pictures.front().pts <= clock + 0.008) {
            if (!P.shown.pixels.empty())
                X.spare.push_back(std::move(P.shown));
            P.shown = std::move(X.pictures.front());
            X.pictures.pop_front();
            due = true;
        }
        // (no sound: the wall's clock runs on its own; a picture stuck far ahead of it means a jump)
        if (!X.has_audio && !X.pictures.empty() && X.pictures.front().pts > clock + 2 && !X.paused) {
            X.clock_pts = X.pictures.front().pts;
            X.clock_at = t;
        }
        error = X.error;
        ended = X.at_end;
        X.error.clear();
    }
    if (due)
        X.wake.notify_all();
    if (!error.empty() && H.toast)
        H.toast("The player: " + error);
    if (ended && !P.closing) {
        // the end (Play > When it ends): the folder's next file (else the player closes), this one again,
        // a stop there, or the player closes
        P.closing = true;
        resume_write(P.path, 0);
        if (error.empty() && P.after == 1) {
            seek_to(0);
            say("Again");
            return true;
        }
        if (error.empty() && P.after == 2) {
            std::lock_guard<std::mutex> guard(X.lock);
            X.clock_pts = clock_locked();
            X.paused = true;
            say("The end");
            P.dirty = true;
            return true;
        }
        const std::string next = P.after == 0 ? neighbour(+1) : "";
        if (!next.empty() && error.empty()) {
            open(next);
            if (H.redraw)
                H.redraw();
            return true;
        }
        if (H.closed)
            H.closed();
        return true;
    }
    // where it is, every 15 s (a sudden end of the app keeps it)
    if (t - P.last_saved > 15) {
        P.last_saved = t;
        save_position();
    }
    const bool osd = t < P.osd_until || t < P.message_until;
    static bool osd_was = false;
    const bool osd_changed = osd != osd_was;
    osd_was = osd;
    if (!P.visible)
        return false;
    if (due || osd_changed || P.dirty || ((osd || P.info) && static_cast<int>(t * 4) != static_cast<int>((t - 0.017) * 4)))
        return true;
    return false;
}

void paint(uint32_t *bgra, int stride, int x, int y, int w, int h)
{
    P.dirty = false;
    if (w != P.w || h != P.h) {
        P.w = w, P.h = h;
        std::lock_guard<std::mutex> guard(X.lock);
        X.area_w = w;
        X.area_h = h;
    }
    P.x = x, P.y = y;
    // the picture, letterboxed in black
    const Picture &pic = P.shown;
    const int px = x + (w - pic.w) / 2, py = y + (h - pic.h) / 2;
    for (int row = 0; row < h; row++) {
        uint32_t *dst = bgra + static_cast<size_t>(y + row) * stride + x;
        const int src_row = y + row - py;
        if (pic.pixels.empty() || src_row < 0 || src_row >= pic.h) {
            std::fill_n(dst, w, 0xff000000u);
            continue;
        }
        const int left = std::max(0, px - x), right = std::max(0, x + w - (px + pic.w));
        std::fill_n(dst, left, 0xff000000u);
        memcpy(dst + left, pic.pixels.data() + static_cast<size_t>(src_row) * pic.w + std::max(0, x - px),
            static_cast<size_t>(std::min(pic.w, w - left - right)) * 4);
        std::fill_n(dst + w - right, right, 0xff000000u);
    }
    double clock, duration;
    bool paused, has_video;
    int volume;
    bool muted;
    std::vector<Subtitle> now;
    std::string audio_name, sub_name, codec;
    {
        std::lock_guard<std::mutex> guard(X.lock);
        clock = clock_locked();
        duration = X.duration;
        paused = X.paused;
        has_video = X.has_video;
        volume = X.volume;
        muted = X.muted;
        // (the subtitles' delay: they show that much later)
        const double sub_clock = clock - P.sub_delay;
        for (const Subtitle &s : X.subtitles)
            if (sub_clock >= s.start && sub_clock < s.end)
                now.push_back(s);
        audio_name = X.audio_now >= 0 && X.audio_now < static_cast<int>(X.audio_names.size()) ? X.audio_names[X.audio_now] : "";
        sub_name = X.sub_now >= 0 && X.sub_now < static_cast<int>(X.sub_names.size()) ? X.sub_names[X.sub_now] : "";
        codec = X.video_codec;
    }
    const double t = now_s();
    const bool osd = t < P.osd_until || paused;
    // music: its name over black
    if (!has_video) {
        const ps4ui::TextSprite name = ps4ui::render_text(P.name, 34, rgb(0xe8eaed), Weight::Bold, w - 200, 0);
        blend_sprite(bgra, stride, x, y, w, h, name.image, x + (w - name.image.width) / 2, y + h / 2 - 60);
        const ps4ui::TextSprite when = ps4ui::render_text(clock_text(clock) + (duration > 0 ? " / " + clock_text(duration) : ""), 26,
            rgb(0x9aa0a6), Weight::Regular, w - 200, 0);
        blend_sprite(bgra, stride, x, y, w, h, when.image, x + (w - when.image.width) / 2, y + h / 2);
    }
    // subtitles: text with a dark outline (the text drawn shadowed), at the bottom (Subtitles > Position
    // moves them, Size scales them, a box behind them if asked); pictures where the video puts them
    int bottom = y + h - (osd ? 150 : 60) - P.sub_lift;
    const int sub_px = std::max(14, 40 * P.sub_size / 100);
    for (auto it = now.rbegin(); it != now.rend(); ++it) {
        const Subtitle &s = *it;
        if (!s.bitmap.empty() && s.frame_w > 0 && !pic.pixels.empty()) {
            const double scale = static_cast<double>(pic.w) / s.frame_w;
            ps4ui::Image image;
            image.width = std::max(1, static_cast<int>(s.w * scale));
            image.height = std::max(1, static_cast<int>(s.h * scale));
            image.pixels.resize(static_cast<size_t>(image.width) * image.height);
            for (int yy = 0; yy < image.height; yy++)
                for (int xx = 0; xx < image.width; xx++) {
                    const uint32_t p = s.bitmap[static_cast<size_t>(std::min(s.h - 1, static_cast<int>(yy / scale))) * s.w
                        + std::min(s.w - 1, static_cast<int>(xx / scale))];
                    // premultiplied ARGB to the sprite's RGBA
                    image.pixels[static_cast<size_t>(yy) * image.width + xx] = (p & 0xff000000u) | ((p >> 16) & 0xff) | (p & 0xff00) | ((p & 0xff) << 16);
                }
            blend_sprite(bgra, stride, x, y, w, h, image, px + static_cast<int>(s.x * scale), py + static_cast<int>(s.y * scale));
            continue;
        }
        if (s.text.empty())
            continue;
        // each line on its own, the last one lowest
        std::vector<std::string> lines;
        size_t start = 0;
        while (start <= s.text.size()) {
            const size_t nl = s.text.find('\n', start);
            lines.push_back(s.text.substr(start, nl == std::string::npos ? std::string::npos : nl - start));
            if (nl == std::string::npos)
                break;
            start = nl + 1;
        }
        for (auto line = lines.rbegin(); line != lines.rend(); ++line) {
            if (line->empty())
                continue;
            const ps4ui::TextSprite sprite = ps4ui::render_text(*line, sub_px, rgb(0xffffff), Weight::Bold, w - 160, 230);
            bottom -= sprite.image.height - 6;
            const int sx = x + (w - sprite.image.width) / 2;
            if (P.sub_box)
                shade(bgra, stride, x, y, w, h, sx - 12, bottom + 2, sprite.image.width + 24, sprite.image.height - 4, 70);
            blend_sprite(bgra, stride, x, y, w, h, sprite.image, sx, bottom);
        }
        bottom -= 10;
    }
    // the bar: play / pause, the times, where it is, the volume, the tracks
    if (osd) {
        Canvas bar(w, 110);
        bar.fill_rect(0, 0, w, 110, rgb(0x16171a, 225));
        const int cx = 50, cy = 46;
        if (paused) {
            bar.line(cx - 8, cy - 16, cx - 8, cy + 16, 7, rgb(0xe8eaed));
            bar.line(cx + 8, cy - 16, cx + 8, cy + 16, 7, rgb(0xe8eaed));
        } else {
            for (int k = 0; k < 30; k++)
                bar.line(cx - 12, cy - 15 + k * 0.5f, cx + 16, cy, 1.2f, rgb(0xe8eaed));
            bar.line(cx - 12, cy - 15, cx - 12, cy + 15, 2, rgb(0xe8eaed));
        }
        const std::string times = clock_text(clock) + (duration > 0 ? " / " + clock_text(duration) : "");
        bar.text(100, 56, times, 24, rgb(0xe8eaed), Weight::Bold);
        const int seek_x = 100 + bar.text_width(times, 24, Weight::Bold) + 40, seek_w = w - seek_x - 260;
        P.seek_x = seek_x;
        P.seek_w = seek_w;
        bar.fill_round_rect(seek_x, 40, seek_w, 12, 6, rgb(0x3a3b40));
        if (duration > 0)
            bar.fill_round_rect(seek_x, 40, std::max(12, static_cast<int>(seek_w * std::clamp(clock / duration, 0.0, 1.0))), 12, 6, rgb(0x8ab4f8));
        const std::string vol = muted ? "Muted" : "Volume " + std::to_string(volume) + "%";
        bar.text(w - 230, 56, vol, 21, muted ? rgb(0xf28b82) : rgb(0x9aa0a6));
        std::string tracks = P.name;
        if (!codec.empty())
            tracks += "  ·  " + codec;
        if (!audio_name.empty())
            tracks += "  ·  Audio: " + audio_name;
        tracks += "  ·  Subtitles: " + (sub_name.empty() ? std::string("off") : sub_name);
        bar.text(100, 92, tracks, 17, rgb(0x9aa0a6), Weight::Regular, w - 420);
        const char *hint = "Square or right-click: the menu";
        bar.text(w - 30 - bar.text_width(hint, 17), 92, hint, 17, rgb(0x8ab4f8));
        put_canvas(bgra, stride, bar, x, y + h - 110, 255);
        // (the bar's seek line, for clicks: in screen coordinates)
        P.osd_until = std::max(P.osd_until, paused ? t + 0.1 : P.osd_until);
    }
    if (t < P.message_until && !P.message.empty()) {
        const ps4ui::TextSprite sprite = ps4ui::render_text(P.message, 36, rgb(0xffffff), Weight::Bold, w - 200, 200);
        blend_sprite(bgra, stride, x, y, w, h, sprite.image, x + 60, y + (P.info ? h - 260 : 50));
    }
    // over it all: the information panel, the picker, the menu
    if (P.info)
        draw_info(bgra, stride, x, y, w);
    if (K.open)
        draw_picker(bgra, stride, x, y, w, h);
    if (M.open)
        draw_menu(bgra, stride, x, y, w, h);
}

bool input(const shell::Input &in, const std::vector<shell::KeyEvent> &keys, int x, int y, int w, int h, float)
{
    if (!P.visible)
        return false;
    const float px = in.x - x, py = in.y - y;
    const bool inside = px >= 0 && py >= 0 && px < w && py < h;
    // the USB keyboard's modifiers (Ctrl + a key: MPC-HC's shortcuts)
    for (const shell::KeyEvent &key : keys) {
        if (key.vkey == 0x11 || key.vkey == 0xA2 || key.vkey == 0xA3)
            P.ctrl = key.down;
        if (key.vkey == 0x10 || key.vkey == 0xA0 || key.vkey == 0xA1)
            P.shift = key.down;
    }
    // the picker and the menu take everything while they show
    if (K.open) {
        picker_input(in, keys, px, py);
        return true;
    }
    if (M.open)
        return menu_input(in, keys, px, py);
    bool used = false;
    auto volume_by = [](int step) {
        std::lock_guard<std::mutex> guard(X.lock);
        X.volume = std::clamp(X.volume + step, 0, 100);
        X.muted = false;
        wisp_setting_set("player_volume", std::to_string(X.volume).c_str());
        say("Volume " + std::to_string(X.volume) + "%" + (X.boost > 100 ? " (boost " + std::to_string(X.boost) + "%)" : ""));
    };
    auto next_audio = [] {
        std::lock_guard<std::mutex> guard(X.lock);
        if (X.audio_names.size() > 1) {
            X.want_audio = (X.audio_now + 1) % static_cast<int>(X.audio_names.size());
            say("Audio: " + X.audio_names[X.want_audio]);
        } else {
            say(X.audio_names.empty() ? "No audio" : "One audio track");
        }
        X.wake.notify_all();
    };
    auto next_sub = [] {
        std::lock_guard<std::mutex> guard(X.lock);
        const int n = static_cast<int>(X.sub_names.size());
        if (!n) {
            say("No subtitles (Square: the menu, Subtitles > Load subtitle file)");
            return;
        }
        const int next = X.sub_now + 1 >= n ? -1 : X.sub_now + 1;
        X.want_sub = next;
        say(next < 0 ? "Subtitles off" : "Subtitles: " + X.sub_names[next]);
        X.wake.notify_all();
    };
    auto cycle_aspect = [](int step) {
        const View v = view_now();
        const int next = (v.aspect + step + ASPECT_COUNT) % ASPECT_COUNT;
        change_view([next](View &w) { w.aspect = next; }, std::string("Aspect ratio: ") + ASPECTS[next].name);
    };
    auto cycle_frame = [] {
        const View v = view_now();
        const int next = (v.frame + 1) % FRAME_MODE_COUNT;
        change_view([next](View &w) { w.frame = next; }, FRAME_MODES[next]);
    };
    if (in.moved && inside) {
        P.osd_until = now_s() + 2.5;
        P.dirty = true;
    }
    // Square, or the mouse's right button: the menu (at the pointer; for the controller at the top left)
    if (in.pressed & shell::BTN_MENU) {
        const bool at_pointer = in.mouse && (in.pressed & shell::BTN_MOUSE_RIGHT) && inside;
        menu_open_at(at_pointer ? px : 60, at_pointer ? py : 60);
        return true;
    }
    if ((in.pressed & shell::BTN_CLICK) && inside) {
        used = true;
        const double t = now_s();
        // the bar's seek line: there
        const int seek_x = P.seek_x, seek_w = std::max(1, P.seek_w);
        double duration;
        {
            std::lock_guard<std::mutex> guard(X.lock);
            duration = X.duration;
        }
        if (t < P.osd_until && py >= h - 110 + 25 && py < h - 110 + 70 && px >= seek_x && px < seek_x + seek_w && duration > 0) {
            seek_to((px - seek_x) / seek_w * duration);
        } else if (in.mouse && t - P.last_click < 0.4) {
            toggle_fullscreen();   // (a double click)
            toggle_pause_now();    // (the first click paused it)
        } else {
            toggle_pause_now();
        }
        P.last_click = t;
        P.osd_until = t + 3;
    }
    if (in.repeat & shell::BTN_LEFT) { seek_by(-10); used = true; }
    if (in.repeat & shell::BTN_RIGHT) { seek_by(+10); used = true; }
    if (in.repeat & shell::BTN_UP) { volume_by(+5); used = true; }
    if (in.repeat & shell::BTN_DOWN) { volume_by(-5); used = true; }
    if (in.pressed & shell::BTN_SWITCH) { next_sub(); used = true; }
    if (in.pressed & shell::BTN_BACK) {
        used = true;
        if (H.closed)
            H.closed();
        return true;
    }
    if (in.scroll != 0 && inside && in.mouse) {
        volume_by(in.scroll < 0 ? +5 : -5);
        used = true;
    }
    for (const shell::KeyEvent &key : keys) {
        if (!key.down)
            continue;
        if (key.vkey == 0x11 || key.vkey == 0xA2 || key.vkey == 0xA3 || key.vkey == 0x10 || key.vkey == 0xA0 || key.vkey == 0xA1)
            continue;
        used = true;
        if (P.ctrl) {
            bool taken = true;
            switch (key.vkey) {
            case 'O': open_picker(false); break;                               // Ctrl+O: open a file
            case 'L': open_picker(true); break;                                // Ctrl+L: load a subtitles file
            case 'G': go_to_time(); break;                                     // Ctrl+G: go to
            case 'S': save_image(); break;                                     // Ctrl+S: save the picture
            case 'R': set_speed(1.0); break;                                   // Ctrl+R: the normal speed
            case 0x26: speed_step(+1); break;                                  // Ctrl+Up: faster
            case 0x28: speed_step(-1); break;                                  // Ctrl+Down: slower
            default: taken = false;
            }
            if (taken) {
                P.osd_until = now_s() + 3;
                P.dirty = true;
                continue;
            }
        }
        switch (key.vkey) {
        case 0x20: toggle_pause_now(); break;                              // Space
        case 0x25: seek_by(-5); break;                                     // Left
        case 0x27: seek_by(+5); break;                                     // Right
        case 0xBC: seek_by(-60); break;                                    // , (a minute back)
        case 0xBE: seek_by(+60); break;                                    // . (a minute on)
        case 0x26: volume_by(+5); break;                                   // Up
        case 0x28: volume_by(-5); break;                                   // Down
        case 'M': {
            std::lock_guard<std::mutex> guard(X.lock);
            X.muted = !X.muted;
            say(X.muted ? "Muted" : "Sound on");
            break;
        }
        case 'F': case 0x7A: toggle_fullscreen(); break;                   // F, F11
        case 'A': next_audio(); break;
        case 'S': next_sub(); break;
        case 'R': if (P.shift) cycle_frame(); else cycle_aspect(+1); break; // R: aspect ratio; Shift+R: the frame
        case 0xBB: set_audio_delay(audio_delay() + 0.1); break;           // = / +: the sound later
        case 0xBD: set_audio_delay(audio_delay() - 0.1); break;           // -: the sound sooner
        case 0x70: set_sub_delay(P.sub_delay - 0.1); break;               // F1: subtitles sooner
        case 0x71: set_sub_delay(P.sub_delay + 0.1); break;               // F2: subtitles later
        case 0x6B:                                                         // numpad +: zoom in
            change_view([](View &v) { v.zoom *= 1.1; }, "Zoom " + std::to_string(static_cast<int>(std::lround(std::min(4.0, view_now().zoom * 1.1) * 100))) + "%");
            break;
        case 0x6D:                                                         // numpad -: zoom out
            change_view([](View &v) { v.zoom /= 1.1; }, "Zoom " + std::to_string(static_cast<int>(std::lround(std::max(0.25, view_now().zoom / 1.1) * 100))) + "%");
            break;
        case 0x65: change_view([](View &v) { v.zoom = 1; }, "Zoom 100%"); break;   // numpad 5
        case 0x09: case 'I': P.info = !P.info; break;                      // Tab, I: the information panel
        case 0x5D: menu_open_at(60, 60); break;                            // the menu key
        case 0x24: seek_to(0); say("From the start"); break;               // Home
        case 0x21: go_file(-1); break;                                     // Page Up
        case 0x22: go_file(+1); break;                                     // Page Down
        case 0x1B:                                                         // Esc
            if (H.closed)
                H.closed();
            return true;
        default: used = false;
        }
        P.osd_until = now_s() + 3;
        P.dirty = true;
    }
    return used || inside;
}

} // namespace wisp::player

#else  // no FFmpeg in this build: the player takes nothing (files open in a tab, as before)

namespace wisp::player {
namespace {
std::string none;
}
void set_host(Host) { }
bool can_play(const std::string &) { return false; }
bool open(const std::string &) { return false; }
void show() { }
void hide() { }
bool visible() { return false; }
void close() { }
bool input(const shell::Input &, const std::vector<shell::KeyEvent> &, int, int, int, int, float) { return false; }
bool dirty() { return false; }
void paint(uint32_t *, int, int, int, int, int) { }
const std::string &path() { return none; }
std::string title() { return "Player"; }
} // namespace wisp::player

#endif
