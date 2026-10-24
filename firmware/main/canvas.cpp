#include "canvas.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "font.h"

Canvas::Canvas(uint16_t width, uint16_t height, uint8_t *buffer, size_t buffer_size)
    : width_(width),
      height_(height),
      stride_((width + 3U) / 4U),
      buffer_(buffer),
      buffer_size_(buffer_size)
{
}

void Canvas::clear(GrayLevel color)
{
    const uint8_t value = static_cast<uint8_t>(color) & 0x03U;
    const uint8_t packed = static_cast<uint8_t>(value * 0x55U);
    if (buffer_ != nullptr) {
        std::memset(buffer_, packed, buffer_size_);
    }
    fill_accent(0);
}

void Canvas::set_accent_plane(uint8_t *buffer, size_t buffer_size)
{
    // A plane too small for the panel would make draw_pixel write past it.
    if (buffer != nullptr && buffer_size < accent_stride() * height_) {
        buffer = nullptr;
    }
    accent_ = buffer;
    accent_size_ = buffer != nullptr ? buffer_size : 0;
    fill_accent(0);
}

void Canvas::fill_accent(uint8_t accent)
{
    if (accent_ == nullptr) return;
    const uint8_t a = accent & 0x0FU;
    std::memset(accent_, static_cast<uint8_t>((a << 4) | a), accent_size_);
}

uint8_t Canvas::accent_at(int px, int py) const
{
    if (accent_ == nullptr || px < 0 || py < 0 || px >= width_ || py >= height_)
        return 0;
    const uint8_t b = accent_[static_cast<size_t>(py) * accent_stride() +
                              static_cast<size_t>(px) / 2U];
    return (px & 1) ? (b & 0x0FU) : (b >> 4);
}

void Canvas::to_physical(int lx, int ly, int &px, int &py) const
{
    switch (rotation_) {
        case CanvasRotation::PortraitRight:      // 90 deg
            px = static_cast<int>(width_) - 1 - ly;
            py = lx;
            break;
        case CanvasRotation::LandscapeFlipped:   // 180 deg
            px = static_cast<int>(width_) - 1 - lx;
            py = static_cast<int>(height_) - 1 - ly;
            break;
        case CanvasRotation::PortraitLeft:       // 270 deg
            px = ly;
            py = static_cast<int>(height_) - 1 - lx;
            break;
        case CanvasRotation::Landscape:
        default:
            px = lx;
            py = ly;
            break;
    }
}

void Canvas::to_logical(int px, int py, int &lx, int &ly) const
{
    switch (rotation_) {
        case CanvasRotation::PortraitRight:
            lx = py;
            ly = static_cast<int>(width_) - 1 - px;
            break;
        case CanvasRotation::LandscapeFlipped:
            lx = static_cast<int>(width_) - 1 - px;
            ly = static_cast<int>(height_) - 1 - py;
            break;
        case CanvasRotation::PortraitLeft:
            lx = static_cast<int>(height_) - 1 - py;
            ly = px;
            break;
        case CanvasRotation::Landscape:
        default:
            lx = px;
            ly = py;
            break;
    }
}

void Canvas::rect_to_physical(int lx, int ly, int lw, int lh,
                              int &px, int &py, int &pw, int &ph) const {
    int ax, ay, bx, by;
    to_physical(lx, ly, ax, ay);
    to_physical(lx + lw - 1, ly + lh - 1, bx, by);
    px = ax < bx ? ax : bx;
    py = ay < by ? ay : by;
    pw = (ax > bx ? ax - bx : bx - ax) + 1;
    ph = (ay > by ? ay - by : by - ay) + 1;
}

void Canvas::draw_pixel(int x, int y, GrayLevel color)
{
    // Clip in LOGICAL space first: a portrait canvas is 480 wide, and letting a
    // 700 px x through would wrap into the wrong physical row instead of being
    // dropped.
    if (buffer_ == nullptr || x < 0 || y < 0 || x >= width() || y >= height()) {
        return;
    }
    // Scroll clip: rows outside the body box are measured, not painted.
    if (clip_on_ && (y < clip_y0_ || y >= clip_y1_)) {
        return;
    }
    int rx = x, ry = y;
    to_physical(x, y, rx, ry);
    x = rx;
    y = ry;
    if (x < 0 || y < 0 || x >= width_ || y >= height_) {
        return;
    }

    const size_t index = static_cast<size_t>(y) * stride_ + static_cast<size_t>(x) / 4U;
    const uint8_t shift = static_cast<uint8_t>((3 - (x & 0x03)) * 2);
    const uint8_t mask = static_cast<uint8_t>(0x03U << shift);
    const uint8_t value = static_cast<uint8_t>(color) << shift;
    buffer_[index] = static_cast<uint8_t>((buffer_[index] & ~mask) | value);

    if (accent_ != nullptr) {
        uint8_t &a = accent_[static_cast<size_t>(y) * accent_stride() +
                             static_cast<size_t>(x) / 2U];
        a = (x & 1) ? static_cast<uint8_t>((a & 0xF0U) | pen_)
                    : static_cast<uint8_t>((a & 0x0FU) | (pen_ << 4));
    }
}

void Canvas::draw_line(int x0, int y0, int x1, int y1, GrayLevel color)
{
    const int dx = std::abs(x1 - x0);
    const int step_x = x0 < x1 ? 1 : -1;
    const int dy = -std::abs(y1 - y0);
    const int step_y = y0 < y1 ? 1 : -1;
    int error = dx + dy;

    while (true) {
        draw_pixel(x0, y0, color);
        if (x0 == x1 && y0 == y1) {
            break;
        }
        const int twice_error = 2 * error;
        if (twice_error >= dy) {
            error += dy;
            x0 += step_x;
        }
        if (twice_error <= dx) {
            error += dx;
            y0 += step_y;
        }
    }
}

void Canvas::draw_rect(int x, int y, int width, int height, GrayLevel color)
{
    if (width <= 0 || height <= 0) {
        return;
    }
    draw_line(x, y, x + width - 1, y, color);
    draw_line(x, y + height - 1, x + width - 1, y + height - 1, color);
    draw_line(x, y, x, y + height - 1, color);
    draw_line(x + width - 1, y, x + width - 1, y + height - 1, color);
}

void Canvas::fill_rect(int x, int y, int width, int height, GrayLevel color)
{
    if (width <= 0 || height <= 0) {
        return;
    }

    const int start_x = std::max(0, x);
    const int start_y = std::max(0, y);
    const int end_x = std::min<int>(this->width(), x + width);
    const int end_y = std::min<int>(this->height(), y + height);
    for (int row = start_y; row < end_y; ++row) {
        for (int column = start_x; column < end_x; ++column) {
            draw_pixel(column, row, color);
        }
    }
}

void Canvas::draw_text(int x, int y, const char *text, uint8_t scale, GrayLevel color)
{
    if (text == nullptr || scale == 0) {
        return;
    }

    int cursor_x = x;
    for (const char *character = text; *character != '\0'; ++character) {
        const FontGlyph &glyph = font_get_glyph(*character);
        for (uint8_t column = 0; column < kFontWidth; ++column) {
            for (uint8_t row = 0; row < kFontHeight; ++row) {
                if ((glyph.columns[column] & (1U << row)) != 0U) {
                    fill_rect(cursor_x + column * scale,
                              y + row * scale,
                              scale,
                              scale,
                              color);
                }
            }
        }
        cursor_x += (kFontWidth + kFontSpacing) * scale;
    }
}
