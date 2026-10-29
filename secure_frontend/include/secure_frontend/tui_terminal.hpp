#pragma once

// strata-tui's terminal (spec docs/superpowers/specs/2026-10-28-server-tui-design.md, "The terminal half"): raw input,
// VT output in UTF-8 on the alternate screen, the size, and the terminal put back as it was on every way out. Windows:
// the console in VT mode; elsewhere: termios raw mode and TIOCGWINSZ. The size is read again at every turn of the
// program's loop (a resize is seen within its 50 ms), so no SIGWINCH handler is needed.

#include "secure_frontend/tui_screen.hpp"

#include <chrono>
#include <optional>
#include <string>
#include <string_view>

namespace secure_frontend::tui {

struct TerminalSize final {
  int width{80};
  int height{24};
  friend bool operator==(const TerminalSize&, const TerminalSize&) = default;
};

class Terminal final {
 public:
  Terminal() = default;
  ~Terminal();
  Terminal(const Terminal&) = delete;
  Terminal& operator=(const Terminal&) = delete;
  // Raw input, the alternate screen, the cursor hidden, the mouse wheel on; a message naming what failed (stdin or
  // stdout is not a terminal) when it cannot.
  [[nodiscard]] std::optional<std::string> open();
  // The terminal as it was (idempotent; the destructor does it too).
  void close();
  [[nodiscard]] TerminalSize size() const;
  // What the terminal can show (NO_COLOR and TERM=dumb turn colours off; TERM=linux, the Linux console, has no braille).
  [[nodiscard]] Capabilities capabilities() const;
  // The bytes typed within `timeout` (empty when none).
  [[nodiscard]] std::string read(std::chrono::milliseconds timeout);
  void write(std::string_view bytes);
  // The terminal went away (its input ended: a hang-up, a console that closed): the program quits.
  [[nodiscard]] bool gone() const noexcept { return gone_; }

 private:
  bool open_{false};
  bool gone_{false};
#if defined(_WIN32)
  void* input_{nullptr};
  void* output_{nullptr};
  unsigned long input_mode_{0};
  unsigned long output_mode_{0};
  unsigned output_code_page_{0};
  unsigned input_code_page_{0};
#else
  struct Saved;
  Saved* saved_{nullptr};
#endif
};

}  // namespace secure_frontend::tui
