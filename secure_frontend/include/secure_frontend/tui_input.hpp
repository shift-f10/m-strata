#pragma once

// strata-tui's keys (spec docs/superpowers/specs/2026-10-28-server-tui-design.md): the bytes a terminal sends - plain
// characters (UTF-8), control keys, and VT sequences for the arrows, Home/End, PageUp/PageDown, Delete, F1-F12,
// Shift-Tab and the mouse wheel (SGR mouse reports) - decoded into keys. A sequence split across reads is held until it
// is whole; a lone Escape is one key when nothing follows it (flush).

#include <string>
#include <string_view>
#include <vector>

namespace secure_frontend::tui {

struct Key final {
  enum class Kind {
    character,  // `text`: one code point as UTF-8
    enter,
    escape,
    tab,
    back_tab,
    backspace,
    del,
    up,
    down,
    left,
    right,
    home,
    end,
    page_up,
    page_down,
    function,  // `number`: 1-12
    ctrl,      // `text`: the letter (Ctrl-C is "c")
    wheel_up,
    wheel_down,
  };
  Kind kind{Kind::character};
  std::string text{};
  int number{0};
  friend bool operator==(const Key&, const Key&) = default;
};

class KeyDecoder final {
 public:
  // The keys `bytes` complete (an unfinished sequence is kept for the next call).
  [[nodiscard]] std::vector<Key> feed(std::string_view bytes);
  // What is held as keys of its own (a lone Escape after a pause, a sequence that never finished).
  [[nodiscard]] std::vector<Key> flush();
  [[nodiscard]] bool holding() const noexcept { return !held_.empty(); }

 private:
  std::string held_;
};

}  // namespace secure_frontend::tui
