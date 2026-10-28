#pragma once

namespace secure_frontend {

// Process-wide defaults every frontend executable (and every test binary
// that opens sockets) installs first thing in main(): a peer that closes a
// socket early must produce an error return from send(), never a SIGPIPE
// that terminates the service. It also makes the process non-dumpable with a
// zero core-size limit (Linux), or suppresses the Windows error-reporting
// dialogs, so a crash cannot write a token to disk. Full Windows Error
// Reporting policy is SP7. Idempotent.
void install_process_defaults() noexcept;

}  // namespace secure_frontend
