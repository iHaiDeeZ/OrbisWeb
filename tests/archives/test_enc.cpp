// OrbisWeb: encrypted archives through Files' code (archives.cpp + fileops.cpp) on the PC
// (tests/archives/run.sh), against Wisp's own host libarchive build (same options as the PS4's,
// OpenSSL's libcrypto) and, with "rar", UnRAR's: for each archive made by
// 7-Zip / WinRAR with the password "secret": is the list readable, what an extraction without a
// password says, with a wrong one, and with the right one (the result compared with the source).
#include "archives.h"
#include "fileops.h"

#include <clocale>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unistd.h>

extern "C" void wisp_log(const char *format, ...) {
    va_list a;
    va_start(a, format);
    std::fprintf(stderr, "  log: ");
    std::vfprintf(stderr, format, a);
    std::fputc('\n', stderr);
    va_end(a);
}

namespace {

std::string W;

// The last job's progress as the job card saw it (sampled while it ran): did "done" ever go back, or past
// the total? (v00.27's RAR progress stopped moving inside files over ~3 GiB: a 64-bit overflow)
struct Progress {
    bool backwards = false, over = false;
    long long last = 0, samples = 0;
} progress;

shell::JobStatus wait_job() {
    progress = {};
    for (int i = 0; i < 600000; i++) {
        shell::JobStatus s = shell::job_status();
        if (s.done < progress.last)
            progress.backwards = true;
        if (s.total > 0 && s.done > s.total)
            progress.over = true;
        progress.last = s.done;
        progress.samples++;
        if (!s.running && s.finished) {
            shell::job_acknowledge();
            return s;
        }
        usleep(100);
    }
    std::printf("FAIL: a job never ended\n");
    std::exit(2);
}

// The bytes of the files below a folder
long long tree_bytes(const std::string &dir) {
    long long n = 0;
    FILE *p = popen(("find '" + dir + "' -type f -printf '%s\\n'").c_str(), "r");
    char line[64];
    while (p && std::fgets(line, sizeof line, p))
        n += std::atoll(line);
    if (p)
        pclose(p);
    return n;
}

int failures = 0;

// RAR's progress counts the unpacked bytes of the files taken out: it ends at its total, which is theirs
void check_rar_progress(const char *what, const shell::JobStatus &s, long long expected_total) {
    const bool ok = !progress.backwards && !progress.over && s.done == s.total && s.total == expected_total;
    std::printf("   progress: %lld of %lld (the files: %lld), %s%s, %lld samples -> %s\n", s.done, s.total, expected_total,
                progress.backwards ? "went back" : "only forward", progress.over ? ", PAST THE TOTAL" : "", progress.samples,
                ok ? "right" : "WRONG");
    if (!ok) {
        std::printf("   ** %s: the progress is wrong\n", what);
        failures++;
    }
}

std::shared_ptr<const wisp::archive::Index> wait_index(const std::string &file, const std::string &password = "") {
    for (int i = 0; i < 4000; i++) {
        if (auto x = wisp::archive::index_of(file, password))
            return x;
        usleep(5000);
    }
    return nullptr;
}

bool same(const std::string &a, const std::string &b) { return std::system(("diff -r '" + a + "' '" + b + "' >/dev/null 2>&1").c_str()) == 0; }

const char *state(const shell::JobStatus &s) {
    if (s.password_needed)
        return s.wrong_password ? "asks again (wrong password)" : "asks for a password";
    if (!s.error.empty())
        return "fails";
    return "extracts";
}

bool rar_mode = false;

bool is_rar_name(const std::string &name) {
    return name.size() > 4 && (name.compare(name.size() - 4, 4, ".rar") == 0);
}

void one(const std::string &name, bool expect_ok) {
    const std::string file = W + "/" + name;
    std::printf("== %s\n", name.c_str());
    auto x = wait_index(file);
    if (!x) {
        std::printf("   list: never came\n");
        failures++;
        return;
    }
    int locked = 0;
    for (const auto &it : x->items)
        locked += it.encrypted;
    std::printf("   list (no password): %zu entries, %d marked locked, needs_password %d, error '%s', format '%s'\n", x->items.size(),
                locked, x->needs_password, x->error.c_str(), x->format.c_str());
    if (x->needs_password) {
        auto w = wait_index(file, "wrong");
        std::printf("   list (wrong password): %zu entries, needs_password %d wrong %d, error '%s'\n", w ? w->items.size() : 0,
                    w ? w->needs_password : -1, w ? w->wrong_password : -1, w ? w->error.c_str() : "?");
        if (!w || !w->needs_password || !w->wrong_password) {
            std::printf("   ** a wrong password should ask again\n");
            failures++;
        }
        auto y = wait_index(file, "secret");
        std::printf("   list (password): %zu entries, needs_password %d wrong %d, error '%s'\n", y ? y->items.size() : 0,
                    y ? y->needs_password : -1, y ? y->wrong_password : -1, y ? y->error.c_str() : "?");
    }
    const std::string out = W + "/out-" + name;
    std::system(("rm -rf '" + out + "'*").c_str());
    shell::JobStatus s;
    wisp::archive::job_extract(file, {}, "", out + "-none", "", "test", "");
    s = wait_job();
    std::printf("   no password:    %s %s\n", state(s), s.error.c_str());
    wisp::archive::job_extract(file, {}, "", out + "-wrong", "wrong", "test", "");
    s = wait_job();
    std::printf("   wrong password: %s %s\n", state(s), s.error.c_str());
    if (expect_ok && !(s.password_needed && s.wrong_password)) {
        std::printf("   ** a wrong password should ask again\n");
        failures++;
    }
    wisp::archive::job_extract(file, {}, "", out + "-right", "secret", "test", "");
    s = wait_job();
    const bool ok = s.error.empty() && !s.password_needed && same(W + "/src", out + "-right");
    std::printf("   right password: %s %s -> %s\n", state(s), s.error.c_str(), ok ? "SAME AS THE SOURCE" : "not the source");
    if (ok != expect_ok) {
        std::printf("   ** expected %s\n", expect_ok ? "to extract" : "to fail");
        failures++;
    }
    if (ok && rar_mode && is_rar_name(name))
        check_rar_progress(name.c_str(), s, tree_bytes(W + "/src"));
}

}  // namespace

int main(int argc, char **argv) {
    W = argc > 1 ? argv[1] : "/tmp/enc-test";
    setlocale(LC_CTYPE, "C.UTF-8");
    // expectations: argv[2] = "rar" when RAR decryption is in (else RAR/7z are expected to fail)
    const bool rar = argc > 2 && std::string(argv[2]) == "rar";
    rar_mode = rar;
    one("zipcrypto.zip", true);
    one("aes256.zip", true);
    one("aes256-lzma.zip", true);
    one("rar5.rar", rar);
    one("rar5-headers.rar", rar);
    one("rar4.rar", rar);
    one("rar4-headers.rar", rar);
    one("vol5.part1.rar", rar);
    one("enc.7z", false);
    one("enc-headers.7z", false);
    if (rar) {
        one("old4.rar", true);  // RAR 4's old part names: old4.rar, old4.r00, old4.r01
        // a solid archive with copies of identical files (RAR 5 file references), encrypted
        std::printf("== solid-copies.rar\n");
        wisp::archive::job_extract(W + "/solid-copies.rar", {}, "", W + "/out-solid", "secret", "test", "");
        shell::JobStatus s = wait_job();
        const bool solid_ok = s.error.empty() && same(W + "/src/dir", W + "/out-solid/dir") && same(W + "/src/dir", W + "/out-solid/dir2");
        std::printf("   %s %s\n", solid_ok ? "both folders came out whole" : "NOT WHOLE", s.error.c_str());
        failures += !solid_ok;
        check_rar_progress("solid-copies.rar", s, 2 * tree_bytes(W + "/src/dir"));  // (copies count too)
        // a plain RAR (no password)
        std::printf("== plain.rar\n");
        wisp::archive::job_extract(W + "/plain.rar", {}, "", W + "/out-plain", "", "test", "");
        s = wait_job();
        const bool plain_ok = s.error.empty() && !s.password_needed && same(W + "/src", W + "/out-plain");
        std::printf("   %s %s\n", plain_ok ? "same as the source" : "NOT THE SOURCE", s.error.c_str());
        failures += !plain_ok;
        // one folder chosen inside another: its entries land after the folder they were chosen in
        std::printf("== rar5.rar, dir/sub chosen in dir\n");
        wisp::archive::job_extract(W + "/rar5.rar", {"dir/sub"}, "dir", W + "/out-part", "secret", "test", "");
        s = wait_job();
        const bool part_ok = s.error.empty() && same(W + "/src/dir/sub", W + "/out-part/sub") && access((W + "/out-part/a.txt").c_str(), F_OK) != 0;
        std::printf("   %s %s\n", part_ok ? "only sub came out, as sub/" : "WRONG", s.error.c_str());
        failures += !part_ok;
        check_rar_progress("dir/sub chosen", s, tree_bytes(W + "/src/dir/sub"));  // (only what was chosen)
        // opening one file inside (it is taken out, then opened)
        std::printf("== rar5-headers.rar, opening dir/a.txt\n");
        wisp::archive::job_extract(W + "/rar5-headers.rar", {"dir/a.txt"}, "dir", W + "/out-open", "secret", "test", "a.txt");
        s = wait_job();
        const bool open_ok = s.error.empty() && s.open_after == W + "/out-open/a.txt" && same(W + "/src/dir/a.txt", W + "/out-open/a.txt");
        std::printf("   %s (open %s) %s\n", open_ok ? "taken out to be opened" : "WRONG", s.open_after.c_str(), s.error.c_str());
        failures += !open_ok;
        // a part missing from a set
        std::printf("== vol5 without part 2\n");
        std::system(("mkdir -p '" + W + "/short' && cp '" + W + "/vol5.part1.rar' '" + W + "/vol5.part3.rar' '" + W + "/short/'").c_str());
        wisp::archive::job_extract(W + "/short/vol5.part1.rar", {}, "", W + "/out-short", "secret", "test", "");
        s = wait_job();
        const bool short_ok = !s.error.empty() && s.error.find("missing") != std::string::npos;
        std::printf("   %s: %s\n", short_ok ? "says a part is missing" : "WRONG", s.error.c_str());
        failures += !short_ok;
        // Cancel while a file comes out (a 64 MiB file): what was written of it goes
        std::printf("== big.rar, cancelled\n");
        wisp::archive::job_extract(W + "/big.rar", {}, "", W + "/out-cancel", "secret", "test", "");
        for (int i = 0; i < 20000; i++) {  // (once data comes: in the middle of the file)
            const shell::JobStatus now = shell::job_status();
            if (now.done > 0 || now.finished)
                break;
            usleep(200);
        }
        shell::job_cancel();
        s = wait_job();
        const bool cancel_ok = s.cancelled && access((W + "/out-cancel/big.bin").c_str(), F_OK) != 0;
        std::printf("   %s (cancelled %d, error '%s')\n", cancel_ok ? "stopped, no half file left" : "WRONG", s.cancelled, s.error.c_str());
        failures += !cancel_ok;
        // the same file whole: its progress moves inside the file (a stored 64 MiB file, many data blocks)
        std::printf("== big.rar, whole\n");
        wisp::archive::job_extract(W + "/big.rar", {}, "", W + "/out-big", "secret", "test", "");
        s = wait_job();
        const bool big_ok = s.error.empty() && progress.samples > 2;
        std::printf("   %s %s\n", big_ok ? "came out" : "WRONG", s.error.c_str());
        failures += !big_ok;
        check_rar_progress("big.rar", s, 64LL * 1024 * 1024);
    }
    std::printf("%s (%d unexpected)\n", failures ? "UNEXPECTED RESULTS" : "as expected", failures);
    return failures != 0;
}
