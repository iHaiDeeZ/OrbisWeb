#!/usr/bin/env bash
# Wisp: media libraries for WebKit's <video> and Media Source Extensions on the PS4 (static, -fPIC),
# into ~/wisp-build/media: FFmpeg (decoders, demuxers, parsers; no programs, devices, network or
# hardware acceleration), GStreamer 1.26 (core, -base, -good, a -bad subset, gst-libav) and the Xiph
# libraries its Ogg/Vorbis/Theora/Opus elements need, plus libgstreamer-full-1.0.a: every GStreamer
# library and plugin in one archive, with the gst_init_static_plugins () that gst_init () calls
# (WebKit's USE_GSTREAMER_FULL build links it; the app needs no registration calls).
#
#   bash ~/wisp/scripts/build-media.sh              # everything not built yet (build/media/<name>.done)
#   bash ~/wisp/scripts/build-media.sh ffmpeg full  # just these (again)
#   bash ~/wisp/scripts/build-media.sh fetch        # only download the sources
#
# Sources: ~/wisp-build/src/<tarball>, downloaded when missing and checked against the SHA-256 sums
# below. Build folders: ~/wisp-build/build/media/<name>. Logs: ~/wisp-build/logs/media-<name>.log.
# .pc files: ~/wisp-build/media/lib/pkgconfig (plugins' own: lib/gstreamer-1.0/pkgconfig).
# The app links ~/wisp-build/media: put PKG_CONFIG_LIBDIR's (and -L) media entries ahead of the
# kit's sysroot, which has an older FFmpeg (7.1) of its own.
#
# WISP_MEDIA_JOBS (default 6): another WebKit build may run at the same time (WSL has 15 GB).
# WISP_MEDIA, WISP_MEDIA_BUILD, WISP_MEDIA_LOGTAG: another prefix, build folder and log name (a
# second, side-by-side build; the sources are shared).
set -euo pipefail
source "$HOME/wisp/scripts/env.sh"
MEDIA="${WISP_MEDIA:-$WB/media}"
S="$WISP_SRC"; B="${WISP_MEDIA_BUILD:-$WB/build/media}"; L="$WISP_LOGS"; LOGTAG="${WISP_MEDIA_LOGTAG:-media}"
JOBS="${WISP_MEDIA_JOBS:-6}"
HOST_TRIPLE=x86_64-pc-freebsd12
mkdir -p "$B" "$L" "$MEDIA/lib/pkgconfig"
# Ours first: the kit's sysroot has an FFmpeg 7.1 of its own (libavcodec 61), which must never be found.
export PKG_CONFIG_LIBDIR="$MEDIA/lib/pkgconfig:$PKG_CONFIG_LIBDIR"
# One section per function and per datum, as WebKit: the app's --gc-sections drops what it never reaches.
MEDIA_CFLAGS="-ffunction-sections -fdata-sections"
CROSS="$B/media-cross.ini"

FFMPEG_VER=8.0.3
GST_VER=1.26.11
OGG_VER=1.3.6
VORBIS_VER=1.3.7
THEORA_VER=1.2.0
OPUS_VER=1.6.1
# file|url|sha256 (FFmpeg's: of the tarball whose GPG signature checked out, 2026-11-20)
SOURCES=(
    "ffmpeg-$FFMPEG_VER.tar.xz|https://ffmpeg.org/releases/ffmpeg-$FFMPEG_VER.tar.xz|6136812ea6d4e68bdba27e33c2a94382711cdf4f8602ffef056ff792bd6f9818"
    "gstreamer-$GST_VER.tar.xz|https://gstreamer.freedesktop.org/src/gstreamer/gstreamer-$GST_VER.tar.xz|2e0bd192d0438ea606a6f76a95c8e16542167656ffec2c2bc3aaf6ee0837fbf6"
    "gst-plugins-base-$GST_VER.tar.xz|https://gstreamer.freedesktop.org/src/gst-plugins-base/gst-plugins-base-$GST_VER.tar.xz|fc50f885d41f5d0407ce0876ec7235d9e7b82d48db2f4bc72c5f244a4ac79263"
    "gst-plugins-good-$GST_VER.tar.xz|https://gstreamer.freedesktop.org/src/gst-plugins-good/gst-plugins-good-$GST_VER.tar.xz|001deb0876d5d743cd3448abf74a27adec3fd850012fcb1b00994861bd6c1145"
    "gst-plugins-bad-$GST_VER.tar.xz|https://gstreamer.freedesktop.org/src/gst-plugins-bad/gst-plugins-bad-$GST_VER.tar.xz|110fb82795f0e569b1e27b12ab9699d35c7762e1ff4db95335d6ac8d1442af3d"
    "gst-libav-$GST_VER.tar.xz|https://gstreamer.freedesktop.org/src/gst-libav/gst-libav-$GST_VER.tar.xz|9bb3d2681ef0de92d1b1a9d955186236ee22e64f372db9bfc0d22e2d0df19865"
    "libogg-$OGG_VER.tar.xz|https://downloads.xiph.org/releases/ogg/libogg-$OGG_VER.tar.xz|5c8253428e181840cd20d41f3ca16557a9cc04bad4a3d04cce84808677fa1061"
    "libvorbis-$VORBIS_VER.tar.xz|https://downloads.xiph.org/releases/vorbis/libvorbis-$VORBIS_VER.tar.xz|b33cc4934322bcbf6efcbacf49e3ca01aadbea4114ec9589d1b1e9d20f72954b"
    "libtheora-$THEORA_VER.tar.xz|https://downloads.xiph.org/releases/theora/libtheora-$THEORA_VER.tar.xz|ebdf77a8f5c0a8f7a9e42323844fa09502b34eb1d1fece7b5f54da41fe2122ec"
    "opus-$OPUS_VER.tar.gz|https://downloads.xiph.org/releases/opus/opus-$OPUS_VER.tar.gz|6ffcb593207be92584df15b32466ed64bbec99109f007c82205f0194572411a1"
)

# --- helpers --------------------------------------------------------------------------------------
fetch() {
    local e name url sum
    for e in "${SOURCES[@]}"; do
        IFS='|' read -r name url sum <<<"$e"
        if [ ! -f "$S/$name" ]; then
            echo "-- download $url"
            curl -fL --retry 3 -sS -o "$S/.$name.part" "$url"
            mv "$S/.$name.part" "$S/$name"
        fi
        echo "$sum  $S/$name" | sha256sum -c --quiet - || { echo "bad checksum: $S/$name" >&2; return 1; }
    done
}
unpack() {  # unpack <tarball> <dir>: the source tree $S/<dir>, unpacked once
    [ -d "$S/$2" ] || { mkdir -p "$S/.tmp.$2"; tar xf "$S/$1" -C "$S/.tmp.$2";
        mv "$S/.tmp.$2/"* "$S/$2"; rmdir "$S/.tmp.$2"; }
}
cross_file() {  # Wisp's meson cross file + the section flags, our .pc files first
    sed -E -e "s#^(c_args = \[.*)\]\$#\1, '-ffunction-sections', '-fdata-sections']#" \
        -e "s#^(cpp_args = \[.*)\]\$#\1, '-ffunction-sections', '-fdata-sections']#" \
        -e "s#^pkg_config_libdir = '#pkg_config_libdir = '$MEDIA/lib/pkgconfig:#" \
        "$WB/wisp-cross.ini" > "$CROSS.tmp"
    [ "$(grep -c "'-fdata-sections'" "$CROSS.tmp")" = 2 ] && grep -q "^pkg_config_libdir = '$MEDIA/lib/pkgconfig:" "$CROSS.tmp" \
        || { echo "cross_file: $WB/wisp-cross.ini changed shape" >&2; return 1; }
    mv "$CROSS.tmp" "$CROSS"
}
ms() {  # ms <name> <srcdir> [meson options]: meson cross build + install into $MEDIA
    local name=$1 src=$2; shift 2
    cross_file
    rm -rf "$B/$name"
    meson setup "$B/$name" "$src" --cross-file "$CROSS" --prefix "$MEDIA" --libdir lib \
        -Ddefault_library=static -Dbuildtype=release -Db_ndebug=true --wrap-mode=nodownload \
        -Dpkg_config_path="$MEDIA/lib/pkgconfig" "$@"
    ninja -C "$B/$name" -j"$JOBS"
    meson install -C "$B/$name" --no-rebuild
    # meson puts a FreeBSD host's .pc files in libdata/pkgconfig: one folder for all of them
    # (their paths are prefix-relative, so they can move)
    if [ -d "$MEDIA/libdata/pkgconfig" ]; then
        mv -f "$MEDIA/libdata/pkgconfig/"*.pc "$MEDIA/lib/pkgconfig/"
        rmdir "$MEDIA/libdata/pkgconfig" "$MEDIA/libdata"
    fi
}
ac() {  # ac <name> <srcdir> [configure options]: autotools cross build + install into $MEDIA
    local name=$1 src=$2; shift 2
    rm -rf "$B/$name"; mkdir -p "$B/$name"; cd "$B/$name"
    env -u CXXFLAGS -u CPPFLAGS -u LDFLAGS -u LIBS CFLAGS="-O2 -g1 $MEDIA_CFLAGS" \
        CC=ps4-cc CXX=ps4-c++ AR=llvm-ar-$CLANG_VER RANLIB=llvm-ranlib-$CLANG_VER NM=llvm-nm-$CLANG_VER \
        STRIP=llvm-strip-$CLANG_VER OBJDUMP=llvm-objdump-$CLANG_VER CC_FOR_BUILD=gcc CPP_FOR_BUILD="gcc -E" \
        PKG_CONFIG=ps4-pkg-config \
        "$src/configure" --host=$HOST_TRIPLE --build=x86_64-pc-linux-gnu --prefix="$MEDIA" \
        --libdir="$MEDIA/lib" --disable-shared --enable-static --with-pic "$@"
    make -j"$JOBS"
    make install
    cd - >/dev/null
}
csv() { local IFS=,; echo "$*"; }
apply_patch() {  # apply_patch <srcdir> <function printing a -p1 diff>: once (a reverse dry run
                 # that applies means it is in already)
    if "$2" | patch -d "$1" -p1 -R --dry-run -s -f >/dev/null 2>&1; then return 0; fi
    "$2" | patch -d "$1" -p1 -N -s
}

# --- Xiph libraries (Ogg demuxing, Vorbis and Theora decoding, the Opus parser) -------------------
b_ogg() {
    unpack libogg-$OGG_VER.tar.xz libogg
    ac ogg "$S/libogg"
}
b_vorbis() {
    unpack libvorbis-$VORBIS_VER.tar.xz libvorbis
    ac vorbis "$S/libvorbis" --disable-docs --disable-examples --disable-oggtest
}
b_theora() {
    unpack libtheora-$THEORA_VER.tar.xz libtheora
    ac theora "$S/libtheora" --disable-doc --disable-spec --disable-examples --disable-oggtest \
        --disable-vorbistest --disable-sdltest --disable-telemetry
}
b_opus() {  # (opusparse uses opus_packet_parse () only)
    unpack opus-$OPUS_VER.tar.gz opus
    ms opus "$S/opus" -Dtests=disabled -Ddocs=disabled -Dextra-programs=disabled -Dcustom-modes=false
}

# --- FFmpeg --------------------------------------------------------------------------------------
# What gst-libav uses: avdec_* (audio and video decoders; it skips vorbis, theora, mpeg1video,
# mp1/mp2, the subtitle decoders and PCM, has its own deinterlacer on libavfilter's yadif, and
# registers none of these demuxers: GStreamer's own do the demuxing). The rest is for direct use.
FF_DECODERS=(h264 hevc vp8 vp9 mpeg4 mpeg2video mpeg1video msmpeg4v3 theora vc1 wmv3
    aac aac_latm mp3 mp3float mp2 mp2float opus vorbis flac ac3 eac3 dca truehd mlp alac wmav2
    pcm_s16le pcm_s16be pcm_s24le pcm_s24be pcm_s32le pcm_f32le pcm_f64le pcm_u8 pcm_s8 pcm_alaw pcm_mulaw
    ass ssa srt subrip movtext webvtt dvdsub pgssub dvbsub)
FF_DEMUXERS=(mov matroska avi mpegts mpegps flv ogg wav mp3 aac flac ac3 eac3 ass srt webvtt h264 hevc)
FF_PARSERS=(h264 hevc vp8 vp9 aac aac_latm mpegaudio opus vorbis flac ac3 dca mpeg4video mpegvideo vc1 mlp)
FF_BSFS=(h264_mp4toannexb hevc_mp4toannexb aac_adtstoasc vp9_superframe_split extract_extradata)
FF_FILTERS=(yadif)   # gst-libav's avdeinterlace (buffer and buffersink are always built)
# The patch to FFmpeg 8.0.3 (applied once; checked on the PC: patched and pristine host builds of
# libavutil give bit-identical FFT/MDCT output, float/double/int32, all lengths, C and AVX/FMA3 code):
# - libavutil TX: the power of two tables from 2048 points up (ff_tx_tab_<N>_{float,double,int32}
#   up to 2097152, ~16 MiB of .bss in any program that links libavutil; a PS4 program's .bss is
#   charged in full to the memory pool the app's processes share) become pointers, av_malloc'ed by
#   their ff_tx_init_tab_<N> (run once through ff_thread_once) the first time a transform needs
#   them; ff_tx_init_tabs_* return AVERROR(ENOMEM) if that fails, and their callers pass it on.
#   The x86 AVX split-radix FFT loads those pointers (it read the arrays' addresses). Tables up to
#   1024 points stay static (~8 KiB in all; the assembly reads them at fixed addresses). x86 only:
#   aarch64's NEON FFT would need the same change.
# - libavformat/mov.c: mov_read_chnl () returned an uninitialized `ret` (it drops ff_mov_read_chnl
#   ()'s result), so a 'chnl' box could fail a whole MP4; it now ignores the box's errors, as meant.
ffmpeg_patch() {
    cat <<'EOF'
--- a/libavformat/mov.c
+++ b/libavformat/mov.c
@@ -1214,7 +1214,7 @@
                 end - avio_tell(pb));
         avio_seek(pb, end, SEEK_SET);
     }
-    return ret;
+    return 0;
 }

 static int mov_read_wfex(MOVContext *c, AVIOContext *pb, MOVAtom atom)
--- a/libavutil/tx_priv.h
+++ b/libavutil/tx_priv.h
@@ -367,10 +367,11 @@
                                         int basis, int dual_stride);

 /* Typed init function to initialize shared tables. Will initialize all tables
- * for all factors of a length. */
-void ff_tx_init_tabs_float (int len);
-void ff_tx_init_tabs_double(int len);
-void ff_tx_init_tabs_int32 (int len);
+ * for all factors of a length. Returns AVERROR(ENOMEM) when a table could not
+ * be allocated (power of two tables from 2048 points up), 0 otherwise. */
+int ff_tx_init_tabs_float (int len);
+int ff_tx_init_tabs_double(int len);
+int ff_tx_init_tabs_int32 (int len);

 /* Typed init function to initialize an MDCT exptab in a context.
  * If pre_tab is set, duplicates the entire table, with the first
--- a/libavutil/tx_template.c
+++ b/libavutil/tx_template.c
@@ -29,7 +29,12 @@
 #define TABLE_DEF(name, size) \
     DECLARE_ALIGNED(32, TXSample, TX_TAB(ff_tx_tab_ ##name))[size]

-#define SR_POW2_TABLES \
+/* Power of two tables. Up to 1024 points static (~2 KiB for each sample type;
+ * the x86 AVX FFT reads those at fixed addresses), from 2048 points up
+ * allocated the first time a transform needs them: as static arrays they took
+ * ~16 MiB of .bss (all three sample types) in every program that links
+ * libavutil. */
+#define SR_POW2_TABLES_STATIC \
     SR_TABLE(8)        \
     SR_TABLE(16)       \
     SR_TABLE(32)       \
@@ -38,6 +43,8 @@
     SR_TABLE(256)      \
     SR_TABLE(512)      \
     SR_TABLE(1024)     \
+
+#define SR_POW2_TABLES_ALLOC \
     SR_TABLE(2048)     \
     SR_TABLE(4096)     \
     SR_TABLE(8192)     \
@@ -50,10 +57,18 @@
     SR_TABLE(1048576)   \
     SR_TABLE(2097152)   \

+#define SR_POW2_TABLES \
+    SR_POW2_TABLES_STATIC \
+    SR_POW2_TABLES_ALLOC
+
 #define SR_TABLE(len) \
     TABLE_DEF(len, len/4 + 1);
 /* Power of two tables */
-SR_POW2_TABLES
+SR_POW2_TABLES_STATIC
+#undef SR_TABLE
+#define SR_TABLE(len) \
+    TXSample *TX_TAB(ff_tx_tab_ ##len);
+SR_POW2_TABLES_ALLOC
 #undef SR_TABLE

 /* Other factors' tables */
@@ -77,7 +92,24 @@
                                                                    \
     *tab = 0;                                                      \
 }
-SR_POW2_TABLES
+SR_POW2_TABLES_STATIC
+#undef SR_TABLE
+
+#define SR_TABLE(len)                                              \
+static av_cold void TX_TAB(ff_tx_init_tab_ ##len)(void)            \
+{                                                                  \
+    double freq = 2*M_PI/len;                                      \
+    TXSample *tab = av_malloc((len/4 + 1)*sizeof(*tab));           \
+                                                                   \
+    if (!tab)                                                      \
+        return;                                                    \
+    for (int i = 0; i < len/4; i++)                                \
+        tab[i] = RESCALE(cos(i*freq));                             \
+                                                                   \
+    tab[len/4] = 0;                                                \
+    TX_TAB(ff_tx_tab_ ##len) = tab;                                \
+}
+SR_POW2_TABLES_ALLOC
 #undef SR_TABLE

 static void (*const sr_tabs_init_funcs[])(void) = {
@@ -92,6 +124,14 @@
 #undef SR_TABLE
 };

+/* The allocated tables, which follow the static ones in the arrays above */
+static TXSample *const *const sr_tabs_alloc[] = {
+#define SR_TABLE(len) &TX_TAB(ff_tx_tab_ ##len),
+    SR_POW2_TABLES_ALLOC
+#undef SR_TABLE
+};
+#define SR_TABS_STATIC ((int)(FF_ARRAY_ELEMS(sr_tabs_init_funcs) - FF_ARRAY_ELEMS(sr_tabs_alloc)))
+
 static av_cold void TX_TAB(ff_tx_init_tab_53)(void)
 {
     /* 5pt, doubled to eliminate AVX lane shuffles */
@@ -145,14 +185,17 @@
     AV_ONCE_INIT,
 };

-av_cold void TX_TAB(ff_tx_init_tabs)(int len)
+av_cold int TX_TAB(ff_tx_init_tabs)(int len)
 {
     int factor_2 = ff_ctz(len);
     if (factor_2) {
         int idx = factor_2 - 3;
-        for (int i = 0; i <= idx; i++)
+        for (int i = 0; i <= idx; i++) {
             ff_thread_once(&sr_tabs_init_once[i],
                             sr_tabs_init_funcs[i]);
+            if (i >= SR_TABS_STATIC && !*sr_tabs_alloc[i - SR_TABS_STATIC])
+                return AVERROR(ENOMEM);
+        }
         len >>= factor_2;
     }

@@ -160,7 +203,7 @@
         int f, f_idx = 0;

         if (len <= 1)
-            return;
+            return 0;

         while ((f = nptwo_tabs_init_data[i].factors[f_idx++])) {
             if (f % len)
@@ -172,6 +215,8 @@
             break;
         }
     }
+
+    return 0;
 }

 static av_always_inline void fft3(TXComplex *out, TXComplex *in,
@@ -486,8 +531,9 @@
                                                   int len, int inv,
                                                   const void *scale)
 {
-    int ret = 0;
-    TX_TAB(ff_tx_init_tabs)(len);
+    int ret = TX_TAB(ff_tx_init_tabs)(len);
+    if (ret < 0)
+        return ret;

     if (len == 15)
         ret = ff_tx_gen_pfa_input_map(s, opts, 3, 5);
@@ -596,7 +642,9 @@
                                                       int len, int inv,
                                                       const void *scale)
 {
-    TX_TAB(ff_tx_init_tabs)(len);
+    int ret = TX_TAB(ff_tx_init_tabs)(len);
+    if (ret < 0)
+        return ret;
     return ff_tx_gen_ptwo_revtab(s, opts);
 }

@@ -1471,9 +1519,7 @@
     if (!(s->tmp = av_malloc(len*sizeof(*s->tmp))))
         return AVERROR(ENOMEM);

-    TX_TAB(ff_tx_init_tabs)(len / sub_len);
-
-    return 0;
+    return TX_TAB(ff_tx_init_tabs)(len / sub_len);
 }

 #define DECL_COMP_IMDCT(N)                                                     \
--- a/libavutil/x86/tx_float.asm
+++ b/libavutil/x86/tx_float.asm
@@ -1097,8 +1097,8 @@
     POP lenq
     sub outq, (%1*4) + (%1*2) + (%1/2)

-    lea rtabq, [tab_ %+ %1 %+ _float]
-    lea itabq, [tab_ %+ %1 %+ _float + %1 - 4*7]
+    mov rtabq, [tab_ %+ %1 %+ _float] ; (2048 points and up: a pointer to the table)
+    lea itabq, [rtabq + %1 - 4*7]

 %if %0 > 1
     cmp tgtq, %1
--- a/libavutil/x86/tx_float_init.c
+++ b/libavutil/x86/tx_float_init.c
@@ -76,7 +76,9 @@
                                                int len, int inv,               \
                                                const void *scale)              \
 {                                                                              \
-    ff_tx_init_tabs_float(len);                                                \
+    int ret = ff_tx_init_tabs_float(len);                                      \
+    if (ret < 0)                                                               \
+        return ret;                                                            \
     if (cd->max_len == 2)                                                      \
         return ff_tx_gen_ptwo_revtab(s, opts);                                 \
     else                                                                       \
@@ -98,7 +100,8 @@
      * to SCATTER if needed. */
     FFTXCodeletOptions sub_opts = { .map_dir = FF_TX_MAP_GATHER };

-    TX_TAB(ff_tx_init_tabs)(len);
+    if ((ret = TX_TAB(ff_tx_init_tabs)(len)) < 0)
+        return ret;

     if (len == 15)
         ret = ff_tx_gen_pfa_input_map(s, &sub_opts, 3, 5);
@@ -223,9 +226,7 @@
     if (!(s->tmp = av_malloc(len*sizeof(*s->tmp))))
         return AVERROR(ENOMEM);

-    TX_TAB(ff_tx_init_tabs)(len / sub_len);
-
-    return 0;
+    return TX_TAB(ff_tx_init_tabs)(len / sub_len);
 }

 const FFTXCodelet * const ff_tx_codelet_list_float_x86[] = {
EOF
}
b_ffmpeg() {
    unpack ffmpeg-$FFMPEG_VER.tar.xz ffmpeg
    apply_patch "$S/ffmpeg" ffmpeg_patch
    rm -rf "$B/ffmpeg"; mkdir -p "$B/ffmpeg"; cd "$B/ffmpeg"
    # --target-os=freebsd: the kernel's (and clang's target); FFmpeg 8.0's configure treats freebsd
    # and linux alike on x86 (both only enable section_data_rel_ro, which only ARM assembly reads).
    # ps4-cc as compiler and linker: configure's link checks then see the PS4's libraries (and the
    # poison library), and nothing of the PS4 link line ends up in the .pc files.
    # No AVX2/FMA3/FMA4/XOP/AVX-512: Jaguar has none of them (AVX, SSE4.2 and below are kept and
    # chosen at run time; FFmpeg flags family 16h AVXSLOW, so it keeps 256-bit AVX for where it pays).
    # (libavutil's TX assembles its FMA3/AVX2 FFTs regardless; the CPU check never picks them here.)
    env -u CFLAGS -u CXXFLAGS -u CPPFLAGS -u LDFLAGS -u LIBS \
    "$S/ffmpeg/configure" --prefix="$MEDIA" --libdir="$MEDIA/lib" --incdir="$MEDIA/include" \
        --pkgconfigdir="$MEDIA/lib/pkgconfig" \
        --enable-cross-compile --arch=x86_64 --cpu=btver2 --target-os=freebsd \
        --cc=ps4-cc --cxx=ps4-c++ --ld=ps4-cc --ar=llvm-ar-$CLANG_VER --nm=llvm-nm-$CLANG_VER \
        --ranlib=llvm-ranlib-$CLANG_VER --strip=llvm-strip-$CLANG_VER --x86asmexe=nasm \
        --pkg-config=ps4-pkg-config --host-cc=gcc --extra-cflags="$MEDIA_CFLAGS" \
        --enable-static --disable-shared --enable-pic --enable-pthreads \
        --disable-debug --disable-stripping --disable-programs --disable-doc --disable-network \
        --disable-autodetect --disable-everything --disable-avdevice \
        --enable-avcodec --enable-avformat --enable-avutil --enable-avfilter --enable-swscale --enable-swresample \
        --enable-asm --enable-x86asm --enable-inline-asm \
        --disable-avx2 --disable-fma3 --disable-fma4 --disable-xop --disable-avx512 --disable-avx512icl \
        --enable-decoder="$(csv "${FF_DECODERS[@]}")" --enable-demuxer="$(csv "${FF_DEMUXERS[@]}")" \
        --enable-parser="$(csv "${FF_PARSERS[@]}")" --enable-bsf="$(csv "${FF_BSFS[@]}")" \
        --enable-filter="$(csv "${FF_FILTERS[@]}")" --enable-protocol=file
    # (av_cpu_count () = sysctl hw.ncpu: libkernel's sysctl, the kit's libc-overlay <sys/sysctl.h>)
    grep -q '^#define HAVE_SYSCTL 1$' config.h
    make -j"$JOBS"
    make install
    cd - >/dev/null
}

# --- GStreamer -----------------------------------------------------------------------------------
# Every project: only what is enabled below (auto_features=disabled), no ORC (it generates code at
# run time: the PS4 has no executable heap; the C fallbacks run), no GL/X11/Wayland/ALSA/Pulse/OSS,
# introspection, tests, examples, tools, docs, NLS or Python.
GST_COMMON=(-Dauto_features=disabled -Dexamples=disabled -Dtests=disabled -Ddoc=disabled -Dnls=disabled
    -Dglib_debug=disabled "-Dpackage-name=GStreamer (Wisp, PS4)" -Dpackage-origin=https://gstreamer.freedesktop.org)
GST_PLUGINS_COMMON=("${GST_COMMON[@]}" -Dorc=disabled)
GST_NO_TOOLS=(-Dtools=disabled -Dintrospection=disabled)   # (-good has neither option)
b_gstreamer() {
    unpack gstreamer-$GST_VER.tar.xz gstreamer
    # gsttask.c names threads with prctl (PR_SET_NAME) when <sys/prctl.h> exists (a header check
    # only): musl's prctl is a Linux system call, another call or none on the PS4's kernel (the
    # poison library has it).
    sed -i 's/^#if defined(HAVE_SYS_PRCTL_H) && defined(PR_SET_NAME)$/& \&\& !defined(__PS4__)/' "$S/gstreamer/gst/gsttask.c"
    grep -q '^#if defined(HAVE_SYS_PRCTL_H) && defined(PR_SET_NAME) && !defined(__PS4__)$' "$S/gstreamer/gst/gsttask.c"
    # (v00.38) clock entries on umtx words, not three pthread objects each: making and destroying those
    # for every wait used up the kernel library's internal pool (Web Audio's clocksync, 375 a second)
    python3 "$WISP/scripts/patch-gst-clock.py" "$S/gstreamer/gst/gstsystemclock.c"
    # registry=false: static plugins only (no plugin scanning, registry cache files or the forked
    # gst-plugin-scanner). gstreamer-static-full: gst_init () calls gst_init_static_plugins (),
    # which libgstreamer-full-1.0.a defines (b_full). gst_debug stays: GST_DEBUG logging.
    ms gstreamer "$S/gstreamer" "${GST_COMMON[@]}" "${GST_NO_TOOLS[@]}" -Dregistry=false -Dgstreamer-static-full=true \
        -Dgst_debug=true -Dgst_parse=true -Dtracer_hooks=true -Doption-parsing=true \
        -Dptp-helper=disabled -Dcheck=disabled -Dlibunwind=disabled -Dlibdw=disabled -Ddbghelp=disabled \
        -Dbash-completion=disabled -Dcoretracers=disabled -Dbenchmarks=disabled
}
b_base() {
    unpack gst-plugins-base-$GST_VER.tar.xz gst-plugins-base
    ms base "$S/gst-plugins-base" "${GST_PLUGINS_COMMON[@]}" "${GST_NO_TOOLS[@]}" -Dgl=disabled -Dx11=disabled -Dxvideo=disabled \
        -Dxshm=disabled -Dxi=disabled -Dalsa=disabled -Dpango=disabled -Dqt5=disabled -Diso-codes=disabled \
        -Dapp=enabled -Daudioconvert=enabled -Daudioresample=enabled -Dplayback=enabled -Dtypefind=enabled \
        -Dvolume=enabled -Dvideoconvertscale=enabled -Dgio=enabled \
        -Dogg=enabled -Dvorbis=enabled -Dtheora=enabled
}
b_good() {
    unpack gst-plugins-good-$GST_VER.tar.xz gst-plugins-good
    ms good "$S/gst-plugins-good" "${GST_PLUGINS_COMMON[@]}" -Dasm=disabled \
        -Disomp4=enabled -Dmatroska=enabled -Daudioparsers=enabled -Dautodetect=enabled \
        -Did3demux=enabled -Dapetag=enabled -Dicydemux=enabled -Dwavparse=enabled -Daudiofx=enabled \
        -Dinterleave=enabled -Davi=enabled -Dflv=enabled
}
b_bad() {
    unpack gst-plugins-bad-$GST_VER.tar.xz gst-plugins-bad
    ms bad "$S/gst-plugins-bad" "${GST_PLUGINS_COMMON[@]}" "${GST_NO_TOOLS[@]}" -Dgpl=disabled \
        -Dvideoparsers=enabled -Dopus=enabled -Dsubenc=enabled -Dmpegtsdemux=enabled -Dmpegdemux=enabled
}
b_libav() {
    unpack gst-libav-$GST_VER.tar.xz gst-libav
    ms libav "$S/gst-libav" -Ddoc=disabled -Dtests=disabled \
        "-Dpackage-name=GStreamer FFmpeg (Wisp, PS4)" -Dpackage-origin=https://gstreamer.freedesktop.org
}

# --- libgstreamer-full-1.0.a ---------------------------------------------------------------------
# Every libgst*-1.0.a and every plugin (lib/gstreamer-1.0/libgst<name>.a) in one archive, plus
# gst_init_static_plugins (), which registers each plugin (GST_PLUGIN_STATIC_REGISTER) and which
# gst_init () calls (core built with gstreamer-static-full). gstreamer-full-1.0.pc: for WebKit's
# USE_GSTREAMER_FULL (its non-full build demands gstreamer-gl-1.0, which -Dgl=disabled leaves out).
# The archive's members stay what they were: the app's link only takes those it reaches.
#
# These plugins register only the listed elements (plugin:source module:elements), under their own
# names (WebKit looks some plugins up by name): their muxers, encoders and the parsers of codecs
# nothing here decodes are never made by WebKit's playback, and unregistered they stay out of the
# app's link (~1.5 MB: libvorbisenc's tables alone are 0.55 MB). Every other plugin registers whole.
GST_FULL_SUBSETS=(
    "isomp4:gst-plugins-good:qtdemux"
    "matroska:gst-plugins-good:matroskademux,matroskaparse"
    "avi:gst-plugins-good:avidemux"
    "flv:gst-plugins-good:flvdemux"
    "ogg:gst-plugins-base:oggdemux"
    "vorbis:gst-plugins-base:vorbisdec,vorbisparse"
    "theora:gst-plugins-base:theoradec"
    "audiofx:gst-plugins-good:scaletempo"
    "audioparsers:gst-plugins-good:aacparse,ac3parse,dcaparse,flacparse,mpegaudioparse"
    "videoparsersbad:gst-plugins-bad:h263parse,h264parse,h265parse,mpegvideoparse,mpeg4videoparse,vc1parse,vp9parse"
)
b_full() {
    local d="$B/full" lib p e s n=0 plugins=() pcs=() reqs libs
    declare -A plib=() subset=() source=()
    rm -rf "$d"; mkdir -p "$d"
    for lib in "$MEDIA"/lib/gstreamer-1.0/libgst*.a; do
        p=$(llvm-nm-$CLANG_VER --defined-only -g "$lib" |
            awk '$2 == "T" && $3 ~ /^gst_plugin_[a-z0-9_]+_register$/ { print substr($3, 12, length($3) - 20) }')
        [ "$(echo "$p" | wc -w)" = 1 ] || { echo "full: no single plugin in $lib: $p" >&2; return 1; }
        plugins+=("$p"); plib[$p]=$lib
    done
    for s in "${GST_FULL_SUBSETS[@]}"; do
        IFS=: read -r p e lib <<<"$s"
        [ -n "${plib[$p]:-}" ] || { echo "full: no plugin $p for its subset" >&2; return 1; }
        source[$p]=$e; subset[$p]=$lib
        lib=$(llvm-nm-$CLANG_VER --defined-only -g "${plib[$p]}")   # (no grep -q in a pipe: pipefail)
        for e in ${subset[$p]//,/ }; do
            grep -q " T gst_element_register_$e\$" <<<"$lib" || { echo "full: $p has no element $e" >&2; return 1; }
        done
    done
    {
        echo "/* Generated by Wisp's scripts/build-media.sh: the static plugins of libgstreamer-full-1.0. */"
        echo '#include <gst/gst.h>'
        echo
        for p in "${plugins[@]}"; do
            if [ -z "${subset[$p]:-}" ]; then echo "GST_PLUGIN_STATIC_DECLARE ($p);"; continue; fi
            for e in ${subset[$p]//,/ }; do echo "GST_ELEMENT_REGISTER_DECLARE ($e);"; done
            echo
            echo "static gboolean"
            echo "wisp_${p}_init (GstPlugin * plugin)"
            echo "{"
            echo "  gboolean ret = FALSE;"
            for e in ${subset[$p]//,/ }; do echo "  ret |= GST_ELEMENT_REGISTER ($e, plugin);"; done
            echo "  return ret;"
            echo "}"
            echo
        done
        echo
        echo 'void gst_init_static_plugins (void);'
        echo
        echo '/* Called by gst_init () (gstreamer-static-full); safe to call again. */'
        echo 'void'
        echo 'gst_init_static_plugins (void)'
        echo '{'
        echo '  static gsize initialized = 0;'
        echo
        echo '  if (g_once_init_enter (&initialized)) {'
        for p in "${plugins[@]}"; do
            if [ -z "${subset[$p]:-}" ]; then echo "    GST_PLUGIN_STATIC_REGISTER ($p);"; continue; fi
            echo "    gst_plugin_register_static (GST_VERSION_MAJOR, GST_VERSION_MINOR, \"$p\","
            echo "        \"$p: ${subset[$p]//,/, } (Wisp)\", wisp_${p}_init, \"$GST_VER\", \"LGPL\","
            echo "        \"${source[$p]}\", \"GStreamer (Wisp, PS4)\", \"https://gstreamer.freedesktop.org\");"
        done
        echo '    g_once_init_leave (&initialized, 1);'
        echo '  }'
        echo '}'
    } > "$d/gstinitstaticplugins.c"
    clang-$CLANG_VER $WISP_CFLAGS $MEDIA_CFLAGS $(ps4-pkg-config --cflags gstreamer-1.0) \
        -c "$d/gstinitstaticplugins.c" -o "$d/gstinitstaticplugins.o"
    rm -f "$MEDIA/lib/libgstreamer-full-1.0.a"
    local gstlibs=("$MEDIA"/lib/libgst*-1.0.a) gstplugins=("$MEDIA"/lib/gstreamer-1.0/libgst*.a)
    # q + L: append the archives' members (all of them, names may repeat across libraries)
    llvm-ar-$CLANG_VER qcL "$MEDIA/lib/libgstreamer-full-1.0.a" "$d/gstinitstaticplugins.o" \
        "${gstlibs[@]}" "${gstplugins[@]}"
    llvm-ranlib-$CLANG_VER "$MEDIA/lib/libgstreamer-full-1.0.a"
    for lib in "${gstlibs[@]}" "${gstplugins[@]}"; do
        n=$((n + $(llvm-ar-$CLANG_VER t "$lib" | wc -l)))
    done
    [ "$(llvm-ar-$CLANG_VER t "$MEDIA/lib/libgstreamer-full-1.0.a" | wc -l)" = $((n + 1)) ] \
        || { echo "full: members missing from the archive" >&2; return 1; }
    # the .pc: everything outside GStreamer that its libraries and plugins require
    pcs=("$MEDIA"/lib/pkgconfig/gstreamer-*1.0.pc "$MEDIA"/lib/gstreamer-1.0/pkgconfig/gst*.pc)
    reqs=$(sed -n -E 's/^Requires(\.private)?:[[:space:]]*//p' "${pcs[@]}" | tr ',' '\n' |
        awk '{ print $1 }' | grep -v -E '^(gstreamer-.*1\.0|gst[a-z0-9]+)$' | sort -u | paste -sd, - | sed 's/,/, /g')
    libs=$(sed -n -E 's/^Libs(\.private)?:[[:space:]]*//p' "${pcs[@]}" | tr ' ' '\n' |
        grep -E '^(-l|-pthread$)' | grep -v '^-lgst' | sort -u | paste -sd' ' -)
    cat > "$MEDIA/lib/pkgconfig/gstreamer-full-1.0.pc" <<EOF
prefix=$MEDIA
libdir=\${prefix}/lib
includedir=\${prefix}/include
pluginsdir=\${libdir}/gstreamer-1.0

Name: gstreamer-full-1.0
Description: GStreamer $GST_VER, all of Wisp's GStreamer libraries and plugins in one static library; gst_init () registers the plugins (PS4)
Version: $GST_VER
Requires: glib-2.0, gobject-2.0
Requires.private: $reqs
Libs: -L\${libdir} -lgstreamer-full-1.0
Libs.private: $libs
Cflags: -I\${includedir}/gstreamer-1.0 -DGST_STATIC_COMPILATION
EOF
    echo "plugins (${#plugins[@]}): ${plugins[*]}"
    for p in "${!subset[@]}"; do echo "  $p: only ${subset[$p]}"; done
    echo "Requires.private: $reqs"
    echo "Libs.private: $libs"
}

ALL=(ogg vorbis theora opus ffmpeg gstreamer base good bad libav full)
if [ "${1:-}" = fetch ]; then fetch; exit 0; fi
want=("$@"); [ ${#want[@]} -gt 0 ] || want=("${ALL[@]}")
log="$L/$LOGTAG-fetch.log"
fetch > "$log" 2>&1 || { echo "!! fetch failed: $log"; tail -5 "$log"; exit 1; }
for name in "${want[@]}"; do
    if [ $# -eq 0 ] && [ -f "$B/$name.done" ]; then continue; fi
    echo "== $name"
    log="$L/$LOGTAG-$name.log"
    # Not inside an `if`: bash ignores set -e in a condition (see build-deps.sh).
    set +e
    ( set -euo pipefail; "b_$name" ) > "$log" 2>&1
    rc=$?
    set -e
    if [ $rc -eq 0 ]; then
        touch "$B/$name.done"
    else
        echo "!! $name failed: $log"; tail -25 "$log"; exit 1
    fi
done
echo "media: all built in $MEDIA"
