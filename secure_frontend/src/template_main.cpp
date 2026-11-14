// A value-only native Jinja interpreter in a fresh, resource-bounded process.
// No filesystem/network/import/eval/host-object functions are registered.
#include "secure_frontend/jinja_renderer.hpp"
#include "secure_frontend/json.hpp"
#include "jinja/parser.h"
#include "jinja/runtime.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <variant>
#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/resource.h>
#include <sys/prctl.h>
#include <unistd.h>
#if defined(__linux__)
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sys/syscall.h>
#endif
#endif

namespace {
using namespace secure_frontend;
constexpr std::size_t kRequestMax = 8U * 1024U * 1024U;
bool confine() {
#if defined(_WIN32)
  // A nested job is supported on Windows 8+; failure refuses all rendering.
  static HANDLE job = CreateJobObjectW(nullptr, nullptr);
  if (job == nullptr) return false;
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
  limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_PROCESS_MEMORY | JOB_OBJECT_LIMIT_PROCESS_TIME |
      JOB_OBJECT_LIMIT_ACTIVE_PROCESS | JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION;
  limits.BasicLimitInformation.ActiveProcessLimit = 1;
  limits.BasicLimitInformation.PerProcessUserTimeLimit.QuadPart = 20000000LL;
  limits.ProcessMemoryLimit = 256U * 1024U * 1024U;
  return SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)) != 0 &&
         AssignProcessToJobObject(job, GetCurrentProcess()) != 0;
#else
  const rlimit memory{256U * 1024U * 1024U, 256U * 1024U * 1024U};
  const rlimit cpu{2, 2};
  const rlimit core{0, 0};
  if (setrlimit(RLIMIT_AS, &memory) != 0 || setrlimit(RLIMIT_CPU, &cpu) != 0 ||
      setrlimit(RLIMIT_CORE, &core) != 0) return false;
#if defined(__linux__) && (defined(__x86_64__) || defined(__aarch64__))
  // Libraries are loaded. The interpreter needs only stdio, allocation, time,
  // synchronization and exit. No opens, network, exec, fork, ptrace or namespace calls.
#define ALLOW(n) BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, __NR_##n, 0, 1), BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW)
  const sock_filter filter[] = {
      BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, arch)),
#if defined(__x86_64__)
      BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AUDIT_ARCH_X86_64, 1, 0),
#else
      BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, AUDIT_ARCH_AARCH64, 1, 0),
#endif
      BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS),
      BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, nr)),
      ALLOW(read), ALLOW(write), ALLOW(close), ALLOW(fstat), ALLOW(lseek),
      ALLOW(brk), ALLOW(mmap), ALLOW(munmap), ALLOW(mprotect), ALLOW(mremap), ALLOW(madvise),
      ALLOW(rt_sigaction), ALLOW(rt_sigprocmask), ALLOW(rt_sigreturn), ALLOW(futex),
      ALLOW(clock_gettime), ALLOW(getrandom), ALLOW(exit), ALLOW(exit_group),
      BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | 1U)};
#undef ALLOW
  const sock_fprog program{static_cast<unsigned short>(sizeof(filter) / sizeof(filter[0])),
                          const_cast<sock_filter*>(filter)};
  return prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) == 0 &&
         prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &program) == 0;
#else
  return false; // no unconfined fallback on an unsupported platform
#endif
#endif
}

jinja::value convert(const JsonValue& value, const bool input = true, const bool message = false) {
  using namespace jinja;
  if (const auto* v = value.if_string()) {
    auto out = mk_val<value_string>(*v);
    if (input) out->mark_input();
    return out;
  }
  if (const auto* v = value.if_boolean()) return mk_val<value_bool>(*v);
  if (const auto* v = value.if_integer()) return mk_val<value_int>(*v);
  if (const auto* v = value.if_decimal()) return mk_val<value_float>(*v);
  if (const auto* v = value.if_array()) {
    auto out = mk_val<value_array>();
    for (const auto& item : *v) out->push_back(convert(item, input, message));
    return out;
  }
  if (const auto* v = value.if_object()) {
    auto out = mk_val<value_object>();
    for (const auto& member : *v) {
      bool mark = input;
      if (message && member.key == "role") {
        const auto* role = member.value.if_string();
        if (role == nullptr || (*role != "system" && *role != "user" && *role != "assistant" && *role != "tool"))
          throw std::runtime_error("invalid role");
        mark = false;
      }
      auto key = mk_val<value_string>(member.key);
      if (input && !message) key->mark_input();
      out->insert(key, convert(member.value, mark));
    }
    return out;
  }
  return mk_val<value_none>();
}

int render() {
  std::string request;
  for (int c = std::getchar(); c != EOF && c != '\n'; c = std::getchar()) {
    if (request.size() == kRequestMax) return 2;
    request.push_back(static_cast<char>(c));
  }
  auto parsed = parse_json(request, {kRequestMax, 66U, 65536U, kTemplateContextMax, true});
  const auto* json = std::get_if<JsonValue>(&parsed);
  if (json == nullptr || json->if_object() == nullptr) return 2;
  const std::string* source = nullptr;
  const JsonValue* data = nullptr;
  for (const auto& field : *json->if_object()) {
    if (field.key == "template") source = field.value.if_string();
    else if (field.key == "context") data = &field.value;
    else return 2;
  }
  if (source == nullptr || source->empty() || source->size() > kTemplateSourceMax ||
      data == nullptr || data->if_object() == nullptr) return 2;
  jinja::lexer lexer;
  auto tokens = lexer.tokenize(*source);
  auto program = jinja::parse_from_tokens(tokens);
  jinja::context context; // no source in exceptions; only fixed errors cross the boundary
  context.current_time = 0; // reproducible; no local clock data exposed to templates
  for (const auto& field : *data->if_object()) {
    if (field.key == "bos_token" || field.key == "eos_token") context.set_val(field.key, convert(field.value, false));
    else context.set_val(field.key, convert(field.value, true, field.key == "messages"));
  }
  jinja::runtime runtime(context);
  auto output = jinja::runtime::gather_string_parts(runtime.execute(program));
  std::size_t total = 0;
  std::string answer = "[";
  bool first = true;
  for (const auto& part : output->val_str.parts) {
    if (part.val.empty()) continue;
    if (part.val.size() > kTemplateOutputMax - total || !is_valid_utf8(part.val)) return 2;
    total += part.val.size();
    if (!first) answer += ',';
    first = false;
    answer += "{\"text\":";
    append_json_string(&answer, part.val);
    answer += part.is_input ? ",\"input\":true}" : ",\"input\":false}";
    if (answer.size() > kRequestMax) return 2;
  }
  answer += "]\n";
  return std::fwrite(answer.data(), 1, answer.size(), stdout) == answer.size() ? 0 : 2;
}
} // namespace

int main() {
  if (!confine()) return 3;
  try { return render(); }
  catch (...) { std::fputs("null\n", stdout); return 2; }
}
