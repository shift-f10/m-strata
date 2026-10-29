// SP4 Task 27: the strata network verbs — pull, run (D6/C6), update --models, doctor —
// with §8.3 interrupts and §7.3 acquire.log records.
#include "secure_frontend/catalog.hpp"
#include "secure_frontend/crt_environment.hpp"
#include "secure_frontend/acquire_url.hpp"
#include "secure_frontend/download_response.hpp"
#include "secure_frontend/downloader.hpp"
#include "secure_frontend/engine_gate.hpp"
#include "secure_frontend/fetch_file.hpp"
#include "secure_frontend/hf_source.hpp"
#include "secure_frontend/json.hpp"
#include "secure_frontend/sha256.hpp"
#include "secure_frontend/strata_cli.hpp"
#include "secure_frontend/token_reader.hpp"
#include "doctor_checks.hpp"
#include "release_manifest.hpp"
#include "service_ops.hpp"
#include "uninstall_plan.hpp"
#include "strata_cli_internal.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cwchar>
#include <fstream>
#include <limits>
#include <ostream>
#include <system_error>

#if defined(_WIN32)
#include "secure_frontend/win32_prelude.hpp"
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace secure_frontend {
namespace {

#if !defined(_WIN32)
}  // namespace
extern "C" char** environ;  // POSIX process environment (presence scan only)
namespace {
#endif

using namespace strata_cli_detail;

std::atomic<bool> g_interrupted{false};

#if defined(_WIN32)
BOOL WINAPI on_console_event(DWORD) {
  g_interrupted.store(true);
  return TRUE;  // handled: run_pull winds down, the downloader terminates its Job, exit 8
}
#else
void on_signal(int) { g_interrupted.store(true); }
#endif

// §8.3: SIGINT/SIGTERM (Windows: console Ctrl-C/Ctrl-Break/close) stop cleanly. Active for the
// whole network verb. At a pending [y/N] the handler (no SA_RESTART) ends the blocked read, and
// do_pull reports `interrupted` (exit 8) rather than `declined` (§8.2); during a transfer the
// downloader sees the flag, kills its process group / Job, and run_pull returns `interrupted`
// with the resume hint.
class InterruptScope final {
 public:
  InterruptScope() {
    g_interrupted.store(false);
#if defined(_WIN32)
    SetConsoleCtrlHandler(on_console_event, TRUE);
#else
    struct sigaction action {};
    action.sa_handler = on_signal;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, &old_int_);
    sigaction(SIGTERM, &action, &old_term_);
#endif
  }
  ~InterruptScope() {
#if defined(_WIN32)
    SetConsoleCtrlHandler(on_console_event, FALSE);
#else
    sigaction(SIGINT, &old_int_, nullptr);
    sigaction(SIGTERM, &old_term_, nullptr);
#endif
  }
  InterruptScope(const InterruptScope&) = delete;
  InterruptScope& operator=(const InterruptScope&) = delete;

 private:
#if !defined(_WIN32)
  struct sigaction old_int_ {};
  struct sigaction old_term_ {};
#endif
};

struct NetworkKit final {
  std::unique_ptr<ConfinedDownloader> downloader;
  std::unique_ptr<HopTransport> transport_owned;
  std::unique_ptr<DownloaderProbe> probe_owned;
  std::unique_ptr<Resolver> resolver;
  std::unique_ptr<LoadedVersions> loaded;
  SystemClock system_clock;
  HopTransport* transport{nullptr};
  DownloaderProbe* probe{nullptr};
  AcquireClock* clock{nullptr};
  const std::atomic<bool>* interrupted{nullptr};
};

std::vector<ProtectedPath> protected_paths(const FrontendConfig& config, const std::filesystem::path& config_file) {
  std::vector<ProtectedPath> paths;
  std::error_code ec;
  const auto canonical_config = std::filesystem::canonical(config_file, ec);
  if (!ec) paths.push_back({"config file", canonical_config.generic_string()});
  paths.push_back({"roots.models", config.roots.models.path().generic_string()});
  if (config.roots.secret) paths.push_back({"roots.secret", config.roots.secret->path().generic_string()});
  return paths;
}

// Spec §3.2 cwd row (ruling B1): every downloader spawn — probe or transfer, also for
// pull --bundle and doctor --downloader — runs in roots.models/.staging/.cwd, so it must exist.
std::optional<AcquireError> ensure_downloader_cwd(const CanonicalRoot& models) {
  if (auto failure = ensure_directory(models, must_rel(std::string{kStagingDirName}))) return failure;
  return make_read_only_directory(models, must_rel(std::string{kStagingDirName} + "/" + std::string{kStagingCwdName}));
}

// `will_spawn`: the verb may run the downloader (a pull, doctor --downloader); plain doctor
// only asks for the confinement label and writes nothing.
std::optional<AcquireError> fill_kit(NetworkKit& kit, const FrontendConfig& config, const std::filesystem::path& config_file,
                                     CliServices& services, const bool will_spawn) {
  kit.interrupted = services.interrupted != nullptr ? services.interrupted : &g_interrupted;
  if (services.transport && services.probe) {
    kit.transport_owned = services.transport(config);
    kit.probe_owned = services.probe(config);
    kit.transport = kit.transport_owned.get();
    kit.probe = kit.probe_owned.get();
  } else {
    if (will_spawn) {
      if (auto failure = ensure_downloader_cwd(config.roots.models)) return failure;
    }
    DownloaderOptions options;
    options.executable = config.pull.downloader;
    options.protected_paths = protected_paths(config, config_file);
    options.cwd = config.roots.models.path() / std::string{kStagingDirName} / std::string{kStagingCwdName};
    if (config.pull.proxy) options.proxy_port = config.pull.proxy->port;
    options.interrupted = kit.interrupted;
#if defined(SECURE_FRONTEND_TEST_HOOKS)
    options.record_file = services.test_record_file;
    options.test_force_no_landlock = services.test_force_no_landlock;
#endif
    kit.downloader = std::make_unique<ConfinedDownloader>(std::move(options));
    kit.transport = kit.downloader.get();
    kit.probe = kit.downloader.get();
  }
  kit.resolver = services.resolver ? services.resolver() : std::make_unique<SystemResolver>();
  kit.loaded = loaded_for(config, services);
  kit.clock = services.clock != nullptr ? services.clock : &kit.system_clock;
  return std::nullopt;
}

std::string utc_now() {
  const std::time_t now = std::time(nullptr);
  std::tm parts{};
#if defined(_WIN32)
  gmtime_s(&parts, &now);
#else
  gmtime_r(&now, &parts);
#endif
  char buffer[32];
  std::strftime(buffer, sizeof buffer, "%Y-%m-%dT%H:%M:%SZ", &parts);
  return buffer;
}

// §7.3: one record per transferred file in <roots.logs>/acquire.log when roots.logs is set
// (off by default). Fixed fields only: relative path, size, digest, host, result, confinement —
// never a URL, address, proxy or config value. A log failure never fails the pull (I14).
// Model downloads (plan 2026-10-23-model-downloads Task 3): pull --progress-lines. stdout carries
// only fixed one-line records the server reads; notices go to stderr. Paths are the manifest's
// relative paths; BYTES at most every 250 ms (and at the end of a file).
class LineProgress final : public ProgressSink {
 public:
  explicit LineProgress(CliStreams& io) : io_(io) {}
  void notice(const std::string_view line) override { io_.err << line << '\n'; }
  void manifest_selected(const std::string_view name_tag, const std::string_view sha256) override {
    io_.out << "MANIFEST " << name_tag << ' ' << sha256 << '\n' << std::flush;
  }
  void file_started(const std::string_view path, const std::uint64_t total) override {
    io_.out << "FILE " << path << ' ' << total << '\n' << std::flush;
    last_ = {};
  }
  void file_bytes(const std::string_view path, const std::uint64_t done, const std::uint64_t total) override {
    const auto now = std::chrono::steady_clock::now();
    if (done != total && now - last_ < std::chrono::milliseconds(250)) return;
    last_ = now;
    io_.out << "BYTES " << path << ' ' << done << ' ' << total << '\n' << std::flush;
  }
  void file_finished(const std::string_view path, const std::uint64_t, const std::string_view, const std::string_view,
                     const std::string_view result) override {
    io_.out << "FINISHED " << path << ' ' << result << '\n' << std::flush;
  }

 private:
  CliStreams& io_;
  std::chrono::steady_clock::time_point last_{};
};

class LoggingProgress final : public ProgressSink {
 public:
  LoggingProgress(ProgressSink& inner, const FrontendConfig& config, std::string verb, std::string confinement)
      : inner_(inner), config_(config), verb_(std::move(verb)), confinement_(std::move(confinement)) {}
  void notice(const std::string_view line) override { inner_.notice(line); }
  void manifest_selected(const std::string_view name_tag, const std::string_view sha256) override {
    model_ = std::string{name_tag};
    manifest_ = std::string{sha256};
    inner_.manifest_selected(name_tag, sha256);
  }
  void file_started(const std::string_view path, const std::uint64_t total) override { inner_.file_started(path, total); }
  void file_bytes(const std::string_view path, const std::uint64_t done, const std::uint64_t total) override {
    inner_.file_bytes(path, done, total);
  }
  void file_finished(const std::string_view path, const std::uint64_t size, const std::string_view sha256, const std::string_view host,
                     const std::string_view result) override {
    inner_.file_finished(path, size, sha256, host, result);
    if (!config_.roots.logs) return;
    auto root = open_root(*config_.roots.logs, RootPolicy::create_private);
    if (!std::holds_alternative<CanonicalRoot>(root)) return;
    const auto& logs = std::get<CanonicalRoot>(root);
    const StoreEnvironment env;
    const auto file = must_rel("acquire.log");
    auto opened = open_part_file(logs, file, exists_within(logs, file) ? PartOpen::open_existing : PartOpen::create_new, env);
    if (!std::holds_alternative<PartFile>(opened)) return;
    const std::string line = utc_now() + " verb=" + verb_ + " model=" + model_ + " manifest=" + manifest_ + " path=" + std::string{path} +
                             " size=" + std::to_string(size) + " sha256=" + std::string{sha256} + " host=" + std::string{host} +
                             " result=" + std::string{result} + " confinement=" + confinement_ + "\n";
    (void)std::get<PartFile>(opened).append(line);
  }

 private:
  ProgressSink& inner_;
  const FrontendConfig& config_;
  std::string verb_;
  std::string confinement_;
  std::string model_;
  std::string manifest_;
};

int do_pull(PullRequest request, const std::string& verb, const FrontendConfig& config, const std::filesystem::path& config_file,
            CliStreams& io, CliServices& services, const bool progress_lines = false) {
#if defined(SECURE_FRONTEND_TEST_HOOKS)
  request.test_cacert = services.test_cacert;  // F-51 / ruling R1: a test config only; never a CLI option
#endif
  // --progress-lines: the people-facing text (summary, notices, refusals) goes to stderr; stdout
  // carries only LineProgress's records and the final RESULT.
  CliStreams people{io.in, progress_lines ? io.err : io.out, io.err, io.interactive};
  const auto finish = [&io, progress_lines](const std::optional<AcquireError>& error) {
    if (progress_lines) io.out << "RESULT " << (error ? reason_name(error->reason) : std::string_view{"ok"}) << '\n' << std::flush;
  };
  NetworkKit kit;
  if (auto failure = fill_kit(kit, config, config_file, services, true)) {
    finish(failure);
    return report(people, *failure);
  }
  StreamConfirmer ui(people);
  StreamProgress stream(people);
  LineProgress lines(io);
  LoggingProgress progress(progress_lines ? static_cast<ProgressSink&>(lines) : static_cast<ProgressSink&>(stream), config, verb,
                           kit.probe->confinement());
  PullServices pull_services{*kit.transport, *kit.probe, *kit.resolver, *kit.clock, store_for(services), *kit.loaded, ui, progress,
                             *kit.interrupted};
  const auto outcome = run_pull(request, config.roots.models, config.pull, pull_services);
  if (outcome.error && outcome.error->reason == AcquireReason::declined && kit.interrupted->load()) {
    // §8.2 (controller ruling): a signal at a pending [y/N] is an interrupt, never a No. EOF
    // (Ctrl-D) leaves the flag clear and stays `declined` (exit 2). Nothing was spawned: the
    // probe runs only after a yes.
    const AcquireError interrupted{AcquireReason::interrupted, outcome.error->detail};
    finish(interrupted);
    return report(people, interrupted);
  }
  finish(outcome.error);
  if (outcome.error) return report(people, *outcome.error);
  return exit_code_value(outcome.exit);
}

int verb_pull(const FrontendConfig& config, const std::filesystem::path& config_file, const std::span<const std::string> rest,
              CliStreams& io, CliServices& services) {
  const auto args = parse_args(rest, {"--yes", "--allow-downgrade", "--verify", "--discard-partial", "--progress-lines"},
                               {"--bundle", "--manifest", "--trust-manifest"});
  if (!args) return usage(io, "pull: unknown or incomplete option");
  bool bad = false;
  PullRequest request;
  request.trust_digest = trust_option(*args, io, bad);
  if (bad) return exit_code_value(ExitCode::usage);
  request.yes = args->flags.contains("--yes");
  request.allow_downgrade = args->flags.contains("--allow-downgrade");
  request.verify = args->flags.contains("--verify");
  request.discard_partial = args->flags.contains("--discard-partial");
  if (const auto it = args->options.find("--bundle"); it != args->options.end()) request.bundle_dir = path_from_utf8(it->second);
  if (const auto it = args->options.find("--manifest"); it != args->options.end()) {
    if (!args->positional.empty()) return usage(io, "pull --manifest takes no <name[:tag]>");
    request.manifest_file = path_from_utf8(it->second);
  } else {
    if (args->positional.size() != 1) return usage(io, "pull takes one <name[:tag]>");
    request.reference = args->positional[0];
  }
  return do_pull(std::move(request), "pull", config, config_file, io, services, args->flags.contains("--progress-lines"));
}

// The tail both run branches share: the engine gate, then where to start the server.
int model_ready(const FrontendConfig& config, const std::filesystem::path& config_file, const IndexEntry& entry, CliStreams& io) {
  auto model = load_installed(config.roots.models, entry);
  if (auto* failure = std::get_if<AcquireError>(&model)) return report(io, *failure);
  if (auto gate = engine_gate(std::get<InstalledModel>(model).manifest.manifest)) return report(io, *gate);
  io.out << "model ready: " << model_ref_text(entry.name, entry.tag) << "\nstart the server with: strata-unified-server --config "
         << path_to_utf8(config_file) << '\n';
  return 0;
}

int verb_run(const FrontendConfig& config, const std::filesystem::path& config_file, const std::span<const std::string> rest,
             CliStreams& io, CliServices& services) {
  const auto args = parse_args(rest, {"--yes"}, {});
  if (!args || args->positional.size() != 1) return usage(io, "run takes one <name[:tag]>");
  const std::string& text = args->positional[0];
  const auto reference = parse_model_reference(text);
  if (!reference) return usage(io, "run takes <name[:tag]>");
  auto found = find_installed_text(config.roots.models, text);
  if (auto* entry = std::get_if<IndexEntry>(&found)) return model_ready(config, config_file, *entry, io);
  const auto& missing = std::get<AcquireError>(found);
  if (missing.reason != AcquireReason::model_unknown) return report(io, missing);
  // D6/C6: the same in-process pull flow after the same confirmation.
  const auto* catalog = find_catalog_entry(reference->name, reference->tag.value_or("default"));
  if (catalog == nullptr) {
    const int code = report(io, AcquireError{AcquireReason::model_unknown, text});  // ruling R11
    io.err << "strata: it is not installed and not in this release's catalog; install it with strata import <bundle> "
              "or strata pull --manifest <file> --trust-manifest sha256:<hex>\n";
    return code;
  }
  const std::string ref = model_ref_text(catalog->name, catalog->tag);
  io.out << ref << " is not installed.\n";
  PullRequest request;
  request.reference = ref;
  request.yes = args->flags.contains("--yes");
  if (const int pulled = do_pull(std::move(request), "run", config, config_file, io, services); pulled != 0) return pulled;
  auto reread = read_model_index(config.roots.models);
  if (auto* failure = std::get_if<AcquireError>(&reread)) return report(io, *failure);
  const auto* installed = find_model(std::get<ModelIndex>(reread), catalog->name, catalog->tag);
  if (installed == nullptr) return report(io, AcquireError{AcquireReason::index_inconsistent, ref});
  return model_ready(config, config_file, *installed, io);
}

int verb_update_models(const FrontendConfig& config, const std::filesystem::path& config_file, const std::span<const std::string> rest,
                       CliStreams& io, CliServices& services) {
  const auto args = parse_args(rest, {"--models", "--yes"}, {});
  if (!args || !args->flags.contains("--models")) {
    return usage(io, "update --models [<name:tag> ...] [--yes]; plain 'strata update' is the program update");
  }
  auto read = read_model_index(config.roots.models);
  if (auto* failure = std::get_if<AcquireError>(&read)) return report(io, *failure);
  std::vector<std::string> targets;
  for (const auto& entry : std::get<ModelIndex>(read).models) {
    const std::string ref = model_ref_text(entry.name, entry.tag);
    const bool named = args->positional.empty() || std::find(args->positional.begin(), args->positional.end(), ref) != args->positional.end();
    // §6.1: answered offline from the catalog compiled into this binary.
    const auto* newer = entry.origin == ModelOrigin::catalog ? find_catalog_entry(entry.name, entry.tag) : nullptr;
    if (named && newer != nullptr && newer->sequence > entry.sequence) targets.push_back(ref);
  }
  if (targets.empty()) {
    io.out << "nothing to update (third-party models update only with strata pull --manifest ... --trust-manifest ...)\n";
    return 0;
  }
  int first_failure = 0;
  for (const auto& ref : targets) {
    PullRequest request;
    request.reference = ref;
    request.yes = args->flags.contains("--yes");
    const int code = do_pull(std::move(request), "update", config, config_file, io, services);
    // §8.3 (final fix F4): an interrupt stops the remaining targets and is the verb's exit (8),
    // even when an earlier target failed for another reason.
    if (code == exit_code_value(ExitCode::interrupted)) return code;
    if (code != 0 && first_failure == 0) first_failure = code;
  }
  return first_failure;
}

bool variable_present(const char* name) {
#if defined(_WIN32)
  const std::wstring wide = path_from_utf8(name).wstring();
  return GetEnvironmentVariableW(wide.c_str(), nullptr, 0) != 0;
#else
  return secure_frontend::crt_getenv(name) != nullptr;  // presence only: the value is never read further, printed or stored
#endif
}

// --- SP7 M1 Task 3 (spec D-7): production wiring of the doctor ProbeBundle ---
// Self path, stat, rlimit and bind probes only. Nothing installs and nothing
// touches the network (PX §8); the only process run is the read-only
// nvidia-smi query through service::run_argv.

std::filesystem::path self_program_dir() {
#if defined(_WIN32)
  std::vector<wchar_t> buffer(32768);
  const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
  if (length == 0 || length >= buffer.size()) return {};
  return std::filesystem::path(std::wstring(buffer.data(), length)).parent_path();
#else
  char buffer[4096];
  const ssize_t length = readlink("/proc/self/exe", buffer, sizeof buffer - 1U);
  if (length <= 0) return {};
  buffer[length] = '\0';
  return std::filesystem::path(buffer).parent_path();
#endif
}

std::optional<unsigned> regular_file_mode(const std::filesystem::path& path) {
  std::error_code ec;
  if (std::filesystem::symlink_status(path, ec).type() != std::filesystem::file_type::regular || ec) return std::nullopt;
  const auto permissions = std::filesystem::status(path, ec).permissions();
  if (ec) return std::nullopt;
  return static_cast<unsigned>(permissions & std::filesystem::perms{0777});
}

std::optional<std::uint64_t> current_memlock() {
#if defined(_WIN32)
  return std::nullopt;  // Windows large pages ride the user right; the verb marks the check not applicable (R-T3-c)
#else
  struct rlimit limit {};
  if (getrlimit(RLIMIT_MEMLOCK, &limit) != 0) return std::nullopt;
  if (limit.rlim_cur == RLIM_INFINITY) return std::numeric_limits<std::uint64_t>::max();
  return static_cast<std::uint64_t>(limit.rlim_cur);
#endif
}

// One bind attempt on the configured listener address: EADDRINUSE and every
// other failure read as "not free" (fail closed).
bool listener_port_free(const FrontendConfig& config) {
  const bool is_v4 = config.listener.host.family() == IpFamily::v4;
#if defined(_WIN32)
  WSADATA wsa{};
  if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
  const SOCKET probe = WSASocketW(is_v4 ? AF_INET : AF_INET6, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, 0);
  const auto close_probe = [](SOCKET s) { closesocket(s); WSACleanup(); };
#else
  const int probe = ::socket(is_v4 ? AF_INET : AF_INET6, SOCK_STREAM, 0);
  const auto close_probe = [](int s) { ::close(s); };
#endif
#if defined(_WIN32)
  if (probe == INVALID_SOCKET) return false;
#else
  if (probe < 0) return false;
#endif
  bool free = false;
  if (is_v4) {
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(config.listener.port);
    std::memcpy(&address.sin_addr, config.listener.host.bytes().data(), 4U);
    free = ::bind(probe, reinterpret_cast<const sockaddr*>(&address), sizeof address) == 0;
  } else {
    sockaddr_in6 address{};
    address.sin6_family = AF_INET6;
    address.sin6_port = htons(config.listener.port);
    std::memcpy(address.sin6_addr.s6_addr, config.listener.host.bytes().data(), 16U);
    free = ::bind(probe, reinterpret_cast<const sockaddr*>(&address), sizeof address) == 0;
  }
  close_probe(probe);
  return free;
}

std::optional<std::filesystem::path> token_file_below_secret(const FrontendConfig& config) {
  if (config.token.kind() != SecretSourceKind::protected_file || !config.roots.secret) return std::nullopt;
  const auto file = parse_relative_path(config.token.location());
  if (const auto* relative = std::get_if<RelativePath>(&file)) return config.roots.secret->join(*relative);
  return std::nullopt;
}

std::vector<doctor::RootProbe> root_probes(const FrontendConfig& config) {
  std::vector<doctor::RootProbe> probes;
  const auto add = [&probes](const std::string_view label, const std::filesystem::path& path, const RootPolicy policy) {
    const auto opened = open_root(path, policy);
    if (std::holds_alternative<CanonicalRoot>(opened)) probes.push_back({std::string{label}, ""});
    else probes.push_back({std::string{label}, std::get<PathError>(opened).message});
  };
  add("roots.models", config.roots.models.path(), RootPolicy::existing);
  if (config.roots.secret) add("roots.secret", config.roots.secret->path(), RootPolicy::existing);
  // R-T3-d: roots.state is only checked when it exists (serve creates/uses it;
  // a pull-only box may legitimately have no state root yet). logs/cache use
  // SP1's own creatable-but-may-be-absent rule.
  if (config.roots.state && std::filesystem::exists(*config.roots.state)) add("roots.state", *config.roots.state, RootPolicy::existing);
  const auto add_creatable = [&probes](const std::string_view label, const std::filesystem::path& path) {
    const auto checked = check_creatable_root(path);
    if (std::holds_alternative<std::filesystem::path>(checked)) probes.push_back({std::string{label}, ""});
    else probes.push_back({std::string{label}, std::get<PathError>(checked).message});
  };
  if (config.roots.logs) add_creatable("roots.logs", *config.roots.logs);
  if (config.roots.cache) add_creatable("roots.cache", *config.roots.cache);
  return probes;
}

std::vector<std::string> deep_digest_failures(const FrontendConfig& config) {
  std::vector<std::string> failures;
  const auto read = read_model_index(config.roots.models);
  if (const auto* failure = std::get_if<AcquireError>(&read)) {
    failures.push_back(std::string{"index: "} + std::string{reason_name(failure->reason)});
    return failures;
  }
  for (const auto& entry : std::get<ModelIndex>(read).models) {
    const auto model = load_installed(config.roots.models, entry);  // the verify path: manifest re-hash, fails closed
    if (const auto* failure = std::get_if<AcquireError>(&model)) {
      failures.push_back(model_ref_text(entry.name, entry.tag) + ": " + std::string{reason_name(failure->reason)});
    }
  }
  return failures;
}

// SP5 M1 (spec §4): the process environment never carries engine knobs.
// Presence of any STRATA_*/MAYA_* name is reported once by doctor; values are
// never read, printed or stored.
bool engine_env_knob_present() {
  const auto hits_prefix = [](std::string_view name) {
    return name.rfind("STRATA_", 0) == 0 || name.rfind("MAYA_", 0) == 0;
  };
#if defined(_WIN32)
  const auto* block = GetEnvironmentStringsW();
  if (block == nullptr) return false;
  struct FreeBlock {
    wchar_t* p;
    ~FreeBlock() { FreeEnvironmentStringsW(p); }
  } free_block{const_cast<wchar_t*>(block)};
  for (const wchar_t* entry = block; *entry != L'\0'; entry += std::wcslen(entry) + 1U) {
    std::string name;
    for (const wchar_t* c = entry; *c != L'\0' && *c != L'='; ++c) {
      name.push_back(*c < 128 ? static_cast<char>(*c) : '?');
    }
    if (hits_prefix(name)) return true;
  }
  return false;
#else
  for (char* const* entry = environ; entry != nullptr && *entry != nullptr; ++entry) {
    const std::string_view text{*entry};
    if (hits_prefix(text.substr(0, text.find('=')))) return true;
  }
  return false;
#endif
}

int verb_doctor(const FrontendConfig& config, const std::filesystem::path& config_file, const std::span<const std::string> rest,
                CliStreams& io, CliServices& services) {
  const auto args = parse_args(rest, {"--downloader", "--deep"}, {});
  if (!args || !args->positional.empty()) return usage(io, "doctor [--downloader] [--deep]");
  if (engine_env_knob_present()) {
    io.out << "doctor: ignoring STRATA_*/MAYA_* environment; configure server.json instead\n";
  }
  const bool probe = args->flags.contains("--downloader");
  NetworkKit kit;
  if (auto failure = fill_kit(kit, config, config_file, services, probe)) return report(io, *failure);
#if !defined(_WIN32)
  const int abi = landlock_abi();
  io.out << (abi > 0 ? "landlock: ABI " + std::to_string(abi) : std::string{"landlock: unavailable"}) << '\n';
#endif
  io.out << "confinement: " << kit.probe->confinement() << '\n';
  // §3.8: presence only.
  if (!config.pull.proxy && (variable_present("HTTPS_PROXY") || variable_present("https_proxy") || variable_present("ALL_PROXY"))) {
    io.out << "hint: a proxy variable is set in this environment; strata never reads it. Set pull.proxy to use a proxy.\n";
  }
  auto read = read_model_index(config.roots.models);
  if (auto* failure = std::get_if<AcquireError>(&read)) return report(io, *failure);
  if (const int code = print_store_usage(config.roots.models, std::get<ModelIndex>(read), io); code != 0) return code;
  // SP7 M1 Task 3 (D-7): the full offline check set. The §3.2 "plain doctor spawns
  // nothing" rule is superseded for the read-only nvidia-smi query (D-7 names it);
  // nothing installs and no network is touched (PX §8). Exit is 0 iff every check
  // passed (R-T3-b; the fixed verdict line is what scripts and tests key on).
  doctor::ProbeBundle bundle;
  // The config outlives this verb; the bundle borrows it (aliasing shared_ptr,
  // never owns).
  bundle.config = std::shared_ptr<const FrontendConfig>(std::shared_ptr<void>{}, std::addressof(config));
  bundle.nvidia_smi = []() -> std::optional<std::string> {
    const auto result = service::run_argv({"nvidia-smi", "--query-gpu=driver_version", "--format=csv,noheader"});
    if (result.exit_code != 0) return std::nullopt;
    return result.out;
  };
  bundle.file_mode = regular_file_mode;
  bundle.program_dir = self_program_dir();
  // where the engine loads cuBLAS from: its own folder (a cards config names it models.default_engine), then
  // engine.lib_dirs (the README audit, owner 2026-10-28)
  if (config.models.default_engine) bundle.engine_dirs.push_back(config.models.default_engine->parent_path());
  if (!config.engine.executable.path().empty()) bundle.engine_dirs.push_back(config.engine.executable.path().parent_path());
  for (const auto& folder : config.engine.lib_dirs) bundle.engine_dirs.push_back(folder);
  bundle.token_file = token_file_below_secret(config);
#if defined(_WIN32)
  bundle.file_modes_apply = false;  // the token file is private by its ACL: ask the store, as the server reads it
  bundle.token_store_error = [&config]() -> std::optional<std::string> {
    if (!config.roots.secret) return std::string{"roots.secret is not configured"};
    const auto read = read_token(config.token, *config.roots.secret);
    if (const auto* error = std::get_if<TokenReadError>(&read)) return error->message;
    return std::nullopt;
  };
#endif
  bundle.roots = root_probes(config);
  bundle.port_free = [&config] { return listener_port_free(config); };
  bundle.memlock_limit = current_memlock;
#if defined(_WIN32)
  bundle.memlock_applicable = false;  // Windows large pages: the user right, not RLIMIT_MEMLOCK (R-T3-c)
#endif
  bundle.deep = args->flags.contains("--deep");
  bundle.deep_digest_failures = [&config] { return deep_digest_failures(config); };
  bool all_checks_passed = true;
  for (const auto& check : doctor::run_checks(bundle)) {
    if (check.ok) io.out << "doctor " << check.name << ": ok\n";
    else {
      all_checks_passed = false;
      io.out << "doctor " << check.name << ": " << check.human_fix << '\n';
    }
  }
  io.out << (all_checks_passed ? "doctor: all checks passed\n" : "doctor: one or more checks failed\n");
  if (!probe) return all_checks_passed ? 0 : 1;
  if (auto failure = kit.probe->probe()) return report(io, *failure);
  if (kit.downloader && kit.downloader->probe_result()) {
    const auto& result = *kit.downloader->probe_result();
    io.out << "downloader: curl " << result.major << "." << result.minor << "." << result.patch << " (" << result.tls_backend << ")\n";
  }
  if (config.pull.downloader != default_downloader_path()) {
    io.out << "note: a custom pull.downloader that reads its CA bundle from SSL_CERT_FILE fails with tls_failed under the empty environment\n";
  }
  return all_checks_passed ? 0 : 1;
}

// --- SP7 M1 Task 5 (spec D-5/D-6): the service verbs and uninstall in production ---

// The user config home (R-T2-a): XDG_CONFIG_HOME when set and non-empty, else
// HOME/.config; Windows has no systemd and the task branch (Task 6) will use
// its own location — the plans answer "not wired yet" until then.
std::filesystem::path user_config_home() {
  if (const char* xdg = secure_frontend::crt_getenv("XDG_CONFIG_HOME"); xdg != nullptr && xdg[0] != '\0') return std::filesystem::path{xdg};
  if (const char* home = secure_frontend::crt_getenv("HOME"); home != nullptr && home[0] != '\0') return std::filesystem::path{home} / ".config";
#if defined(_WIN32)
  if (const char* appdata = secure_frontend::crt_getenv("APPDATA"); appdata != nullptr && appdata[0] != '\0') return std::filesystem::path{appdata};
#endif
  return {};
}

std::filesystem::path self_executable() {
#if defined(_WIN32)
  std::vector<wchar_t> buffer(32768);
  const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
  if (length == 0 || length >= buffer.size()) return {};
  return std::filesystem::path(std::wstring(buffer.data(), length));
#else
  char buffer[4096];
  const ssize_t length = readlink("/proc/self/exe", buffer, sizeof buffer - 1U);
  if (length <= 0) return {};
  buffer[length] = '\0';
  return std::filesystem::path(buffer);
#endif
}

// --- SP7 M2 Task 3 (spec D-2/D-3/D-4/D-10): `strata update`, the program half.
// Flow (rulings R-T3-g/R-T3-l): update.source -> pinned key (fetch path only; a
// rollback is a pure pointer flip) -> versioned-install check ->
// fetch the signed envelope from each mirror base URL (<base>/envelope, R-T3-a) ->
// verify -> show current vs offered -> confirm (--yes) -> fetch every component
// (<base>/<file>) into <install>/.update-staging/<version>/bin and verify each
// SHA-256 -> rename the version directory into place -> flip the `current`
// pointer (keeping `previous`) -> restart hint. Any failure leaves `current`
// untouched and removes the staging (R-T3-i); failures exit 12 (R-T3-d), a
// decline exits 2, an interrupt exits 8 (§8.3). Fetching is the SP4 confined
// downloader stack through fill_kit: never a bare system curl at runtime.

constexpr std::size_t kMaxEnvelopeBytes = 1U << 20U;          // the signed envelope is small, like a model manifest
constexpr std::uint64_t kUpdateHopSeconds = 600;              // per-request time bound (update has no run budget)
inline constexpr std::string_view kStagingUpdateName = ".update-staging";

bool is_safe_release_version(const std::string_view version) noexcept {
  if (version.empty() || version.size() > 64 || version == "." || version == "..") return false;
  for (const char c : version) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_';
    if (!ok) return false;
  }
  return true;
}

// The D-4 pointers. Linux: `current`/`previous` symlinks flipped by atomic rename
// (spec D-4 verbatim); Windows: current.txt/previous.txt pointer files written to
// a temporary and renamed (runtime proof is owner-side, windows-runtime gates).
std::optional<std::string> read_pointer(const std::filesystem::path& install_root, const std::string& name) {
#if defined(_WIN32)
  std::ifstream pointer{install_root / (name + ".txt")};
  std::string value;
  if (!(pointer >> value)) return std::nullopt;
  if (!value.empty() && value.back() == '\r') value.pop_back();
  return value;
#else
  std::error_code ec;
  const auto target = std::filesystem::read_symlink(install_root / name, ec);
  if (ec) return std::nullopt;
  return target.string();
#endif
}

bool write_pointer(const std::filesystem::path& install_root, const std::string& name, const std::string& value) {
#if defined(_WIN32)
  // Compile-proven here; the Windows runtime claim waits for the owner's box.
  const auto tmp = install_root / (name + ".txt.tmp");
  std::ofstream out{tmp, std::ios::binary | std::ios::trunc};
  if (!out) return false;
  out << value << "\n";
  out.close();
  if (!out) return false;
  std::error_code ec;
  std::filesystem::rename(tmp, install_root / (name + ".txt"), ec);
  if (ec) {
    // Windows rename never replaces: the only replacement is remove-then-rename,
    // which leaves the pointer absent for a moment (the NSIS installer of Task 6
    // owns the atomic repoint story; the runtime claim is owner-side anyway).
    std::error_code rm;
    std::filesystem::remove(install_root / (name + ".txt"), rm);
    std::filesystem::rename(tmp, install_root / (name + ".txt"), ec);
  }
  return !ec;
#else
  const auto tmp = install_root / (name + ".tmp");
  std::error_code ec;
  std::filesystem::remove(tmp, ec);
  ec.clear();
  std::filesystem::create_symlink(value, tmp, ec);
  if (ec) return false;
  std::filesystem::rename(tmp, install_root / name, ec);  // POSIX rename replaces atomically
  return !ec;
#endif
}

struct InstallView final {
  std::filesystem::path install_root;
  std::filesystem::path version_dir;
  std::string version;  // the version directory's name
};

// The running exe sits at <root>/<version>/bin/<exe> and <root>/current resolves
// to <version>; anything else is not a versioned install (D-4 refuses).
std::optional<InstallView> resolve_versioned_install(const std::filesystem::path& self_path) {
  std::error_code ec;
  const auto self = std::filesystem::canonical(self_path, ec);
  if (ec || !std::filesystem::is_regular_file(self)) return std::nullopt;
  const auto version_dir = self.parent_path().parent_path();
  const auto install_root = version_dir.parent_path();
  if (install_root.empty() || version_dir.empty()) return std::nullopt;
  const auto pointed = read_pointer(install_root, "current");
  if (!pointed) return std::nullopt;
  std::error_code ignored;
  if (std::filesystem::canonical(install_root / *pointed, ignored) != version_dir) return std::nullopt;
  return InstallView{install_root, version_dir, version_dir.filename().string()};
}

// The pinned release key: the build-time pinned value, or the explicit test pin
// in hooks builds (CliServices::test_pinned_release_key_hex, 64 lowercase hex).
std::optional<std::array<std::uint8_t, 32>> update_pinned_key(const CliServices& services) {
#if defined(SECURE_FRONTEND_TEST_HOOKS)
  if (!services.test_pinned_release_key_hex.empty()) {
    if (!is_lower_hex_digest(services.test_pinned_release_key_hex)) return std::nullopt;
    std::array<std::uint8_t, 32> key{};
    for (std::size_t i = 0; i < 32U; ++i) {
      const auto high = static_cast<unsigned char>(services.test_pinned_release_key_hex[2 * i]);
      const auto low = static_cast<unsigned char>(services.test_pinned_release_key_hex[2 * i + 1]);
      const auto nibble = [](const unsigned char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
      key[i] = static_cast<std::uint8_t>((nibble(high) << 4) | nibble(low));
    }
    return key;
  }
#else
  (void)services;  // the explicit pin exists in hooks builds only
#endif
  return pinned_release_public_key();
}

// Resolve and pin exactly like fetch_file's §3.5 rule (proxy configurations let
// curl's proxy resolve, invisible to us, §13.2).
std::optional<AcquireError> pin_hop(const NetworkKit& kit, const FetchSettings& fetch, const HttpsUrl& url, TransferRequest& request) {
  if (fetch.proxy) return std::nullopt;
  const auto answers = kit.resolver->resolve(url.host);
  if (!answers || answers->empty()) return AcquireError{AcquireReason::dns_failed, url.host};
  const auto verdict = choose_pinned_address(*answers, fetch.allow_private_addresses);
  if (verdict.kind != AddressVerdictKind::pinned) return AcquireError{AcquireReason::address_not_allowed, url.host};
  request.pin = verdict.pinned;
  return std::nullopt;
}

std::optional<HttpsUrl> mirror_child(const HttpsUrl& base, const std::string& child) {
  const auto parsed = parse_source_url("https://" + base.host + base.target + "/" + child);
  if (const auto* problem = std::get_if<UrlProblem>(&parsed)) {
    (void)problem;
    return std::nullopt;
  }
  return std::get<HttpsUrl>(parsed);
}

// One GET into memory with a hard cap. The envelope size is unknown ahead, so the
// head gets the update's own fixed rules (200 only; no redirects: --max-redirs 0).
std::optional<AcquireError> fetch_envelope(const NetworkKit& kit, const FetchSettings& fetch, const HttpsUrl& url, std::string& body) {
  TransferRequest request = hop_request(url, fetch, kUpdateHopSeconds);
  if (auto failure = pin_hop(kit, fetch, url, request)) return failure;
  ResponseHeadParser parser;
  bool decided = false;
  bool writable = false;
  std::optional<AcquireError> body_error;
  const auto on_bytes = [&](std::string_view bytes) -> bool {
    if (kit.interrupted->load()) {
      body_error = AcquireError{AcquireReason::interrupted, url.host};
      return false;
    }
    if (!decided) {
      const auto fed = parser.feed(bytes);
      if (fed.state == ResponseHeadParser::State::malformed) {
        body_error = AcquireError{AcquireReason::response_malformed, url.host};
        return false;
      }
      if (fed.state == ResponseHeadParser::State::need_more) return true;
      const int status = parser.head().status;
      if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
        body_error = AcquireError{AcquireReason::redirect_not_allowed, url.host};
        return false;
      }
      if (status != 200) {
        body_error = AcquireError{AcquireReason::http_status, url.host + " HTTP " + std::to_string(status)};
        return false;
      }
      for (const auto& header : parser.head().headers) {
        if (header.first != "content-length") continue;
        std::uint64_t announced = 0;
        const auto [ptr, ec] = std::from_chars(header.second.data(), header.second.data() + header.second.size(), announced);
        if (ec != std::errc{} || announced > kMaxEnvelopeBytes) {
          body_error = AcquireError{AcquireReason::size_exceeded, url.host};
          return false;
        }
      }
      decided = true;
      writable = true;
      bytes.remove_prefix(fed.consumed);
    }
    if (!writable || bytes.empty()) return true;
    if (body.size() + bytes.size() > kMaxEnvelopeBytes) {
      body_error = AcquireError{AcquireReason::size_exceeded, url.host};
      return false;
    }
    body.append(bytes);
    return true;
  };
  const HopExit exit = kit.transport->run(request, on_bytes);
  if (exit.failure) return *exit.failure;
  if (body_error) return body_error;
  if (kit.interrupted->load()) return AcquireError{AcquireReason::interrupted, url.host};
  if (exit.curl_exit && *exit.curl_exit != 0) {
    return AcquireError{map_curl_exit(*exit.curl_exit, false).value_or(AcquireReason::transport_failed), url.host};
  }
  if (!decided || body.empty()) return AcquireError{AcquireReason::response_malformed, url.host};
  return std::nullopt;
}

// One GET streamed to disk, hashed, capped and verified against the manifest row
// (the same §3.4/§3.6 discipline fetch_file applies to model files).
std::optional<AcquireError> fetch_component(const NetworkKit& kit, const FetchSettings& fetch, const HttpsUrl& url,
                                            const ReleaseComponent& component, const std::filesystem::path& dest) {
  TransferRequest request = hop_request(url, fetch, kUpdateHopSeconds);
  if (auto failure = pin_hop(kit, fetch, url, request)) return failure;
  std::ofstream out{dest, std::ios::binary | std::ios::trunc};
  if (!out) return AcquireError{AcquireReason::publish_failed, "update staging"};
  Sha256 hasher;
  std::uint64_t written = 0;
  ResponseHeadParser parser;
  std::optional<HeadDecision> decision;
  std::optional<AcquireError> body_error;
  bool writing = false;
  const auto on_bytes = [&](std::string_view bytes) -> bool {
    if (kit.interrupted->load()) {
      body_error = AcquireError{AcquireReason::interrupted, url.host};
      return false;
    }
    if (!decision) {
      const auto fed = parser.feed(bytes);
      if (fed.state == ResponseHeadParser::State::malformed) {
        body_error = AcquireError{AcquireReason::response_malformed, url.host};
        return false;
      }
      if (fed.state == ResponseHeadParser::State::need_more) return true;
      decision = classify_head(parser.head(), HopExpectation{component.size, std::nullopt});
      bytes.remove_prefix(fed.consumed);
      if (decision->action != HeadAction::write_body) return false;  // redirect or failure: the body is not needed
      writing = true;
    }
    if (!writing || bytes.empty()) return true;
    if (written + bytes.size() > component.size) {
      body_error = AcquireError{AcquireReason::size_exceeded, component.file};
      return false;
    }
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!out) {
      body_error = AcquireError{AcquireReason::disk_space, component.file};
      return false;
    }
    hasher.update(bytes);
    written += bytes.size();
    return true;
  };
  const HopExit exit = kit.transport->run(request, on_bytes);
  out.close();
  const auto finish = [&](const AcquireReason reason, const std::string& detail) {
    std::error_code ec;
    std::filesystem::remove(dest, ec);
    return AcquireError{reason, detail};
  };
  if (exit.failure) return *exit.failure;
  if (body_error) return body_error;
  if (kit.interrupted->load()) return finish(AcquireReason::interrupted, component.file);
  if (decision && decision->action == HeadAction::redirect) return finish(AcquireReason::redirect_not_allowed, component.file);
  if (decision && decision->action == HeadAction::fail) {
    std::string detail = url.host;
    if (decision->reason == AcquireReason::http_status) detail += " HTTP " + std::to_string(decision->status);
    return finish(decision->reason, detail);
  }
  if (!decision) {
    const AcquireReason reason = exit.curl_exit && *exit.curl_exit != 0 ? map_curl_exit(*exit.curl_exit, false).value_or(AcquireReason::transport_failed)
                                                                        : AcquireReason::response_malformed;
    return finish(reason, url.host);
  }
  if (exit.curl_exit && *exit.curl_exit != 0) return finish(map_curl_exit(*exit.curl_exit, false).value_or(AcquireReason::transport_failed), component.file);
  if (written != component.size) return finish(AcquireReason::size_short, component.file);
  if (to_hex(hasher.finish()) != component.sha256) return finish(AcquireReason::digest_mismatch, component.file);
  return std::nullopt;
}

int update_failure(CliStreams& io, const std::string& current_version) {
  io.out << "update: failed; current version " << current_version << " untouched\n";
  return exit_code_value(ExitCode::program_update);
}

int verb_update_program(const FrontendConfig& config, const std::filesystem::path& config_file, const std::span<const std::string> rest,
                        CliStreams& io, CliServices& services) {
  const auto args = parse_args(rest, {"--yes", "--rollback"}, {});
  if (!args) return usage(io, "update [--yes] | update --rollback | update --models [<name:tag> ...] [--yes]");
  if (!args->positional.empty()) return usage(io, "strata update takes no operands (the models half is update --models ...)");
  if (args->flags.contains("--rollback") && args->flags.contains("--yes")) return usage(io, "update --rollback takes no --yes");
  StreamConfirmer ui(io);

  if (config.update.source.empty()) {
    io.out << "strata update: update.source is not configured\n";
    return exit_code_value(ExitCode::program_update);
  }
  // R-T3-l: on the fetch path the pinned key (a property of this binary) refuses
  // before any environment question (Task 2's contract: unpinned == `no release
  // key pinned`). A rollback is a pure pointer flip — no crypto, no key needed.
  const bool rollback = args->flags.contains("--rollback");
  std::optional<std::array<std::uint8_t, 32>> key;
  if (!rollback) {
    key = update_pinned_key(services);
    if (!key) {
      io.out << describe_release_issue(ReleaseIssue{ReleaseReject::no_pinned_key, ""}) << "\n";
      return exit_code_value(ExitCode::program_update);
    }
  }
  std::filesystem::path self = self_executable();
#if defined(SECURE_FRONTEND_TEST_HOOKS)
  if (!services.test_self_executable.empty()) self = services.test_self_executable;
#endif
  const auto install = resolve_versioned_install(self);
  if (!install) {
    io.out << "strata update: not a versioned install (this program is not started through a current pointer)\n";
    return exit_code_value(ExitCode::program_update);
  }

  if (rollback) {
    const auto previous = read_pointer(install->install_root, "previous");
    if (!previous || !std::filesystem::is_directory(install->install_root / *previous)) {
      io.out << "update: nothing kept to roll back to\n";
      return exit_code_value(ExitCode::program_update);
    }
    io.out << "update: rollback: current " << install->version << ", kept " << *previous << "\n";
    if (!ui.confirm("roll back to the kept version " + *previous + "?", false)) {
      io.out << "update: declined; current version " << install->version << " untouched\n";
      return exit_code_value(ExitCode::declined);
    }
    // previous first: a failed flip always leaves a working `current` (D-4).
    if (!write_pointer(install->install_root, "previous", install->version) || !write_pointer(install->install_root, "current", *previous)) {
      return update_failure(io, install->version);
    }
    io.out << "update: current -> " << *previous << " (previous kept: " << install->version << ")\n";
    io.out << "update: restart strata to run the rolled-back version\n";
    return 0;
  }

  NetworkKit kit;
  if (auto failure = fill_kit(kit, config, config_file, services, true)) return report(io, *failure);
  FetchSettings fetch = fetch_settings_from(config.pull);
#if defined(SECURE_FRONTEND_TEST_HOOKS)
  fetch.test_cacert = services.test_cacert;
#endif

  // R-T3-a: each source is a mirror base URL; the first that delivers a valid
  // signed envelope wins, and its components come from the same mirror.
  ReleaseManifest offer;
  HttpsUrl chosen;
  bool found = false;
  for (std::size_t i = 0; i < config.update.source.size(); ++i) {
    const auto parsed = parse_source_url(config.update.source[i]);
    const auto* base = std::get_if<HttpsUrl>(&parsed);
    if (base == nullptr) {
      io.err << format_error(AcquireError{AcquireReason::manifest_invalid, "update.source"}) << '\n';
    } else {
      std::string envelope;
      std::optional<AcquireError> failure;
      if (const auto envelope_url = mirror_child(*base, "envelope")) {
        failure = fetch_envelope(kit, fetch, *envelope_url, envelope);
      } else {
        failure = AcquireError{AcquireReason::manifest_invalid, base->host};
      }
      if (failure) {
        report(io, *failure);
      } else {
        auto verified = verify_release_manifest(envelope, *key);
        if (const auto* issue = std::get_if<ReleaseIssue>(&verified)) {
          io.err << describe_release_issue(*issue) << '\n';
        } else {
          offer = std::get<ReleaseManifest>(std::move(verified));
          chosen = *base;
          found = true;
          break;
        }
      }
    }
    if (!found && i + 1 < config.update.source.size()) {
      io.out << "update: source " << (i + 1) << " refused the envelope; trying source " << (i + 2) << "\n";
    }
  }
  if (!found) return update_failure(io, install->version);

  if (!is_safe_release_version(offer.version)) {
    io.err << "update: the offered version is not a usable version-directory name\n";
    return update_failure(io, install->version);
  }
  io.out << "update: current version " << install->version << "\n";
  io.out << "update: offered version " << offer.version << " (channel " << offer.channel << ")\n";
  for (const auto& component : offer.components) {
    io.out << "update: component " << component.name << ": " << component.file << ", " << component.size << " bytes, sha256 " << component.sha256 << "\n";
  }
  if (offer.version == install->version) {
    io.out << "update: already the current version " << offer.version << "\n";
    return 0;
  }
  if (std::filesystem::exists(install->install_root / offer.version)) {
    io.err << "update: " << offer.version << " is already a version directory below the install root\n";
    return update_failure(io, install->version);
  }
  if (!ui.confirm("apply update to version " + offer.version + "?", args->flags.contains("--yes"))) {
    io.out << "update: declined; current version " << install->version << " untouched\n";
    return exit_code_value(ExitCode::declined);
  }

  const auto staging = install->install_root / std::string{kStagingUpdateName};
  std::error_code ec;
  std::filesystem::remove_all(staging, ec);
  std::error_code mk_ec;
  std::filesystem::create_directories(staging / offer.version / "bin", mk_ec);
  if (mk_ec) {
    io.err << format_error(AcquireError{AcquireReason::publish_failed, "update staging"}) << '\n';
    return update_failure(io, install->version);
  }
  for (const auto& component : offer.components) {
    const auto url = mirror_child(chosen, component.file);
    if (!url) {
      io.err << format_error(AcquireError{AcquireReason::manifest_invalid, component.file}) << '\n';
      std::filesystem::remove_all(staging, ec);
      return update_failure(io, install->version);
    }
    if (auto failure = fetch_component(kit, fetch, *url, component, staging / offer.version / "bin" / component.file)) {
      report(io, *failure);
      std::filesystem::remove_all(staging, ec);  // R-T3-i: no half-staged leftovers
      if (failure->reason == AcquireReason::interrupted) {
        io.out << "update: failed; current version " << install->version << " untouched\n";
        return exit_code_value(ExitCode::interrupted);
      }
      return update_failure(io, install->version);
    }
  }
  std::error_code rename_ec;
  std::filesystem::rename(staging / offer.version, install->install_root / offer.version, rename_ec);
  std::filesystem::remove_all(staging, ec);
  if (rename_ec) {
    io.err << format_error(AcquireError{AcquireReason::publish_failed, "update install"}) << '\n';
    return update_failure(io, install->version);
  }
  io.out << "update: installed version " << offer.version << " with " << offer.components.size() << " components\n";
  if (!write_pointer(install->install_root, "previous", install->version) || !write_pointer(install->install_root, "current", offer.version)) {
    // The version directory is installed but the pointer flip failed: `current`
    // still names the old version, which is exactly the promise.
    return update_failure(io, install->version);
  }
  io.out << "update: current -> " << offer.version << " (previous kept: " << install->version << ")\n";
  io.out << "update: restart strata to run the new version\n";
  return 0;
}

// `update` splits: the documented --models operand goes to the models half
// (SP4 §6.1, unchanged); anything else is the program half (SP7 M2 Task 3).
int verb_update(const FrontendConfig& config, const std::filesystem::path& config_file, const std::span<const std::string> rest,
                CliStreams& io, CliServices& services) {
  if (std::find(rest.begin(), rest.end(), "--models") == rest.end()) return verb_update_program(config, config_file, rest, io, services);
  return verb_update_models(config, config_file, rest, io, services);
}


// The system-tool runner of `service` and `uninstall`: run_argv (on Windows,
// System32 only). A Windows hooks build's --test-system-dir points it at a test
// directory, so a test never reaches the real Task Scheduler.
service::CommandRunner system_tool_runner(const CliServices& services) {
#if defined(_WIN32) && defined(SECURE_FRONTEND_TEST_HOOKS)
  if (!services.test_tool_directory.empty()) {
    return [directory = services.test_tool_directory](const std::vector<std::string>& argv) {
      return service::run_argv_in(directory, argv);
    };
  }
#else
  (void)services;  // the test directory exists in Windows hooks builds only
#endif
  return &service::run_argv;
}

int verb_service(const FrontendConfig& config, const std::filesystem::path& config_file, const std::span<const std::string> rest,
                 CliStreams& io, const CliServices& services) {
  const auto args = parse_args(rest, {}, {});
  if (!args || args->positional.size() != 1U ||
      (args->positional.front() != "enable" && args->positional.front() != "disable" && args->positional.front() != "status")) {
    return usage(io, "service enable|disable|status");
  }
  const std::string sub = args->positional.front();
  const auto home = user_config_home();
  if (home.empty()) {
    io.out << "service: neither XDG_CONFIG_HOME nor HOME is set, so there is no user unit directory\n";
    return 1;
  }
  service::ServiceEnv env;
#if defined(_WIN32)
  env.user_unit_dir = home / "strata";  // the task-file directory (Task 6; runtime is the owner's box)
  env.task_xml_path = env.user_unit_dir / std::string{service::kTaskXmlFileName};
  env.unit_path = env.task_xml_path;
#else
  env.user_unit_dir = home / "systemd" / "user";
  env.unit_path = env.user_unit_dir / std::string{service::kUnitFileName};
#endif
  if (sub != "status") {
    // enable/disable render the unit; the served process needs its write target.
    if (!config.roots.state) {
      io.out << "service: roots.state is not set in your config; strata will not install a service without it\n";
      return 1;
    }
    const auto target = service::resolve_current_target(self_executable());
    if (auto* error = std::get_if<service::ServiceError>(&target)) {
      io.out << "service: " << error->message << '\n';
      return 1;
    }
    env.unit.exec_start = std::get<std::filesystem::path>(target);
    env.unit.config_file = std::filesystem::weakly_canonical(config_file).generic_string();
    // R-T1-a: the unit's ReadWritePaths are exactly the roots serve writes.
    env.unit.read_write_paths.push_back(config.roots.state->generic_string());
    if (config.roots.logs) env.unit.read_write_paths.push_back(config.roots.logs->generic_string());
    if (config.roots.cache) env.unit.read_write_paths.push_back(config.roots.cache->generic_string());
  }
  service::CommandRunner runner = system_tool_runner(services);
  if (sub == "status") {
    const auto status = service::plan_status(env, runner);
    io.out << status.enabled_line << '\n' << status.active_line << '\n';
    return 0;  // a report, not a verdict (R-T5-a)
  }
  const auto outcome = sub == "enable" ? service::plan_enable(env, runner) : service::plan_disable(env, runner);
  io.out << outcome.message << '\n';
  return outcome.ok ? 0 : 1;
}

int verb_uninstall(const FrontendConfig& config, const std::filesystem::path& config_file, const std::span<const std::string> rest,
                   CliStreams& io, const CliServices& services) {
  const auto args = parse_args(rest, {"--purge"}, {});
  if (!args || !args->positional.empty()) return usage(io, "uninstall [--purge]");

  uninstall::UninstallEnv env;
  env.config_path = std::filesystem::weakly_canonical(config_file);
  if (config.roots.state) env.state_root = *config.roots.state;
  if (const auto token = token_file_below_secret(config)) env.token_path = *token;
  env.models_root = config.roots.models.path();
  if (config.roots.logs) env.logs_root = *config.roots.logs;
  const auto home = user_config_home();
#if defined(_WIN32)
  const auto unit_path = home.empty() ? std::filesystem::path{} : home / "strata" / std::string{service::kTaskXmlFileName};
#else
  const auto unit_path = home.empty() ? std::filesystem::path{} : home / "systemd" / "user" / std::string{service::kUnitFileName};
#endif
  std::error_code ec;
  if (!unit_path.empty() && std::filesystem::exists(unit_path, ec)) {
    env.unit_installed = true;
    env.unit_path = unit_path;
  }
  service::CommandRunner runner = system_tool_runner(services);
  env.manager_available = service::user_manager_reachable(runner);
  env.server_listening = uninstall::server_answers_on(config.listener.host, config.listener.port);
  // M1 has no install metadata yet — the versioned layout and `current` pointer
  // are the M2 update's own story; nothing to compare, so honestly matching (R-T5-b).
  env.install_location_ok = true;
  env.purge = args->flags.contains("--purge");
  if (env.purge) {
    io.out << "uninstall: --purge also removes your models under " << env.models_root.generic_string() << '\n'
           << "uninstall: type the word purge to confirm: " << std::flush;
    std::string line;
    if (std::getline(io.in, line)) {
      while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
      env.purge_confirmed = line == "purge";
    }
  }
  env.allowed_roots = {env.config_path.parent_path(), env.state_root, env.models_root, env.logs_root, home};
  if (config.roots.secret) env.allowed_roots.push_back(config.roots.secret->path());
  const auto plan = uninstall::plan_uninstall(env);
  if (!plan.proceed) {
    io.out << "uninstall: " << plan.deny_reason << '\n';
    return 1;
  }
  if (plan.stop == uninstall::StopDecision::via_service_manager) {
    service::ServiceEnv service_env;
#if defined(_WIN32)
    service_env.user_unit_dir = home / "strata";
    service_env.task_xml_path = unit_path;
    service_env.unit_path = unit_path;
#else
    service_env.user_unit_dir = home / "systemd" / "user";
    service_env.unit_path = unit_path;
#endif
    const auto stopped = service::plan_disable(service_env, runner);
    io.out << stopped.message << '\n';
    if (!stopped.ok) {
      io.out << "uninstall: the service could not be stopped; nothing was removed\n";
      return 1;
    }
  }
  for (const auto& removal : plan.removals) {
    std::error_code exists_ec;
    if (!std::filesystem::exists(removal, exists_ec)) {
      // Legitimately gone already: disable --now removed the unit file itself.
      io.out << "uninstall: " << removal.generic_string() << " is already gone\n";
      continue;
    }
    std::error_code remove_ec;
    const auto removed = std::filesystem::is_directory(removal, remove_ec) ? std::filesystem::remove_all(removal, remove_ec)
                                                                          : std::filesystem::remove(removal, remove_ec);
    if (remove_ec || removed == 0) {
      io.out << "uninstall: could not remove " << removal.generic_string() << "; stopping here\n";
      return 1;
    }
    io.out << "uninstall: removed " << removal.generic_string() << '\n';
  }
  for (const auto& kept : plan.kept) io.out << "uninstall: kept " << kept.generic_string() << '\n';
  return 0;
}

// Model downloads (plan 2026-10-23-model-downloads Task 3): strata hf-info <link>. The link is read
// by hf_source (O4: huggingface.co, a .gguf file) before anything is spawned; then Hugging Face's
// API, through the confined downloader (fetch_envelope: 200 only, no redirect, 1 MiB), gives the
// revision's commit and the file's folder listing; stdout is one JSON object with the file's whole
// shard set, each with its LFS size and SHA-256, or `ERROR <code>` (the reason on stderr).
int verb_hf_info(const FrontendConfig& config, const std::filesystem::path& config_file, const std::span<const std::string> rest,
                 CliStreams& io, CliServices& services) {
  const auto args = parse_args(rest, {}, {});
  if (!args || args->positional.size() != 1) return usage(io, "hf-info takes one Hugging Face link");
  const auto refuse = [&io](const std::string_view code, const std::string_view why) {
    io.out << "ERROR " << code << '\n' << std::flush;
    io.err << "strata: hf-info: " << why << '\n';
    return exit_code_value(ExitCode::usage);
  };
  const auto parsed = parse_hf_link(args->positional[0]);
  if (const auto* reason = std::get_if<std::string_view>(&parsed)) return refuse("link_refused", *reason);
  const auto& link = std::get<HfFileLink>(parsed);
  NetworkKit kit;
  if (auto failure = fill_kit(kit, config, config_file, services, true)) {
    io.out << "ERROR " << reason_name(failure->reason) << '\n' << std::flush;
    return report(io, *failure);
  }
  const FetchSettings fetch = fetch_settings_from(config.pull);
  // A string may fill the answer (a model card's chat template runs past 10 KB); the fields read
  // from it (the commit, each path and LFS SHA-256) are checked against their own grammars.
  constexpr JsonLimits kApiLimits{.max_bytes = 1U << 20U, .max_depth = 8U, .max_container_entries = 65536U,
                                  .max_string_bytes = 1U << 20U, .allow_fractions = true};
  std::optional<AcquireError> network_failure;
  const auto get_json = [&](const std::string& url_text) -> std::optional<JsonValue> {
    const auto url = parse_source_url(url_text);
    if (!std::holds_alternative<HttpsUrl>(url)) {
      network_failure = AcquireError{AcquireReason::manifest_invalid, "hf-info"};
      return std::nullopt;
    }
    std::string body;
    if (auto failure = fetch_envelope(kit, fetch, std::get<HttpsUrl>(url), body)) {
      network_failure = std::move(failure);
      return std::nullopt;
    }
    auto json = parse_json(body, kApiLimits);
    if (auto* value = std::get_if<JsonValue>(&json)) return std::move(*value);
    return std::nullopt;
  };
  const auto member = [](const JsonValue& object, const std::string_view key) -> const JsonValue* {
    const auto* members = object.if_object();
    if (members == nullptr) return nullptr;
    for (const auto& m : *members) {
      if (m.key == key) return &m.value;
    }
    return nullptr;
  };
  const auto network_refusal = [&]() {
    if (network_failure) {
      io.out << "ERROR " << reason_name(network_failure->reason) << '\n' << std::flush;
      return report(io, *network_failure);
    }
    return refuse("unexpected_answer", "Hugging Face answered in an unexpected shape");
  };
  std::string commit = link.revision;
  if (!is_hf_commit(commit)) {
    const auto revision = get_json(hf_revision_api_url(link.repo, link.revision));
    const auto* sha = revision ? member(*revision, "sha") : nullptr;
    if (sha == nullptr || sha->if_string() == nullptr) return network_refusal();
    commit = *sha->if_string();
    if (!is_hf_commit(commit)) return refuse("unexpected_answer", "the revision did not resolve to a commit");
  }
  const auto slash = link.path.rfind('/');
  const std::string folder = slash == std::string::npos ? std::string{} : link.path.substr(0, slash);
  const auto listing = get_json(hf_tree_api_url(link.repo, commit, folder));
  if (!listing || listing->if_array() == nullptr) return network_refusal();
  std::string out = "{\"repo\":";
  append_json_string(&out, link.repo);
  out += ",\"commit\":";
  append_json_string(&out, commit);
  out += ",\"files\":[";
  const auto shards = gguf_shard_set(link.path);
  for (std::size_t i = 0; i < shards.size(); ++i) {
    const JsonValue* found = nullptr;
    for (const auto& entry : *listing->if_array()) {
      const auto* path = member(entry, "path");
      if (path != nullptr && path->if_string() != nullptr && *path->if_string() == shards[i]) found = &entry;
    }
    if (found == nullptr) return refuse("file_not_found", "the file (or one of its shards) is not at that revision");
    const auto* lfs = member(*found, "lfs");
    const auto* oid = lfs ? member(*lfs, "oid") : nullptr;
    const auto* size = lfs ? member(*lfs, "size") : nullptr;
    if (oid == nullptr || oid->if_string() == nullptr || size == nullptr || size->if_integer() == nullptr || *size->if_integer() <= 0) {
      return refuse("not_lfs", "the file carries no LFS size and SHA-256");
    }
    const std::string& sha256 = *oid->if_string();
    if (sha256.size() != 64 || !std::all_of(sha256.begin(), sha256.end(), [](const char c) {
          return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        })) {
      return refuse("not_lfs", "the file's LFS SHA-256 is malformed");
    }
    if (i != 0U) out += ',';
    out += "{\"path\":";
    append_json_string(&out, shards[i]);
    out += ",\"size\":" + std::to_string(*size->if_integer()) + ",\"sha256\":";
    append_json_string(&out, sha256);
    out += '}';
  }
  out += "]}\n";
  io.out << out << std::flush;
  return 0;
}

}  // namespace

int run_network_verb(const std::string& verb, const FrontendConfig& config, const std::filesystem::path& config_file,
                     const std::span<const std::string> rest, CliStreams& io, CliServices& services) {
  const InterruptScope scope;  // the whole verb: prompts, every update --models target, run's re-read
  if (verb == "pull") return verb_pull(config, config_file, rest, io, services);
  if (verb == "hf-info") return verb_hf_info(config, config_file, rest, io, services);
  if (verb == "run") return verb_run(config, config_file, rest, io, services);
  if (verb == "doctor") return verb_doctor(config, config_file, rest, io, services);
  if (verb == "service") return verb_service(config, config_file, rest, io, services);
  if (verb == "uninstall") return verb_uninstall(config, config_file, rest, io, services);
  return verb_update(config, config_file, rest, io, services);
}

}  // namespace secure_frontend
