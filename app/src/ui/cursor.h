// Wisp: the mouse pointer's pictures (ui/cursor.cpp): an arrow, a hand over links, a text beam over
// text, as a desktop browser shows them, smoothed and with a soft shadow so they show on any page.
#pragma once

#include <cstdint>
#include <string>

namespace wisp {

enum class CursorShape { Arrow, Hand, Text, Crosshair, None };

// The shape for the CSS cursor name WebKit asks for ("default", "pointer", "text", "none", ...)
CursorShape cursor_shape_for(const std::string &css_name);

// Draws shape with its hot spot at (x, y) into a BGRA picture (0xAARRGGBB words) of width x height
void cursor_draw(uint32_t *pixels, int width, int height, float x, float y, CursorShape shape);
// The rows [y0, y1) cursor_draw covers with its hot spot at row y (none for CursorShape::None)
void cursor_rows(float y, CursorShape shape, int &y0, int &y1);

} // namespace wisp
