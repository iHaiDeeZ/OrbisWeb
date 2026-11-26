// Wisp: archives for Files (libarchive): their lists (Files opens one as a compressed folder, as
// Windows does), taking files out of them, and making ZIP / 7z / tar.gz files. The long work runs as a
// job of shell/fileops (its card shows the progress, Cancel stops it). Without libarchive
// (WISP_HAVE_LIBARCHIVE unset) available() is false and every job says so.
#pragma once

#include <ctime>
#include <memory>
#include <string>
#include <vector>

namespace wisp::archive {

bool available();
// A name Files opens as a compressed folder: .zip, .rar, .7z, .tar(.gz, .bz2, .xz, .zst, ...), .iso,
// .cab, .lzh, .cpio, ...; a file packed alone (.gz, .bz2, .xz, .zst, .lz4, .lzma, .Z) holds one file;
// a later part of a set (.part2.rar, .r00, .7z.002) opens the set
bool is_archive_name(const std::string &name);
// A set's parts in order (the first part first) from any one of them; just the file when it is no set
std::vector<std::string> volumes_of(const std::string &file);
// The name an archive's contents go under: "photos" for photos.zip, photos.tar.gz, photos.part1.rar
std::string stem_of(const std::string &file);

struct Item {
    std::string path;  // inside: '/'-separated, no leading '/'
    bool dir = false;
    long long size = -1;
    std::time_t modified = 0;
    bool encrypted = false;
};

struct Index {
    std::string file;
    std::string format;           // as libarchive names it ("ZIP 2.0 (deflation)", "RAR5", "7-Zip", ...)
    std::vector<Item> items;      // folders too (made up for paths whose folders the archive does not list)
    std::string error;            // why the list is not whole ("" when it is)
    bool needs_password = false;  // the list itself is encrypted (ZIP's entries: Item::encrypted)
    bool wrong_password = false;
    long long unpacked = 0;       // the files' sizes, as far as the archive says
};

// The list of an archive, read on a thread of its own: nullptr while it is read (list_ready() turns
// true when it has been: Files polls it). A file that changed is read again.
std::shared_ptr<const Index> index_of(const std::string &file, const std::string &password = std::string());
bool list_ready();

// Takes the entries at the inside paths `items` (every entry when empty), each with what is below it,
// into folder (made when missing); their paths there start after `base` (the inside folder they were
// chosen in: "" for the archive's top). An entry whose first part is taken in folder already gets
// another name ("name (2)"). open: the one file to open once it is out (opening a file in an archive).
bool job_extract(const std::string &file, const std::vector<std::string> &items, const std::string &base,
                 const std::string &folder, const std::string &password, const std::string &title,
                 const std::string &open = std::string());
// Where a file opened from inside an archive is taken out to (a folder of the app's, emptied at start)
std::string open_folder(const std::string &file);

enum class Format { Zip, SevenZip, TarGz };
// The sources (one folder's entries) into a new archive at target
bool job_compress(const std::vector<std::string> &sources, const std::string &target, Format format);
const char *extension_of(Format format);

}  // namespace wisp::archive
