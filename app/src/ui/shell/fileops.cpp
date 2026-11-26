// WinePS4's file operations (fileops.h). Paths are absolute; ps4-kit's file layer takes them as
// they are. A move within one file system is a rename; across them (a USB drive and /data) it is a
// copy, then the delete of the original once the copy is whole.
#include "fileops.h"

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <thread>

#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>

namespace shell {
namespace {

std::mutex lock;
JobStatus status;
// Cancel pressed (the job's loops look at it between files and blocks); a job that stops for it returns
// CANCELLED, which finish() turns into status.cancelled
std::atomic<bool> cancel_requested{false};
const std::string CANCELLED = "\x01" "cancelled";

bool cancelling() { return cancel_requested.load(std::memory_order_relaxed); }

bool exists(const std::string &path) {
    struct stat st{};
    return lstat(path.c_str(), &st) == 0;
}

std::string base_of(const std::string &path) {
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

std::string parent_of(const std::string &path) {
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos || slash == 0 ? "/" : path.substr(0, slash);
}

std::string why(const char *what, const std::string &path) {
    return std::string(what) + " " + base_of(path) + ": " + std::strerror(errno);
}

void set_current(const std::string &path) {
    std::lock_guard guard(lock);
    status.current = base_of(path);
}

void add_done(long long n) {
    std::lock_guard guard(lock);
    status.done += n;
}

// bytes in files below path (or its size, a file)
long long tree_size(const std::string &path, long long &items) {
    struct stat st{};
    if (lstat(path.c_str(), &st) != 0)
        return 0;
    items++;
    if (!S_ISDIR(st.st_mode))
        return st.st_size;
    long long total = 0;
    if (DIR *dir = opendir(path.c_str())) {
        while (dirent *e = readdir(dir)) {
            if (!std::strcmp(e->d_name, ".") || !std::strcmp(e->d_name, ".."))
                continue;
            total += tree_size(path + "/" + e->d_name, items);
        }
        closedir(dir);
    }
    return total;
}

std::string copy_file(const std::string &from, const std::string &to, mode_t mode) {
    // (on the heap while copying: a static buffer was 4 MiB of the program image, which comes out of
    // the flexible memory pool Wisp's processes share)
    constexpr size_t BUFFER = 4 << 20;
    std::unique_ptr<char[]> holder(new (std::nothrow) char[BUFFER]);
    if (!holder)
        return "Not enough memory to copy " + from;
    char *const buffer = holder.get();
    const int in = open(from.c_str(), O_RDONLY);
    if (in < 0)
        return why("Could not open", from);
    const int out = open(to.c_str(), O_WRONLY | O_CREAT | O_TRUNC, mode & 0777 ? mode & 0777 : 0666);
    if (out < 0) {
        const std::string e = why("Could not create", to);
        close(in);
        return e;
    }
    set_current(from);
    std::string error;
    for (;;) {
        if (cancelling()) {
            error = CANCELLED;  // (the part copied goes, as below)
            break;
        }
        const ssize_t n = read(in, buffer, BUFFER);
        if (n == 0)
            break;
        if (n < 0) {
            error = why("Could not read", from);
            break;
        }
        if (write(out, buffer, n) != n) {
            error = why("Could not write", to);  // (full, most likely)
            break;
        }
        add_done(n);
    }
    close(in);
    close(out);
    if (!error.empty())
        unlink(to.c_str());
    return error;
}

std::string copy_tree(const std::string &from, const std::string &to) {
    struct stat st{};
    if (lstat(from.c_str(), &st) != 0)
        return why("Could not read", from);
    if (!S_ISDIR(st.st_mode))
        return copy_file(from, to, st.st_mode);
    if (mkdir(to.c_str(), 0777) != 0 && errno != EEXIST)
        return why("Could not create", to);
    DIR *dir = opendir(from.c_str());
    if (!dir)
        return why("Could not open", from);
    std::string error;
    while (dirent *e = readdir(dir)) {
        if (!std::strcmp(e->d_name, ".") || !std::strcmp(e->d_name, ".."))
            continue;
        error = copy_tree(from + "/" + e->d_name, to + "/" + e->d_name);
        if (!error.empty())
            break;
    }
    closedir(dir);
    return error;
}

std::string remove_tree(const std::string &path) {
    if (cancelling())
        return CANCELLED;
    struct stat st{};
    if (lstat(path.c_str(), &st) != 0)
        return why("Could not read", path);
    set_current(path);
    if (S_ISDIR(st.st_mode)) {
        if (DIR *dir = opendir(path.c_str())) {
            std::vector<std::string> names;
            while (dirent *e = readdir(dir))
                if (std::strcmp(e->d_name, ".") && std::strcmp(e->d_name, ".."))
                    names.push_back(e->d_name);
            closedir(dir);
            for (const std::string &n : names) {
                const std::string error = remove_tree(path + "/" + n);
                if (!error.empty())
                    return error;
            }
        }
        if (rmdir(path.c_str()) != 0)
            return why("Could not delete", path);
    } else if (unlink(path.c_str()) != 0) {
        return why("Could not delete", path);
    }
    add_done(1);
    return {};
}

// "name - Copy.ext", "name - Copy (2).ext": a name for a copy in the same folder
std::string copy_name(const std::string &folder, const std::string &name, bool dir) {
    const size_t dot = dir ? std::string::npos : name.find_last_of('.');
    const std::string stem = dot == std::string::npos || dot == 0 ? name : name.substr(0, dot);
    const std::string ext = dot == std::string::npos || dot == 0 ? "" : name.substr(dot);
    for (int i = 1;; i++) {
        const std::string candidate = stem + " - Copy" + (i > 1 ? " (" + std::to_string(i) + ")" : "") + ext;
        if (!exists(folder + "/" + candidate))
            return candidate;
    }
}

void finish(const std::string &error, std::vector<std::string> touched) {
    std::lock_guard guard(lock);
    status.running = false;
    status.finished = true;
    status.cancelled = error == CANCELLED || (cancelling() && !status.password_needed);
    status.error = status.cancelled ? std::string() : error;
    if (status.cancelled || !status.error.empty() || status.password_needed)
        status.open_after.clear();
    status.touched = std::move(touched);
}

void paste_job(std::vector<std::string> sources, bool cut, std::string folder) {
    long long items = 0, total = 0;
    for (const std::string &s : sources)
        total += tree_size(s, items);
    {
        std::lock_guard guard(lock);
        status.total = total;
    }
    std::vector<std::string> touched{folder};
    std::string error;
    for (const std::string &from : sources) {
        if (cancelling()) {
            error = CANCELLED;
            break;
        }
        if (folder == from || folder.rfind(from + "/", 0) == 0) {
            error = "The folder " + base_of(from) + " can't go inside itself";
            break;
        }
        const std::string name = base_of(from);
        struct stat st{};
        lstat(from.c_str(), &st);
        std::string to = folder + "/" + name;
        if (cut && parent_of(from) == folder)
            continue;  // already there
        if (exists(to))
            to = folder + "/" + copy_name(folder, name, S_ISDIR(st.st_mode));
        if (cut) {
            touched.push_back(parent_of(from));
            if (rename(from.c_str(), to.c_str()) == 0) {
                long long n = 0;
                add_done(tree_size(to, n));
                continue;
            }
            // another file system: copy, then delete the original
        }
        error = copy_tree(from, to);
        if (error.empty() && cut)
            error = remove_tree(from);
        if (!error.empty())
            break;
    }
    finish(error, touched);
}

void delete_job(std::vector<std::string> paths) {
    long long items = 0;
    for (const std::string &p : paths)
        tree_size(p, items);
    {
        std::lock_guard guard(lock);
        status.total = items;
    }
    std::vector<std::string> touched;
    std::string error;
    for (const std::string &p : paths) {
        touched.push_back(parent_of(p));
        error = remove_tree(p);
        if (!error.empty())
            break;
    }
    finish(error, touched);
}

bool start(const std::string &title) {
    std::lock_guard guard(lock);
    if (status.running)
        return false;
    status = JobStatus{};
    status.running = true;
    status.title = title;
    cancel_requested.store(false);
    return true;
}

// A job of another kind on a thread of its own: 1 MiB of stack (libarchive's decoders; the kit's default
// for a thread is 256 KiB)
struct RunJob {
    std::function<std::string(JobControl &)> work;
    std::vector<std::string> touched;
};

void *run_job(void *arg) {
    std::unique_ptr<RunJob> job(static_cast<RunJob *>(arg));
    JobControl control;
    std::string error = job->work(control);
    finish(error, std::move(job->touched));
    return nullptr;
}

std::string items_text(size_t n) { return std::to_string(n) + (n == 1 ? " item" : " items"); }

} // namespace

std::string unique_name(const std::string &dir, const std::string &stem, const std::string &extension) {
    for (int i = 1;; i++) {
        const std::string name = stem + (i > 1 ? " (" + std::to_string(i) + ")" : "") + extension;
        if (!exists(dir + "/" + name))
            return name;
    }
}

std::string make_folder(const std::string &path) {
    return mkdir(path.c_str(), 0777) == 0 ? std::string() : why("Could not create", path);
}

std::string make_file(const std::string &path) {
    const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0666);
    if (fd < 0)
        return why("Could not create", path);
    close(fd);
    return {};
}

std::string rename_entry(const std::string &from, const std::string &to) {
    if (exists(to))
        return base_of(to) + " is already there";
    return rename(from.c_str(), to.c_str()) == 0 ? std::string() : why("Could not rename", from);
}

long long folder_size(const std::string &path, long long &items) { return tree_size(path, items); }

bool job_paste(const std::vector<std::string> &sources, bool cut, const std::string &folder) {
    if (!start((cut ? "Moving " : "Copying ") + items_text(sources.size())))
        return false;
    std::thread(paste_job, sources, cut, folder).detach();
    return true;
}

bool job_delete(const std::vector<std::string> &paths) {
    if (!start("Deleting " + items_text(paths.size())))
        return false;
    {
        std::lock_guard guard(lock);
        status.counts_items = true;
    }
    std::thread(delete_job, paths).detach();
    return true;
}

bool job_run(const std::string &title, std::function<std::string(JobControl &)> work, std::vector<std::string> touched) {
    if (!start(title))
        return false;
    auto *job = new RunJob{std::move(work), std::move(touched)};
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, static_cast<size_t>(1) << 20);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_t thread;
    const int result = pthread_create(&thread, &attr, run_job, job);
    pthread_attr_destroy(&attr);
    if (result != 0) {
        delete job;
        finish("No thread could start for it (" + std::string(std::strerror(result)) + ")", {});
    }
    return true;
}

void job_cancel() {
    std::lock_guard guard(lock);
    if (!status.running)
        return;
    cancel_requested.store(true);
    status.cancelling = true;
}

void JobControl::set_total(long long n) {
    std::lock_guard guard(lock);
    status.total = n;
}

void JobControl::add_done(long long n) {
    std::lock_guard guard(lock);
    status.done += n;
}

void JobControl::set_done(long long n) {
    std::lock_guard guard(lock);
    status.done = n;
}

void JobControl::set_current(const std::string &name) {
    std::lock_guard guard(lock);
    status.current = name;
}

void JobControl::set_counts_items(bool items) {
    std::lock_guard guard(lock);
    status.counts_items = items;
}

bool JobControl::cancelled() const { return cancelling(); }

void JobControl::need_password(bool wrong) {
    std::lock_guard guard(lock);
    status.password_needed = true;
    status.wrong_password = wrong;
}

void JobControl::open_after(const std::string &path) {
    std::lock_guard guard(lock);
    status.open_after = path;
}

JobStatus job_status() {
    std::lock_guard guard(lock);
    return status;
}

void job_acknowledge() {
    std::lock_guard guard(lock);
    status.finished = false;
}

} // namespace shell
