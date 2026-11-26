// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

// PS4: the launcher's software canvas. Text comes from the PS Vita's fonts once Vita3K has the
// font package (sa0/data/font/pvf: Latin, Japanese, Chinese, Korean), else from the PS4's own
// (/preinst/common/font, where the app may read it: on the test console it couldn't, and v00.04
// drew no text at all), else from the Roboto in the package (/app0/fonts). Rasterized with
// stb_truetype (implemented in overlay/src/font.cpp); pictures are decoded with stb_image
// (renderer/src/texture/replacement.cpp).

#include "ps4_canvas.h"

#include <util/log.h>

#include <stb_image.h>
#include <stb_truetype.h>

// Wisp: text that needs shaping (Arabic, Hebrew, the Indic scripts, Thai, ...) is laid out by
// HarfBuzz in ICU's bidirectional order (both are linked for WebKit)
#include <hb.h>
#include <unicode/ubidi.h>
#include <unicode/uchar.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>

extern "C" void ps4_boot_trace(const char *line); // port/ps4_crashlog.cpp

namespace ps4ui {

namespace {

struct Font {
    std::vector<uint8_t> data;
    stbtt_fontinfo info{};
    int index = 0;              // the face, in a collection
    hb_font_t *hb = nullptr;    // HarfBuzz's view of it, for shaping (made when first needed)
};

// A place fonts come from: its Latin fonts (the first it has of each list) and its fonts for other
// scripts
struct FontSource {
    std::string dir;
    std::vector<const char *> regular;
    std::vector<const char *> bold;
    std::vector<const char *> scripts;
};

struct Glyph {
    int width = 0;
    int height = 0;
    int xoff = 0;
    int yoff = 0;
    float advance = 0;
    std::vector<uint8_t> alpha;
};

struct FontSet {
    std::vector<std::shared_ptr<Font>> fonts; // Latin first, then the other scripts
    std::unordered_map<uint64_t, Glyph> glyphs;
};

std::mutex fonts_mutex;
bool fonts_tried = false;
std::string vita_font_dir; // set_vita_font_dir
FontSet regular_set;
FontSet bold_set;

std::shared_ptr<Font> load_font(const std::string &dir, const char *name, int index = 0) {
    const std::string path = dir + name;
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f)
        return nullptr;
    auto font = std::make_shared<Font>();
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    font->data.resize(size > 0 ? static_cast<size_t>(size) : 0);
    const size_t got = std::fread(font->data.data(), 1, font->data.size(), f);
    std::fclose(f);
    font->index = index;
    const int offset = got == font->data.size() ? stbtt_GetFontOffsetForIndex(font->data.data(), index) : -1;
    if (offset < 0 || !stbtt_InitFont(&font->info, font->data.data(), offset)) {
        LOG_WARN("PS4 launcher: font {} unreadable", path);
        return nullptr;
    }
    return font;
}

// Wisp: the fonts for the other scripts, from the browser's Noto set, read when a character first
// needs one (the CJK collection alone is 19 MiB)
struct ScriptFont {
    char32_t first, last;
    const char *file;
    int index;
};
#ifndef WISP_APP0
#define WISP_APP0 "/app0"   // (a PC test points it at a folder of its own)
#endif
const char *const SCRIPT_FONT_DIR = WISP_APP0 "/wisp/fonts/noto/";
const ScriptFont SCRIPT_FONTS[] = {
    { 0x0370, 0x03FF, "NotoSans-Regular.ttf", 0 }, { 0x0400, 0x052F, "NotoSans-Regular.ttf", 0 },
    { 0x1E00, 0x1FFF, "NotoSans-Regular.ttf", 0 }, { 0x0530, 0x058F, "NotoSansArmenian-Regular.ttf", 0 },
    { 0x0590, 0x05FF, "NotoSansHebrew-Regular.ttf", 0 }, { 0xFB1D, 0xFB4F, "NotoSansHebrew-Regular.ttf", 0 },
    { 0x0600, 0x06FF, "NotoNaskhArabic-Regular.ttf", 0 }, { 0x0750, 0x077F, "NotoNaskhArabic-Regular.ttf", 0 },
    { 0x0870, 0x08FF, "NotoNaskhArabic-Regular.ttf", 0 }, { 0xFB50, 0xFDFF, "NotoNaskhArabic-Regular.ttf", 0 },
    { 0xFE70, 0xFEFF, "NotoNaskhArabic-Regular.ttf", 0 }, { 0x0700, 0x074F, "NotoSansSyriac-Regular.ttf", 0 },
    { 0x0780, 0x07BF, "NotoSansThaana-Regular.ttf", 0 }, { 0x0900, 0x097F, "NotoSansDevanagari-Regular.ttf", 0 },
    { 0x0980, 0x09FF, "NotoSansBengali-Regular.ttf", 0 }, { 0x0A00, 0x0A7F, "NotoSansGurmukhi-Regular.ttf", 0 },
    { 0x0A80, 0x0AFF, "NotoSansGujarati-Regular.ttf", 0 }, { 0x0B00, 0x0B7F, "NotoSansOriya-Regular.ttf", 0 },
    { 0x0B80, 0x0BFF, "NotoSansTamil-Regular.ttf", 0 }, { 0x0C00, 0x0C7F, "NotoSansTelugu-Regular.ttf", 0 },
    { 0x0C80, 0x0CFF, "NotoSansKannada-Regular.ttf", 0 }, { 0x0D00, 0x0D7F, "NotoSansMalayalam-Regular.ttf", 0 },
    { 0x0D80, 0x0DFF, "NotoSansSinhala-Regular.ttf", 0 }, { 0x0E00, 0x0E7F, "NotoSansThai-Regular.ttf", 0 },
    { 0x0E80, 0x0EFF, "NotoSansLao-Regular.ttf", 0 }, { 0x0F00, 0x0FFF, "NotoSerifTibetan-Regular.ttf", 0 },
    { 0x1000, 0x109F, "NotoSansMyanmar-Regular.ttf", 0 }, { 0x10A0, 0x10FF, "NotoSansGeorgian-Regular.ttf", 0 },
    { 0x1200, 0x139F, "NotoSansEthiopic-Regular.ttf", 0 }, { 0x13A0, 0x13FF, "NotoSansCherokee-Regular.ttf", 0 },
    { 0x1780, 0x17FF, "NotoSansKhmer-Regular.ttf", 0 }, { 0x2190, 0x2BFF, "NotoSansSymbols2-Regular.ttf", 0 },
    // Chinese, Japanese and Korean: one face of the collection for all of them
    { 0x1100, 0x11FF, "NotoSansCJK-Regular.ttc", 0 }, { 0x2E80, 0x33FF, "NotoSansCJK-Regular.ttc", 0 },
    { 0x3400, 0x4DBF, "NotoSansCJK-Regular.ttc", 0 }, { 0x4E00, 0x9FFF, "NotoSansCJK-Regular.ttc", 0 },
    { 0xA960, 0xA97F, "NotoSansCJK-Regular.ttc", 0 }, { 0xAC00, 0xD7AF, "NotoSansCJK-Regular.ttc", 0 },
    { 0xF900, 0xFAFF, "NotoSansCJK-Regular.ttc", 0 }, { 0xFE30, 0xFE4F, "NotoSansCJK-Regular.ttc", 0 },
    { 0xFF00, 0xFFEF, "NotoSansCJK-Regular.ttc", 0 }, { 0x20000, 0x2FA1F, "NotoSansCJK-Regular.ttc", 0 },
};

// The fonts of the first place that has them
void load_fonts() {
    if (fonts_tried)
        return;
    fonts_tried = true;
    std::vector<FontSource> sources;
    if (!vita_font_dir.empty())
        sources.push_back({ vita_font_dir, { "ltn0.pvf" }, { "ltn4.pvf" }, { "jpn0.pvf", "cn0.pvf", "kr0.pvf" } });
    sources.push_back({ "/preinst/common/font/", { "SST-Roman.otf", "SST-Medium.otf" }, { "SST-Bold.otf", "SST-Medium.otf" },
        { "SSTJpPro-Regular.otf", "DFHEI5-SONY.ttf", "SCEPS4Yoongd-Medium.otf" } });
    sources.push_back({ WISP_APP0 "/fonts/", { "Roboto-Regular.ttf" }, { "Roboto-Bold.ttf" }, {} });

    const auto first = [](const FontSource &source, const std::vector<const char *> &names) -> std::shared_ptr<Font> {
        for (const char *name : names) {
            if (auto font = load_font(source.dir, name))
                return font;
        }
        return nullptr;
    };
    std::string line = "launcher: no fonts, so no text";
    for (const FontSource &source : sources) {
        const std::shared_ptr<Font> regular = first(source, source.regular);
        if (!regular)
            continue;
        const std::shared_ptr<Font> bold = first(source, source.bold);
        regular_set.fonts.push_back(regular);
        bold_set.fonts.push_back(bold ? bold : regular);
        // (the sets share these: the Chinese font alone is 8 MiB)
        for (const char *name : source.scripts) {
            if (auto font = load_font(source.dir, name)) {
                regular_set.fonts.push_back(font);
                bold_set.fonts.push_back(font);
            }
        }
        line = fmt::format("launcher: fonts from {} ({} with the other scripts)", source.dir, regular_set.fonts.size());
        break;
    }
    LOG_INFO("PS4 {}", line);
    ps4_boot_trace(line.c_str());
}

FontSet &font_set(Weight weight) {
    load_fonts();
    return weight == Weight::Bold && !bold_set.fonts.empty() ? bold_set : regular_set;
}

// Decodes the next code point of a UTF-8 string (U+FFFD for broken sequences)
char32_t next_code_point(std::string_view s, size_t &i) {
    const auto c = static_cast<uint8_t>(s[i++]);
    if (c < 0x80)
        return c;
    int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : -1;
    if (extra < 0)
        return 0xFFFD;
    char32_t cp = c & (0x3F >> extra);
    while (extra-- > 0) {
        if (i >= s.size() || (static_cast<uint8_t>(s[i]) & 0xC0) != 0x80)
            return 0xFFFD;
        cp = (cp << 6) | (static_cast<uint8_t>(s[i++]) & 0x3F);
    }
    return cp;
}

// Wisp: the script fonts read so far (null: unreadable), by file
std::unordered_map<std::string, std::shared_ptr<Font>> script_fonts_read;

// a script font for cp (SCRIPT_FONTS), read now and added to both sets, at the same place in each;
// false when there is none or it was read already
bool add_script_font(char32_t cp) {
    for (const ScriptFont &s : SCRIPT_FONTS) {
        if (cp < s.first || cp > s.last)
            continue;
        if (script_fonts_read.count(s.file))
            return false;
        auto font = load_font(SCRIPT_FONT_DIR, s.file, s.index);
        script_fonts_read[s.file] = font;
        if (!font || regular_set.fonts.empty())
            return false;
        // (load_fonts gives the bold set a font wherever the regular set has one)
        regular_set.fonts.push_back(font);
        bold_set.fonts.push_back(font);
        return true;
    }
    return false;
}

// the first font of set with cp's glyph (reading a script font when none has it); false: none has it
bool find_glyph(FontSet &set, char32_t cp, size_t &font_index, int &glyph) {
    for (int pass = 0; pass < 2; pass++) {
        for (size_t i = 0; i < set.fonts.size(); i++) {
            glyph = stbtt_FindGlyphIndex(&set.fonts[i]->info, static_cast<int>(cp));
            if (glyph) {
                font_index = i;
                return true;
            }
        }
        if (pass || !add_script_font(cp))
            break;
    }
    font_index = 0;
    glyph = 0;
    return false;
}

const Glyph *glyph_by_index(FontSet &set, size_t font_index, int glyph, int px);

const Glyph *glyph_for(FontSet &set, char32_t cp, int px) {
    if (set.fonts.empty())
        return nullptr;
    size_t font_index = 0;
    int glyph = 0;
    find_glyph(set, cp, font_index, glyph);   // (none: the first font's missing-glyph box)
    return glyph_by_index(set, font_index, glyph, px);
}

// The scale of a set's font at px: the first (Latin) font fills px with its ascent and descent, the
// others get the same em (scaled by their own heights, Arabic's tall marks made it a third smaller)
float font_scale(FontSet &set, size_t font_index, int px) {
    const float first = stbtt_ScaleForPixelHeight(&set.fonts[0]->info, static_cast<float>(px));
    if (font_index == 0)
        return first;
    const float em = first / stbtt_ScaleForMappingEmToPixels(&set.fonts[0]->info, 1.f);
    return stbtt_ScaleForMappingEmToPixels(&set.fonts[font_index]->info, em);
}

const Glyph *glyph_by_index(FontSet &set, size_t font_index, int glyph, int px) {
    if (font_index >= set.fonts.size())
        return nullptr;
    const uint64_t key = (static_cast<uint64_t>(font_index) << 48) | (static_cast<uint64_t>(px) << 32) | static_cast<uint32_t>(glyph);
    auto it = set.glyphs.find(key);
    if (it != set.glyphs.end())
        return &it->second;
    const stbtt_fontinfo &info = set.fonts[font_index]->info;
    const float scale = font_scale(set, font_index, px);
    Glyph g;
    int advance = 0, lsb = 0;
    stbtt_GetGlyphHMetrics(&info, glyph, &advance, &lsb);
    g.advance = advance * scale;
    unsigned char *bitmap = stbtt_GetGlyphBitmap(&info, scale, scale, glyph, &g.width, &g.height, &g.xoff, &g.yoff);
    if (bitmap) {
        g.alpha.assign(bitmap, bitmap + g.width * g.height);
        stbtt_FreeBitmap(bitmap, nullptr);
    }
    return &set.glyphs.emplace(key, std::move(g)).first->second;
}

// Wisp: text that needs shaping or right-to-left order. Drawing a code point at a time, Arabic came out
// as unjoined letters from left to right; it goes through HarfBuzz in ICU's visual order instead.
bool complex_code_point(char32_t cp) {
    return (cp >= 0x0590 && cp <= 0x08FF) || (cp >= 0x0900 && cp <= 0x0DFF) || (cp >= 0x0E00 && cp <= 0x0EFF)
        || (cp >= 0x0F00 && cp <= 0x109F) || (cp >= 0x1780 && cp <= 0x17FF) || (cp >= 0x200C && cp <= 0x200F)
        || (cp >= 0x202A && cp <= 0x202E) || (cp >= 0x2066 && cp <= 0x2069) || (cp >= 0xFB1D && cp <= 0xFDFF)
        || (cp >= 0xFE70 && cp <= 0xFEFF);
}

bool needs_shaping(std::string_view utf8) {
    for (size_t i = 0; i < utf8.size();) {
        if (static_cast<uint8_t>(utf8[i]) < 0x80) {
            i++;
            continue;
        }
        if (complex_code_point(next_code_point(utf8, i)))
            return true;
    }
    return false;
}

// the text's first strong character is right-to-left (Unicode's paragraph rule)
bool rtl_paragraph(std::string_view utf8) {
    for (size_t i = 0; i < utf8.size();) {
        const UCharDirection direction = u_charDirection(static_cast<UChar32>(next_code_point(utf8, i)));
        if (direction == U_LEFT_TO_RIGHT)
            return false;
        if (direction == U_RIGHT_TO_LEFT || direction == U_RIGHT_TO_LEFT_ARABIC)
            return true;
    }
    return false;
}

hb_font_t *hb_font_of(Font &font) {
    if (!font.hb) {
        hb_blob_t *blob = hb_blob_create(reinterpret_cast<const char *>(font.data.data()), static_cast<unsigned>(font.data.size()),
                                         HB_MEMORY_MODE_READONLY, nullptr, nullptr);
        hb_face_t *face = hb_face_create(blob, static_cast<unsigned>(font.index));
        hb_blob_destroy(blob);
        font.hb = hb_font_create(face);
        const int upem = static_cast<int>(hb_face_get_upem(face));
        hb_face_destroy(face);
        hb_font_set_scale(font.hb, upem, upem); // positions in font units, scaled like stb's glyphs
    }
    return font.hb;
}

struct Placed {
    size_t font;
    int glyph;
    float x, y; // the pen position from the text's start, the glyph's offset down
};

// utf8 laid out in visual order (out: each glyph and where it goes); its width
float shape(FontSet &set, std::string_view utf8, int px, std::vector<Placed> *out) {
    std::vector<char32_t> cps;
    for (size_t i = 0; i < utf8.size();)
        cps.push_back(next_code_point(utf8, i));
    if (cps.empty() || set.fonts.empty())
        return 0;
    // UTF-16 for ICU, with each unit's code point
    std::vector<UChar> units;
    std::vector<size_t> unit_cp;
    for (size_t k = 0; k < cps.size(); k++) {
        char32_t c = cps[k];
        if (c >= 0x10000 && c <= 0x10FFFF) {
            c -= 0x10000;
            units.push_back(static_cast<UChar>(0xD800 + (c >> 10)));
            units.push_back(static_cast<UChar>(0xDC00 + (c & 0x3FF)));
            unit_cp.push_back(k);
        } else {
            units.push_back(static_cast<UChar>(c > 0xFFFF ? 0xFFFD : c));
        }
        unit_cp.push_back(k);
    }
    // each code point's font: the first with its glyph; marks, joiners and variation selectors keep
    // the font of the character before them
    std::vector<size_t> fonts(cps.size(), 0);
    for (size_t k = 0; k < cps.size(); k++) {
        const int8_t type = u_charType(static_cast<UChar32>(cps[k]));
        const bool attached = k > 0 && (type == U_NON_SPACING_MARK || type == U_ENCLOSING_MARK || type == U_COMBINING_SPACING_MARK
                                        || cps[k] == 0x200C || cps[k] == 0x200D || (cps[k] >= 0xFE00 && cps[k] <= 0xFE0F));
        size_t font = 0;
        int glyph = 0;
        fonts[k] = attached ? fonts[k - 1] : find_glyph(set, cps[k], font, glyph) ? font : 0;
    }
    float pen = 0;
    // a piece of one font, shaped with the text around it as context
    const auto shape_piece = [&](size_t a, size_t b, bool rtl) {
        Font &font = *set.fonts[fonts[a]];
        hb_buffer_t *buffer = hb_buffer_create();
        hb_buffer_add_utf32(buffer, reinterpret_cast<const uint32_t *>(cps.data()), static_cast<int>(cps.size()),
                            static_cast<unsigned>(a), static_cast<int>(b - a));
        hb_buffer_set_direction(buffer, rtl ? HB_DIRECTION_RTL : HB_DIRECTION_LTR);
        hb_buffer_guess_segment_properties(buffer);
        hb_shape(hb_font_of(font), buffer, nullptr, 0);
        unsigned count = 0;
        const hb_glyph_info_t *info = hb_buffer_get_glyph_infos(buffer, &count);
        const hb_glyph_position_t *pos = hb_buffer_get_glyph_positions(buffer, &count);
        const float scale = font_scale(set, fonts[a], px);
        for (unsigned g = 0; g < count; g++) {
            if (out)
                out->push_back({ fonts[a], static_cast<int>(info[g].codepoint), pen + pos[g].x_offset * scale, -pos[g].y_offset * scale });
            pen += pos[g].x_advance * scale;
        }
        hb_buffer_destroy(buffer);
    };
    // a visual run: its pieces in logical order, placed from the right in a right-to-left run
    const auto lay_run = [&](size_t a, size_t b, bool rtl) {
        std::vector<std::pair<size_t, size_t>> pieces;
        for (size_t k = a; k < b;) {
            size_t end = k + 1;
            while (end < b && fonts[end] == fonts[k])
                end++;
            pieces.push_back({ k, end });
            k = end;
        }
        if (rtl)
            std::reverse(pieces.begin(), pieces.end());
        for (const auto &piece : pieces)
            shape_piece(piece.first, piece.second, rtl);
    };
    UErrorCode error = U_ZERO_ERROR;
    UBiDi *bidi = ubidi_openSized(static_cast<int32_t>(units.size()), 0, &error);
    if (bidi && U_SUCCESS(error))
        ubidi_setPara(bidi, units.data(), static_cast<int32_t>(units.size()), UBIDI_DEFAULT_LTR, nullptr, &error);
    const int32_t runs = bidi && U_SUCCESS(error) ? ubidi_countRuns(bidi, &error) : -1;
    if (runs < 0 || U_FAILURE(error)) {
        lay_run(0, cps.size(), false);
    } else {
        for (int32_t r = 0; r < runs; r++) {
            int32_t start = 0, length = 0;
            const UBiDiDirection direction = ubidi_getVisualRun(bidi, r, &start, &length);
            if (length > 0)
                lay_run(unit_cp[start], unit_cp[start + length - 1] + 1, direction == UBIDI_RTL);
        }
    }
    if (bidi)
        ubidi_close(bidi);
    return pen;
}

uint32_t pack(Color c) {
    return static_cast<uint32_t>(c.r) | (static_cast<uint32_t>(c.g) << 8) | (static_cast<uint32_t>(c.b) << 16) | (static_cast<uint32_t>(c.a) << 24);
}

uint8_t coverage(float v) {
    return static_cast<uint8_t>(std::clamp(v, 0.f, 1.f) * 255.f + 0.5f);
}

// x * a / 255, rounded, without a division
inline uint32_t mul255(uint32_t x, uint32_t a) {
    const uint32_t t = x * a + 128;
    return (t + (t >> 8)) >> 8;
}

// Three threads and the caller share a job's rows, in eight parts so they finish together. Waking
// them costs some tens of microseconds, so small jobs don't. One job at a time: a second caller
// meanwhile does its own alone.
class RowPool {
public:
    static constexpr int THREADS = 3;
    static constexpr int PARTS = 8;

    RowPool() {
        for (int i = 0; i < THREADS; i++)
            std::thread([this] { work(); }).detach();
    }

    bool try_run(int y0, int y1, const std::function<void(int, int)> &fn) {
        std::unique_lock<std::mutex> busy(run_mutex, std::try_to_lock);
        if (!busy.owns_lock())
            return false;
        {
            const std::lock_guard<std::mutex> lock(mutex);
            job = &fn;
            first = y0;
            last = y1;
            next.store(0);
            done.store(0);
            generation++;
        }
        wake.notify_all();
        take_parts();
        std::unique_lock<std::mutex> lock(mutex);
        finished.wait(lock, [this] { return done.load() == PARTS; });
        job = nullptr;
        return true;
    }

private:
    void take_parts() {
        int ran = 0;
        for (int part; (part = next.fetch_add(1)) < PARTS;) {
            const int a = first + (last - first) * part / PARTS, b = first + (last - first) * (part + 1) / PARTS;
            if (a < b)
                (*job)(a, b);
            ran++;
        }
        if (ran > 0 && done.fetch_add(ran) + ran == PARTS) {
            const std::lock_guard<std::mutex> lock(mutex);
            finished.notify_all();
        }
    }

    void work() {
        uint64_t seen = 0;
        for (;;) {
            {
                std::unique_lock<std::mutex> lock(mutex);
                wake.wait(lock, [&] { return generation != seen; });
                seen = generation;
            }
            take_parts();
        }
    }

    std::mutex run_mutex;
    std::mutex mutex;
    std::condition_variable wake;
    std::condition_variable finished;
    const std::function<void(int, int)> *job = nullptr;
    int first = 0;
    int last = 0;
    std::atomic<int> next{ 0 };
    std::atomic<int> done{ 0 };
    uint64_t generation = 0;
};

thread_local bool t_serial = false;

} // namespace

void parallel_rows(int y0, int y1, size_t pixels, const std::function<void(int, int)> &fn) {
    // (made once, and kept: its threads wait for work for as long as the app runs)
    static RowPool *const pool = new RowPool;
    if (y1 - y0 < 16 || pixels < 100000 || t_serial || !pool->try_run(y0, y1, fn))
        fn(y0, y1);
}

void set_serial(bool serial) {
    t_serial = serial;
}

namespace {

} // namespace

bool fonts_loaded() {
    const std::lock_guard<std::mutex> lock(fonts_mutex);
    load_fonts();
    return !regular_set.fonts.empty();
}

void set_vita_font_dir(const std::string &dir) {
    const std::lock_guard<std::mutex> lock(fonts_mutex);
    vita_font_dir = dir;
    fonts_tried = false;
    regular_set = FontSet{};
    bold_set = FontSet{};
}

bool image_size(const std::string &path, int &width, int &height) {
    int n = 0;
    return stbi_info(path.c_str(), &width, &height, &n) != 0;
}

Image load_image(const std::string &path, int width, int height) {
    Image out;
    int w = 0, h = 0, n = 0;
    unsigned char *data = stbi_load(path.c_str(), &w, &h, &n, 4);
    if (!data)
        return out;
    if (width <= 0 || height <= 0) {
        width = w;
        height = h;
    }
    // fit, keeping the proportions
    const float scale = std::min(static_cast<float>(width) / w, static_cast<float>(height) / h);
    out.width = std::max(1, static_cast<int>(w * scale));
    out.height = std::max(1, static_cast<int>(h * scale));
    out.pixels.resize(static_cast<size_t>(out.width) * out.height);
    for (int y = 0; y < out.height; y++) {
        const float sy = std::clamp((y + 0.5f) / scale - 0.5f, 0.f, static_cast<float>(h - 1));
        const int y0 = static_cast<int>(sy);
        const int y1 = std::min(y0 + 1, h - 1);
        const float fy = sy - y0;
        for (int x = 0; x < out.width; x++) {
            const float sx = std::clamp((x + 0.5f) / scale - 0.5f, 0.f, static_cast<float>(w - 1));
            const int x0 = static_cast<int>(sx);
            const int x1 = std::min(x0 + 1, w - 1);
            const float fx = sx - x0;
            uint8_t px[4];
            for (int c = 0; c < 4; c++) {
                const float a = data[(y0 * w + x0) * 4 + c] * (1 - fx) + data[(y0 * w + x1) * 4 + c] * fx;
                const float b = data[(y1 * w + x0) * 4 + c] * (1 - fx) + data[(y1 * w + x1) * 4 + c] * fx;
                px[c] = static_cast<uint8_t>(a * (1 - fy) + b * fy + 0.5f);
            }
            out.pixels[static_cast<size_t>(y) * out.width + x] = pack(Color{ px[0], px[1], px[2], px[3] });
        }
    }
    stbi_image_free(data);
    return out;
}

void premultiply(Image &image) {
    for (uint32_t &p : image.pixels) {
        const uint32_t a = p >> 24;
        if (a == 255)
            continue;
        const uint32_t r = ((p & 0xFF) * a + 127) / 255;
        const uint32_t g = (((p >> 8) & 0xFF) * a + 127) / 255;
        const uint32_t b = (((p >> 16) & 0xFF) * a + 127) / 255;
        p = r | (g << 8) | (b << 16) | (a << 24);
    }
}

Image shrink_image(const Image &image, int width, int height) {
    Image out;
    if (image.empty() || width <= 0 || height <= 0)
        return out;
    out.width = width;
    out.height = height;
    out.pixels.resize(static_cast<size_t>(width) * height);
    for (int y = 0; y < height; y++) {
        const int y0 = y * image.height / height;
        const int y1 = std::max(y0 + 1, (y + 1) * image.height / height);
        for (int x = 0; x < width; x++) {
            const int x0 = x * image.width / width;
            const int x1 = std::max(x0 + 1, (x + 1) * image.width / width);
            uint32_t sum[4] = {};
            for (int sy = y0; sy < y1; sy++) {
                for (int sx = x0; sx < x1; sx++) {
                    const uint32_t p = image.pixels[static_cast<size_t>(sy) * image.width + sx];
                    for (int c = 0; c < 4; c++)
                        sum[c] += (p >> (c * 8)) & 0xFF;
                }
            }
            const uint32_t n = static_cast<uint32_t>((y1 - y0) * (x1 - x0));
            uint32_t p = 0;
            for (int c = 0; c < 4; c++)
                p |= ((sum[c] + n / 2) / n) << (c * 8);
            out.pixels[static_cast<size_t>(y) * width + x] = p;
        }
    }
    return out;
}

TextSprite render_text(std::string_view utf8, int px, Color c, Weight weight, int max_width, uint8_t shadow) {
    // white on black: the red channel is then the text's coverage
    const int pad = shadow ? 4 : 1;
    Canvas scratch(1, 1);
    int width = scratch.text_width(utf8, px, weight);
    if (max_width > 0)
        width = std::min(width, max_width);
    TextSprite out;
    out.baseline = pad + px;
    const int sw = width + 2 * pad + 2, sh = px + px / 2 + 2 * pad;
    Canvas mask(sw, sh);
    mask.text(pad, out.baseline, utf8, px, rgb(0xffffff), weight, max_width);
    const uint32_t *cov = mask.data();
    out.image.width = sw;
    out.image.height = sh;
    out.image.pixels.resize(static_cast<size_t>(sw) * sh);
    for (int y = 0; y < sh; y++) {
        for (int x = 0; x < sw; x++) {
            const uint32_t a = cov[static_cast<size_t>(y) * sw + x] & 0xFF;
            uint32_t s = 0;
            if (shadow) {
                // the text's coverage 1 px right, 2 px down, softened over 3x3
                uint32_t sum = 0;
                for (int dy = -1; dy <= 1; dy++)
                    for (int dx = -1; dx <= 1; dx++) {
                        const int sx = x - 1 + dx, sy = y - 2 + dy;
                        if (sx >= 0 && sy >= 0 && sx < sw && sy < sh)
                            sum += cov[static_cast<size_t>(sy) * sw + sx] & 0xFF;
                    }
                s = sum / 9 * shadow / 255;
            }
            const uint32_t ta = a * c.a / 255;
            const uint32_t oa = ta + s * (255 - ta) / 255;
            out.image.pixels[static_cast<size_t>(y) * sw + x] = (c.r * ta / 255) | ((c.g * ta / 255) << 8) | ((c.b * ta / 255) << 16) | (oa << 24);
        }
    }
    return out;
}

Canvas::Canvas(int width, int height)
    : w(width)
    , h(height)
    , pixels(static_cast<size_t>(width) * height) {}

Image Canvas::snapshot() const {
    Image out;
    out.width = w;
    out.height = h;
    out.pixels = pixels;
    return out;
}

void Canvas::copy_from(const Canvas &src, int dx, int dy) {
    if (src.w != w || src.h != h)
        return;
    const int x0 = std::max(0, dx), x1 = std::min(w, w + dx);
    const int y0 = std::max(0, dy), y1 = std::min(h, h + dy);
    if (x1 <= x0 || y1 <= y0)
        return;
    parallel_rows(y0, y1, static_cast<size_t>(y1 - y0) * (x1 - x0), [&](int a, int b) {
        for (int y = a; y < b; y++)
            std::memcpy(&pixels[static_cast<size_t>(y) * w + x0], &src.pixels[static_cast<size_t>(y - dy) * w + (x0 - dx)], static_cast<size_t>(x1 - x0) * 4);
    });
}

namespace {
// dst = src + dst * (1 - src alpha), src premultiplied, the result opaque
inline uint32_t over(uint32_t dst, uint32_t src) {
    const uint32_t a = src >> 24;
    if (a == 255)
        return src;
    const uint32_t ia = 255 - a;
    // two channels at a time
    const uint32_t rb = (((dst & 0x00FF00FFu) * ia + 0x00800080u) >> 8) & 0x00FF00FFu;
    const uint32_t g = (((dst & 0x0000FF00u) * ia + 0x00008000u) >> 8) & 0x0000FF00u;
    return ((src & 0x00FFFFFFu) + rb + g) | 0xFF000000u;
}

inline uint32_t fade(uint32_t p, uint32_t alpha) {
    const uint32_t rb = (((p & 0x00FF00FFu) * alpha + 0x00800080u) >> 8) & 0x00FF00FFu;
    const uint32_t ag = ((((p >> 8) & 0x00FF00FFu) * alpha + 0x00800080u) >> 8) & 0x00FF00FFu;
    return rb | (ag << 8);
}

// between two pixels, per channel, f in 0..256
inline uint32_t lerp(uint32_t a, uint32_t b, uint32_t f) {
    const uint32_t rb = ((((a & 0x00FF00FFu) * (256 - f)) + ((b & 0x00FF00FFu) * f)) >> 8) & 0x00FF00FFu;
    const uint32_t ag = (((((a >> 8) & 0x00FF00FFu) * (256 - f)) + (((b >> 8) & 0x00FF00FFu) * f)) >> 8) & 0x00FF00FFu;
    return rb | (ag << 8);
}
} // namespace

void Canvas::blit(const Image &sprite, int x, int y, uint8_t alpha) {
    const int x0 = std::max(0, x), x1 = std::min(w, x + sprite.width);
    const int y0 = std::max(0, y), y1 = std::min(h, y + sprite.height);
    if (x0 >= x1 || y0 >= y1)
        return;
    parallel_rows(y0, y1, static_cast<size_t>(y1 - y0) * (x1 - x0), [&](int a, int b) {
        for (int yy = a; yy < b; yy++) {
            const uint32_t *src = &sprite.pixels[static_cast<size_t>(yy - y) * sprite.width + (x0 - x)];
            uint32_t *dst = &pixels[static_cast<size_t>(yy) * w + x0];
            for (int xx = x0; xx < x1; xx++, src++, dst++) {
                uint32_t p = *src;
                if (alpha != 255)
                    p = fade(p, alpha + 1u);
                if ((p >> 24) == 0)
                    continue;
                *dst = over(*dst, p);
            }
        }
    });
}

void Canvas::blit_scaled(const Image &sprite, float cx, float cy, float scale, uint8_t alpha) {
    if (sprite.empty() || scale <= 0.01f)
        return;
    if (std::fabs(scale - 1.f) < 0.004f) {
        blit(sprite, static_cast<int>(std::lround(cx - sprite.width / 2.f)), static_cast<int>(std::lround(cy - sprite.height / 2.f)), alpha);
        return;
    }
    const float half_w = sprite.width * scale / 2, half_h = sprite.height * scale / 2;
    const int x0 = std::max(0, static_cast<int>(cx - half_w)), x1 = std::min(w, static_cast<int>(cx + half_w) + 1);
    const int y0 = std::max(0, static_cast<int>(cy - half_h)), y1 = std::min(h, static_cast<int>(cy + half_h) + 1);
    if (x0 >= x1 || y0 >= y1)
        return;
    const float inv = 1.f / scale;
    // 16.16 fixed point source coordinates, bilinear
    const int32_t step = static_cast<int32_t>(inv * 65536.f);
    const int sw = sprite.width, sh = sprite.height;
    const uint32_t *const sp = sprite.pixels.data();
    parallel_rows(y0, y1, static_cast<size_t>(y1 - y0) * (x1 - x0), [&](int a, int b) {
        for (int yy = a; yy < b; yy++) {
            const float sy = (yy + 0.5f - cy) * inv + sh / 2.f - 0.5f;
            if (sy < -1.f || sy >= sh)
                continue;
            const int iy = static_cast<int>(std::floor(sy));
            const uint32_t fy = static_cast<uint32_t>((sy - iy) * 256.f);
            const bool top_in = iy >= 0, bottom_in = iy + 1 < sh;
            const uint32_t *const row0 = sp + static_cast<size_t>(std::max(iy, 0)) * sw;
            const uint32_t *const row1 = sp + static_cast<size_t>(std::min(iy + 1, sh - 1)) * sw;
            int32_t sx = static_cast<int32_t>(((x0 + 0.5f - cx) * inv + sw / 2.f - 0.5f) * 65536.f);
            uint32_t *dst = &pixels[static_cast<size_t>(yy) * w];
            for (int xx = x0; xx < x1; xx++, sx += step) {
                const int ix = sx >> 16;
                if (ix < -1 || ix >= sw)
                    continue;
                const uint32_t fx = (static_cast<uint32_t>(sx) >> 8) & 0xFF;
                uint32_t p00, p10, p01, p11;
                if (top_in && bottom_in && ix >= 0 && ix + 1 < sw) {
                    // (inside the picture, as nearly all of them are)
                    p00 = row0[ix];
                    p10 = row0[ix + 1];
                    p01 = row1[ix];
                    p11 = row1[ix + 1];
                } else {
                    const bool left_in = ix >= 0, right_in = ix + 1 < sw;
                    p00 = top_in && left_in ? row0[ix] : 0u;
                    p10 = top_in && right_in ? row0[ix + 1] : 0u;
                    p01 = bottom_in && left_in ? row1[ix] : 0u;
                    p11 = bottom_in && right_in ? row1[ix + 1] : 0u;
                }
                if ((p00 | p10 | p01 | p11) == 0)
                    continue;
                uint32_t p = lerp(lerp(p00, p10, fx), lerp(p01, p11, fx), fy);
                if (alpha != 255)
                    p = fade(p, alpha + 1u);
                if ((p >> 24) == 0)
                    continue;
                dst[xx] = over(dst[xx], p);
            }
        }
    });
}

void Canvas::blend(int x, int y, Color c, uint8_t cov) {
    if (x < 0 || y < 0 || x >= w || y >= h || cov == 0)
        return;
    const uint32_t a = mul255(c.a, cov);
    if (a == 0)
        return;
    uint32_t &dst = pixels[static_cast<size_t>(y) * w + x];
    if (a == 255) {
        dst = pack(Color{ c.r, c.g, c.b, 255 });
        return;
    }
    // (d * (255 - a) + c * a) / 255, rounded, without a division
    const uint32_t ia = 255 - a;
    const auto mix = [](uint32_t d, uint32_t s, uint32_t sa, uint32_t sia) {
        const uint32_t t = d * sia + s * sa + 128;
        return (t + (t >> 8)) >> 8;
    };
    const uint32_t r = mix(dst & 0xFF, c.r, a, ia);
    const uint32_t g = mix((dst >> 8) & 0xFF, c.g, a, ia);
    const uint32_t b = mix((dst >> 16) & 0xFF, c.b, a, ia);
    dst = r | (g << 8) | (b << 16) | 0xFF000000u;
}

void Canvas::clear_gradient(Color top, Color bottom) {
    parallel_rows(0, h, static_cast<size_t>(w) * h, [&](int a, int b) {
        for (int y = a; y < b; y++) {
            const float t = static_cast<float>(y) / std::max(1, h - 1);
            const Color c{
                static_cast<uint8_t>(top.r + (bottom.r - top.r) * t),
                static_cast<uint8_t>(top.g + (bottom.g - top.g) * t),
                static_cast<uint8_t>(top.b + (bottom.b - top.b) * t),
                255
            };
            std::fill_n(&pixels[static_cast<size_t>(y) * w], w, pack(c));
        }
    });
}

void Canvas::fill_rect(int x, int y, int width, int height, Color c) {
    const int x0 = std::max(x, 0), x1 = std::min(x + width, w);
    const int y0 = std::max(y, 0), y1 = std::min(y + height, h);
    if (c.a == 0)
        return;
    const uint32_t a = c.a;
    const uint32_t src = a == 255 ? pack(c) : ((c.r * a / 255) | ((c.g * a / 255) << 8) | ((c.b * a / 255) << 16) | (a << 24));
    if (x0 >= x1 || y0 >= y1)
        return;
    parallel_rows(y0, y1, static_cast<size_t>(y1 - y0) * (x1 - x0), [&](int ra, int rb) {
        for (int yy = ra; yy < rb; yy++) {
            uint32_t *row = &pixels[static_cast<size_t>(yy) * w];
            if (a == 255) {
                std::fill(row + x0, row + x1, src);
                continue;
            }
            for (int xx = x0; xx < x1; xx++)
                row[xx] = over(row[xx], src);
        }
    });
}

void Canvas::fill_round_rect(int x, int y, int width, int height, int radius, Color c) {
    radius = std::min({ radius, width / 2, height / 2 });
    if (radius <= 0) {
        fill_rect(x, y, width, height, c);
        return;
    }
    if (c.a == 0)
        return;
    const uint32_t a = c.a;
    const uint32_t src = a == 255 ? pack(c) : ((c.r * a / 255) | ((c.g * a / 255) << 8) | ((c.b * a / 255) << 16) | (a << 24));
    const int x0 = std::max(x, 0), x1 = std::min(x + width, w);
    const int y0 = std::max(y, 0), y1 = std::min(y + height, h);
    if (x0 >= x1 || y0 >= y1)
        return;
    const float r = static_cast<float>(radius);
    const float left = static_cast<float>(x + radius), right = static_cast<float>(x + width - radius);
    const float top = static_cast<float>(y + radius), bottom = static_cast<float>(y + height - radius);
    // per row: the run fully inside in one go, only the pixels near the corners' arcs worked out
    // (each pixel's distance to its corner was a hypot and a blend for every pixel of the row)
    parallel_rows(y0, y1, static_cast<size_t>(y1 - y0) * (x1 - x0), [&](int ra, int rb) {
        for (int yy = ra; yy < rb; yy++) {
            uint32_t *row = &pixels[static_cast<size_t>(yy) * w];
            const float py = yy + 0.5f;
            const float dy = py - std::clamp(py, top, bottom);
            // how far from the corners' centres the row is fully inside, and touched at all
            const float full = (r - 0.5f) * (r - 0.5f) - dy * dy;
            const float touched = (r + 0.5f) * (r + 0.5f) - dy * dy;
            if (touched <= 0)
                continue;
            const float in = full > 0 ? std::sqrt(full) : -1.f;
            const float out = std::sqrt(touched);
            const int full0 = in >= 0 ? std::max(x0, static_cast<int>(std::ceil(left - in - 0.5f))) : x1;
            const int full1 = in >= 0 ? std::min(x1, static_cast<int>(std::floor(right + in - 0.5f)) + 1) : x1;
            const int edge0 = std::max(x0, static_cast<int>(std::floor(left - out - 0.5f)));
            const int edge1 = std::min(x1, static_cast<int>(std::ceil(right + out - 0.5f)) + 1);
            const auto edge_pixel = [&](int xx) {
                const float px = xx + 0.5f;
                const float d = std::hypot(px - std::clamp(px, left, right), dy);
                blend(xx, yy, c, coverage(r - d + 0.5f));
            };
            for (int xx = edge0; xx < std::min(full0, edge1); xx++)
                edge_pixel(xx);
            if (a == 255)
                std::fill(row + std::max(full0, x0), row + std::max(full1, full0), src);
            else
                for (int xx = full0; xx < full1; xx++)
                    row[xx] = over(row[xx], src);
            for (int xx = std::max(full1, edge0); xx < edge1; xx++)
                edge_pixel(xx);
        }
    });
}

void Canvas::fill_circle(float cx, float cy, float radius, Color c) {
    // per row: the run inside the circle in one go, only its two edges anti-aliased
    if (c.a == 0)
        return;
    const uint32_t a = c.a;
    const uint32_t src = a == 255 ? pack(c) : ((c.r * a / 255) | ((c.g * a / 255) << 8) | ((c.b * a / 255) << 16) | (a << 24));
    for (int yy = std::max(0, static_cast<int>(cy - radius - 1)); yy <= std::min(h - 1, static_cast<int>(cy + radius + 1)); yy++) {
        const float dy = yy + 0.5f - cy;
        const float outer2 = (radius + 1) * (radius + 1) - dy * dy;
        if (outer2 <= 0)
            continue;
        const float inner2 = (radius - 1) * (radius - 1) - dy * dy;
        const float inner = inner2 > 0 ? std::sqrt(inner2) : -1.f;
        const int in0 = inner > 0 ? std::max(0, static_cast<int>(std::ceil(cx - inner + 0.5f))) : 1;
        const int in1 = inner > 0 ? std::min(w, static_cast<int>(std::floor(cx + inner - 0.5f))) : 0;
        const float outer = std::sqrt(outer2);
        const int e0 = std::max(0, static_cast<int>(cx - outer - 1)), e1 = std::min(w - 1, static_cast<int>(cx + outer + 1));
        uint32_t *row = &pixels[static_cast<size_t>(yy) * w];
        for (int xx = e0; xx <= e1; xx++) {
            if (xx >= in0 && xx < in1) {
                for (; xx < in1; xx++)
                    row[xx] = over(row[xx], src);
                xx--;
                continue;
            }
            blend(xx, yy, c, coverage(radius - std::hypot(xx + 0.5f - cx, dy) + 0.5f));
        }
    }
}

void Canvas::ring(float cx, float cy, float radius, float thickness, Color c) {
    const float half = thickness / 2;
    for (int yy = static_cast<int>(cy - radius - half - 1); yy <= static_cast<int>(cy + radius + half + 1); yy++)
        for (int xx = static_cast<int>(cx - radius - half - 1); xx <= static_cast<int>(cx + radius + half + 1); xx++)
            blend(xx, yy, c, coverage(half - std::fabs(std::hypot(xx + 0.5f - cx, yy + 0.5f - cy) - radius) + 0.5f));
}

void Canvas::line(float x0, float y0, float x1, float y1, float thickness, Color c) {
    const float half = thickness / 2;
    const float dx = x1 - x0, dy = y1 - y0;
    const float len2 = std::max(dx * dx + dy * dy, 1e-6f);
    for (int yy = static_cast<int>(std::min(y0, y1) - half - 1); yy <= static_cast<int>(std::max(y0, y1) + half + 1); yy++) {
        for (int xx = static_cast<int>(std::min(x0, x1) - half - 1); xx <= static_cast<int>(std::max(x0, x1) + half + 1); xx++) {
            const float px = xx + 0.5f, py = yy + 0.5f;
            const float t = std::clamp(((px - x0) * dx + (py - y0) * dy) / len2, 0.f, 1.f);
            const float d = std::hypot(px - (x0 + t * dx), py - (y0 + t * dy));
            blend(xx, yy, c, coverage(half - d + 0.5f));
        }
    }
}

void Canvas::draw_image(const Image &image, int x, int y, uint8_t alpha) {
    // (a picture's own alpha, not premultiplied: each pixel made so for over(), without a division)
    const int x0 = std::max(0, x), x1 = std::min(w, x + image.width);
    const int y0 = std::max(0, y), y1 = std::min(h, y + image.height);
    if (x0 >= x1 || y0 >= y1)
        return;
    parallel_rows(y0, y1, static_cast<size_t>(y1 - y0) * (x1 - x0), [&](int ra, int rb) {
        for (int yy = ra; yy < rb; yy++) {
            const uint32_t *src = &image.pixels[static_cast<size_t>(yy - y) * image.width + (x0 - x)];
            uint32_t *dst = &pixels[static_cast<size_t>(yy) * w + x0];
            for (int xx = x0; xx < x1; xx++, src++, dst++) {
                const uint32_t p = *src;
                const uint32_t a = mul255(p >> 24, alpha);
                if (a == 0)
                    continue;
                if (a == 255) {
                    *dst = p | 0xFF000000u;
                    continue;
                }
                *dst = over(*dst, mul255(p & 0xFF, a) | (mul255((p >> 8) & 0xFF, a) << 8) | (mul255((p >> 16) & 0xFF, a) << 16) | (a << 24));
            }
        }
    });
}

void Canvas::draw_image_circle(const Image &image, float cx, float cy, float radius, uint8_t alpha) {
    const int left = static_cast<int>(cx - image.width / 2.f);
    const int top = static_cast<int>(cy - image.height / 2.f);
    for (int yy = static_cast<int>(cy - radius - 1); yy <= static_cast<int>(cy + radius + 1); yy++) {
        for (int xx = static_cast<int>(cx - radius - 1); xx <= static_cast<int>(cx + radius + 1); xx++) {
            const int ix = xx - left, iy = yy - top;
            if (ix < 0 || iy < 0 || ix >= image.width || iy >= image.height)
                continue;
            const uint8_t edge = coverage(radius - std::hypot(xx + 0.5f - cx, yy + 0.5f - cy) + 0.5f);
            if (edge == 0)
                continue;
            const uint32_t p = image.pixels[static_cast<size_t>(iy) * image.width + ix];
            const Color c{ static_cast<uint8_t>(p), static_cast<uint8_t>(p >> 8), static_cast<uint8_t>(p >> 16), static_cast<uint8_t>(p >> 24) };
            blend(xx, yy, c, static_cast<uint8_t>(edge * alpha / 255));
        }
    }
}

int Canvas::text_width(std::string_view utf8, int px, Weight weight) {
    const std::lock_guard<std::mutex> lock(fonts_mutex);
    FontSet &set = font_set(weight);
    if (needs_shaping(utf8))
        return static_cast<int>(shape(set, utf8, px, nullptr) + 0.5f);
    float width = 0;
    for (size_t i = 0; i < utf8.size();) {
        const char32_t cp = next_code_point(utf8, i);
        if (const Glyph *g = glyph_for(set, cp, px))
            width += g->advance;
    }
    return static_cast<int>(width + 0.5f);
}

int Canvas::text(int x, int y, std::string_view utf8, int px, Color c, Weight weight, int max_width) {
    if (max_width > 0 && text_width(utf8, px, weight) > max_width) {
        // cut it short, with an ellipsis
        const int ellipsis = text_width("...", px, weight);
        size_t cut = 0, i = 0;
        std::string_view kept;
        while (i < utf8.size()) {
            next_code_point(utf8, i);
            if (text_width(utf8.substr(0, i), px, weight) + ellipsis > max_width)
                break;
            cut = i;
        }
        kept = utf8.substr(0, cut);
        if (rtl_paragraph(kept)) {
            // a right-to-left text ends on the left
            const int dots = text(x, y, "...", px, c, weight);
            return dots + text(x + dots, y, kept, px, c, weight);
        }
        const int drawn = text(x, y, kept, px, c, weight);
        return drawn + text(x + drawn, y, "...", px, c, weight);
    }
    const std::lock_guard<std::mutex> lock(fonts_mutex);
    FontSet &set = font_set(weight);
    if (needs_shaping(utf8)) {
        std::vector<Placed> placed;
        const float width = shape(set, utf8, px, &placed);
        for (const Placed &p : placed) {
            const Glyph *g = glyph_by_index(set, p.font, p.glyph, px);
            if (!g)
                continue;
            const int gx = static_cast<int>(x + p.x + 0.5f) + g->xoff;
            const int gy = static_cast<int>(y + p.y + 0.5f) + g->yoff;
            for (int yy = 0; yy < g->height; yy++)
                for (int xx = 0; xx < g->width; xx++)
                    blend(gx + xx, gy + yy, c, g->alpha[static_cast<size_t>(yy) * g->width + xx]);
        }
        return static_cast<int>(width + 0.5f);
    }
    float pen = static_cast<float>(x);
    for (size_t i = 0; i < utf8.size();) {
        const char32_t cp = next_code_point(utf8, i);
        const Glyph *g = glyph_for(set, cp, px);
        if (!g)
            continue;
        const int gx = static_cast<int>(pen + 0.5f) + g->xoff;
        const int gy = y + g->yoff;
        for (int yy = 0; yy < g->height; yy++)
            for (int xx = 0; xx < g->width; xx++)
                blend(gx + xx, gy + yy, c, g->alpha[static_cast<size_t>(yy) * g->width + xx]);
        pen += g->advance;
    }
    return static_cast<int>(pen - x + 0.5f);
}

void Canvas::text_centered(int cx, int y, std::string_view utf8, int px, Color c, Weight weight) {
    text(cx - text_width(utf8, px, weight) / 2, y, utf8, px, c, weight);
}

int Canvas::text_wrapped(int x, int y, std::string_view utf8, int px, Color c, int max_width, int line_height, Weight weight) {
    size_t start = 0;
    while (start < utf8.size()) {
        // the longest run of words that fits (or one word cut short)
        size_t end = start, last_space = std::string_view::npos;
        while (end < utf8.size() && utf8[end] != '\n') {
            size_t next = end;
            next_code_point(utf8, next);
            if (text_width(utf8.substr(start, next - start), px, weight) > max_width)
                break;
            if (utf8[end] == ' ')
                last_space = end;
            end = next;
        }
        if (end < utf8.size() && utf8[end] != '\n' && last_space != std::string_view::npos && last_space > start)
            end = last_space;
        if (end == start && start < utf8.size() && utf8[start] != '\n')
            next_code_point(utf8, end); // a single character wider than the line
        text(x, y, utf8.substr(start, end - start), px, c, weight);
        y += line_height;
        start = end;
        while (start < utf8.size() && (utf8[start] == ' ' || utf8[start] == '\n')) {
            const bool newline = utf8[start] == '\n';
            start++;
            if (newline)
                break;
        }
    }
    return y;
}

void Canvas::cross_button(float cx, float cy, float r) {
    fill_circle(cx, cy, r, rgb(0x20242c));
    const float s = r * 0.42f;
    line(cx - s, cy - s, cx + s, cy + s, r * 0.18f, rgb(0x8fb6ff));
    line(cx - s, cy + s, cx + s, cy - s, r * 0.18f, rgb(0x8fb6ff));
}

void Canvas::circle_button(float cx, float cy, float r) {
    fill_circle(cx, cy, r, rgb(0x20242c));
    ring(cx, cy, r * 0.48f, r * 0.17f, rgb(0xff7a7a));
}

void Canvas::triangle_button(float cx, float cy, float r) {
    fill_circle(cx, cy, r, rgb(0x20242c));
    const float s = r * 0.55f;
    const float t = r * 0.16f;
    const float ax = cx, ay = cy - s * 0.95f;
    const float bx = cx - s, by = cy + s * 0.6f;
    const float qx = cx + s, qy = cy + s * 0.6f;
    line(ax, ay, bx, by, t, rgb(0x4fe0b0));
    line(bx, by, qx, qy, t, rgb(0x4fe0b0));
    line(qx, qy, ax, ay, t, rgb(0x4fe0b0));
}

void Canvas::square_button(float cx, float cy, float r) {
    fill_circle(cx, cy, r, rgb(0x20242c));
    const float s = r * 0.42f;
    const float t = r * 0.16f;
    line(cx - s, cy - s, cx + s, cy - s, t, rgb(0xff8ad8));
    line(cx + s, cy - s, cx + s, cy + s, t, rgb(0xff8ad8));
    line(cx + s, cy + s, cx - s, cy + s, t, rgb(0xff8ad8));
    line(cx - s, cy + s, cx - s, cy - s, t, rgb(0xff8ad8));
}

void Canvas::pill_button(int x, int y, std::string_view label, int px) {
    const int tw = text_width(label, px, Weight::Bold);
    const int height = px + 14;
    fill_round_rect(x, y - height / 2, tw + 28, height, height / 2, rgb(0x20242c));
    text(x + 14, y + px / 3, label, px, rgb(0xd8dde8), Weight::Bold);
}

} // namespace ps4ui
