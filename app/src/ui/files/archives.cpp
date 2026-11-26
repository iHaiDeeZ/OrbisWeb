// Wisp: archives for Files (archives.h). libarchive reads ZIP (with ZIP's passwords), 7-Zip, tar and its
// compressed forms, ISO 9660, CAB, LHA, cpio, ar, XAR, and single compressed files; it writes ZIP, 7-Zip
// and tar.gz. RAR goes to RARLAB's UnRAR library when it is in the build (WISP_HAVE_UNRAR, v00.27:
// encrypted RAR files and lists, which libarchive cannot decrypt); without it, to libarchive.
//
// Only the decompressors linked into the program are registered: libarchive falls back on an outside
// program (gzip, bzip2, lrzip, ...) for one that is not, and the PS4's fork() starts no process (it
// returns 0 in the app itself, which would then run the child's side).
//
// UnRAR's license (scripts/build-unrar.sh; the package has it as wisp/licenses/unrar.txt): UnRAR source
// code may be used in any software to handle RAR archives without limitations free of charge, but cannot
// be used to develop RAR (WinRAR) compatible archiver and to re-create RAR compression algorithm, which
// is proprietary. Distribution of modified UnRAR source code in separate form or as a part of other
// software is permitted, provided that full text of this paragraph, starting from "UnRAR source code"
// words, is included in license, or in documentation if license is not available, and in source code
// comments of resulting package. (OrbisWeb builds it unmodified.)
#include "archives.h"

#include "../../common/wisp.h"
#include "../shell/fileops.h"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <set>

#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(WISP_HAVE_LIBARCHIVE)
#include <archive.h>
#include <archive_entry.h>
#endif

#if defined(WISP_HAVE_LIBARCHIVE) && defined(WISP_HAVE_UNRAR)
// (last: on Unix, dll.hpp defines HANDLE, UINT, LPARAM, CALLBACK and PASCAL as macros)
#define _UNIX
#include <unrar/dll.hpp>
static_assert(sizeof(wchar_t) == 4, "UnRAR's wide strings are UTF-32 here");
#endif

namespace wisp::archive {
namespace {

std::string lower(std::string s) {
    for (char &c : s)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool ends_with(const std::string &s, const std::string &suffix) {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string base_of(const std::string &path) {
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string parent_of(const std::string &path) {
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? std::string() : slash == 0 ? "/" : path.substr(0, slash);
}

bool exists(const std::string &path) {
    struct stat st{};
    return lstat(path.c_str(), &st) == 0;
}

bool all_digits(const std::string &s) {
    return !s.empty() && std::all_of(s.begin(), s.end(), [](char c) { return c >= '0' && c <= '9'; });
}

// "name.part3.rar": its stem ("name"), number and digits' count
bool rar_part(const std::string &name, std::string &stem, int &number, size_t &width) {
    const std::string l = lower(name);
    if (!ends_with(l, ".rar"))
        return false;
    const size_t dot = l.rfind(".part", l.size() - 4);
    if (dot == std::string::npos)
        return false;
    const std::string digits = l.substr(dot + 5, l.size() - 4 - dot - 5);
    if (!all_digits(digits))
        return false;
    stem = name.substr(0, dot);
    number = std::atoi(digits.c_str());
    width = digits.size();
    return true;
}

// "name.r00" (an old-style RAR set's second part on): its stem
bool rar_old_part(const std::string &name, std::string &stem) {
    const std::string l = lower(name);
    if (l.size() < 5 || l[l.size() - 4] != '.' || (l[l.size() - 3] != 'r' && l[l.size() - 3] != 's') || !all_digits(l.substr(l.size() - 2)))
        return false;
    stem = name.substr(0, name.size() - 4);
    return true;
}

// "name.7z.001": its stem ("name.7z")
bool split_part(const std::string &name, std::string &stem) {
    const size_t dot = name.find_last_of('.');
    if (dot == std::string::npos || name.size() - dot - 1 != 3 || !all_digits(name.substr(dot + 1)))
        return false;
    stem = name.substr(0, dot);
    return true;
}

// a file packed alone (no tar inside): libarchive reads it in its "raw" format
bool single_stream(const std::string &file) {
    const std::string l = lower(base_of(file));
    static const char *const tars[] = {".tar.gz", ".tar.bz2", ".tar.xz", ".tar.zst", ".tar.lz4", ".tar.lzma", ".tar.lz", ".tar.z"};
    for (const char *t : tars)
        if (ends_with(l, t))
            return false;
    static const char *const singles[] = {".gz", ".bz2", ".xz", ".zst", ".lz4", ".lzma", ".lz", ".z"};
    return std::any_of(std::begin(singles), std::end(singles), [&](const char *e) { return ends_with(l, e); });
}

long long file_size(const std::string &path) {
    struct stat st{};
    return stat(path.c_str(), &st) == 0 ? static_cast<long long>(st.st_size) : 0;
}

// An entry's path made safe to write below a folder: '/'-separated, no leading '/', no "." or ".."
// parts (an archive can't put a file outside the folder it is taken out to)
std::string clean_path(const std::string &raw) {
    std::string out, part;
    const auto flush = [&] {
        if (!part.empty() && part != "." && part != "..")
            out += (out.empty() ? "" : "/") + part;
        part.clear();
    };
    for (char c : raw) {
        if (c == '/' || c == '\\')
            flush();
        else
            part += c;
    }
    flush();
    return out;
}

// USB drives are exFAT: names there can't hold these
std::string usb_safe(const std::string &path) {
    std::string out = path;
    for (char &c : out)
        if (c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|' || (static_cast<unsigned char>(c) < 0x20))
            c = '_';
    return out;
}

bool make_dirs(const std::string &path) {
    if (path.empty() || exists(path))
        return true;
    if (!make_dirs(parent_of(path)))
        return false;
    return mkdir(path.c_str(), 0777) == 0 || errno == EEXIST;
}

void remove_all(const std::string &path) {
    struct stat st{};
    if (lstat(path.c_str(), &st) != 0)
        return;
    if (S_ISDIR(st.st_mode)) {
        if (DIR *dir = opendir(path.c_str())) {
            std::vector<std::string> names;
            while (dirent *e = readdir(dir))
                if (std::strcmp(e->d_name, ".") && std::strcmp(e->d_name, ".."))
                    names.push_back(e->d_name);
            closedir(dir);
            for (const std::string &n : names)
                remove_all(path + "/" + n);
        }
        rmdir(path.c_str());
    } else {
        unlink(path.c_str());
    }
}

// A thread of 1 MiB (libarchive's decoders; the kit's default is 256 KiB)
bool start_thread(void *(*body)(void *), void *arg) {
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, static_cast<size_t>(1) << 20);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_t thread;
    const bool ok = pthread_create(&thread, &attr, body, arg) == 0;
    pthread_attr_destroy(&attr);
    return ok;
}

#if defined(WISP_HAVE_LIBARCHIVE)

std::string message(struct archive *a, const char *otherwise) {
    const char *m = archive_error_string(a);
    return m && *m ? m : otherwise;
}

// "Passphrase required for this entry", "Incorrect passphrase" (ZIP's; the other formats' encryption
// is not supported: their messages say so)
bool passphrase_error(const std::string &m) { return m.find("assphrase") != std::string::npos; }

struct Extraction;
#if defined(WISP_HAVE_UNRAR)
// RAR archives go to UnRAR (below: libarchive decrypts no RAR)
bool is_rar(const std::string &file);
std::shared_ptr<Index> rar_read_index(const std::string &file, const std::string &password);
std::string rar_extract(shell::JobControl &job, const Extraction &x);
#endif

// The decompressors whose libraries are in the program (see the top)
void support_filters(struct archive *a) {
    if (archive_zlib_version())
        archive_read_support_filter_gzip(a);
    if (archive_bzlib_version())
        archive_read_support_filter_bzip2(a);
    if (archive_liblzma_version()) {
        archive_read_support_filter_xz(a);
        archive_read_support_filter_lzma(a);
        archive_read_support_filter_lzip(a);
    }
    if (archive_libzstd_version())
        archive_read_support_filter_zstd(a);
    if (archive_liblz4_version())
        archive_read_support_filter_lz4(a);
    archive_read_support_filter_compress(a);  // (libarchive's own: .Z)
    archive_read_support_filter_uu(a);
    archive_read_support_filter_rpm(a);
}

struct archive *open_reader(const std::vector<std::string> &volumes, const std::string &password, bool raw, std::string &error) {
    struct archive *a = archive_read_new();
    if (!a) {
        error = "Not enough memory to open it";
        return nullptr;
    }
    support_filters(a);
    if (raw) {
        archive_read_support_format_raw(a);
    } else {
        archive_read_support_format_all(a);
        // names a ZIP holds without its UTF-8 flag (Windows' own zip tool writes them so), and RAR's
        // older non-Unicode names: the DOS code page, as 7-Zip reads them (iconv: musl's)
        archive_read_set_options(a, "zip:hdrcharset=CP437,rar:hdrcharset=CP437");
    }
    if (!password.empty())
        archive_read_add_passphrase(a, password.c_str());
    std::vector<const char *> names;
    for (const std::string &v : volumes)
        names.push_back(v.c_str());
    names.push_back(nullptr);
    if (archive_read_open_filenames(a, names.data(), static_cast<size_t>(1) << 20) != ARCHIVE_OK) {
        error = message(a, "It is no archive OrbisWeb can read");
        archive_read_free(a);
        return nullptr;
    }
    return a;
}

// The entry's path inside (a raw stream's one file is named after the archive)
std::string entry_path(struct archive_entry *entry, bool raw, const std::string &file) {
    if (raw)
        return stem_of(file);
    const char *p = archive_entry_pathname_utf8(entry);
    if (!p)
        p = archive_entry_pathname(entry);
    return clean_path(p ? p : "");
}

std::shared_ptr<Index> read_index_libarchive(const std::string &file, const std::string &password);

std::shared_ptr<Index> read_index(const std::string &file, const std::string &password) {
#if defined(WISP_HAVE_UNRAR)
    std::shared_ptr<Index> index = is_rar(volumes_of(file).front()) ? rar_read_index(file, password) : read_index_libarchive(file, password);
#else
    std::shared_ptr<Index> index = read_index_libarchive(file, password);
#endif
    // (why a list is not whole, in ui.log: Files shows the archive's note, not the library's words)
    if (!index->error.empty())
        wisp_log("files: %s (%s): its list: %s%s", base_of(file).c_str(), index->format.empty() ? "?" : index->format.c_str(),
                 index->error.c_str(), index->needs_password ? (index->wrong_password ? " (a wrong password)" : " (no password)") : "");
    return index;
}

std::shared_ptr<Index> read_index_libarchive(const std::string &file, const std::string &password) {
    auto index = std::make_shared<Index>();
    index->file = file;
    const bool raw = single_stream(file);
    std::string error;
    struct archive *a = open_reader(volumes_of(file), password, raw, error);
    if (!a) {
        index->error = error;
        index->needs_password = passphrase_error(error);
        index->wrong_password = index->needs_password && !password.empty();
        return index;
    }
    std::set<std::string> dirs;
    const auto add_dir = [&](const std::string &path, std::time_t when) {
        if (dirs.insert(path).second) {
            Item item;
            item.path = path;
            item.dir = true;
            item.modified = when;
            index->items.push_back(item);
        }
    };
    struct archive_entry *entry = nullptr;
    int failures = 0;
    for (;;) {
        const int r = archive_read_next_header(a, &entry);
        if (r == ARCHIVE_EOF)
            break;
        if (r == ARCHIVE_FATAL || (r == ARCHIVE_FAILED && ++failures > 64)) {
            index->error = message(a, "The archive is damaged");
            index->needs_password = passphrase_error(index->error);
            index->wrong_password = index->needs_password && !password.empty();
            break;
        }
        if (r == ARCHIVE_FAILED)
            continue;
        const std::string path = entry_path(entry, raw, file);
        if (path.empty())
            continue;
        // the folders above it, whether the archive lists them or not
        for (size_t at = path.find('/'); at != std::string::npos; at = path.find('/', at + 1))
            add_dir(path.substr(0, at), 0);
        const auto type = archive_entry_filetype(entry);
        if (type == AE_IFDIR) {
            add_dir(path, archive_entry_mtime(entry));
            continue;
        }
        Item item;
        item.path = path;
        item.size = archive_entry_size_is_set(entry) ? static_cast<long long>(archive_entry_size(entry)) : -1;
        item.modified = archive_entry_mtime(entry);
        item.encrypted = archive_entry_is_encrypted(entry) != 0;
        if (item.size > 0)
            index->unpacked += item.size;
        index->items.push_back(item);
    }
    if (const char *f = archive_format_name(a))
        index->format = f;
    archive_read_free(a);
    return index;
}

// --- taking files out ---------------------------------------------------------------------------------

struct Extraction {
    std::string file, base, folder, password, open;
    std::vector<std::string> items;
};

// Where an entry goes: below the folder, its first part renamed ("name (2)") when that is taken there
// already (by something this extraction did not make); not renamed: as it is, replacing (a file
// taken out to be opened)
struct Destination {
    std::string folder;
    bool usb = false;
    bool rename = true;
    std::map<std::string, std::string> first;

    std::string place(const std::string &relative) {
        if (!rename)
            return usb ? usb_safe(folder + "/" + relative) : folder + "/" + relative;
        const size_t slash = relative.find('/');
        const std::string head = relative.substr(0, slash), rest = slash == std::string::npos ? std::string() : relative.substr(slash);
        auto it = first.find(head);
        if (it == first.end()) {
            std::string name = head;
            if (exists(folder + "/" + head)) {
                const size_t dot = rest.empty() ? head.find_last_of('.') : std::string::npos;
                const std::string stem = dot == std::string::npos || dot == 0 ? head : head.substr(0, dot);
                const std::string ext = dot == std::string::npos || dot == 0 ? std::string() : head.substr(dot);
                name = shell::unique_name(folder, stem, ext);
            }
            it = first.emplace(head, name).first;
        }
        const std::string to = folder + "/" + it->second + rest;
        return usb ? usb_safe(to) : to;
    }
};

bool chosen(const std::string &path, const std::vector<std::string> &items) {
    if (items.empty())
        return true;
    for (const std::string &item : items)
        if (path == item || (path.size() > item.size() && path.compare(0, item.size(), item) == 0 && path[item.size()] == '/'))
            return true;
    return false;
}

bool write_all(int fd, const void *data, size_t size) {
    const char *p = static_cast<const char *>(data);
    while (size) {
        const ssize_t n = write(fd, p, size);
        if (n <= 0)
            return false;
        p += n;
        size -= static_cast<size_t>(n);
    }
    return true;
}

std::string extract_libarchive(shell::JobControl &job, const Extraction &x);

std::string extract(shell::JobControl &job, const Extraction &x) {
#if defined(WISP_HAVE_UNRAR)
    const bool rar = is_rar(volumes_of(x.file).front());
    const std::string error = rar ? rar_extract(job, x) : extract_libarchive(job, x);
#else
    const bool rar = false;
    const std::string error = extract_libarchive(job, x);
#endif
    // (how it went, in ui.log: Files' own message says it to the user)
    const shell::JobStatus status = shell::job_status();
    if (!error.empty() || status.password_needed)
        wisp_log("files: taking files out of %s (%s) stopped: %s%s", base_of(x.file).c_str(), rar ? "UnRAR" : "libarchive",
                 error.empty() ? "-" : error.c_str(),
                 status.password_needed ? (x.password.empty() ? " (a password is needed)" : " (the password was wrong)") : "");
    return error;
}

std::string extract_libarchive(shell::JobControl &job, const Extraction &x) {
    const std::vector<std::string> volumes = volumes_of(x.file);
    long long total = 0;
    for (const std::string &v : volumes)
        total += file_size(v);
    job.set_total(total);
    const bool raw = single_stream(x.file);
    std::string error;
    struct archive *a = open_reader(volumes, x.password, raw, error);
    if (!a) {
        if (passphrase_error(error))
            job.need_password(!x.password.empty());
        return error;
    }
    if (!make_dirs(x.folder)) {
        archive_read_free(a);
        return "Could not create " + base_of(x.folder) + ": " + std::strerror(errno);
    }
    Destination to{x.folder, x.folder.rfind("/mnt/usb", 0) == 0, x.open.empty(), {}};
    std::map<std::string, std::string> written;  // inside path -> where it went (a tar's hard links)
    std::vector<char> copy_buffer;
    int skipped = 0, failures = 0;
    struct archive_entry *entry = nullptr;
    while (error.empty() && !job.cancelled()) {
        const int r = archive_read_next_header(a, &entry);
        if (r == ARCHIVE_EOF)
            break;
        if (r == ARCHIVE_FATAL || (r == ARCHIVE_FAILED && ++failures > 64)) {
            error = message(a, "The archive is damaged");
            break;
        }
        if (r == ARCHIVE_FAILED)
            continue;
        const std::string path = entry_path(entry, raw, x.file);
        if (path.empty() || !chosen(path, x.items))
            continue;
        std::string relative = path;
        if (!x.base.empty()) {
            if (path.size() <= x.base.size() + 1 || path.compare(0, x.base.size() + 1, x.base + "/") != 0)
                continue;
            relative = path.substr(x.base.size() + 1);
        }
        const std::string target = to.place(relative);
        job.set_current(base_of(relative));
        const auto type = archive_entry_filetype(entry);
        if (type == AE_IFDIR) {
            if (!make_dirs(target))
                error = "Could not create " + base_of(target) + ": " + std::strerror(errno);
            continue;
        }
        if (!make_dirs(parent_of(target))) {
            error = "Could not create " + base_of(parent_of(target)) + ": " + std::strerror(errno);
            break;
        }
        // a tar's hard link: a copy of the file it links to, when that came out already
        if (const char *link = archive_entry_hardlink(entry)) {
            const auto from = written.find(clean_path(link));
            if (from == written.end()) {
                skipped++;
                continue;
            }
            const int in = open(from->second.c_str(), O_RDONLY);
            const int out = in < 0 ? -1 : open(target.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
            if (out >= 0) {
                copy_buffer.resize(static_cast<size_t>(1) << 20);
                ssize_t n;
                while ((n = read(in, copy_buffer.data(), copy_buffer.size())) > 0)
                    if (!write_all(out, copy_buffer.data(), static_cast<size_t>(n))) {
                        error = "Could not write " + base_of(target) + ": " + std::strerror(errno);
                        break;
                    }
                close(out);
            }
            if (in >= 0)
                close(in);
            continue;
        }
        if (type != AE_IFREG && !raw) {  // symbolic links, devices: left out
            skipped++;
            continue;
        }
        const int fd = open(target.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (fd < 0) {
            error = "Could not create " + base_of(target) + ": " + std::strerror(errno);
            break;
        }
        const void *block = nullptr;
        size_t size = 0;
        la_int64_t offset = 0, at = 0;
        bool whole = false;
        for (;;) {
            if (job.cancelled())
                break;
            const int d = archive_read_data_block(a, &block, &size, &offset);
            if (d == ARCHIVE_EOF) {
                whole = true;
                break;
            }
            if (d < ARCHIVE_WARN) {
                const std::string m = message(a, "The archive is damaged");
                if (passphrase_error(m))
                    job.need_password(!x.password.empty());
                error = base_of(relative) + ": " + m;
                break;
            }
            if (offset != at && lseek(fd, static_cast<off_t>(offset), SEEK_SET) < 0) {
                error = "Could not write " + base_of(target) + ": " + std::strerror(errno);
                break;
            }
            if (size && !write_all(fd, block, size)) {
                error = "Could not write " + base_of(target) + ": " + std::strerror(errno);  // (full, most likely)
                break;
            }
            at = offset + static_cast<la_int64_t>(size);
            job.set_done(archive_filter_bytes(a, -1));
        }
        // (a sparse file's empty end)
        if (whole && archive_entry_size_is_set(entry) && at < archive_entry_size(entry))
            ftruncate(fd, static_cast<off_t>(archive_entry_size(entry)));
        close(fd);
        if (!whole) {
            unlink(target.c_str());  // (what was written of it: cancelled, or it failed)
            break;
        }
        written[path] = target;
    }
    archive_read_free(a);
    if (skipped)
        wisp_log("files: %s: %d links or special files left out", base_of(x.file).c_str(), skipped);
    if (error.empty() && !job.cancelled()) {
        job.set_done(total);
        if (!x.open.empty())
            job.open_after(x.folder + "/" + x.open);
    }
    return error;
}

// --- RAR: RARLAB's UnRAR library --------------------------------------------------------------------------
//
// libarchive lists and unpacks RAR, but decrypts none of it: an encrypted file stopped with "Reading
// encrypted data is not currently supported" (RAR5) or "Prefix found" (RAR4), an encrypted list with
// "Encryption is not supported" / "RAR encryption support unavailable" (v00.26 and before). UnRAR's DLL
// API reads every RAR version, encrypted files and lists, and sets of parts (it opens the next part
// itself and says so through the callback). Files are tested (RAR_TEST), not extracted, by UnRAR: their
// data comes to the callback, which writes it where Files puts the file (the same names, renaming, USB
// rules and Cancel as libarchive's formats). UnRAR keeps its error state in one global object, so one
// archive is read at a time (rar_lock: a list asked for while files come out of another RAR waits).

#if defined(WISP_HAVE_UNRAR)

std::mutex rar_lock;

constexpr unsigned rar_max_memory_mib = 512;  // the most a dictionary may take (the UI's heap is 768 MiB)

// UTF-8 <-> UnRAR's wide strings (UTF-32 here), without the C library's locale
std::wstring rar_wide(const std::string &s) {
    std::wstring out;
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        const auto cont = [&](size_t k) { return i + k < s.size() && (static_cast<unsigned char>(s[i + k]) & 0xC0) == 0x80; };
        uint32_t cp = 0xFFFD;
        size_t n = 1;
        if (c < 0x80) {
            cp = c;
        } else if ((c >> 5) == 6 && cont(1)) {
            cp = (c & 0x1Fu) << 6 | (s[i + 1] & 0x3Fu);
            n = 2;
        } else if ((c >> 4) == 14 && cont(1) && cont(2)) {
            cp = (c & 0x0Fu) << 12 | (s[i + 1] & 0x3Fu) << 6 | (s[i + 2] & 0x3Fu);
            n = 3;
        } else if ((c >> 3) == 30 && cont(1) && cont(2) && cont(3)) {
            cp = (c & 0x07u) << 18 | (s[i + 1] & 0x3Fu) << 12 | (s[i + 2] & 0x3Fu) << 6 | (s[i + 3] & 0x3Fu);
            n = 4;
        }
        out.push_back(static_cast<wchar_t>(cp));
        i += n;
    }
    return out;
}

// The DOS code page's upper half (CP437): RAR 4's names stored without Unicode, which UnRAR hands over as
// its private-use stand-ins for bytes it could not convert (U+E080..U+E0FF), as 7-Zip reads them
const char16_t cp437_high[128] = {
    0x00C7, 0x00FC, 0x00E9, 0x00E2, 0x00E4, 0x00E0, 0x00E5, 0x00E7, 0x00EA, 0x00EB, 0x00E8, 0x00EF, 0x00EE, 0x00EC, 0x00C4, 0x00C5,
    0x00C9, 0x00E6, 0x00C6, 0x00F4, 0x00F6, 0x00F2, 0x00FB, 0x00F9, 0x00FF, 0x00D6, 0x00DC, 0x00A2, 0x00A3, 0x00A5, 0x20A7, 0x0192,
    0x00E1, 0x00ED, 0x00F3, 0x00FA, 0x00F1, 0x00D1, 0x00AA, 0x00BA, 0x00BF, 0x2310, 0x00AC, 0x00BD, 0x00BC, 0x00A1, 0x00AB, 0x00BB,
    0x2591, 0x2592, 0x2593, 0x2502, 0x2524, 0x2561, 0x2562, 0x2556, 0x2555, 0x2563, 0x2551, 0x2557, 0x255D, 0x255C, 0x255B, 0x2510,
    0x2514, 0x2534, 0x252C, 0x251C, 0x2500, 0x253C, 0x255E, 0x255F, 0x255A, 0x2554, 0x2569, 0x2566, 0x2560, 0x2550, 0x256C, 0x2567,
    0x2568, 0x2564, 0x2565, 0x2559, 0x2558, 0x2552, 0x2553, 0x256B, 0x256A, 0x2518, 0x250C, 0x2588, 0x2584, 0x258C, 0x2590, 0x2580,
    0x03B1, 0x00DF, 0x0393, 0x03C0, 0x03A3, 0x03C3, 0x00B5, 0x03C4, 0x03A6, 0x0398, 0x03A9, 0x03B4, 0x221E, 0x03C6, 0x03B5, 0x2229,
    0x2261, 0x00B1, 0x2265, 0x2264, 0x2320, 0x2321, 0x00F7, 0x2248, 0x00B0, 0x2219, 0x00B7, 0x221A, 0x207F, 0x00B2, 0x25A0, 0x00A0};

std::string rar_narrow(const wchar_t *w) {
    std::string out;
    for (; w && *w; ++w) {
        uint32_t cp = static_cast<uint32_t>(*w);
        if (cp >= 0xE080 && cp <= 0xE0FF)
            cp = cp437_high[cp - 0xE080];
        if ((cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF)
            cp = 0xFFFD;
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | cp >> 6);
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | cp >> 12);
            out += static_cast<char>(0x80 | (cp >> 6 & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | cp >> 18);
            out += static_cast<char>(0x80 | (cp >> 12 & 0x3F));
            out += static_cast<char>(0x80 | (cp >> 6 & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }
    return out;
}

// RAR's marks: "Rar!" 1A 07 00 (RAR 1.5 to 4) and "Rar!" 1A 07 01 00 (RAR 5)
int rar_version_of(const std::string &file) {
    unsigned char head[8] = {};
    const int fd = open(file.c_str(), O_RDONLY);
    if (fd < 0)
        return 0;
    const ssize_t n = read(fd, head, sizeof head);
    close(fd);
    if (n < 7 || std::memcmp(head, "Rar!\x1a\x07", 6) != 0)
        return 0;
    if (head[6] == 0)
        return 4;
    return n >= 8 && head[6] == 1 && head[7] == 0 ? 5 : 0;
}

bool is_rar(const std::string &file) { return rar_version_of(file) != 0; }

// What one call into UnRAR needs from its callback, and what the callback saw
struct RarCall {
    std::wstring password;      // the one given ("": none)
    bool password_asked = false;  // UnRAR wanted one and there was none
    std::string missing_part;   // a part of the set UnRAR could not find
    // taking a file out: where its data goes, and the job's progress (unpacked bytes: those of the
    // files done, then this file's written so far)
    int fd = -1;
    bool write_failed = false;
    int write_errno = 0;
    bool cancelled = false;
    shell::JobControl *job = nullptr;
    long long written = 0, done_before = 0, last_done = 0;
};

void rar_progress(RarCall &c, long long done) {
    if (!c.job || done <= c.last_done)
        return;
    c.last_done = done;
    c.job->set_done(done);
}

int CALLBACK rar_callback(UINT message, LPARAM user, LPARAM p1, LPARAM p2) {
    RarCall &c = *reinterpret_cast<RarCall *>(user);
    switch (message) {
    case UCM_NEEDPASSWORDW: {
        if (c.password.empty()) {
            c.password_asked = true;
            return -1;
        }
        auto *buffer = reinterpret_cast<wchar_t *>(p1);
        const size_t room = static_cast<size_t>(p2);
        if (!buffer || room < 2)
            return -1;
        const size_t n = std::min(c.password.size(), room - 1);
        std::copy(c.password.begin(), c.password.begin() + static_cast<long>(n), buffer);
        buffer[n] = 0;
        return 1;
    }
    case UCM_NEEDPASSWORD:  // (asked only when the wide question got no answer)
        c.password_asked = true;
        return -1;
    case UCM_CHANGEVOLUMEW:
    case UCM_CHANGEVOLUME: {
        const std::string name = message == UCM_CHANGEVOLUMEW ? rar_narrow(reinterpret_cast<const wchar_t *>(p1))
                                                               : std::string(reinterpret_cast<const char *>(p1));
        if (p2 == RAR_VOL_ASK) {  // not there: the set is short of a part
            c.missing_part = name;
            return -1;
        }
        return 1;  // (on to the next part)
    }
    case UCM_PROCESSDATA: {
        if (c.job && c.job->cancelled()) {
            c.cancelled = true;
            return -1;
        }
        const auto *data = reinterpret_cast<const void *>(p1);
        const size_t size = static_cast<size_t>(p2);
        if (c.fd >= 0 && size && !write_all(c.fd, data, size)) {
            c.write_failed = true;
            c.write_errno = errno;
            return -1;
        }
        c.written += static_cast<long long>(size);
        rar_progress(c, c.done_before + c.written);
        return 1;
    }
    case UCM_LARGEDICT:  // a dictionary over 4 GiB (RAR 7): far more than the UI's heap
        return 0;
    }
    return 0;
}

std::string rar_message(int code, const RarCall &c) {
    switch (code) {
    case ERAR_MISSING_PASSWORD: return "It is locked with a password";
    case ERAR_BAD_PASSWORD: return "That password is not this archive's";
    case ERAR_EOPEN:
        return c.missing_part.empty() ? "It could not be opened" : "A part of the set is missing: " + base_of(c.missing_part);
    case ERAR_BAD_DATA: return "The archive is damaged (a checksum does not match)";
    case ERAR_BAD_ARCHIVE: return "It is no RAR archive OrbisWeb can read";
    case ERAR_UNKNOWN_FORMAT: return "Its RAR format is newer than OrbisWeb's UnRAR";
    case ERAR_NO_MEMORY: return "Not enough memory to unpack it";
    case ERAR_EREAD: return "It could not be read (the drive?)";
    case ERAR_EREFERENCE: return "A file it copies from is missing";
    case ERAR_LARGE_DICT: return "It was packed with a dictionary over 4 GiB (WinRAR 7): too large to unpack here";
    case ERAR_USER_LIMIT:
        return "It needs more than " + std::to_string(rar_max_memory_mib) + " MiB of memory to unpack (its dictionary)";
    case ERAR_ECREATE: case ERAR_EWRITE: return "A file could not be written";
    default: return "UnRAR stopped (error " + std::to_string(code) + ")";
    }
}

bool rar_password_problem(int code) { return code == ERAR_MISSING_PASSWORD || code == ERAR_BAD_PASSWORD; }

HANDLE rar_open(const std::string &first, unsigned mode, RarCall &call, int &code, unsigned &flags) {
    std::wstring name = rar_wide(first);
    RAROpenArchiveDataEx d;
    std::memset(&d, 0, sizeof d);
    d.ArcNameW = name.data();
    d.OpenMode = mode;
    d.Callback = rar_callback;
    d.UserData = reinterpret_cast<LPARAM>(&call);
    d.MaxAllocate = rar_max_memory_mib;
    HANDLE h = RAROpenArchiveEx(&d);
    code = h ? static_cast<int>(d.OpenResult) : (d.OpenResult ? static_cast<int>(d.OpenResult) : ERAR_EOPEN);
    if (h && code != ERAR_SUCCESS) {
        RARCloseArchive(h);
        h = nullptr;
    }
    // RAR 4 has no password check: with a wrong password, its encrypted list reads as headers whose
    // checksums are wrong (a password is only given to an archive that asked for one)
    if (!h && code == ERAR_BAD_DATA && !call.password.empty())
        code = ERAR_BAD_PASSWORD;
    flags = d.Flags;
    return h;
}

// One entry's header, with room for long names and a link's target
struct RarHeader {
    RARHeaderDataEx d;
    std::vector<wchar_t> name, redirect;
    RarHeader() : name(8192), redirect(4096) { reset(); }
    void reset() {
        std::memset(&d, 0, sizeof d);
        d.FileNameEx = name.data();
        d.FileNameExSize = static_cast<unsigned>(name.size());
        d.RedirName = redirect.data();
        d.RedirNameSize = static_cast<unsigned>(redirect.size());
        name[0] = 0;
        redirect[0] = 0;
    }
    std::string path() const { return clean_path(rar_narrow(name[0] ? name.data() : d.FileNameW)); }
    long long size() const { return static_cast<long long>(d.UnpSize) | static_cast<long long>(d.UnpSizeHigh) << 32; }
    long long packed() const { return static_cast<long long>(d.PackSize) | static_cast<long long>(d.PackSizeHigh) << 32; }
    std::time_t modified() const {
        const uint64_t ft = static_cast<uint64_t>(d.MtimeHigh) << 32 | d.MtimeLow;  // Windows' 100 ns since 1601
        return ft ? static_cast<std::time_t>(ft / 10000000ull - 11644473600ull) : 0;
    }
};

const char *rar_format_name(const std::string &first) { return rar_version_of(first) == 5 ? "RAR5" : "RAR"; }

// The next entry's header. (RAR 4 again: in a list encrypted with a wrong password, the first header's
// checksum is what fails)
int rar_read_header(HANDLE h, RarHeader &header, unsigned flags, const RarCall &call) {
    header.reset();
    const int r = RARReadHeaderEx(h, &header.d);
    return r == ERAR_BAD_DATA && (flags & ROADF_ENCHEADERS) && !call.password.empty() ? ERAR_BAD_PASSWORD : r;
}

std::shared_ptr<Index> rar_read_index(const std::string &file, const std::string &password) {
    auto index = std::make_shared<Index>();
    index->file = file;
    const std::vector<std::string> parts = volumes_of(file);
    index->format = rar_format_name(parts.front());
    std::lock_guard guard(rar_lock);
    RarCall call;
    call.password = rar_wide(password);
    int code = 0;
    unsigned flags = 0;
    HANDLE h = rar_open(parts.front(), RAR_OM_LIST, call, code, flags);
    if (!h) {
        index->error = rar_message(code, call);
        index->needs_password = rar_password_problem(code) || call.password_asked;
        index->wrong_password = index->needs_password && !password.empty();
        return index;
    }
    if (flags & ROADF_ENCHEADERS)
        index->format += " (its list encrypted)";
    std::set<std::string> dirs;
    const auto add_dir = [&](const std::string &path, std::time_t when) {
        if (dirs.insert(path).second) {
            Item item;
            item.path = path;
            item.dir = true;
            item.modified = when;
            index->items.push_back(item);
        }
    };
    auto header = std::make_unique<RarHeader>();
    for (;;) {
        int r = rar_read_header(h, *header, flags, call);
        if (r == ERAR_END_ARCHIVE)
            break;
        if (r != ERAR_SUCCESS) {
            index->error = rar_message(r, call);
            index->needs_password = rar_password_problem(r) || call.password_asked;
            index->wrong_password = index->needs_password && !password.empty();
            break;
        }
        const std::string path = header->path();
        if (!path.empty()) {
            for (size_t at = path.find('/'); at != std::string::npos; at = path.find('/', at + 1))
                add_dir(path.substr(0, at), 0);
            if (header->d.Flags & RHDF_DIRECTORY) {
                add_dir(path, header->modified());
            } else {
                Item item;
                item.path = path;
                item.size = header->size();
                item.modified = header->modified();
                item.encrypted = (header->d.Flags & RHDF_ENCRYPTED) || (flags & ROADF_ENCHEADERS);
                if (item.size > 0)
                    index->unpacked += item.size;
                index->items.push_back(item);
            }
        }
        r = RARProcessFileW(h, RAR_SKIP, nullptr, nullptr);
        if (r != ERAR_SUCCESS) {
            index->error = rar_message(r, call);
            break;
        }
    }
    RARCloseArchive(h);
    return index;
}

// Where an entry goes below the extraction's folder ("": not wanted: not chosen, or outside the archive
// folder being taken out)
std::string rar_relative(const std::string &path, const Extraction &x) {
    if (path.empty() || !chosen(path, x.items))
        return {};
    if (x.base.empty())
        return path;
    if (path.size() <= x.base.size() + 1 || path.compare(0, x.base.size() + 1, x.base + "/") != 0)
        return {};
    return path.substr(x.base.size() + 1);
}

// An entry's unpacked size as the progress counts it (none for a size RAR does not know)
long long rar_entry_bytes(const RarHeader &header) {
    const long long size = header.size();
    return size > 0 && size < (1LL << 50) ? size : 0;
}

// The unpacked bytes of the files an extraction takes out, from the list: the progress's total (a file's
// header has its whole size, also when its data runs on into later parts). 0: the list could not be
// read (taking the files out then says why)
long long rar_wanted_bytes(const std::string &first, const Extraction &x) {
    RarCall call;
    call.password = rar_wide(x.password);
    int code = 0;
    unsigned flags = 0;
    HANDLE h = rar_open(first, RAR_OM_LIST, call, code, flags);
    if (!h)
        return 0;
    long long total = 0;
    auto header = std::make_unique<RarHeader>();
    while (rar_read_header(h, *header, flags, call) == ERAR_SUCCESS) {
        if (!(header->d.Flags & RHDF_DIRECTORY) && !rar_relative(header->path(), x).empty())
            total += rar_entry_bytes(*header);
        if (RARProcessFileW(h, RAR_SKIP, nullptr, nullptr) != ERAR_SUCCESS)
            break;
    }
    RARCloseArchive(h);
    return total;
}

// The job's progress counts unpacked bytes: the files done, then the one coming out. (v00.27 counted the
// archive's packed bytes, a file's share as packed x written / size: that product passed 2^63 inside
// any file over ~3 GiB, and the progress stopped there, at 1.54 of 6.34 GB with a 5.6 GB file, while
// the files went on coming out)
std::string rar_extract(shell::JobControl &job, const Extraction &x) {
    const std::string first = volumes_of(x.file).front();
    std::lock_guard guard(rar_lock);
    const long long total = std::max(1LL, rar_wanted_bytes(first, x));
    job.set_total(total);
    RarCall call;
    call.password = rar_wide(x.password);
    call.job = &job;
    int code = 0;
    unsigned flags = 0;
    HANDLE h = rar_open(first, RAR_OM_EXTRACT, call, code, flags);
    if (!h) {
        if (rar_password_problem(code) || call.password_asked)
            job.need_password(!x.password.empty());
        return rar_message(code, call);
    }
    if (!make_dirs(x.folder)) {
        RARCloseArchive(h);
        return "Could not create " + base_of(x.folder) + ": " + std::strerror(errno);
    }
    Destination to{x.folder, x.folder.rfind("/mnt/usb", 0) == 0, x.open.empty(), {}};
    std::map<std::string, std::string> written;  // inside path -> where it went (copies and hard links)
    std::vector<char> copy_buffer;
    std::string error;
    int skipped = 0;
    auto header = std::make_unique<RarHeader>();
    while (error.empty() && !job.cancelled()) {
        int r = rar_read_header(h, *header, flags, call);
        if (r == ERAR_END_ARCHIVE)
            break;
        if (r != ERAR_SUCCESS) {
            if (rar_password_problem(r) || call.password_asked)
                job.need_password(!x.password.empty());
            error = rar_message(r, call);
            break;
        }
        const std::string path = header->path();
        const auto skip = [&] {
            const int s = RARProcessFileW(h, RAR_SKIP, nullptr, nullptr);
            if (s != ERAR_SUCCESS && error.empty()) {
                if (rar_password_problem(s) || call.password_asked)
                    job.need_password(!x.password.empty());
                error = rar_message(s, call);
            }
        };
        // a wanted file is done (taken out, copied, or left out as a link): its bytes count
        const auto count_done = [&] {
            call.written = 0;
            call.done_before += rar_entry_bytes(*header);
            rar_progress(call, call.done_before);
        };
        const std::string relative = rar_relative(path, x);
        if (relative.empty()) {
            skip();
            continue;
        }
        const std::string target = to.place(relative);
        job.set_current(base_of(relative));
        if (header->d.Flags & RHDF_DIRECTORY) {
            if (!make_dirs(target))
                error = "Could not create " + base_of(target) + ": " + std::strerror(errno);
            skip();
            continue;
        }
        if (!make_dirs(parent_of(target))) {
            error = "Could not create " + base_of(parent_of(target)) + ": " + std::strerror(errno);
            break;
        }
        // a link or a copy of another file in the archive (RAR 5): a copy of that file when it came out
        // already; symbolic links and junctions are left out, as libarchive's formats' are
        if (header->d.RedirType != 0) {
            const unsigned hard_link = 4, file_copy = 5;  // (UnRAR's FSREDIR_HARDLINK, FSREDIR_FILECOPY)
            const auto from = written.find(clean_path(rar_narrow(header->redirect.data())));
            if ((header->d.RedirType == hard_link || header->d.RedirType == file_copy) && from != written.end()) {
                const int in = open(from->second.c_str(), O_RDONLY);
                const int out = in < 0 ? -1 : open(target.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
                if (out >= 0) {
                    copy_buffer.resize(static_cast<size_t>(1) << 20);
                    ssize_t n;
                    while ((n = read(in, copy_buffer.data(), copy_buffer.size())) > 0)
                        if (!write_all(out, copy_buffer.data(), static_cast<size_t>(n))) {
                            error = "Could not write " + base_of(target) + ": " + std::strerror(errno);
                            break;
                        }
                    close(out);
                    written[path] = target;
                }
                if (in >= 0)
                    close(in);
            } else {
                skipped++;
            }
            skip();
            count_done();
            continue;
        }
        const int fd = open(target.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (fd < 0) {
            error = "Could not create " + base_of(target) + ": " + std::strerror(errno);
            break;
        }
        call.fd = fd;
        call.written = 0;
        call.write_failed = false;
        r = RARProcessFileW(h, RAR_TEST, nullptr, nullptr);
        call.fd = -1;
        close(fd);
        if (r != ERAR_SUCCESS || call.cancelled || call.write_failed) {
            unlink(target.c_str());  // (what was written of it: cancelled, or it failed)
            if (call.cancelled || job.cancelled())
                break;
            if (call.write_failed)
                error = "Could not write " + base_of(target) + ": " + std::strerror(call.write_errno);  // (full, most likely)
            else if (rar_password_problem(r) || call.password_asked ||
                     (r == ERAR_BAD_DATA && (header->d.Flags & RHDF_ENCRYPTED) && !x.password.empty())) {
                // (RAR 4 has no password check: a wrong one shows as a file whose checksum is wrong)
                job.need_password(!x.password.empty());
                error = base_of(relative) + ": " + rar_message(r == ERAR_BAD_DATA ? ERAR_BAD_PASSWORD : r, call);
            } else
                error = base_of(relative) + ": " + rar_message(r, call);
            break;
        }
        written[path] = target;
        count_done();
    }
    RARCloseArchive(h);
    if (skipped)
        wisp_log("files: %s: %d links left out", base_of(x.file).c_str(), skipped);
    if (error.empty() && !job.cancelled()) {
        job.set_done(total);
        if (!x.open.empty())
            job.open_after(x.folder + "/" + x.open);
    }
    return error;
}

#endif  // WISP_HAVE_UNRAR

// --- making archives -------------------------------------------------------------------------------------

std::string add_tree(struct archive *w, const std::string &path, const std::string &name, shell::JobControl &job, std::vector<char> &buffer) {
    if (job.cancelled())
        return {};
    struct stat st{};
    if (lstat(path.c_str(), &st) != 0)
        return "Could not read " + base_of(path) + ": " + std::strerror(errno);
    if (!S_ISDIR(st.st_mode) && !S_ISREG(st.st_mode))
        return {};  // (links, devices: left out)
    struct archive_entry *e = archive_entry_new();
    // The name as UTF-8; with a locale that is not UTF-8 (the UI process sets C.UTF-8), as its bytes in
    // the locale's form: libarchive's writers read that form, a non-ASCII name can't be converted into
    // it, and tar's writer then took the missing name for a string (libarchive 3.8: a crash)
    if (MB_CUR_MAX > 1)
        archive_entry_set_pathname_utf8(e, name.c_str());
    else
        archive_entry_copy_pathname(e, name.c_str());
    archive_entry_set_mtime(e, st.st_mtime, 0);
    archive_entry_set_filetype(e, S_ISDIR(st.st_mode) ? AE_IFDIR : AE_IFREG);
    archive_entry_set_perm(e, S_ISDIR(st.st_mode) ? 0755 : 0644);
    archive_entry_set_size(e, S_ISDIR(st.st_mode) ? 0 : st.st_size);
    job.set_current(name);
    if (archive_write_header(w, e) < ARCHIVE_WARN) {
        const std::string m = message(w, "It could not be added");
        archive_entry_free(e);
        return base_of(name) + ": " + m;
    }
    archive_entry_free(e);
    if (S_ISDIR(st.st_mode)) {
        std::vector<std::string> names;
        if (DIR *dir = opendir(path.c_str())) {
            while (dirent *d = readdir(dir))
                if (std::strcmp(d->d_name, ".") && std::strcmp(d->d_name, ".."))
                    names.push_back(d->d_name);
            closedir(dir);
        }
        std::sort(names.begin(), names.end());
        for (const std::string &n : names) {
            std::string error = add_tree(w, path + "/" + n, name + "/" + n, job, buffer);
            if (!error.empty())
                return error;
        }
        return {};
    }
    const int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0)
        return "Could not open " + base_of(path) + ": " + std::strerror(errno);
    std::string error;
    ssize_t n;
    while (!job.cancelled() && (n = read(fd, buffer.data(), buffer.size())) > 0) {
        if (archive_write_data(w, buffer.data(), static_cast<size_t>(n)) < 0) {
            error = base_of(name) + ": " + message(w, "It could not be written");
            break;
        }
        job.add_done(n);
    }
    close(fd);
    return error;
}

std::string compress(shell::JobControl &job, const std::vector<std::string> &sources, const std::string &target, Format format) {
    long long items = 0, total = 0;
    for (const std::string &s : sources)
        total += shell::folder_size(s, items);
    job.set_total(total);
    struct archive *w = archive_write_new();
    if (!w)
        return "Not enough memory to make it";
    switch (format) {
    case Format::Zip:
        archive_write_set_format_zip(w);
        if (archive_zlib_version())
            archive_write_zip_set_compression_deflate(w);
        archive_write_set_format_option(w, "zip", "hdrcharset", "UTF-8");  // (names as typed: the UTF-8 flag)
        break;
    case Format::SevenZip:
        archive_write_set_format_7zip(w);
        archive_write_set_format_option(w, "7zip", "compression", archive_liblzma_version() ? "lzma2" : "deflate");
        archive_write_set_format_option(w, "7zip", "compression-level", "5");  // (7-Zip's "Normal")
        break;
    case Format::TarGz:
        archive_write_set_format_pax_restricted(w);
        // names as their bytes (UTF-8 here) when the locale is not UTF-8 (the UI process sets C.UTF-8):
        // pax's writer converts names through the locale's character set, and with one that can't hold
        // them it fell over (a NULL string's length; libarchive 3.8)
        // (no module name: pax_restricted's options go by "restricted pax", not "pax")
        if (MB_CUR_MAX == 1)
            archive_write_set_format_option(w, nullptr, "hdrcharset", "BINARY");
        if (!archive_zlib_version()) {
            archive_write_free(w);
            return "This build has no gzip";
        }
        archive_write_add_filter_gzip(w);
        break;
    }
    if (archive_write_open_filename(w, target.c_str()) != ARCHIVE_OK) {
        const std::string m = message(w, "It could not be made");
        archive_write_free(w);
        return "Could not create " + base_of(target) + ": " + m;
    }
    std::vector<char> buffer(static_cast<size_t>(1) << 20);
    std::string error;
    for (const std::string &s : sources) {
        error = add_tree(w, s, base_of(s), job, buffer);
        if (!error.empty() || job.cancelled())
            break;
    }
    // (7-Zip's list is written last, and its packed data copied in after it: this can take a while)
    if (error.empty() && !job.cancelled()) {
        job.set_current("Finishing " + base_of(target));
        if (archive_write_close(w) != ARCHIVE_OK)
            error = "Could not finish " + base_of(target) + ": " + message(w, "the drive may be full");
    }
    archive_write_free(w);
    if (!error.empty() || job.cancelled())
        unlink(target.c_str());
    return error;
}

#endif  // WISP_HAVE_LIBARCHIVE

// --- lists, read on a thread ------------------------------------------------------------------------------

struct Stamp {
    long long size = -1;
    std::time_t modified = 0;
    bool operator==(const Stamp &o) const { return size == o.size && modified == o.modified; }
};

Stamp stamp_of(const std::string &file) {
    struct stat st{};
    if (stat(file.c_str(), &st) != 0)
        return {};
    return {static_cast<long long>(st.st_size), st.st_mtime};
}

std::mutex index_lock;
std::shared_ptr<const Index> cached;  // the last list read
Stamp cached_stamp;
std::string cached_password;
std::string reading, reading_password;  // the list being read ("": none)
std::string wanted, wanted_password;    // the one asked for while another was read
bool finished_one = false;

void start_reading(const std::string &file, const std::string &password);

void *read_main(void *) {
    std::string file, password;
    {
        std::lock_guard g(index_lock);
        file = reading;
        password = reading_password;
    }
#if defined(WISP_HAVE_LIBARCHIVE)
    std::shared_ptr<const Index> index = read_index(file, password);
#else
    auto none = std::make_shared<Index>();
    none->file = file;
    none->error = "This build of OrbisWeb has no archive support";
    std::shared_ptr<const Index> index = none;
#endif
    std::lock_guard g(index_lock);
    cached = index;
    cached_stamp = stamp_of(file);
    cached_password = password;
    finished_one = true;
    reading.clear();
    if (!wanted.empty()) {
        const std::string next = wanted, next_password = wanted_password;
        wanted.clear();
        start_reading(next, next_password);
    }
    return nullptr;
}

// (index_lock held)
void start_reading(const std::string &file, const std::string &password) {
    reading = file;
    reading_password = password;
    if (!start_thread(read_main, nullptr)) {
        auto failed = std::make_shared<Index>();
        failed->file = file;
        failed->error = "No thread could start to read it";
        cached = failed;
        cached_stamp = stamp_of(file);
        cached_password = password;
        finished_one = true;
        reading.clear();
    }
}

std::string opened_root() { return std::string(WISP_DATA) + "/tmp/opened"; }

}  // namespace

bool available() {
#if defined(WISP_HAVE_LIBARCHIVE)
    return true;
#else
    return false;
#endif
}

bool is_archive_name(const std::string &name) {
    const std::string l = lower(name);
    static const char *const exts[] = {".zip", ".zipx", ".jar", ".apk", ".cbz", ".rar", ".cbr", ".7z", ".cb7", ".tar", ".tgz",
                                       ".tbz", ".tbz2", ".txz", ".tzst", ".tlz", ".taz", ".gz", ".bz2", ".xz", ".lzma", ".lz",
                                       ".lz4", ".zst", ".z", ".iso", ".cab", ".lzh", ".lha", ".cpio", ".ar", ".deb", ".rpm",
                                       ".xar", ".warc", ".cbt"};
    if (std::any_of(std::begin(exts), std::end(exts), [&](const char *e) { return ends_with(l, e); }))
        return true;
    std::string stem;
    if (rar_old_part(name, stem))
        return true;
    return split_part(name, stem) && is_archive_name(stem);
}

std::vector<std::string> volumes_of(const std::string &file) {
    const std::string dir = parent_of(file), name = base_of(file);
    const auto in_dir = [&](const std::string &n) { return dir.empty() ? n : dir + "/" + n; };
    std::string stem;
    int number = 0;
    size_t width = 0;
    // name.part1.rar, name.part2.rar, ...
    if (rar_part(name, stem, number, width)) {
        const std::string ext = name.substr(name.size() - 4);
        const std::string part = name.substr(stem.size(), 5);  // ".part" as written
        std::vector<std::string> list;
        for (int i = 1; i < 10000; i++) {
            char digits[16];
            std::snprintf(digits, sizeof digits, "%0*d", static_cast<int>(width), i);
            const std::string path = in_dir(stem + part + digits + ext);
            if (!exists(path))
                break;
            list.push_back(path);
        }
        return list.empty() ? std::vector<std::string>{file} : list;
    }
    // name.rar, name.r00, name.r01, ... (.s00 after .r99)
    std::string rar;
    if (ends_with(lower(name), ".rar"))
        rar = in_dir(name);
    else if (rar_old_part(name, stem)) {
        for (const char *e : {".rar", ".RAR"})
            if (exists(in_dir(stem + e)))
                rar = in_dir(stem + e);
    }
    if (!rar.empty()) {
        const std::string base = rar.substr(0, rar.size() - 4);
        const bool upper = rar.back() == 'R';
        std::vector<std::string> list{rar};
        // .r00 ... .r99, then .s00 ... .s99, .t00 ...
        for (int i = 0; i < 500; i++) {
            char ext[8];
            std::snprintf(ext, sizeof ext, ".%c%02d", (upper ? 'R' : 'r') + i / 100, i % 100);
            if (!exists(base + ext))
                break;
            list.push_back(base + ext);
        }
        return list;
    }
    // name.7z.001, name.7z.002, ... (a file cut in pieces: read as one)
    if (split_part(name, stem)) {
        std::vector<std::string> list;
        for (int i = 1; i < 1000; i++) {
            char ext[8];
            std::snprintf(ext, sizeof ext, ".%03d", i);
            const std::string path = in_dir(stem + ext);
            if (!exists(path))
                break;
            list.push_back(path);
        }
        return list.empty() ? std::vector<std::string>{file} : list;
    }
    return {file};
}

std::string stem_of(const std::string &file) {
    std::string name = base_of(file);
    std::string stem;
    int number = 0;
    size_t width = 0;
    if (rar_part(name, stem, number, width))
        return stem;
    if (rar_old_part(name, stem))
        return stem;
    if (split_part(name, stem))
        name = stem;
    const std::string l = lower(name);
    static const char *const compound[] = {".tar.gz", ".tar.bz2", ".tar.xz", ".tar.zst", ".tar.lz4", ".tar.lzma", ".tar.lz", ".tar.z"};
    for (const char *c : compound)
        if (ends_with(l, c))
            return name.substr(0, name.size() - std::strlen(c));
    const size_t dot = name.find_last_of('.');
    return dot == std::string::npos || dot == 0 ? name + " (unpacked)" : name.substr(0, dot);
}

std::shared_ptr<const Index> index_of(const std::string &file, const std::string &password) {
    std::lock_guard g(index_lock);
    if (cached && cached->file == file && cached_password == password && cached_stamp == stamp_of(file))
        return cached;
    if (reading == file && reading_password == password)
        return nullptr;
    if (!reading.empty()) {
        wanted = file;
        wanted_password = password;
        return nullptr;
    }
    start_reading(file, password);
    return nullptr;
}

bool list_ready() {
    std::lock_guard g(index_lock);
    const bool ready = finished_one;
    finished_one = false;
    return ready;
}

std::string open_folder(const std::string &file) {
    // (the files opened before this run of the app: gone)
    static std::once_flag emptied;
    std::call_once(emptied, [] { remove_all(opened_root()); });
    return opened_root() + "/" + stem_of(file);
}

bool job_extract(const std::string &file, const std::vector<std::string> &items, const std::string &base, const std::string &folder,
                 const std::string &password, const std::string &title, const std::string &open) {
#if defined(WISP_HAVE_LIBARCHIVE)
    Extraction x{file, base, folder, password, open, items};
    return shell::job_run(title, [x](shell::JobControl &job) { return extract(job, x); }, {parent_of(folder), folder});
#else
    (void)file, (void)items, (void)base, (void)password, (void)open;
    return shell::job_run(title, [](shell::JobControl &) { return std::string("This build of OrbisWeb has no archive support"); }, {folder});
#endif
}

bool job_compress(const std::vector<std::string> &sources, const std::string &target, Format format) {
    const std::string title = "Compressing to " + base_of(target);
#if defined(WISP_HAVE_LIBARCHIVE)
    return shell::job_run(title, [sources, target, format](shell::JobControl &job) { return compress(job, sources, target, format); },
                          {parent_of(target)});
#else
    (void)sources, (void)format;
    return shell::job_run(title, [](shell::JobControl &) { return std::string("This build of OrbisWeb has no archive support"); },
                          {parent_of(target)});
#endif
}

const char *extension_of(Format format) {
    switch (format) {
    case Format::Zip: return ".zip";
    case Format::SevenZip: return ".7z";
    case Format::TarGz: return ".tar.gz";
    }
    return ".zip";
}

}  // namespace wisp::archive
