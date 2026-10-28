#include "support/binary_imports.hpp"
#include "support/egress_support.hpp"
#include "support/server_harness.hpp"
#include "support/test_paths.hpp"
#include "support/test_support.hpp"
#include "secure_frontend/socket_io.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include "secure_frontend/win32_prelude.hpp"
#endif

namespace {
using namespace std::chrono_literals;
int failures = 0;
void expect(bool condition, const std::string& message) {
  if (!condition) { ++failures; std::cerr << "FAILED: " << message << '\n'; }
}
bool has(const std::vector<std::string>& values, std::string_view wanted) {
  return std::any_of(values.begin(), values.end(), [wanted](const std::string& v) {
    return v.size() == wanted.size() && std::equal(v.begin(), v.end(), wanted.begin(), [](char a, char b) {
             return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
           });
  });
}

#if defined(_WIN32)
// The export names of the ws2_32.dll the loader binds (%SystemRoot%\System32), for
// the ordinal-only imports MSVC's ws2_32.lib links (session log 2026-10-21).
test_support::OrdinalNames system_ws2_32_ordinal_names() {
  wchar_t directory[MAX_PATH];
  const UINT length = GetSystemDirectoryW(directory, MAX_PATH);
  if (length == 0U || length >= MAX_PATH) return {};
  std::ifstream in(std::filesystem::path{std::wstring{directory, length}} / L"ws2_32.dll", std::ios::binary);
  const std::vector<unsigned char> bytes{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
  const auto exports = test_support::read_pe_exports(bytes);
  test_support::OrdinalNames names;
  for (const auto& [ordinal, name] : exports.names) names["ws2_32.dll"][ordinal] = name;
  return names;
}
#endif

void egress_server_imports_no_resolver_or_http_client() {
  const auto report = test_support::read_binary_imports(test_support::server_path());
  expect(report.ok, "the server binary has an import table: " + report.error);
  test_support::OrdinalNames ordinal_names;
  // Positive controls: the reader really sees the socket imports the listener needs.
#if defined(_WIN32)
  expect(has(report.libraries, "ws2_32.dll"), "ws2_32.dll is imported");
  ordinal_names = system_ws2_32_ordinal_names();
  expect(!ordinal_names.empty(), "the system ws2_32.dll export table was read");
  const auto names = test_support::import_names(report, ordinal_names);
  expect(has(names, "accept") && has(names, "listen"), "accept and listen are seen, by name or by resolved ordinal");
#else
  expect(has(report.symbols, "accept") || has(report.symbols, "accept4"), "accept is an undefined dynamic symbol");
#endif
  const auto forbidden = test_support::forbidden_imports(report, ordinal_names);
  std::string list;
  for (const auto& f : forbidden) list += f + " ";
#if defined(SECURE_FRONTEND_EMAIL_ALERTS)
  // The owner's choice (2026-10-27): email alerts are built into the server, which opens the SMTP connection
  // itself. A build with them may resolve the mail host - getaddrinfo and nothing else; no HTTP client, no
  // other resolver. A build without them (the default) keeps the rule below.
  expect(forbidden == std::vector<std::string>{"getaddrinfo"}, "only the SMTP host lookup (getaddrinfo) is imported: " + list);
#else
  expect(forbidden.empty(), "no resolver, HTTP client or outbound-connect import: " + list);
#endif
}

// Web search (spec 2026-10-27-web-search-design §2-§3): strata-search, not the server, is the search connection - it
// resolves the configured instance (getaddrinfo) and speaks HTTP itself, with no HTTP client library and no other
// resolver. The server binary's own rule (the case above) is the same in a build with search.
void egress_strata_search_is_the_search_client() {
  const auto report = test_support::read_binary_imports(secure_frontend_test::extra_arguments().at(0));
  expect(report.ok, "strata-search has an import table: " + report.error);
  test_support::OrdinalNames ordinal_names;
#if defined(_WIN32)
  ordinal_names = system_ws2_32_ordinal_names();
#endif
  const auto forbidden = test_support::forbidden_imports(report, ordinal_names);
  std::string list;
  bool resolver = false;
  bool other = false;
  for (const auto& f : forbidden) {
    list += f + " ";
    if (f.find("getaddrinfo") != std::string::npos) resolver = true;
    else other = true;
  }
  expect(resolver && !other, "the instance's host lookup (getaddrinfo) and nothing else: " + list);
}

// Little-endian writers for the synthetic images (ruling R-I: they go through the real parsers).
void put(std::vector<unsigned char>& b, std::size_t at, std::uint64_t value, std::size_t width) {
  if (b.size() < at + width) b.resize(at + width, 0);
  for (std::size_t i = 0; i < width; ++i) b[at + i] = static_cast<unsigned char>(value >> (8U * i));
}
void put_text(std::vector<unsigned char>& b, std::size_t at, std::string_view text) {
  if (b.size() < at + text.size() + 1U) b.resize(at + text.size() + 1U, 0);
  std::copy(text.begin(), text.end(), b.begin() + static_cast<std::ptrdiff_t>(at));
}
// ELF64 x86-64: .dynstr at 64, optional .dynsym at 96 (null + getaddrinfo + accept, all undefined),
// .dynamic at 168 (DT_NEEDED libc.so.6), section table at 200.
std::vector<unsigned char> synthetic_elf(const bool with_dynsym) {
  std::vector<unsigned char> b(64, 0);
  b[0] = 0x7f; b[1] = 'E'; b[2] = 'L'; b[3] = 'F'; b[4] = 2; b[5] = 1; b[6] = 1;
  put(b, 0x10, 2, 2);      // ET_EXEC
  put(b, 0x12, 0x3e, 2);   // EM_X86_64
  put_text(b, 65, "getaddrinfo");   // dynstr: "\0getaddrinfo\0accept\0libc.so.6\0" (offsets 1, 13, 20)
  put_text(b, 77, "accept");
  put_text(b, 84, "libc.so.6");
  put(b, 96 + 24 * 1, 1, 4);   // sym 1: st_name = 1, st_shndx = 0 (undefined)
  put(b, 96 + 24 * 2, 13, 4);  // sym 2: st_name = 13
  put(b, 168, 1, 8);           // DT_NEEDED
  put(b, 176, 20, 8);          //   -> "libc.so.6"
  put(b, 199, 0, 1);           // zero DT_NULL terminator up to the section table
  const std::size_t table = 200;
  std::size_t index = 1;       // section 0 is the null section
  const auto section = [&](std::uint32_t type, std::uint64_t offset, std::uint64_t size, std::uint32_t link, std::uint64_t entsize) {
    const std::size_t base = table + index * 64U;
    put(b, base + 4, type, 4);
    put(b, base + 0x18, offset, 8);
    put(b, base + 0x20, size, 8);
    put(b, base + 0x28, link, 4);
    put(b, base + 0x38, entsize, 8);
    ++index;
  };
  section(3, 64, 30, 0, 0);                    // 1: .dynstr (SHT_STRTAB)
  if (with_dynsym) section(11, 96, 72, 1, 24); // 2: .dynsym (SHT_DYNSYM) -> strtab 1
  section(6, 168, 32, 1, 16);                  // .dynamic (SHT_DYNAMIC) -> strtab 1
  put(b, table + index * 64U - 1U, 0, 1);      // size the buffer to the whole table
  put(b, 0x28, table, 8);                      // e_shoff
  put(b, 0x3a, 64, 2);                         // e_shentsize
  put(b, 0x3c, index, 2);                      // e_shnum
  return b;
}
// PE32+: one section (VA 0x1000, raw 0x200); import directory at RVA 0x1000 naming WS2_32.dll
// with one ordinal-only thunk (52 = gethostbyname).
std::vector<unsigned char> synthetic_pe() {
  std::vector<unsigned char> b(0x400, 0);
  b[0] = 'M'; b[1] = 'Z';
  put(b, 0x3c, 0x40, 4);              // e_lfanew
  put_text(b, 0x40, "PE");            // "PE\0\0"
  put(b, 0x44, 0x8664, 2);            // machine x64
  put(b, 0x46, 1, 2);                 // one section
  put(b, 0x54, 240, 2);               // SizeOfOptionalHeader
  put(b, 0x58, 0x20b, 2);             // PE32+ magic (optional header at 0x58)
  put(b, 0x58 + 120, 0x1000, 4);      // import directory RVA
  put(b, 0x58 + 124, 40, 4);          // import directory size
  const std::size_t section = 0x58 + 240;
  put(b, section + 8, 0x200, 4);      // VirtualSize
  put(b, section + 12, 0x1000, 4);    // VirtualAddress
  put(b, section + 16, 0x200, 4);     // SizeOfRawData
  put(b, section + 20, 0x200, 4);     // PointerToRawData
  put(b, 0x200 + 0, 0x1040, 4);       // descriptor 0: OriginalFirstThunk
  put(b, 0x200 + 12, 0x1060, 4);      //               Name
  put(b, 0x200 + 16, 0x1040, 4);      //               FirstThunk (descriptor 1 stays zero)
  put(b, 0x240, (1ULL << 63U) | 52U, 8);   // ordinal-only thunk, then a zero thunk
  put_text(b, 0x260, "WS2_32.dll");
  return b;
}
// The same layout as a DLL: export directory at RVA 0x1000 (data directory 0), Base 51,
// two functions named gethostbyaddr (ordinal 51) and gethostbyname (ordinal 52).
std::vector<unsigned char> synthetic_pe_exports() {
  std::vector<unsigned char> b = synthetic_pe();
  std::fill(b.begin() + 0x200, b.end(), static_cast<unsigned char>(0));
  put(b, 0x58 + 120, 0, 8);           // no import directory
  put(b, 0x58 + 112, 0x1000, 4);      // export directory RVA
  put(b, 0x58 + 116, 0x90, 4);        // export directory size
  put(b, 0x200 + 16, 51, 4);          // Base
  put(b, 0x200 + 20, 2, 4);           // NumberOfFunctions
  put(b, 0x200 + 24, 2, 4);           // NumberOfNames
  put(b, 0x200 + 28, 0x1040, 4);      // AddressOfFunctions
  put(b, 0x200 + 32, 0x1050, 4);      // AddressOfNames
  put(b, 0x200 + 36, 0x1060, 4);      // AddressOfNameOrdinals
  put(b, 0x250, 0x1070, 4);           // name 0 -> "gethostbyaddr"
  put(b, 0x254, 0x1080, 4);           // name 1 -> "gethostbyname"
  put(b, 0x260, 0, 2);                // name 0 is function index 0 (ordinal 51)
  put(b, 0x262, 1, 2);                // name 1 is function index 1 (ordinal 52)
  put_text(b, 0x270, "gethostbyaddr");
  put_text(b, 0x280, "gethostbyname");
  return b;
}

void egress_static_tripwire_fails_on_binary_without_dynamic_section() {
  const auto stripped = test_support::read_elf_imports(synthetic_elf(false));
  expect(!stripped.ok && stripped.error.find("no dynamic section") != std::string::npos,
         "SHT_DYNAMIC without SHT_DYNSYM fails the tripwire, never passes vacuously: " + stripped.error);
  const auto resolver = test_support::read_elf_imports(synthetic_elf(true));
  expect(resolver.ok && has(resolver.symbols, "getaddrinfo") && has(resolver.symbols, "accept") && has(resolver.libraries, "libc.so.6"),
         "the parser reads .dynsym and DT_NEEDED: " + resolver.error);
  const auto flagged = test_support::forbidden_imports(resolver);
  expect(flagged.size() == 1U && flagged.front() == "getaddrinfo", "an undefined getaddrinfo is flagged");
  const auto pe = test_support::read_pe_imports(synthetic_pe());
  expect(pe.ok && has(pe.libraries, "ws2_32.dll") && pe.ordinals.size() == 1U && pe.ordinals.front().second == 52U,
         "the PE parser walks the import directory to the ordinal thunk: " + pe.error);
  expect(!test_support::forbidden_imports(pe).empty(), "an ordinal-only ws2_32 import (gethostbyname #52) is flagged");
  // Ordinal resolution goes through the real export reader (ruling R-I), and only a
  // resolved name can clear an ordinal: an unresolved ws2_32 ordinal stays flagged.
  const auto exported = test_support::read_pe_exports(synthetic_pe_exports());
  expect(exported.ok && exported.names == std::vector<std::pair<std::uint16_t, std::string>>{{std::uint16_t{51}, "gethostbyaddr"}, {std::uint16_t{52}, "gethostbyname"}},
         "the export reader maps Base + name-ordinal index to each name: " + exported.error);
  test_support::OrdinalNames resolved;
  for (const auto& [ordinal, name] : exported.names) resolved["ws2_32.dll"][ordinal] = name;
  const auto named = test_support::forbidden_imports(pe, resolved);
  expect(named == std::vector<std::string>{"WS2_32.dll#52 (gethostbyname)"}, "a resolved #52 is flagged by its name");
  expect(test_support::forbidden_imports(pe, {{"ws2_32.dll", {{std::uint16_t{52}, "accept"}}}}).empty(), "a resolved, allowed ordinal passes");
  expect(test_support::forbidden_imports(pe, {{"ws2_32.dll", {{std::uint16_t{1}, "accept"}}}}) == std::vector<std::string>{"WS2_32.dll#52"},
         "an ordinal the table does not name stays flagged (fail closed)");
  const auto no_exports = test_support::read_pe_exports(synthetic_pe());
  expect(!no_exports.ok && no_exports.error.find("no export directory") != std::string::npos, "a PE without exports fails the reader");
  std::vector<unsigned char> no_imports = synthetic_pe();
  put(no_imports, 0x58 + 120, 0, 8);   // clear the import directory entry
  const auto empty = test_support::read_pe_imports(no_imports);
  expect(!empty.ok && empty.error.find("no import directory") != std::string::npos, "a PE without an import directory fails");
}

// SP3 Task 19 (spec §6.2 runtime tripwires, PRIV-01/03, DL-01).
void egress_interposer_self_test() {
#if defined(STRATA_EGRESS_PROBE_PATH) && defined(STRATA_EGRESS_TRIPWIRE_PATH)
  const auto dir = test_support::make_temp_dir("probe");
  const auto record = dir / "record.txt";
  const auto out = dir / "out.txt";
  const std::string command = std::string{"LD_PRELOAD='"} + STRATA_EGRESS_TRIPWIRE_PATH + "' STRATA_EGRESS_RECORD='" +
                              test_support::utf8_path(record) + "' '" + STRATA_EGRESS_PROBE_PATH + "' > '" + test_support::utf8_path(out) + "'";
  expect(std::system(command.c_str()) == 0, "the probe helper runs");
  const std::string recorded = test_support::file_text(record);
  for (const char* call : {"getaddrinfo", "connect", "socket_unix", "socket_dgram", "gethostbyname"}) {
    expect(recorded.find(call) != std::string::npos, std::string{"recorded: "} + call);
  }
  const std::string printed = test_support::file_text(out);
  expect(printed.find("getaddrinfo=denied") != std::string::npos && printed.find("connect=denied") != std::string::npos &&
         printed.find("unix=denied") != std::string::npos, "and denied: " + printed);
#else
  test_support::skip("the LD_PRELOAD interposer is Linux-only");
#endif
}

void egress_interposer_records_nothing_during_lifecycle() {
  const auto dir = test_support::make_temp_dir("ipose");
  const auto record = dir / "egress.txt";
  test_support::ServerOptions options;
  options.engine_args = {"--fail-first=1", "--counter-file=" + test_support::utf8_path(dir / "count"), "--tokens=10000", "--token-interval-ms=5"};
  if (!test_support::arm_egress_tripwire(options, record)) test_support::skip("the LD_PRELOAD interposer is Linux-only");
  auto server = test_support::ServerProcess::start(options);
  expect(server && server->wait_until_listening(10s) && server->wait_for_health(test_support::kHealthReady, 20s), "ready after one restart");
  if (!server) return;
  test_support::run_lifecycle(*server, std::string{test_support::kHarnessToken});
  server->stop(test_support::ServerProcess::Stop::term);
  expect(server->wait_exit(10s) == std::optional<int>{0}, "clean shutdown");
  expect(test_support::file_text(record).empty(), "no outbound call, DNS lookup, UDP or Unix socket: " + test_support::file_text(record));
}

void egress_table_scan_during_lifecycle() {
  const auto dir = test_support::make_temp_dir("scan");
  test_support::ServerOptions options;
  options.engine_args = {"--fail-first=1", "--counter-file=" + test_support::utf8_path(dir / "count"), "--tokens=10000", "--token-interval-ms=5"};
  auto server = test_support::ServerProcess::start(options);
  expect(server && server->wait_until_listening(10s), "listening");
  if (!server) return;
  test_support::SocketScan scan(server->pid(), server->port());
  expect(server->wait_for_health(test_support::kHealthReady, 20s), "ready after one restart");
  test_support::run_lifecycle(*server, std::string{test_support::kHarnessToken});
#if defined(_WIN32)
  server->stop(test_support::ServerProcess::Stop::ctrl_break);
#else
  server->stop(test_support::ServerProcess::Stop::term);
#endif
  (void)server->wait_exit(10s);
  std::string list;
  for (const auto& v : scan.stop()) list += v + "; ";
  expect(list.empty(), "only the listener and its accepted connections, no UDP, no Unix socket: " + list);
}

void egress_command_like_values_trigger_nothing() {
  const auto dir = test_support::make_temp_dir("cmd");
  const auto record = dir / "record.txt";
  const auto egress = dir / "egress.txt";
  test_support::ServerOptions options;
  options.engine_args = {"--record-file=" + test_support::utf8_path(record)};
  const bool armed = test_support::arm_egress_tripwire(options, egress);
  auto server = test_support::ServerProcess::start(options);
  expect(server && server->wait_until_listening(10s) && server->wait_for_health(test_support::kHealthReady, 15s), "ready");
  if (!server) return;
  const auto before = server->active_processes();
  const std::string a = "127.0.0.1:" + std::to_string(server->port());
  const std::string token{test_support::kHarnessToken};
  for (const std::string content : {"https://example.com/model.gguf", "mcp://x", "file:///etc/passwd", "data:text/plain,x", "HTTP://EXAMPLE.COM"}) {
    const auto r = raw_http::exchange("127.0.0.1", server->port(),
        raw_http::request("POST", "/v1/chat/completions", a, {raw_http::bearer(token), "Content-Type: application/json"},
                          R"({"model":"fake-model","messages":[{"role":"user","content":")" + content + R"("}]})"));
    // Session log step 15: SP3 §6.2 "rejected or treated as text" — text now; what this case
    // pins is that nothing is fetched or started for it (the checks below).
    expect(r && r->status == 200, "remote-looking content is served as text, never fetched: " + content);
  }
  const auto pull = raw_http::exchange("127.0.0.1", server->port(),
      raw_http::request("POST", "/v1/chat/completions", a, {raw_http::bearer(token), "Content-Type: application/json", "X-Strata-Update: now"},
                        R"({"model":"pull:qwen","messages":[{"role":"user","content":"Hi"}]})"));
  // Ruling T12-l (SP5 M1 Task 12): name:tag is the served managed-model id grammar,
  // so "pull:qwen" now parses and answers model_not_found — still a refusal, still
  // no engine contact, which is what this case pins.
  expect(pull && pull->status == 404, "a pull-like model value is refused as model_not_found");
  for (const std::string target : {"/api/pull", "/v1/pull", "/update", "/v1/models/pull:x"}) {
    const auto r = raw_http::exchange("127.0.0.1", server->port(), raw_http::request("GET", target, a, {raw_http::bearer(token)}));
    expect(r && r->status == 404, "no acquire/update route: " + target);
  }
  const std::string recorded = test_support::file_text(record);
  std::size_t gens = 0;
  for (std::size_t at = recorded.find("GEN "); at != std::string::npos; at = recorded.find("GEN ", at + 1U)) ++gens;
  expect(gens == 5U, "the engine received exactly the five text chats, nothing for the refused model or routes: " +
                         std::to_string(gens));
  expect(server->active_processes() == before, "no process started or ended");
  if (armed) expect(test_support::file_text(egress).empty(), "no outbound attempt (interposer)");
}
}  // namespace

int main(int argc, char** argv) {
  secure_frontend::WinsockGuard winsock;
  return secure_frontend_test::run_cases(argc, argv, {
      {"egress_server_imports_no_resolver_or_http_client", egress_server_imports_no_resolver_or_http_client},
      {"egress_strata_search_is_the_search_client", egress_strata_search_is_the_search_client},
      {"egress_static_tripwire_fails_on_binary_without_dynamic_section", egress_static_tripwire_fails_on_binary_without_dynamic_section},
      {"egress_interposer_self_test", egress_interposer_self_test},
      {"egress_interposer_records_nothing_during_lifecycle", egress_interposer_records_nothing_during_lifecycle},
      {"egress_table_scan_during_lifecycle", egress_table_scan_during_lifecycle},
      {"egress_command_like_values_trigger_nothing", egress_command_like_values_trigger_nothing},
  }, failures);
}
