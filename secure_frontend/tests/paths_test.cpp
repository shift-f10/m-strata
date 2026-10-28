#include "secure_frontend/canonical_path.hpp"
#include "secure_frontend/token_reader.hpp"
#include "support/test_support.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#if !defined(_WIN32)
#include <sched.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {
int failures = 0;
void expect(const bool condition, const std::string& message) {
  if (!condition) { ++failures; std::cerr << "FAILED: " << message << '\n'; }
}
using secure_frontend::PathErrorCode;

bool rejects(const std::string& text, const PathErrorCode code) {
  const auto result = secure_frontend::parse_relative_path(text);
  const auto* error = std::get_if<secure_frontend::PathError>(&result);
  return error != nullptr && error->code == code;
}

void relative_path_accepts_portable_forms() {
  for (const char* text : {"model.gguf", "qwen/model-00001-of-00004.gguf", "Jos\xC3\xA9/mod\xC3\xA8le.gguf", "x~y", "~",
                           "CONSOLE", "COM10", "LPT", "NULL.txt", "con-fig"}) {
    expect(std::holds_alternative<secure_frontend::RelativePath>(secure_frontend::parse_relative_path(text)),
           std::string{"portable path must be accepted: "} + text);
  }
  const auto normalized = secure_frontend::parse_relative_path("a\\b\\c.gguf");
  const auto* path = std::get_if<secure_frontend::RelativePath>(&normalized);
  expect(path != nullptr && path->generic() == "a/b/c.gguf" && path->components().size() == 3U,
         "a backslash is accepted on input and normalized to '/'");
  expect(rejects("\xFF", PathErrorCode::invalid_utf8), "invalid UTF-8");
  expect(rejects(std::string(1025, 'a'), PathErrorCode::too_long), "total length is at most 1024 bytes");
  expect(rejects(std::string(256, 'a'), PathErrorCode::component_too_long), "a component is at most 255 bytes");
}

void cfg02_rejects_traversal_and_absolute() {
  for (const char* text : {"..", "../x", "a/../b", "a/..", ".", "a/./b"}) {
    expect(rejects(text, PathErrorCode::traversal), std::string{"traversal: "} + text);
  }
  for (const char* text : {"/etc/passwd", "\\x", "\\\\server\\share\\x", "\\\\?\\C:\\x", "//x"}) {
    expect(rejects(text, PathErrorCode::absolute), std::string{"absolute or UNC: "} + text);
  }
  for (const char* text : {"", "a//b", "a/", "a\\\\b"}) {
    expect(rejects(text, PathErrorCode::empty), std::string{"empty component: "} + text);
  }
}

void cfg02_rejects_nul_and_control_bytes() {
  for (const std::string& text : {std::string("a\0b", 3), std::string("a\nb"), std::string("a\tb"), std::string("a\x7f"),
                                 std::string("\x01")}) {
    expect(rejects(text, PathErrorCode::control_character), "control byte must be rejected");
  }
}

void cfg04_rejects_reserved_and_device_names() {
  for (const char* text : {"CON", "con", "Con.txt", "nul.gguf", "AUX", "PRN.log", "COM0", "COM1", "com9.x", "LPT0",
                           "LPT9", "COM\xC2\xB9", "LPT\xC2\xB3.txt", "CONIN$", "conout$", "CLOCK$", "dir/NUL/file",
                           "CON .txt"}) {
    expect(rejects(text, PathErrorCode::reserved_name), std::string{"reserved device name: "} + text);
  }
}

void cfg04_rejects_trailing_dot_space_and_8dot3() {
  for (const char* text : {"model.", "model ", "dir./x", "dir /x"}) {
    expect(rejects(text, PathErrorCode::trailing_dot_or_space), std::string{"trailing dot or space: "} + text);
  }
  for (const char* text : {"PROGRA~1", "MODEL~2.GGU", "a/b~1/c"}) {
    expect(rejects(text, PathErrorCode::short_name_alias), std::string{"8.3 alias: "} + text);
  }
}

void cfg04_rejects_ads_and_drive_forms() {
  for (const char* text : {"C:x", "C:\\x", "model.gguf:stream", "model.gguf::$DATA", "a/b:c"}) {
    expect(rejects(text, PathErrorCode::drive_or_stream), std::string{"drive or stream: "} + text);
  }
  for (const char* text : {"a<b", "a>b", "a\"b", "a|b", "a?b", "a*b"}) {
    expect(rejects(text, PathErrorCode::invalid_character), std::string{"invalid character: "} + text);
  }
}

template <typename T>
bool fails_with(const std::variant<T, secure_frontend::PathError>& result, const PathErrorCode code) {
  const auto* error = std::get_if<secure_frontend::PathError>(&result);
  return error != nullptr && error->code == code;
}

void cfg02_rejects_relative_root() {
  for (const char* text : {"models", "./models", "../models"}) {
    expect(fails_with(secure_frontend::open_root(text), PathErrorCode::not_absolute), std::string{"relative root: "} + text);
  }
  expect(fails_with(secure_frontend::open_root(""), PathErrorCode::empty), "empty root");
  const auto missing = secure_frontend_test::make_scratch_directory() / "missing";
  expect(fails_with(secure_frontend::open_root(missing), PathErrorCode::root_unavailable), "missing root");
}

void cfg02_sibling_prefix_root_rejected() {
#if defined(_WIN32)
  expect(!secure_frontend::is_within(L"C:\\r", L"C:\\r2\\model.gguf"), "C:\\r does not contain C:\\r2");
  expect(secure_frontend::is_within(L"C:\\r", L"c:\\R\\model.gguf"), "comparison ignores case on a separator boundary");
  expect(secure_frontend::is_within(L"C:\\r\\", L"C:\\r\\x"), "a trailing separator on the root is fine");
#else
  expect(!secure_frontend::is_within("/srv/r", "/srv/r2/model.gguf"), "/srv/r does not contain /srv/r2");
  expect(secure_frontend::is_within("/srv/r", "/srv/r/model.gguf"), "/srv/r contains its child");
  expect(secure_frontend::is_within("/srv/r/", "/srv/r/x"), "a trailing separator on the root is fine");
  expect(!secure_frontend::is_within("/srv/r", "/srv/R/x"), "POSIX comparison is exact");
#endif
}

void cfg04_rejects_unc_device_and_extended_roots() {
  for (const wchar_t* text : {L"C:foo", L"\\foo", L"/foo", L"\\\\server\\share\\x", L"\\\\?\\C:\\x", L"\\\\.\\C:\\x",
                              L"\\??\\C:\\x", L"//server/share", L"C:\\models\\file.gguf:stream", L"C:\\CON\\x", L"models"}) {
    expect(secure_frontend::check_windows_root_form(text).has_value(), "unsafe Windows root form must be rejected");
  }
  for (const wchar_t* text : {L"C:\\Users\\Jos\u00e9\\models", L"D:/models", L"c:\\"}) {
    expect(!secure_frontend::check_windows_root_form(text).has_value(), "drive-absolute roots are accepted");
  }
}

// Review Focus 2: a root under OneDrive Known Folder Move or a roaming profile
// resolves to \\?\UNC\...; the error must say what happened and what to do.
void cfg04_final_path_on_network_share_is_actionable() {
  const auto share = secure_frontend::classify_final_path(L"\\\\?\\UNC\\server\\share\\models");
  const auto* error = std::get_if<secure_frontend::PathError>(&share);
  expect(error != nullptr && error->code == PathErrorCode::network_location, "a UNC final path is rejected");
  if (error != nullptr) {
    expect(error->message.find("network share") != std::string::npos &&
               error->message.find("local folder") != std::string::npos,
           "the message is actionable: " + error->message);
  }
  const auto local = secure_frontend::classify_final_path(L"\\\\?\\C:\\Users\\x\\models");
  const auto* text = std::get_if<std::wstring>(&local);
  expect(text != nullptr && *text == L"C:\\Users\\x\\models", "the \\\\?\\ prefix is stripped from a local final path");
}

// Review Focus 4: non-ASCII names and paths beyond MAX_PATH round-trip through the module.
void paths_handle_non_ascii_and_long_names() {
  const auto root_path = secure_frontend_test::make_scratch_directory() / secure_frontend::path_from_utf8("Jos\xC3\xA9-\xC3\xBC");
  secure_frontend_test::make_private_directory(root_path);
  std::string relative;
  auto current = root_path;
  for (int level = 0; level < 6; ++level) {
    const std::string name = std::string(48, static_cast<char>('a' + level)) + "-\xC3\xA9";
    current /= secure_frontend::path_from_utf8(name);
    secure_frontend_test::make_private_directory(current);
    relative += name + "/";
  }
  relative += "mod\xC3\xA8le.txt";
  secure_frontend_test::write_file(current / secure_frontend::path_from_utf8("mod\xC3\xA8le.txt"), "payload");
  expect(secure_frontend::path_to_utf8(current).size() > 300U, "the test path is longer than MAX_PATH");
  const auto root = secure_frontend::open_root(root_path);
  const auto* opened_root = std::get_if<secure_frontend::CanonicalRoot>(&root);
  expect(opened_root != nullptr, "a non-ASCII root opens");
  if (opened_root == nullptr) return;
  expect(secure_frontend::path_to_utf8(opened_root->path()).find("Jos\xC3\xA9-\xC3\xBC") != std::string::npos,
         "the canonical root keeps its UTF-8 name");
  const auto file = secure_frontend::open_beneath(*opened_root,
                                                  std::get<secure_frontend::RelativePath>(secure_frontend::parse_relative_path(relative)));
  const auto* opened = std::get_if<secure_frontend::OpenedFile>(&file);
  expect(opened != nullptr, "a > 260-character path below the root opens");
  if (opened != nullptr) {
    const auto text = opened->read_all(64);
    expect(std::holds_alternative<std::string>(text) && std::get<std::string>(text) == "payload", "and reads");
  }
}

#if defined(_WIN32)
// Defect (b): the old reader opened directories with FILE_READ_ATTRIBUTES only,
// so GetSecurityInfo failed and every protected token was "insecure".
void auth02_windows_token_file_reads_with_private_dacl() {
  const auto secret = secure_frontend_test::make_scratch_directory() / "secret";
  secure_frontend_test::make_protected_directory(secret);
  const std::string token = "windows-token-0123456789abcdefghijklmnopqrs";
  secure_frontend_test::write_protected_file(secret / "token", token);
  const auto source = std::get<secure_frontend::SecretSource>(secure_frontend::make_protected_file_secret_source("token"));
  const auto result = secure_frontend::read_token(source, secret);
  const auto* error = std::get_if<secure_frontend::TokenReadError>(&result);
  expect(error == nullptr, "a private-DACL token file must read: " + (error ? error->message : std::string{}));
}

void cfg03_rejects_junction_below_root() {
  const auto root = secure_frontend_test::make_scratch_directory();
  const auto outside = secure_frontend_test::make_scratch_directory();
  secure_frontend_test::write_file(outside / "f", "outside");
  expect(secure_frontend_test::create_junction(root / "j", outside), "junction created without privilege");
  expect(fails_with(secure_frontend::open_beneath(std::get<secure_frontend::CanonicalRoot>(secure_frontend::open_root(root)),
                                                  std::get<secure_frontend::RelativePath>(secure_frontend::parse_relative_path("j/f"))),
                    PathErrorCode::symlink_not_allowed),
         "a junction below the root is rejected");
}

void cfg03_rejects_symlink_below_root() {
  const auto root = secure_frontend_test::make_scratch_directory();
  const auto outside = secure_frontend_test::make_scratch_directory();
  secure_frontend_test::write_file(outside / "f", "outside");
  if (CreateSymbolicLinkW((root / "s").c_str(), (outside / "f").c_str(), 0x2 /* ALLOW_UNPRIVILEGED_CREATE */) == 0) {
    std::cout << "SKIPPED: SeCreateSymbolicLinkPrivilege / developer mode not available\n";
    return;
  }
  expect(fails_with(secure_frontend::open_beneath(std::get<secure_frontend::CanonicalRoot>(secure_frontend::open_root(root)),
                                                  std::get<secure_frontend::RelativePath>(secure_frontend::parse_relative_path("s"))),
                    PathErrorCode::symlink_not_allowed),
         "a file symlink below the root is rejected");
}
#endif

#if !defined(_WIN32)
void cfg02_rejects_shared_writable_root() {
  for (const mode_t mode : {0777, 0775, 0757}) {
    const auto root = secure_frontend_test::make_scratch_directory();
    chmod(root.c_str(), mode);
    expect(fails_with(secure_frontend::open_root(root), PathErrorCode::shared_writable), "group/other-writable root");
  }
  const auto ok = secure_frontend_test::make_scratch_directory();
  chmod(ok.c_str(), 0755);
  expect(std::holds_alternative<secure_frontend::CanonicalRoot>(secure_frontend::open_root(ok)), "0755 root is fine");
}

void cfg02_root_may_traverse_symlink() {
  const auto scratch = secure_frontend_test::make_scratch_directory();
  secure_frontend_test::make_private_directory(scratch / "real");
  std::filesystem::create_directory_symlink(scratch / "real", scratch / "link");
  const auto root = secure_frontend::open_root(scratch / "link");
  const auto* opened = std::get_if<secure_frontend::CanonicalRoot>(&root);
  expect(opened != nullptr, "a root given through a symlink is accepted");
  if (opened != nullptr) {
    expect(opened->path() == std::filesystem::canonical(scratch / "real"), "and resolved once to its canonical form");
  }
}

void cfg02_engine_executable_must_be_private_regular_file() {
  const auto dir = secure_frontend_test::make_scratch_directory();
  secure_frontend_test::write_file(dir / "engine", "x", 0700);
  expect(std::holds_alternative<secure_frontend::CanonicalFile>(secure_frontend::open_executable(dir / "engine")),
         "a 0700 file in a 0700 directory is accepted");
  secure_frontend_test::write_file(dir / "open", "x", 0777);
  expect(fails_with(secure_frontend::open_executable(dir / "open"), PathErrorCode::shared_writable), "0777 executable");
  expect(fails_with(secure_frontend::open_executable(dir), PathErrorCode::not_regular_file), "a directory");
  expect(fails_with(secure_frontend::open_executable("engine"), PathErrorCode::not_absolute), "relative");
  const auto shared = secure_frontend_test::make_scratch_directory();
  secure_frontend_test::write_file(shared / "engine", "x", 0700);
  chmod(shared.c_str(), 0777);
  expect(fails_with(secure_frontend::open_executable(shared / "engine"), PathErrorCode::shared_writable),
         "an executable in a shared-writable directory");
}

secure_frontend::CanonicalRoot root_at(const std::filesystem::path& path) {
  return std::get<secure_frontend::CanonicalRoot>(secure_frontend::open_root(path));
}
secure_frontend::RelativePath rel(const std::string& text) {
  return std::get<secure_frontend::RelativePath>(secure_frontend::parse_relative_path(text));
}
constexpr secure_frontend::WalkMode kModes[] = {secure_frontend::WalkMode::automatic, secure_frontend::WalkMode::fallback_only};

void cfg03_rejects_symlinked_intermediate_directory() {
  const auto root = secure_frontend_test::make_scratch_directory();
  const auto outside = secure_frontend_test::make_scratch_directory();
  secure_frontend_test::make_private_directory(outside / "sub");
  secure_frontend_test::write_file(outside / "sub" / "f", "outside");
  std::filesystem::create_directory_symlink(outside / "sub", root / "sub");
  for (const auto mode : kModes) {
    expect(fails_with(secure_frontend::open_beneath(root_at(root), rel("sub/f"), {}, mode), PathErrorCode::symlink_not_allowed),
           "a symlinked intermediate directory is rejected in both walk modes");
  }
}

void cfg03_rejects_symlinked_final_file() {
  const auto root = secure_frontend_test::make_scratch_directory();
  const auto outside = secure_frontend_test::make_scratch_directory();
  secure_frontend_test::write_file(outside / "f", "outside");
  secure_frontend_test::write_file(root / "real", "inside");
  std::filesystem::create_symlink(outside / "f", root / "escape");
  std::filesystem::create_symlink(root / "real", root / "inside-link");
  for (const auto mode : kModes) {
    expect(fails_with(secure_frontend::open_beneath(root_at(root), rel("escape"), {}, mode), PathErrorCode::symlink_not_allowed),
           "a symlink to a file outside the root is rejected");
    expect(fails_with(secure_frontend::open_beneath(root_at(root), rel("inside-link"), {}, mode), PathErrorCode::symlink_not_allowed),
           "a symlink is rejected even when its target is inside the root");
  }
}

void cfg03_fallback_walk_matches_openat2() {
  const auto root = secure_frontend_test::make_scratch_directory();
  secure_frontend_test::make_private_directory(root / "dir");
  secure_frontend_test::write_file(root / "dir" / "file", "payload");
  std::filesystem::create_symlink(root / "dir" / "file", root / "link");
  mkfifo((root / "fifo").c_str(), 0600);
  struct Row { const char* path; std::optional<PathErrorCode> code; };
  const Row rows[] = {{"dir/file", std::nullopt}, {"dir", PathErrorCode::not_regular_file},
                      {"missing", PathErrorCode::not_found}, {"link", PathErrorCode::symlink_not_allowed},
                      {"fifo", PathErrorCode::not_regular_file}, {"dir/file/x", PathErrorCode::not_directory}};
  for (const auto& row : rows) {
    for (const auto mode : kModes) {
      const auto result = secure_frontend::open_beneath(root_at(root), rel(row.path), {}, mode);
      if (!row.code.has_value()) {
        const auto* opened = std::get_if<secure_frontend::OpenedFile>(&result);
        expect(opened != nullptr, std::string{"opens: "} + row.path);
        if (opened != nullptr) {
          const auto text = opened->read_all(64);
          expect(std::holds_alternative<std::string>(text) && std::get<std::string>(text) == "payload", "reads the file");
        }
      } else {
        expect(fails_with(result, *row.code), std::string{"same outcome in both modes for "} + row.path);
      }
    }
  }
}

// Mounts a tmpfs below the root inside a private user+mount namespace; skips
// (exit 77 -> "SKIPPED:") where unprivileged namespaces are not allowed.
void cfg03_rejects_mount_crossing() {
  const auto root = secure_frontend_test::make_scratch_directory();
  secure_frontend_test::make_private_directory(root / "mnt");
  const uid_t uid = geteuid();
  const gid_t gid = getegid();
  const pid_t child = fork();
  if (child == 0) {
    if (unshare(CLONE_NEWUSER | CLONE_NEWNS) != 0) _exit(77);
    std::ofstream("/proc/self/setgroups") << "deny";
    std::ofstream("/proc/self/uid_map") << uid << ' ' << uid << " 1";
    std::ofstream("/proc/self/gid_map") << gid << ' ' << gid << " 1";
    if (mount("none", (root / "mnt").c_str(), "tmpfs", 0, "mode=0700") != 0) _exit(77);
    std::ofstream(root / "mnt" / "f") << "x";
    for (const auto mode : kModes) {
      if (!fails_with(secure_frontend::open_beneath(root_at(root), rel("mnt/f"), {}, mode), PathErrorCode::cross_device)) _exit(1);
    }
    _exit(0);
  }
  int status = 0;
  waitpid(child, &status, 0);
  if (WIFEXITED(status) && WEXITSTATUS(status) == 77) {
    std::cout << "SKIPPED: no unprivileged mount namespace on this host\n";
    return;
  }
  expect(WIFEXITED(status) && WEXITSTATUS(status) == 0, "a mount below the root is a cross-device walk in both modes");
}
#endif

std::vector<std::string> listing(const std::filesystem::path& base) {
  std::vector<std::string> names;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(base)) {
    names.push_back(secure_frontend::path_to_utf8(entry.path().lexically_relative(base)));
  }
  std::sort(names.begin(), names.end());
  return names;
}

void cfg02_create_within_root_writes_nothing_outside() {
  const auto scratch = secure_frontend_test::make_scratch_directory();
  secure_frontend_test::make_private_directory(scratch / "root");
  secure_frontend_test::make_private_directory(scratch / "outside");
  secure_frontend_test::make_private_directory(scratch / "root2");
  const auto root = std::get<secure_frontend::CanonicalRoot>(secure_frontend::open_root(scratch / "root"));
  const auto rel = [](const char* text) {
    return std::get<secure_frontend::RelativePath>(secure_frontend::parse_relative_path(text));
  };
#if !defined(_WIN32)
  std::filesystem::create_directory_symlink(scratch / "outside", scratch / "root" / "l");
  secure_frontend_test::write_file(scratch / "root" / "f", "file");
#else
  secure_frontend_test::create_junction(scratch / "root" / "l", scratch / "outside");
  secure_frontend_test::write_file(scratch / "root" / "f", "file");
#endif
  const auto before = listing(scratch);
  for (const char* escape : {"../outside/x", "/abs", "a/../../x", "..\\root2\\x"}) {
    expect(std::holds_alternative<secure_frontend::PathError>(secure_frontend::parse_relative_path(escape)),
           std::string{"escapes never become a RelativePath: "} + escape);
  }
  const auto through_link = secure_frontend::create_directory_within(root, rel("l/x"));
  expect(through_link.has_value() && through_link->code == secure_frontend::PathErrorCode::symlink_not_allowed,
         "creating through a link is refused");
  const auto through_file = secure_frontend::create_directory_within(root, rel("f/x"));
  expect(through_file.has_value() && through_file->code == secure_frontend::PathErrorCode::not_directory,
         "creating below a file is refused");
  expect(listing(scratch) == before, "the failed attempts wrote nothing anywhere");
  expect(!secure_frontend::create_directory_within(root, rel("a/b")).has_value(), "a/b is created below the root");
  auto after = listing(scratch);
  std::vector<std::string> expected = before;
  expected.push_back(secure_frontend::path_to_utf8(std::filesystem::path{"root"} / "a"));
  expected.push_back(secure_frontend::path_to_utf8(std::filesystem::path{"root"} / "a" / "b"));
  std::sort(expected.begin(), expected.end());
  expect(after == expected, "exactly root/a and root/a/b were added");
#if !defined(_WIN32)
  struct stat st {};
  stat((scratch / "root" / "a" / "b").c_str(), &st);
  expect((st.st_mode & 0777) == 0700, "created directories are 0700");
#endif
}

// SP1 delta f (SP4 §5.3): bundle directories on removable media.
void external_root_skips_ownership_checks() {
  const auto scratch = secure_frontend_test::make_scratch_directory();
  const auto shared = scratch / "shared";
  secure_frontend_test::make_private_directory(shared);
#if !defined(_WIN32)
  ::chmod(shared.c_str(), 0777);
#endif
  auto opened = secure_frontend::open_root(shared, secure_frontend::RootPolicy::external);
  expect(std::holds_alternative<secure_frontend::CanonicalRoot>(opened) && std::get<secure_frontend::CanonicalRoot>(opened).external(),
         "external roots accept shared-writable directories and say so");
  if (!std::holds_alternative<secure_frontend::CanonicalRoot>(opened)) return;
  auto ordinary = secure_frontend::open_root(scratch);
  expect(std::holds_alternative<secure_frontend::CanonicalRoot>(ordinary) &&
             !std::get<secure_frontend::CanonicalRoot>(ordinary).external(),
         "an ordinary root is not external");
#if !defined(_WIN32)
  expect(std::holds_alternative<secure_frontend::PathError>(secure_frontend::open_root(shared)), "existing roots still refuse them");
  ::chmod(shared.c_str(), 0700);
  const auto inner = shared / "inner";
  secure_frontend_test::make_private_directory(inner);
  ::chmod(inner.c_str(), 0777);
  ::chmod(shared.c_str(), 0777);
  const auto& root = std::get<secure_frontend::CanonicalRoot>(opened);
  expect(!secure_frontend::create_directory_within(root, std::get<secure_frontend::RelativePath>(secure_frontend::parse_relative_path("inner/deeper"))),
         "directories below an external root may be shared-writable (FAT dmask=000)");
  std::filesystem::create_directory_symlink(scratch, shared / "link");
  const auto through_link =
      secure_frontend::create_directory_within(root, std::get<secure_frontend::RelativePath>(secure_frontend::parse_relative_path("link/x")));
  expect(through_link.has_value() && through_link->code == PathErrorCode::symlink_not_allowed,
         "links below an external root are still refused");
  auto private_root = secure_frontend::open_root(scratch);
  expect(secure_frontend::create_directory_within(std::get<secure_frontend::CanonicalRoot>(private_root),
             std::get<secure_frontend::RelativePath>(secure_frontend::parse_relative_path("shared/inner/x"))).has_value(),
         "below an ordinary root a shared-writable directory on the way is still refused");
#endif
}
}  // namespace

int main(int argc, char** argv) {
  return secure_frontend_test::run_cases(argc, argv, {
      {"relative_path_accepts_portable_forms", relative_path_accepts_portable_forms},
      {"cfg02_rejects_traversal_and_absolute", cfg02_rejects_traversal_and_absolute},
      {"cfg02_rejects_nul_and_control_bytes", cfg02_rejects_nul_and_control_bytes},
      {"cfg04_rejects_reserved_and_device_names", cfg04_rejects_reserved_and_device_names},
      {"cfg04_rejects_trailing_dot_space_and_8dot3", cfg04_rejects_trailing_dot_space_and_8dot3},
      {"cfg04_rejects_ads_and_drive_forms", cfg04_rejects_ads_and_drive_forms},
      {"cfg02_rejects_relative_root", cfg02_rejects_relative_root},
      {"cfg02_sibling_prefix_root_rejected", cfg02_sibling_prefix_root_rejected},
      {"cfg04_rejects_unc_device_and_extended_roots", cfg04_rejects_unc_device_and_extended_roots},
      {"cfg04_final_path_on_network_share_is_actionable", cfg04_final_path_on_network_share_is_actionable},
      {"paths_handle_non_ascii_and_long_names", paths_handle_non_ascii_and_long_names},
      {"cfg02_create_within_root_writes_nothing_outside", cfg02_create_within_root_writes_nothing_outside},
      {"external_root_skips_ownership_checks", external_root_skips_ownership_checks},
#if defined(_WIN32)
      {"auth02_windows_token_file_reads_with_private_dacl", auth02_windows_token_file_reads_with_private_dacl},
      {"cfg03_rejects_junction_below_root", cfg03_rejects_junction_below_root},
      {"cfg03_rejects_symlink_below_root", cfg03_rejects_symlink_below_root},
#endif
#if !defined(_WIN32)
      {"cfg02_rejects_shared_writable_root", cfg02_rejects_shared_writable_root},
      {"cfg02_root_may_traverse_symlink", cfg02_root_may_traverse_symlink},
      {"cfg02_engine_executable_must_be_private_regular_file", cfg02_engine_executable_must_be_private_regular_file},
      {"cfg03_rejects_symlinked_intermediate_directory", cfg03_rejects_symlinked_intermediate_directory},
      {"cfg03_rejects_symlinked_final_file", cfg03_rejects_symlinked_final_file},
      {"cfg03_fallback_walk_matches_openat2", cfg03_fallback_walk_matches_openat2},
      {"cfg03_rejects_mount_crossing", cfg03_rejects_mount_crossing},
#endif
  }, failures);
}
