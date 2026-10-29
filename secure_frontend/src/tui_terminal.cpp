#include "secure_frontend/tui_terminal.hpp"

#include <algorithm>
#include <cstdlib>
#include <string>

#if defined(_WIN32)
#include "secure_frontend/win32_prelude.hpp"
#else
#include <cerrno>
#include <poll.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace secure_frontend::tui {
namespace {

// The alternate screen, no cursor, no line wrap (a full last line must not scroll), SGR mouse reports for the wheel.
constexpr std::string_view kEnter = "\x1b[?1049h\x1b[?25l\x1b[?7l\x1b[?1000h\x1b[?1006h\x1b[2J";
constexpr std::string_view kLeave = "\x1b[?1006l\x1b[?1000l\x1b[?7h\x1b[0m\x1b[?25h\x1b[?1049l";

std::string environment(const char* name) {
#if defined(_WIN32)
  char buffer[256];
  const DWORD length = GetEnvironmentVariableA(name, buffer, sizeof(buffer));
  return length > 0U && length < sizeof(buffer) ? std::string{buffer, length} : std::string{};
#else
  const char* value = std::getenv(name);
  return value != nullptr ? std::string{value} : std::string{};
#endif
}

}  // namespace

Terminal::~Terminal() { close(); }

Capabilities Terminal::capabilities() const {
  const std::string term = environment("TERM");
  Capabilities out;
  out.color = environment("NO_COLOR").empty() && term != "dumb";
  out.braille = term != "linux" && term != "dumb";
  return out;
}

#if defined(_WIN32)

std::optional<std::string> Terminal::open() {
  if (open_) return std::nullopt;
  input_ = GetStdHandle(STD_INPUT_HANDLE);
  output_ = GetStdHandle(STD_OUTPUT_HANDLE);
  DWORD in_mode = 0;
  DWORD out_mode = 0;
  if (input_ == INVALID_HANDLE_VALUE || output_ == INVALID_HANDLE_VALUE || GetConsoleMode(input_, &in_mode) == FALSE ||
      GetConsoleMode(output_, &out_mode) == FALSE) {
    return std::string{"strata-tui needs a console (run it in a terminal window)"};
  }
  input_mode_ = in_mode;
  output_mode_ = out_mode;
  output_code_page_ = GetConsoleOutputCP();
  input_code_page_ = GetConsoleCP();
  // keys as VT sequences, no echo, no line editing, Ctrl-C as a key; output with VT processing
  const DWORD raw_in = (in_mode & ~static_cast<DWORD>(ENABLE_ECHO_INPUT | ENABLE_LINE_INPUT | ENABLE_PROCESSED_INPUT | ENABLE_QUICK_EDIT_MODE |
                                                       ENABLE_MOUSE_INPUT)) |
                       ENABLE_VIRTUAL_TERMINAL_INPUT | ENABLE_WINDOW_INPUT | ENABLE_EXTENDED_FLAGS;
  const DWORD vt_out = out_mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING | DISABLE_NEWLINE_AUTO_RETURN;
  if (SetConsoleMode(input_, raw_in) == FALSE || SetConsoleMode(output_, vt_out) == FALSE) {
    (void)SetConsoleMode(input_, in_mode);
    (void)SetConsoleMode(output_, out_mode);
    return std::string{"this console does not take VT sequences (Windows 10 1809 or newer, or Windows Terminal)"};
  }
  (void)SetConsoleOutputCP(CP_UTF8);
  (void)SetConsoleCP(CP_UTF8);
  open_ = true;
  write(kEnter);
  return std::nullopt;
}

void Terminal::close() {
  if (!open_) return;
  write(kLeave);
  (void)SetConsoleMode(input_, input_mode_);
  (void)SetConsoleMode(output_, output_mode_);
  if (output_code_page_ != 0U) (void)SetConsoleOutputCP(output_code_page_);
  if (input_code_page_ != 0U) (void)SetConsoleCP(input_code_page_);
  open_ = false;
}

TerminalSize Terminal::size() const {
  CONSOLE_SCREEN_BUFFER_INFO info{};
  if (GetConsoleScreenBufferInfo(output_, &info) == FALSE) return {};
  return TerminalSize{info.srWindow.Right - info.srWindow.Left + 1, info.srWindow.Bottom - info.srWindow.Top + 1};
}

std::string Terminal::read(const std::chrono::milliseconds timeout) {
  std::string out;
  const DWORD waited = WaitForSingleObject(input_, static_cast<DWORD>(timeout.count()));
  if (waited == WAIT_FAILED) gone_ = true;  // (the console's input is no more)
  if (waited != WAIT_OBJECT_0) return out;
  INPUT_RECORD records[64];
  DWORD count = 0;
  if (ReadConsoleInputW(input_, records, 64, &count) == FALSE) {
    gone_ = true;
    return out;
  }
  std::wstring typed;
  for (DWORD i = 0; i < count; ++i) {
    if (records[i].EventType != KEY_EVENT || records[i].Event.KeyEvent.bKeyDown == FALSE) continue;
    const wchar_t c = records[i].Event.KeyEvent.uChar.UnicodeChar;
    for (WORD repeat = 0; c != 0 && repeat < std::max<WORD>(1, records[i].Event.KeyEvent.wRepeatCount); ++repeat) typed.push_back(c);
  }
  if (typed.empty()) return out;
  const int bytes = WideCharToMultiByte(CP_UTF8, 0, typed.data(), static_cast<int>(typed.size()), nullptr, 0, nullptr, nullptr);
  if (bytes <= 0) return out;
  out.resize(static_cast<std::size_t>(bytes));
  (void)WideCharToMultiByte(CP_UTF8, 0, typed.data(), static_cast<int>(typed.size()), out.data(), bytes, nullptr, nullptr);
  return out;
}

void Terminal::write(const std::string_view bytes) {
  std::size_t at = 0;
  while (at < bytes.size()) {
    DWORD written = 0;
    const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(bytes.size() - at, 1U << 20U));
    if (WriteFile(output_, bytes.data() + at, chunk, &written, nullptr) == FALSE || written == 0U) return;
    at += written;
  }
}

#else

struct Terminal::Saved final {
  termios mode{};
};

std::optional<std::string> Terminal::open() {
  if (open_) return std::nullopt;
  if (isatty(STDIN_FILENO) == 0 || isatty(STDOUT_FILENO) == 0) return std::string{"strata-tui needs a terminal (stdin and stdout)"};
  auto* saved = new Saved{};
  if (tcgetattr(STDIN_FILENO, &saved->mode) != 0) {
    delete saved;
    return std::string{"the terminal's mode could not be read"};
  }
  termios raw = saved->mode;
  raw.c_iflag &= static_cast<tcflag_t>(~(BRKINT | ICRNL | INPCK | ISTRIP | IXON));
  raw.c_oflag &= static_cast<tcflag_t>(~OPOST);
  raw.c_cflag |= CS8;
  raw.c_lflag &= static_cast<tcflag_t>(~(ECHO | ICANON | IEXTEN | ISIG));
  raw.c_cc[VMIN] = 0;
  raw.c_cc[VTIME] = 0;
  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) != 0) {
    delete saved;
    return std::string{"the terminal could not be put in raw mode"};
  }
  saved_ = saved;
  open_ = true;
  write(kEnter);
  return std::nullopt;
}

void Terminal::close() {
  if (!open_) return;
  write(kLeave);
  if (saved_ != nullptr) {
    (void)tcsetattr(STDIN_FILENO, TCSAFLUSH, &saved_->mode);
    delete saved_;
    saved_ = nullptr;
  }
  open_ = false;
}

TerminalSize Terminal::size() const {
  winsize window{};
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &window) != 0 || window.ws_col == 0U || window.ws_row == 0U) return {};
  return TerminalSize{window.ws_col, window.ws_row};
}

std::string Terminal::read(const std::chrono::milliseconds timeout) {
  pollfd input{STDIN_FILENO, POLLIN, 0};
  const int ready = poll(&input, 1, static_cast<int>(timeout.count()));
  if (ready <= 0) return {};  // (none, or a signal: the loop looks at its flag)
  if ((input.revents & POLLIN) == 0) {
    // a hang-up or an error with nothing to read: the terminal went away (and poll would return at once from now on)
    if ((input.revents & (POLLHUP | POLLERR | POLLNVAL)) != 0) gone_ = true;
    return {};
  }
  char buffer[512];
  const ssize_t got = ::read(STDIN_FILENO, buffer, sizeof(buffer));
  if (got > 0) return std::string{buffer, static_cast<std::size_t>(got)};
  if (got == 0 || (errno != EINTR && errno != EAGAIN)) gone_ = true;  // the end of the input, or EIO after a hang-up
  return {};
}

void Terminal::write(const std::string_view bytes) {
  std::size_t at = 0;
  int waits = 0;
  while (at < bytes.size()) {
    const ssize_t written = ::write(STDOUT_FILENO, bytes.data() + at, bytes.size() - at);
    if (written > 0) {
      at += static_cast<std::size_t>(written);
      continue;
    }
    if (written < 0 && errno == EINTR) continue;
    if (written < 0 && errno == EAGAIN && waits++ < 50) {  // (a terminal someone set non-blocking: wait for room)
      pollfd output{STDOUT_FILENO, POLLOUT, 0};
      (void)poll(&output, 1, 100);
      continue;
    }
    return;  // the terminal went away: nothing more to write
  }
}

#endif

}  // namespace secure_frontend::tui
