// Wisp: the UI and network processes play nothing, but WebKit's code they keep refers to GStreamer
// anyway: GraphicsContext's vtable is live in both (its drawVideoFrame takes a frame's picture through
// GStreamer's sample-to-image code), and so are ImageDecoderGStreamer's paths. From there WebKit's
// ensureGStreamerInitialized reaches gst_init, which registers every plugin GStreamer was built with
// (gst_init_static_plugins) and with them all of FFmpeg's decoders: ~19 MiB of program image in each
// (v00.17's first build: UI 89 -> 112 MiB, network 83 -> 102 MiB), out of the flexible memory pool the
// app's processes share. The link of those two processes sends gst_init here instead (--wrap,
// app/CMakeLists.txt): what stays of GStreamer is what WebKit's kept code calls itself; the plugins and
// FFmpeg go. The web processes keep the real gst_init.
#include <cstddef>

extern "C" {

struct _GError;

// gboolean gst_init_check(int *argc, char ***argv, GError **error): GStreamer is not in this process
int __wrap_gst_init_check(int *, char ***, _GError **)
{
    return 0;
}

// void gst_init(int *argc, char ***argv)
void __wrap_gst_init(int *, char ***)
{
}

}
