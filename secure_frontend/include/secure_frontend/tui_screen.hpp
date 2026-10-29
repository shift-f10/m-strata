#pragma once

// strata-tui's screen (spec docs/superpowers/specs/2026-10-28-server-tui-design.md; the owner: "make the TUI look
// professional and make monitoring the default view in a nice graph dashboard"): a grid of cells the views draw into -
// text, boxes, gauges and braille charts - and the VT output that turns one frame into the next. No terminal here: the
// terminal half writes what this renders.

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace secure_frontend::tui {

// A colour of the 256-colour palette, or the terminal's own.
inline constexpr int kDefault = -1;
struct Style final {
  int fg{kDefault};
  int bg{kDefault};
  bool bold{false};
  bool dim{false};
  bool reverse{false};
  friend bool operator==(const Style&, const Style&) = default;
};

// The right half of a wide character (CJK, emoji): the cell to its left holds the character and draws both.
inline constexpr char32_t kWideTail = char32_t{0};

struct Cell final {
  char32_t glyph{U' '};
  Style style{};
  // the combining marks drawn with the glyph (they take no cell of their own; a third one is dropped)
  std::array<char32_t, 2> marks{};
  friend bool operator==(const Cell&, const Cell&) = default;
};

struct Rect final {
  int x{0};
  int y{0};
  int width{0};
  int height{0};
  [[nodiscard]] bool empty() const noexcept { return width <= 0 || height <= 0; }
  // The rectangle inside a one-cell border.
  [[nodiscard]] Rect inner() const noexcept { return Rect{x + 1, y + 1, width - 2, height - 2}; }
};

// How a terminal shows a screen: colours or not (NO_COLOR, TERM=dumb), braille or blocks for the charts.
struct Capabilities final {
  bool color{true};
  bool braille{true};
};

class Screen final {
 public:
  Screen(int width, int height);
  [[nodiscard]] int width() const noexcept { return width_; }
  [[nodiscard]] int height() const noexcept { return height_; }
  [[nodiscard]] const Cell& at(int x, int y) const;
  // Every cell to `style`'s background, blank.
  void clear(Style style = {});
  void fill(Rect area, char32_t glyph, Style style);
  // UTF-8 text from (x, y), each code point in the cells a terminal gives it (cell_width), clipped at `max_width` cells
  // (or the screen); a control character (C0, DEL, C1) is shown as a space, so text from the server cannot carry a
  // terminal sequence. Returns the cells written.
  int text(int x, int y, std::string_view utf8, Style style, int max_width = -1);
  // Text cut to `width` cells with a trailing ellipsis (U+2026) when it does not fit, padded with spaces to the width.
  void field(int x, int y, int width, std::string_view utf8, Style style);
  // A rounded border with a title in its top edge.
  void box(Rect area, std::string_view title, Style border, Style title_style);
  void hline(int x, int y, int width, Style style, char32_t glyph = char32_t{0x2500});
  // A bar of `width` cells filled to `fraction` (0-1) with eighth blocks, the rest in `track`.
  void gauge(int x, int y, int width, double fraction, Style bar, Style track);
  // A line chart of `values` (the newest last) scaled to 0..`top`, right-aligned in `area`, in braille dots (2 x 4 a
  // cell) or, without braille, in block columns.
  void chart(Rect area, std::span<const double> values, double top, Style style, const Capabilities& capabilities);

 private:
  [[nodiscard]] bool inside(int x, int y) const noexcept { return x >= 0 && y >= 0 && x < width_ && y < height_; }
  [[nodiscard]] Cell& cell(int x, int y) noexcept;
  // One cell; writing over half of a wide character turns its other half into a space.
  void put(int x, int y, char32_t glyph, Style style);
  // One code point of text at (x, y) in its cells (a combining mark on the cell before it); the cells it took.
  int put_text(int x, int y, char32_t glyph, Style style);
  int width_;
  int height_;
  std::vector<Cell> cells_;
};

// UTF-8 of one code point.
[[nodiscard]] std::string utf8(char32_t glyph);
// The code points of UTF-8 text (an invalid byte is U+FFFD).
[[nodiscard]] std::u32string code_points(std::string_view utf8);
// The cells a terminal gives a code point: 2 for the East Asian wide and fullwidth ones and the emoji, 0 for a
// combining mark or a zero-width format character (ZWJ, a variation selector, a bidi control), 1 for the rest.
[[nodiscard]] int cell_width(char32_t glyph);
// The cells text takes (each code point's cell_width).
[[nodiscard]] int text_width(std::string_view utf8);

// The VT output that turns `before` into `after` (both the same size): only the cells that changed, each run with one
// cursor move and its SGR (a wide character written once for its two cells, a combining mark after its glyph). A first
// frame (or a resize) is `full`: the screen cleared and every cell written.
[[nodiscard]] std::string render(const Screen& after, const Screen* before, const Capabilities& capabilities);

}  // namespace secure_frontend::tui
