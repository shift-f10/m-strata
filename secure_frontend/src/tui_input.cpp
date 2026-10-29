#include "secure_frontend/tui_input.hpp"

#include <charconv>
#include <cstddef>
#include <utility>

namespace secure_frontend::tui {
namespace {

Key plain(const Key::Kind kind) { return Key{kind, {}, 0}; }

std::vector<int> numbers_of(const std::string_view params) {
  std::vector<int> out;
  std::size_t at = 0;
  while (at <= params.size()) {
    const std::size_t semicolon = params.find(';', at);
    const std::string_view part = params.substr(at, semicolon == std::string_view::npos ? std::string_view::npos : semicolon - at);
    int value = 0;
    if (!part.empty()) static_cast<void>(std::from_chars(part.data(), part.data() + part.size(), value));  // (0 if not a number)
    out.push_back(value);
    if (semicolon == std::string_view::npos) break;
    at = semicolon + 1U;
  }
  return out;
}

// A whole CSI sequence (ESC [ params final) as a key; nothing for one the TUI does not use.
std::vector<Key> csi_key(const std::string_view params, const char final) {
  if (!params.empty() && params.front() == '<') {
    // SGR mouse: ESC [ < button ; x ; y (M press, m release); 64 the wheel up, 65 down
    const auto values = numbers_of(params.substr(1U));
    if (final == 'M' && !values.empty()) {
      if ((values[0] & ~0x1C) == 64) return {plain(Key::Kind::wheel_up)};
      if ((values[0] & ~0x1C) == 65) return {plain(Key::Kind::wheel_down)};
    }
    return {};
  }
  switch (final) {
    case 'A': return {plain(Key::Kind::up)};
    case 'B': return {plain(Key::Kind::down)};
    case 'C': return {plain(Key::Kind::right)};
    case 'D': return {plain(Key::Kind::left)};
    case 'H': return {plain(Key::Kind::home)};
    case 'F': return {plain(Key::Kind::end)};
    case 'Z': return {plain(Key::Kind::back_tab)};
    case 'P': return {Key{Key::Kind::function, {}, 1}};
    case 'Q': return {Key{Key::Kind::function, {}, 2}};
    case 'R': return {Key{Key::Kind::function, {}, 3}};
    case 'S': return {Key{Key::Kind::function, {}, 4}};
    case '~': {
      const auto values = numbers_of(params);
      const int code = values.empty() ? 0 : values[0];
      switch (code) {
        case 1:
        case 7: return {plain(Key::Kind::home)};
        case 4:
        case 8: return {plain(Key::Kind::end)};
        case 3: return {plain(Key::Kind::del)};
        case 5: return {plain(Key::Kind::page_up)};
        case 6: return {plain(Key::Kind::page_down)};
        case 11: case 12: case 13: case 14: case 15: return {Key{Key::Kind::function, {}, code - 10}};
        case 17: case 18: case 19: case 20: case 21: return {Key{Key::Kind::function, {}, code - 11}};
        case 23: case 24: return {Key{Key::Kind::function, {}, code - 12}};
        default: return {};
      }
    }
    default: return {};
  }
}

}  // namespace

std::vector<Key> KeyDecoder::feed(const std::string_view bytes) {
  held_.append(bytes);
  std::vector<Key> keys;
  std::size_t at = 0;
  while (at < held_.size()) {
    const auto c = static_cast<unsigned char>(held_[at]);
    if (c == 0x1BU) {
      if (at + 1U >= held_.size()) break;  // Escape, or the start of a sequence: wait (flush decides)
      const char next = held_[at + 1U];
      if (next == '[') {
        std::size_t end = at + 2U;
        while (end < held_.size() && !(static_cast<unsigned char>(held_[end]) >= 0x40U && static_cast<unsigned char>(held_[end]) <= 0x7EU)) {
          ++end;
        }
        if (end >= held_.size()) break;  // not whole yet
        for (auto& key : csi_key(std::string_view{held_}.substr(at + 2U, end - at - 2U), held_[end])) keys.push_back(std::move(key));
        at = end + 1U;
        continue;
      }
      if (next == 'O') {
        if (at + 2U >= held_.size()) break;
        for (auto& key : csi_key({}, held_[at + 2U])) keys.push_back(std::move(key));
        at += 3U;
        continue;
      }
      keys.push_back(plain(Key::Kind::escape));  // (Escape, then the key after it as itself)
      ++at;
      continue;
    }
    if (c == '\r' || c == '\n') {
      keys.push_back(plain(Key::Kind::enter));
      at += c == '\r' && at + 1U < held_.size() && held_[at + 1U] == '\n' ? 2U : 1U;
      continue;
    }
    if (c == '\t') {
      keys.push_back(plain(Key::Kind::tab));
      ++at;
      continue;
    }
    if (c == 0x7FU || c == 0x08U) {
      keys.push_back(plain(Key::Kind::backspace));
      ++at;
      continue;
    }
    if (c < 0x20U) {
      if (c >= 1U && c <= 26U) keys.push_back(Key{Key::Kind::ctrl, std::string(1U, static_cast<char>('a' + c - 1U)), 0});
      ++at;
      continue;
    }
    const std::size_t width = c < 0x80U ? 1U : (c >> 5U) == 0x6U ? 2U : (c >> 4U) == 0xEU ? 3U : (c >> 3U) == 0x1EU ? 4U : 1U;
    if (at + width > held_.size()) break;  // the rest of the character in the next read
    keys.push_back(Key{Key::Kind::character, held_.substr(at, width), 0});
    at += width;
  }
  held_.erase(0, at);
  return keys;
}

std::vector<Key> KeyDecoder::flush() {
  std::vector<Key> keys;
  if (!held_.empty() && held_.front() == '\x1b') keys.push_back(plain(Key::Kind::escape));
  held_.clear();
  return keys;
}

}  // namespace secure_frontend::tui
