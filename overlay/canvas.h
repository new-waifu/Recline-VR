// Minimal software RGBA canvas for the dashboard overlay (no GPU dependency).
#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "font_data.h"

namespace recline {

struct Color {
  uint8_t r, g, b, a;
};

struct Rect {
  int x = 0, y = 0, w = 0, h = 0;
  bool Contains(int px, int py) const { return px >= x && py >= y && px < x + w && py < y + h; }
};

class Canvas {
 public:
  Canvas(int w, int h) : w_(w), h_(h), px_(size_t(w) * h * 4) {}
  int W() const { return w_; }
  int H() const { return h_; }
  const uint8_t* Data() const { return px_.data(); }
  uint8_t* MutableData() { return px_.data(); }

  void Clear(Color c) { FillRect({0, 0, w_, h_}, c); }

  void FillRect(Rect r, Color c) {
    const int x0 = r.x < 0 ? 0 : r.x, y0 = r.y < 0 ? 0 : r.y;
    const int x1 = r.x + r.w > w_ ? w_ : r.x + r.w, y1 = r.y + r.h > h_ ? h_ : r.y + r.h;
    for (int y = y0; y < y1; ++y)
      for (int x = x0; x < x1; ++x) Blend(x, y, c, 255);
  }

  void StrokeRect(Rect r, Color c, int t = 2) {
    FillRect({r.x, r.y, r.w, t}, c);
    FillRect({r.x, r.y + r.h - t, r.w, t}, c);
    FillRect({r.x, r.y, t, r.h}, c);
    FillRect({r.x + r.w - t, r.y, t, r.h}, c);
  }

  static int TextWidth(const std::string& s, const recline_font::FontFace& f) {
    return int(s.size()) * f.cellW;
  }

  // Draws ASCII text with its top-left at (x, y). Returns the end x.
  int Text(int x, int y, const std::string& s, const recline_font::FontFace& f, Color c) {
    for (char ch : s) {
      int code = static_cast<unsigned char>(ch);
      if (code < 32 || code > 126) code = '?';
      const uint8_t* cell = f.alpha + size_t(code - 32) * f.cellW * f.cellH;
      for (int cy = 0; cy < f.cellH; ++cy)
        for (int cx = 0; cx < f.cellW; ++cx) {
          const uint8_t a = cell[cy * f.cellW + cx];
          if (a) Blend(x + cx, y + cy, c, a);
        }
      x += f.cellW;
    }
    return x;
  }

  void TextCentered(Rect r, const std::string& s, const recline_font::FontFace& f, Color c) {
    Text(r.x + (r.w - TextWidth(s, f)) / 2, r.y + (r.h - f.cellH) / 2, s, f, c);
  }

 private:
  void Blend(int x, int y, Color c, uint8_t cov) {
    if (x < 0 || y < 0 || x >= w_ || y >= h_) return;
    uint8_t* p = &px_[(size_t(y) * w_ + x) * 4];
    const unsigned a = unsigned(c.a) * cov / 255;
    p[0] = uint8_t((c.r * a + p[0] * (255 - a)) / 255);
    p[1] = uint8_t((c.g * a + p[1] * (255 - a)) / 255);
    p[2] = uint8_t((c.b * a + p[2] * (255 - a)) / 255);
    p[3] = uint8_t(a + p[3] * (255 - a) / 255);
  }

  int w_, h_;
  std::vector<uint8_t> px_;
};

}  // namespace recline
