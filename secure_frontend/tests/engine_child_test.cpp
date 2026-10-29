#include "secure_frontend/engine_process.hpp"
#include "secure_frontend/engine_stdio_process.hpp"
#include "secure_frontend/exit_status.hpp"

#include "support/fake_engine_dump.hpp"
#include "support/server_harness.hpp"
#include "support/test_paths.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <thread>
#include <variant>
#include <vector>

#if defined(_WIN32)
#include "secure_frontend/win32_prelude.hpp"

#include <aclapi.h>
#include <sddl.h>

#include "win_child_launch.hpp"
#else
#include <csignal>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "posix_child_launch.hpp"
#endif

namespace {

using namespace std::chrono_literals;
using secure_frontend::ExitKind;
using secure_frontend::ExitPlatform;
using secure_frontend::ExitStatus;
using secure_frontend::KillReason;

int failures = 0;

void expect(const bool condition, const std::string& message) {
  if (!condition) {
    ++failures;
    std::cerr << "FAILED: " << message << '\n';
  }
}

std::optional<ExitStatus> wait_reaped(secure_frontend::EngineStdioProcess& child, const std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  for (;;) {
    if (auto status = child.try_reap()) return status;
    if (std::chrono::steady_clock::now() >= deadline) return std::nullopt;
    std::this_thread::sleep_for(10ms);
  }
}

void proc04_exit_status_is_classified() {
  struct Row {
    ExitStatus status;
    KillReason sent;
    ExitPlatform platform;
    ExitKind kind;
    KillReason reason;
    const char* name;
  };
  const std::vector<Row> table{
      {{false, 0U}, KillReason::none, ExitPlatform::posix, ExitKind::clean_exit, KillReason::none, "posix exit 0"},
      {{false, 1U}, KillReason::none, ExitPlatform::posix, ExitKind::error_exit, KillReason::none, "posix exit 1 (ERR path or CUDA fault)"},
      {{false, 127U}, KillReason::none, ExitPlatform::posix, ExitKind::error_exit, KillReason::none, "posix exit 127 after a successful exec"},
      {{true, 6U}, KillReason::none, ExitPlatform::posix, ExitKind::abort, KillReason::none, "posix SIGABRT (watchdog)"},
      {{true, 9U}, KillReason::none, ExitPlatform::posix, ExitKind::killed_external, KillReason::none, "posix SIGKILL not ours (OOM)"},
      {{true, 11U}, KillReason::none, ExitPlatform::posix, ExitKind::crashed, KillReason::none, "posix SIGSEGV"},
      {{true, 9U}, KillReason::fatal_line, ExitPlatform::posix, ExitKind::killed_by_supervisor, KillReason::fatal_line, "posix our SIGKILL"},
      {{true, 15U}, KillReason::shutdown, ExitPlatform::posix, ExitKind::killed_by_supervisor, KillReason::shutdown, "posix our SIGTERM"},
      {{true, 6U}, KillReason::protocol_failed, ExitPlatform::posix, ExitKind::abort, KillReason::none, "aborted before our kill landed"},
      {{false, 0U}, KillReason::none, ExitPlatform::windows, ExitKind::clean_exit, KillReason::none, "windows exit 0"},
      {{false, 1U}, KillReason::none, ExitPlatform::windows, ExitKind::error_exit, KillReason::none, "windows exit 1"},
      {{false, 3U}, KillReason::none, ExitPlatform::windows, ExitKind::abort, KillReason::none, "windows abort() exit 3"},
      {{false, 0xC0000409U}, KillReason::none, ExitPlatform::windows, ExitKind::abort, KillReason::none, "windows fast-fail abort"},
      {{false, 0xC0000005U}, KillReason::none, ExitPlatform::windows, ExitKind::crashed, KillReason::none, "windows access violation"},
      {{false, 0x20000001U}, KillReason::ready_timeout, ExitPlatform::windows, ExitKind::killed_by_supervisor, KillReason::ready_timeout,
       "windows our TerminateJobObject code"},
  };
  for (const auto& row : table) {
    const auto got = secure_frontend::classify_exit(row.status, row.sent, row.platform);
    expect(got.kind == row.kind && got.reason == row.reason, std::string("classification: ") + row.name);
  }
  expect(secure_frontend::describe_exit({ExitKind::killed_by_supervisor, KillReason::fatal_line}) == "killed_by_supervisor(fatal_line)",
         "describe a supervisor kill");
  expect(secure_frontend::describe_exit({ExitKind::abort, KillReason::none}) == "abort", "describe an abort");
  // Owner 2026-10-29 (to pinpoint what crashes a calibration): the raw end beside the kind - a Windows status in hex as
  // NTSTATUS codes are read, a small exit code and a POSIX signal in decimal.
  expect(secure_frontend::describe_exit_status({false, 0xC0000005U}, ExitPlatform::windows) == "exit code 0xC0000005",
         "an access violation's status in hex");
  expect(secure_frontend::describe_exit_status({false, 0xC0000409U}, ExitPlatform::windows) == "exit code 0xC0000409",
         "a fast-fail abort in hex");
  expect(secure_frontend::describe_exit_status({false, 1U}, ExitPlatform::windows) == "exit code 1", "exit 1 in decimal");
  expect(secure_frontend::describe_exit_status({false, 3U}, ExitPlatform::posix) == "exit code 3", "a POSIX exit code");
  expect(secure_frontend::describe_exit_status({true, 11U}, ExitPlatform::posix) == "signal 11", "a POSIX signal");

  // Real children: the raw status is captured by try_reap() and classified natively.
  struct Live {
    std::string argument;
    ExitKind kind;
  };
  std::vector<Live> live{{"--exit-before-ready=1", ExitKind::error_exit}, {"--crash-after-ready=ABRT", ExitKind::abort},
                         {"--crash-after-ready=SEGV", ExitKind::crashed}};
#if !defined(_WIN32)
  live.push_back({"--crash-after-ready=KILL", ExitKind::killed_external});
#endif
  for (const auto& run : live) {
    secure_frontend::EngineStdioProcess child;
    expect(!child.start({.executable = test_support::fake_engine_path(), .arguments = {run.argument}, .environment = {}}).has_value(),
           "start " + run.argument);
    const auto status = wait_reaped(child, 10s);
    expect(status.has_value(), "the child must end: " + run.argument);
    if (status) {
      expect(secure_frontend::classify_exit(*status, KillReason::none).kind == run.kind, "live classification: " + run.argument);
      expect(child.exit_status() == status, "exit_status() keeps the reaped status");
    }
  }
  // Our own kill is ours, never confused with the engine's `return 1`.
  secure_frontend::EngineStdioProcess killed;
  expect(!killed.start({.executable = test_support::fake_engine_path(), .arguments = {"--hang-before-ready"}, .environment = {}}).has_value(),
         "start hang");
  expect(killed.terminate(100ms), "terminate");
  const auto status = killed.exit_status();
  expect(status && secure_frontend::classify_exit(*status, KillReason::shutdown).kind == ExitKind::killed_by_supervisor,
         "a supervisor kill classifies as ours");
}

#if !defined(_WIN32)
void proc05_group_killed_after_leader_reaped() {
  const auto dir = test_support::make_temp_dir("c1");
  const auto pidfile = dir / "grandchild.pid";
  secure_frontend::EngineStdioProcess engine;
  expect(!engine.start({.executable = test_support::fake_engine_path(),
                        .arguments = {"--spawn-grandchild", "--grandchild-ignore-term", "--grandchild-pidfile=" + test_support::utf8_path(pidfile)},
                        .environment = {}})
              .has_value(),
         "start");
  expect(engine.read_line(5s).line == "READY 4096 stop", "READY");
  const auto grandchild = test_support::read_pidfile(pidfile, 5s);
  expect(grandchild.has_value(), "the grandchild started");
  if (!grandchild) return;
  (void)kill(static_cast<pid_t>(engine.pid()), SIGKILL);  // only the leader
  expect(wait_reaped(engine, 5s).has_value(), "the leader is reaped first");
  expect(test_support::process_alive(*grandchild), "precondition: the grandchild outlived its leader");
  expect(engine.terminate(200ms), "terminate succeeds");
  expect(test_support::wait_until_gone(*grandchild, 2s), "a group member dies although the leader was already reaped");
  test_support::kill_process(*grandchild);
}
#endif

#if !defined(_WIN32)
void proc02_child_signal_state_is_default() {
  struct sigaction ignore {};
  ignore.sa_handler = SIG_IGN;
  sigemptyset(&ignore.sa_mask);
  struct sigaction previous {};
  (void)sigaction(SIGPIPE, &ignore, &previous);  // what install_process_defaults() does in the frontend
  sigset_t block;
  sigemptyset(&block);
  sigaddset(&block, SIGUSR1);
  sigset_t old_mask;
  (void)pthread_sigmask(SIG_BLOCK, &block, &old_mask);
  secure_frontend::EngineStdioProcess child;
  const bool started = !child.start({.executable = test_support::fake_engine_path(), .arguments = {}, .environment = {}}).has_value();
  (void)pthread_sigmask(SIG_SETMASK, &old_mask, nullptr);
  (void)sigaction(SIGPIPE, &previous, nullptr);
  expect(started, "start");
  expect(child.read_line(5s).line == "READY 4096 stop", "READY");
  const auto dump = test_support::request_dump(child, 5s);
  expect(dump && dump->sig_blocked == "0000000000000000", "the engine starts with an empty signal mask, saw " + (dump ? dump->sig_blocked : ""));
  expect(dump && dump->sig_ignored == "0000000000000000", "and nothing ignored (default SIGPIPE), saw " + (dump ? dump->sig_ignored : ""));
  (void)child.terminate(250ms);
}
#endif

#if !defined(_WIN32)
void proc02_canary_descriptors_absent() {
  // What a careless launcher would leak: a non-CLOEXEC fd, a listening socket, an open token file.
  const auto dir = test_support::make_temp_dir("n5");
  const auto token = dir / "token";
  secure_frontend_test::write_file(token, "secret-token", 0600);
  const int token_fd = open(token.c_str(), O_RDONLY);
  const int null_fd = open("/dev/null", O_RDONLY);
  const int listener = socket(AF_INET, SOCK_STREAM, 0);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  const bool listening = listener >= 0 && bind(listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0 && listen(listener, 1) == 0;
  expect(token_fd >= 0 && null_fd >= 0 && listening, "canaries opened");
  for (const int fd : {token_fd, null_fd, listener}) expect(fcntl(fd, F_GETFD) == 0, "precondition: canary " + std::to_string(fd) + " is inheritable");
  secure_frontend::EngineStdioProcess child;
  expect(!child.start({.executable = test_support::fake_engine_path(), .arguments = {}, .environment = {}}).has_value(), "start");
  expect(child.read_line(5s).line == "READY 4096 stop", "READY");
  const auto dump = test_support::request_dump(child, 5s);
  expect(dump && dump->fds == std::vector<int>{0, 1, 2}, "the engine sees only stdio: no canary descriptor");
  (void)child.terminate(250ms);
  for (const int fd : {token_fd, null_fd, listener}) (void)close(fd);
}
#endif

#if !defined(_WIN32)
std::optional<int> run_command(const std::string& executable, const std::vector<std::string>& arguments) {
  secure_frontend::EngineProcess process;
  if (process.start({.executable = executable, .arguments = arguments, .environment = {}}).has_value()) return std::nullopt;
  return process.wait_for(10s);
}

std::optional<int> run_as_nobody(const std::vector<std::string>& command) {
  std::vector<std::string> arguments{"-n", "-u", "nobody"};
  arguments.insert(arguments.end(), command.begin(), command.end());
  return run_command("/usr/bin/sudo", arguments);
}

// Exit 0: opened /proc/<pid>/fd/0 for writing. 3: /proc/<pid> not visible
// (engine gone, or hidepid). 4: the open itself was refused. The open runs in a
// subshell because a failed `exec` redirection exits a POSIX shell outright.
constexpr const char* kReopenProbe = R"([ -d "/proc/$1" ] || exit 3; ( exec 3>"$0" ) 2>/dev/null || exit 4; exit 0)";
constexpr int kProbeOpened = 0;
constexpr int kProbeOpenDenied = 4;

void proc03_other_uid_cannot_open_engine_fd() {
  const char* enabled = std::getenv("STRATA_TEST_SECOND_UID_SUDO");
  if (enabled == nullptr || std::string(enabled) != "1") {
    test_support::skip("needs a second uid: CI sets STRATA_TEST_SECOND_UID_SUDO=1 where `sudo -n -u nobody` is allowed");
  }
  // The variable is CI's demand that R2 be proven: a runner that cannot sudo fails, never skips.
  if (run_as_nobody({"/bin/true"}) != 0) {
    expect(false, "STRATA_TEST_SECOND_UID_SUDO=1 but `sudo -n -u nobody` failed");
    return;
  }
  secure_frontend::EngineStdioProcess child;
  expect(!child.start({.executable = test_support::fake_engine_path(), .arguments = {}, .environment = {}}).has_value(), "start");
  expect(child.read_line(5s).line == "READY 4096 stop", "READY");
  const std::string pid = std::to_string(child.pid());
  const std::string target = "/proc/" + pid + "/fd/0";
  // Positive control: the identical probe as our own uid reaches and opens the path.
  const auto same_uid = run_command("/bin/sh", {"-c", kReopenProbe, target, pid});
  expect(same_uid == kProbeOpened, "positive control: the same uid opens " + target + ", probe exit " +
                                       (same_uid ? std::to_string(*same_uid) : std::string("none")));
  const auto other_uid = run_as_nobody({"/bin/sh", "-c", kReopenProbe, target, pid});
  expect(other_uid == kProbeOpenDenied, "another uid sees the engine but cannot open its stdin through /proc, probe exit " +
                                            (other_uid ? std::to_string(*other_uid) : std::string("none")));
  // The engine was alive throughout: it still answers on the frontend's own stdin.
  std::string error;
  expect(child.write_line("DUMP", secure_frontend::deadline_after(std::chrono::seconds(5)), error), "DUMP through the frontend's stdin: " + error);
  const auto reply = child.read_line(5s);
  expect(reply.kind == secure_frontend::EngineReadKind::line && reply.line.rfind("FDS ", 0) == 0, "the engine is alive after the probes");
  (void)child.terminate(250ms);
}

void proc03_same_uid_reopen_is_accepted_risk() {
  secure_frontend::EngineStdioProcess child;
  expect(!child.start({.executable = test_support::fake_engine_path(), .arguments = {}, .environment = {}}).has_value(), "start");
  expect(child.read_line(5s).line == "READY 4096 stop", "READY");
  const std::string target = "/proc/" + std::to_string(child.pid()) + "/fd/0";
  const int injected = open(target.c_str(), O_WRONLY | O_CLOEXEC);
  // Spec §3.13: the same uid CAN reopen the engine's stdin through /proc and
  // inject protocol lines. Recorded accepted risk (threat model, release
  // record): the upstream engine cannot be made non-dumpable and its protocol
  // has no nonce without engine changes. If this starts failing, a mitigation
  // landed: update the threat model and flip this test.
  const int open_error = errno;
  expect(injected >= 0, "same-uid reopen of the engine's stdin still works (accepted risk, spec 3.13); open failed: " +
                            std::string(std::strerror(open_error)));
  if (injected >= 0) {
    const std::string line = "DUMP\n";
    expect(write(injected, line.data(), line.size()) == static_cast<ssize_t>(line.size()), "an injected line is accepted by the pipe");
    (void)close(injected);
    const auto reply = child.read_line(5s);
    expect(reply.kind == secure_frontend::EngineReadKind::line && reply.line.rfind("FDS ", 0) == 0, "the engine acts on the injected line");
  }
  (void)child.terminate(250ms);
}
#endif

#if defined(_WIN32)
template <typename Function>
Function resolve(const wchar_t* module, const char* name) {
  const HMODULE handle = GetModuleHandleW(module);
  if (handle == nullptr) return nullptr;
  return reinterpret_cast<Function>(reinterpret_cast<void (*)()>(GetProcAddress(handle, name)));
}

struct HandleEntry {
  HANDLE HandleValue;
  ULONG_PTR HandleCount;
  ULONG_PTR PointerCount;
  ULONG GrantedAccess;
  ULONG ObjectTypeIndex;
  ULONG HandleAttributes;
  ULONG Reserved;
};
struct HandleSnapshot {
  ULONG_PTR NumberOfHandles;
  ULONG_PTR Reserved;
  HandleEntry Handles[1];
};

void proc02_canary_handles_absent() {
  SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
  const auto dir = test_support::make_temp_dir("n5w");
  HANDLE file = CreateFileW((dir / "token").wstring().c_str(), GENERIC_WRITE, 0, &inherit, CREATE_ALWAYS, 0, nullptr);
  HANDLE event = CreateEventW(&inherit, TRUE, FALSE, nullptr);
  WSADATA data{};
  (void)WSAStartup(MAKEWORD(2, 2), &data);
  const SOCKET socket_handle = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0, 0);
  (void)SetHandleInformation(reinterpret_cast<HANDLE>(socket_handle), HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
  const std::vector<HANDLE> canaries{file, event, reinterpret_cast<HANDLE>(socket_handle)};
  for (HANDLE canary : canaries) {
    DWORD flags = 0;
    expect(canary != nullptr && canary != INVALID_HANDLE_VALUE && GetHandleInformation(canary, &flags) && (flags & HANDLE_FLAG_INHERIT) != 0U,
           "precondition: the canary is inheritable");
  }
  secure_frontend::EngineStdioProcess child;
  expect(!child.start({.executable = test_support::fake_engine_path(), .arguments = {}, .environment = {}}).has_value(), "start");
  expect(child.read_line(5s).line == "READY 4096 stop", "READY");
  // Parent-side inspection (spec §3.11): snapshot the child's handle table,
  // duplicate each entry and compare objects; assert canary absence, not a count.
  using QueryFn = LONG(WINAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);
  using CompareFn = BOOL(WINAPI*)(HANDLE, HANDLE);
  const auto query = resolve<QueryFn>(L"ntdll.dll", "NtQueryInformationProcess");
  const auto compare = resolve<CompareFn>(L"kernelbase.dll", "CompareObjectHandles");
  HANDLE process = OpenProcess(PROCESS_DUP_HANDLE | PROCESS_QUERY_INFORMATION, FALSE, static_cast<DWORD>(child.pid()));
  expect(process != nullptr && query != nullptr && compare != nullptr, "inspection APIs are available (Windows 10 1607+)");
  if (process != nullptr && query != nullptr && compare != nullptr) {
    // True when some entry of a fresh snapshot of the engine's handle table is
    // the same kernel object as `target`. The snapshot itself is checked so a
    // failed or garbled inspection cannot pass as "absent".
    const auto engine_holds = [&](HANDLE target) -> bool {
      constexpr LONG info_length_mismatch = static_cast<LONG>(0xC0000004UL);  // STATUS_INFO_LENGTH_MISMATCH
      std::vector<unsigned char> buffer(1U << 16U);
      ULONG needed = 0;
      LONG status = query(process, 51 /* ProcessHandleInformation */, buffer.data(), static_cast<ULONG>(buffer.size()), &needed);
      for (int attempt = 0; status == info_length_mismatch && attempt < 4; ++attempt) {
        buffer.resize(std::max<std::size_t>(needed, buffer.size() * 2U) + 4096U);
        status = query(process, 51, buffer.data(), static_cast<ULONG>(buffer.size()), &needed);
      }
      if (status != 0) {
        std::ostringstream hex;
        hex << std::hex << static_cast<unsigned long>(static_cast<ULONG>(status));
        expect(false, "handle snapshot of the engine, NTSTATUS 0x" + hex.str());
        return false;
      }
      const auto* snapshot = reinterpret_cast<const HandleSnapshot*>(buffer.data());
      const std::size_t capacity = (buffer.size() - offsetof(HandleSnapshot, Handles)) / sizeof(HandleEntry);
      expect(snapshot->NumberOfHandles >= 3U && snapshot->NumberOfHandles <= capacity,
             "the snapshot lists a plausible handle count (at least stdio), saw " + std::to_string(snapshot->NumberOfHandles));
      const std::size_t count = std::min<std::size_t>(snapshot->NumberOfHandles, capacity);
      const HandleEntry* entries = snapshot->Handles;
      bool duplicated_any = false;
      bool found = false;
      for (std::size_t index = 0; index < count; ++index) {
        HANDLE duplicate = nullptr;
        if (!DuplicateHandle(process, entries[index].HandleValue, GetCurrentProcess(), &duplicate, 0, FALSE, DUPLICATE_SAME_ACCESS)) continue;
        duplicated_any = true;
        if (compare(duplicate, target)) found = true;
        CloseHandle(duplicate);
      }
      expect(duplicated_any, "at least one engine handle could be duplicated for comparison");
      return found;
    };
    for (HANDLE canary : canaries) expect(!engine_holds(canary), "a parent canary handle reached the engine");
    // Positive control: plant the event in the engine; the same inspection must now find it.
    HANDLE injected = nullptr;
    const bool planted = DuplicateHandle(GetCurrentProcess(), event, process, &injected, 0, FALSE, DUPLICATE_SAME_ACCESS) != 0;
    expect(planted, "positive control: the event is planted in the engine");
    if (planted) {
      expect(engine_holds(event), "positive control: the inspection finds a handle the engine does hold");
      (void)DuplicateHandle(process, injected, nullptr, nullptr, 0, FALSE, DUPLICATE_CLOSE_SOURCE);
    }
  }
  if (process != nullptr) CloseHandle(process);
  (void)child.terminate(250ms);
  CloseHandle(file);
  CloseHandle(event);
  (void)closesocket(socket_handle);
}
std::vector<unsigned char> current_token_user() {
  HANDLE token = nullptr;
  (void)OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token);
  DWORD size = 0;
  (void)GetTokenInformation(token, TokenUser, nullptr, 0, &size);
  std::vector<unsigned char> buffer(size);
  (void)GetTokenInformation(token, TokenUser, buffer.data(), size, &size);
  CloseHandle(token);
  return buffer;  // a TOKEN_USER
}

void proc02_stdin_pipe_dacl_is_owner_only() {
  secure_frontend::EngineStdioProcess child;
  expect(!child.start({.executable = test_support::fake_engine_path(), .arguments = {}, .environment = {}}).has_value(), "start");
  expect(child.read_line(5s).line == "READY 4096 stop", "READY");
  PACL dacl = nullptr;
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  const DWORD status = GetSecurityInfo(static_cast<HANDLE>(child.stdin_handle_for_tests()), SE_KERNEL_OBJECT, DACL_SECURITY_INFORMATION,
                                       nullptr, nullptr, &dacl, nullptr, &descriptor);
  expect(status == ERROR_SUCCESS && dacl != nullptr, "the pipe has an explicit DACL");
  if (status == ERROR_SUCCESS && dacl != nullptr) {
    ACL_SIZE_INFORMATION size{};
    expect(GetAclInformation(dacl, &size, sizeof(size), AclSizeInformation) && size.AceCount == 1U, "exactly one ACE");
    void* ace = nullptr;
    if (GetAce(dacl, 0, &ace)) {
      const auto* allowed = static_cast<ACCESS_ALLOWED_ACE*>(ace);
      const auto user = current_token_user();
      expect(allowed->Header.AceType == ACCESS_ALLOWED_ACE_TYPE, "an allow ACE");
      expect(EqualSid(const_cast<DWORD*>(&allowed->SidStart), reinterpret_cast<const TOKEN_USER*>(user.data())->User.Sid) != FALSE,
             "for the current user only");
    }
    SECURITY_DESCRIPTOR_CONTROL control = 0;
    DWORD revision = 0;
    expect(GetSecurityDescriptorControl(descriptor, &control, &revision) && (control & SE_DACL_PROTECTED) != 0, "protected: nothing inherited");
  }
  if (descriptor != nullptr) LocalFree(descriptor);
  const std::wstring& name = child.stdin_pipe_name_for_tests();
  expect(name.size() > 33U && name.find_last_of(L'-') == name.size() - 33U, "the name ends with a 128-bit random suffix");
  (void)child.terminate(250ms);
}

void proc03_windows_pipe_second_open_fails() {
  secure_frontend::EngineStdioProcess child;
  expect(!child.start({.executable = test_support::fake_engine_path(), .arguments = {}, .environment = {}}).has_value(), "start");
  expect(child.read_line(5s).line == "READY 4096 stop", "READY");
  // A squatter opens the read end, as the engine does: the single instance is
  // taken by our own client, so the pipe must be busy (not merely the wrong mode).
  HANDLE second = CreateFileW(child.stdin_pipe_name_for_tests().c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
  const DWORD error = second == INVALID_HANDLE_VALUE ? GetLastError() : 0U;
  if (second != INVALID_HANDLE_VALUE) CloseHandle(second);
  expect(second == INVALID_HANDLE_VALUE && error == ERROR_PIPE_BUSY,
         "a second client open of the engine stdin pipe is busy, error " + std::to_string(error));
  // Nor can a second server instance be created on the name (first-instance / one-instance guard).
  HANDLE rival = CreateNamedPipeW(child.stdin_pipe_name_for_tests().c_str(), PIPE_ACCESS_INBOUND, PIPE_TYPE_BYTE | PIPE_WAIT,
                                  PIPE_UNLIMITED_INSTANCES, 4096, 4096, 0, nullptr);
  const DWORD rival_error = rival == INVALID_HANDLE_VALUE ? GetLastError() : 0U;
  if (rival != INVALID_HANDLE_VALUE) CloseHandle(rival);
  expect(rival == INVALID_HANDLE_VALUE && (rival_error == ERROR_ACCESS_DENIED || rival_error == ERROR_PIPE_BUSY),
         "a second server instance on the name is refused, error " + std::to_string(rival_error));
  (void)child.terminate(250ms);
}
void proc01_windows_argv_round_trip() {
  const std::vector<std::string> tricky{"a \"quoted\" word", "C:\\path with space\\", "trailing\\", "back\\\\slashes\\\"",
                                        "%PATH%",           "^&|<>()",               "line\nbreak", "\xE2\x9C\x93 \xF0\x9F\x98\x80",
                                        "",                 "-flag=value"};
  std::vector<std::string> arguments{"--echo-cmdline"};
  arguments.insert(arguments.end(), tricky.begin(), tricky.end());
  secure_frontend::EngineStdioProcess child;
  expect(!child.start({.executable = test_support::fake_engine_path(), .arguments = arguments, .environment = {}}).has_value(), "start");
  expect(child.read_line(5s).line.rfind("CMDLINE ", 0) == 0, "the raw command line is reported first");
  expect(child.read_line(5s).line == "READY 4096 stop", "READY");
  const auto dump = test_support::request_dump(child, 5s);
  std::vector<std::string> expected{test_support::utf8_path(test_support::fake_engine_path()), "--serve"};
  expected.insert(expected.end(), arguments.begin(), arguments.end());
  expect(dump && dump->argv == expected, "CommandLineToArgvW in the child yields exactly the argv the frontend was given");
  (void)child.terminate(250ms);
}

// This process's user SID, elevation and integrity RID, keyed as the fake
// engine's DUMP `IDS` line keys them.
std::map<std::string, std::string, std::less<>> own_token_ids() {
  std::map<std::string, std::string, std::less<>> ids;
  HANDLE token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return ids;
  const auto user = current_token_user();
  LPWSTR sid = nullptr;
  if (!user.empty() && ConvertSidToStringSidW(reinterpret_cast<const TOKEN_USER*>(user.data())->User.Sid, &sid)) {
    const std::wstring wide(sid);
    std::string ascii;  // SID strings are ASCII
    for (const wchar_t c : wide) ascii.push_back(static_cast<char>(c));
    ids["sid"] = ascii;
    LocalFree(sid);
  }
  TOKEN_ELEVATION elevation{};
  DWORD size = sizeof(elevation);
  ids["elevated"] = GetTokenInformation(token, TokenElevation, &elevation, size, &size) && elevation.TokenIsElevated != 0 ? "1" : "0";
  size = 0;
  (void)GetTokenInformation(token, TokenIntegrityLevel, nullptr, 0, &size);
  std::vector<unsigned char> label(size);
  if (size != 0 && GetTokenInformation(token, TokenIntegrityLevel, label.data(), size, &size)) {
    PSID level = reinterpret_cast<TOKEN_MANDATORY_LABEL*>(label.data())->Label.Sid;
    ids["integrity"] = std::to_string(*GetSidSubAuthority(level, static_cast<DWORD>(*GetSidSubAuthorityCount(level) - 1U)));
  }
  CloseHandle(token);
  return ids;
}
#endif

// G1 (spec §3.12; relative identity rule by owner decision, Global
// Constraints): the engine runs in <state_dir>/engine and never with more
// privilege than the frontend.
void proc06_child_ids_and_cwd() {
  const auto state = secure_frontend_test::make_scratch_directory() / "g1";
  secure_frontend_test::make_owner_private_directory(state);
  const auto cwd = state / "engine";
  secure_frontend_test::make_private_directory(cwd);
  secure_frontend::EngineStdioProcess child;
  expect(!child.start({.executable = test_support::fake_engine_path(), .arguments = {}, .environment = {}, .working_directory = cwd})
              .has_value(),
         "start");
  expect(child.read_line(5s).line == "READY 4096 stop", "READY");
  const auto dump = test_support::request_dump(child, 5s);
  expect(dump.has_value(), "DUMP");
  if (dump) {
    std::error_code error;
    expect(std::filesystem::equivalent(dump->cwd, cwd, error), "the engine runs in <state_dir>/engine (its cwd-relative files land there)");
#if defined(_WIN32)
    const auto parent = own_token_ids();
    expect(dump->ids.count("sid") == 1 && parent.count("sid") == 1 && dump->ids.at("sid") == parent.at("sid"), "same user SID");
    // Relative by owner decision (Global Constraints): CI runs this binary
    // elevated, so an elevated engine under an elevated frontend is allowed;
    // an engine MORE elevated than the frontend never is.
    expect(dump->ids.count("elevated") == 1 && parent.count("elevated") == 1 &&
               !(dump->ids.at("elevated") == "1" && parent.at("elevated") == "0"),
           "never more elevated than the frontend: no elevation is requested");
    expect(dump->ids.count("integrity") == 1 && parent.count("integrity") == 1 &&
               std::stoul(dump->ids.at("integrity")) <= std::stoul(parent.at("integrity")),
           "integrity never above the frontend's");
#else
    expect(dump->ids.count("uid") == 1 && dump->ids.count("gid") == 1 && dump->ids.count("euid") == 1 && dump->ids.count("egid") == 1,
           "IDS carries uid, gid, euid and egid");
    if (dump->ids.count("uid") == 1 && dump->ids.count("gid") == 1 && dump->ids.count("euid") == 1 && dump->ids.count("egid") == 1) {
      expect(dump->ids.at("uid") == std::to_string(getuid()) && dump->ids.at("gid") == std::to_string(getgid()) &&
                 dump->ids.at("euid") == std::to_string(geteuid()) && dump->ids.at("egid") == std::to_string(getegid()),
             "the same uid/gid as the frontend: nothing gained");
    }
#endif
  }
  (void)child.terminate(250ms);
}

#if defined(_WIN32)
// G3 -- UNVERIFIED in CI (the runners are elevated, so requireAdministrator
// needs no elevation and error 740 cannot occur) and under wine (no UAC). Not
// registered with CTest and not a skipped test. Validation kit, owner's
// offline Windows box, from a NON-elevated prompt:
//   secure_frontend_engine_child_tests.exe --case proc06_elevation_required_is_spawn_failed
void proc06_elevation_required_is_spawn_failed() {
  const bool elevated = own_token_ids()["elevated"] != "0";
  expect(!elevated, "run this case from a non-elevated prompt: an elevated token never sees error 740");
  if (elevated) return;
  secure_frontend::EngineStdioProcess child;
  const auto error = child.start({.executable = std::filesystem::path(STRATA_FAKE_ENGINE_ELEVATED_PATH), .arguments = {}, .environment = {}});
  expect(error && error->code == secure_frontend::ProcessErrorCode::spawn_failed && error->message == "engine requires elevation; refused",
         "ERROR_ELEVATION_REQUIRED is spawn_failed and is never retried via runas");
}
#endif

#if !defined(_WIN32)
// This test process's own NoNewPrivs ("1" when an outer sandbox already set
// it: then every child inherits it and "not requested" cannot read "0").
std::string own_no_new_privs() {
  std::ifstream status("/proc/self/status");
  std::string line;
  while (std::getline(status, line)) {
    if (line.rfind("NoNewPrivs:", 0) == 0) return line.substr(line.find_first_not_of(" \t", 11));
  }
  return "?";
}

// G2 (spec §3.12): PR_SET_NO_NEW_PRIVS before execve when the command asks for
// it; the server asks by default and --allow-new-privileges turns it off.
void proc06_child_has_no_new_privs() {
  for (const bool requested : {true, false}) {
    secure_frontend::EngineStdioProcess child;
    expect(!child.start({.executable = test_support::fake_engine_path(), .arguments = {}, .environment = {}, .no_new_privileges = requested})
                .has_value(),
           "start");
    expect(child.read_line(5s).line == "READY 4096 stop", "READY");
    const auto dump = test_support::request_dump(child, 5s);
    const std::string expected = requested || own_no_new_privs() == "1" ? "1" : "0";
    expect(dump && dump->ids.count("nnp") == 1 && dump->ids.at("nnp") == expected,
           std::string("NoNewPrivs is ") + expected + (requested ? " when requested" : " when not requested"));
    (void)child.terminate(250ms);
  }
  // The production default is ON; --allow-new-privileges turns it off.
  for (const bool allow : {false, true}) {
    const auto dir = test_support::make_temp_dir("g2");
    test_support::ServerOptions options;
    options.engine_args = {"--dump-file=" + test_support::utf8_path(dir / "dump.txt")};
    if (allow) options.extra_args = {"--allow-new-privileges"};
    auto server = test_support::ServerProcess::start(options);
    expect(server != nullptr && server->wait_until_listening(10s), "server listens");
    const auto dump = test_support::read_dump_file(dir / "dump.txt", 10s);
    const std::string expected = !allow || own_no_new_privs() == "1" ? "1" : "0";
    expect(dump && dump->ids.count("nnp") == 1 && dump->ids.at("nnp") == expected,
           allow ? "--allow-new-privileges leaves NoNewPrivs off" : "the server sets NoNewPrivs by default");
  }
}
#endif

#if !defined(_WIN32)
// ---- SP4 launch seam (SP2 R4; SP4 spec §3.2), POSIX half ---------------------
namespace launch = secure_frontend::posix_launch;

// What the seam hook checks in the child. Filled before fork(): the child
// only reads it (async-signal-safe).
struct SeamHookContext final {
  dev_t devices[2]{};
  ino_t inodes[2]{};
  int swept_parent_fd{-1};  // open in the parent, not kept: the sweep must have closed it
};

// Runs in the forked child after the sweep. Distinct errno values name the
// check that failed, so a RED run says which property broke.
int seam_hook(const void* raw, const std::span<const int> lifted) noexcept {
  const auto* context = static_cast<const SeamHookContext*>(raw);
  if (lifted.size() != 2U) return E2BIG;
  for (std::size_t index = 0; index < 2U; ++index) {
    if (lifted[index] < launch::kLiftFloor) return ERANGE;  // lifted at or above the floor
    const int flags = fcntl(lifted[index], F_GETFD);
    if (flags < 0 || (flags & FD_CLOEXEC) == 0) return ENOTSUP;  // lifted copies are close-on-exec
    struct stat status {};
    if (fstat(lifted[index], &status) != 0 || status.st_dev != context->devices[index] || status.st_ino != context->inodes[index]) {
      return EDOM;  // the same open files, in keep_fds order
    }
  }
  if (fcntl(context->swept_parent_fd, F_GETFD) != -1) return EEXIST;  // the hook runs after the sweep
  // SP4's own move (§3.2): the first kept descriptor becomes the child's fd 4.
  if (dup2(lifted[0], 4) != 4) return errno;
  return 0;
}

int seam_hook_refuses(const void*, std::span<const int>) noexcept { return EPERM; }

struct SeamLaunch final {
  std::string report;  // raw report bytes; empty = execve succeeded
  int status{-1};
};

// Forks and runs exec_child exactly as the launchers do (signals blocked
// across fork; stdout and stderr to /dev/null).
SeamLaunch seam_launch(const secure_frontend::EngineCommand& command, const launch::ExecOptions& options) {
  launch::PreparedExec prepared;
  launch::prepare(prepared, command, /*insert_serve=*/false);
  SeamLaunch result;
  int report[2]{-1, -1};
  const int sink = open("/dev/null", O_WRONLY | O_CLOEXEC);
  if (sink < 0 || pipe2(report, O_CLOEXEC) != 0) {
    expect(false, "seam launch setup");
    return result;
  }
  pid_t child = -1;
  {
    launch::BlockAllSignals blocked;
    child = fork();
    if (child == 0) launch::exec_child(prepared, -1, sink, /*null_stderr=*/true, report[1], options);
  }
  (void)close(report[1]);
  (void)close(sink);
  expect(child > 0, "fork");
  result.report = launch::read_report(report[0]);
  (void)close(report[0]);
  if (child > 0) (void)waitpid(child, &result.status, 0);
  return result;
}

// keep_fds holds a parent descriptor numbered 3 (the number SP2 dup2s its
// report pipe onto) and one above 3. The hook sees both lifted >= kLiftFloor,
// close-on-exec, in keep_fds order, after the sweep; it dup2s the first to 4.
// The child then holds exactly 0, 1, 2 and 4, and fd 4 is the parent's fd 3.
void sp4_seam_keep_fds_lift_and_hook_order() {
  const auto dir = test_support::make_temp_dir("sp4p");
  const auto first_path = dir / "first.txt";
  const auto second_path = dir / "second.txt";
  // Free fd 3 so the first kept file is opened as fd 3 (SP4's test-hook shape).
  const int saved3 = fcntl(3, F_GETFD) >= 0 ? fcntl(3, F_DUPFD_CLOEXEC, 20) : -1;
  if (saved3 >= 0) (void)close(3);
  const int first = open(first_path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
  const int second = open(second_path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
  const int swept = open("/dev/null", O_RDONLY);  // inheritable on purpose: only the sweep closes it
  expect(first == 3, "precondition: the first kept descriptor is fd 3, saw " + std::to_string(first));
  expect(second > 3 && swept > 3, "precondition: the second kept descriptor is above 3");
  SeamHookContext context;
  struct stat status {};
  for (const auto& [index, fd] : {std::pair{0, first}, std::pair{1, second}}) {
    expect(fstat(fd, &status) == 0, "fstat");
    context.devices[index] = status.st_dev;
    context.inodes[index] = status.st_ino;
  }
  context.swept_parent_fd = swept;
  const std::array<int, 2> keep{first, second};
  const auto dump_path = dir / "dump.txt";
  const auto launched = seam_launch(
      {.executable = test_support::fake_engine_path(),
       .arguments = {"--exit-before-ready=0", "--write-inherited=4", "--dump-file=" + test_support::utf8_path(dump_path)},
       .environment = {}},
      launch::ExecOptions{keep, seam_hook, &context});
  int hook_errno = 0;
  if (launched.report.size() == 1U + sizeof(int)) std::memcpy(&hook_errno, launched.report.data() + 1, sizeof(int));
  expect(launched.report.empty(), "execve succeeded (report bytes " + std::to_string(launched.report.size()) + ", errno " +
                                      std::to_string(hook_errno) + ")");
  expect(WIFEXITED(launched.status) && WEXITSTATUS(launched.status) == 0, "the fake wrote through fd 4 and exited 0");
  const auto dump = test_support::read_dump_file(dump_path, 10s);
  expect(dump && dump->fds == std::vector<int>{0, 1, 2, 4},
         "the child holds exactly stdio and the hook's fd 4: the hook ran after the sweep, and the lifted copies were close-on-exec");
  std::ifstream written(first_path);
  const std::string text{std::istreambuf_iterator<char>(written), std::istreambuf_iterator<char>()};
  expect(text == "INHERITED 4\n", "fd 4 in the child is the parent's fd 3: it survived the 0..3 dup2 sequence, saw '" + text + "'");
  for (const int fd : {first, second, swept}) (void)close(fd);
  if (saved3 >= 0) {
    (void)dup2(saved3, 3);
    (void)close(saved3);
  }
}

// A hook that returns EPERM: report is exactly 'H' + EPERM (SP4's classifier:
// size == 1 + sizeof(int) && front() == 'H'), exit 127, distinct from an
// execve failure (errno only). A keep_fds entry that cannot be lifted fails
// closed the same way as a hook failure. Both launchers' texts differ.
void sp4_seam_hook_failure_is_distinct_from_execve_failure() {
  const auto marked = [](const std::string& report, const int expected) {
    int value = 0;
    if (report.size() == 1U + sizeof(int)) std::memcpy(&value, report.data() + 1, sizeof(int));
    return report.size() == 1U + sizeof(int) && report.front() == launch::kHookFailureMarker && value == expected;
  };
  const secure_frontend::EngineCommand fake{.executable = test_support::fake_engine_path(), .arguments = {"--exit-before-ready=0"}, .environment = {}};
  const std::array<int, 1> stdin_only{0};
  const auto refused = seam_launch(fake, launch::ExecOptions{stdin_only, seam_hook_refuses, nullptr});
  expect(marked(refused.report, EPERM), "a hook failure reports 'H' + EPERM");
  expect(WIFEXITED(refused.status) && WEXITSTATUS(refused.status) == 127, "and the child _exits (127) without execve");
  const auto classified = launch::classify_report(refused.report);
  expect(classified.kind == launch::ChildReportKind::hook_failed && classified.error == EPERM, "classified as a hook failure");
  expect(launch::describe_report(classified) == std::string("pre-exec hook failed: ") + std::strerror(EPERM), "hook failure text");

  const auto missing = seam_launch({.executable = "/nonexistent/strata-r4-missing", .arguments = {}, .environment = {}}, launch::ExecOptions{});
  int exec_errno = 0;
  if (missing.report.size() == sizeof(int)) std::memcpy(&exec_errno, missing.report.data(), sizeof(int));
  expect(missing.report.size() == sizeof(int) && exec_errno == ENOENT, "an execve failure keeps SP2's report: errno only, no marker");
  const auto exec_failure = launch::classify_report(missing.report);
  expect(exec_failure.kind == launch::ChildReportKind::exec_failed && exec_failure.error == ENOENT, "classified as an execve failure");
  expect(launch::describe_report(exec_failure) == std::string("execve failed: ") + std::strerror(ENOENT), "execve failure text is unchanged");
  expect(launch::describe_report(exec_failure) != launch::describe_report(classified), "the two failures read distinctly");
  expect(launch::classify_report("").kind == launch::ChildReportKind::none, "no bytes: execve succeeded");
  expect(launch::classify_report("xyz").kind == launch::ChildReportKind::malformed, "anything else is malformed, never success");
  // read_report keeps at most 16 bytes, however much the pipe holds.
  int flood[2]{-1, -1};
  expect(pipe2(flood, O_CLOEXEC) == 0, "pipe");
  const std::string forty(40, 'x');
  expect(write(flood[1], forty.data(), forty.size()) == static_cast<ssize_t>(forty.size()), "fill");
  (void)close(flood[1]);
  const auto bounded = launch::read_report(flood[0]);
  (void)close(flood[0]);
  expect(bounded.size() == 16U, "read_report stops at 16 bytes, read " + std::to_string(bounded.size()));
  expect(launch::classify_report(bounded).kind == launch::ChildReportKind::malformed, "and a long report is malformed");

  // Lifting fails closed: an fd that is not open cannot be lifted (EBADF); the hook never runs.
  // (A high number nothing here opens: a just-closed low number would be reused by the launch's own open.)
  int closed_fd = 900;
  while (fcntl(closed_fd, F_GETFD) != -1) ++closed_fd;
  const std::array<int, 1> not_open{closed_fd};
  const auto unliftable = seam_launch(fake, launch::ExecOptions{not_open, seam_hook_refuses, nullptr});
  expect(marked(unliftable.report, EBADF), "a keep_fds lift failure reports 'H' + EBADF (fail closed)");

  // The engine launcher passes ExecOptions{} and still reports execve failures as before.
  secure_frontend::EngineStdioProcess engine;
  const auto error = engine.start({.executable = "/nonexistent/strata-r4-missing", .arguments = {}, .environment = {}});
  expect(error && error->code == secure_frontend::ProcessErrorCode::spawn_failed &&
             error->message == std::string("execve failed: ") + std::strerror(ENOENT),
         "the engine's execve failure text is unchanged");
}
#endif

#if defined(_WIN32)
// ---- SP4 launch seam (SP2 R4; SP4 spec §3.2), Windows half -------------------
// launch_in_job with one extra inheritable handle and a CRT block whose entry 4
// is that handle (SP4's test-hook layout, n = 5): the fake parses the block from
// STARTUPINFO and writes through entry 4. A launch without the extra handle (same
// block) cannot write: the explicit handle list, not inheritance, carries it.
void sp4_seam_launch_in_job_handle_and_crt_block() {
  namespace wl = secure_frontend::win_launch;
  const auto dir = test_support::make_temp_dir("sp4w");
  const auto record = dir / "record.txt";
  SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
  HANDLE record_handle = CreateFileW(record.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, CREATE_ALWAYS,
                                     FILE_ATTRIBUTE_NORMAL, nullptr);
  HANDLE in = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, OPEN_EXISTING, 0, nullptr);
  HANDLE out = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, OPEN_EXISTING, 0, nullptr);
  HANDLE err = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, OPEN_EXISTING, 0, nullptr);
  expect(record_handle != INVALID_HANDLE_VALUE && in != INVALID_HANDLE_VALUE && out != INVALID_HANDLE_VALUE && err != INVALID_HANDLE_VALUE,
         "handles opened");
  // Microsoft CRT inheritance layout, packed: int n, n flag bytes, n HANDLEs.
  constexpr int kEntries = 5;
  std::vector<std::uint8_t> block(sizeof(int) + kEntries * (1U + sizeof(HANDLE)));
  std::memcpy(block.data(), &kEntries, sizeof(int));
  const std::array<std::uint8_t, kEntries> flags{0x41, 0x41, 0x41, 0x00, 0x01};  // FOPEN|FDEV x3, unused, FOPEN
  std::memcpy(block.data() + sizeof(int), flags.data(), flags.size());
  const std::array<HANDLE, kEntries> handles{in, out, err, INVALID_HANDLE_VALUE, record_handle};
  std::memcpy(block.data() + sizeof(int) + kEntries, handles.data(), sizeof(handles));
  const std::wstring executable = test_support::fake_engine_path().wstring();
  const std::wstring line = L"\"" + executable + L"\" --exit-before-ready=0 --write-inherited=4";
  std::wstring environment;
  std::vector<wchar_t> root(32768);
  const DWORD root_length = GetEnvironmentVariableW(L"SystemRoot", root.data(), static_cast<DWORD>(root.size()));
  if (root_length != 0 && root_length < root.size()) environment = L"SystemRoot=" + std::wstring(root.data(), root_length);
  environment.push_back(L'\0');
  environment.push_back(L'\0');

  // engine_job = false: SP4 Task 26's exact positional shape (executable ..
  // crt_fd_block), so the Job options keep their defaults, which are ON (RF11
  // amended). engine_job = true: both turned off, as the engine's start() does.
  const auto run = [&](const bool with_extra, const bool engine_job) -> std::optional<DWORD> {
    wl::WinLaunchSpec spec{executable, line, environment, L"", in, out, err, nullptr,
                           with_extra ? std::vector<HANDLE>{record_handle} : std::vector<HANDLE>{}, block};
    if (engine_job) {
      spec.limit_active_process_to_one = false;
      spec.restrict_ui = false;
    }
    const auto launched = wl::launch_in_job(spec);
    if (const auto* code = std::get_if<DWORD>(&launched)) {
      expect(false, "launch_in_job failed (Win32 error " + std::to_string(*code) + ")");
      return std::nullopt;
    }
    const auto child = std::get<wl::WinLaunched>(launched);
    std::optional<DWORD> exit_code;
    if (WaitForSingleObject(child.process, 10000) == WAIT_OBJECT_0) {
      DWORD code = 0;
      if (GetExitCodeProcess(child.process, &code)) exit_code = code;
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION extended{};
    JOBOBJECT_BASIC_UI_RESTRICTIONS ui{};
    const bool queried =
        QueryInformationJobObject(child.job, JobObjectExtendedLimitInformation, &extended, sizeof(extended), nullptr) != FALSE &&
        QueryInformationJobObject(child.job, JobObjectBasicUIRestrictions, &ui, sizeof(ui), nullptr) != FALSE;
    const DWORD flags = extended.BasicLimitInformation.LimitFlags;
    expect(queried && (flags & JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE) != 0 && (flags & JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION) != 0,
           "every Job is KILL_ON_JOB_CLOSE | DIE_ON_UNHANDLED_EXCEPTION");
    if (engine_job) {
      expect(queried && (flags & JOB_OBJECT_LIMIT_ACTIVE_PROCESS) == 0 && ui.UIRestrictionsClass == 0,
             "the engine's spec (both options false) keeps the engine Job: no ACTIVE_PROCESS limit, no UI restriction");
    } else {
      expect(queried && (flags & JOB_OBJECT_LIMIT_ACTIVE_PROCESS) != 0 && extended.BasicLimitInformation.ActiveProcessLimit == 1,
             "the default spec (SP4's positional shape) gets ACTIVE_PROCESS = 1");
      expect(queried && ui.UIRestrictionsClass == wl::kAllUiRestrictions, "and every UI restriction");
    }
    (void)TerminateJobObject(child.job, 1);
    CloseHandle(child.thread);
    CloseHandle(child.process);
    CloseHandle(child.job);
    return exit_code;
  };
  const auto read_record = [&] {
    std::ifstream input(record, std::ios::binary);
    return std::string{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
  };
  const auto with_handle = run(true, false);
  expect(with_handle == 0U, "the fake found the handle at CRT entry 4 and wrote through it (exit 0)");
  expect(read_record() == "INHERITED 4\n", "the extra handle and the CRT block reached the child");
  const auto without_handle = run(false, true);
  expect(without_handle.has_value() && *without_handle != 0U, "without the extra handle in the list the child cannot write");
  expect(read_record() == "INHERITED 4\n", "and nothing more was written");
  for (HANDLE handle : {record_handle, in, out, err}) CloseHandle(handle);
}

// An extra handle that is not inheritable is refused before anything is
// created: ERROR_INVALID_HANDLE at step `attributes` (header contract).
void sp4_seam_launch_in_job_rejects_non_inheritable_handle() {
  namespace wl = secure_frontend::win_launch;
  SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
  HANDLE in = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, OPEN_EXISTING, 0, nullptr);
  HANDLE out = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, OPEN_EXISTING, 0, nullptr);
  HANDLE private_handle = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
  expect(in != INVALID_HANDLE_VALUE && out != INVALID_HANDLE_VALUE && private_handle != INVALID_HANDLE_VALUE, "handles opened");
  DWORD flags = 0;
  expect(GetHandleInformation(private_handle, &flags) != FALSE && (flags & HANDLE_FLAG_INHERIT) == 0, "precondition: not inheritable");
  const std::wstring executable = test_support::fake_engine_path().wstring();
  const std::wstring line = L"\"" + executable + L"\" --exit-before-ready=0";
  std::wstring environment{L'\0'};
  wl::WinLaunchSpec spec{executable, line, environment, L"", in, out, out, nullptr, std::vector<HANDLE>{private_handle}, {}};
  wl::WinLaunchStep step = wl::WinLaunchStep::none;
  const auto launched = wl::launch_in_job(spec, &step);
  const auto* code = std::get_if<DWORD>(&launched);
  expect(code != nullptr && *code == ERROR_INVALID_HANDLE && step == wl::WinLaunchStep::attributes,
         "a non-inheritable extra handle is ERROR_INVALID_HANDLE at the attributes step");
  if (const auto* child = std::get_if<wl::WinLaunched>(&launched)) {
    (void)TerminateJobObject(child->job, 1);
    for (HANDLE handle : {child->thread, child->process, child->job}) CloseHandle(handle);
  }
  for (HANDLE handle : {in, out, private_handle}) CloseHandle(handle);
}
#endif

}  // namespace

int main(int argc, char* argv[]) {
  return secure_frontend_test::run_cases(argc, argv,
                                         {
                                             {"proc04_exit_status_is_classified", proc04_exit_status_is_classified},
                                             {"proc06_child_ids_and_cwd", proc06_child_ids_and_cwd},
#if !defined(_WIN32)
                                             {"proc05_group_killed_after_leader_reaped", proc05_group_killed_after_leader_reaped},
                                             {"proc02_child_signal_state_is_default", proc02_child_signal_state_is_default},
                                             {"proc02_canary_descriptors_absent", proc02_canary_descriptors_absent},
                                             {"proc03_other_uid_cannot_open_engine_fd", proc03_other_uid_cannot_open_engine_fd},
                                             {"proc03_same_uid_reopen_is_accepted_risk", proc03_same_uid_reopen_is_accepted_risk},
                                             {"proc06_child_has_no_new_privs", proc06_child_has_no_new_privs},
                                             {"sp4_seam_keep_fds_lift_and_hook_order", sp4_seam_keep_fds_lift_and_hook_order},
                                             {"sp4_seam_hook_failure_is_distinct_from_execve_failure", sp4_seam_hook_failure_is_distinct_from_execve_failure},
#endif
#if defined(_WIN32)
                                             {"proc02_canary_handles_absent", proc02_canary_handles_absent},
                                             {"proc02_stdin_pipe_dacl_is_owner_only", proc02_stdin_pipe_dacl_is_owner_only},
                                             {"proc03_windows_pipe_second_open_fails", proc03_windows_pipe_second_open_fails},
                                             {"proc01_windows_argv_round_trip", proc01_windows_argv_round_trip},
                                             {"sp4_seam_launch_in_job_handle_and_crt_block", sp4_seam_launch_in_job_handle_and_crt_block},
                                             {"sp4_seam_launch_in_job_rejects_non_inheritable_handle", sp4_seam_launch_in_job_rejects_non_inheritable_handle},
                                             // G3: in the case list only, never registered with CTest.
                                             {"proc06_elevation_required_is_spawn_failed", proc06_elevation_required_is_spawn_failed},
#endif
                                         },
                                         failures);
}
