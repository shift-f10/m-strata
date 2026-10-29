#include "secure_frontend/tui_screen.hpp"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <optional>

namespace secure_frontend::tui {
namespace {

// The Unicode braille cell's dot bits: column 0 rows 0-3 are dots 1, 2, 3, 7; column 1 rows 0-3 dots 4, 5, 6, 8.
constexpr std::uint8_t kBrailleBit[2][4] = {{0x01, 0x02, 0x04, 0x40}, {0x08, 0x10, 0x20, 0x80}};
// Left-aligned eighths of a cell, 1/8 to 7/8 (U+258F down to U+2589); U+2588 is a whole one.
constexpr char32_t kLeftEighths[8] = {U' ', char32_t{0x258F}, char32_t{0x258E}, char32_t{0x258D}, char32_t{0x258C}, char32_t{0x258B}, char32_t{0x258A}, char32_t{0x2589}};
// Bottom-aligned eighths, 1/8 to 8/8.
constexpr char32_t kLowerEighths[9] = {U' ', char32_t{0x2581}, char32_t{0x2582}, char32_t{0x2583}, char32_t{0x2584}, char32_t{0x2585}, char32_t{0x2586}, char32_t{0x2587}, char32_t{0x2588}};

std::string sgr(const Style& style, const Capabilities& capabilities) {
  std::string out = "\x1b[0";
  if (style.bold) out += ";1";
  if (style.dim) out += ";2";
  if (style.reverse) out += ";7";
  if (capabilities.color) {
    if (style.fg >= 0) out += ";38;5;" + std::to_string(std::min(style.fg, 255));
    if (style.bg >= 0) out += ";48;5;" + std::to_string(std::min(style.bg, 255));
  }
  return out + "m";
}

std::string move_to(const int x, const int y) { return "\x1b[" + std::to_string(y + 1) + ";" + std::to_string(x + 1) + "H"; }

// What a cell shows for a code point of text: a control (C0, DEL, C1 - U+009B is a one-byte CSI where 8-bit controls
// are honoured) is a space, so no text from the server's answers can carry a terminal sequence.
char32_t shown(const char32_t c) { return c < 0x20 || (c >= 0x7F && c < 0xA0) ? U' ' : c; }

struct Range final {
  char32_t first;
  char32_t last;
};

// Combining marks (Unicode Mn/Me in the scripts a model or file name is likely to use): drawn on the cell before them.
constexpr Range kCombining[] = {
    {0x0300, 0x036F}, {0x0483, 0x0489}, {0x0591, 0x05BD}, {0x05BF, 0x05BF}, {0x05C1, 0x05C2}, {0x05C4, 0x05C5},
    {0x05C7, 0x05C7}, {0x0610, 0x061A}, {0x064B, 0x065F}, {0x0670, 0x0670}, {0x06D6, 0x06DC}, {0x06DF, 0x06E4},
    {0x06E7, 0x06E8}, {0x06EA, 0x06ED}, {0x0900, 0x0902}, {0x093A, 0x093A}, {0x093C, 0x093C}, {0x0941, 0x0948},
    {0x094D, 0x094D}, {0x0951, 0x0957}, {0x0962, 0x0963}, {0x0E31, 0x0E31}, {0x0E34, 0x0E3A}, {0x0E47, 0x0E4E},
    {0x1AB0, 0x1AFF}, {0x1DC0, 0x1DFF}, {0x20D0, 0x20FF}, {0x302A, 0x302D}, {0x3099, 0x309A}, {0xFE20, 0xFE2F}};
// Zero-width format characters, dropped: zero-width space and joiners, the bidi marks and controls, the word joiner and
// invisible operators, the variation selectors (an emoji presentation selector would widen the glyph before it), the
// BOM, the emoji skin-tone modifiers and the tag characters.
constexpr Range kFormat[] = {{0x200B, 0x200F}, {0x202A, 0x202E}, {0x2060, 0x2064}, {0x2066, 0x206F}, {0xFE00, 0xFE0F},
                             {0xFEFF, 0xFEFF}, {0x1F3FB, 0x1F3FF}, {0xE0001, 0xE0001}, {0xE0020, 0xE007F}, {0xE0100, 0xE01EF}};
// East Asian Wide and Fullwidth, and the emoji with an emoji presentation: two cells.
constexpr Range kWide[] = {
    {0x1100, 0x115F},   {0x231A, 0x231B},   {0x2329, 0x232A},   {0x23E9, 0x23EC},   {0x23F0, 0x23F0},   {0x23F3, 0x23F3},
    {0x25FD, 0x25FE},   {0x2614, 0x2615},   {0x2648, 0x2653},   {0x267F, 0x267F},   {0x2693, 0x2693},   {0x26A1, 0x26A1},
    {0x26AA, 0x26AB},   {0x26BD, 0x26BE},   {0x26C4, 0x26C5},   {0x26CE, 0x26CE},   {0x26D4, 0x26D4},   {0x26EA, 0x26EA},
    {0x26F2, 0x26F3},   {0x26F5, 0x26F5},   {0x26FA, 0x26FA},   {0x26FD, 0x26FD},   {0x2705, 0x2705},   {0x270A, 0x270B},
    {0x2728, 0x2728},   {0x274C, 0x274C},   {0x274E, 0x274E},   {0x2753, 0x2755},   {0x2757, 0x2757},   {0x2795, 0x2797},
    {0x27B0, 0x27B0},   {0x27BF, 0x27BF},   {0x2B1B, 0x2B1C},   {0x2B50, 0x2B50},   {0x2B55, 0x2B55},   {0x2E80, 0x303E},
    {0x3041, 0x33FF},   {0x3400, 0x4DBF},   {0x4E00, 0x9FFF},   {0xA000, 0xA4CF},   {0xA960, 0xA97F},   {0xAC00, 0xD7A3},
    {0xF900, 0xFAFF},   {0xFE10, 0xFE19},   {0xFE30, 0xFE6F},   {0xFF00, 0xFF60},   {0xFFE0, 0xFFE6},   {0x16FE0, 0x16FE4},
    {0x17000, 0x18AFF}, {0x1B000, 0x1B2FF}, {0x1F004, 0x1F004}, {0x1F0CF, 0x1F0CF}, {0x1F18E, 0x1F18E}, {0x1F191, 0x1F19A},
    {0x1F200, 0x1F202}, {0x1F210, 0x1F23B}, {0x1F240, 0x1F248}, {0x1F250, 0x1F251}, {0x1F260, 0x1F265}, {0x1F300, 0x1F64F},
    {0x1F680, 0x1F6FF}, {0x1F7E0, 0x1F7EB}, {0x1F90C, 0x1F9FF}, {0x1FA70, 0x1FAFF}, {0x20000, 0x2FFFD}, {0x30000, 0x3FFFD}};

template <std::size_t N>
bool in(const Range (&ranges)[N], const char32_t c) {
  const auto* found = std::upper_bound(std::begin(ranges), std::end(ranges), c, [](const char32_t v, const Range& r) { return v < r.first; });
  return found != std::begin(ranges) && c <= (found - 1)->last;
}

bool combining(const char32_t c) { return in(kCombining, c); }

}  // namespace

std::string utf8(const char32_t glyph) {
  std::string out;
  const auto c = static_cast<std::uint32_t>(glyph);
  if (c < 0x80U) {
    out.push_back(static_cast<char>(c));
  } else if (c < 0x800U) {
    out.push_back(static_cast<char>(0xC0U | (c >> 6U)));
    out.push_back(static_cast<char>(0x80U | (c & 0x3FU)));
  } else if (c < 0x10000U) {
    out.push_back(static_cast<char>(0xE0U | (c >> 12U)));
    out.push_back(static_cast<char>(0x80U | ((c >> 6U) & 0x3FU)));
    out.push_back(static_cast<char>(0x80U | (c & 0x3FU)));
  } else {
    out.push_back(static_cast<char>(0xF0U | (c >> 18U)));
    out.push_back(static_cast<char>(0x80U | ((c >> 12U) & 0x3FU)));
    out.push_back(static_cast<char>(0x80U | ((c >> 6U) & 0x3FU)));
    out.push_back(static_cast<char>(0x80U | (c & 0x3FU)));
  }
  return out;
}

std::u32string code_points(const std::string_view text) {
  std::u32string out;
  for (std::size_t i = 0; i < text.size();) {
    const auto lead = static_cast<unsigned char>(text[i]);
    const std::size_t width = lead < 0x80U ? 1U : (lead >> 5U) == 0x6U ? 2U : (lead >> 4U) == 0xEU ? 3U : (lead >> 3U) == 0x1EU ? 4U : 0U;
    if (width == 0U || i + width > text.size()) {
      out.push_back(char32_t{0xFFFD});
      ++i;
      continue;
    }
    char32_t c = width == 1U ? lead : width == 2U ? (lead & 0x1FU) : width == 3U ? (lead & 0x0FU) : (lead & 0x07U);
    bool valid = true;
    for (std::size_t k = 1; k < width; ++k) {
      const auto next = static_cast<unsigned char>(text[i + k]);
      if ((next & 0xC0U) != 0x80U) valid = false;
      c = (c << 6U) | (next & 0x3FU);
    }
    out.push_back(valid ? c : char32_t{0xFFFD});
    i += valid ? width : 1U;
  }
  return out;
}

int cell_width(const char32_t glyph) {
  const char32_t c = shown(glyph);
  if (c < 0x300) return 1;
  if (in(kCombining, c) || in(kFormat, c)) return 0;
  return in(kWide, c) ? 2 : 1;
}

int text_width(const std::string_view text) {
  int cells = 0;
  for (const char32_t c : code_points(text)) cells += cell_width(c);
  return cells;
}

Cell& Screen::cell(const int x, const int y) noexcept {
  return cells_[static_cast<std::size_t>(y) * static_cast<std::size_t>(width_) + static_cast<std::size_t>(x)];
}

int Screen::put_text(const int x, const int y, const char32_t glyph, const Style style) {
  const char32_t c = shown(glyph);
  const int cells = cell_width(c);
  if (cells == 0) {
    // a combining mark on the glyph before it (the wide character whose half that is); a format character is dropped
    if (!combining(c) || !inside(x - 1, y)) return 0;
    Cell* before = &cell(x - 1, y);
    if (before->glyph == kWideTail && inside(x - 2, y)) before = &cell(x - 2, y);
    for (char32_t& mark : before->marks) {
      if (mark == 0) {
        mark = c;
        break;
      }
    }
    return 0;
  }
  if (cells == 2) {
    if (!inside(x + 1, y)) {  // no room for its right half: a space in its place
      put(x, y, U' ', style);
      return 1;
    }
    put(x, y, c, style);
    put(x + 1, y, kWideTail, style);
    return 2;
  }
  put(x, y, c, style);
  return 1;
}

Screen::Screen(const int width, const int height)
    : width_(std::max(1, width)), height_(std::max(1, height)), cells_(static_cast<std::size_t>(width_) * static_cast<std::size_t>(height_)) {}

const Cell& Screen::at(const int x, const int y) const {
  return cells_[static_cast<std::size_t>(std::clamp(y, 0, height_ - 1)) * static_cast<std::size_t>(width_) +
                static_cast<std::size_t>(std::clamp(x, 0, width_ - 1))];
}

void Screen::put(const int x, const int y, const char32_t glyph, const Style style) {
  if (!inside(x, y)) return;
  Cell& here = cell(x, y);
  // half of a wide character written over: its other half is a space (as the terminal shows it)
  if (here.glyph == kWideTail && inside(x - 1, y)) {
    Cell& head = cell(x - 1, y);
    head = Cell{U' ', head.style};
  } else if (inside(x + 1, y) && cell(x + 1, y).glyph == kWideTail) {
    Cell& tail = cell(x + 1, y);
    tail = Cell{U' ', tail.style};
  }
  here = Cell{glyph, style};
}

void Screen::clear(const Style style) {
  for (auto& cell : cells_) cell = Cell{U' ', style};
}

void Screen::fill(const Rect area, const char32_t glyph, const Style style) {
  for (int y = area.y; y < area.y + area.height; ++y) {
    for (int x = area.x; x < area.x + area.width; ++x) put(x, y, glyph, style);
  }
}

int Screen::text(const int x, const int y, const std::string_view text, const Style style, const int max_width) {
  const int limit = max_width < 0 ? width_ - x : max_width;
  int written = 0;
  for (const char32_t c : code_points(text)) {
    if (cell_width(c) == 0 && written == 0) continue;  // a mark with no glyph of this text before it
    if (written + cell_width(c) > limit) break;
    written += put_text(x + written, y, c, style);
  }
  return written;
}

void Screen::field(const int x, const int y, const int width, const std::string_view text, const Style style) {
  if (width <= 0) return;
  const bool cut = text_width(text) > width;
  int written = this->text(x, y, text, style, cut ? width - 1 : width);
  if (cut) put(x + written++, y, char32_t{0x2026}, style);
  for (; written < width; ++written) put(x + written, y, U' ', style);
}

void Screen::box(const Rect area, const std::string_view title, const Style border, const Style title_style) {
  if (area.width < 2 || area.height < 2) return;
  const int right = area.x + area.width - 1;
  const int bottom = area.y + area.height - 1;
  for (int x = area.x + 1; x < right; ++x) {
    put(x, area.y, char32_t{0x2500}, border);
    put(x, bottom, char32_t{0x2500}, border);
  }
  for (int y = area.y + 1; y < bottom; ++y) {
    put(area.x, y, char32_t{0x2502}, border);
    put(right, y, char32_t{0x2502}, border);
  }
  put(area.x, area.y, char32_t{0x256D}, border);
  put(right, area.y, char32_t{0x256E}, border);
  put(area.x, bottom, char32_t{0x2570}, border);
  put(right, bottom, char32_t{0x256F}, border);
  if (!title.empty() && area.width > 6) {
    put(area.x + 1, area.y, U' ', title_style);
    const int written = text(area.x + 2, area.y, title, title_style, area.width - 5);
    put(area.x + 2 + written, area.y, U' ', title_style);
  }
}

void Screen::hline(const int x, const int y, const int width, const Style style, const char32_t glyph) {
  for (int i = 0; i < width; ++i) put(x + i, y, glyph, style);
}

void Screen::gauge(const int x, const int y, const int width, const double fraction, const Style bar, const Style track) {
  if (width <= 0) return;
  const double clamped = std::isfinite(fraction) ? std::clamp(fraction, 0.0, 1.0) : 0.0;
  const int eighths = static_cast<int>(std::lround(clamped * width * 8.0));
  for (int i = 0; i < width; ++i) {
    const int here = std::clamp(eighths - i * 8, 0, 8);
    if (here == 8) put(x + i, y, char32_t{0x2588}, bar);
    else if (here > 0) put(x + i, y, kLeftEighths[here], Style{bar.fg, track.bg, bar.bold, bar.dim, false});
    else put(x + i, y, char32_t{0x00B7}, track);
  }
}

void Screen::chart(const Rect area, const std::span<const double> values, const double top, const Style style,
                   const Capabilities& capabilities) {
  if (area.empty()) return;
  const double scale = top > 0.0 && std::isfinite(top) ? top : 1.0;
  const auto level = [scale](const double v) { return std::isfinite(v) ? std::clamp(v / scale, 0.0, 1.0) : 0.0; };
  if (!capabilities.braille) {
    // a column of bottom-aligned blocks a value, the newest at the right
    const std::size_t shown = std::min(values.size(), static_cast<std::size_t>(area.width));
    for (std::size_t i = 0; i < shown; ++i) {
      const double v = level(values[values.size() - shown + i]);
      const int x = area.x + area.width - static_cast<int>(shown) + static_cast<int>(i);
      int eighths = static_cast<int>(std::lround(v * area.height * 8.0));
      for (int row = area.height - 1; row >= 0 && eighths > 0; --row) {
        put(x, area.y + row, kLowerEighths[std::min(eighths, 8)], style);
        eighths -= 8;
      }
    }
    return;
  }
  // a dot column a value (two a cell), each joined to the one before it
  const int dots_wide = area.width * 2;
  const int dots_high = area.height * 4;
  std::vector<std::uint8_t> bits(static_cast<std::size_t>(area.width) * static_cast<std::size_t>(area.height), 0U);
  const auto set = [&](const int dx, const int dy) {
    if (dx < 0 || dy < 0 || dx >= dots_wide || dy >= dots_high) return;
    auto& cell = bits[static_cast<std::size_t>(dy / 4) * static_cast<std::size_t>(area.width) + static_cast<std::size_t>(dx / 2)];
    cell = static_cast<std::uint8_t>(cell | kBrailleBit[dx % 2][dy % 4]);
  };
  const std::size_t shown = std::min(values.size(), static_cast<std::size_t>(dots_wide));
  int previous = -1;
  for (std::size_t i = 0; i < shown; ++i) {
    const int dx = dots_wide - static_cast<int>(shown) + static_cast<int>(i);
    const double value = values[values.size() - shown + i];
    if (!std::isfinite(value)) {  // an unknown sample: no dot, and the line does not join across it
      previous = -1;
      continue;
    }
    const int dy = static_cast<int>(std::lround((1.0 - level(value)) * (dots_high - 1)));
    const int from = previous < 0 ? dy : std::min(previous, dy);
    const int to = previous < 0 ? dy : std::max(previous, dy);
    for (int y = from; y <= to; ++y) set(dx, y);
    previous = dy;
  }
  for (int row = 0; row < area.height; ++row) {
    for (int column = 0; column < area.width; ++column) {
      const auto b = bits[static_cast<std::size_t>(row) * static_cast<std::size_t>(area.width) + static_cast<std::size_t>(column)];
      if (b != 0U) put(area.x + column, area.y + row, static_cast<char32_t>(0x2800U + b), style);
    }
  }
}

std::string render(const Screen& after, const Screen* before, const Capabilities& capabilities) {
  const bool full = before == nullptr || before->width() != after.width() || before->height() != after.height();
  std::string out;
  out.reserve(full ? static_cast<std::size_t>(after.width() * after.height()) * 4U : 1024U);
  if (full) out += "\x1b[0m\x1b[2J";
  std::optional<Style> current;
  for (int y = 0; y < after.height(); ++y) {
    int x = 0;
    while (x < after.width()) {
      if (!full && after.at(x, y) == before->at(x, y)) {
        ++x;
        continue;
      }
      // a run of changed cells: one move, then the cells with their styles (a wide character's right half is written
      // with it: a run that starts there starts at the character)
      if (after.at(x, y).glyph == kWideTail && x > 0) --x;
      out += move_to(x, y);
      bool first = true;
      while (x < after.width() && (full || first || !(after.at(x, y) == before->at(x, y)))) {
        first = false;
        const Cell& cell = after.at(x, y);
        ++x;
        if (!current || !(*current == cell.style)) {
          out += sgr(cell.style, capabilities);
          current = cell.style;
        }
        // (a right half reached on its own has no character before it - put_text never leaves one - so a space)
        out += utf8(cell.glyph == kWideTail ? U' ' : cell.glyph);
        for (const char32_t mark : cell.marks) {
          if (mark != 0) out += utf8(mark);
        }
        if (x < after.width() && after.at(x, y).glyph == kWideTail) ++x;  // the terminal moved past both its cells
      }
    }
  }
  if (!out.empty()) out += "\x1b[0m";
  return out;
}

}  // namespace secure_frontend::tui
