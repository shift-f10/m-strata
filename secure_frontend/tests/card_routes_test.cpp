// GUI-first start-up (docs/superpowers/specs/2026-10-27-gui-first-install-design.md §4): the page's card and folder
// routes over a real launcher - this PC's folders, a card added from a GGUF file, a folder made private, a card that
// follows its moved folder, and the danger zone's delete behind its three checks.
#include "secure_frontend/canonical_path.hpp"
#include "secure_frontend/card_routes.hpp"
#include "secure_frontend/config_file.hpp"
#include "secure_frontend/first_start.hpp"
#include "secure_frontend/json.hpp"
#include "secure_frontend/model_cards.hpp"
#include "support/gguf_builder.hpp"
#include "support/test_paths.hpp"
#include "support/test_support.hpp"
#include "support/test_tokenizer.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <variant>

namespace {

int failures = 0;
void expect(const bool condition, const std::string& message) {
  if (!condition) {
    ++failures;
    std::cerr << "FAILED: " << message << '\n';
  }
}

constexpr std::string_view kKey = "card-routes-key-0123456789abcdefghijklmnop";
const std::string kModelFile = "tiny-gemma-Q8_0.gguf";

std::string json_text(const std::string_view text) {
  std::string out;
  secure_frontend::append_json_string(&out, text);
  return out;
}

std::string json_path(const std::filesystem::path& path) { return json_text(secure_frontend::path_to_utf8(path)); }

bool has(const secure_frontend::HttpResponse& response, const std::string_view text) {
  return response.body.find(text) != std::string::npos;
}

std::string shown(const secure_frontend::HttpResponse& response) { return std::to_string(response.status) + " " + response.body; }

// A first start's data folder, a private models folder with a small Gemma 4 GGUF, the fake engine as the default
// engine, and a launcher holding one operator card ("Op") and the routes over it.
struct Fixture final {
  std::filesystem::path root = secure_frontend_test::make_scratch_directory();
  std::filesystem::path models = root / "models";
  std::filesystem::path engine;
  secure_frontend::DataFolder data = secure_frontend::data_folder_layout(root / "data");
  secure_frontend::FrontendConfigDraft main;
  std::unique_ptr<secure_frontend::ModelLauncher> launcher;
  std::unique_ptr<secure_frontend::CardRoutes> routes;
  std::vector<std::string> removed_downloads;   // the downloads the routes asked to take out of their store
  std::optional<std::string> remove_failure;    // what that answers (nullopt: removed)
  std::vector<std::vector<std::string>> prepared;   // each strata-prepare command line the routes ran
  std::function<void(const std::vector<std::string>&)> on_prepare;   // what that child writes

  // `shared_folders`: the main config's roots.allow_untrusted_media (the owner's "use folders as they are");
  // `with_config`: the routes know the config file (the Model folders setting can be saved).
  explicit Fixture(const bool keyed = true, std::function<bool(const secure_frontend::RequestContext&)> client_allowed = {},
                   const bool with_prepare = true, const bool shared_folders = false, const bool with_config = true) {
    secure_frontend_test::make_private_directory(models);
    write_gguf(models / kModelFile, "gemma4");
    secure_frontend_test::write_file(models / "notes.txt", "mine");
    const auto bin = root / "bin";
    secure_frontend_test::make_private_directory(bin);
    const std::filesystem::path fake{STRATA_FAKE_ENGINE_PATH};
    engine = bin / fake.filename();
    std::error_code ec;
    std::filesystem::copy_file(fake, engine, ec);
    (void)secure_frontend::ensure_data_folder(data, engine);
    main = std::get<secure_frontend::FrontendConfigDraft>(
        secure_frontend::parse_config_text(secure_frontend_test::read_file(data.config_file), data.root));
    if (shared_folders) {
      main.entries["roots.allow_untrusted_media"] = secure_frontend::ConfigDraftEntry{true, secure_frontend::ConfigSource::file};
    }
    secure_frontend::ModelCard op;
    op.name = "Op";
    launcher = std::make_unique<secure_frontend::ModelLauncher>(secure_frontend::LauncherSettings{
        .main = main, .cards = {op}, .operator_cards = 1U, .memory_file = {}, .gate = {}, .log = [](std::string_view) {}});
    secure_frontend_test::make_private_directory(data.state / "cards");
    secure_frontend::CardRouteSettings settings{.cards_dir = data.state / "cards", .engine = engine, .main = main};
    if (keyed) settings.key_matches = [](const std::string_view key) { return key == kKey; };
    if (with_config) settings.config_file = data.config_file;
    settings.client_allowed = std::move(client_allowed);
    settings.remove_download = [this](const std::string_view id) {
      removed_downloads.emplace_back(id);
      return remove_failure;
    };
    if (with_prepare) {
      settings.packs_dir = data.state / "packs";
      settings.prepare = [this](const std::vector<std::string>& arguments) -> std::optional<std::string> {
        prepared.push_back(arguments);
        if (on_prepare) on_prepare(arguments);
        return std::nullopt;
      };
    }
    settings.log = [](std::string_view) {};
    routes = std::make_unique<secure_frontend::CardRoutes>(*launcher, std::move(settings));
  }
  ~Fixture() { launcher->shutdown(); }
  Fixture(const Fixture&) = delete;
  Fixture& operator=(const Fixture&) = delete;

  // A folder other users of this PC may change (what a shared drive's folders often are).
  static void widen(const std::filesystem::path& folder) {
#if defined(_WIN32)
    expect(secure_frontend_test::grant_everyone_write(folder), "the test widens a folder");
#else
    ::chmod(folder.c_str(), 0777);
#endif
  }
  static bool is_private(const std::filesystem::path& folder) {
    return std::holds_alternative<secure_frontend::CanonicalRoot>(secure_frontend::open_root(folder, secure_frontend::RootPolicy::existing));
  }
  static void write_gguf(const std::filesystem::path& file, const std::string& arch) {
    secure_frontend_test::write_file(
        file, test_support::gguf_with_tokenizer(test_support::spm_tokenizer(), "gemma4", arch).u32(arch + ".context_length", 32768U).bytes());
  }
  secure_frontend::HttpResponse post(const secure_frontend::RouteId route, const std::string& body, const bool loopback = true) {
    return routes->answer(route, body, secure_frontend::RequestContext{.client_gone = {}, .loopback_peer = loopback});
  }
  // A remote client at an IPv4 address a.b.c.d (the accepted socket's peer).
  secure_frontend::HttpResponse post_from(const secure_frontend::RouteId route, const std::string& body,
                                          const std::array<unsigned char, 4> address) {
    secure_frontend::RequestContext context{.client_gone = {}, .loopback_peer = false};
    context.peer[0] = 4U;
    std::copy(address.begin(), address.end(), context.peer.begin() + 1);
    return routes->answer(route, body, context);
  }
  secure_frontend::HttpResponse add(const std::filesystem::path& folder, const std::string& file) {
    return post(secure_frontend::RouteId::strata_card_add, "{\"folder\":" + json_path(folder) + ",\"file\":" + json_text(file) + "}");
  }
  std::string card_folder(const std::string& name) const {
    const auto held = launcher->card(name);
    if (!held) return {};
    const auto found = held->card.config.entries.find("roots.models");
    return found == held->card.config.entries.end() ? std::string{} : std::get<std::string>(found->second.value);
  }
};

// D4: paths and deletes stay on this PC - a remote client is refused on every route, before anything is read.
void card_routes_answer_loopback_clients_only() {
  Fixture f;
  for (const auto& entry : secure_frontend::kModelCardRoutes) {
    const auto answer = f.post(entry.id, "{}", false);
    expect(answer.status == 403 && has(answer, "\"code\":\"control_not_allowed\""), std::string{entry.path} + " refuses a remote client");
  }
}

void folders_list_the_drives_and_one_folder() {
  Fixture f;
  const auto drives = f.post(secure_frontend::RouteId::strata_folders, "{}");
  expect(drives.status == 200 && has(drives, "\"volumes\":[{\"path\":") && has(drives, "\"free_bytes\":"), "the drives: " + shown(drives));
  const auto folder = f.post(secure_frontend::RouteId::strata_folders, "{\"path\":" + json_path(f.models) + "}");
  expect(folder.status == 200 && has(folder, "{\"name\":\"" + kModelFile + "\",\"folder\":false,\"bytes\":") &&
             !has(folder, "notes.txt") && has(folder, "\"parent\":"),
         "a folder's GGUF files: " + shown(folder));
  const auto bad = f.post(secure_frontend::RouteId::strata_folders, "{\"path\":\"relative/x\"}");
  expect(bad.status == 422 && has(bad, "\"code\":\"folder_refused\"") && !has(bad, "relative"), "a bad path, not echoed: " + shown(bad));
  const auto junk = f.post(secure_frontend::RouteId::strata_folders, "[]");
  expect(junk.status == 400, "a body that is not an object: " + shown(junk));
}

void a_card_is_added_from_a_folder() {
  Fixture f;
  const auto added = f.add(f.models, kModelFile);
  expect(added.status == 201 && has(added, "\"name\":\"tiny-gemma-Q8_0\"") && has(added, "\"profile\":\"gemma4\""),
         "the card is added: " + shown(added));
  const auto held = f.launcher->card("tiny-gemma-Q8_0");
  expect(held && held->page_card && held->card.source == "folder" && f.card_folder("tiny-gemma-Q8_0") == secure_frontend::path_to_utf8(f.models),
         "the launcher holds it, from that folder");
  const auto file = f.data.state / "cards" / "tiny-gemma-Q8_0.json";
  expect(std::holds_alternative<secure_frontend::ModelCard>(secure_frontend::read_model_card(file)), "its card file is written and reads");
  const auto again = f.add(f.models, kModelFile);
  expect(again.status == 409 && has(again, "\"code\":\"card_exists\""), "a second card of that name: " + shown(again));
  const auto notes = f.add(f.models, "notes.txt");
  expect(notes.status == 422 && has(notes, "\"code\":\"card_refused\""), "not a GGUF file: " + shown(notes));
  const auto missing = f.add(f.models, "absent.gguf");
  expect(missing.status == 422 && has(missing, "\"code\":\"card_refused\""), "no such file: " + shown(missing));
  const auto escape = f.add(f.models, "../models/" + kModelFile);
  expect(escape.status == 422, "a file name, not a path: " + shown(escape));
  Fixture::write_gguf(f.models / "llama-7b.gguf", "llama");
  const auto llama = f.add(f.models, "llama-7b.gguf");
  expect(llama.status == 422 && has(llama, "llama") && has(llama, "\"code\":\"card_refused\""), "another family, by name: " + shown(llama));

  // a folder other users may write is not trusted: the page offers to make it private
  const auto shared = f.root / "shared";
  secure_frontend_test::make_private_directory(shared);
  Fixture::write_gguf(shared / "other-gemma.gguf", "gemma4");
#if defined(_WIN32)
  expect(secure_frontend_test::grant_everyone_write(shared), "the test widens a folder");
#else
  ::chmod(shared.c_str(), 0777);
#endif
  const auto open = f.add(shared, "other-gemma.gguf");
  expect(open.status == 409 && has(open, "\"code\":\"folder_not_private\""), "a shared folder: " + shown(open));
  const auto made = f.post(secure_frontend::RouteId::strata_folder_private, "{\"path\":" + json_path(shared) + "}");
  expect(made.status == 200 && has(made, "\"private\":true"), "it is made private: " + shown(made));
  expect(f.add(shared, "other-gemma.gguf").status == 201, "then it is added");
  const auto root = f.post(secure_frontend::RouteId::strata_folder_private, "{\"path\":" + json_path(f.root.root_path()) + "}");
  expect(root.status == 422 && has(root, "\"code\":\"folder_refused\""), "a drive's root is never changed: " + shown(root));
}

// Vision for cards (spec 2026-10-28 point 3): a Qwen model added from a folder that holds its picture encoder (one
// mmproj*.gguf beside it) gets a card that reads pictures - vision.mmproj names the file, the Pictures option is listed;
// a folder without one gives a text-only card.
void a_qwen_model_beside_its_encoder_reads_pictures() {
  Fixture f;
  const auto folder = f.root / "qwen36";
  secure_frontend_test::make_private_directory(folder);
  const auto qwen36 = test_support::gguf_with_tokenizer(test_support::byte_tokenizer(), "gpt2", "qwen35moe")
                          .u32("qwen35moe.context_length", 262144U)
                          .bytes();
  secure_frontend_test::write_file(folder / "Qwen3.6-Test-Q8_0.gguf", qwen36);
  const auto plain = f.add(folder, "Qwen3.6-Test-Q8_0.gguf");
  expect(plain.status == 201, "a Qwen3.6 model is added: " + shown(plain));
  const auto encoder_of = [&f](const std::string& name) {
    std::pair<std::string, bool> out;
    const auto held = f.launcher->card(name);
    if (!held) return out;
    if (const auto row = held->card.config.entries.find("vision.mmproj"); row != held->card.config.entries.end()) {
      out.first = std::get<std::string>(row->second.value);
    }
    for (const auto& limit : held->card.options) out.second = out.second || limit.spec->name == "vision";
    return out;
  };
  expect(encoder_of("Qwen3.6-Test-Q8_0") == std::pair<std::string, bool>{}, "no encoder beside it: a text-only card");
  const auto seer = f.root / "seer";
  secure_frontend_test::make_private_directory(seer);
  secure_frontend_test::write_file(seer / "Seer-Q8_0.gguf", qwen36);
  secure_frontend_test::write_file(seer / "mmproj-F16.gguf", "GGUF");
  const auto added = f.add(seer, "Seer-Q8_0.gguf");
  expect(added.status == 201, "a Qwen3.6 model beside its encoder is added: " + shown(added));
  const auto [row, listed] = encoder_of("Seer-Q8_0");
  expect(row == secure_frontend::path_to_utf8(std::filesystem::canonical(seer) / "mmproj-F16.gguf") && listed,
         "its card names the encoder and lists Pictures: " + row);
}

// Owner 2026-10-28 ("gemma4 has an mtp ... make sure we work that mtp in our gemma engine"): a Gemma 4 model added from
// a folder that holds its MTP draft file (unsloth's MTP/mtp-*.gguf, or an mtp-*.gguf beside it) gets a card that drafts
// with it - engine.args --mtp <the file>, the MTP option; one in a folder other users may change is not used (the card
// runs without it) unless model folders are used as they are.
void a_gemma4_model_beside_its_mtp_file_drafts_with_it() {
  const auto args_of = [](Fixture& f, const std::string& name) {
    const auto held = f.launcher->card(name);
    if (!held) return std::string{"<no card>"};
    std::string all;
    const auto found = held->card.config.entries.find("engine.args");
    if (found != held->card.config.entries.end()) {
      for (const auto& a : std::get<std::vector<std::string>>(found->second.value)) all += " " + a;
    }
    for (const auto& limit : held->card.options) {
      if (limit.spec->name == "mtp") all += " option:mtp";
    }
    return all;
  };
  const auto gemma_folder = [](Fixture& f, const std::string& name) {
    const auto folder = f.root / name;
    secure_frontend_test::make_private_directory(folder);
    secure_frontend_test::make_private_directory(folder / "MTP");
    Fixture::write_gguf(folder / (name + "-Q8_0.gguf"), "gemma4");
    secure_frontend_test::write_file(folder / "MTP" / ("mtp-" + name + "-Q8_0.gguf"), "GGUF");
    return folder;
  };
  Fixture f;
  const auto plain = f.add(f.models, kModelFile);
  expect(plain.status == 201 && args_of(f, "tiny-gemma-Q8_0").empty(), "no MTP file beside it: no --mtp: " + args_of(f, "tiny-gemma-Q8_0"));
  const auto folder = gemma_folder(f, "gemma-mtp");
  const auto added = f.add(folder, "gemma-mtp-Q8_0.gguf");
  expect(added.status == 201, "a Gemma 4 model beside its MTP file is added: " + shown(added));
  const std::string mtp = secure_frontend::path_to_utf8(std::filesystem::canonical(folder) / "MTP" / "mtp-gemma-mtp-Q8_0.gguf");
  expect(args_of(f, "gemma-mtp-Q8_0") == " --mtp " + mtp + " option:mtp", "its card drafts with it: " + args_of(f, "gemma-mtp-Q8_0"));
  const auto open = gemma_folder(f, "gemma-open");
  Fixture::widen(open / "MTP");
  const auto shared = f.add(open, "gemma-open-Q8_0.gguf");
  expect(shared.status == 201 && args_of(f, "gemma-open-Q8_0").empty(),
         "an MTP file other users may change is not used: " + shown(shared) + args_of(f, "gemma-open-Q8_0"));
  Fixture as_they_are(true, {}, true, true);
  const auto open_too = gemma_folder(as_they_are, "gemma-open");
  Fixture::widen(open_too / "MTP");
  const auto used = as_they_are.add(open_too, "gemma-open-Q8_0.gguf");
  expect(used.status == 201 && args_of(as_they_are, "gemma-open-Q8_0").find(" --mtp ") == 0U,
         "with model folders used as they are, it is used: " + shown(used) + args_of(as_they_are, "gemma-open-Q8_0"));
}

std::string value_after(const std::vector<std::string>& arguments, const std::string& flag) {
  const auto at = std::find(arguments.begin(), arguments.end(), flag);
  return at == arguments.end() || at + 1 == arguments.end() ? std::string{} : *(at + 1);
}

// Owner 2026-10-27 ("Then work on the add model"): a Qwen3.8 model (qwen4exp) or a GLM-5.3 one (glm5-next) added
// from its folder has its pack built first by strata-prepare - the Qwen pack in the server's own packs folder (the
// model folder is left as it is), the GLM pack beside the shards in pack/ (where Maya's engine reads them, as Maya's
// setup puts it), or pack-<name>/ when pack/ holds another model's - then its card, as a download's.
// Owner 2026-10-28 ("I don't see MTP settings in the cards"): a Qwen3.8 model added from its folder runs the MTP draft
// layer upstream's setup made (its data folder's mtp/rt: dense.bin, experts.bin, draft_vocab.bin) when the model folder,
// its parent or its grandparent holds one that is private; an incomplete one is not used.
void a_qwen38_model_finds_its_draft_layer() {
  const auto added_args = [](const bool complete) {
    Fixture f;
    const auto profiles = f.engine.parent_path() / "profiles";
    secure_frontend_test::make_private_directory(profiles);
    secure_frontend_test::write_file(profiles / "expert-profile.bin", "profile");
    const auto rt = f.root / "mtp" / "rt";
    secure_frontend_test::make_private_directory(f.root / "mtp");
    secure_frontend_test::make_private_directory(rt);
    for (const char* file : {"dense.bin", "experts.bin", "draft_vocab.bin"}) {
      if (complete || std::string_view{file} != "draft_vocab.bin") secure_frontend_test::write_file(rt / file, "x");
    }
    const std::string first = "Qwen3.8-Test-IQ3_S-00001-of-00002.gguf";
    secure_frontend_test::write_file(f.models / first, test_support::gguf_with_tokenizer(test_support::byte_tokenizer(), "gpt2", "qwen4exp")
                                                           .u32("qwen4exp.expert_count", 512U)
                                                           .bytes());
    secure_frontend_test::write_file(f.models / "Qwen3.8-Test-IQ3_S-00002-of-00002.gguf", "shard two");
    f.on_prepare = [&f](const std::vector<std::string>& a) {
      const auto out = secure_frontend::path_from_utf8(value_after(a, "--out"));
      std::filesystem::create_directories(out / "tokenizer");
      secure_frontend_test::write_file(out / "native_experts.txt", "# strata native experts v3: ... absolute offsets in Qwen3.8-Test-IQ3_S-00001-of-00002.gguf\n");
      secure_frontend_test::write_file(out / "prepared.json", "{\"pack\": " + json_path(out) + ", \"tokenizer\": null, \"ple\": " +
                                                                  json_path(f.models / "Qwen3.8-Test-IQ3_S-00002-of-00002.gguf") + ", \"shards\": 2}\n");
    };
    const auto added = f.add(f.models, first);
    expect(added.status == 201, "the Qwen3.8 model is added: " + shown(added));
    const auto held = f.launcher->card("Qwen3.8-Test-IQ3_S");
    std::string all;
    if (held) {
      const auto found = held->card.config.entries.find("engine.args");
      if (found != held->card.config.entries.end()) {
        for (const auto& a : std::get<std::vector<std::string>>(found->second.value)) all += " " + a;
      }
      for (const auto& limit : held->card.options) all += " option:" + std::string{limit.spec->name};
    }
    return std::pair{all, secure_frontend::path_to_utf8(rt)};
  };
  const auto [complete, rt] = added_args(true);
  expect(complete.find(" --mtp " + rt) != std::string::npos && complete.find(" option:mtp") != std::string::npos &&
             complete.find(" option:spec_min_p") != std::string::npos,
         "upstream's mtp/rt beside the models folder: --mtp and the MTP settings:" + complete);
  const auto [partial, unused] = added_args(false);
  expect(partial.find("--mtp") == std::string::npos && partial.find(" option:mtp") == std::string::npos,
         "an incomplete draft layer is not used:" + partial);
}

void qwen38_and_glm_models_are_added_with_their_packs() {
  Fixture f;
  const auto profiles = f.engine.parent_path() / "profiles";
  secure_frontend_test::make_private_directory(profiles);
  secure_frontend_test::write_file(profiles / "expert-profile.bin", "profile");
  const std::string first = "Qwen3.8-Test-IQ3_S-00001-of-00002.gguf";
  secure_frontend_test::write_file(f.models / first, test_support::gguf_with_tokenizer(test_support::byte_tokenizer(), "gpt2", "qwen4exp")
                                                         .u32("qwen4exp.expert_count", 512U)
                                                         .bytes());
  secure_frontend_test::write_file(f.models / "Qwen3.8-Test-IQ3_S-00002-of-00002.gguf", "shard two");
  f.on_prepare = [&f](const std::vector<std::string>& a) {
    const auto out = secure_frontend::path_from_utf8(value_after(a, "--out"));
    std::filesystem::create_directories(out / "tokenizer");
    const bool maya = std::find(a.begin(), a.end(), "--maya") != a.end();
    secure_frontend_test::write_file(out / "native_experts.txt", "# strata native experts v3: ... absolute offsets in " +
                                                                     secure_frontend::path_to_utf8(secure_frontend::path_from_utf8(value_after(a, "--gguf")).filename()) +
                                                                     ", or in the named shard(s) beside it)\n");
    secure_frontend_test::write_file(out / "prepared.json",
                                     "{\"pack\": " + json_path(out) + ", \"tokenizer\": " + (maya ? json_path(out / "tokenizer") : "null") +
                                         ", \"ple\": " + (maya ? "null" : json_path(f.models / "Qwen3.8-Test-IQ3_S-00002-of-00002.gguf")) +
                                         ", \"shards\": 2}\n");
  };
  const auto second = f.add(f.models, "Qwen3.8-Test-IQ3_S-00002-of-00002.gguf");
  expect(second.status == 422 && has(second, "first") && f.prepared.empty(), "a later shard is refused before anything runs: " + shown(second));
  const auto qwen = f.add(f.models, first);
  expect(qwen.status == 201 && has(qwen, "\"name\":\"Qwen3.8-Test-IQ3_S\"") && has(qwen, "\"profile\":\"qwen\""), "a Qwen3.8 model is added: " + shown(qwen));
  const auto pack = f.data.state / "packs" / "Qwen3.8-Test-IQ3_S";
  expect(f.prepared.size() == 1U && f.prepared[0] == std::vector<std::string>{"download", "--gguf", secure_frontend::path_to_utf8(f.models / first),
                                                                              "--out", secure_frontend::path_to_utf8(pack), "--pack-arg=--compat-bf16"},
         "strata-prepare download: the first shard, the pack in the server's packs folder, --compat-bf16 (any quant packs)");
  const auto held = f.launcher->card("Qwen3.8-Test-IQ3_S");
  const auto args_of = [](const secure_frontend::ModelCard& card) {
    const auto found = card.config.entries.find("engine.args");
    return found == card.config.entries.end() ? std::vector<std::string>{} : std::get<std::vector<std::string>>(found->second.value);
  };
  expect(held && held->card.source == "folder" && f.card_folder("Qwen3.8-Test-IQ3_S") == secure_frontend::path_to_utf8(f.models) &&
             args_of(held->card) == std::vector<std::string>{"--pack", secure_frontend::path_to_utf8(pack), "--ple-gguf",
                                                              secure_frontend::path_to_utf8(f.models / "Qwen3.8-Test-IQ3_S-00002-of-00002.gguf"),
                                                              "--expert-profile", secure_frontend::path_to_utf8(profiles / "expert-profile.bin")},
         "the card: the model folder, the pack, the PLE shard, the bundled profile for 512 experts: " +
             (held ? [&] {
               std::string all = f.card_folder("Qwen3.8-Test-IQ3_S") + " |";
               for (const auto& a : args_of(held->card)) all += " " + a;
               return all;
             }()
                   : std::string{"no card"}));

  // GLM-5.3: the pack beside the shards
  const auto glm = f.root / "glm";
  secure_frontend_test::make_private_directory(glm);
  const std::string glm_first = "GLM-Test-00001-of-00002.gguf";
  secure_frontend_test::write_file(glm / glm_first, test_support::gguf_with_tokenizer(test_support::byte_tokenizer(), "gpt2", "glm5-next").bytes());
  secure_frontend_test::write_file(glm / "GLM-Test-00002-of-00002.gguf", "shard two");
  const auto added = f.add(glm, glm_first);
  expect(added.status == 201 && has(added, "\"name\":\"GLM-Test\"") && has(added, "\"profile\":\"glm53\""), "a GLM model is added: " + shown(added));
  expect(f.prepared.size() == 2U && f.prepared[1] == std::vector<std::string>{"download", "--maya", "--gguf", secure_frontend::path_to_utf8(glm / glm_first),
                                                                              "--out", secure_frontend::path_to_utf8(glm / "pack")},
         "strata-prepare download --maya: the pack in pack/ beside the shards");
  expect(std::filesystem::exists(glm / "pack" / "glm-5.3-flash.gguf"), "the pack's marker file: Maya's model name");
  const auto glm_card = f.launcher->card("GLM-Test");
  const auto row = [&glm_card](const std::string& key) {
    const auto found = glm_card->card.config.entries.find(key);
    return found == glm_card->card.config.entries.end() ? std::string{} : std::get<std::string>(found->second.value);
  };
  expect(glm_card && glm_card->card.profile == secure_frontend::EngineProfile::glm53 &&
             row("roots.models") == secure_frontend::path_to_utf8(glm / "pack") &&
             row("roots.tokenizer_pack") == secure_frontend::path_to_utf8(glm / "pack" / "tokenizer") && row("engine.model_id") == "glm-5.3-flash",
         "the GLM card: the pack as its model folder, its tokenizer, Maya's model name");
  // a folder whose pack/ is another model's: Maya's pack-<name>/
  const auto glm2 = f.root / "glm2";
  secure_frontend_test::make_private_directory(glm2);
  secure_frontend_test::write_file(glm2 / "GLM-Two-00001-of-00002.gguf", test_support::gguf_with_tokenizer(test_support::byte_tokenizer(), "gpt2", "glm5next").bytes());
  secure_frontend_test::write_file(glm2 / "GLM-Two-00002-of-00002.gguf", "shard two");
  std::filesystem::create_directories(glm2 / "pack");
  secure_frontend_test::write_file(glm2 / "pack" / "native_experts.txt", "# strata native experts v3: ... absolute offsets in Other-00001-of-00002.gguf, or ...)\n");
  expect(f.add(glm2, "GLM-Two-00001-of-00002.gguf").status == 201 && f.prepared.size() == 3U &&
             value_after(f.prepared[2], "--out") == secure_frontend::path_to_utf8(glm2 / "pack-glm-two"),
         "another model's pack/ is left alone: pack-<name>/");

  Fixture none(true, {}, false);
  secure_frontend_test::write_file(none.models / first, test_support::gguf_with_tokenizer(test_support::byte_tokenizer(), "gpt2", "qwen4exp").bytes());
  const auto refused = none.add(none.models, first);
  expect(refused.status == 422 && has(refused, "strata-prepare"), "no strata-prepare beside the engine: refused, by name: " + shown(refused));
}

// DeepSeek-V4.1 (2026-10-27, spec 2026-10-27-strata-prepare-design §7.2): Maya's DeepSeek step builds the pack in
// <folder>/maya-ds4 (maya.py ds4_pack_step), the shipped profile beside the engine handed to it to seed
// (ds4_seed_profile); a maya-ds4/ whose sources.json lists another model's shards is left alone (maya-ds4-<name>/).
// The card is deepseek on the pack, its marker Maya's model name.
void a_deepseek_model_is_added_with_mayas_ds4_pack() {
  Fixture f;
  const auto profiles = f.engine.parent_path() / "profiles";
  secure_frontend_test::make_private_directory(profiles);
  secure_frontend_test::write_file(profiles / "expert-profile-ds4.bin", "profile");
  f.on_prepare = [](const std::vector<std::string>& a) {
    const auto out = secure_frontend::path_from_utf8(value_after(a, "--out"));
    std::filesystem::create_directories(out / "tokenizer");
    secure_frontend_test::write_file(out / "prepared.json", "{\"pack\": " + json_path(out) + ", \"tokenizer\": " + json_path(out / "tokenizer") +
                                                                ", \"ple\": null, \"shards\": 2}\n");
  };
  const auto ds = f.root / "ds";
  secure_frontend_test::make_private_directory(ds);
  const std::string first = "DeepSeek-Test-00001-of-00002.gguf";
  secure_frontend_test::write_file(ds / first, test_support::gguf_with_tokenizer(test_support::byte_tokenizer(), "gpt2", "deepseek41").bytes());
  secure_frontend_test::write_file(ds / "DeepSeek-Test-00002-of-00002.gguf", "shard two");
  const auto added = f.add(ds, first);
  expect(added.status == 201 && has(added, "\"name\":\"DeepSeek-Test\"") && has(added, "\"profile\":\"deepseek\""),
         "a DeepSeek model is added: " + shown(added));
  expect(f.prepared.size() == 1U &&
             f.prepared[0] == std::vector<std::string>{"download", "--maya", "--gguf", secure_frontend::path_to_utf8(ds / first), "--out",
                                                       secure_frontend::path_to_utf8(ds / "maya-ds4"), "--expert-profile",
                                                       secure_frontend::path_to_utf8(profiles / "expert-profile-ds4.bin")},
         "strata-prepare download --maya: the pack in maya-ds4/ beside the shards, the shipped profile to seed");
  expect(std::filesystem::exists(ds / "maya-ds4" / "deepseek-v4.1-flash.gguf"), "the pack's marker file: Maya's model name");
  const auto card = f.launcher->card("DeepSeek-Test");
  const auto row = [&card](const std::string& key) {
    const auto found = card->card.config.entries.find(key);
    return found == card->card.config.entries.end() || !std::holds_alternative<std::string>(found->second.value)
               ? std::string{}
               : std::get<std::string>(found->second.value);
  };
  expect(card && card->card.profile == secure_frontend::EngineProfile::deepseek &&
             row("roots.models") == secure_frontend::path_to_utf8(ds / "maya-ds4") &&
             row("roots.tokenizer_pack") == secure_frontend::path_to_utf8(ds / "maya-ds4" / "tokenizer") &&
             row("engine.model_id") == "deepseek-v4.1-flash",
         "the DeepSeek card: the pack as its model folder, its tokenizer, Maya's model name");
  // a folder whose maya-ds4/ indexes another model: maya-ds4-<name>/
  const auto ds2 = f.root / "ds2";
  secure_frontend_test::make_private_directory(ds2);
  secure_frontend_test::write_file(ds2 / "DeepSeek-Two-00001-of-00002.gguf",
                                   test_support::gguf_with_tokenizer(test_support::byte_tokenizer(), "gpt2", "deepseek41").bytes());
  secure_frontend_test::write_file(ds2 / "DeepSeek-Two-00002-of-00002.gguf", "shard two");
  std::filesystem::create_directories(ds2 / "maya-ds4");
  secure_frontend_test::write_file(ds2 / "maya-ds4" / "sources.json",
                                   "{\n \"files\": [\n  {\n   \"name\": \"Other-00001-of-00002.gguf\",\n   \"size\": 1,\n   \"mtime_ns\": 2\n  }\n ]\n}");
  expect(f.add(ds2, "DeepSeek-Two-00001-of-00002.gguf").status == 201 && f.prepared.size() == 2U &&
             value_after(f.prepared[1], "--out") == secure_frontend::path_to_utf8(ds2 / "maya-ds4-deepseek-two"),
         "another model's maya-ds4/ is left alone: maya-ds4-<name>/");
}

// MiniMax-M3.1-Flash (owner 2026-10-28): Maya's MiniMax step builds the pack in <folder>/maya-mm3 (maya.py mm3_pack_step)
// with Maya's chat template, which the install ships beside the engine (engine/templates/minimax-m3.1); a maya-mm3/
// whose sources.json lists another model's shards is left alone (maya-mm3-<name>/). The card is minimax on the pack, its
// marker Maya's model name. Without the shipped template the model is refused, by name.
void a_minimax_model_is_added_with_mayas_mm3_pack() {
  Fixture f;
  f.on_prepare = [](const std::vector<std::string>& a) {
    const auto out = secure_frontend::path_from_utf8(value_after(a, "--out"));
    std::filesystem::create_directories(out / "tokenizer");
    secure_frontend_test::write_file(out / "prepared.json", "{\"pack\": " + json_path(out) + ", \"tokenizer\": " + json_path(out / "tokenizer") +
                                                                ", \"ple\": null, \"shards\": 2}\n");
  };
  const auto mm = f.root / "mm";
  secure_frontend_test::make_private_directory(mm);
  const std::string first = "MiniMax-Test-IQ1_S-00001-of-00002.gguf";
  secure_frontend_test::write_file(mm / first, test_support::gguf_with_tokenizer(test_support::byte_tokenizer(), "gpt2", "minimax-m3").bytes());
  secure_frontend_test::write_file(mm / "MiniMax-Test-IQ1_S-00002-of-00002.gguf", "shard two");
  const auto no_template = f.add(mm, first);
  expect(no_template.status == 422 && has(no_template, "chat template"), "no shipped MiniMax template: refused, by name: " + shown(no_template));
  const auto templates = f.engine.parent_path() / "templates" / "minimax-m3.1";
  secure_frontend_test::make_private_directory(f.engine.parent_path() / "templates");
  secure_frontend_test::make_private_directory(templates);
  secure_frontend_test::write_file(templates / "chat_template.jinja", "]~!b[");
  const auto added = f.add(mm, first);
  expect(added.status == 201 && has(added, "\"name\":\"MiniMax-Test-IQ1_S\"") && has(added, "\"profile\":\"minimax\""),
         "a MiniMax model is added: " + shown(added));
  expect(!f.prepared.empty() &&
             f.prepared.back() == std::vector<std::string>{"download", "--maya", "--gguf", secure_frontend::path_to_utf8(mm / first), "--out",
                                                           secure_frontend::path_to_utf8(mm / "maya-mm3"), "--template",
                                                           secure_frontend::path_to_utf8(templates / "chat_template.jinja")},
         "strata-prepare download --maya: the pack in maya-mm3/ beside the shards, Maya's template from the install");
  expect(std::filesystem::exists(mm / "maya-mm3" / "minimax-m3.1-flash.gguf"), "the pack's marker file: Maya's model name");
  const auto card = f.launcher->card("MiniMax-Test-IQ1_S");
  const auto row = [&card](const std::string& key) {
    const auto found = card->card.config.entries.find(key);
    return found == card->card.config.entries.end() || !std::holds_alternative<std::string>(found->second.value)
               ? std::string{}
               : std::get<std::string>(found->second.value);
  };
  expect(card && card->card.profile == secure_frontend::EngineProfile::minimax &&
             row("roots.models") == secure_frontend::path_to_utf8(mm / "maya-mm3") &&
             row("roots.tokenizer_pack") == secure_frontend::path_to_utf8(mm / "maya-mm3" / "tokenizer") &&
             row("engine.model_id") == "minimax-m3.1-flash",
         "the MiniMax card: the pack as its model folder, its tokenizer, Maya's model name");
  // a folder whose maya-mm3/ indexes another model: maya-mm3-<name>/
  const auto mm2 = f.root / "mm2";
  secure_frontend_test::make_private_directory(mm2);
  secure_frontend_test::write_file(mm2 / "MiniMax-Two-00001-of-00002.gguf",
                                   test_support::gguf_with_tokenizer(test_support::byte_tokenizer(), "gpt2", "minimax-m3").bytes());
  secure_frontend_test::write_file(mm2 / "MiniMax-Two-00002-of-00002.gguf", "shard two");
  std::filesystem::create_directories(mm2 / "maya-mm3");
  secure_frontend_test::write_file(mm2 / "maya-mm3" / "sources.json",
                                   "{\n \"files\": [\n  {\n   \"name\": \"Other-00001-of-00002.gguf\",\n   \"size\": 1,\n   \"mtime_ns\": 2\n  }\n ]\n}");
  expect(f.add(mm2, "MiniMax-Two-00001-of-00002.gguf").status == 201 &&
             value_after(f.prepared.back(), "--out") == secure_frontend::path_to_utf8(mm2 / "maya-mm3-minimax-two"),
         "another model's maya-mm3/ is left alone: maya-mm3-<name>/");
}

// §4.3: a download's destination on a drive others may write - a new folder there is owner-only from the start.
void a_new_private_folder_is_made() {
  Fixture f;
  const auto shared = f.root / "drive";
  secure_frontend_test::make_private_directory(shared);
  const auto make = [&f, &shared](const std::string& name) {
    return f.post(secure_frontend::RouteId::strata_folder_new, "{\"parent\":" + json_path(shared) + ",\"name\":" + json_text(name) + "}");
  };
#if defined(_WIN32)
  expect(secure_frontend_test::grant_everyone_write(shared), "the test widens a folder");
#else
  // POSIX: a parent others may write without the sticky bit lets them rename what is in it - refused; with the sticky
  // bit (a shared drive's usual 1777, as /tmp) it is the Windows case ("everyone may write")
  ::chmod(shared.c_str(), 0777);
  const auto no_sticky = make("strata models");
  expect(no_sticky.status == 422 && has(no_sticky, "\"code\":\"folder_refused\"") && !std::filesystem::exists(shared / "strata models"),
         "a parent anyone may rename in (0777, no sticky bit) is refused: " + shown(no_sticky));
  ::chmod(shared.c_str(), 01777);
#endif
  const auto made = make("strata models");
  expect(made.status == 201 && has(made, "\"path\":" + json_path(shared / "strata models")), "a new folder: " + shown(made));
  expect(std::holds_alternative<secure_frontend::CanonicalRoot>(secure_frontend::open_root(shared / "strata models")),
         "it is a trusted (owner-only) folder");
  expect(make("strata models").status == 201, "asking again answers the same folder");
  for (const std::string& bad : std::vector<std::string>{"", "..", ".hidden", "a/b", "a\\b", "trailing.", "x:y"}) {
    expect(make(bad).status == 422, "a bad name is refused: " + bad);
  }
  secure_frontend_test::make_private_directory(shared / "open");
#if defined(_WIN32)
  (void)secure_frontend_test::grant_everyone_write(shared / "open");
#else
  ::chmod((shared / "open").c_str(), 0777);
#endif
  expect(make("open").status == 422, "an existing folder others may write is not taken");
}

void a_card_follows_its_moved_folder() {
  Fixture f;
  expect(f.add(f.models, kModelFile).status == 201, "the card");
  const auto moved = f.root / "moved";
  secure_frontend_test::make_private_directory(moved);
  const auto repoint = [&f](const std::string& name, const std::filesystem::path& folder) {
    return f.post(secure_frontend::RouteId::strata_card_folder, "{\"name\":" + json_text(name) + ",\"folder\":" + json_path(folder) + "}");
  };
  const auto empty = repoint("tiny-gemma-Q8_0", moved);
  expect(empty.status == 422 && has(empty, "\"code\":\"card_refused\""), "the model file must be in the new folder: " + shown(empty));
  std::filesystem::copy_file(f.models / kModelFile, moved / kModelFile);
  const auto done = repoint("tiny-gemma-Q8_0", moved);
  expect(done.status == 200, "the card follows: " + shown(done));
  expect(f.card_folder("tiny-gemma-Q8_0") == secure_frontend::path_to_utf8(moved), "the launcher holds the new folder");
  const auto reread = secure_frontend::read_model_card(f.data.state / "cards" / "tiny-gemma-Q8_0.json");
  expect(std::holds_alternative<secure_frontend::ModelCard>(reread) &&
             std::get<std::string>(std::get<secure_frontend::ModelCard>(reread).config.entries.at("roots.models").value) ==
                 secure_frontend::path_to_utf8(moved),
         "and so does its file");
  const auto op = repoint("Op", moved);
  expect(op.status == 403 && has(op, "\"code\":\"card_read_only\""), "an operator card: " + shown(op));
  const auto nobody = repoint("Nobody", moved);
  expect(nobody.status == 404 && has(nobody, "\"code\":\"model_not_found\""), "an unknown card: " + shown(nobody));
}

std::string code_of(const secure_frontend::HttpResponse& response) {
  const auto at = response.body.find("\"code\":\"");
  return at == std::string::npos ? std::string{} : response.body.substr(at + 8U, 9U);
}

// §4.5: a fresh code, the API key again, and "permanent": true - each checked by the server; a wrong try spends the
// code; then only the card's model file goes (the folder keeps what else it holds), then the card.
void delete_needs_the_code_the_key_and_permanent() {
  Fixture f;
  expect(f.add(f.models, kModelFile).status == 201, "the card");
  const auto begin = [&f](const std::string& name) {
    return f.post(secure_frontend::RouteId::strata_card_delete_begin, "{\"name\":" + json_text(name) + "}");
  };
  const auto confirm = [&f](const std::string& code, const std::string_view key, const std::string& permanent) {
    return f.post(secure_frontend::RouteId::strata_card_delete, "{\"name\":\"tiny-gemma-Q8_0\",\"code\":" + json_text(code) +
                                                                    ",\"key\":" + json_text(key) + ",\"permanent\":" + permanent + "}");
  };
  const auto first = begin("tiny-gemma-Q8_0");
  const std::string code = code_of(first);
  expect(first.status == 200 && code.size() == 9U && first.header_value("Cache-Control") == std::optional<std::string_view>{"no-store"},
         "a code, never cached: " + shown(first));
  expect(confirm("AAAA-AAAA", kKey, "true").status == 403, "a wrong code is refused");
  expect(confirm(code, kKey, "true").status == 403, "and spent the code");
  const auto wrong_key = confirm(code_of(begin("tiny-gemma-Q8_0")), "not-the-key", "true");
  expect(wrong_key.status == 403 && has(wrong_key, "\"code\":\"delete_refused\""), "a wrong key: " + shown(wrong_key));
  expect(confirm(code_of(begin("tiny-gemma-Q8_0")), kKey, "false").status == 403, "not marked permanent");
  expect(confirm(code_of(begin("tiny-gemma-Q8_0")), kKey, "\"true\"").status == 403, "permanent is the JSON true");
  expect(std::filesystem::exists(f.models / kModelFile) && f.launcher->card("tiny-gemma-Q8_0"), "nothing was deleted yet");

  const auto deleted = confirm(code_of(begin("tiny-gemma-Q8_0")), kKey, "true");
  expect(deleted.status == 200 && has(deleted, "\"files\":1") && has(deleted, "\"folder_removed\":false"), "deleted: " + shown(deleted));
  expect(!std::filesystem::exists(f.models / kModelFile) && std::filesystem::exists(f.models / "notes.txt"),
         "only the model's file went");
  expect(!f.launcher->card("tiny-gemma-Q8_0") && !std::filesystem::exists(f.data.state / "cards" / "tiny-gemma-Q8_0.json"),
         "and its card");
  expect(f.removed_downloads.empty() && !has(deleted, "download_removed"), "a folder's card leaves the downloads alone");
  const auto op = begin("Op");
  expect(op.status == 403 && has(op, "\"code\":\"card_read_only\""), "an operator card can't be deleted: " + shown(op));
  expect(begin("Nobody").status == 404, "an unknown card");

  Fixture keyless(false);
  expect(keyless.add(keyless.models, kModelFile).status == 201, "a card on a server without a key");
  const auto refused = keyless.post(secure_frontend::RouteId::strata_card_delete_begin, "{\"name\":\"tiny-gemma-Q8_0\"}");
  expect(refused.status == 422 && has(refused, "\"code\":\"card_refused\""), "no key to confirm with: no delete: " + shown(refused));
}

// §4.5: a downloaded model's delete also takes it out of its download store (strata rm, through the download manager),
// so the Download list offers it again; a store that keeps it is reported, after the files and the card are gone.
void a_downloaded_model_leaves_its_store_too() {
  for (const bool store_fails : {false, true}) {
    Fixture f;
    if (store_fails) f.remove_failure = "internal_error";
    expect(f.add(f.models, kModelFile).status == 201, "the card");
    auto card = f.launcher->card("tiny-gemma-Q8_0")->card;
    card.source = "download:tiny-gemma";
    expect(f.launcher->replace_card(card) == secure_frontend::CardChange::done, "it came from a download");
    const auto begun = f.post(secure_frontend::RouteId::strata_card_delete_begin, "{\"name\":\"tiny-gemma-Q8_0\"}");
    const auto deleted = f.post(secure_frontend::RouteId::strata_card_delete,
                                "{\"name\":\"tiny-gemma-Q8_0\",\"code\":" + json_text(code_of(begun)) + ",\"key\":" + json_text(kKey) +
                                    ",\"permanent\":true}");
    expect(deleted.status == 200 && has(deleted, "\"files\":1") && !std::filesystem::exists(f.models / kModelFile) &&
               !f.launcher->card("tiny-gemma-Q8_0"),
           "the files and the card go: " + shown(deleted));
    expect(f.removed_downloads == std::vector<std::string>{"tiny-gemma"}, "and the download is taken out of its store");
    expect(has(deleted, store_fails ? "\"download_removed\":false" : "\"download_removed\":true"),
           "the answer says whether the store let it go: " + shown(deleted));
  }
}

// Owner 2026-10-27 ("Allow all from subnets"): the server's own rule (model_files_allowed) may let a remote client in;
// the routes then serve it like this PC, and refuse any other.
void listed_remote_clients_use_the_file_routes() {
  Fixture f(true, [](const secure_frontend::RequestContext& context) {
    return secure_frontend::model_files_allowed(context.loopback_peer, context.peer, true,
                                                {*secure_frontend::parse_ip_network("172.16.1.0/24")});
  });
  const auto listed = f.post_from(secure_frontend::RouteId::strata_folders, "{}", {172, 16, 1, 7});
  expect(listed.status == 200 && has(listed, "\"volumes\":["), "a client in the listed subnet sees the drives: " + shown(listed));
  const auto added = f.post_from(secure_frontend::RouteId::strata_card_add,
                                 "{\"folder\":" + json_path(f.models) + ",\"file\":" + json_text(kModelFile) + "}", {172, 16, 1, 7});
  expect(added.status == 201, "and adds a card: " + shown(added));
  const auto other = f.post_from(secure_frontend::RouteId::strata_folders, "{}", {172, 16, 3, 7});
  expect(other.status == 403 && has(other, "\"code\":\"control_not_allowed\""), "a client outside it is refused: " + shown(other));
}

// The owner's raon1 folder (2026-10-27: "it says the permission issue, and it did not add the card"): a raon-1-mini
// GGUF in a folder others may write, with the pack an earlier setup built beside it (pack/). The page is told to make it
// private; that is done with the pack in it; then the model is added - its pack by strata-prepare download --maya in
// pack/ (which reuses an up-to-date one), the marker raon1-mini.gguf, a raon1 card on the pack.
void a_raon1_model_is_added_from_its_folder() {
  using secure_frontend::RouteId;
  Fixture f;
  const auto folder = f.root / "raon1_q8";
  const std::string file = "raon-1-mini-preview-Q8_0.gguf";
  secure_frontend_test::make_private_directory(folder);
  secure_frontend_test::write_file(folder / file, test_support::gguf_with_tokenizer(test_support::byte_tokenizer(), "gpt2", "raon1").bytes());
  std::filesystem::create_directories(folder / "pack" / "tokenizer");
  secure_frontend_test::write_file(folder / "pack" / "native_experts.txt",
                                   "# strata native experts v3: ... absolute offsets in " + file + ", or in the named shard(s) beside it)\n");
  for (const auto& inner : {folder, folder / "pack", folder / "pack" / "tokenizer"}) Fixture::widen(inner);
  f.on_prepare = [](const std::vector<std::string>& a) {
    const auto out = secure_frontend::path_from_utf8(value_after(a, "--out"));
    secure_frontend_test::write_file(out / "prepared.json", "{\"pack\": " + json_path(out) + ", \"tokenizer\": " +
                                                                json_path(out / "tokenizer") + ", \"ple\": null, \"shards\": 1}\n");
  };
  const auto refused = f.add(folder, file);
  expect(refused.status == 409 && has(refused, "\"code\":\"folder_not_private\"") && f.prepared.empty(),
         "a folder others may change: the page is asked to make it private: " + shown(refused));
  const auto made = f.post(RouteId::strata_folder_private, "{\"path\":" + json_path(folder) + "}");
  expect(made.status == 200, "the folder is made private with its pack in it: " + shown(made));
  const auto added = f.add(folder, file);
  expect(added.status == 201 && has(added, "\"name\":\"raon-1-mini-preview-Q8_0\"") && has(added, "\"profile\":\"raon1\""),
         "then the raon1 model is added: " + shown(added));
  expect(f.prepared.size() == 1U && f.prepared[0] == std::vector<std::string>{"download", "--maya", "--gguf", secure_frontend::path_to_utf8(folder / file),
                                                                              "--out", secure_frontend::path_to_utf8(folder / "pack")},
         "strata-prepare download --maya on the pack beside it");
  expect(std::filesystem::exists(folder / "pack" / "raon1-mini.gguf"), "the pack's marker: the served model name");
  const auto held = f.launcher->card("raon-1-mini-preview-Q8_0");
  const auto row = [&held](const std::string& key) {
    const auto found = held->card.config.entries.find(key);
    return found == held->card.config.entries.end() ? std::string{} : std::get<std::string>(found->second.value);
  };
  expect(held && held->card.profile == secure_frontend::EngineProfile::raon1 && row("roots.models") == secure_frontend::path_to_utf8(folder / "pack") &&
             row("roots.tokenizer_pack") == secure_frontend::path_to_utf8(folder / "pack" / "tokenizer") && row("engine.model_id") == "raon1-mini",
         "the raon1 card: the pack as its model folder, its tokenizer, raon1-mini");
}

// A running model's card can't be deleted (its engine holds the files); its folder may change and applies at the next
// start.
void a_running_card_is_not_deleted() {
  Fixture f;
  expect(f.add(f.models, kModelFile).status == 201, "the card");
  expect(!f.launcher->start("tiny-gemma-Q8_0", nullptr, false).has_value(), "it starts");
  expect(f.launcher->wait_until_settled(std::chrono::seconds(60)), "the start settles");
  const auto state = f.launcher->state_of("tiny-gemma-Q8_0");
  expect(state == secure_frontend::CardState::running, "it runs (state " + std::string{secure_frontend::card_state_name(state)} + ")");
  const auto begin = f.post(secure_frontend::RouteId::strata_card_delete_begin, "{\"name\":\"tiny-gemma-Q8_0\"}");
  expect(begin.status == 409 && has(begin, "\"code\":\"card_in_use\""), "no delete while it runs: " + shown(begin));
  expect(f.launcher->remove_card("tiny-gemma-Q8_0") == secure_frontend::CardChange::running, "nor a remove");
  (void)f.launcher->stop(false);
  expect(f.launcher->wait_until_settled(std::chrono::seconds(60)), "the stop settles");
}

// Owner 2026-10-28 ("also add an ability to remove an active model card but not delete files"): a card the page made is
// removed - out of the launcher, its card file and its calibration gone - while the model's files stay where they are
// and the download store is not touched; the model can be added again from its folder. An operator card stays.
void a_card_is_removed_and_its_files_stay() {
  Fixture f;
  expect(f.add(f.models, kModelFile).status == 201, "the card");
  const auto remove = [&f](const std::string& name) {
    return f.post(secure_frontend::RouteId::strata_card_remove, "{\"name\":" + json_text(name) + "}");
  };
  const auto removed = remove("tiny-gemma-Q8_0");
  expect(removed.status == 200 && has(removed, "\"name\":\"tiny-gemma-Q8_0\"") && has(removed, "\"stopping\":false"),
         "removed: " + shown(removed));
  expect(!f.launcher->card("tiny-gemma-Q8_0") && !std::filesystem::exists(f.data.state / "cards" / "tiny-gemma-Q8_0.json"),
         "the card and its file are gone");
  expect(std::filesystem::exists(f.models / kModelFile) && std::filesystem::exists(f.models / "notes.txt"), "the model's files stay");
  expect(f.removed_downloads.empty(), "the download store is not touched");
  expect(f.add(f.models, kModelFile).status == 201, "the model can be added again from its folder");
  const auto op = remove("Op");
  expect(op.status == 403 && has(op, "\"code\":\"card_read_only\"") && f.launcher->card("Op"), "an operator card stays: " + shown(op));
  expect(remove("Nobody").status == 404, "an unknown card");
}

// The same for the card that runs ("an active model card"): its model is stopped first (as Stop does), then the card
// goes; its files stay.
void a_running_card_is_stopped_then_removed() {
  Fixture f;
  expect(f.add(f.models, kModelFile).status == 201, "the card");
  expect(!f.launcher->start("tiny-gemma-Q8_0", nullptr, false).has_value() && f.launcher->wait_until_settled(std::chrono::seconds(60)) &&
             f.launcher->state_of("tiny-gemma-Q8_0") == secure_frontend::CardState::running,
         "it runs");
  const auto removed = f.post(secure_frontend::RouteId::strata_card_remove, "{\"name\":\"tiny-gemma-Q8_0\"}");
  expect(removed.status == 200 && has(removed, "\"stopping\":true"), "accepted, its model stopping first: " + shown(removed));
  expect(f.launcher->wait_until_settled(std::chrono::seconds(60)), "the stop settles");
  expect(!f.launcher->card("tiny-gemma-Q8_0") && !std::filesystem::exists(f.data.state / "cards" / "tiny-gemma-Q8_0.json"),
         "then the card is gone");
  expect(f.launcher->list_json().find("\"running\":null") != std::string::npos, "nothing runs: " + f.launcher->list_json());
  expect(std::filesystem::exists(f.models / kModelFile), "and the model's file stays");
}

// Owner 2026-10-27 ("an option to not change the permissions when browsing and using existing models in the local
// system ... ignore that other users can access the files locally"): with roots.allow_untrusted_media the page uses a
// model folder as it is - a model is added from a folder other users may change, a card moves to another, and a GLM
// pack strata-prepare made beside such shards is not made private either; each folder's permissions stay as they were.
// Without it (the default) a pack that came out widely writable is made private, as before.
void a_shared_folder_is_used_as_it_is_when_the_owner_allows_it() {
  Fixture f(true, {}, true, true);
  const auto shared = f.root / "shared";
  secure_frontend_test::make_private_directory(shared);
  Fixture::write_gguf(shared / "other-gemma.gguf", "gemma4");
  Fixture::widen(shared);
  const auto added = f.add(shared, "other-gemma.gguf");
  expect(added.status == 201 && has(added, "\"name\":\"other-gemma\""), "a model is added from a shared folder: " + shown(added));
  expect(f.card_folder("other-gemma") == secure_frontend::path_to_utf8(shared), "its card reads from that folder");
  expect(!Fixture::is_private(shared), "the folder's permissions are as they were");

  const auto moved = f.root / "moved";
  secure_frontend_test::make_private_directory(moved);
  Fixture::write_gguf(moved / "other-gemma.gguf", "gemma4");
  Fixture::widen(moved);
  const auto repointed = f.post(secure_frontend::RouteId::strata_card_folder, "{\"name\":\"other-gemma\",\"folder\":" + json_path(moved) + "}");
  expect(repointed.status == 200 && f.card_folder("other-gemma") == secure_frontend::path_to_utf8(moved),
         "a card moves to another shared folder: " + shown(repointed));
  expect(!Fixture::is_private(moved), "that folder is left as it is too");

  const auto glm_pack = [](const std::vector<std::string>& a) {
    const auto out = secure_frontend::path_from_utf8(value_after(a, "--out"));
    std::filesystem::create_directories(out / "tokenizer");
    secure_frontend_test::write_file(out / "native_experts.txt",
                                     "# strata native experts v3: ... absolute offsets in GLM-Test-00001-of-00002.gguf, or ...)\n");
    secure_frontend_test::write_file(out / "prepared.json", "{\"pack\": " + json_path(out) + ", \"tokenizer\": " +
                                                                json_path(out / "tokenizer") + ", \"ple\": null, \"shards\": 2}\n");
    Fixture::widen(out);  // as a pack made in a widely writable folder can come out
  };
  const auto glm_folder = [](const std::filesystem::path& folder, const bool wide) {
    secure_frontend_test::make_private_directory(folder);
    secure_frontend_test::write_file(folder / "GLM-Test-00001-of-00002.gguf",
                                     test_support::gguf_with_tokenizer(test_support::byte_tokenizer(), "gpt2", "glm5-next").bytes());
    secure_frontend_test::write_file(folder / "GLM-Test-00002-of-00002.gguf", "shard two");
    if (wide) Fixture::widen(folder);
  };
  glm_folder(f.root / "glm", true);
  f.on_prepare = glm_pack;
  const auto packed = f.add(f.root / "glm", "GLM-Test-00001-of-00002.gguf");
  expect(packed.status == 201 && has(packed, "\"profile\":\"glm53\""), "a GLM model is added from a shared folder: " + shown(packed));
  expect(!Fixture::is_private(f.root / "glm" / "pack"), "its pack is not made private");

  Fixture strict;
  glm_folder(strict.root / "glm", false);
  strict.on_prepare = glm_pack;
  expect(strict.add(strict.root / "glm", "GLM-Test-00001-of-00002.gguf").status == 201 && Fixture::is_private(strict.root / "glm" / "pack"),
         "the control: without the option the pack is made private");
}

// The page's Settings > Model folders: roots.allow_untrusted_media read from and saved to the config file (it applies at
// the next start, as the page's other settings), for the clients the file routes serve: this PC's, and the listed
// remote ones the server's rule lets in (owner 2026-10-27: the "make it private" dialog names the setting, and a page
// opened from another PC did not show it).
void the_shared_folders_setting_is_read_and_saved() {
  using secure_frontend::RouteId;
  Fixture f;
  const auto first = f.post(RouteId::strata_folder_sharing, "{}");
  expect(first.status == 200 && has(first, "\"shared\":false") && has(first, "\"running\":false") && has(first, "\"editable\":true"),
         "off by default: " + shown(first));
  const auto saved = f.post(RouteId::strata_folder_sharing, "{\"shared\":true}");
  expect(saved.status == 200 && has(saved, "\"saved\":true") && has(saved, "\"restart_needed\":true"), "saved: " + shown(saved));
  const auto draft = secure_frontend::read_config_file(f.data.config_file);
  const auto* entries = std::get_if<secure_frontend::FrontendConfigDraft>(&draft);
  const bool row_on = entries != nullptr && entries->entries.contains("roots.allow_untrusted_media") &&
                      std::get<bool>(entries->entries.at("roots.allow_untrusted_media").value);
  expect(row_on && entries->entries.contains("listener.port"), "the config file holds the row, its other rows kept");
  const auto again = f.post(RouteId::strata_folder_sharing, "{}");
  expect(has(again, "\"shared\":true") && has(again, "\"running\":false"), "read back; this server keeps its own until restarted: " + shown(again));
  expect(f.post(RouteId::strata_folder_sharing, "{\"shared\":false}").status == 200 &&
             has(f.post(RouteId::strata_folder_sharing, "{}"), "\"shared\":false"),
         "and turned off again");
  for (const char* body : {"{\"shared\":\"yes\"}", "{\"other\":true}", "[]", "{\"shared\":true,\"shared\":false}"}) {
    expect(f.post(RouteId::strata_folder_sharing, body).status == 400, std::string{"a malformed body is refused: "} + body);
  }

  Fixture listed(true, [](const secure_frontend::RequestContext& context) {
    return secure_frontend::model_files_allowed(context.loopback_peer, context.peer, true,
                                                {*secure_frontend::parse_ip_network("172.16.1.0/24")});
  });
  const auto remote = listed.post_from(RouteId::strata_folder_sharing, "{\"shared\":true}", {172, 16, 1, 7});
  expect(remote.status == 200 && has(remote, "\"saved\":true"), "a client in the listed subnet saves it: " + shown(remote));
  expect(has(listed.post_from(RouteId::strata_folder_sharing, "{}", {172, 16, 1, 7}), "\"shared\":true"), "and reads it back");
  const auto outside = listed.post_from(RouteId::strata_folder_sharing, "{}", {172, 16, 3, 7});
  expect(outside.status == 403 && has(outside, "control_not_allowed"), "a client outside it is refused: " + shown(outside));

  Fixture on(true, {}, true, true);
  expect(has(on.post(RouteId::strata_folder_sharing, "{}"), "\"running\":true"), "a server started with it says so");

  Fixture fileless(true, {}, true, false, false);
  const auto read = fileless.post(RouteId::strata_folder_sharing, "{}");
  const auto refused = fileless.post(RouteId::strata_folder_sharing, "{\"shared\":true}");
  expect(read.status == 200 && has(read, "\"editable\":false") && refused.status == 422 && has(refused, "settings_refused"),
         "without a config file the setting is shown, not saved: " + shown(refused));
}


}  // namespace

int main(int argc, char** argv) {
  return secure_frontend_test::run_cases(argc, argv, {
      {"a_shared_folder_is_used_as_it_is_when_the_owner_allows_it", a_shared_folder_is_used_as_it_is_when_the_owner_allows_it},
      {"the_shared_folders_setting_is_read_and_saved", the_shared_folders_setting_is_read_and_saved},
      {"card_routes_answer_loopback_clients_only", card_routes_answer_loopback_clients_only},
      {"folders_list_the_drives_and_one_folder", folders_list_the_drives_and_one_folder},
      {"a_card_is_added_from_a_folder", a_card_is_added_from_a_folder},
      {"a_qwen_model_beside_its_encoder_reads_pictures", a_qwen_model_beside_its_encoder_reads_pictures},
      {"a_gemma4_model_beside_its_mtp_file_drafts_with_it", a_gemma4_model_beside_its_mtp_file_drafts_with_it},
      {"qwen38_and_glm_models_are_added_with_their_packs", qwen38_and_glm_models_are_added_with_their_packs},
      {"a_qwen38_model_finds_its_draft_layer", a_qwen38_model_finds_its_draft_layer},
      {"a_deepseek_model_is_added_with_mayas_ds4_pack", a_deepseek_model_is_added_with_mayas_ds4_pack},
      {"a_minimax_model_is_added_with_mayas_mm3_pack", a_minimax_model_is_added_with_mayas_mm3_pack},
      {"a_card_follows_its_moved_folder", a_card_follows_its_moved_folder},
      {"a_new_private_folder_is_made", a_new_private_folder_is_made},
      {"delete_needs_the_code_the_key_and_permanent", delete_needs_the_code_the_key_and_permanent},
      {"a_running_card_is_not_deleted", a_running_card_is_not_deleted},
      {"a_card_is_removed_and_its_files_stay", a_card_is_removed_and_its_files_stay},
      {"a_running_card_is_stopped_then_removed", a_running_card_is_stopped_then_removed},
      {"a_downloaded_model_leaves_its_store_too", a_downloaded_model_leaves_its_store_too},
      {"listed_remote_clients_use_the_file_routes", listed_remote_clients_use_the_file_routes},
      {"a_raon1_model_is_added_from_its_folder", a_raon1_model_is_added_from_its_folder},
  }, failures);
}
