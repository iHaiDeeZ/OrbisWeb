// Wisp: the mouse pointer's pictures (ui/cursor.h). Each shape is a signed distance field in design
// units (about a pixel of a desktop's pointer; the hot spot at 0,0), drawn once at the pointer's size
// (settings.txt pointer_size, percent; 160 by default: a TV is further away than a monitor) with 4x4
// samples a pixel: smooth edges, a black border and a soft shadow, so the pointer shows on light and
// dark pages alike.
#include "cursor.h"

#include "../common/wisp.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <vector>

namespace wisp {
namespace {

struct P {
    float x, y;
};

struct Sprite {
    int w = 0, h = 0, hot_x = 0, hot_y = 0;
    std::vector<uint32_t> px;   // premultiplied 0xAARRGGBB
};

float segment_distance(float x, float y, P a, P b)
{
    const float ex = b.x - a.x, ey = b.y - a.y, wx = x - a.x, wy = y - a.y;
    const float length = ex * ex + ey * ey;
    const float t = length > 0 ? std::clamp((wx * ex + wy * ey) / length, 0.f, 1.f) : 0.f;
    const float dx = wx - ex * t, dy = wy - ey * t;
    return std::sqrt(dx * dx + dy * dy);
}

float capsule(float x, float y, P a, P b, float r) { return segment_distance(x, y, a, b) - r; }

float rounded_box(float x, float y, float x0, float y0, float x1, float y1, float r)
{
    const float cx = (x0 + x1) / 2, cy = (y0 + y1) / 2;
    const float qx = std::fabs(x - cx) - ((x1 - x0) / 2 - r), qy = std::fabs(y - cy) - ((y1 - y0) / 2 - r);
    const float ox = std::max(qx, 0.f), oy = std::max(qy, 0.f);
    return std::sqrt(ox * ox + oy * oy) + std::min(std::max(qx, qy), 0.f) - r;
}

float polygon(float x, float y, const P *v, int n)
{
    float d = 1e9f;
    bool inside = false;
    for (int i = 0, j = n - 1; i < n; j = i++) {
        d = std::min(d, segment_distance(x, y, v[j], v[i]));
        if ((v[i].y > y) != (v[j].y > y) && x < (v[j].x - v[i].x) * (y - v[i].y) / (v[j].y - v[i].y) + v[i].x)
            inside = !inside;
    }
    return inside ? -d : d;
}

// Filled: white inside a black border (the arrow, the hand). Beam: a black core with a white outline
// around it (the text beam, the crosshair), as desktops draw those.
enum class Style { Filled, Beam };

struct Shape {
    std::function<float(float, float)> distance;   // < 0 inside
    std::function<float(float, float)> lines;      // distance to the lines drawn inside (the hand's fingers)
    float x0, y0, x1, y1;                          // the design bounds
    Style style;
};

float scale_setting()
{
    return std::clamp(wisp_setting_int("pointer_size", 160), 80, 400) / 100.f;
}

Sprite render(const Shape &shape)
{
    const float scale = scale_setting();
    const float border = 1.2f, outline = 1.1f, line_half = 0.42f;
    const P shadow = { 1.1f, 1.5f };
    const float soft = 1.3f, shadow_alpha = 0.4f;
    const float margin = (shape.style == Style::Beam ? outline : 0.f) + soft + 1.6f;
    const float x0 = shape.x0 - margin, y0 = shape.y0 - margin;
    const float x1 = shape.x1 + margin + shadow.x, y1 = shape.y1 + margin + shadow.y;
    Sprite s;
    s.w = static_cast<int>(std::ceil((x1 - x0) * scale));
    s.h = static_cast<int>(std::ceil((y1 - y0) * scale));
    s.hot_x = static_cast<int>(std::lround(-x0 * scale));
    s.hot_y = static_cast<int>(std::lround(-y0 * scale));
    s.px.assign(static_cast<size_t>(s.w) * s.h, 0);
    constexpr int N = 4;
    for (int py = 0; py < s.h; py++) {
        for (int px = 0; px < s.w; px++) {
            float a = 0, r = 0, g = 0, b = 0;   // premultiplied sums
            for (int sy = 0; sy < N; sy++) {
                for (int sx = 0; sx < N; sx++) {
                    const float ux = x0 + (px + (sx + 0.5f) / N) / scale;
                    const float uy = y0 + (py + (sy + 0.5f) / N) / scale;
                    const float d = shape.distance(ux, uy);
                    float c = -1;   // the sample's grey (0 black, 1 white), -1 none
                    if (shape.style == Style::Filled) {
                        if (d <= 0)
                            c = d > -border || (shape.lines && shape.lines(ux, uy) < line_half) ? 0.f : 1.f;
                    } else if (d <= 0) {
                        c = 0.f;
                    } else if (d <= outline) {
                        c = 1.f;
                    }
                    if (c >= 0) {
                        a += 1;
                        r += c;
                        g += c;
                        b += c;
                        continue;
                    }
                    // the shadow under it: the shape moved down-right, its edge softened
                    const float ds = shape.distance(ux - shadow.x, uy - shadow.y) - (shape.style == Style::Beam ? outline : 0.f);
                    const float t = std::clamp((soft - ds) / (2 * soft), 0.f, 1.f);
                    a += shadow_alpha * t * t * (3 - 2 * t);
                }
            }
            constexpr float samples = N * N;
            const auto byte = [](float v) { return static_cast<uint32_t>(std::lround(std::clamp(v, 0.f, 1.f) * 255)); };
            s.px[static_cast<size_t>(py) * s.w + px] = byte(a / samples) << 24 | byte(r / samples) << 16 |
                byte(g / samples) << 8 | byte(b / samples);
        }
    }
    return s;
}

Shape arrow_shape()
{
    // the classic arrow: the tip at the hot spot, a notch on each side of the tail
    static const P v[] = { { 0, 0 }, { 0, 16.6f }, { 4.0f, 12.8f }, { 6.9f, 19.3f }, { 9.4f, 18.2f }, { 6.6f, 11.9f },
        { 11.9f, 11.9f } };
    return { [](float x, float y) { return polygon(x, y, v, 7); }, nullptr, 0, 0, 11.9f, 19.3f, Style::Filled };
}

Shape hand_shape()
{
    // a hand pointing up: the index finger's tip at the hot spot, three fingers folded, the thumb out
    const auto distance = [](float x, float y) {
        float d = capsule(x, y, { 0, 1.6f }, { 0, 10.5f }, 1.6f);                       // index finger
        d = std::min(d, rounded_box(x, y, -1.6f, 8.2f, 10.4f, 19.2f, 3.0f));           // palm
        d = std::min(d, capsule(x, y, { 3.2f, 8.6f }, { 3.2f, 10.6f }, 1.6f));          // middle finger
        d = std::min(d, capsule(x, y, { 6.3f, 9.0f }, { 6.3f, 11.0f }, 1.55f));         // ring finger
        d = std::min(d, capsule(x, y, { 9.0f, 9.9f }, { 9.0f, 11.8f }, 1.4f));          // little finger
        d = std::min(d, capsule(x, y, { -1.2f, 14.6f }, { -4.6f, 11.0f }, 1.55f));      // thumb
        return d;
    };
    const auto lines = [](float x, float y) {
        float d = segment_distance(x, y, { 1.6f, 9.2f }, { 1.6f, 12.2f });
        d = std::min(d, segment_distance(x, y, { 4.75f, 9.6f }, { 4.75f, 12.4f }));
        d = std::min(d, segment_distance(x, y, { 7.65f, 10.4f }, { 7.65f, 12.8f }));
        return d;
    };
    return { distance, lines, -6.2f, 0, 10.4f, 19.2f, Style::Filled };
}

Shape text_shape()
{
    // the text beam, centered on the hot spot
    const auto distance = [](float x, float y) {
        float d = capsule(x, y, { 0, -9.0f }, { 0, 9.0f }, 0.7f);
        d = std::min(d, capsule(x, y, { -3.0f, -9.8f }, { -0.6f, -9.8f }, 0.7f));
        d = std::min(d, capsule(x, y, { 0.6f, -9.8f }, { 3.0f, -9.8f }, 0.7f));
        d = std::min(d, capsule(x, y, { -3.0f, 9.8f }, { -0.6f, 9.8f }, 0.7f));
        d = std::min(d, capsule(x, y, { 0.6f, 9.8f }, { 3.0f, 9.8f }, 0.7f));
        return d;
    };
    return { distance, nullptr, -3.7f, -10.5f, 3.7f, 10.5f, Style::Beam };
}

Shape crosshair_shape()
{
    const auto distance = [](float x, float y) {
        return std::min(capsule(x, y, { -8, 0 }, { 8, 0 }, 0.6f), capsule(x, y, { 0, -8 }, { 0, 8 }, 0.6f));
    };
    return { distance, nullptr, -8.6f, -8.6f, 8.6f, 8.6f, Style::Beam };
}

const Sprite &sprite(CursorShape shape)
{
    static Sprite sprites[4];
    static bool made[4];
    const int i = static_cast<int>(shape);
    if (!made[i]) {
        made[i] = true;
        switch (shape) {
        case CursorShape::Hand: sprites[i] = render(hand_shape()); break;
        case CursorShape::Text: sprites[i] = render(text_shape()); break;
        case CursorShape::Crosshair: sprites[i] = render(crosshair_shape()); break;
        default: sprites[i] = render(arrow_shape()); break;
        }
    }
    return sprites[i];
}

} // namespace

CursorShape cursor_shape_for(const std::string &name)
{
    if (name == "pointer" || name == "grab" || name == "grabbing" || name == "hand")
        return CursorShape::Hand;
    if (name == "text" || name == "vertical-text")
        return CursorShape::Text;
    if (name == "crosshair" || name == "cell")
        return CursorShape::Crosshair;
    if (name == "none")
        return CursorShape::None;
    return CursorShape::Arrow;
}

void cursor_rows(float y, CursorShape shape, int &y0, int &y1)
{
    y0 = y1 = 0;
    if (shape == CursorShape::None)
        return;
    const Sprite &s = sprite(shape);
    y0 = static_cast<int>(std::lround(y)) - s.hot_y;
    y1 = y0 + s.h;
}

void cursor_draw(uint32_t *pixels, int width, int height, float x, float y, CursorShape shape)
{
    if (shape == CursorShape::None)
        return;
    const Sprite &s = sprite(shape);
    const int x0 = static_cast<int>(std::lround(x)) - s.hot_x, y0 = static_cast<int>(std::lround(y)) - s.hot_y;
    for (int row = 0; row < s.h; row++) {
        const int dy = y0 + row;
        if (dy < 0 || dy >= height)
            continue;
        uint32_t *dst = pixels + static_cast<size_t>(dy) * width;
        const uint32_t *src = s.px.data() + static_cast<size_t>(row) * s.w;
        for (int col = 0; col < s.w; col++) {
            const int dx = x0 + col;
            const uint32_t p = src[col], alpha = p >> 24;
            if (dx < 0 || dx >= width || !alpha)
                continue;
            if (alpha == 255) {
                dst[dx] = p;
                continue;
            }
            const uint32_t d = dst[dx], keep = 255 - alpha;
            const auto mix = [&](int shift) {
                return std::min<uint32_t>(255, ((p >> shift) & 0xff) + (((d >> shift) & 0xff) * keep + 127) / 255) << shift;
            };
            dst[dx] = 0xff000000u | mix(16) | mix(8) | mix(0);
        }
    }
}

} // namespace wisp
