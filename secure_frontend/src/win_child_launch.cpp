#include "win_child_launch.hpp"

#if defined(_WIN32)

#include "secure_frontend/exit_status.hpp"

#include <cstddef>

namespace secure_frontend::win_launch {

std::variant<WinLaunched, DWORD> launch_in_job(const WinLaunchSpec& spec, WinLaunchStep* failed_step) {
  const auto failure = [failed_step](const WinLaunchStep step, const DWORD code) -> std::variant<WinLaunched, DWORD> {
    if (failed_step != nullptr) *failed_step = step;
    return code;
  };
  if (failed_step != nullptr) *failed_step = WinLaunchStep::none;
  // Seam inputs are checked before anything is created.
  if (spec.crt_fd_block.size() > 0xFFFFU) return failure(WinLaunchStep::attributes, ERROR_INVALID_PARAMETER);
  for (HANDLE handle : spec.extra_inherited_handles) {
    DWORD flags = 0;
    if (!GetHandleInformation(handle, &flags)) return failure(WinLaunchStep::attributes, GetLastError());
    if ((flags & HANDLE_FLAG_INHERIT) == 0) return failure(WinLaunchStep::attributes, ERROR_INVALID_HANDLE);
  }
  HANDLE job = CreateJobObjectW(nullptr, nullptr);
  if (job == nullptr) return failure(WinLaunchStep::job, GetLastError());
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
  // Spec §3.7: an unhandled exception in any Job process ends it at once
  // (no WER wait), and closing the Job kills every member.
  limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION;
  if (spec.limit_active_process_to_one) {
    limits.BasicLimitInformation.LimitFlags |= JOB_OBJECT_LIMIT_ACTIVE_PROCESS;
    limits.BasicLimitInformation.ActiveProcessLimit = 1;
  }
  JOBOBJECT_BASIC_UI_RESTRICTIONS ui{kAllUiRestrictions};
  if (!SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)) ||
      (spec.restrict_ui && !SetInformationJobObject(job, JobObjectBasicUIRestrictions, &ui, sizeof(ui)))) {
    const DWORD error = GetLastError();
    CloseHandle(job);
    return failure(WinLaunchStep::job, error);
  }
  // Only these handles are inherited, whatever else is inheritable in this process.
  std::vector<HANDLE> inherited{spec.stdin_read, spec.stdout_write, spec.stderr_handle};
  inherited.insert(inherited.end(), spec.extra_inherited_handles.begin(), spec.extra_inherited_handles.end());
  SIZE_T attribute_list_size = 0;
  (void)InitializeProcThreadAttributeList(nullptr, 1, 0, &attribute_list_size);
  std::vector<std::byte> attribute_list_storage(attribute_list_size);
  auto* attribute_list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attribute_list_storage.data());
  if (!InitializeProcThreadAttributeList(attribute_list, 1, 0, &attribute_list_size)) {
    const DWORD error = GetLastError();
    CloseHandle(job);
    return failure(WinLaunchStep::attributes, error);
  }
  if (!UpdateProcThreadAttribute(attribute_list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited.data(), inherited.size() * sizeof(HANDLE),
                                 nullptr, nullptr)) {
    const DWORD error = GetLastError();
    DeleteProcThreadAttributeList(attribute_list);
    CloseHandle(job);
    return failure(WinLaunchStep::attributes, error);
  }
  STARTUPINFOEXW startup{};
  startup.StartupInfo.cb = sizeof(startup);
  startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
  startup.StartupInfo.hStdInput = spec.stdin_read;
  startup.StartupInfo.hStdOutput = spec.stdout_write;
  startup.StartupInfo.hStdError = spec.stderr_handle;
  startup.lpAttributeList = attribute_list;
  // lpReserved2 is non-const in the API: hand it a private copy.
  std::vector<BYTE> crt_block(spec.crt_fd_block.begin(), spec.crt_fd_block.end());
  if (!crt_block.empty()) {
    startup.StartupInfo.cbReserved2 = static_cast<WORD>(crt_block.size());
    startup.StartupInfo.lpReserved2 = crt_block.data();
  }
  PROCESS_INFORMATION info{};
  // Spec §3.7: a crashing child must never wait on a Windows Error Reporting
  // dialog; the error mode is inherited by the child. Set once per process.
  static const UINT previous_error_mode = SetErrorMode(GetErrorMode() | SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS);
  (void)previous_error_mode;
  // Start suspended so the child is inside the Job before it runs a single
  // instruction: no early descendant can escape the kill-on-close Job (PROC-05).
  // CREATE_NEW_PROCESS_GROUP: Ctrl-C is disabled in the child's group and
  // group-targeted Ctrl-Break reaches only the frontend. CREATE_NO_WINDOW
  // (SP2 Task 15 ruling): the child runs without the frontend's console, so a
  // console-wide Ctrl-Break or a closed console window reaches only the
  // frontend, whose console handler (shutdown_signal.cpp) runs the orderly
  // shutdown (spec §3.7). Its stdio is redirected, so it needs no console.
  // Authoritative in MSYS2.
  std::wstring command_line = spec.command_line;  // CreateProcess*W may write to it
  std::wstring environment = spec.environment_block;
  if (environment.empty()) environment.push_back(L'\0');  // an empty block is still double-NUL terminated
  constexpr DWORD kCreationFlags =
      CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED | CREATE_NEW_PROCESS_GROUP | CREATE_NO_WINDOW;
  const wchar_t* cwd = spec.cwd.empty() ? nullptr : spec.cwd.c_str();
  const BOOL created = spec.primary_token != nullptr
                           ? CreateProcessAsUserW(spec.primary_token, spec.executable.c_str(), command_line.data(), nullptr, nullptr, TRUE,
                                                  kCreationFlags, environment.data(), cwd, &startup.StartupInfo, &info)
                           : CreateProcessW(spec.executable.c_str(), command_line.data(), nullptr, nullptr, TRUE, kCreationFlags,
                                            environment.data(), cwd, &startup.StartupInfo, &info);
  const DWORD create_error = created ? ERROR_SUCCESS : GetLastError();  // before any other call can overwrite it
  DeleteProcThreadAttributeList(attribute_list);
  if (!created) {
    CloseHandle(job);
    return failure(WinLaunchStep::create_process, create_error);  // 740 included: never retried via runas
  }
  if (!AssignProcessToJobObject(job, info.hProcess)) {
    const DWORD error = GetLastError();
    TerminateProcess(info.hProcess, 1);  // still suspended: it never ran
    CloseHandle(info.hThread);
    CloseHandle(info.hProcess);
    CloseHandle(job);
    return failure(WinLaunchStep::assign_job, error);
  }
  if (ResumeThread(info.hThread) == static_cast<DWORD>(-1)) {
    const DWORD error = GetLastError();
    TerminateJobObject(job, kSupervisorKillExitCode);
    CloseHandle(info.hThread);
    CloseHandle(info.hProcess);
    CloseHandle(job);
    return failure(WinLaunchStep::resume, error);
  }
  return WinLaunched{info.hProcess, info.hThread, job};
}

}  // namespace secure_frontend::win_launch

#endif  // defined(_WIN32)
