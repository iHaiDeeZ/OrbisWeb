// WinePS4's file operations for the desktop's Files: new folders and files, renaming, and the long
// ones (copy, move, delete) on a thread of their own with their progress.
#pragma once

#include <functional>
#include <string>
#include <vector>

namespace shell {

// "New folder", then "New folder (2)", ... : a name not yet used in dir
std::string unique_name(const std::string &dir, const std::string &stem, const std::string &extension);
// What went wrong, or "" when it worked
std::string make_folder(const std::string &path);
std::string make_file(const std::string &path);
std::string rename_entry(const std::string &from, const std::string &to);
// The bytes in the files below path (a file's own size); items: how many there are, path among them
long long folder_size(const std::string &path, long long &items);

struct JobStatus {
    bool running = false;
    bool finished = false;          // ended since job_acknowledge
    std::string title;              // "Copying 3 items", ...
    std::string current;            // the file being worked on
    long long done = 0, total = 0;  // bytes (copying), items (deleting: counts_items)
    bool counts_items = false;
    bool cancelling = false;        // Cancel pressed: it stops at the next file or block
    bool cancelled = false;         // (finished) it was stopped
    std::string error;              // why it stopped, if it did
    std::vector<std::string> touched;  // folders whose lists changed
    // (archives) the entries need a password; wrong_password: the one given was not it
    bool password_needed = false, wrong_password = false;
    std::string open_after;         // (finished well) a file to open: one taken out of an archive to open it
};

// What a job of another kind (archives: files/archives.cpp) tells and asks while it runs
class JobControl {
public:
    void set_total(long long n);
    void add_done(long long n);
    void set_done(long long n);
    void set_current(const std::string &name);
    void set_counts_items(bool items);  // done/total are items (the card says "3 of 10"), not bytes
    bool cancelled() const;
    void need_password(bool wrong);
    void open_after(const std::string &path);
};

// Copies (or moves, cut) the sources into the folder; false while another job runs
bool job_paste(const std::vector<std::string> &sources, bool cut, const std::string &folder);
// Deletes them for good (Files' Recycle Bin is a move: files/files.cpp)
bool job_delete(const std::vector<std::string> &paths);
// Runs work on the job's thread (1 MiB of stack); what it returns is the error ("" when it worked); touched:
// the folders whose lists change. False while another job runs.
bool job_run(const std::string &title, std::function<std::string(JobControl &)> work, std::vector<std::string> touched);
// Stops the job that runs (at its next file or block; what it was writing is removed)
void job_cancel();
JobStatus job_status();
void job_acknowledge();

} // namespace shell
