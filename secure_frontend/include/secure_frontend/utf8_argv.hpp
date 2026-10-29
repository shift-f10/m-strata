#pragma once

#include <optional>
#include <string>
#include <vector>

namespace secure_frontend {

// The process's command line as UTF-8, argv[0] included (spec A2: every value
// arrives literally; the config and CLI layers decode values with
// path_from_utf8). POSIX: argv as given. Windows: the CRT's char** argv is in
// the ANSI code page, which turns any character outside it into '?', so the
// wide command line (CommandLineToArgvW) is converted instead; argc/argv are
// ignored there. nullopt: the Windows command line is not valid UTF-16.
// Shared by strata-unified-server and strata.
[[nodiscard]] std::optional<std::vector<std::string>> utf8_command_line(int argc, char** argv);

}  // namespace secure_frontend
