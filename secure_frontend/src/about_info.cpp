#include "secure_frontend/about_info.hpp"

#include "secure_frontend/calibration.hpp"
#include "secure_frontend/host_readings.hpp"
#include "secure_frontend/json.hpp"

#include <fstream>
#include <iterator>
#include <string_view>
#include <system_error>
#include <variant>

#if defined(_WIN32)
#include "secure_frontend/win32_prelude.hpp"
#else
#include <sys/utsname.h>
#endif

namespace secure_frontend {
namespace {

constexpr std::uintmax_t kMaxRecordBytes = 64U * 1024U;
constexpr std::size_t kMaxText = 200U;
constexpr std::size_t kMaxEngines = 32U;
constexpr std::size_t kMaxFeatures = 8U;

bool printable(const std::string& text) {
  if (text.size() > kMaxText) return false;
  for (const char c : text) {
    if (c < 0x20 || c >= 0x7f) return false;
  }
  return true;
}

const JsonValue* member(const JsonValue& object, const std::string_view key) {
  for (const auto& entry : *object.if_object()) {
    if (entry.key == key) return &entry.value;
  }
  return nullptr;
}

// A text field: missing is empty; anything but printable text of at most kMaxText refuses the record.
bool read_text(const JsonValue& object, const std::string_view key, std::string& out) {
  const JsonValue* value = member(object, key);
  if (value == nullptr) return true;
  const std::string* text = value->if_string();
  if (text == nullptr || !printable(*text)) return false;
  out = *text;
  return true;
}

std::string ascii(const std::string_view text) {
  std::string out;
  for (const char c : text) out.push_back(c >= 0x20 && c < 0x7f ? c : '?');
  while (!out.empty() && out.front() == ' ') out.erase(out.begin());
  while (!out.empty() && out.back() == ' ') out.pop_back();
  if (out.size() > kMaxText) out.resize(kMaxText);
  return out;
}

#if defined(_WIN32)
constexpr const wchar_t* kVersionKey = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";

std::string registry_text(const wchar_t* name) {
  wchar_t buffer[256]{};
  DWORD bytes = sizeof(buffer);
  if (RegGetValueW(HKEY_LOCAL_MACHINE, kVersionKey, name, RRF_RT_REG_SZ, nullptr, buffer, &bytes) != ERROR_SUCCESS) return {};
  std::string out;
  for (const wchar_t* c = buffer; *c != L'\0'; ++c) out.push_back(*c >= 0x20 && *c < 0x7f ? static_cast<char>(*c) : '?');
  return ascii(out);
}

std::optional<DWORD> registry_number(const wchar_t* name) {
  DWORD value = 0;
  DWORD bytes = sizeof(value);
  if (RegGetValueW(HKEY_LOCAL_MACHINE, kVersionKey, name, RRF_RT_REG_DWORD, nullptr, &value, &bytes) != ERROR_SUCCESS) return std::nullopt;
  return value;
}

// "Windows 11 Pro 25H2 (build 26200.1)": the edition, its version and build. Windows 11 still names itself Windows 10 in
// ProductName; its builds start at 22000.
std::string os_fact() {
  std::string product = registry_text(L"ProductName");
  const std::string build = registry_text(L"CurrentBuild");
  const std::string version = registry_text(L"DisplayVersion");
  const auto revision = registry_number(L"UBR");
  unsigned long number = 0;
  for (const char c : build) {
    if (c < '0' || c > '9' || number > 1000000UL) break;
    number = number * 10UL + static_cast<unsigned long>(c - '0');
  }
  if (number >= 22000UL && product.rfind("Windows 10", 0) == 0U) product.replace(0, 10, "Windows 11");
  if (product.empty()) product = "Windows";
  std::string out = product + (version.empty() ? "" : " " + version);
  if (!build.empty()) out += " (build " + build + (revision ? "." + std::to_string(*revision) : "") + ")";
  return ascii(out);
}
#else
// "Ubuntu 24.04.3 LTS (Linux 6.8.0-49-generic)": /etc/os-release's PRETTY_NAME and the kernel release.
std::string os_fact() {
  std::string pretty;
  std::ifstream release("/etc/os-release");
  for (std::string line; std::getline(release, line);) {
    if (line.rfind("PRETTY_NAME=", 0) != 0U) continue;
    pretty = line.substr(12);
    if (pretty.size() >= 2U && (pretty.front() == '"' || pretty.front() == '\'') && pretty.back() == pretty.front()) {
      pretty = pretty.substr(1, pretty.size() - 2U);
    }
    break;
  }
  struct utsname name {};
  const std::string kernel = ::uname(&name) == 0 ? std::string{name.sysname} + " " + name.release : std::string{};
  if (pretty.empty()) return ascii(kernel);
  return ascii(kernel.empty() ? pretty : pretty + " (" + kernel + ")");
}
#endif

void append_key(std::string* out, const std::string_view key, const bool first = false) {
  if (!first) out->push_back(',');
  append_json_string(out, key);
  out->push_back(':');
}

void append_text_or_null(std::string* out, const std::string& text) {
  if (text.empty()) out->append("null");
  else append_json_string(out, text);
}

}  // namespace

std::optional<EngineBuild> read_engine_build(const std::filesystem::path& engine_folder) {
  const auto file = engine_folder / "build.json";
  std::error_code ec;
  const auto size = std::filesystem::file_size(file, ec);
  if (ec || size > kMaxRecordBytes) return std::nullopt;
  std::ifstream in(file, std::ios::binary);
  if (!in) return std::nullopt;
  const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  const auto parsed = parse_json(text, JsonLimits{.max_bytes = static_cast<std::size_t>(kMaxRecordBytes), .max_depth = 4U,
                                                  .max_container_entries = 64U, .max_string_bytes = 1024U, .allow_fractions = false});
  const auto* root = std::get_if<JsonValue>(&parsed);
  if (root == nullptr || root->if_object() == nullptr) return std::nullopt;
  const JsonValue* schema = member(*root, "schema_version");
  if (schema == nullptr || schema->if_integer() == nullptr || *schema->if_integer() != 1) return std::nullopt;
  EngineBuild build;
  for (const auto& [key, field] : {std::pair<std::string_view, std::string*>{"built", &build.built}, {"commit", &build.commit},
                                   {"ggml", &build.ggml}, {"cuda_arch", &build.cuda_arch}}) {
    if (!read_text(*root, key, *field)) return std::nullopt;
  }
  if (const JsonValue* features = member(*root, "features")) {
    const auto* list = features->if_array();
    if (list == nullptr || list->size() > kMaxFeatures) return std::nullopt;
    for (const auto& item : *list) {
      const std::string* name = item.if_string();
      if (name == nullptr || !printable(*name)) return std::nullopt;
      build.features.push_back(*name);
    }
  }
  if (const JsonValue* engines = member(*root, "engines")) {
    const auto* list = engines->if_array();
    if (list == nullptr || list->size() > kMaxEngines) return std::nullopt;
    for (const auto& item : *list) {
      if (item.if_object() == nullptr) return std::nullopt;
      EngineBuildLibrary library;
      if (!read_text(item, "library", library.library) || !read_text(item, "models", library.models) ||
          !read_text(item, "source", library.source) || library.library.empty()) {
        return std::nullopt;
      }
      if (const JsonValue* bytes = member(item, "bytes")) {
        if (bytes->if_integer() == nullptr || *bytes->if_integer() < 0) return std::nullopt;
        library.bytes = static_cast<std::uint64_t>(*bytes->if_integer());
      }
      build.engines.push_back(std::move(library));
    }
  }
  return build;
}

HardwareFacts hardware_facts() {
  HardwareFacts facts;
  facts.cpu = ascii(cpu_fact());
  if (const auto memory = read_memory()) facts.ram_bytes = memory->total_bytes;
  facts.os = os_fact();
  return facts;
}

std::string render_about_json(const std::optional<EngineBuild>& build, const HardwareFacts& hardware) {
  std::string out = "{";
  append_key(&out, "build", true);
  if (!build.has_value()) {
    out.append("null");
  } else {
    out.push_back('{');
    append_key(&out, "built", true);
    append_text_or_null(&out, build->built);
    append_key(&out, "commit");
    append_text_or_null(&out, build->commit);
    append_key(&out, "ggml");
    append_text_or_null(&out, build->ggml);
    append_key(&out, "cuda_arch");
    append_text_or_null(&out, build->cuda_arch);
    append_key(&out, "features");
    out.push_back('[');
    for (std::size_t i = 0; i < build->features.size(); ++i) {
      if (i != 0U) out.push_back(',');
      append_json_string(&out, build->features[i]);
    }
    out.push_back(']');
    append_key(&out, "engines");
    out.push_back('[');
    for (std::size_t i = 0; i < build->engines.size(); ++i) {
      const auto& library = build->engines[i];
      if (i != 0U) out.push_back(',');
      out.push_back('{');
      append_key(&out, "library", true);
      append_json_string(&out, library.library);
      append_key(&out, "models");
      append_json_string(&out, library.models);
      append_key(&out, "source");
      append_json_string(&out, library.source);
      append_key(&out, "bytes");
      out.append(std::to_string(library.bytes));
      out.push_back('}');
    }
    out.append("]}");
  }
  append_key(&out, "hardware");
  out.push_back('{');
  append_key(&out, "cpu", true);
  append_text_or_null(&out, hardware.cpu);
  append_key(&out, "ram_bytes");
  out.append(hardware.ram_bytes.has_value() ? std::to_string(*hardware.ram_bytes) : std::string{"null"});
  append_key(&out, "gpu");
  append_text_or_null(&out, hardware.gpu);
  append_key(&out, "os");
  append_text_or_null(&out, hardware.os);
  out.append("}}\n");
  return out;
}

}  // namespace secure_frontend
