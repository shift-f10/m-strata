#include "secure_frontend/jinja_renderer.hpp"
#include "secure_frontend/engine_stdio_process.hpp"
#include "secure_frontend/json.hpp"

#include <chrono>
#if defined(_WIN32)
#include <windows.h>
#include "secure_frontend/canonical_path.hpp"
#else
#include <unistd.h>
#endif

namespace secure_frontend {
std::filesystem::path jinja_helper_path() {
#if defined(_WIN32)
  wchar_t buffer[32768];
  const DWORD n = GetModuleFileNameW(nullptr, buffer, 32768U);
  if (n == 0 || n >= 32768U) return {};
  return std::filesystem::path(std::wstring(buffer, n)).parent_path() / "strata-template.exe";
#else
  char buffer[32768];
  const auto n = readlink("/proc/self/exe", buffer, sizeof(buffer));
  if (n <= 0 || static_cast<std::size_t>(n) == sizeof(buffer)) return {};
  return std::filesystem::path(std::string(buffer, static_cast<std::size_t>(n))).parent_path() / "strata-template";
#endif
}
TemplateRenderResult render_jinja_template(const std::filesystem::path& helper, const std::string_view source,
                                           const std::string_view context_json) {
  if (source.empty() || source.size() > kTemplateSourceMax || context_json.size() > kTemplateContextMax ||
      !is_valid_utf8(source)) return TemplateRenderError::invalid_request;
  const auto context = parse_json(context_json, {kTemplateContextMax, 64U, 65536U, kTemplateContextMax, true});
  const auto* value = std::get_if<JsonValue>(&context);
  if (value == nullptr || value->if_object() == nullptr) return TemplateRenderError::invalid_request;
  std::string request = "{\"template\":";
  append_json_string(&request, source);
  request += ",\"context\":";
  request += python_json_text(*value);
  request += '}';
  if (request.size() > 8U * 1024U * 1024U) return TemplateRenderError::invalid_request;
  EngineCommand command;
  command.executable = helper;
  command.insert_serve = false;
  command.no_new_privileges = true;
  command.private_files = true;
  command.working_directory = helper.parent_path();
  command.max_read_line_bytes = 8U * 1024U * 1024U;
#if defined(_WIN32)
  wchar_t root[32768];
  const UINT n = GetSystemWindowsDirectoryW(root, 32768U);
  if (n == 0U || n >= 32768U) return TemplateRenderError::unavailable;
  command.environment.emplace("SystemRoot", path_to_utf8(std::filesystem::path(std::wstring(root, n))));
#endif
  EngineStdioProcess process;
  if (process.start(command)) return TemplateRenderError::unavailable;
  const auto deadline = deadline_after(std::chrono::seconds(3));
  std::string error;
  if (!process.write_line(request, deadline, error)) {
    (void)process.terminate(std::chrono::milliseconds(0));
    return TemplateRenderError::resource_limit;
  }
  process.close_stdin();
  const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - MonotonicClock::now());
  if (remaining.count() <= 0) {
    (void)process.terminate(std::chrono::milliseconds(0));
    return TemplateRenderError::resource_limit;
  }
  auto answer = process.read_line(remaining);
  (void)process.terminate(std::chrono::milliseconds(0));
  if (answer.kind != EngineReadKind::line) return TemplateRenderError::resource_limit;
  auto parsed = parse_json(answer.line, {8U * 1024U * 1024U, 4U, 65536U, kTemplateOutputMax, false});
  const auto* body = std::get_if<JsonValue>(&parsed);
  if (body == nullptr || body->if_array() == nullptr) return TemplateRenderError::rejected;
  std::vector<TemplateSpan> spans;
  std::size_t total = 0;
  for (const auto& item : *body->if_array()) {
    const auto* fields = item.if_object();
    if (fields == nullptr || fields->size() != 2U) return TemplateRenderError::rejected;
    const std::string* text = nullptr;
    const bool* input = nullptr;
    for (const auto& field : *fields) {
      if (field.key == "text") text = field.value.if_string();
      if (field.key == "input") input = field.value.if_boolean();
    }
    if (text == nullptr || input == nullptr || text->size() > kTemplateOutputMax - total)
      return TemplateRenderError::rejected;
    total += text->size();
    spans.push_back({*text, *input});
  }
  return spans;
}
} // namespace secure_frontend
