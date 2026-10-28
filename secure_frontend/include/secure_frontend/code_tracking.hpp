#pragma once

// The tool-call parsers' view of the text gone out (upstream's and Maya's OutputParser._track, Strata #1058): the open
// code fence, the current line and the current paragraph's backticks, to tell a call from a quoted one.

#include "secure_frontend/python_text.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>

namespace secure_frontend {

struct CodeTracking final {
  char fence{'\0'};  // the open fence's character ('`' or '~'); '\0': none
  std::string line;
  std::size_t ticks{0};

  void track(const std::string_view text) {
    std::size_t from = 0U;
    for (std::size_t newline = text.find('\n'); newline != std::string_view::npos; newline = text.find('\n', from)) {
      line.append(text.substr(from, newline - from));
      const std::string_view s = python_lstrip(line);
      if (fence != '\0') {
        if (s.substr(0U, 3U) == std::string(3U, fence)) fence = '\0';
      } else if ((s.substr(0U, 3U) == "```" || s.substr(0U, 3U) == "~~~") && s.substr(3U).find(std::string(3U, s.front())) == std::string_view::npos) {
        fence = s.front();
      } else if (s.empty()) {
        ticks = 0U;
      } else {
        ticks += static_cast<std::size_t>(std::count(line.begin(), line.end(), '`'));
      }
      line.clear();
      from = newline + 1U;
    }
    line.append(text.substr(from));
  }
  // The next character starts a line, outside a fence and inline code.
  [[nodiscard]] bool opener_ok() const { return fence == '\0' && python_strip(line).empty() && ticks % 2U == 0U; }
  // The next character is inside a fence or inline code.
  [[nodiscard]] bool in_code() const {
    return fence != '\0' || (ticks + static_cast<std::size_t>(std::count(line.begin(), line.end(), '`'))) % 2U == 1U;
  }
};

}  // namespace secure_frontend
