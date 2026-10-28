#include "secure_frontend/canonical_path.hpp"

#include "canonical_path_internal.hpp"

#include <array>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <atomic>

namespace secure_frontend {

static std::atomic<bool>& relaxed_paths_flag() { static std::atomic<bool> f{false}; return f; }
void set_relaxed_paths(bool on) noexcept { relaxed_paths_flag().store(on, std::memory_order_relaxed); }
bool relaxed_paths() noexcept { return relaxed_paths_flag().load(std::memory_order_relaxed); }

namespace {

PathError fail(const PathErrorCode code, std::string message) { return PathError{code, std::move(message)}; }

char ascii_upper(const char c) { return c >= 'a' && c <= 'z' ? static_cast<char>(c - ('a' - 'A')) : c; }

bool valid_utf8(const std::string_view text) {
  for (std::size_t i = 0; i < text.size();) {
    const auto b = static_cast<unsigned char>(text[i]);
    std::size_t length = 0;
    std::uint32_t point = 0;
    if (b < 0x80U) { ++i; continue; }
    if (b >= 0xc2U && b <= 0xdfU) { length = 2; point = b & 0x1fU; }
    else if (b >= 0xe0U && b <= 0xefU) { length = 3; point = b & 0x0fU; }
    else if (b >= 0xf0U && b <= 0xf4U) { length = 4; point = b & 0x07U; }
    else return false;
    if (text.size() - i < length) return false;
    for (std::size_t k = 1; k < length; ++k) {
      const auto c = static_cast<unsigned char>(text[i + k]);
      if ((c & 0xc0U) != 0x80U) return false;
      point = (point << 6U) | (c & 0x3fU);
    }
    if ((length == 3 && point < 0x800U) || (length == 4 && (point < 0x10000U || point > 0x10ffffU)) ||
        (point >= 0xd800U && point <= 0xdfffU)) return false;
    i += length;
  }
  return true;
}

std::optional<PathError> check_component(const std::string_view c) {
  if (c.empty()) return fail(PathErrorCode::empty, "path components must not be empty");
  if (c.size() > 255U) return fail(PathErrorCode::component_too_long, "a path component is longer than 255 bytes");
  if (c == "." || c == "..") return fail(PathErrorCode::traversal, "path components must not be . or ..");
  for (const char ch : c) {
    const auto b = static_cast<unsigned char>(ch);
    if (b < 0x20U || b == 0x7fU) return fail(PathErrorCode::control_character, "paths must not contain control bytes");
    if (ch == ':') return fail(PathErrorCode::drive_or_stream, "paths must not contain ':' (drives, streams)");
    if (ch == '<' || ch == '>' || ch == '"' || ch == '|' || ch == '?' || ch == '*') {
      return fail(PathErrorCode::invalid_character, "paths must not contain < > \" | ? *");
    }
  }
  if (c.back() == '.' || c.back() == ' ') {
    return fail(PathErrorCode::trailing_dot_or_space, "path components must not end with a dot or space");
  }
  if (is_windows_reserved_component(c)) return fail(PathErrorCode::reserved_name, "Windows reserved device names are not allowed");
  for (std::size_t i = 0; i + 1 < c.size(); ++i) {
    if (c[i] == '~' && c[i + 1] >= '0' && c[i + 1] <= '9') {
      return fail(PathErrorCode::short_name_alias, "8.3 short-name aliases (~1) are not allowed");
    }
  }
  return std::nullopt;
}

}  // namespace

bool is_windows_reserved_component(const std::string_view component) noexcept {
  std::string_view base = component.substr(0, component.find('.'));
  while (!base.empty() && base.back() == ' ') base.remove_suffix(1);
  std::string upper;
  for (const char c : base) upper.push_back(ascii_upper(c));
  if (upper == "CON" || upper == "PRN" || upper == "AUX" || upper == "NUL" || upper == "CONIN$" ||
      upper == "CONOUT$" || upper == "CLOCK$") return true;
  if (upper.size() >= 4U && (upper.rfind("COM", 0) == 0 || upper.rfind("LPT", 0) == 0)) {
    const std::string_view suffix = std::string_view{upper}.substr(3);
    if (suffix.size() == 1U && suffix[0] >= '0' && suffix[0] <= '9') return true;
    if (suffix == "\xC2\xB9" || suffix == "\xC2\xB2" || suffix == "\xC2\xB3") return true;  // superscript 1 2 3
  }
  return false;
}

std::variant<RelativePath, PathError> parse_relative_path(const std::string_view text) {
  if (text.empty()) return fail(PathErrorCode::empty, "the path is empty");
  if (text.size() > kRelativePathMaxBytes) return fail(PathErrorCode::too_long, "the path is longer than 1024 bytes");
  if (text.front() == '/' || text.front() == '\\') return fail(PathErrorCode::absolute, "the path must be relative");
  if (!valid_utf8(text)) return fail(PathErrorCode::invalid_utf8, "the path is not valid UTF-8");
  std::vector<std::string> components;
  std::size_t start = 0;
  while (true) {
    const auto end = text.find_first_of("/\\", start);
    const auto component = text.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
    if (auto error = check_component(component)) return *error;
    components.emplace_back(component);
    if (end == std::string_view::npos) break;
    start = end + 1U;
  }
  std::string generic;
  for (const auto& component : components) generic += (generic.empty() ? "" : "/") + component;
  return RelativePath{std::move(generic), std::move(components)};
}

std::filesystem::path path_from_utf8(const std::string_view text) {
  const std::u8string utf8(text.begin(), text.end());
  return std::filesystem::path{utf8};
}

std::string path_to_utf8(const std::filesystem::path& path) {
  const std::u8string utf8 = path.u8string();
  return std::string(utf8.begin(), utf8.end());
}

std::filesystem::path CanonicalRoot::join(const RelativePath& relative) const {
  std::filesystem::path joined = path_;
  for (const auto& component : relative.components()) joined /= path_from_utf8(component);
  joined.make_preferred();
  return joined;
}

std::variant<CanonicalRoot, PathError> open_root(const std::filesystem::path& requested, const RootPolicy policy) {
  if (policy == RootPolicy::create_private && requested.has_filename() && requested.has_parent_path()) {
    std::error_code error;
    const auto status = std::filesystem::symlink_status(requested, error);
    if (status.type() == std::filesystem::file_type::not_found) {
      auto parent = detail::open_existing_root(requested.parent_path());
      if (const auto* failure = std::get_if<PathError>(&parent)) return *failure;
      const auto name = parse_relative_path(path_to_utf8(requested.filename()));
      if (const auto* failure = std::get_if<PathError>(&name)) return *failure;
      if (auto failure = create_directory_within(std::get<CanonicalRoot>(parent), std::get<RelativePath>(name))) return *failure;
    }
  }
  return detail::open_existing_root(requested, policy == RootPolicy::external ? RootPolicy::external : RootPolicy::existing);
}

std::variant<std::filesystem::path, PathError> check_creatable_root(const std::filesystem::path& requested) {
  std::error_code error;
  const auto status = std::filesystem::symlink_status(requested, error);
  if (status.type() != std::filesystem::file_type::not_found) {
    auto existing = detail::open_existing_root(requested);  // also rejects a dangling link: it is not "absent"
    if (const auto* failure = std::get_if<PathError>(&existing)) return *failure;
    return std::get<CanonicalRoot>(existing).path();
  }
  if (!requested.is_absolute() || !requested.has_filename() || !requested.has_parent_path()) {
    return detail::path_error(PathErrorCode::not_absolute, "roots must be absolute paths naming a directory");
  }
  auto parent = detail::open_existing_root(requested.parent_path());
  if (const auto* failure = std::get_if<PathError>(&parent)) return *failure;
  const auto name = parse_relative_path(path_to_utf8(requested.filename()));
  if (const auto* failure = std::get_if<PathError>(&name)) return *failure;
  return std::get<CanonicalRoot>(parent).path() / requested.filename();
}

std::optional<PathError> check_windows_root_form(const std::wstring_view text) {
  const auto is_separator = [](const wchar_t c) { return c == L'\\' || c == L'/'; };
  if (text.empty()) return detail::path_error(PathErrorCode::empty, "the root path is empty");
  if (text.size() >= 2U && is_separator(text[0]) && is_separator(text[1])) {
    return detail::path_error(PathErrorCode::unsupported_root_form, "UNC, device and \\\\?\\ paths are not accepted as roots");
  }
  if (text.substr(0, 4) == L"\\??\\") {
    return detail::path_error(PathErrorCode::unsupported_root_form, "NT object paths are not accepted as roots");
  }
  if (is_separator(text[0])) {
    return detail::path_error(PathErrorCode::not_absolute, "a rooted path without a drive letter depends on the current drive");
  }
  const bool drive_letter = text.size() >= 2U && ((text[0] >= L'A' && text[0] <= L'Z') || (text[0] >= L'a' && text[0] <= L'z')) &&
                            text[1] == L':';
  if (!drive_letter) return detail::path_error(PathErrorCode::not_absolute, "roots must be absolute (X:\\...)");
  if (text.size() == 2U || !is_separator(text[2])) {
    return detail::path_error(PathErrorCode::not_absolute, "a drive-relative path (C:foo) depends on the current directory");
  }
  std::size_t start = 3U;
  while (start < text.size()) {
    std::size_t end = start;
    while (end < text.size() && !is_separator(text[end])) ++end;
    const std::wstring_view component = text.substr(start, end - start);
    if (component.find(L':') != std::wstring_view::npos) {
      return detail::path_error(PathErrorCode::drive_or_stream, "roots must not name an alternate data stream");
    }
    std::string ascii;
    for (const wchar_t c : component) ascii.push_back(c < 0x80 ? static_cast<char>(c) : '_');
    if (!ascii.empty() && is_windows_reserved_component(ascii)) {
      return detail::path_error(PathErrorCode::reserved_name, "roots must not contain a reserved device name");
    }
    start = end + 1U;
  }
  return std::nullopt;
}

std::variant<std::wstring, PathError> classify_final_path(const std::wstring_view final_path) {
  if (final_path.substr(0, 8) == L"\\\\?\\UNC\\") {
    return detail::path_error(PathErrorCode::network_location,
                              "the folder resolves to a network share or a redirected folder (for example OneDrive "
                              "or a roaming profile); choose a local folder on this computer");
  }
  if (final_path.size() >= 7U && final_path.substr(0, 4) == L"\\\\?\\" && final_path[5] == L':') {
    return std::wstring{final_path.substr(4)};
  }
  return detail::path_error(PathErrorCode::unsupported_root_form, "the folder resolves to an unsupported location");
}

OpenedFile& OpenedFile::operator=(OpenedFile&& other) noexcept {
  if (this != &other) {
    close_now();
    handle_ = other.handle_;
    size_ = other.size_;
    owns_ = other.owns_;
    other.owns_ = false;
  }
  return *this;
}

OpenedFile::~OpenedFile() { close_now(); }

std::variant<std::string, PathError> OpenedFile::read_all(const std::size_t max_bytes) const {
  std::string out;
  std::array<char, 65536> chunk{};
  while (true) {
    auto got = read_some(chunk.data(), chunk.size());
    if (auto* error = std::get_if<PathError>(&got)) return *error;
    const std::size_t count = std::get<std::size_t>(got);
    if (count == 0U) return out;
    if (count > max_bytes - out.size()) return detail::path_error(PathErrorCode::too_large, "the file is larger than its limit");
    out.append(chunk.data(), count);
  }
}

}  // namespace secure_frontend
