// SP4 Task 22 (spec §1.3, §2.5, §2.7, §3.7, §4.6, §4.7, §6.1, §8.3): run_pull end to end over
// the scripted transport. Every request is scripted (ScriptedTransport throws otherwise).
#include "acquire_cases.hpp"
#include "secure_frontend/model_index_store.hpp"
#include "support/expect.hpp"
#include "support/pull_harness.hpp"

#include <string>
#include <utility>

#if defined(_WIN32)
#include "secure_frontend/win32_prelude.hpp"

#include <aclapi.h>
#include <sddl.h>
#else
#include <sys/stat.h>
#endif

namespace acquire_cases {
namespace {

namespace fx = secure_frontend_test::acquire;
using secure_frontend::AcquireReason;
using secure_frontend::ExitCode;
using secure_frontend_test::expect;
using secure_frontend_test::pull::PullHarness;
using secure_frontend_test::pull::ranged_drop;
using secure_frontend_test::pull::ranged_rest;

std::string manifest(const std::uint32_t sequence = 1, const std::string_view salt = "", const bool sources = true) {
  return fx::manifest_text("m", "t", sequence, fx::qwen_payload(salt), sources);
}

// The first shard arrives with only `cut` bytes and curl exit 18; every ranged retry
// (§3.6: size_short is retried on the same source) delivers nothing more.
std::string cut_first_shard(const std::vector<fx::PayloadFile>& payload, const std::uint64_t cut) {
  return "response https://fixture.example/fx/" + payload[0].path + "\nstatus 200 OK\nbody-file " + payload[0].path +
         "\ntruncate-at " + std::to_string(cut) + "\nexit 18\nend\n" + ranged_drop(payload[0], cut);
}

std::string part_of(const std::string& manifest_text, const std::string& path) {
  const auto key = secure_frontend::staging_key(secure_frontend::to_hex(secure_frontend::sha256(manifest_text)));
  return ".staging/" + key + "/" + path + ".part";
}

}  // namespace

void dl02_correct_digest_publishes_atomically() {
  PullHarness h;
  h.serve(fx::qwen_payload());
  const auto outcome = h.pull(h.third_party(manifest()));
  expect(outcome.exit == ExitCode::success && outcome.installed.has_value(), "pull succeeds");
  const auto index = h.index();
  expect(index.models.size() == 1 && index.models.front().origin == secure_frontend::ModelOrigin::third_party, "indexed once");
  expect(!secure_frontend::exists_within(h.models, secure_frontend::must_rel(".staging/" + secure_frontend::staging_key(index.models.front().manifest_sha256))),
         "the staging key was renamed into place (atomic publish)");
  expect(h.ui.text().find("strata run m:t") != std::string::npos, "ends with the run hint");
  expect(h.ui.text().find("The source host sees your IP address and which files you download.") != std::string::npos,
         "the §1.3 privacy sentence is printed");
  expect(h.probe.calls == 1, "one probe after confirmation");
}

void dl02_byte_limit_enforced_while_streaming() {
  PullHarness h;
  h.serve(fx::qwen_payload(), "response https://fixture.example/fx/pack/index.txt\nstatus 200 OK\nbody-text index \nexceed-by 100\nend\n");
  const auto outcome = h.pull(h.third_party(manifest()));
  expect(outcome.error && outcome.error->reason == AcquireReason::size_exceeded && outcome.exit == ExitCode::verification, "exit 4");
  expect(h.index().models.empty(), "nothing indexed");
}

void dl02_resume_with_range_completes_and_verifies() {
  const auto payload = fx::qwen_payload();
  const auto& shard = payload[0];
  PullHarness h;
  h.serve(payload, cut_first_shard(payload, 5));
  const auto request = h.third_party(manifest());
  const auto first = h.pull(request);
  expect(first.exit == ExitCode::transport && first.error && secure_frontend::reason_info(first.error->reason).resumable, "first run: resumable exit 3");
  expect(h.ui.text().find("again to resume") != std::string::npos, "the resume hint is printed");
  h.serve(payload, ranged_rest(shard, 5));
  const auto second = h.pull(request);
  expect(second.exit == ExitCode::success, "rerun completes");
  bool ranged = false;
  for (const auto& argv : h.transport->argvs) ranged = ranged || (argv.size() >= 2 && argv[argv.size() - 2] == "--range" && argv.back() == "5-");
  expect(ranged, "resumed with --range 5- after re-hashing the 5 staged bytes");
}

void resume_offset_ignores_preallocation() {
  // Review Focus 1: the resume offset is the .part length, never its preallocated size.
  const auto payload = fx::qwen_payload();
  PullHarness h;
  h.serve(payload, cut_first_shard(payload, 3));
  const auto request = h.third_party(manifest());
  (void)h.pull(request);
  const auto part = h.models.join(secure_frontend::must_rel(part_of(manifest(), payload[0].path)));
  expect(std::filesystem::file_size(part) == 3, "the .part is 3 bytes long (preallocated with KEEP_SIZE / FileAllocationInfo)");
  h.serve(payload, ranged_rest(payload[0], 3));
  expect(h.pull(request).exit == ExitCode::success, "resumed run completes");
  bool offset_three = false;
  for (const auto& argv : h.transport->argvs) offset_three = offset_three || argv.back() == "3-";
  expect(offset_three, "--range 3- although the .part was preallocated to the full size");
}

void dl02_published_files_are_private() {
  PullHarness h;
  h.serve(fx::qwen_payload());
  expect(h.pull(h.third_party(manifest())).exit == ExitCode::success, "pull");
#if !defined(_WIN32)
  const auto entry = h.index().models.front();
  struct stat info {};
  const auto dir = h.models.join(secure_frontend::must_rel("m/t/" + entry.dir));
  expect(::stat(dir.c_str(), &info) == 0 && (info.st_mode & 0777) == 0700, "version directory 0700");
  expect(::stat((dir / "weights" / "fx-00001-of-00002.gguf").c_str(), &info) == 0 && (info.st_mode & 0777) == 0600, "files 0600");
  expect(::stat((dir / "manifest.json").c_str(), &info) == 0 && (info.st_mode & 0777) == 0600, "manifest.json 0600");
  expect(::stat(h.models.join(secure_frontend::must_rel("index.json")).c_str(), &info) == 0 && (info.st_mode & 0777) == 0600, "index 0600");
#endif
}

#if defined(_WIN32)
void dl02_published_files_have_private_dacl_windows() {
  PullHarness h;
  h.serve(fx::qwen_payload());
  expect(h.pull(h.third_party(manifest())).exit == ExitCode::success, "pull");
  const auto entry = h.index().models.front();
  const auto file = h.models.join(secure_frontend::must_rel("m/t/" + entry.dir + "/weights/fx-00001-of-00002.gguf"));
  PACL dacl = nullptr;
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  expect(GetNamedSecurityInfoW(file.wstring().c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &dacl, nullptr,
                               &descriptor) == ERROR_SUCCESS, "DACL readable");
  SECURITY_DESCRIPTOR_CONTROL control = 0;
  DWORD revision = 0;
  GetSecurityDescriptorControl(descriptor, &control, &revision);
  expect((control & SE_DACL_PROTECTED) != 0, "protected DACL (nothing inherited)");
  PSID system = nullptr;
  ConvertStringSidToSidW(L"S-1-5-18", &system);
  HANDLE token = nullptr;
  OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token);
  DWORD size = 0;
  GetTokenInformation(token, TokenUser, nullptr, 0, &size);
  std::vector<unsigned char> user(size);
  GetTokenInformation(token, TokenUser, user.data(), size, &size);
  CloseHandle(token);
  const PSID self = reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid;
  for (DWORD i = 0; dacl != nullptr && i < dacl->AceCount; ++i) {
    void* raw = nullptr;
    GetAce(dacl, i, &raw);
    const auto* ace = static_cast<ACCESS_ALLOWED_ACE*>(raw);
    const PSID sid = reinterpret_cast<PSID>(const_cast<DWORD*>(&ace->SidStart));
    expect(EqualSid(sid, self) || EqualSid(sid, system), "only the user and SYSTEM are granted access");
  }
  LocalFree(system);
  LocalFree(descriptor);
}
#endif

void dl02_preflight_refuses_without_space() {
  class Tiny final : public secure_frontend::StoreEnvironment {
   public:
    std::variant<std::uint64_t, secure_frontend::AcquireError> free_bytes(const secure_frontend::CanonicalRoot&) const override {
      return std::uint64_t{1024};
    }
  };
  PullHarness h;
  h.serve(fx::qwen_payload());
  const Tiny tiny;
  const auto outcome = h.pull(h.third_party(manifest()), &tiny);
  expect(outcome.error && outcome.error->reason == AcquireReason::disk_space && outcome.exit == ExitCode::space, "exit 6");
  expect(h.transport->argvs.empty() && h.probe.calls == 0, "before any connection or probe");
}

void dl03_untrusted_manifest_digest_rejected() {
  PullHarness h;
  h.serve(fx::qwen_payload());
  auto request = h.third_party(manifest());
  request.trust_digest = std::string(64, '0');
  const auto outcome = h.pull(request);
  expect(outcome.error && outcome.error->reason == AcquireReason::manifest_untrusted && outcome.exit == ExitCode::manifest, "exit 5");
  request.trust_digest.reset();
  const auto again = h.pull(request);
  expect(again.error && again.error->reason == AcquireReason::manifest_untrusted, "no digest at all");
  expect(h.transport->argvs.empty(), "no network");
}

void dl03_downgrade_rejected() {
  PullHarness h;
  h.serve(fx::qwen_payload("2"));
  expect(h.pull(h.third_party(manifest(2, "2"))).exit == ExitCode::success, "v2 installed");
  h.serve(fx::qwen_payload("1"));
  const auto outcome = h.pull(h.third_party(manifest(1, "1")));
  expect(outcome.error && outcome.error->reason == AcquireReason::manifest_downgrade && outcome.exit == ExitCode::manifest, "refused");
  auto allowed = h.third_party(manifest(1, "1"));
  allowed.allow_downgrade = true;
  expect(h.pull(allowed).exit == ExitCode::success && h.index().models.front().sequence == 1, "--allow-downgrade");
}

void dl03_sequence_conflict_rejected() {
  PullHarness h;
  h.serve(fx::qwen_payload("a"));
  expect(h.pull(h.third_party(manifest(2, "a"))).exit == ExitCode::success, "installed");
  h.serve(fx::qwen_payload("b"));
  const auto outcome = h.pull(h.third_party(manifest(2, "b")));
  expect(outcome.error && outcome.error->reason == AcquireReason::manifest_conflict, "same sequence, other digest");
}

#if defined(SECURE_FRONTEND_TEST_HOOKS)
void dl03_third_party_cannot_shadow_catalog() {
  PullHarness h;
  h.serve(fx::qwen_payload());
  const auto outcome = h.pull(h.third_party(fx::manifest_text("fixture-qwen", "t1", 9, fx::qwen_payload())));
  expect(outcome.error && outcome.error->reason == AcquireReason::manifest_shadows_catalog && outcome.exit == ExitCode::manifest, "exit 5");
}
#endif

void dl04_interrupt_during_download_never_indexed() {
  PullHarness h;
  h.serve(fx::qwen_payload());
  h.transport->after_first_chunk = [&h] { h.interrupted.store(true); };
  const auto outcome = h.pull(h.third_party(manifest()));
  expect(outcome.error && outcome.error->reason == AcquireReason::interrupted && outcome.exit == ExitCode::interrupted, "exit 8");
  expect(h.index().models.empty(), "never indexed");
  expect(h.ui.text().find("again to resume") != std::string::npos, "resume hint printed");
  expect(std::holds_alternative<secure_frontend::ModelsLock>(secure_frontend::lock_models_root(h.models)), ".lock released");
}

void dl04_interrupt_during_verification() {
  PullHarness h;
  h.serve(fx::qwen_payload());
  h.progress_interrupt = true;
  const auto outcome = h.pull(h.third_party(manifest()));
  expect(outcome.exit == ExitCode::interrupted && h.index().models.empty(), "interrupt after the last byte, before publish");
}

void dl04_stale_staging_from_other_manifest_discarded() {
  PullHarness h;
  const std::string bogus_key(32, 'a');
  expect(!secure_frontend::ensure_directory(h.models, secure_frontend::must_rel(".staging/" + bogus_key)), "bogus key dir");
  expect(!secure_frontend::write_new_file(h.models, secure_frontend::must_rel(".staging/" + bogus_key + "/MANIFEST"), "not this key's manifest"),
         "bogus MANIFEST");
  h.serve(fx::qwen_payload());
  expect(h.pull(h.third_party(manifest())).exit == ExitCode::success, "pull");
  expect(!secure_frontend::exists_within(h.models, secure_frontend::must_rel(".staging/" + bogus_key)), "stale key deleted");
  expect(h.ui.text().find("stale_staging_discarded") != std::string::npos, "and announced");
  expect(secure_frontend::exists_within(h.models, secure_frontend::must_rel(".staging/.cwd")), ".cwd is never treated as a key");
}

void abandoned_partials_kept_with_yes() {
  PullHarness h;
  const auto v1 = fx::qwen_payload("1");
  h.serve(v1, cut_first_shard(v1, 5));
  (void)h.pull(h.third_party(manifest(1, "1")));  // leaves v1 partials
  h.serve(fx::qwen_payload("2"));
  const auto outcome = h.pull(h.third_party(manifest(2, "2")));  // --yes, non-interactive
  expect(outcome.exit == ExitCode::success, "v2 installs");
  expect(h.ui.text().find("strata rm --partials m:t") != std::string::npos, "--yes never deletes; the rm command is printed");
  const auto v1_key = secure_frontend::staging_key(secure_frontend::to_hex(secure_frontend::sha256(manifest(1, "1"))));
  expect(secure_frontend::exists_within(h.models, secure_frontend::must_rel(".staging/" + v1_key)), "v1 partials kept");
}

void abandoned_staging_reported_and_offered_for_deletion() {
  PullHarness h;
  const auto v1 = fx::qwen_payload("1");
  h.serve(v1, cut_first_shard(v1, 5));
  (void)h.pull(h.third_party(manifest(1, "1")));
  h.serve(fx::qwen_payload("2"));
  h.ui.interactive = true;
  h.ui.answers = {true};
  expect(h.pull(h.third_party(manifest(2, "2"))).exit == ExitCode::success, "v2 installs");
  bool asked = false;
  for (const auto& q : h.ui.questions) asked = asked || q.find("of partial downloads for other versions of m:t? [y/N]") != std::string::npos;
  expect(asked, "the abandoned bytes are reported and offered for deletion");
  const auto v1_key = secure_frontend::staging_key(secure_frontend::to_hex(secure_frontend::sha256(manifest(1, "1"))));
  expect(!secure_frontend::exists_within(h.models, secure_frontend::must_rel(".staging/" + v1_key)), "deleted on y");
}

void all_sources_failed_exits_3_and_keeps_part() {
  PullHarness h;
  h.serve(fx::qwen_payload(), "response https://fixture.example/fx/weights/fx-00001-of-00002.gguf\nstatus 503 Busy\nend\n");
  const auto outcome = h.pull(h.third_party(manifest()));
  expect(outcome.exit == ExitCode::transport && outcome.error && outcome.error->reason == AcquireReason::http_status, "exit 3");
  expect(h.clock.sleeps.size() == 4, "503 is retried 5 times on the single source");
  // F-54: a transport failure keeps the (here empty) .part for the next run.
  expect(secure_frontend::exists_within(h.models, secure_frontend::must_rel(part_of(manifest(), "weights/fx-00001-of-00002.gguf"))),
         ".part kept");
}

void enospc_mid_stream_keeps_part_and_exits_6() {
  class Full final : public secure_frontend::StoreEnvironment {
   public:
    std::optional<std::uint64_t> fail_writes_after_bytes() const override { return 4; }
  };
  PullHarness h;
  h.serve(fx::qwen_payload());
  const Full full;
  const auto outcome = h.pull(h.third_party(manifest()), &full);
  expect(outcome.error && outcome.error->reason == AcquireReason::disk_space && outcome.exit == ExitCode::space, "exit 6");
  expect(secure_frontend::exists_within(h.models, secure_frontend::must_rel(part_of(manifest(), "weights/fx-00001-of-00002.gguf"))),
         ".part kept (resumable)");
}

void update_reuses_unchanged_files() {
  PullHarness h;
  auto v1 = fx::qwen_payload();
  h.serve(v1);
  expect(h.pull(h.third_party(fx::manifest_text("m", "t", 1, v1))).exit == ExitCode::success, "v1");
  auto v2 = v1;
  v2[2].bytes = "index v2";
  h.serve(v2);
  h.transport->argvs.clear();
  const auto outcome = h.pull(h.third_party(fx::manifest_text("m", "t", 2, v2)));
  expect(outcome.exit == ExitCode::success && h.index().models.front().sequence == 2, "v2 published");
  expect(h.transport->argvs.size() == 1, "only the changed file is downloaded");
  expect(h.ui.text().find("file changed: pack/index.txt") != std::string::npos, "manifest diff printed");
  expect(h.index().retired.empty(), "v1 retired and, being unheld, deleted (§4.5 step 6)");
}

// Final fix F1 (§8.3): Ctrl-C while an unchanged 30-55 GB shard is copied from the installed
// version stops within one chunk, not after the whole file: exit 8, nothing indexed, the
// half-copied file is discarded, and a rerun completes.
void update_reuse_copy_interrupted_mid_file_exits_8() {
  PullHarness h;
  auto v1 = fx::qwen_payload();
  v1[0].bytes = std::string(secure_frontend::kSha256ReadChunkBytes + 1, 's');  // two read chunks
  h.serve(v1);
  expect(h.pull(h.third_party(fx::manifest_text("m", "t", 1, v1))).exit == ExitCode::success, "v1");
  auto v2 = v1;
  v2[2].bytes = "index v2";
  h.serve(v2);
  h.transport->argvs.clear();
  const auto v2_manifest = fx::manifest_text("m", "t", 2, v2);
  const auto request = h.third_party(v2_manifest);
  h.mid_file_interrupt = true;
  const auto outcome = h.pull(request);
  expect(outcome.error && outcome.error->reason == AcquireReason::interrupted && outcome.exit == ExitCode::interrupted, "exit 8");
  const auto index = h.index();
  expect(index.models.size() == 1 && index.models.front().sequence == 1, "v1 stays the indexed version");
  const auto key = secure_frontend::staging_key(secure_frontend::to_hex(secure_frontend::sha256(v2_manifest)));
  const auto shard = ".staging/" + key + "/" + v1[0].path;
  expect(!secure_frontend::exists_within(h.models, secure_frontend::must_rel(shard)), "the copy stopped mid-file (never completed)");
  expect(!secure_frontend::exists_within(h.models, secure_frontend::must_rel(shard + ".part")), "the half copy is discarded");
  expect(h.transport->argvs.empty(), "nothing was downloaded");
  expect(h.ui.text().find("again to resume") != std::string::npos, "resume hint printed");
  h.mid_file_interrupt = false;
  h.interrupted.store(false);
  const auto rerun = h.pull(request);
  expect(rerun.exit == ExitCode::success && h.index().models.front().sequence == 2, "a rerun completes the update");
}

// Final fix F1 (§8.3): pull --verify re-hashes every installed file; a set flag stops it with exit 8.
void pull_verify_interrupted_exits_8() {
  PullHarness h;
  h.serve(fx::qwen_payload());
  expect(h.pull(h.third_party(manifest())).exit == ExitCode::success, "installed");
  auto request = h.third_party(manifest());
  request.verify = true;
  h.interrupted.store(true);
  const auto outcome = h.pull(request);
  expect(outcome.error && outcome.error->reason == AcquireReason::interrupted && outcome.exit == ExitCode::interrupted, "exit 8");
  expect(h.ui.text().find("verified: m:t") == std::string::npos, "never reported as verified");
  expect(h.index().models.size() == 1, "the index is unchanged");
}

void license_change_requires_interactive_yes() {
  PullHarness h;
  h.serve(fx::qwen_payload());
  expect(h.pull(h.third_party(fx::manifest_text("m", "t", 1, fx::qwen_payload()))).exit == ExitCode::success, "v1");
  std::string v2 = fx::manifest_text("m", "t", 2, fx::qwen_payload());
  v2.replace(v2.find("\"Apache-2.0\""), 12, "\"MIT\"");
  const auto outcome = h.pull(h.third_party(v2));  // --yes but not interactive
  expect(outcome.exit == ExitCode::declined, "a license change is never answered by --yes");
  expect(h.index().models.front().sequence == 1, "v1 stays active");
  h.ui.interactive = true;
  h.ui.answers = {true};
  expect(h.pull(h.third_party(v2)).exit == ExitCode::success && h.index().models.front().sequence == 2, "an interactive y accepts it");
}

void pull_without_bundle_refuses_sourceless_files() {
  PullHarness h;
  h.serve(fx::qwen_payload());
  const auto outcome = h.pull(h.third_party(manifest(1, "", false)));
  expect(outcome.error && outcome.error->reason == AcquireReason::manifest_requires_bundle && outcome.exit == ExitCode::manifest, "exit 5");
  expect(h.ui.text().find("--bundle") != std::string::npos && h.transport->argvs.empty(), "steps printed, no connection");
}

void confirmation_answers_parsed_strictly() {
  for (const auto* yes : {"y", "Y", "yes", "YES", " yes ", "y\r", "Yes\n"}) {
    expect(secure_frontend::parse_confirmation(yes), std::string{"accepts "} + yes);
  }
  for (const auto* no : {"", " ", "n", "no", "yep", "y es", "ye", "yes please", "1", "yy"}) {
    expect(!secure_frontend::parse_confirmation(no), std::string{"declines "} + no);
  }
  PullHarness h;
  h.serve(fx::qwen_payload());
  auto request = h.third_party(manifest());
  request.yes = false;
  const auto declined = h.pull(request);
  expect(declined.exit == ExitCode::declined && h.probe.calls == 0 && h.transport->argvs.empty(),
         "non-interactive without --yes is No: exit 2, nothing spawned");
  expect(h.ui.questions.size() == 1 && h.ui.questions.front().find("Download ") == 0 &&
             h.ui.questions.front().find(" from fixture.example? [y/N]") != std::string::npos,
         "one [y/N] question naming the host");
}

void already_installed_needs_no_network() {
  PullHarness h;
  h.serve(fx::qwen_payload());
  const auto request = h.third_party(manifest());
  expect(h.pull(request).exit == ExitCode::success, "installed");
  h.transport->argvs.clear();
  h.probe.calls = 0;
  const auto again = h.pull(request);
  expect(again.exit == ExitCode::success && again.already_installed && h.transport->argvs.empty() && h.probe.calls == 0, "no network");
  auto verify = request;
  verify.verify = true;
  expect(h.pull(verify).exit == ExitCode::success && h.transport->argvs.empty(), "--verify re-hashes locally");
  const auto entry = h.index().models.front();
  secure_frontend_test::write_file(h.models.join(secure_frontend::must_rel("m/t/" + entry.dir + "/pack/index.txt")), "rotten");
  expect(h.pull(verify).exit == ExitCode::verification, "--verify detects bit rot");
}

// §8.3: pull.max_total_bytes is total_budget_exceeded (exit 6, before anything is written);
// pull.max_duration_s is time_budget_exceeded (exit 3, resumable).
void pull_budgets_map_to_exit_classes() {
  {
    PullHarness h;
    h.serve(fx::qwen_payload());
    h.settings.max_total_bytes = 16;
    const auto outcome = h.pull(h.third_party(manifest()));
    expect(outcome.error && outcome.error->reason == AcquireReason::total_budget_exceeded && outcome.exit == ExitCode::space,
           "total budget: exit 6");
    expect(h.transport->argvs.empty() && h.probe.calls == 0, "no connection");
  }
  {
    PullHarness h;
    h.serve(fx::qwen_payload());
    h.settings.max_duration = std::chrono::seconds{0};
    const auto outcome = h.pull(h.third_party(manifest()));
    expect(outcome.error && outcome.error->reason == AcquireReason::time_budget_exceeded && outcome.exit == ExitCode::transport &&
               secure_frontend::reason_info(outcome.error->reason).resumable,
           "time budget: exit 3, resumable");
    expect(h.ui.text().find("again to resume") != std::string::npos, "resume hint");
  }
}

// F-56: --discard-partial and staging tidying never run before the [y/N] answer.
void discard_partial_runs_only_after_confirmation() {
  const auto payload = fx::qwen_payload();
  PullHarness h;
  h.serve(payload, cut_first_shard(payload, 5));
  auto request = h.third_party(manifest());
  (void)h.pull(request);
  const auto part = secure_frontend::must_rel(part_of(manifest(), payload[0].path));
  expect(secure_frontend::exists_within(h.models, part), "a 5-byte .part was left");
  request.discard_partial = true;
  request.yes = false;
  h.serve(payload);
  expect(h.pull(request).exit == ExitCode::declined, "declined");
  expect(secure_frontend::exists_within(h.models, part), "a declined pull deletes nothing");
  request.yes = true;
  expect(h.pull(request).exit == ExitCode::success, "accepted: the own staging is discarded and fetched from byte 0");
  for (const auto& argv : h.transport->argvs) expect(argv[argv.size() - 2] != "--range", "no range request after --discard-partial");
}

// Fix round 1: an installed version that cannot be loaded has an UNKNOWN license, so the
// update asks with yes_applies=false (§6.1): --yes alone declines; an interactive y proceeds.
void unreadable_installed_license_requires_interactive_yes() {
  PullHarness h;
  const auto v1 = fx::qwen_payload();
  h.serve(v1);
  expect(h.pull(h.third_party(fx::manifest_text("m", "t", 1, v1))).exit == ExitCode::success, "v1");
  const auto entry = h.index().models.front();
  secure_frontend_test::write_file(h.models.join(secure_frontend::must_rel("m/t/" + entry.dir + "/manifest.json")), "rotten");
  auto v2 = v1;
  v2[2].bytes = "index v2";
  const auto v2_text = fx::manifest_text("m", "t", 2, v2);
  h.serve(v2);
  h.probe.calls = 0;
  const auto declined = h.pull(h.third_party(v2_text));  // --yes, non-interactive
  expect(declined.exit == ExitCode::declined && declined.error && declined.error->reason == AcquireReason::declined,
         "--yes does not answer an unknown installed license: exit 2");
  expect(h.transport->argvs.empty() && h.probe.calls == 0, "nothing spawned");
  const auto v2_key = secure_frontend::staging_key(secure_frontend::to_hex(secure_frontend::sha256(v2_text)));
  expect(!secure_frontend::exists_within(h.models, secure_frontend::must_rel(".staging/" + v2_key)), "nothing staged");
  expect(h.index().models.front().sequence == 1, "index unchanged");
  expect(h.ui.text().find("installed version of m:t cannot be read") != std::string::npos, "the reason is printed");
  h.ui.interactive = true;
  h.ui.answers = {true};
  expect(h.pull(h.third_party(v2_text)).exit == ExitCode::success && h.index().models.front().sequence == 2, "an interactive y proceeds");
}

#if defined(SECURE_FRONTEND_TEST_HOOKS)
namespace {
class FailVersionRenameOnce final : public secure_frontend::StoreEnvironment {
 public:
  [[nodiscard]] bool fail_version_rename() const override { return !std::exchange(failed_, true); }

 private:
  mutable bool failed_{false};
};
}  // namespace

// Final review I1 (§8.2 publish_failed: rerun resumes): a publish that fails after step 2
// leaves .staging/<k>/manifest.json, not MANIFEST; the rerun still trusts the key (its digest
// equals the key) and publishes without a single transfer.
void publish_failure_after_step_2_resumes_without_download() {
  PullHarness h;
  h.serve(fx::qwen_payload());
  const auto request = h.third_party(manifest());
  const FailVersionRenameOnce once;
  const auto first = h.pull(request, &once);
  expect(first.error && first.error->reason == AcquireReason::publish_failed, "step 4 fails once: publish_failed");
  const auto key = secure_frontend::staging_key(secure_frontend::to_hex(secure_frontend::sha256(manifest())));
  expect(secure_frontend::exists_within(h.models, secure_frontend::must_rel(".staging/" + key + "/manifest.json")) &&
             !secure_frontend::exists_within(h.models, secure_frontend::must_rel(".staging/" + key + "/MANIFEST")),
         "step 2 ran: the staged manifest is manifest.json");
  const auto transfers = h.transport->argvs.size();
  const auto probes = h.probe.calls;
  const auto second = h.pull(request);
  expect(second.exit == ExitCode::success && h.index().models.size() == 1, "the rerun publishes: " + h.ui.text());
  expect(h.transport->argvs.size() == transfers && h.probe.calls == probes, "with zero transfers");
  expect(h.ui.text().find("stale_staging_discarded") == std::string::npos, "the staged download is never discarded as stale");
  expect(!secure_frontend::exists_within(h.models, secure_frontend::must_rel(".staging/" + key)), "the key was published");
}

// F-51: the hooks-only CA bundle reaches every hop's argv through run_pull.
void pull_test_cacert_reaches_argv_in_hook_builds() {
  PullHarness h;
  h.serve(fx::qwen_payload());
  auto request = h.third_party(manifest());
  request.test_cacert = "/fixture/ca.pem";
  expect(h.pull(request).exit == ExitCode::success, "pull");
  expect(!h.transport->argvs.empty(), "hops ran");
  for (const auto& argv : h.transport->argvs) {
    expect(argv.back() == "/fixture/ca.pem" && argv[argv.size() - 2] == "--cacert", "--cacert on every hop");
  }
}
#endif

}  // namespace acquire_cases
