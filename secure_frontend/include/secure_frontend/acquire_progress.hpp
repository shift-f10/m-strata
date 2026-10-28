#pragma once

#include <cstdint>
#include <string_view>

namespace secure_frontend {

// Progress and notices (fixed text, relative paths, sizes, digests, hosts only — §7.3).
// The base class is the silent sink.
class ProgressSink {
 public:
  virtual ~ProgressSink() = default;
  virtual void notice(std::string_view) {}
  virtual void manifest_selected(std::string_view /*name_tag*/, std::string_view /*manifest_sha256*/) {}
  virtual void file_started(std::string_view, std::uint64_t) {}
  virtual void file_bytes(std::string_view, std::uint64_t, std::uint64_t) {}
  // §7.3 log record inputs: relative path, size, sha256, source host, result ("ok" or a reason name).
  virtual void file_finished(std::string_view, std::uint64_t, std::string_view, std::string_view, std::string_view) {}
};

// [y/N] questions and the console lines around them (pull, import).
class Confirmer {
 public:
  virtual ~Confirmer() = default;
  virtual void say(std::string_view line) = 0;
  // yes_applies: --yes was given and answers this question. Otherwise a
  // non-interactive run answers No; an interactive one reads [y/N] (parse_confirmation).
  [[nodiscard]] virtual bool confirm(std::string_view question, bool yes_applies) = 0;
};

}  // namespace secure_frontend
