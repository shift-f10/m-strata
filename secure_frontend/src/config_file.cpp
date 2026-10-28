#include "secure_frontend/config_file.hpp"

#include "secure_frontend/canonical_path.hpp"
#include "secure_frontend/json.hpp"
#include "secure_frontend/parity_table.hpp"

#include <charconv>
#include <cmath>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace secure_frontend {
namespace {

// A decimal row as JSON: the shortest text that reads back as the same double, always with a fraction or an exponent
// (1.0, never 1), so the schema reader keeps it a decimal. A draft never holds a non-finite value (the reader refuses
// one); "0" stands in for it all the same.
std::string decimal_literal(const double value) {
  if (!std::isfinite(value)) return "0";
  char buffer[32];
  const auto [end, error] = std::to_chars(buffer, buffer + sizeof(buffer), value);
  if (error != std::errc{}) return "0";
  std::string text{buffer, end};
  if (text.find_first_of(".eE") == std::string::npos) text += ".0";
  return text;
}

constexpr JsonLimits kConfigJsonLimits{.max_bytes = kMaxConfigFileBytes, .max_depth = 4U,
                                       .max_container_entries = 256U, .max_string_bytes = 4096U,
                                       // SP5 M1 Task 4: defaults.sampling.* rows are decimal; integer
                                       // rows still reject fractions in the schema reader.
                                       .allow_fractions = true};

// SP5 M1 (spec D-3): document-order walk for the first member whose parity row
// is `rejected`. Paths the schema knows (a key or a section) never match: the
// typed namespace wins over the flat upstream name. SP5 M1 Task 10 sharpens the
// section rule: a section name wins ONLY when the object actually speaks the typed
// namespace — every member names a schema path under it. `replicas: {"count": 4}` is
// the pinned rejected scheduler dict, not our `replicas.parallel` section (C-4).
bool typed_section_object(const JsonValue& value, const std::string& prefix) {
  const auto* members = value.if_object();
  if (members == nullptr) return false;
  for (const auto& member : *members) {
    if (member.key.empty() || member.key.find('.') != std::string::npos) return false;
    if (find_config_key(prefix + "." + member.key) == nullptr) return false;
  }
  return true;
}

bool walk_rejected(const std::vector<JsonMember>& members, const std::string& prefix, std::optional<std::string>& out) {
  for (const auto& member : members) {
    if (member.key.empty() || member.key.find('.') != std::string::npos) continue;  // not a path segment
    const std::string path = prefix.empty() ? member.key : prefix + "." + member.key;
    if (find_config_key(path) == nullptr && !(is_config_section(path) && typed_section_object(member.value, path))) {
      const auto* row = parity::strata_parity_row(member.key);
      if (row != nullptr && row->disposition == parity::ParityDisposition::rejected) {
        out = path;
        return true;
      }
    }
    if (const auto* inner = member.value.if_object()) {
      if (walk_rejected(*inner, path, out)) return true;
    }
  }
  return false;
}

// SP5 M1 Task 4: with allow_fractions the parser admits fraction/exponent
// literals, but SP1's schema contract stands — outside a decimal row a
// fraction is the same malformed document the strict parser used to reject at
// its byte, and it wins over excluded-name and unknown-key detection exactly
// like the old parse-time failure did.
bool walk_fraction_outside_decimal(const std::vector<JsonMember>& members, const std::string& prefix) {
  for (const auto& member : members) {
    const std::string path = prefix.empty() ? member.key : prefix + "." + member.key;
    if (const auto* inner = member.value.if_object()) {
      if (walk_fraction_outside_decimal(*inner, path)) return true;
      continue;
    }
    const auto is_decimal = [](const JsonValue& value) { return value.if_decimal() != nullptr; };
    if (const auto* items = member.value.if_array()) {
      for (const auto& item : *items) {
        if (is_decimal(item)) return true;  // no array row is decimal-valued
      }
      continue;
    }
    if (!is_decimal(member.value)) continue;
    const auto* key = find_config_key(path);
    if (key == nullptr || key->type != ConfigValueType::decimal) return true;
  }
  return false;
}

std::optional<ConfigDraftValue> draft_value(const JsonValue& value) {
  if (const auto* b = value.if_boolean()) return ConfigDraftValue{*b};
  if (const auto* i = value.if_integer()) return ConfigDraftValue{*i};
  if (const auto* d = value.if_decimal()) return ConfigDraftValue{*d};
  if (const auto* s = value.if_string()) return ConfigDraftValue{*s};
  if (const auto* items = value.if_array()) {
    std::vector<std::string> strings;
    for (const auto& item : *items) {
      const auto* s = item.if_string();
      if (s == nullptr) return std::nullopt;
      strings.push_back(*s);
    }
    return ConfigDraftValue{std::move(strings)};
  }
  return std::nullopt;
}

void collect(const std::vector<JsonMember>& members, const std::string& prefix, FrontendConfigDraft& draft,
             ConfigErrors& errors) {
  for (const auto& member : members) {
    const std::string path = prefix.empty() ? member.key : prefix + "." + member.key;
    // A member name is one path segment: an empty or dotted name never aliases
    // a nested key ({"listener.port":1} is not {"listener":{"port":1}}).
    if (member.key.empty() || member.key.find('.') != std::string::npos) {
      errors.push_back(ConfigIssue{path, ConfigIssueCode::unknown_key, "unknown key"});
      continue;
    }
    if (const ConfigKey* key = find_config_key(path)) {
      if (draft.entries.contains(path)) {
        errors.push_back(ConfigIssue{path, ConfigIssueCode::duplicate_key, "is set more than once"});
      } else if (auto value = draft_value(member.value)) {
        draft.entries.emplace(path, ConfigDraftEntry{std::move(*value), ConfigSource::file});
      } else {
        errors.push_back(ConfigIssue{path, ConfigIssueCode::wrong_type, std::string{type_reason(key->type)}});
      }
    } else if (is_config_section(path)) {
      if (const auto* inner = member.value.if_object()) collect(*inner, path, draft, errors);
      else errors.push_back(ConfigIssue{path, ConfigIssueCode::wrong_type, "must be an object"});
    } else {
      errors.push_back(ConfigIssue{path, ConfigIssueCode::unknown_key, "unknown key"});
    }
  }
}

}  // namespace

std::optional<std::string> find_rejected_key(const JsonValue& root) {
  const auto* members = root.if_object();
  if (members == nullptr) return std::nullopt;
  std::optional<std::string> found;
  walk_rejected(*members, "", found);
  return found;
}

std::variant<FrontendConfigDraft, ConfigErrors> parse_config_text(const std::string_view text,
                                                                  const std::filesystem::path& config_directory) {
  const auto parsed = parse_json(text, kConfigJsonLimits);
  if (const auto* error = std::get_if<JsonError>(&parsed)) {
    const auto code = error->code == JsonErrorCode::duplicate_key ? ConfigIssueCode::duplicate_key
                                                                  : ConfigIssueCode::malformed_document;
    return ConfigErrors{ConfigIssue{"config file", code,
                                    std::string{describe_json_error(error->code)} + " (byte " +
                                        std::to_string(error->offset) + ")"}};
  }
  return parse_config_object(std::get<JsonValue>(parsed), config_directory);
}

std::variant<FrontendConfigDraft, ConfigErrors> parse_config_object(const JsonValue& root,
                                                                    const std::filesystem::path& config_directory) {
  const auto* members = root.if_object();
  if (members == nullptr) {
    return ConfigErrors{ConfigIssue{"config file", ConfigIssueCode::malformed_document, "must be a JSON object"}};
  }
  // SP5 M1 Task 4: a fraction outside a decimal row is malformed at the whole-
  // document level, before name-based checks (see walk_fraction_outside_decimal).
  if (walk_fraction_outside_decimal(*members, "")) {
    return ConfigErrors{ConfigIssue{"config file", ConfigIssueCode::malformed_document, "has a number that is not an integer"}};
  }
  // SP5 M1 Task 3: an excluded upstream option is named with its rule before
  // generic unknown-key detection (spec D-3).
  if (const auto rejected = find_rejected_key(root)) {
    return ConfigErrors{ConfigIssue{*rejected, ConfigIssueCode::excluded_key,
                                    "is an excluded upstream option (SP5 parity table; the accepted narrowing is in "
                                    "README, section Configuration; threat model docs/security/threat-model.md)"}};
  }
  FrontendConfigDraft draft;
  draft.config_directory = config_directory;
  ConfigErrors errors;
  collect(*members, "", draft, errors);
  if (!errors.empty()) return errors;
  return draft;
}

std::variant<TrustedConfigText, ConfigErrors> read_trusted_config_text(const std::filesystem::path& file) {
  const auto issue = [](const ConfigIssueCode code, std::string reason) {
    return ConfigErrors{ConfigIssue{"config file", code, std::move(reason)}};
  };
  std::error_code error;
  const auto absolute = std::filesystem::absolute(file, error);
  if (error || !absolute.has_filename()) return issue(ConfigIssueCode::file_unavailable, "cannot be resolved");
  // The directory is opened as a root: owned by you (or root) and not writable
  // by others, who could otherwise replace the file between check and use.
  auto directory = open_root(absolute.parent_path());
  if (const auto* failure = std::get_if<PathError>(&directory)) {
    const bool trust = failure->code == PathErrorCode::shared_writable || failure->code == PathErrorCode::untrusted_owner;
    return issue(trust ? ConfigIssueCode::file_untrusted : ConfigIssueCode::file_unavailable, "directory: " + failure->message);
  }
  const auto& root = std::get<CanonicalRoot>(directory);
  const auto name = parse_relative_path(path_to_utf8(absolute.filename()));
  if (!std::holds_alternative<RelativePath>(name)) return issue(ConfigIssueCode::file_unavailable, "has a non-portable file name");
  auto opened = open_beneath(root, std::get<RelativePath>(name),
                             OpenOptions{.kind = OpenKind::regular_file, .trust = FileTrust::not_shared_writable});
  if (const auto* failure = std::get_if<PathError>(&opened)) {
    switch (failure->code) {
      case PathErrorCode::not_found: return issue(ConfigIssueCode::file_unavailable, "does not exist");
      case PathErrorCode::symlink_not_allowed:
      case PathErrorCode::shared_writable:
      case PathErrorCode::untrusted_owner: return issue(ConfigIssueCode::file_untrusted, failure->message);
      default: return issue(ConfigIssueCode::file_unavailable, failure->message);
    }
  }
  auto text = std::get<OpenedFile>(opened).read_all(kMaxConfigFileBytes);
  if (const auto* failure = std::get_if<PathError>(&text)) {
    return failure->code == PathErrorCode::too_large ? issue(ConfigIssueCode::malformed_document, "is larger than 64 KiB")
                                                     : issue(ConfigIssueCode::file_unavailable, failure->message);
  }
  return TrustedConfigText{std::move(std::get<std::string>(text)), root.path()};
}

std::variant<FrontendConfigDraft, ConfigErrors> read_config_file(const std::filesystem::path& file) {
  auto text = read_trusted_config_text(file);
  if (auto* errors = std::get_if<ConfigErrors>(&text)) return std::move(*errors);
  const auto& read = std::get<TrustedConfigText>(text);
  return parse_config_text(read.text, read.directory);
}

FrontendConfigDraft merge_drafts(FrontendConfigDraft file, const FrontendConfigDraft& flags) {
  for (const auto& [key, entry] : flags.entries) {
    if (file.entries.find(key) != file.entries.end()) file.overridden_by_command_line.push_back(key);
    file.entries[key] = entry;
  }
  return file;
}

std::string render_config_literals(const std::map<std::string, std::string>& literals) {
  struct Node {
    std::vector<std::string> names;
    std::vector<Node> children;
    std::string literal;
  };
  Node root;
  for (const auto& [path, literal] : literals) {
    Node* node = &root;
    std::size_t start = 0;
    while (true) {
      const auto dot = path.find('.', start);
      const std::string name = path.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
      std::size_t index = 0;
      while (index < node->names.size() && node->names[index] != name) ++index;
      if (index == node->names.size()) { node->names.push_back(name); node->children.emplace_back(); }
      node = &node->children[index];
      if (dot == std::string::npos) break;
      start = dot + 1;
    }
    node->literal = literal;
  }
  std::string out;
  const std::function<void(const Node&, std::size_t)> emit = [&](const Node& node, const std::size_t depth) {
    out += "{\n";
    for (std::size_t i = 0; i < node.names.size(); ++i) {
      out.append(2U * (depth + 1U), ' ');
      append_json_string(&out, node.names[i]);
      out += ": ";
      if (node.children[i].names.empty()) out += node.children[i].literal;
      else emit(node.children[i], depth + 1U);
      out += i + 1U < node.names.size() ? ",\n" : "\n";
    }
    out.append(2U * depth, ' ');
    out += "}";
  };
  emit(root, 0U);
  return out;
}

std::string render_config_document(const FrontendConfigDraft& draft) {
  std::map<std::string, std::string> literals;
  for (const auto& [path, entry] : draft.entries) {
    std::string& literal = literals[path];
    if (const auto* b = std::get_if<bool>(&entry.value)) literal = *b ? "true" : "false";
    else if (const auto* i = std::get_if<std::int64_t>(&entry.value)) literal = std::to_string(*i);
    else if (const auto* s = std::get_if<std::string>(&entry.value)) append_json_string(&literal, *s);
    else if (const auto* d = std::get_if<double>(&entry.value)) literal = decimal_literal(*d);
    else {
      literal = "[";
      for (const auto& item : std::get<std::vector<std::string>>(entry.value)) {
        if (literal.size() > 1U) literal += ", ";
        append_json_string(&literal, item);
      }
      literal += "]";
    }
  }
  return render_config_literals(literals) + "\n";
}

}  // namespace secure_frontend
