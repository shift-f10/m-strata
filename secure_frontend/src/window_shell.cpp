#include "secure_frontend/window_shell.hpp"

#include "secure_frontend/api_client.hpp"
#include "secure_frontend/config.hpp"
#include "secure_frontend/frontend_config.hpp"
#include "secure_frontend/json.hpp"
#include "secure_frontend/loopback_address.hpp"
#include "secure_frontend/model_cards.hpp"
#include "secure_frontend/server_cli.hpp"
#include "secure_frontend/token_reader.hpp"
#include "secure_frontend/utf8_stream_decoder.hpp"

#include <algorithm>
#include <cctype>
#include <condition_variable>
#include <cstddef>
#include <ctime>
#include <cwchar>
#include <mutex>
#include <system_error>
#include <thread>
#include <utility>

#if defined(_WIN32)
#include "secure_frontend/win32_prelude.hpp"

#include "win_child_launch.hpp"
#else
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/prctl.h>
#endif
extern char** environ;  // the process environment (current_environment)
#endif

namespace secure_frontend::window {
namespace {

constexpr std::string_view kSignInPrefix = "Sign in: open ";
constexpr std::string_view kListeningPrefix = "Secure server listening on http://";
constexpr std::size_t kMaxLineBytes = 16U * 1024U;
constexpr std::size_t kMaxAnswerBytes = 8U * 1024U * 1024U;

std::string lower(std::string_view text) {
  std::string out{text};
  std::transform(out.begin(), out.end(), out.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
}

bool all_digits(const std::string_view text) {
  return !text.empty() && std::all_of(text.begin(), text.end(), [](const unsigned char c) { return std::isdigit(c) != 0; });
}

// "host:port" split at the port's colon; an IPv6 host keeps its brackets. nullopt: not host:port with a 1-65535 port.
std::optional<std::pair<std::string_view, std::string_view>> split_authority(const std::string_view authority) {
  const auto colon = authority.rfind(':');
  if (colon == std::string_view::npos || colon == 0U) return std::nullopt;
  const auto host = authority.substr(0, colon);
  const auto port = authority.substr(colon + 1);
  if (!all_digits(port) || port.size() > 5U || std::stoul(std::string{port}) == 0U || std::stoul(std::string{port}) > 65535U) {
    return std::nullopt;
  }
  if (host.front() == '[') {
    if (host.back() != ']' || host.size() < 3U) return std::nullopt;
    const auto inner = host.substr(1, host.size() - 2);
    if (!std::all_of(inner.begin(), inner.end(), [](const unsigned char c) { return std::isxdigit(c) != 0 || c == ':' || c == '.'; })) {
      return std::nullopt;
    }
  } else if (!std::all_of(host.begin(), host.end(), [](const unsigned char c) { return std::isalnum(c) != 0 || c == '.' || c == '-'; })) {
    return std::nullopt;
  }
  return std::pair{host, port};
}

bool loopback_host(const std::string_view host) {
  const auto name = lower(host);
  return name == "127.0.0.1" || name == "localhost" || name == "[::1]";
}

std::string json_text(const std::string_view text) {
  std::string out;
  append_json_string(&out, text);
  return out;
}

const JsonValue* member(const JsonValue& object, const std::string_view key) {
  const auto* members = object.if_object();
  if (members == nullptr) return nullptr;
  for (const auto& entry : *members) {
    if (entry.key == key) return &entry.value;
  }
  return nullptr;
}

// The origin's host (brackets off) and port, as a loopback address to connect to.
std::optional<std::pair<LoopbackAddress, std::uint16_t>> loopback_target(const std::string_view origin) {
  if (!origin.starts_with("http://")) return std::nullopt;
  const auto parts = split_authority(origin.substr(7));
  if (!parts) return std::nullopt;
  std::string_view host = parts->first;
  if (host.front() == '[') host = host.substr(1, host.size() - 2);
  const auto address = parse_listener_address(host);
  if (!address) return std::nullopt;
  auto loopback = LoopbackAddress::from(*address);
  if (!loopback) return std::nullopt;
  return std::pair{std::move(*loopback), static_cast<std::uint16_t>(std::stoul(std::string{parts->second}))};
}

#if defined(_WIN32)
constexpr std::string_view kServerName = "strata-unified-server.exe";
#else
constexpr std::string_view kServerName = "strata-unified-server";
#endif

std::variant<ServerInvocation, std::string> parse_arguments(const std::vector<std::string>& arguments) {
  std::vector<const char*> argv{"strata-unified-server"};
  for (const auto& argument : arguments) argv.push_back(argument.c_str());
  argv.push_back(nullptr);
  auto parsed = parse_server_command_line(static_cast<int>(argv.size() - 1U), argv.data());
  if (const auto* error = std::get_if<CliError>(&parsed)) return "the server's options: " + error->message;
  auto invocation = std::get<ServerInvocation>(std::move(parsed));
  if (invocation.command != ServerCommand::serve) {
    return std::string{"this program runs the server; use strata-unified-server itself for init, --check and --help"};
  }
  return invocation;
}

// Bytes in, lines out (without "\r\n"), each at most kMaxLineBytes (a longer one is cut there).
class LineSplitter final {
 public:
  explicit LineSplitter(ServerChild::LineHandler& on_line) : on_line_(on_line) {}
  void feed(const char* data, const std::size_t size) {
    for (std::size_t i = 0; i < size; ++i) {
      if (data[i] == '\n') {
        emit();
      } else {
        pending_ += data[i];
        if (pending_.size() >= kMaxLineBytes) emit();
      }
    }
  }
  void finish() {
    if (!pending_.empty()) emit();
  }

 private:
  void emit() {
    if (!pending_.empty() && pending_.back() == '\r') pending_.pop_back();
    if (on_line_) on_line_(std::move(pending_));
    pending_.clear();
  }
  ServerChild::LineHandler& on_line_;
  std::string pending_;
};

// A line as the window's Log keeps it: valid UTF-8 (a broken sequence becomes U+FFFD), its control characters spaces
// (a tab stays), at most `max_bytes` long and never cut inside a character.
std::string shown_line(const std::string_view line, const std::size_t max_bytes) {
  Utf8StreamDecoder decoder;
  std::string out = decoder.push(line);
  out += decoder.finish(false);  // an incomplete sequence at the end: one U+FFFD
  for (char& c : out) {
    const auto byte = static_cast<unsigned char>(c);
    if ((byte < 0x20U && c != '\t') || byte == 0x7FU) c = ' ';
  }
  if (out.size() > max_bytes) {
    std::size_t cut = max_bytes;
    while (cut > 0U && (static_cast<unsigned char>(out[cut]) & 0xC0U) == 0x80U) --cut;
    out.resize(cut);
  }
  return out;
}

}  // namespace

// --------------------------------------------------------------------------- the server's console
std::optional<std::string> sign_in_url(const std::string_view line) {
  if (!line.starts_with(kSignInPrefix)) return std::nullopt;
  auto url = line.substr(kSignInPrefix.size());
  url = url.substr(0, url.find(' '));
  if (!url.starts_with("http://")) return std::nullopt;
  const auto rest = url.substr(7);
  const auto slash = rest.find('/');
  if (slash == std::string_view::npos) return std::nullopt;
  const auto parts = split_authority(rest.substr(0, slash));
  if (!parts || !loopback_host(parts->first)) return std::nullopt;
  const auto tail = rest.substr(slash);
  constexpr std::string_view kFragment = "/#login=";
  if (!tail.starts_with(kFragment)) return std::nullopt;
  // the page's rule for a code (app.js): 16 to 128 of A-Z a-z 0-9 _ - (LoginLinks issues 43, base64url)
  const auto code = tail.substr(kFragment.size());
  if (code.size() < 16U || code.size() > 128U ||
      !std::all_of(code.begin(), code.end(), [](const unsigned char c) { return std::isalnum(c) != 0 || c == '-' || c == '_'; })) {
    return std::nullopt;
  }
  return std::string{url};
}

std::optional<std::string> listening_origin(const std::string_view line) {
  if (!line.starts_with(kListeningPrefix)) return std::nullopt;
  auto authority = line.substr(kListeningPrefix.size());
  authority = authority.substr(0, authority.find(' '));
  const auto parts = split_authority(authority);
  if (!parts) return std::nullopt;
  std::string host{parts->first};
  if (host == "0.0.0.0") host = "127.0.0.1";
  if (host == "[::]") host = "[::1]";
  return "http://" + host + ":" + std::string{parts->second};
}

bool loggable(const std::string_view line) { return line.find("#login=") == std::string_view::npos; }

std::string page_address(const std::optional<std::string>& sign_in, const std::string_view origin, const bool first) {
  if (sign_in) return *sign_in + (first ? "&view=monitor" : "");
  return std::string{origin} + (first ? "/#view=monitor" : "/");
}

// --------------------------------------------------------------------------- the web view
Navigation navigation_for(const std::string_view url, const std::string_view origin) {
  const auto scheme_end = url.find("://");
  if (scheme_end == std::string_view::npos) return Navigation::refuse;
  const auto scheme = lower(url.substr(0, scheme_end));
  if (scheme != "http" && scheme != "https") return Navigation::refuse;
  const auto rest = url.substr(scheme_end + 3);
  const auto authority = rest.substr(0, rest.find_first_of("/?#\\"));
  if (authority.empty() || authority.find('@') != std::string_view::npos) return Navigation::refuse;
  return lower(scheme) + "://" + lower(authority) == lower(origin) ? Navigation::stay : Navigation::browser;
}

// --------------------------------------------------------------------------- the tray's models
std::optional<TrayModels> tray_models(const std::string_view launcher_json) {
  // fractions: a card's sampling options are decimals (top_p 0.95)
  constexpr JsonLimits kLimits{.max_bytes = 4U * 1024U * 1024U, .max_depth = 12U, .max_container_entries = 4096U,
                               .max_string_bytes = 64U * 1024U, .allow_fractions = true};
  const auto parsed = parse_json(launcher_json, kLimits);
  const auto* root = std::get_if<JsonValue>(&parsed);
  if (root == nullptr || root->if_object() == nullptr) return std::nullopt;
  const auto* data = member(*root, "data");
  const auto* state = member(*root, "state");
  const auto* running = member(*root, "running");
  if (data == nullptr || data->if_array() == nullptr || state == nullptr || state->if_string() == nullptr || running == nullptr ||
      (running->if_string() == nullptr && running->kind() != JsonKind::null)) {
    return std::nullopt;
  }
  TrayModels models{.models = {}, .running = {}, .state = *state->if_string()};
  if (const auto* name = running->if_string()) models.running = *name;
  for (const auto& card : *data->if_array()) {
    const auto* name = member(card, "name");
    const auto* card_state = member(card, "state");
    if (name == nullptr || name->if_string() == nullptr || card_state == nullptr || card_state->if_string() == nullptr) return std::nullopt;
    models.models.push_back({.name = *name->if_string(), .state = *card_state->if_string()});
  }
  return models;
}

std::string start_body(const std::string_view card) { return "{\"name\":" + json_text(card) + "}"; }

// --------------------------------------------------------------------------- requests to the server
std::string request_text(const std::string_view method, const std::string_view path, const std::string_view authority,
                         const std::string* key, const std::string_view json_body) {
  std::string out = std::string{method} + " " + std::string{path} + " HTTP/1.1\r\nHost: " + std::string{authority} + "\r\n";
  if (key != nullptr) out += "Authorization: Bearer " + *key + "\r\n";
  if (!json_body.empty()) out += "Content-Type: application/json\r\n";
  if (method != "GET") out += "Content-Length: " + std::to_string(json_body.size()) + "\r\n";
  out += "Connection: close\r\n\r\n";
  out += json_body;
  return out;
}

std::optional<Answer> parse_answer(const std::string_view raw) {
  const auto head_end = raw.find("\r\n\r\n");
  if (head_end == std::string_view::npos) return std::nullopt;
  const auto head = parse_response_head(raw.substr(0, head_end + 4));
  if (!head || head->header("Transfer-Encoding").has_value()) return std::nullopt;
  const auto length = head->header("Content-Length");
  if (!length || !all_digits(*length) || length->size() > 9U) return std::nullopt;
  const std::size_t size = std::stoul(std::string{*length});
  const auto body = raw.substr(head_end + 4);
  if (body.size() < size) return std::nullopt;
  return Answer{.status = head->status, .body = std::string{body.substr(0, size)}};
}

std::optional<Answer> loopback_request(const std::string_view origin, const std::string_view method, const std::string_view path,
                                       const std::string* key, const std::string_view json_body,
                                       const std::chrono::milliseconds timeout) {
  const auto target = loopback_target(origin);
  if (!target) return std::nullopt;
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  const auto left = [deadline] {
    return std::max(std::chrono::milliseconds{1},
                    std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()));
  };
  auto connection = LoopbackStreamConnection::open(target->first, target->second, timeout);
  if (!connection) return std::nullopt;
  if (!connection->send_all(request_text(method, path, origin.substr(7), key, json_body), left())) return std::nullopt;
  std::string raw;
  while (std::chrono::steady_clock::now() < deadline && raw.size() < kMaxAnswerBytes) {
    if (auto answer = parse_answer(raw)) return answer;
    const auto some = connection->receive_some(left());
    if (!some) return std::nullopt;
    if (some->empty() && connection->eof()) break;
    raw += *some;
  }
  return parse_answer(raw);
}

// --------------------------------------------------------------------------- what the window starts
std::variant<ServerSetup, std::string> server_setup(const std::vector<std::string>& arguments, const std::filesystem::path& install_dir) {
  auto parsed = parse_arguments(arguments);
  if (auto* message = std::get_if<std::string>(&parsed)) return std::move(*message);
  const auto& invocation = std::get<ServerInvocation>(parsed);
  ServerSetup setup{.server = install_dir / kServerName, .arguments = arguments, .data = std::nullopt,
                    .environment = current_environment()};
  if (uses_data_folder(invocation)) setup.data = data_folder_layout(invocation.data_dir.value_or(install_dir / "data"));
  return setup;
}

std::optional<ServerKey> server_key(const ServerSetup& setup) {
  auto parsed = parse_arguments(setup.arguments);
  auto* invocation = std::get_if<ServerInvocation>(&parsed);
  if (invocation == nullptr) return std::nullopt;
  if (setup.data) invocation->config_file = setup.data->config_file;
  const auto draft = load_server_draft(*invocation, nullptr);
  const auto* main = std::get_if<FrontendConfigDraft>(&draft);
  if (main == nullptr) return std::nullopt;
  // the server's own rule: a cards config by the card-less rules, any other by validate()
  const auto checked = main->entries.contains("models.cards") ? card_less_server_config(*main) : validate(*main);
  const auto* validated = std::get_if<ValidatedConfig>(&checked);
  if (validated == nullptr) return std::nullopt;
  const FrontendConfig& config = validated->get();
  if (config.listener.loopback_without_key) return ServerKey{.key = std::nullopt, .keyless = true};
  const auto read = config.roots.secret.has_value() ? read_token(config.token, *config.roots.secret)
                                                    : read_token(config.token, std::filesystem::path{"."});
  const auto* key = std::get_if<SecretBytes>(&read);
  if (key == nullptr) return std::nullopt;
  return ServerKey{.key = std::string{key->view()}, .keyless = false};
}

std::vector<std::string> current_environment() {
  std::vector<std::string> entries;
#if defined(_WIN32)
  if (wchar_t* block = GetEnvironmentStringsW(); block != nullptr) {
    for (const wchar_t* entry = block; *entry != L'\0'; entry += std::wcslen(entry) + 1) {
      const int size = static_cast<int>(std::wcslen(entry));
      const int length = WideCharToMultiByte(CP_UTF8, 0, entry, size, nullptr, 0, nullptr, nullptr);
      std::string text(static_cast<std::size_t>(std::max(length, 0)), '\0');
      if (length > 0) (void)WideCharToMultiByte(CP_UTF8, 0, entry, size, text.data(), length, nullptr, nullptr);
      entries.push_back(std::move(text));
    }
    (void)FreeEnvironmentStringsW(block);
  }
#else
  for (char** entry = environ; entry != nullptr && *entry != nullptr; ++entry) entries.emplace_back(*entry);
#endif
  return entries;
}

// --------------------------------------------------------------------------- the server's log
RotatingLog::RotatingLog(std::filesystem::path file, const std::uintmax_t max_bytes) : file_(std::move(file)), max_bytes_(max_bytes) {
  open();
}

void RotatingLog::open() {
#if !defined(_WIN32)
  // owner-only from its creation (the data folder's logs folder is private already)
  const int fd = ::open(file_.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
  if (fd >= 0) {
    (void)::fchmod(fd, 0600);
    (void)::close(fd);
  }
#endif
  std::error_code error;
  const auto size = std::filesystem::file_size(file_, error);
  size_ = error ? 0U : size;
  out_.open(file_, std::ios::binary | std::ios::app);
}

void RotatingLog::write(const std::string_view line) {
  const std::uintmax_t bytes = line.size() + 1U;
  if (size_ > 0U && size_ + bytes > max_bytes_) {
    out_.close();
    std::error_code error;
    auto older = file_;  // (a path joined, never through string(): a folder name the code page cannot spell)
    older += ".1";
    std::filesystem::rename(file_, older, error);  // replaces the older one
    if (error) std::filesystem::remove(file_, error);
    open();
  }
  if (!out_) return;
  out_.write(line.data(), static_cast<std::streamsize>(line.size()));
  out_.put('\n');
  out_.flush();
  size_ += bytes;
}

// --------------------------------------------------------------------------- the window's Log
LogLines::LogLines(const std::size_t max_lines, const std::size_t max_bytes)
    : max_lines_(std::max<std::size_t>(max_lines, 1U)), max_bytes_(std::max<std::size_t>(max_bytes, 1U)) {}

void LogLines::add(const std::string_view line) {
  if (!loggable(line)) return;
  lines_.push_back(shown_line(line, max_bytes_));
  bytes_ += lines_.back().size();
  if (lines_.size() <= max_lines_ && bytes_ <= max_bytes_) return;
  // full: the oldest tenth goes at once (so a view shows every line again only now and then); the newest stays
  const std::size_t keep_lines = max_lines_ - max_lines_ / 10U;
  const std::size_t keep_bytes = max_bytes_ - max_bytes_ / 10U;
  while (lines_.size() > 1U && (lines_.size() > keep_lines || bytes_ > keep_bytes)) {
    bytes_ -= lines_.front().size();
    lines_.pop_front();
    ++first_;
  }
}

std::optional<std::size_t> LogLines::newer_than(const std::uint64_t shown_first, const std::uint64_t shown_end) const {
  if (shown_first != first_ || shown_end < first_ || shown_end > end()) return std::nullopt;
  return static_cast<std::size_t>(shown_end - first_);
}

std::string LogLines::text(const std::string_view newline) const {
  std::string out;
  out.reserve(bytes_ + lines_.size() * newline.size());
  for (const auto& line : lines_) {
    out += line;
    out += newline;
  }
  return out;
}

std::vector<std::string> LogLines::last(const std::size_t count) const {
  const std::size_t from = lines_.size() > count ? lines_.size() - count : 0U;
  return {lines_.begin() + static_cast<std::ptrdiff_t>(from), lines_.end()};
}

std::optional<std::string> LogLines::save(const std::filesystem::path& file) const {
#if !defined(_WIN32)
  // a new file owner-only from its creation, as server.log (one the user chose to replace keeps its own mode)
  const int fd = ::open(file.c_str(), O_WRONLY | O_CREAT | O_CLOEXEC, 0600);
  if (fd >= 0) (void)::close(fd);
#endif
  std::ofstream out(file, std::ios::binary | std::ios::trunc);
  const std::string all = text();
  out.write(all.data(), static_cast<std::streamsize>(all.size()));
  out.flush();
  if (!out) return std::string{"the log could not be written there"};
  return std::nullopt;
}

std::string saved_log_name(const std::tm& when) {
  char name[64] = {};
  if (std::strftime(name, sizeof name, "m-strata-log-%Y-%m-%d-%H%M%S.txt", &when) == 0U) return "m-strata-log.txt";
  return name;
}

// --------------------------------------------------------------------------- the server child
// One reader thread per start owns the child's output and its reaping: it reads lines until the output ends (or the
// child has ended and its last output is read), reaps the child, then reports the exit code. stop() asks and waits for
// that report, and kills the child when it does not come within the grace period.
struct ServerChild::State final {
  std::mutex stopping;  // one stop at a time (the window's Exit and the system's end of session may overlap)
  mutable std::mutex mutex;
  std::condition_variable reaped_changed;
  bool started{false};
  bool reaped{false};
  int exit_code{0};
  LineHandler on_line;
  ExitHandler on_exit;
  std::thread reader;
#if defined(_WIN32)
  HANDLE process{nullptr};
  HANDLE job{nullptr};
  HANDLE output{nullptr};
  DWORD pid{0};
#else
  pid_t pid{-1};
  int output{-1};
  bool gone{false};  // reaped: its pid is no longer the child's (under `mutex`)
#endif

  void report(const int code) {
    {
      std::lock_guard lock(mutex);
      reaped = true;
      exit_code = code;
    }
    reaped_changed.notify_all();
    if (on_exit) on_exit(code);
  }

  void read_until_exit() {
    LineSplitter lines(on_line);
    char buffer[4096];
    std::optional<std::chrono::steady_clock::time_point> drain_until;
#if defined(_WIN32)
    for (;;) {
      DWORD available = 0;
      if (!PeekNamedPipe(output, nullptr, 0, nullptr, &available, nullptr)) break;  // every writer closed: the end
      if (available > 0) {
        DWORD read = 0;
        if (!ReadFile(output, buffer, std::min(available, static_cast<DWORD>(sizeof buffer)), &read, nullptr) || read == 0) break;
        lines.feed(buffer, read);
        continue;
      }
      if (!drain_until && WaitForSingleObject(process, 50) == WAIT_OBJECT_0) {
        drain_until = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
      }
      if (drain_until) {
        if (std::chrono::steady_clock::now() >= *drain_until) break;
        Sleep(20);
      }
    }
    lines.finish();
    (void)WaitForSingleObject(process, INFINITE);
    DWORD code = 0;
    (void)GetExitCodeProcess(process, &code);
    report(static_cast<int>(code));
#else
    int status = 0;
    for (;;) {
      pollfd entry{.fd = output, .events = POLLIN, .revents = 0};
      const int ready = ::poll(&entry, 1, 50);
      if (ready > 0) {
        const ssize_t read = ::read(output, buffer, sizeof buffer);
        if (read > 0) {
          lines.feed(buffer, static_cast<std::size_t>(read));
          continue;
        }
        if (read == 0 || (errno != EINTR && errno != EAGAIN)) break;  // the end of every writer's output
      }
      if (!drain_until && reap(status)) drain_until = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
      if (drain_until && std::chrono::steady_clock::now() >= *drain_until) break;
    }
    lines.finish();
    while (!reap(status)) std::this_thread::sleep_for(std::chrono::milliseconds(20));
    report(WIFEXITED(status) ? WEXITSTATUS(status) : 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0));
#endif
  }

#if !defined(_WIN32)
  // Reaped under the lock, so stop() never signals a pid that is no longer the child's.
  bool reap(int& status) {
    std::lock_guard lock(mutex);
    if (gone) return true;
    if (::waitpid(pid, &status, WNOHANG) != pid) return false;
    gone = true;
    return true;
  }
#endif

  // The request the server's own handler answers with an orderly stop (shutdown_signal.cpp).
  void ask_to_stop() {
#if defined(_WIN32)
    // The child has a console of its own (no window): Ctrl-Break reaches it only from that console, so this process
    // attaches to it for the one call and goes back to the console it had (none for the window app).
    DWORD attached[1];
    const bool had_console = GetConsoleProcessList(attached, 1) != 0;
    if (had_console) (void)FreeConsole();
    if (AttachConsole(pid)) {
      (void)SetConsoleCtrlHandler(nullptr, TRUE);
      (void)GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, pid);  // its group only (CREATE_NEW_PROCESS_GROUP)
      (void)FreeConsole();
      (void)SetConsoleCtrlHandler(nullptr, FALSE);
    }
    if (had_console) (void)AttachConsole(ATTACH_PARENT_PROCESS);
#else
    if (!gone) (void)::kill(pid, SIGTERM);  // called under `mutex`
#endif
  }

  void kill() {
#if defined(_WIN32)
    (void)TerminateJobObject(job, 1);  // the server and every engine it runs
#else
    if (!gone) (void)::kill(pid, SIGKILL);  // called under `mutex`
#endif
  }

  void close_handles() {
#if defined(_WIN32)
    for (HANDLE* handle : {&process, &job, &output}) {
      if (*handle != nullptr) CloseHandle(*handle);
      *handle = nullptr;
    }
#else
    if (output >= 0) (void)::close(output);
    output = -1;
#endif
  }
};

ServerChild::ServerChild() : state_(std::make_unique<State>()) {}

ServerChild::~ServerChild() {
  if (state_->started) (void)stop(std::chrono::seconds(5));
}

bool ServerChild::running() const {
  std::lock_guard lock(state_->mutex);
  return state_->started && !state_->reaped;
}

std::optional<std::string> ServerChild::start(const std::filesystem::path& executable, const std::vector<std::string>& arguments,
                                              const std::vector<std::string>& environment, LineHandler on_line,
                                              ExitHandler on_exit) {
  if (state_->started) (void)stop(std::chrono::seconds(5));
  state_ = std::make_unique<State>();
  state_->on_line = std::move(on_line);
  state_->on_exit = std::move(on_exit);
  const std::string refused = "the server could not be started (its program is not beside strata-window, or cannot run)";
#if defined(_WIN32)
  const auto wide = [](const std::string& text) -> std::optional<std::wstring> {
    if (text.empty()) return std::wstring{};
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (length <= 0) return std::nullopt;
    std::wstring out(static_cast<std::size_t>(length), L'\0');
    (void)MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), out.data(), length);
    return out;
  };
  const auto quote = [](const std::wstring_view argument) {  // engine_process.cpp's rule
    std::wstring quoted{L"\""};
    std::size_t slashes = 0;
    for (const wchar_t character : argument) {
      if (character == L'\\') {
        ++slashes;
      } else if (character == L'\"') {
        quoted.append(slashes * 2 + 1, L'\\');
        quoted += character;
        slashes = 0;
      } else {
        quoted.append(slashes, L'\\');
        quoted += character;
        slashes = 0;
      }
    }
    quoted.append(slashes * 2, L'\\');
    return quoted + L"\"";
  };
  std::error_code error;
  const auto program = std::filesystem::absolute(executable, error);
  if (error || !std::filesystem::is_regular_file(program, error)) return refused;
  std::wstring command_line = quote(program.wstring());
  for (const auto& argument : arguments) {
    const auto converted = wide(argument);
    if (!converted) return std::string{"an argument is not valid UTF-8"};
    command_line += L' ' + quote(*converted);
  }
  // the environment given (the window's when it started), NAME=VALUE\0...\0
  std::wstring environment_block;
  for (const auto& entry : environment) {
    const auto converted = wide(entry);
    if (!converted) return std::string{"an environment variable is not valid UTF-8"};
    environment_block += *converted;
    environment_block += L'\0';
  }
  environment_block += L'\0';
  SECURITY_ATTRIBUTES inheritable{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
  HANDLE read_end = nullptr;
  HANDLE write_end = nullptr;
  if (!CreatePipe(&read_end, &write_end, &inheritable, 0)) return refused;
  (void)SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);
  HANDLE error_end = nullptr;  // a second handle to the pipe: the handle list takes each handle once
  const HANDLE self = GetCurrentProcess();
  if (!DuplicateHandle(self, write_end, self, &error_end, 0, TRUE, DUPLICATE_SAME_ACCESS)) error_end = nullptr;
  HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable, OPEN_EXISTING, 0, nullptr);
  const auto close_ours = [&] {
    for (HANDLE handle : {write_end, error_end, nul}) {
      if (handle != nullptr && handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
    }
  };
  if (error_end == nullptr || nul == INVALID_HANDLE_VALUE) {
    close_ours();
    CloseHandle(read_end);
    return refused;
  }
  win_launch::WinLaunchSpec spec{.executable = program.wstring(), .command_line = command_line, .environment_block = environment_block,
                                 .cwd = {}, .stdin_read = nul, .stdout_write = write_end, .stderr_handle = error_end,
                                 .primary_token = nullptr, .extra_inherited_handles = {}, .crt_fd_block = {},
                                 .limit_active_process_to_one = false, .restrict_ui = false};
  auto launched = win_launch::launch_in_job(spec);
  close_ours();  // the child holds its own copies
  if (!std::holds_alternative<win_launch::WinLaunched>(launched)) {
    CloseHandle(read_end);
    return refused;
  }
  const auto& child = std::get<win_launch::WinLaunched>(launched);
  CloseHandle(child.thread);
  state_->process = child.process;
  state_->job = child.job;
  state_->output = read_end;
  state_->pid = GetProcessId(child.process);
#else
  const std::string program = executable.string();
  if (::access(program.c_str(), X_OK) != 0) return refused;
  std::vector<std::string> storage{program};
  storage.insert(storage.end(), arguments.begin(), arguments.end());
  std::vector<char*> argv;
  for (auto& argument : storage) argv.push_back(argument.data());
  argv.push_back(nullptr);
  std::vector<std::string> variables{environment};  // the environment given (the window's when it started)
  std::vector<char*> envp;
  for (auto& variable : variables) envp.push_back(variable.data());
  envp.push_back(nullptr);
  int pipe_ends[2];
  if (::pipe2(pipe_ends, O_CLOEXEC) != 0) return refused;
  const int null_input = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
  if (null_input < 0) {
    (void)::close(pipe_ends[0]);
    (void)::close(pipe_ends[1]);
    return refused;
  }
  const pid_t parent = ::getpid();
  // Started from the window's main (UI) thread: the parent-death signal follows the thread that forks.
  const pid_t pid = ::fork();
  if (pid == 0) {
    // async-signal-safe calls only until exec
    sigset_t none;
    sigemptyset(&none);
    (void)::sigprocmask(SIG_SETMASK, &none, nullptr);
    if (::dup2(null_input, 0) < 0 || ::dup2(pipe_ends[1], 1) < 0 || ::dup2(pipe_ends[1], 2) < 0) ::_exit(127);
    // its own process group: a terminal's hangup (SSH gone, the terminal closed), Ctrl-C or Ctrl-Z reaches the window or
    // the TUI, which stops the server in order, never the server itself (or the parent-death signal below does)
    (void)::setpgid(0, 0);
#if defined(__linux__)
    (void)::prctl(PR_SET_PDEATHSIG, SIGTERM);  // the server stops in order when the window ends
    if (::getppid() != parent) ::_exit(127);
#endif
    ::execve(argv[0], argv.data(), envp.data());
    ::_exit(127);
  }
  (void)parent;
  (void)::close(pipe_ends[1]);
  (void)::close(null_input);
  if (pid < 0) {
    (void)::close(pipe_ends[0]);
    return refused;
  }
  state_->pid = pid;
  state_->output = pipe_ends[0];
#endif
  state_->started = true;
  State* state = state_.get();
  state_->reader = std::thread([state] { state->read_until_exit(); });
  return std::nullopt;
}

bool ServerChild::stop(const std::chrono::milliseconds grace) {
  State& state = *state_;
  const std::lock_guard one_stop(state.stopping);
  if (!state.started) return true;
  bool orderly = true;
  {
    std::unique_lock lock(state.mutex);
    if (!state.reaped) {
      state.ask_to_stop();
      if (!state.reaped_changed.wait_for(lock, grace, [&state] { return state.reaped; })) {
        orderly = false;
        state.kill();
        (void)state.reaped_changed.wait_for(lock, std::chrono::seconds(10), [&state] { return state.reaped; });
      }
    }
  }
  if (state.reader.joinable()) state.reader.join();
  state.close_handles();
  {
    std::lock_guard lock(state.mutex);
    state.started = false;
  }
  return orderly;
}

bool asks_before_starting() noexcept { return debugging_build(); }

}  // namespace secure_frontend::window
