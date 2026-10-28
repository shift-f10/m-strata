#pragma once
#include "secure_frontend/config_file.hpp"
#include "secure_frontend/json.hpp"
#include "support/test_support.hpp"

#include <filesystem>
#include <map>
#include <string>

namespace secure_frontend_test {

inline std::string json_literal(const std::string& text) {
  std::string out;
  secure_frontend::append_json_string(&out, text);
  return out;
}

// Renders {"a":{"b":1}} from {{"a.b","1"}} (values are JSON literals) through
// the production renderer. The key path named by `duplicate` (a leaf or a
// section) is written twice, for duplicate-key tests: a sibling copy is
// rendered under a placeholder name, which is then renamed to the real one.
inline std::string render_config_json(const std::map<std::string, std::string>& literals,
                                      const std::string& duplicate = {}) {
  if (duplicate.empty()) return secure_frontend::render_config_literals(literals);
  const auto dot = duplicate.rfind('.');
  const std::string parent = dot == std::string::npos ? std::string{} : duplicate.substr(0, dot + 1U);
  const std::string name = dot == std::string::npos ? duplicate : duplicate.substr(dot + 1U);
  const std::string placeholder = "__render_config_json_duplicate__";
  auto copied = literals;
  for (const auto& [path, literal] : literals) {
    if (path == duplicate) copied[parent + placeholder] = literal;
    else if (path.rfind(duplicate + ".", 0) == 0) copied[parent + placeholder + path.substr(duplicate.size())] = literal;
  }
  std::string out = secure_frontend::render_config_literals(copied);
  const std::string quoted = json_literal(placeholder);
  const auto at = out.find(quoted);
  if (at != std::string::npos) out.replace(at, quoted.size(), json_literal(name));
  return out;
}

// A valid minimal configuration over real, owner-only directories: models,
// tokenizer pack, secret root and an engine file (mode 0700).
struct ConfigFixture final {
  std::filesystem::path dir = make_scratch_directory();
  std::map<std::string, std::string> literals;

  ConfigFixture() {
    for (const char* name : {"models", "tok", "secret", "bin"}) make_private_directory(dir / name);
    write_file(dir / "bin" / "engine", "not a real engine\n", 0700);
    literals = {{"schema_version", "1"},
                {"roots.models", json_literal((dir / "models").string())},
                {"roots.tokenizer_pack", json_literal((dir / "tok").string())},
                {"roots.secret", json_literal((dir / "secret").string())},
                {"engine.executable", json_literal((dir / "bin" / "engine").string())},
                {"engine.model_id", json_literal("qwen-local")}};
  }
  [[nodiscard]] std::string document(const std::string& duplicate = {}) const {
    return render_config_json(literals, duplicate);
  }
};

}  // namespace secure_frontend_test
