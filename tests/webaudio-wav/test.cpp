// Stand-ins for WebKit's AudioBus/RefPtr, then 0048's reader as patched (reader.inc), then the checks.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <memory>
#include <span>
#include <sstream>
#include <string>
#include <vector>

struct AudioChannel {
    std::vector<float> data;
    std::span<float> mutableSpan() { return data; }
};
struct AudioBus;
template<typename T> using RefPtr = std::shared_ptr<T>;
template<typename T> struct Ref {
    std::shared_ptr<T> p;
    T* operator->() const { return p.get(); }
    T& get() const { return *p; }
    operator RefPtr<T>() const { return p; }
};
static int g_resamples, g_mixes;
struct AudioBus {
    std::vector<AudioChannel> channels;
    float rate = 0;
    static Ref<AudioBus> create(unsigned n, size_t length, bool)
    {
        auto bus = std::make_shared<AudioBus>();
        bus->channels.resize(n);
        for (auto& c : bus->channels)
            c.data.assign(length, -99.f);   // (a frame left unwritten shows)
        return { bus };
    }
    void setSampleRate(float r) { rate = r; }
    AudioChannel* channel(unsigned i) { return &channels[i]; }
    static RefPtr<AudioBus> createBySampleRateConverting(const AudioBus& b, bool, double r)
    {
        ++g_resamples;
        auto copy = std::make_shared<AudioBus>(b);
        copy->rate = static_cast<float>(r);
        copy->channels.resize(1);
        copy->channels[0].data.assign(1, 12345.f);   // marks "resampled"
        return copy;
    }
    static Ref<AudioBus> createByMixingToMono(const AudioBus& b)
    {
        ++g_mixes;
        return { std::make_shared<AudioBus>(b) };
    }
};

#include "reader.inc"

static std::vector<uint8_t> readFile(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    return { std::istreambuf_iterator<char>(f), {} };
}

int main(int, char** argv)
{
    const std::string dir = argv[1];
    std::vector<std::string> names;
    if (DIR* d = opendir(dir.c_str())) {
        while (dirent* e = readdir(d)) {
            std::string n = e->d_name;
            if (n.size() > 4 && n.substr(n.size() - 4) == ".wav")
                names.push_back(n.substr(0, n.size() - 4));
        }
        closedir(d);
    }
    std::sort(names.begin(), names.end());
    int failures = 0;
    for (const auto& name : names) {
        auto bytes = readFile(dir + "/" + name + ".wav");
        const bool reject = name.rfind("reject_", 0) == 0;
        g_resamples = g_mixes = 0;
        auto bus = wispBusFromWav(std::span<const uint8_t>(bytes), false, 48000.f);
        if (reject) {
            const bool ok = !bus;
            printf("%-18s %s (left to GStreamer)\n", name.c_str(), ok ? "ok" : "FAIL: read");
            failures += !ok;
            continue;
        }
        std::ifstream t(dir + "/" + name + ".txt");
        unsigned channels, rate, frames;
        t >> channels >> rate >> frames;
        if (!bus) {
            printf("%-18s FAIL: not read\n", name.c_str());
            ++failures;
            continue;
        }
        if (rate != 48000) {
            const bool ok = g_resamples == 1 && bus->rate == 48000.f;
            printf("%-18s %s (resampled to 48000)\n", name.c_str(), ok ? "ok" : "FAIL");
            failures += !ok;
            continue;
        }
        double worst = 0;
        bool shape = bus->channels.size() == channels && bus->rate == static_cast<float>(rate);
        for (unsigned c = 0; shape && c < channels; ++c) {
            shape = bus->channels[c].data.size() == frames;
            for (unsigned f = 0; shape && f < frames; ++f) {
                double want;
                t >> want;
                worst = std::max(worst, std::fabs(bus->channels[c].data[f] - want));
            }
        }
        const bool ok = shape && worst < 1e-6;
        printf("%-18s %s (%u ch, %u frames, worst error %.2g)\n", name.c_str(), ok ? "ok" : "FAIL", channels, frames, worst);
        failures += !ok;
    }
    printf("%d failure(s)\n", failures);
    return failures != 0;
}
