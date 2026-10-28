#include "server_harness.hpp"

#include "config_json.hpp"
#include "http_client.hpp"
#include "non_elevated_launch.hpp"
#include "windows_command_line.hpp"
#include "test_paths.hpp"
#include "test_tokenizer.hpp"
#include "acquire_fixtures.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <iostream>
#include <iterator>
#include <system_error>
#include <thread>

#if defined(_WIN32)
#include "secure_frontend/win32_prelude.hpp"
#include "secure_frontend/crt_environment.hpp"
#else
#include <arpa/inet.h>
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/prctl.h>
#endif
#endif

namespace test_support {
namespace {

#if defined(_WIN32)
using Socket = SOCKET;
constexpr Socket kInvalidSocket = INVALID_SOCKET;
void close_socket(const Socket socket) { (void)closesocket(socket); }
void ensure_winsock() {
  static const bool started = [] {
    WSADATA data{};
    return WSAStartup(MAKEWORD(2, 2), &data) == 0;
  }();
  (void)started;
}
std::wstring widen(const std::string& text) {
  if (text.empty()) return {};
  const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
  std::wstring out(static_cast<std::size_t>(std::max(size, 0)), L'\0');
  if (size > 0) (void)MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size);
  return out;
}
using secure_frontend_test::token_elevated;

// The path Windows resolves `path` to, in the server's canonical form
// (canonical_path_windows.cpp: GetFinalPathNameByHandleW with
// FILE_NAME_NORMALIZED, "\\?\" removed): long names for 8.3 components, the
// on-disk case. Unchanged when the path cannot be opened or resolved.
std::filesystem::path final_path(const std::filesystem::path& path) {
  HANDLE handle = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                              OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
  if (handle == INVALID_HANDLE_VALUE) return path;
  std::wstring resolved(32768, L'\0');
  const DWORD length = GetFinalPathNameByHandleW(handle, resolved.data(), static_cast<DWORD>(resolved.size()), FILE_NAME_NORMALIZED);
  CloseHandle(handle);
  if (length == 0U || length >= resolved.size()) return path;
  resolved.resize(length);
  if (resolved.size() >= 7U && resolved.compare(0, 4, L"\\\\?\\") == 0 && resolved[5] == L':') resolved.erase(0, 4);
  return std::filesystem::path{resolved};
}

// Full Win32 image path of `pid`; empty when it cannot be read.
std::wstring process_image(const DWORD pid) {
  HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (process == nullptr) return {};
  std::wstring image(32768, L'\0');
  DWORD size = static_cast<DWORD>(image.size());
  const bool queried = QueryFullProcessImageNameW(process, 0, image.data(), &size) != 0;
  CloseHandle(process);
  if (!queried) return {};
  image.resize(size);
  return image;
}

// Windows' console server, %SystemRoot%\System32\conhost.exe (exact path,
// case-insensitive). A process whose image cannot be read is not one.
bool is_console_host(const DWORD pid) {
  const std::wstring image = process_image(pid);
  std::wstring system(MAX_PATH, L'\0');
  const UINT length = GetSystemDirectoryW(system.data(), static_cast<UINT>(system.size()));
  if (image.empty() || length == 0U || length >= system.size()) return false;
  system.resize(length);
  const std::wstring conhost = system + L"\\conhost.exe";
  return CompareStringOrdinal(image.c_str(), static_cast<int>(image.size()), conhost.c_str(), static_cast<int>(conhost.size()), TRUE) ==
         CSTR_EQUAL;
}

// Every process currently in `job` (JobObjectBasicProcessIdList).
std::vector<DWORD> job_members(HANDLE job) {
  std::vector<unsigned char> buffer(sizeof(JOBOBJECT_BASIC_PROCESS_ID_LIST) + 256U * sizeof(ULONG_PTR));
  auto* list = reinterpret_cast<JOBOBJECT_BASIC_PROCESS_ID_LIST*>(buffer.data());
  list->NumberOfAssignedProcesses = 256U;
  std::vector<DWORD> pids;
  if (QueryInformationJobObject(job, JobObjectBasicProcessIdList, list, static_cast<DWORD>(buffer.size()), nullptr)) {
    const ULONG_PTR* ids = list->ProcessIdList;
    for (DWORD index = 0; index < list->NumberOfProcessIdsInList; ++index) pids.push_back(static_cast<DWORD>(ids[index]));
  }
  return pids;
}
#else
using Socket = int;
constexpr Socket kInvalidSocket = -1;
void close_socket(const Socket socket) { (void)close(socket); }
void ensure_winsock() {}
#endif

void set_timeouts(const Socket socket, const std::chrono::milliseconds timeout) {
#if defined(_WIN32)
  const DWORD milliseconds = static_cast<DWORD>(timeout.count());
  (void)setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&milliseconds), sizeof(milliseconds));
  (void)setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&milliseconds), sizeof(milliseconds));
#else
  timeval value{};
  value.tv_sec = static_cast<time_t>(timeout.count() / 1000);
  value.tv_usec = static_cast<suseconds_t>((timeout.count() % 1000) * 1000);
  (void)setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &value, sizeof(value));
  (void)setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, &value, sizeof(value));
#endif
}

Socket connect_loopback(const std::uint16_t port, const std::chrono::milliseconds timeout) {
  ensure_winsock();
  const Socket socket_handle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
  if (socket_handle == kInvalidSocket) return kInvalidSocket;
  set_timeouts(socket_handle, timeout);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (connect(socket_handle, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
    close_socket(socket_handle);
    return kInvalidSocket;
  }
  return socket_handle;
}

}  // namespace

std::vector<long> children_of(const long pid) {
  std::vector<long> children;
#if !defined(_WIN32)
  std::error_code error;
  for (const auto& task : std::filesystem::directory_iterator("/proc/" + std::to_string(pid) + "/task", error)) {
    std::ifstream input(task.path() / "children");
    long child = 0;
    while (input >> child) children.push_back(child);
  }
  std::sort(children.begin(), children.end());
  children.erase(std::unique(children.begin(), children.end()), children.end());
#else
  (void)pid;
#endif
  return children;
}

std::vector<long> descendants_of(const long pid) {
  std::vector<long> result;
  std::deque<long> pending{pid};
  while (!pending.empty()) {
    const long current = pending.front();
    pending.pop_front();
    for (const long child : children_of(current)) {
      result.push_back(child);
      pending.push_back(child);
    }
  }
  return result;
}

bool process_token_elevated(const long pid) {
#if defined(_WIN32)
  HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
  if (process == nullptr) return true;
  HANDLE token = nullptr;
  const bool opened = OpenProcessToken(process, TOKEN_QUERY, &token) != 0;
  CloseHandle(process);
  if (!opened) return true;
  const bool elevated = token_elevated(token);
  CloseHandle(token);
  return elevated;
#else
  (void)pid;
  return false;
#endif
}

std::optional<HttpResult> http_request(const std::uint16_t port, const std::string_view method, const std::string_view target,
                                       const std::string_view body, const std::chrono::milliseconds timeout) {
  const Socket socket_handle = connect_loopback(port, timeout);
  if (socket_handle == kInvalidSocket) return std::nullopt;
  std::string request = std::string(method) + " " + std::string(target) + " HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) +
                        "\r\nAuthorization: Bearer " + std::string(kHarnessToken) + "\r\nConnection: close\r\n";
  if (!body.empty()) request += "Content-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) + "\r\n";
  request += "\r\n";
  request.append(body);
  std::size_t sent = 0;
  while (sent < request.size()) {
#if defined(_WIN32)
    const int count = send(socket_handle, request.data() + sent, static_cast<int>(request.size() - sent), 0);
#else
    const ssize_t count = send(socket_handle, request.data() + sent, request.size() - sent, MSG_NOSIGNAL);
#endif
    if (count <= 0) {
      close_socket(socket_handle);
      return std::nullopt;
    }
    sent += static_cast<std::size_t>(count);
  }
  std::string response;
  std::array<char, 4096> buffer{};
  for (;;) {
#if defined(_WIN32)
    const int count = recv(socket_handle, buffer.data(), static_cast<int>(buffer.size()), 0);
#else
    const ssize_t count = recv(socket_handle, buffer.data(), buffer.size(), 0);
#endif
    if (count <= 0) break;
    response.append(buffer.data(), static_cast<std::size_t>(count));
  }
  close_socket(socket_handle);
  if (response.size() < 12U || response.rfind("HTTP/1.1 ", 0) != 0) return std::nullopt;
  HttpResult result;
  result.status = std::stoi(response.substr(9, 3));
  const auto split = response.find("\r\n\r\n");
  if (split != std::string::npos) result.body = response.substr(split + 4U);
  return result;
}

bool port_refuses(const std::uint16_t port) {
  const Socket socket_handle = connect_loopback(port, std::chrono::milliseconds(500));
  if (socket_handle == kInvalidSocket) return true;
  close_socket(socket_handle);
  return false;
}

std::unique_ptr<IdleConnection> IdleConnection::open(const std::uint16_t port) {
  const Socket socket_handle = connect_loopback(port, std::chrono::milliseconds(2000));
  if (socket_handle == kInvalidSocket) return nullptr;
  return std::unique_ptr<IdleConnection>(new IdleConnection(static_cast<std::uintptr_t>(socket_handle)));
}

IdleConnection::~IdleConnection() { close_socket(static_cast<Socket>(socket_)); }

std::unique_ptr<ServerProcess> ServerProcess::start(const ServerOptions& options) {
#if defined(__linux__)
  // Orphans of a server that died (or was SIGKILLed by a test) re-parent to
  // this process, so the destructor can kill and reap them.
  static const bool subreaper = prctl(PR_SET_CHILD_SUBREAPER, 1UL, 0UL, 0UL, 0UL) == 0;
  (void)subreaper;
#endif
  std::unique_ptr<ServerProcess> server(new ServerProcess());
  server->root_ = make_temp_dir("server");  // 0700; Windows: the current user is granted access
#if defined(_WIN32)
  // The server canonicalises the engine and every root to the final path, and
  // hands the engine those (argv[0], the model path); %TEMP% may be 8.3
  // (C:\Users\RUNNER~1\...). Use the same form so expected paths compare equal.
  server->root_ = final_path(server->root_);
#endif
  const auto models = server->root_ / "models";
  const auto tokenizer = server->root_ / "tokenizer";
  secure_frontend_test::make_private_directory(models);     // 0700
  secure_frontend_test::make_private_directory(tokenizer);  // 0700
  if (options.managed_vision) {
    // SP5 M1 Task 12: the managed path. Install fx m:t straight into models/ (the same
    // publish_staged sequence as cli::install), and skip the expert fixture files.
    namespace fx = secure_frontend_test::acquire;
    const auto store_root = fx::models_root(models);
    const auto payload = fx::qwen_payload_vision_pack();
    const auto manifest = fx::trusted(
        fx::manifest_text("m", "t", 1U, payload, true, true, "fixture.example", "mmproj/mmproj.gguf"),
        secure_frontend::ModelOrigin::third_party);
    fx::stage(store_root, manifest, payload);
    fx::FakeLoaded nobody;
    const secure_frontend::StoreEnvironment env;
    const auto published =
        secure_frontend::publish_staged(store_root, secure_frontend::staging_key(manifest.parsed.sha256), manifest, nobody, env, true);
    if (const auto* publish_error = std::get_if<secure_frontend::AcquireError>(&published)) {
      std::cerr << "harness: managed vision install failed: reason=" << static_cast<int>(publish_error->reason)
                << " detail=" << publish_error->detail << "\n";
      return nullptr;
    }
  } else {
    secure_frontend_test::write_file(models / "fake-model.gguf", "GGUF", 0600);
    // Model launcher: a gemma4 card loads its pack in spm_bpe mode from its own directory.
    if (std::any_of(options.model_cards.begin(), options.model_cards.end(), [](const CardOptions& card) { return card.profile == "gemma4"; })) {
      secure_frontend_test::make_private_directory(server->root_ / "tokenizer-spm");
      write_spm_tokenizer_pack(server->root_ / "tokenizer-spm");
    }
    // Gemma 4 plan 1 Task 11: the gemma4 profile loads its pack in spm_bpe mode (merges.json).
    if (options.profile == std::optional<std::string>{"gemma4"}) {
      write_spm_tokenizer_pack(tokenizer);  // 0600 files
    } else if (options.tool_tokenizer) {
      write_tool_tokenizer_pack(tokenizer);  // web search: the reasoning and tool tags too
    } else {
      write_tokenizer_pack(tokenizer);  // 0600 files
    }
  }
  // SP1's open_executable refuses an engine file or folder writable by group or
  // others; the build tree is 0775 under umask 0002. Copy the engine into a
  // private <root>/bin exactly as SP1's ServerFixture does.
  const auto bin = server->root_ / "bin";
  secure_frontend_test::make_private_directory(bin);  // 0700
  const auto source = options.engine.value_or(fake_engine_path());
  server->engine_path_ = bin / source.filename();
  std::error_code copy_error;
  std::filesystem::copy_file(source, server->engine_path_, std::filesystem::copy_options::overwrite_existing, copy_error);
  if (copy_error) {
    std::cerr << "harness: could not copy the engine into " << utf8_path(bin) << ": " << copy_error.message() << '\n';
    return nullptr;
  }
#if defined(_WIN32)
  server->engine_path_.make_preferred();
#else
  if (chmod(server->engine_path_.c_str(), 0700) != 0) {
    std::cerr << "harness: chmod 0700 failed on the engine copy\n";
    return nullptr;
  }
  // SP1 hands the engine its canonical path as argv[0].
  server->engine_path_ = std::filesystem::canonical(server->engine_path_);
#endif
  // Gemma 4 plan 1 Task 11: the fake's options file beside its copy (one option per line).
  if (!options.fake_engine_options.empty()) {
    std::string lines;
    for (const auto& option : options.fake_engine_options) {
      if (option.empty() || option.find_first_of("\r\n") != std::string::npos) {
        std::cerr << "harness: a fake engine option must be one non-empty line\n";
        return nullptr;
      }
      lines += option + "\n";
    }
    secure_frontend_test::write_file(bin / kFakeEngineOptionsFile, lines, 0600);
  }
  server->port_ = secure_frontend_test::pick_free_port();  // SP1's helper (support/http_client.hpp)
  if (options.managed_vision) {
    // Ruling T11-y: the encoder exe is OUR distribution's — maya-vision beside the
    // engine executable. The fake child stands in, copied under that exact name.
    const auto vision_source = fake_vision_child_path();
    const auto vision_exe = bin / "maya-vision";
    std::error_code vision_error;
    std::filesystem::copy_file(vision_source, vision_exe, std::filesystem::copy_options::overwrite_existing, vision_error);
    if (vision_error) {
      std::cerr << "harness: could not copy maya-vision into " << utf8_path(bin) << ": " << vision_error.message() << '\n';
      return nullptr;
    }
#if defined(_WIN32)
    // The server looks for maya-vision.exe; Windows execution needs the suffix.
    std::error_code rename_error;
    std::filesystem::rename(vision_exe, vision_exe.string() + ".exe", rename_error);
    if (rename_error) {
      std::cerr << "harness: could not name maya-vision.exe\n";
      return nullptr;
    }
#else
    if (chmod(vision_exe.c_str(), 0700) != 0) {
      std::cerr << "harness: chmod 0700 failed on the maya-vision copy\n";
      return nullptr;
    }
#endif
  }
  const std::filesystem::path state_dir = options.state_dir.value_or(server->root_ / "state");
  if (!options.state_dir) secure_frontend_test::make_owner_private_directory(state_dir);  // 0700; Windows owner = the user
  std::vector<std::string> arguments{utf8_path(server_path())};
  const bool launcher = !options.model_cards.empty();
  std::map<std::string, std::string> config_literals = options.config_literals;
  if (launcher) {
    const auto cards = server->root_ / "cards";
    secure_frontend_test::make_private_directory(cards);  // 0700
    std::string listed = "[";
    for (std::size_t i = 0; i < options.model_cards.size(); ++i) {
      const CardOptions& card = options.model_cards[i];
      std::map<std::string, std::string> rows{
          {"roots.models", secure_frontend_test::json_literal(utf8_path(models))},
          {"roots.tokenizer_pack",
           secure_frontend_test::json_literal(utf8_path(card.profile == "gemma4" ? server->root_ / "tokenizer-spm" : tokenizer))},
          {"engine.executable", secure_frontend_test::json_literal(utf8_path(server->engine_path_))},
          {"engine.model_id", secure_frontend_test::json_literal("fake-model")}};
      if (!card.engine_args.empty()) {
        std::string args = "[";
        for (std::size_t a = 0; a < card.engine_args.size(); ++a) args += (a ? "," : "") + secure_frontend_test::json_literal(card.engine_args[a]);
        rows["engine.args"] = args + "]";
      }
      for (const auto& [key, literal] : card.config_literals) rows[key] = literal;
      const std::string text = "{\"schema_version\": 1, \"name\": " + secure_frontend_test::json_literal(card.name) +
                               ", \"profile\": " + secure_frontend_test::json_literal(card.profile) +
                               ", \"config\": " + secure_frontend::render_config_literals(rows) + ", \"options\": " + card.options_json + "}";
      const auto file = cards / (std::to_string(i + 1U) + ".json");
      secure_frontend_test::write_file(file, text, 0600);
      listed += (i ? "," : "") + secure_frontend_test::json_literal(utf8_path(file));
    }
    config_literals["models.cards"] = listed + "]";
  }
  if (!options.managed_vision && !launcher) arguments.emplace_back("--expert");  // the managed path launches from the manifest
  // Gemma 4 plan 1 Task 11: engine.profile (an --expert key; the managed path refuses it).
  if (options.profile && !launcher) {
    arguments.emplace_back("--profile");
    arguments.push_back(*options.profile);
  }
  if (!launcher) {
    arguments.push_back("--models-root");
    arguments.push_back(utf8_path(models));
  }
  if (!options.managed_vision && !launcher) {
    arguments.push_back("--tokenizer-pack");
    arguments.push_back(utf8_path(tokenizer));
  }
  if (!launcher) {
    arguments.push_back("--engine");
    arguments.push_back(utf8_path(server->engine_path_));
    arguments.push_back("--model-id");
    arguments.push_back(options.managed_vision ? "m:t" : "fake-model");
  }
  if (!options.no_token) {
    arguments.push_back("--token-env");
    arguments.push_back(std::string(kHarnessTokenEnv));
  }
  arguments.push_back("--port");
  arguments.push_back(std::to_string(server->port_));
  arguments.push_back("--state-dir");
  arguments.push_back(utf8_path(state_dir));
  for (const auto& argument : options.engine_args) {
    arguments.emplace_back("--engine-arg");
    arguments.push_back(argument);
  }
  if (!config_literals.empty()) {
    std::map<std::string, std::string> literals = config_literals;
    literals.emplace("schema_version", "1");
    const auto config_file = server->root_ / "config.json";
    secure_frontend_test::write_file(config_file, secure_frontend_test::render_config_json(literals), 0600);
    arguments.emplace_back("--config");
    arguments.push_back(utf8_path(config_file));
  }
  arguments.insert(arguments.end(), options.extra_args.begin(), options.extra_args.end());
  std::map<std::string, std::string> environment{{std::string(kHarnessTokenEnv), std::string(kHarnessToken)}};
#if defined(_WIN32)
  // The server itself (not the engine) needs these for Winsock and the CRT.
  for (const char* name : {"SystemRoot", "SystemDrive", "WINDIR", "PATH", "TEMP", "TMP"}) {
    if (const char* value = secure_frontend::crt_getenv(name)) environment.emplace(name, value);
  }
#else
  environment.emplace("PATH", "/usr/bin:/bin");
#endif
  for (const auto& [name, value] : options.extra_env) environment[name] = value;
  if (!server->launch(arguments, environment)) {
    std::cerr << "harness: could not launch strata-secure-server\n";
    return nullptr;
  }
  return server;
}

bool ServerProcess::launch(const std::vector<std::string>& arguments, const std::map<std::string, std::string>& environment) {
  const std::string out_log = utf8_path(root_ / "stdout.log");
  const std::string err_log = utf8_path(root_ / "stderr.log");
#if defined(_WIN32)
  std::wstring command_line;
  for (const auto& argument : arguments) {
    if (!command_line.empty()) command_line += L' ';
    command_line += secure_frontend_test::quote_windows_argument(widen(argument));
  }
  std::wstring block;
  for (const auto& [name, value] : environment) {
    block += widen(name) + L"=" + widen(value);
    block.push_back(L'\0');
  }
  block.push_back(L'\0');
  SECURITY_ATTRIBUTES inherit{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
  HANDLE input = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, OPEN_EXISTING, 0, nullptr);
  HANDLE output = CreateFileW(widen(out_log).c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, CREATE_ALWAYS, 0, nullptr);
  HANDLE error = CreateFileW(widen(err_log).c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, CREATE_ALWAYS, 0, nullptr);
  HANDLE job = CreateJobObjectW(nullptr, nullptr);
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
  limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  const bool ready = input != INVALID_HANDLE_VALUE && output != INVALID_HANDLE_VALUE && error != INVALID_HANDLE_VALUE && job != nullptr &&
                     SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
  STARTUPINFOW startup{};
  startup.cb = sizeof(startup);
  startup.dwFlags = STARTF_USESTDHANDLES;
  startup.hStdInput = input;
  startup.hStdOutput = output;
  startup.hStdError = error;
  PROCESS_INFORMATION info{};
  const std::wstring executable = widen(arguments.front());
  // Non-elevated (G4) and still suspended, so the Job is assigned before it runs.
  const bool created = ready && secure_frontend_test::create_non_elevated_process(
                                    executable, command_line, CREATE_UNICODE_ENVIRONMENT | CREATE_NEW_PROCESS_GROUP,
                                    block.data(), startup, info, "harness");
  for (HANDLE handle : {input, output, error}) {
    if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
  }
  if (!created) {
    if (job != nullptr) CloseHandle(job);
    return false;
  }
  if (!AssignProcessToJobObject(job, info.hProcess)) {
    (void)TerminateProcess(info.hProcess, 1);
    CloseHandle(info.hThread);
    CloseHandle(info.hProcess);
    CloseHandle(job);
    return false;
  }
  (void)ResumeThread(info.hThread);
  CloseHandle(info.hThread);
  process_ = info.hProcess;
  job_ = job;
  pid_ = static_cast<long>(info.dwProcessId);
  return true;
#else
  std::vector<std::string> argument_storage = arguments;
  std::vector<char*> argv;
  for (auto& argument : argument_storage) argv.push_back(argument.data());
  argv.push_back(nullptr);
  std::vector<std::string> environment_storage;
  for (const auto& [name, value] : environment) environment_storage.push_back(name + "=" + value);
  std::vector<char*> envp;
  for (auto& entry : environment_storage) envp.push_back(entry.data());
  envp.push_back(nullptr);
  const pid_t child = fork();
  if (child < 0) return false;
  if (child == 0) {
    const int input = open("/dev/null", O_RDONLY);
    const int output = open(out_log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    const int error = open(err_log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (input < 0 || output < 0 || error < 0 || dup2(input, 0) < 0 || dup2(output, 1) < 0 || dup2(error, 2) < 0) _exit(126);
    execve(argv[0], argv.data(), envp.data());
    _exit(127);
  }
  pid_ = static_cast<long>(child);
  return true;
#endif
}

void ServerProcess::remember_descendants() const {
#if !defined(_WIN32)
  if (!exit_code_ && pid_ > 0) {
    for (const long pid : descendants_of(pid_)) known_descendants_.insert(pid);
  }
#endif
}

// No fake engine (--hang-*, --stall-stdin) or grandchild may outlive a test on
// the dev host, however the test ended.
ServerProcess::~ServerProcess() {
  remember_descendants();
  if (!exit_code_ && pid_ > 0) {
    stop(Stop::kill);
    (void)wait_exit(std::chrono::seconds(5));
  }
#if defined(_WIN32)
  if (job_ != nullptr) (void)TerminateJobObject(static_cast<HANDLE>(job_), 1);
  if (process_ != nullptr) CloseHandle(static_cast<HANDLE>(process_));
  if (job_ != nullptr) CloseHandle(static_cast<HANDLE>(job_));  // kill-on-close: nothing of the server survives the harness
#else
  for (const long pid : known_descendants_) {
    (void)kill(static_cast<pid_t>(pid), SIGKILL);
    // Re-parented to this subreaper: reap it. Not our child (ECHILD) or already gone: done.
    for (int step = 0; step < 100; ++step) {
      if (waitpid(static_cast<pid_t>(pid), nullptr, WNOHANG) != 0) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }
#endif
}

std::optional<HttpResult> ServerProcess::get(const std::string_view target, const std::chrono::milliseconds timeout) const {
  return http_request(port_, "GET", target, {}, timeout);
}

std::optional<HttpResult> ServerProcess::post_chat(const std::string_view body, const std::chrono::milliseconds timeout) const {
  return http_request(port_, "POST", "/v1/chat/completions", body, timeout);
}

bool ServerProcess::wait_until_listening(const std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (get("/health", std::chrono::milliseconds(500)).has_value()) return true;
    if (wait_exit(std::chrono::milliseconds(0)).has_value()) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return false;
}

bool ServerProcess::wait_for_health(const std::string_view body, const std::chrono::milliseconds timeout) const {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    const auto health = get("/health", std::chrono::milliseconds(500));
    if (health && health->body == body) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return false;
}

void ServerProcess::stop(const Stop how) {
  if (pid_ <= 0) return;
  remember_descendants();
#if defined(_WIN32)
  if (how == Stop::kill) {
    (void)TerminateProcess(static_cast<HANDLE>(process_), 1);
  } else {
    (void)GenerateConsoleCtrlEvent(CTRL_BREAK_EVENT, static_cast<DWORD>(pid_));
  }
#else
  const int signal_number = how == Stop::term ? SIGTERM : how == Stop::kill ? SIGKILL : SIGINT;
  (void)kill(static_cast<pid_t>(pid_), signal_number);
#endif
}

std::optional<int> ServerProcess::wait_exit(const std::chrono::milliseconds timeout) {
  if (exit_code_) return exit_code_;
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  for (;;) {
#if defined(_WIN32)
    if (WaitForSingleObject(static_cast<HANDLE>(process_), 0) == WAIT_OBJECT_0) {
      DWORD code = 1;
      (void)GetExitCodeProcess(static_cast<HANDLE>(process_), &code);
      exit_code_ = static_cast<int>(code);
      return exit_code_;
    }
#else
    int status = 0;
    if (waitpid(static_cast<pid_t>(pid_), &status, WNOHANG) == static_cast<pid_t>(pid_)) {
      exit_code_ = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
      return exit_code_;
    }
#endif
    if (std::chrono::steady_clock::now() >= deadline) return std::nullopt;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
}

bool ServerProcess::port_refuses() const { return test_support::port_refuses(port_); }

std::string ServerProcess::stderr_text() const {
  std::ifstream input(root_ / "stderr.log", std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
}

std::vector<long> ServerProcess::descendants() const {
#if defined(_WIN32)
  std::vector<long> pids;
  for (const DWORD pid : job_members(static_cast<HANDLE>(job_))) {
    if (static_cast<long>(pid) != pid_ && !is_console_host(pid)) pids.push_back(static_cast<long>(pid));
  }
  return pids;
#else
  remember_descendants();
  return descendants_of(pid_);
#endif
}

unsigned long ServerProcess::active_processes() {
#if defined(_WIN32)
  unsigned long count = 0;
  for (const DWORD pid : job_members(static_cast<HANDLE>(job_))) {
    if (!is_console_host(pid)) ++count;
  }
  return count;
#else
  if (wait_exit(std::chrono::milliseconds(0)).has_value()) return 0;
  remember_descendants();
  return 1U + static_cast<unsigned long>(descendants_of(pid_).size());
#endif
}

std::string ServerProcess::describe_processes() const {
  std::string text;
#if defined(_WIN32)
  for (const DWORD pid : job_members(static_cast<HANDLE>(job_))) {
    const std::wstring image = process_image(pid);
    const auto name = std::filesystem::path(image).filename().u8string();
    text += (text.empty() ? "" : ", ") + std::to_string(pid) + ":" + (image.empty() ? std::string("?") : std::string(name.begin(), name.end()));
  }
#else
  for (const long pid : descendants_of(pid_)) text += (text.empty() ? "" : ", ") + std::to_string(pid);
#endif
  return "[" + text + "]";
}

}  // namespace test_support
