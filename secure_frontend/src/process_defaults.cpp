#include "secure_frontend/process_defaults.hpp"

#if defined(_WIN32)
#include "secure_frontend/win32_prelude.hpp"
#else
#include <csignal>
#include <sys/resource.h>
#if defined(__linux__)
#include <sys/prctl.h>
#endif
#endif

namespace secure_frontend {

void install_process_defaults() noexcept {
#if defined(_WIN32)
  (void)SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
#else
  struct sigaction action {};
  action.sa_handler = SIG_IGN;
  sigemptyset(&action.sa_mask);
  (void)sigaction(SIGPIPE, &action, nullptr);
  const rlimit no_core{0, 0};
  (void)setrlimit(RLIMIT_CORE, &no_core);
#if defined(__linux__)
  (void)prctl(PR_SET_DUMPABLE, 0, 0, 0, 0);
#endif
#endif
}

}  // namespace secure_frontend
