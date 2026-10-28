// strata-prepare: what a downloaded model needs before the engine can run it, with no Python (spec
// docs/superpowers/specs/2026-10-27-strata-prepare-design.md).
//
//   strata-prepare pack --gguf <shard 1> --out <pack> [--compat-bf16] [--experts-bin]
//       upstream Strata's tools/iq_pack.py, byte for byte (iq_pack.hpp); its messages on stdout
//   strata-prepare pack --maya --gguf <shard 1> --out <pack> [--compat-bf16] [--experts-bin]
//       Project Maya's fork of it (v1.1.30), the GLM-5.3-Flash pack, byte for byte (maya_pack.hpp)
//   strata-prepare pack --maya-ds4 --model <dir> [--out <dir>] [--template <file>] [--engram <dir>] [--shards <name>...]
//   strata-prepare pack --maya-mm3 --model <dir> [--out <dir>] --template <file> [--indexers <file>] [--shards <name>...]
//       Maya's tools/ds4/pack_gguf.py and tools/mm3/pack_gguf.py, byte for byte (maya_moe_pack.hpp); --template is
//       Maya's chat_template.jinja of that family (DeepSeek without it: the GGUF's own)
//   strata-prepare tokenizer --gguf <gguf> --out <dir>
//       Maya's tools/strata_tokenizer.py: <dir>/tokenizer/ from the GGUF, byte for byte (tokenizer_export.hpp)
//   strata-prepare download --gguf <shard 1> --out <pack> [--pack-arg FLAG ...]
//       the server's prepare step (tools/secure/prepare_download.py's protocol without Python): stdout is one line each,
//       `STEP pack`, `STEP ple`, then `DONE`, or `ERROR <reason>` (bad_arguments, not_gguf, missing_gguf, not_first_shard,
//       missing_shard, pack_failed, pack_incomplete, ple_failed, write_failed) and a non-zero exit; the pack's own
//       messages go to stderr. <pack>/prepared.json = {"pack", "tokenizer": null (the server reads it from the GGUF),
//       "ple", "shards"}.
//   strata-prepare download --maya --gguf <shard 1> --out <pack>
//       Maya's setup step for a GLM download (maya.py pack_step): `STEP pack` (Maya's iq_pack.py --compat-bf16, then its
//       PACK_FILES check - or nothing, for a whole pack of this model whose sources.json still matches the shards:
//       pack_source, pack_stale), `STEP sources` (sources.json: the shards' names, sizes and mtime_ns, maya.py stamp_pack),
//       `DONE`; prepared.json's "tokenizer" is <pack>/tokenizer and "ple" null. The engine reads the GGUF files at
//       <pack>/.., so <pack> is a folder beside them.
//   strata-prepare download --maya --gguf <shard 1> --out <pack> [--expert-profile <file>]
//       for a DeepSeek-V4.1 GGUF (architecture deepseek41), Maya's DeepSeek step instead (maya.py ds4_pack_step):
//       `STEP pack` (pack_gguf.py over the shard set, its `need` files checked - or nothing, for a whole pack these tools
//       built whose shards still match), `STEP sources` (stamp_pack's sources.json with the "tools" that built it), the
//       shipped expert profile copied in as <pack>/expert-profile.bin when the pack has none and it is the pack's size
//       (maya.py ds4_seed_profile), `DONE`. ds4.txt names the model's folder, so <pack> may be anywhere.
//   strata-prepare download --maya --gguf <shard 1> --out <pack> --template <file>
//       for a MiniMax-M3.1 GGUF (architecture minimax-m3; owner 2026-10-28), Maya's MiniMax step (maya.py mm3_pack_step):
//       `STEP pack` (pack_gguf.py --text-only over the shard set and its *-maya-indexers.gguf, with Maya's chat template
//       <file>, its `need` files checked - or nothing, for a whole pack these tools built whose files still match),
//       `STEP sources` (stamp_pack's sources.json over every file, with the "tools"), `DONE`.
#include "gguf_header.hpp"
#include "iq_pack.hpp"
#include "maya_moe_pack.hpp"
#include "maya_pack.hpp"
#include "pack_common.hpp"
#include "py_json.hpp"
#include "tokenizer_export.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <optional>
#include <regex>
#include <string>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
// windows.h first
#include <shellapi.h>
#else
#include <sys/stat.h>
#endif

namespace fs = std::filesystem;
using strata_prepare::Json;
using strata_prepare::path_from_utf8;
using strata_prepare::utf8;

namespace {

constexpr const char* kUsage =
    "usage: strata-prepare pack [--maya] --gguf <shard 1> --out <pack> [--compat-bf16] [--experts-bin]\n"
    "       strata-prepare pack --maya-ds4 --model <dir> [--out <dir>] [--template <file>] [--engram <dir>] [--shards <name>...]\n"
    "       strata-prepare pack --maya-mm3 --model <dir> [--out <dir>] --template <file> [--indexers <file>] [--shards <name>...]\n"
    "       strata-prepare tokenizer --gguf <gguf> --out <dir>\n"
    "       strata-prepare download [--maya] --gguf <shard 1> --out <pack> [--pack-arg FLAG ...] [--expert-profile <file>]\n"
    "                                [--template <file>]\n";

// maya.py's PACK_FILES: what a GLM pack must hold once its builder is done
constexpr std::array<const char*, 7> kMayaPackFiles{"index.txt", "native_experts.txt", "dense.bin", "tokenizer/vocab.json",
                                                    "tokenizer/merges.txt", "tokenizer/token_type.json",
                                                    "tokenizer/chat_template.jinja"};
// maya.py ds4_pack_step's `need`: what a DeepSeek pack must hold once pack_gguf.py is done
constexpr std::array<const char*, 5> kMayaDs4PackFiles{"ds4.txt", "tensors.txt", "engram.bin", "tokenizer/vocab.json",
                                                       "tokenizer/chat_template.jinja"};
// sources.json's "tools" for a DeepSeek pack this program built (maya.py records a hash of its Python pack tools there;
// a pack of other tools is built again, as Maya builds ours again)
constexpr const char* kDs4Tools = "strata-prepare (Maya v1.1.30 tools/ds4/pack_gguf.py)";
// maya.py mm3_pack_step's `need` and its "tools" (owner 2026-10-28: MiniMax-M3.1-Flash's GGUF quants)
constexpr std::array<const char*, 4> kMayaMm3PackFiles{"mm3.txt", "tensors.txt", "tokenizer/vocab.json", "tokenizer/chat_template.jinja"};
constexpr const char* kMm3Tools = "strata-prepare (Maya v1.1.30 tools/mm3/pack_gguf.py)";

struct Refusal {
  std::string reason;
};

// pack --maya-ds4 / --maya-mm3: pack_gguf.py's options (--shards takes the names up to the next option)
int moe_pack_command(const bool ds4, const std::vector<std::string>& args) {
  strata_prepare::MoePackOptions options;
  for (std::size_t i = 1; i < args.size(); ++i) {
    const auto& a = args[i];
    const bool valued = i + 1 < args.size();
    if (a == "--model" && valued) options.model = path_from_utf8(args[++i]);
    else if (a == "--out" && valued) options.out = path_from_utf8(args[++i]);
    else if (a == "--template" && valued) options.chat_template = path_from_utf8(args[++i]);
    else if (a == "--engram" && valued && ds4) options.engram = path_from_utf8(args[++i]);
    else if (a == "--indexers" && valued && !ds4) options.indexers = path_from_utf8(args[++i]);
    else if (a == "--shards" && valued) {
      while (i + 1 < args.size() && args[i + 1].rfind("--", 0) != 0) options.shards.push_back(args[++i]);
    } else {
      std::cerr << kUsage;
      return 2;
    }
  }
  if (options.model.empty()) {
    std::cerr << kUsage;
    return 2;
  }
  return ds4 ? strata_prepare::maya_ds4_pack(options, std::cout) : strata_prepare::maya_mm3_pack(options, std::cout);
}

int pack_command(const std::vector<std::string>& args) {
  if (!args.empty() && (args.front() == "--maya-ds4" || args.front() == "--maya-mm3")) return moe_pack_command(args.front() == "--maya-ds4", args);
  strata_prepare::PackOptions options;
  bool maya = false;
  for (std::size_t i = 0; i < args.size(); ++i) {
    const auto& a = args[i];
    if ((a == "--gguf" || a == "--out") && i + 1 < args.size()) {
      (a == "--gguf" ? options.gguf : options.out) = path_from_utf8(args[++i]);
    } else if (a == "--compat-bf16") {
      options.compat_bf16 = true;
    } else if (a == "--experts-bin") {
      options.experts_bin = true;
    } else if (a == "--maya") {
      maya = true;
    } else {
      std::cerr << kUsage;
      return 2;
    }
  }
  if (options.gguf.empty() || options.out.empty()) {
    std::cerr << kUsage;
    return 2;
  }
  return maya ? strata_prepare::maya_iq_pack(options, std::cout) : strata_prepare::iq_pack(options, std::cout);
}

int tokenizer_command(const std::vector<std::string>& args) {
  fs::path gguf;
  fs::path out;
  for (std::size_t i = 0; i < args.size(); ++i) {
    if ((args[i] == "--gguf" || args[i] == "--out") && i + 1 < args.size()) {
      (args[i] == "--gguf" ? gguf : out) = path_from_utf8(args[i + 1]);
      ++i;
    } else {
      std::cerr << kUsage;
      return 2;
    }
  }
  if (gguf.empty() || out.empty()) {
    std::cerr << kUsage;
    return 2;
  }
  return strata_prepare::export_tokenizer(gguf, out, std::cout);
}

std::vector<fs::path> shard_list(const fs::path& first) {
  std::string extension = utf8(first.extension());
  for (auto& c : extension) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  if (extension != ".gguf") throw Refusal{"not_gguf"};
  if (!fs::is_regular_file(first)) throw Refusal{"missing_gguf"};
  static const std::regex shard(R"((.+)-(\d{5})-of-(\d{5})(\.gguf))", std::regex::icase);
  std::smatch m;
  const std::string name = utf8(first.filename());
  if (!std::regex_match(name, m, shard)) return {first};
  const int n = std::stoi(m[3].str());
  if (std::stoi(m[2].str()) != 1 || n < 1) throw Refusal{"not_first_shard"};
  std::vector<fs::path> shards;
  for (int k = 1; k <= n; ++k) {
    char index[8];
    std::snprintf(index, sizeof(index), "%05d", k);
    shards.push_back(first.parent_path() / path_from_utf8(m[1].str() + "-" + index + "-of-" + m[3].str() + m[4].str()));
  }
  for (const auto& s : shards) {
    if (!fs::is_regular_file(s)) throw Refusal{"missing_shard"};
  }
  return shards;
}

void step(const char* name) { std::cout << "STEP " << name << "\n" << std::flush; }

// os.stat(path).st_mtime_ns, the link followed: Windows' last-write FILETIME (100 ns since 1601) as Python converts it,
// else st_mtim.
std::optional<std::int64_t> mtime_ns(const fs::path& path) {
#if defined(_WIN32)
  HANDLE h = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                         OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
  if (h == INVALID_HANDLE_VALUE) return std::nullopt;
  BY_HANDLE_FILE_INFORMATION info{};
  const BOOL got = GetFileInformationByHandle(h, &info);
  CloseHandle(h);
  if (got == 0) return std::nullopt;
  const std::uint64_t ticks = (static_cast<std::uint64_t>(info.ftLastWriteTime.dwHighDateTime) << 32U) | info.ftLastWriteTime.dwLowDateTime;
  constexpr std::int64_t kSecondsTo1970 = 11644473600LL;
  const auto seconds = static_cast<std::int64_t>(ticks / 10000000U) - kSecondsTo1970;
  return seconds * 1000000000LL + static_cast<std::int64_t>(ticks % 10000000U) * 100;
#else
  struct stat st {};
  if (::stat(path.c_str(), &st) != 0) return std::nullopt;
#if defined(__APPLE__)
  return static_cast<std::int64_t>(st.st_mtimespec.tv_sec) * 1000000000LL + st.st_mtimespec.tv_nsec;
#else
  return static_cast<std::int64_t>(st.st_mtim.tv_sec) * 1000000000LL + st.st_mtim.tv_nsec;
#endif
#endif
}

// maya.py pack_source: the first .gguf a pack indexes (native_experts.txt's header names it), or nullopt (no pack there)
std::optional<std::string> pack_source(const fs::path& pack) {
  std::ifstream f(pack / "native_experts.txt", std::ios::binary);
  std::string line;
  if (!f || !std::getline(f, line)) return std::nullopt;
  static const std::regex offsets(R"(absolute offsets in ([^,)]+))");
  std::smatch m;
  if (!std::regex_search(line, m, offsets)) return std::nullopt;
  std::string name = m[1].str();
  if (!name.empty() && name.back() == '\r') name.pop_back();
  return name;
}

// A JSON string literal's text (json.dumps's escapes: \" \\ \/ \b \f \n \r \t and \uXXXX, surrogate pairs) as UTF-8
std::string json_unescape(const std::string& s) {
  std::string out;
  const auto put = [&out](const unsigned cp) {
    if (cp < 0x80U) {
      out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800U) {
      out.push_back(static_cast<char>(0xC0U | (cp >> 6U)));
      out.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
    } else if (cp < 0x10000U) {
      out.push_back(static_cast<char>(0xE0U | (cp >> 12U)));
      out.push_back(static_cast<char>(0x80U | ((cp >> 6U) & 0x3FU)));
      out.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
    } else {
      out.push_back(static_cast<char>(0xF0U | (cp >> 18U)));
      out.push_back(static_cast<char>(0x80U | ((cp >> 12U) & 0x3FU)));
      out.push_back(static_cast<char>(0x80U | ((cp >> 6U) & 0x3FU)));
      out.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
    }
  };
  for (std::size_t i = 0; i < s.size(); ++i) {
    if (s[i] != '\\' || i + 1 >= s.size()) {
      out.push_back(s[i]);
      continue;
    }
    const char e = s[++i];
    if (e == 'u' && i + 4 < s.size()) {
      unsigned cp = static_cast<unsigned>(std::stoul(s.substr(i + 1, 4), nullptr, 16));
      i += 4;
      if (cp >= 0xD800U && cp < 0xDC00U && i + 6 < s.size() && s[i + 1] == '\\' && s[i + 2] == 'u') {
        const auto low = static_cast<unsigned>(std::stoul(s.substr(i + 3, 4), nullptr, 16));
        cp = 0x10000U + ((cp - 0xD800U) << 10U) + (low - 0xDC00U);
        i += 6;
      }
      put(cp);
    } else {
      out.push_back(e == 'n' ? '\n' : e == 'r' ? '\r' : e == 't' ? '\t' : e == 'b' ? '\b' : e == 'f' ? '\f' : e);
    }
  }
  return out;
}

// maya.py pack_stale: why a pack no longer matches its model files, or nullopt. With sources.json, every shard's size
// and mtime_ns as recorded; without it, a shard written more than a minute after the pack's native_experts.txt.
std::optional<std::string> pack_stale(const fs::path& pack, const std::vector<fs::path>& shards) {
  std::ifstream in(pack / "sources.json", std::ios::binary);
  const std::string text = in ? std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()) : std::string{};
  static const std::regex row(R"x(\{\s*"name":\s*"((?:[^"\\]|\\.)*)",\s*"size":\s*(\d+),\s*"mtime_ns":\s*(-?\d+)\s*\})x");
  std::map<std::string, std::pair<std::uint64_t, std::int64_t>> old;
  for (std::sregex_iterator it(text.begin(), text.end(), row), end; it != end; ++it) {
    old[json_unescape((*it)[1].str())] = {std::stoull((*it)[2].str()), std::stoll((*it)[3].str())};
  }
  std::error_code ec;
  if (!old.empty()) {
    for (const auto& s : shards) {
      const auto found = old.find(utf8(s.filename()));
      const auto ns = mtime_ns(s);
      if (found == old.end() || found->second.first != fs::file_size(s, ec) || !ns || found->second.second != *ns) {
        return utf8(s.filename()) + " changed after the pack was built";
      }
    }
    return std::nullopt;
  }
  const auto built = mtime_ns(pack / "native_experts.txt");
  if (!built) return std::nullopt;
  for (const auto& s : shards) {
    if (const auto ns = mtime_ns(s); ns && *ns > *built + 60LL * 1000000000LL) return utf8(s.filename()) + " was written after the pack was built";
  }
  return std::nullopt;
}

// maya.py stamp_pack: the model files' sizes and times into <pack>/sources.json (failures ignored, as there); a DeepSeek
// pack's step adds the "tools" that built it (ds4_pack_step: rec["tools"] = tools, json.dumps(rec, indent=1))
void stamp_pack(const fs::path& pack, const std::vector<fs::path>& shards, const std::string& tools = {}) {
  try {
    Json files = Json::list();
    for (const auto& s : shards) {
      const auto ns = mtime_ns(s);
      if (!ns) return;
      files.push(Json::object()
                     .set("name", Json::text(utf8(s.filename())))
                     .set("size", Json::integer(static_cast<std::int64_t>(fs::file_size(s))))
                     .set("mtime_ns", Json::integer(*ns)));
    }
    Json record = Json::object().set("files", std::move(files));
    if (!tools.empty()) record.set("tools", Json::text(tools));
    strata_prepare::write_text(pack / "sources.json", record.dumps(1));
  } catch (const std::exception&) {
  }
}

// read_json(pack / "sources.json").get("tools"), or empty
std::string recorded_tools(const fs::path& pack) {
  std::ifstream in(pack / "sources.json", std::ios::binary);
  const std::string text = in ? std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()) : std::string{};
  static const std::regex tools(R"x("tools":\s*"((?:[^"\\]|\\.)*)")x");
  std::smatch m;
  return std::regex_search(text, m, tools) ? json_unescape(m[1].str()) : std::string{};
}

// maya.py ds4_seed_profile: the shipped DeepSeek profile copied into a pack that has none, where the engine reads it and
// saves what it learns - only one of the pack's geometry (its layers x routed experts x 4 bytes); failures ignored
void seed_ds4_profile(const fs::path& profile, const fs::path& pack) {
  std::error_code ec;
  const fs::path dst = pack / "expert-profile.bin";
  if (fs::exists(dst, ec) || !fs::is_regular_file(profile, ec)) return;
  std::ifstream geometry(pack / "ds4.txt", std::ios::binary);
  std::map<std::string, std::string> geo;
  for (std::string line; std::getline(geometry, line);) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    const auto space = line.find(' ');
    geo[line.substr(0, space)] = space == std::string::npos ? std::string{} : line.substr(space + 1U);
  }
  try {
    const auto want = std::stoull(geo.at("num_hidden_layers")) * std::stoull(geo.at("n_routed_experts")) * 4U;
    if (fs::file_size(profile) != want) return;
  } catch (const std::exception&) {
    return;
  }
  fs::copy_file(profile, dst, ec);
}

// A MiniMax set's indexers side file (Maya's quants ship <quant>-maya-indexers.gguf beside the shards): the folder's
// *-maya-indexers.gguf whose maya.indexers.for names the set (general.name with its spaces as dashes, pack_gguf.py's
// check), else the only one there; none: nullopt (the pack refuses the set without it)
std::optional<fs::path> mm3_indexers(const fs::path& first) {
  std::string set_name;
  try {
    const strata_prepare::GgufHeader header(first);
    if (const auto* name = header.get("general.name"); name != nullptr && name->kind == strata_prepare::MetaValue::Kind::text) set_name = name->text;
  } catch (const std::exception&) {
  }
  std::replace(set_name.begin(), set_name.end(), ' ', '-');
  std::vector<fs::path> found;
  std::error_code ec;
  for (const auto& entry : fs::directory_iterator(first.parent_path(), ec)) {
    const std::string name = utf8(entry.path().filename());
    const std::string tail = "-maya-indexers.gguf";
    if (name.size() > tail.size() && name.compare(name.size() - tail.size(), tail.size(), tail) == 0 && entry.is_regular_file(ec)) {
      found.push_back(entry.path());
    }
  }
  std::sort(found.begin(), found.end());
  for (const auto& path : found) {
    try {
      const strata_prepare::GgufHeader header(path);
      const auto* for_ = header.get("maya.indexers.for");
      if (for_ != nullptr && for_->kind == strata_prepare::MetaValue::Kind::text && !set_name.empty() && for_->text == set_name) return path;
    } catch (const std::exception&) {
    }
  }
  if (found.size() == 1U) return found.front();
  return std::nullopt;
}

int download_command(const std::vector<std::string>& args) {
  try {
    std::string gguf;
    std::string out_text;
    std::string profile_text;
    std::string template_text;
    bool maya = false;
    strata_prepare::PackOptions options;
    static const std::regex flag(R"(--[a-z0-9][a-z0-9-]{0,63})");
    for (std::size_t i = 0; i < args.size(); ++i) {
      std::string name = args[i];
      if (name == "--maya") {
        maya = true;
        continue;
      }
      std::string value;
      bool joined = false;
      if (const auto eq = name.find('='); eq != std::string::npos) {
        value = name.substr(eq + 1);
        name = name.substr(0, eq);
        joined = true;
      }
      if (name != "--gguf" && name != "--out" && name != "--pack-arg" && name != "--expert-profile" && name != "--template") {
        throw Refusal{"bad_arguments"};
      }
      if (!joined) {
        if (++i == args.size()) throw Refusal{"bad_arguments"};
        value = args[i];
      }
      if (name == "--pack-arg") {
        if (!std::regex_match(value, flag)) throw Refusal{"bad_arguments"};
        if (value == "--compat-bf16") options.compat_bf16 = true;
        else if (value == "--experts-bin") options.experts_bin = true;
        else throw Refusal{"bad_arguments"};
      } else {
        auto& target = name == "--gguf" ? gguf : name == "--out" ? out_text : name == "--template" ? template_text : profile_text;
        if (!target.empty() || value.empty()) throw Refusal{"bad_arguments"};
        target = value;
      }
    }
    // Maya's step passes its builder exactly --compat-bf16
    if (gguf.empty() || out_text.empty() || (maya && (options.compat_bf16 || options.experts_bin)) ||
        (!maya && (!profile_text.empty() || !template_text.empty()))) {
      throw Refusal{"bad_arguments"};
    }
    const fs::path first = fs::absolute(path_from_utf8(gguf)).make_preferred();
    const fs::path out = fs::absolute(path_from_utf8(out_text)).make_preferred();
    auto shards = shard_list(first);
    // the family Maya's setup follows: DeepSeek-V4.1's own step for its GGUF (maya.py ds4_pack_step), MiniMax-M3.1's
    // (mm3_pack_step), else GLM's
    bool deepseek = false;
    bool minimax = false;
    if (maya) {
      try {
        const strata_prepare::GgufHeader header(first);
        const auto* arch = header.get("general.architecture");
        const bool text = arch != nullptr && arch->kind == strata_prepare::MetaValue::Kind::text;
        deepseek = text && arch->text == "deepseek41";
        minimax = text && arch->text == "minimax-m3";
      } catch (const std::exception&) {
        throw Refusal{"not_gguf"};
      }
    }
    if (!profile_text.empty() && !deepseek) throw Refusal{"bad_arguments"};  // (the DeepSeek step's seed alone)
    if (minimax == template_text.empty()) throw Refusal{"bad_arguments"};    // (Maya's MiniMax template: that step's alone, and it needs it)
    const fs::path record = out / "prepared.json";
    std::error_code ec;
    fs::create_directories(out, ec);
    fs::remove(record, ec);
    if (!fs::is_directory(out)) throw Refusal{"write_failed"};
    step("pack");
    options.gguf = first;
    options.out = out;
    Json ple = Json::null_value();
    Json tokenizer = Json::null_value();
    if (deepseek) {
      // maya.py ds4_pack_step: a pack these tools built, whole, whose shards still match is used as it is
      const auto whole = [&out] {
        for (const char* file : kMayaDs4PackFiles) {
          if (!fs::is_regular_file(out / path_from_utf8(file))) return false;
        }
        return true;
      };
      bool built = false;
      if (recorded_tools(out) == kDs4Tools && whole()) {
        if (const auto why = pack_stale(out, shards)) {
          std::cerr << "the pack is built again: " << *why << "\n";
        } else {
          std::cerr << "pack already built: " << utf8(out) << "\n";
          built = true;
        }
      } else if (fs::exists(out / "ds4.txt", ec)) {
        std::cerr << "the pack is built again: other tools built it\n";
      }
      if (!built) {
        strata_prepare::MoePackOptions moe;
        moe.model = first.parent_path();
        moe.out = out;
        for (const auto& s : shards) moe.shards.push_back(utf8(s.filename()));
        if (strata_prepare::maya_ds4_pack(moe, std::cerr) != 0) throw Refusal{"pack_failed"};
        if (!whole()) throw Refusal{"pack_incomplete"};
      }
      step("sources");
      if (!built) stamp_pack(out, shards, kDs4Tools);
      if (!profile_text.empty()) seed_ds4_profile(fs::absolute(path_from_utf8(profile_text)), out);
      tokenizer = Json::text(utf8(out / "tokenizer"));
    } else if (minimax) {
      // maya.py mm3_pack_step for a GGUF quant: pack_gguf.py --text-only over the shards and the indexers file, reused
      // while a pack of these tools is whole and its files unchanged; sources.json covers every file
      if (const auto side = mm3_indexers(first)) shards.push_back(*side);
      const auto whole = [&out] {
        for (const char* file : kMayaMm3PackFiles) {
          if (!fs::is_regular_file(out / path_from_utf8(file))) return false;
        }
        return true;
      };
      bool built = false;
      if (recorded_tools(out) == kMm3Tools && whole()) {
        if (const auto why = pack_stale(out, shards)) {
          std::cerr << "the pack is built again: " << *why << "\n";
        } else {
          std::cerr << "pack already built: " << utf8(out) << "\n";
          built = true;
        }
      } else if (fs::exists(out / "mm3.txt", ec)) {
        std::cerr << "the pack is built again: other tools built it\n";
      }
      if (!built) {
        strata_prepare::MoePackOptions moe;
        moe.model = first.parent_path();
        moe.out = out;
        moe.chat_template = fs::absolute(path_from_utf8(template_text));
        for (const auto& s : shards) moe.shards.push_back(utf8(s.filename()));
        if (strata_prepare::maya_mm3_pack(moe, std::cerr) != 0) throw Refusal{"pack_failed"};
        if (!whole()) throw Refusal{"pack_incomplete"};
      }
      step("sources");
      if (!built) stamp_pack(out, shards, kMm3Tools);
      tokenizer = Json::text(utf8(out / "tokenizer"));
    } else if (maya) {
      // maya.py pack_step: a pack of this model, whole, that still matches its files is used as it is
      const auto whole = [&out] {
        for (const char* file : kMayaPackFiles) {
          if (!fs::is_regular_file(out / path_from_utf8(file))) return false;
        }
        return true;
      };
      bool built = false;
      if (pack_source(out) == utf8(first.filename()) && whole()) {
        if (const auto why = pack_stale(out, shards)) {
          std::cerr << "the pack is built again: " << *why << "\n";
        } else {
          std::cerr << "pack already built: " << utf8(out) << "\n";
          built = true;
        }
      }
      if (!built) {
        options.compat_bf16 = true;
        if (strata_prepare::maya_iq_pack(options, std::cerr) != 0) throw Refusal{"pack_failed"};
        if (!whole()) throw Refusal{"pack_incomplete"};
      }
      step("sources");
      if (!built || !fs::exists(out / "sources.json")) stamp_pack(out, shards);
      tokenizer = Json::text(utf8(out / "tokenizer"));
    } else {
      if (strata_prepare::iq_pack(options, std::cerr) != 0) throw Refusal{"pack_failed"};
      step("ple");
      try {
        for (const auto& s : shards) {
          const strata_prepare::GgufHeader header(s);
          bool found = false;
          for (const auto& t : header.tensors) found = found || t.name == "per_layer_token_embd.weight";
          if (found) {
            ple = Json::text(utf8(s));
            break;
          }
        }
      } catch (const std::exception&) {
        throw Refusal{"ple_failed"};
      }
    }
    const std::string data = Json::object()
                                 .set("pack", Json::text(utf8(out)))
                                 .set("tokenizer", tokenizer)
                                 .set("ple", ple)
                                 .set("shards", Json::integer(static_cast<std::int64_t>(shards.size())))
                                 .dumps(2) +
                             "\n";
    const fs::path tmp = out / "prepared.json.tmp";
    {
      std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
      f.write(data.data(), static_cast<std::streamsize>(data.size()));
      if (!f) throw Refusal{"write_failed"};
    }
    fs::rename(tmp, record, ec);
    if (ec) throw Refusal{"write_failed"};
  } catch (const Refusal& r) {
    std::cout << "ERROR " << r.reason << "\n" << std::flush;
    return r.reason == "bad_arguments" ? 2 : 1;
  }
  std::cout << "DONE\n" << std::flush;
  return 0;
}

}  // namespace

static int run(std::vector<std::string> args) {
  if (args.empty()) {
    std::cerr << kUsage;
    return 2;
  }
  const std::string verb = args.front();
  args.erase(args.begin());
  if (verb == "pack") return pack_command(args);
  if (verb == "tokenizer") return tokenizer_command(args);
  if (verb == "download") return download_command(args);
  std::cerr << kUsage;
  return 2;
}

// The arguments as UTF-8: on Windows the wide command line, converted (the ANSI argv cannot hold every path; wmain is
// not portable to MinGW), else argv as it is.
int main(int argc, char** argv) {
#if defined(_WIN32)
  static_cast<void>(argc);
  static_cast<void>(argv);
  int n_wide = 0;
  wchar_t** wide = CommandLineToArgvW(GetCommandLineW(), &n_wide);
  if (wide == nullptr) return 2;
  std::vector<std::string> args;
  for (int i = 1; i < n_wide; ++i) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, nullptr, 0, nullptr, nullptr);
    std::string a(n > 1 ? static_cast<std::size_t>(n - 1) : 0U, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, wide[i], -1, a.data(), n, nullptr, nullptr);
    args.push_back(std::move(a));
  }
  LocalFree(wide);
  return run(std::move(args));
#else
  return run(std::vector<std::string>(argv + 1, argv + argc));
#endif
}
