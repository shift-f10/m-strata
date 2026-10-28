// parity_table_test.cpp — SP5 M1 Task 2 meta-tests (Review Focus 1), extended
// by SP5-P2 Task 1 with the drift tripwires (plan 2026-10-22-sp5-p2): the
// CURRENT vendored trees are re-enumerated at run time (upstream @ v0.2.2,
// engines/maya-ref @ v1.1.21) and EVERY knob found — argv options, engine env
// names, SETTINGS.md keys — must carry exactly one dispositioned row whose
// citation lands on a line mentioning the name at the current pin.
//
// Every option name the pinned upstream reads must have exactly one
// dispositioned row; a name in no bucket — or in two — is a defect. Typed rows
// must name where the value lands; rejected rows must cite the excluding rule;
// mapped-away rows must cite a governing read site.

#include "parity_keys_upstream.hpp"
#include "parity_keys_maya.hpp"
#include "secure_frontend/parity_table.hpp"
#include "support/test_support.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace {

using secure_frontend::parity::ParityDisposition;
using secure_frontend::parity::ParityDestination;
using secure_frontend::parity::ParityRow;
using secure_frontend::parity::maya_config_keys;
using secure_frontend::parity::maya_env_knobs;
using secure_frontend::parity::maya_parity_row;
using secure_frontend::parity::maya_parity_rows;
using secure_frontend::parity::strata_parity_row;
using secure_frontend::parity::strata_parity_rows;
using secure_frontend::parity::upstream_config_keys;
using secure_frontend::parity::upstream_env_knobs;

int failures = 0;
void expect(const bool condition, const std::string& message) {
  if (!condition) { ++failures; std::printf("FAILED: %s\n", message.c_str()); }
}

std::size_t row_count_named(std::string_view name) {
  std::size_t hits = 0;
  for (const auto& row : strata_parity_rows()) {
    if (row.key == name) ++hits;
  }
  return hits;
}

std::size_t maya_row_count_named(std::string_view name) {
  std::size_t hits = 0;
  for (const auto& row : maya_parity_rows()) {
    if (row.key == name) ++hits;
  }
  return hits;
}

void every_upstream_key_has_exactly_one_disposition() {
  const auto keys = upstream_config_keys();
  expect(keys.size() == 92U, "the generated config-key inventory holds the pin's 92 keys");

  for (const auto key : keys) {
    expect(row_count_named(key) == 1U,
           std::string("config key '") + std::string(key) + "' must have exactly one parity row");
    const auto* row = strata_parity_row(key);
    expect(row != nullptr, std::string("strata_parity_row('") + std::string(key) + "') must resolve");
  }
}

void every_env_knob_is_dispositioned() {
  const auto knobs = upstream_env_knobs();
  expect(knobs.size() == 38U, "the generated env-knob inventory holds the pin's 38 names");
  for (const auto knob : knobs) {
    expect(row_count_named(knob) == 1U,
           std::string("env knob '") + std::string(knob) + "' must have exactly one parity row");
    const auto* row = strata_parity_row(knob);
    expect(row != nullptr, std::string("strata_parity_row('") + std::string(knob) + "') must resolve");
  }
}

void typed_rows_name_a_destination() {
  for (const auto& row : strata_parity_rows()) {
    switch (row.disposition) {
      case ParityDisposition::typed:
        expect(row.destination != secure_frontend::parity::ParityDestination::none,
               std::string("typed row '") + std::string(row.key) + "' must name a destination");
        expect(!row.citation.empty(),
               std::string("typed row '") + std::string(row.key) + "' must cite the pinned read site");
        break;
      case ParityDisposition::rejected:
      case ParityDisposition::mapped_away:
        expect(!row.citation.empty(),
               std::string("dispositioned row '") + std::string(row.key) + "' must carry a citation");
        break;
      case ParityDisposition::out_of_scope:
        // SP5-P2 (plan D-7 disposition set): out-of-scope is not a typed
        // channel and not a security exclusion — it reaches nothing, so it
        // must name no destination and cite the finding at the pin.
        expect(!row.citation.empty(),
               std::string("out-of-scope row '") + std::string(row.key) + "' must carry a citation");
        expect(row.destination == ParityDestination::none,
               std::string("out-of-scope row '") + std::string(row.key) + "' must reach nothing");
        break;
    }
    if (row.disposition == ParityDisposition::rejected) {
      // D-3: every reject is an exclusion of the spec's own rule set — the
      // citation must name the rule so the reject rows are auditable.
      expect(row.citation.find("spec ") == 0U,
             std::string("rejected row '") + std::string(row.key) + "' must cite the excluding rule");
    }
  }
  // A row for a name no inventory grep sees is allowed only as an extra rule
  // row (e.g. mcpServers, vram_reserve_mib); nothing may duplicate a name.
  for (const auto& row : strata_parity_rows()) {
    expect(row_count_named(row.key) == 1U,
           std::string("row name '") + std::string(row.key) + "' must be unique in the table");
  }
}

void every_maya_key_has_exactly_one_disposition() {
  const auto keys = maya_config_keys();
  expect(keys.size() == 42U, "the generated Maya config-key inventory holds the vendored pin's 42 keys");
  for (const auto key : keys) {
    expect(maya_row_count_named(key) == 1U,
           std::string("Maya config key '") + std::string(key) + "' must have exactly one parity row");
    const auto* row = maya_parity_row(key);
    expect(row != nullptr, std::string("maya_parity_row('") + std::string(key) + "') must resolve");
    if (row == nullptr) continue;
    expect(!row->citation.empty(),
           std::string("Maya row '") + std::string(key) + "' must cite the vendored read site");
    if (row->disposition == ParityDisposition::typed) {
      expect(row->destination != secure_frontend::parity::ParityDestination::none,
             std::string("typed Maya row '") + std::string(key) + "' must name a destination");
    }
    if (row->disposition == ParityDisposition::rejected) {
      expect(row->citation.find("spec ") == 0U,
             std::string("rejected Maya row '") + std::string(key) + "' must cite the excluding rule");
    }
  }
}

void every_maya_env_knob_is_dispositioned() {
  const auto knobs = maya_env_knobs();
  expect(knobs.size() == 690U, "the generated Maya env-knob inventory holds the pin's 690 names");
  for (const auto knob : knobs) {
    expect(maya_row_count_named(knob) == 1U,
           std::string("Maya env knob '") + std::string(knob) + "' must have exactly one parity row");
    const auto* row = maya_parity_row(knob);
    expect(row != nullptr, std::string("maya_parity_row('") + std::string(knob) + "') must resolve");
    if (row == nullptr) continue;
    expect(!row->citation.empty(),
           std::string("Maya row '") + std::string(knob) + "' must cite the vendored read site");
    if (row->disposition == ParityDisposition::typed) {
      expect(row->destination != secure_frontend::parity::ParityDestination::none,
             std::string("typed Maya row '") + std::string(knob) + "' must name a destination");
    }
    if (row->disposition == ParityDisposition::rejected) {
      expect(row->citation.find("spec ") == 0U,
             std::string("rejected Maya row '") + std::string(knob) + "' must cite the excluding rule");
    }
  }
  // Nothing may name a row twice in the GLM table (extra rule rows included).
  for (const auto& row : maya_parity_rows()) {
    expect(maya_row_count_named(row.key) == 1U,
           std::string("GLM row name '") + std::string(row.key) + "' must be unique in the table");
  }
}

void dflash_is_rejected_on_licence() {
  // C-5: the DFlash2 drafter and its knobs stay rejected on the A7 licence
  // ground (CC BY-NC-ND weights) even under D-7 — not a knob-visibility call.
  for (const std::string_view name :
       {std::string_view("STRATA_GLM_DFLASH"), std::string_view("STRATA_GLM_DFLASH_MIN"),
        std::string_view("STRATA_GLM_DFLASH_STATS"), std::string_view("STRATA_GLM_DFLASH_PROF"),
        std::string_view("--dflash")}) {
    const auto* row = maya_parity_row(name);
    expect(row != nullptr, std::string("maya_parity_row('") + std::string(name) + "') must resolve");
    if (row == nullptr) continue;
    expect(row->disposition == ParityDisposition::rejected,
           std::string("'") + std::string(name) + "' is rejected on the licence ground (C-5)");
    expect(row->citation.find("C-5") != std::string_view::npos &&
               row->citation.find("licence") != std::string_view::npos,
           std::string("'") + std::string(name) + "' must cite the C-5/A7 licence exclusion");
  }
}

void updater_and_installer_rows_are_rejected() {
  // PRIV-01: no update checks, no self-updater (spec §4 Maya block).
  for (const std::string_view name :
       {std::string_view("MAYA_UPDATE_CHECK"), std::string_view("MAYA_RESTART_ON_UPDATE"),
        std::string_view("installer")}) {
    const auto* row = maya_parity_row(name);
    expect(row != nullptr, std::string("maya_parity_row('") + std::string(name) + "') must resolve");
    if (row == nullptr) continue;
    expect(row->disposition == ParityDisposition::rejected,
           std::string("'") + std::string(name) + "' must be rejected (PRIV-01)");
    expect(row->citation.find("PRIV-01") != std::string_view::npos,
           std::string("'") + std::string(name) + "' must cite PRIV-01");
  }
}

// ---- SP5-P2 Task 1: drift tripwires over the CURRENT vendored trees ---------
// Plan 2026-10-22-sp5-p2 Task 1 (ledger ruling R-T1-a canonizes the scans;
// the spec Appendix A greps are the upstream canon):
//   upstream    cfg.get("<key>"  and STRATA_[A-Z0-9_]+ in upstream/serve/*.py.
//               Upstream's own serve argv is its launch/test CLI, not the
//               run-config surface (Appendix A enumerates exactly the greps),
//               so no argv scan runs on the upstream side.
//   Maya config cfg.get("<key>"  in engines/maya-ref/serve/*.py + maya.py.
//   Maya env    environ.get("<NAME>") and fully-quoted "<NAME>" literals in
//               those files (NAME matching (STRATA|MAYA)_...), plus quoted
//               "(STRATA|DS4)_..." literals across engines/maya-ref/src/**:
//               the engine also reads env through helpers (env_num/env_on),
//               so a getenv-only scan would miss knobs.
//   Maya docs   the backticked MAYA_[A-Z0-9_]+ names and `--flag` literals in
//               docs/SETTINGS.md. include/strata/platform/env_alias.hpp maps
//               every MAYA_X to STRATA_X at engine start (the name the code
//               reads), so a doc-name row carries its engine row in target.
//   Maya argv   the quoted "--flag" literals across maya.py + serve/*.py (the
//               launcher the SP4 manifest replaces: every flag it names gets
//               a row).
// Every found name must have exactly one row in its table, and the row's
// first well-formed `file:line` citations (resolved under the profile's
// vendored root) must land at the CURRENT pin on a line mentioning the name
// or its alias spelling. A missing root argument or an implausibly small
// scan fails: the tripwire never passes vacuously.

using Sites = std::map<std::string, std::string>;  // name -> first site "rel:line"

std::string scan_root_or_fail() {
  const auto& args = secure_frontend_test::extra_arguments();
  if (args.empty()) {
    expect(false, "the scan cases need the repository root as their first argument");
    return {};
  }
  std::error_code ec;
  const auto root = std::filesystem::weakly_canonical(std::filesystem::path{args[0]}, ec);
  if (ec || !std::filesystem::is_directory(root)) {
    expect(false, "the vendored tree root is missing: " + args[0]);
    return {};
  }
  return root.string();
}

bool is_upper_name_char(const char c) { return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; }
bool is_flag_char(const char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'; }

const std::vector<std::string>& file_lines_cached(const std::filesystem::path& path) {
  static std::map<std::string, std::vector<std::string>> cache;
  const std::string key = path.string();
  if (const auto found = cache.find(key); found != cache.end()) return found->second;
  std::ifstream input(path, std::ios::binary);
  std::string content;
  if (input) {
    std::ostringstream text;
    text << input.rdbuf();
    content = text.str();
  }
  auto& lines = cache[key];
  std::string_view all{content};
  std::size_t start = 0;
  while (true) {
    const auto nl = all.find('\n', start);
    if (nl == std::string_view::npos) { lines.emplace_back(all.substr(start)); break; }
    lines.emplace_back(all.substr(start, nl - start));
    start = nl + 1;
  }
  return lines;
}

std::vector<std::pair<std::filesystem::path, std::string>> py_files_below(const std::filesystem::path& dir,
                                                                          const std::string& rel_prefix) {
  std::vector<std::pair<std::filesystem::path, std::string>> out;
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
    if (!entry.is_regular_file()) continue;
    const auto name = entry.path().filename().string();
    if (name.size() < 4 || name.compare(name.size() - 3, 3, ".py") != 0) continue;
    out.emplace_back(entry.path(), rel_prefix + name);
  }
  std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.second < b.second; });
  return out;
}

constexpr std::array<std::string_view, 8> kSourceExtensions{{".c", ".cc", ".cpp", ".cu", ".h", ".hh", ".hpp", ".hxx"}};

bool has_source_extension(const std::string& name) {
  for (const auto ext : kSourceExtensions) {
    if (name.size() >= ext.size() && name.compare(name.size() - ext.size(), ext.size(), ext) == 0) return true;
  }
  return false;
}

void collect_source_files(const std::filesystem::path& dir, const std::filesystem::path& base,
                          std::vector<std::pair<std::filesystem::path, std::string>>& out) {
  std::error_code ec;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(dir, ec)) {
    if (!entry.is_regular_file()) continue;
    const auto name = entry.path().filename().string();
    if (!has_source_extension(name)) continue;
    std::string rel = std::filesystem::relative(entry.path(), base, ec).generic_string();
    if (ec) continue;
    out.emplace_back(entry.path(), rel);
  }
  std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.second < b.second; });
}

void add_site(Sites& sites, const std::string_view name, const std::string& rel, const std::size_t line_no) {
  sites.emplace(std::string{name}, rel + ":" + std::to_string(line_no));
}

void scan_cfg_get(const std::string& line, Sites& sites, const std::string& rel, const std::size_t line_no) {
  // The literal `cfg.get("` is nine characters.
  for (std::size_t pos = line.find("cfg.get(\""); pos != std::string::npos; pos = line.find("cfg.get(\"", pos + 9)) {
    std::size_t end = pos + 9;
    while (end < line.size() && ((line[end] >= 'a' && line[end] <= 'z') || is_upper_name_char(line[end]))) ++end;
    if (end > pos + 9 && end < line.size() && line[end] == '"') add_site(sites, std::string_view{line}.substr(pos + 9, end - pos - 9), rel, line_no);
  }
}

void scan_env_loose(const std::string& line, std::string_view prefix, Sites& sites, const std::string& rel,
                    const std::size_t line_no) {
  for (std::size_t pos = line.find(prefix); pos != std::string::npos; pos = line.find(prefix, pos + prefix.size())) {
    std::size_t end = pos + prefix.size();
    while (end < line.size() && is_upper_name_char(line[end])) ++end;
    if (end > pos + prefix.size()) add_site(sites, std::string_view{line}.substr(pos, end - pos), rel, line_no);
  }
}

void scan_environ_get(const std::string& line, Sites& sites, const std::string& rel, const std::size_t line_no) {
  static constexpr std::array<std::string_view, 2> kPrefixes{{"STRATA_", "MAYA_"}};
  // The literal `environ.get("` is thirteen characters.
  for (std::size_t pos = line.find("environ.get(\""); pos != std::string::npos; pos = line.find("environ.get(\"", pos + 13)) {
    const std::size_t start = pos + 13;
    for (const auto prefix : kPrefixes) {
      if (line.compare(start, prefix.size(), prefix) != 0) continue;
      std::size_t end = start + prefix.size();
      while (end < line.size() && is_upper_name_char(line[end])) ++end;
      if (end > start + prefix.size() && end < line.size() && line[end] == '"')
        add_site(sites, std::string_view{line}.substr(start, end - start), rel, line_no);
    }
  }
}

void scan_quoted(const std::string& line, std::initializer_list<std::string_view> prefixes, Sites& sites,
                 const std::string& rel, const std::size_t line_no) {
  for (std::size_t pos = 0; pos + 1 < line.size(); ++pos) {
    if (line[pos] != '"') continue;
    for (const auto prefix : prefixes) {
      if (line.compare(pos + 1, prefix.size(), prefix) != 0) continue;
      std::size_t end = pos + 1 + prefix.size();
      while (end < line.size() && is_upper_name_char(line[end])) ++end;
      if (end > pos + 1 + prefix.size() && end < line.size() && line[end] == '"')
        add_site(sites, std::string_view{line}.substr(pos + 1, end - pos - 1), rel, line_no);
    }
  }
}

// A fully-quoted "--flag" literal: -- + [a-z0-9-]+ ending in an alphanumeric.
void scan_quoted_flag(const std::string& line, Sites& sites, const std::string& rel, const std::size_t line_no) {
  for (std::size_t pos = 0; pos + 4 < line.size(); ++pos) {
    if (line[pos] != '"' || line[pos + 1] != '-' || line[pos + 2] != '-' || !((line[pos + 3] >= 'a' && line[pos + 3] <= 'z'))) continue;
    std::size_t end = pos + 3;
    while (end < line.size() && is_flag_char(line[end])) ++end;
    if (end <= pos + 3 || end >= line.size() || line[end] != '"' || line[end - 1] == '-') continue;
    add_site(sites, std::string_view{line}.substr(pos + 1, end - pos - 1), rel, line_no);
  }
}

// A backticked `MAYA_*` env name or `--flag` in the reference docs.
void scan_backtick_docs(const std::string& line, Sites& sites, const std::string& rel, const std::size_t line_no) {
  for (std::size_t pos = 0; pos + 1 < line.size(); ++pos) {
    if (line[pos] != '`') continue;
    const std::size_t start = pos + 1;
    if (line.compare(start, 5, "MAYA_") == 0) {
      std::size_t end = start + 5;
      while (end < line.size() && is_upper_name_char(line[end])) ++end;
      if (end > start + 5 && end < line.size() && line[end] == '`')
        add_site(sites, std::string_view{line}.substr(start, end - start), rel, line_no);
      continue;
    }
    if (line.compare(start, 2, "--") == 0 && start + 2 < line.size() && line[start + 2] >= 'a' && line[start + 2] <= 'z') {
      std::size_t end = start + 2;
      while (end < line.size() && is_flag_char(line[end])) ++end;
      if (end < line.size() && line[end] == '`' && line[end - 1] != '-')
        add_site(sites, std::string_view{line}.substr(start, end - start), rel, line_no);
    }
  }
}

std::vector<std::pair<std::string, std::size_t>> extract_citations(const std::string_view citation) {
  static constexpr std::array<std::string_view, 9> kCiteExtensions{
      {".cpp:", ".hpp:", ".hxx:", ".cxx:", ".py:", ".md:", ".sh:", ".cu:", ".c:"}};
  auto is_path_char = [](const char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '/' || c == '-' || c == '.';
  };
  std::vector<std::pair<std::string, std::size_t>> out;
  for (std::size_t pos = 0; pos < citation.size(); ++pos) {
    for (const auto ext : kCiteExtensions) {
      const std::size_t name_len = ext.size() - 1;  // ext without the trailing ':'
      if (citation.compare(pos, name_len, ext, 0, name_len) != 0) continue;
      if (pos + name_len >= citation.size() || citation[pos + name_len] != ':') continue;
      std::size_t scan = pos + name_len + 1;
      if (scan >= citation.size() || citation[scan] < '0' || citation[scan] > '9') continue;
      std::size_t path_start = pos;
      while (path_start > 0 && is_path_char(citation[path_start - 1])) --path_start;
      std::size_t number = 0;
      while (scan < citation.size() && citation[scan] >= '0' && citation[scan] <= '9') number = number * 10U + (citation[scan++] - '0');
      out.emplace_back(std::string{citation.substr(path_start, pos - path_start + name_len)}, number);
      pos = scan;
      break;
    }
  }
  return out;
}

std::vector<std::string> key_spellings(const std::string_view key, const bool aliasable) {
  std::vector<std::string> out{std::string{key}};
  if (aliasable && key.rfind("STRATA_", 0) == 0) out.emplace_back("MAYA_" + std::string{key.substr(7)});
  if (aliasable && key.rfind("MAYA_", 0) == 0) out.emplace_back("STRATA_" + std::string{key.substr(5)});
  return out;
}

void check_current_citation(const ParityRow& row, const std::vector<std::string>& spellings,
                            const std::filesystem::path& tree_root) {
  const auto citations = extract_citations(row.citation);
  for (const auto& [rel, line_no] : citations) {
    const auto file = tree_root / rel;
    if (!std::filesystem::is_regular_file(file)) continue;
    const auto& lines = file_lines_cached(file);
    if (line_no == 0U || line_no > lines.size()) continue;
    const std::string& text = lines[line_no - 1];
    if (std::any_of(spellings.begin(), spellings.end(),
                    [&](const std::string& spelling) { return text.find(spelling) != std::string::npos; }))
      return;
  }
  expect(false, "STALE-CITATION " + std::string{row.key} +
                    ": no cited `file:line` lands at the current pin on a line mentioning the name (" +
                    std::string{row.citation}.substr(0, 90) + ")");
}

void check_sites_dispositioned(const Sites& sites, const char* what, std::size_t (*count_named)(std::string_view),
                               const ParityRow* (*lookup)(std::string_view),
                               const std::filesystem::path& tree_root, const bool aliasable) {
  for (const auto& [name, site] : sites) {
    const auto hits = count_named(name);
    if (hits != 1U) {
      expect(false, std::string{"UNDISPOSITIONED "} + what + " '" + name + "' found at " + site +
                        "; rows naming it in the table: " + std::to_string(hits));
      continue;
    }
    const auto* row = lookup(name);
    if (row == nullptr) {
      expect(false, "lookup failed for " + name);
      continue;
    }
    check_current_citation(*row, key_spellings(name, aliasable), tree_root);
  }
}

void scan_upstream_knobs_need_dispositions_and_current_citations() {
  const std::string root_text = scan_root_or_fail();
  if (root_text.empty()) return;
  const std::filesystem::path root{std::filesystem::path{root_text} / "upstream"};
  const auto files = py_files_below(root / "serve", "serve/");
  expect(files.size() >= 50U, "the upstream serve tree is present (" + std::to_string(files.size()) + " .py files)");
  Sites cfg;
  Sites env;
  for (const auto& [file, rel] : files) {
    const auto& lines = file_lines_cached(file);
    for (std::size_t i = 0; i < lines.size(); ++i) {
      scan_cfg_get(lines[i], cfg, rel, i + 1);
      scan_env_loose(lines[i], "STRATA_", env, rel, i + 1);
    }
  }
  expect(cfg.size() >= 80U, "the upstream cfg.get scan is plausible (" + std::to_string(cfg.size()) + " keys)");
  expect(env.size() >= 25U, "the upstream STRATA_ scan is plausible (" + std::to_string(env.size()) + " names)");
  check_sites_dispositioned(cfg, "upstream config key", &row_count_named, &strata_parity_row, root, false);
  check_sites_dispositioned(env, "upstream env knob", &row_count_named, &strata_parity_row, root, false);
}

void scan_maya_knobs_need_dispositions_and_current_citations() {
  const std::string root_text = scan_root_or_fail();
  if (root_text.empty()) return;
  const std::filesystem::path root{std::filesystem::path{root_text} / "engines" / "maya-ref"};
  auto py = py_files_below(root / "serve", "serve/");
  if (const auto maya_py = root / "maya.py"; std::filesystem::is_regular_file(maya_py)) py.emplace_back(maya_py, "maya.py");
  expect(py.size() >= 15U, "the vendored Maya serve tree is present (" + std::to_string(py.size()) + " .py files)");
  Sites cfg;
  Sites env;   // server environ.get + quoted literals + engine src literals
  Sites flags;  // quoted "--flag" literals across maya.py + serve/*.py
  for (const auto& [file, rel] : py) {
    const auto& lines = file_lines_cached(file);
    for (std::size_t i = 0; i < lines.size(); ++i) {
      scan_cfg_get(lines[i], cfg, rel, i + 1);
      scan_environ_get(lines[i], env, rel, i + 1);
      scan_quoted(lines[i], {"STRATA_", "MAYA_"}, env, rel, i + 1);
      scan_quoted_flag(lines[i], flags, rel, i + 1);
    }
  }
  std::vector<std::pair<std::filesystem::path, std::string>> src;
  collect_source_files(root / "src", root, src);
  collect_source_files(root / "include", root, src);
  expect(src.size() >= 250U, "the vendored Maya engine trees are present (" + std::to_string(src.size()) + " source files)");
  for (const auto& [file, rel] : src) {
    const auto& lines = file_lines_cached(file);
    for (std::size_t i = 0; i < lines.size(); ++i) scan_quoted(lines[i], {"STRATA_", "DS4_"}, env, rel, i + 1);
  }
  Sites doc_names;
  const auto settings = root / "docs" / "SETTINGS.md";
  expect(std::filesystem::is_regular_file(settings), "the vendored Maya docs/SETTINGS.md is present");
  {
    const auto& lines = file_lines_cached(settings);
    for (std::size_t i = 0; i < lines.size(); ++i) scan_backtick_docs(lines[i], doc_names, "docs/SETTINGS.md", i + 1);
  }
  expect(cfg.size() >= 25U, "the Maya cfg.get scan is plausible (" + std::to_string(cfg.size()) + " keys)");
  expect(env.size() >= 350U, "the Maya env scan is plausible (" + std::to_string(env.size()) + " names)");
  expect(flags.size() >= 50U, "the Maya launcher-flag scan is plausible (" + std::to_string(flags.size()) + " flags)");
  expect(doc_names.size() >= 120U, "the Maya SETTINGS.md name scan is plausible (" + std::to_string(doc_names.size()) + " names)");
  check_sites_dispositioned(cfg, "Maya config key", &maya_row_count_named, &maya_parity_row, root, false);
  check_sites_dispositioned(env, "Maya env knob", &maya_row_count_named, &maya_parity_row, root, true);
  check_sites_dispositioned(doc_names, "Maya documented name", &maya_row_count_named, &maya_parity_row, root, true);
  check_sites_dispositioned(flags, "Maya launcher flag", &maya_row_count_named, &maya_parity_row, root, false);
}

void maya_alias_rows_name_a_target() {
  // A MAYA_* row whose alias target has its own row is an env_alias.hpp alias
  // row; it must name the engine row that carries the behaviour. A MAYA_* row
  // with no target row is a knob read under that literal name (the updater) or
  // launcher-internal, and needs no target.
  for (const auto& row : maya_parity_rows()) {
    if (row.key.rfind("MAYA_", 0) != 0) continue;
    const std::string alias = "STRATA_" + std::string{row.key.substr(5)};
    if (maya_parity_row(alias) == nullptr) continue;
    expect(!row.target.empty(), "the documented-name row '" + std::string{row.key} + "' must name its engine row (" + alias + ")");
    if (row.target.empty()) continue;
    expect(maya_parity_row(row.target) != nullptr,
           "the documented-name row '" + std::string{row.key} + "' targets '" + std::string{row.target} + "', which has no row");
  }
}

void maya_engine_knobs_are_never_mapped_away() {
  // Plan 2026-10-22-sp5-p2 Task 4 (D-7, the owner's full-surface ruling: "we
  // need the same knobs as the project maya has"): every name the engine
  // BINARY itself reads — the fully-quoted STRATA_/DS4_ literals across
  // src/** and include/**, the same scan the inventory was generated from —
  // must be typed or rejected on a security/licence ground, never mapped
  // away. Out of scope is allowed only on named grounds: a DeepSeek DS4-path
  // name (§Non-goals), an env_alias.hpp alias row whose engine target carries
  // the knob (that target must itself not be mapped away), or a name the
  // reference docs never mention — an engine-internal switch, off the parity
  // surface D-7 describes, and unreachable anyway: the closed child-env
  // allowlist takes its names from the typed rows.
  const std::string root_text = scan_root_or_fail();
  if (root_text.empty()) return;
  const std::filesystem::path root{std::filesystem::path{root_text} / "engines" / "maya-ref"};
  std::vector<std::pair<std::filesystem::path, std::string>> src;
  collect_source_files(root / "src", root, src);
  collect_source_files(root / "include", root, src);
  expect(src.size() >= 250U, "the vendored Maya engine trees are present (" + std::to_string(src.size()) + " source files)");
  Sites knobs;
  for (const auto& [file, rel] : src) {
    const auto& lines = file_lines_cached(file);
    for (std::size_t i = 0; i < lines.size(); ++i) scan_quoted(lines[i], {"STRATA_", "DS4_"}, knobs, rel, i + 1);
  }
  expect(knobs.size() >= 250U, "the engine-knob scan is plausible (" + std::to_string(knobs.size()) + " names)");
  // The reference docs' documented spellings: `MAYA_X` backticks and any
  // literal STRATA_X in docs/SETTINGS.md at the pin.
  const std::filesystem::path settings_path = root / "docs" / "SETTINGS.md";
  const auto& settings_lines = file_lines_cached(settings_path);
  expect(settings_lines.size() >= 100U, "the vendored Maya docs/SETTINGS.md is present (" + std::to_string(settings_lines.size()) + " lines)");
  const auto contains_token = [](const std::string& line, const std::string& token) {
    for (std::size_t pos = line.find(token); pos != std::string::npos; pos = line.find(token, pos + 1)) {
      const std::size_t end = pos + token.size();
      if (pos > 0 && is_upper_name_char(line[pos - 1])) continue;
      if (end < line.size() && is_upper_name_char(line[end])) continue;
      return true;
    }
    return false;
  };
  const auto documented = [&](const std::string& name) {
    const std::string maya_alias = name.rfind("STRATA_", 0) == 0 ? "MAYA_" + name.substr(7) : name;
    for (const auto& line : settings_lines) {
      if (contains_token(line, name)) return true;
      if (name.rfind("STRATA_", 0) == 0 && contains_token(line, maya_alias)) return true;
    }
    return false;
  };
  for (const auto& [name, site] : knobs) {
    const auto* row = maya_parity_row(name);
    if (row == nullptr) {
      expect(false, "engine knob '" + name + "' (" + site + ") has no GLM parity row at all");
      continue;
    }
    if (row->disposition == ParityDisposition::mapped_away) {
      expect(false, "engine knob '" + name + "' (" + site + ") is mapped away; D-7 demands typed or rejected");
      continue;
    }
    if (row->disposition == ParityDisposition::out_of_scope) {
      const bool ds4_path = name.rfind("DS4_", 0) == 0 || name.rfind("STRATA_DS4_", 0) == 0;
      const auto* target = row->target.empty() ? nullptr : maya_parity_row(row->target);
      const bool internal = !ds4_path && target == nullptr && !documented(name);
      if (!ds4_path && target == nullptr && !internal)
        expect(false, "engine knob '" + name + "' (" + site + ") is out of scope on no named ground (not DS4, no alias target, documented)");
      if (target != nullptr && target->disposition == ParityDisposition::mapped_away)
        expect(false, "alias row '" + name + "' targets '" + std::string{row->target} + "', which is mapped away");
    }
  }
}

} // namespace

int main(int argc, char** argv) {
  return secure_frontend_test::run_cases(argc, argv, {
      {"every_upstream_key_has_exactly_one_disposition", every_upstream_key_has_exactly_one_disposition},
      {"every_env_knob_is_dispositioned", every_env_knob_is_dispositioned},
      {"typed_rows_name_a_destination", typed_rows_name_a_destination},
      {"every_maya_key_has_exactly_one_disposition", every_maya_key_has_exactly_one_disposition},
      {"every_maya_env_knob_is_dispositioned", every_maya_env_knob_is_dispositioned},
      {"dflash_is_rejected_on_licence", dflash_is_rejected_on_licence},
      {"updater_and_installer_rows_are_rejected", updater_and_installer_rows_are_rejected},
      {"scan_upstream_knobs_need_dispositions_and_current_citations", scan_upstream_knobs_need_dispositions_and_current_citations},
      {"scan_maya_knobs_need_dispositions_and_current_citations", scan_maya_knobs_need_dispositions_and_current_citations},
      {"maya_alias_rows_name_a_target", maya_alias_rows_name_a_target},
      {"maya_engine_knobs_are_never_mapped_away", maya_engine_knobs_are_never_mapped_away},
  }, failures);
}
