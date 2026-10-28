#pragma once
// Test-only process socket/thread/handle inspection (spec §6.2 table scan,
// DOS-02 idle counts, NET-01). Linux: /proc/<pid>/fd + /proc/<pid>/net/*;
// Windows: GetExtendedTcpTable/GetExtendedUdpTable filtered by pid, Toolhelp
// threads, GetProcessHandleCount, and handles by object type (handle_types).
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#if defined(_WIN32)
#include "secure_frontend/win32_prelude.hpp"
#include <iphlpapi.h>
#include <tlhelp32.h>
#else
#include <netinet/in.h>
#include <unistd.h>
#endif

namespace test_support {

struct SocketRow final {
  std::string kind;           // tcp, tcp6, udp, udp6, unix
  bool listening{false};
  std::uint16_t local_port{0};
  std::string local_address;  // kernel hex (Linux) or dotted/hex (Windows); compared only for loopback checks
};

#if defined(_WIN32)
inline std::vector<SocketRow> sockets_of(const long pid) {
  std::vector<SocketRow> rows;
  for (const ULONG family : {static_cast<ULONG>(AF_INET), static_cast<ULONG>(AF_INET6)}) {
    DWORD size = 0;
    GetExtendedTcpTable(nullptr, &size, FALSE, family, TCP_TABLE_OWNER_PID_ALL, 0);
    std::vector<unsigned char> buffer(size);
    if (GetExtendedTcpTable(buffer.data(), &size, FALSE, family, TCP_TABLE_OWNER_PID_ALL, 0) == NO_ERROR) {
      if (family == AF_INET) {
        const auto* table = reinterpret_cast<const MIB_TCPTABLE_OWNER_PID*>(buffer.data());
        for (DWORD i = 0; i < table->dwNumEntries; ++i) {
          const auto& r = table->table[i];
          if (static_cast<long>(r.dwOwningPid) != pid) continue;
          rows.push_back({"tcp", r.dwState == MIB_TCP_STATE_LISTEN, ntohs(static_cast<u_short>(r.dwLocalPort)), std::to_string(r.dwLocalAddr)});
        }
      } else {
        const auto* table = reinterpret_cast<const MIB_TCP6TABLE_OWNER_PID*>(buffer.data());
        for (DWORD i = 0; i < table->dwNumEntries; ++i) {
          const auto& r = table->table[i];
          if (static_cast<long>(r.dwOwningPid) != pid) continue;
          std::string addr(reinterpret_cast<const char*>(r.ucLocalAddr), 16U);
          rows.push_back({"tcp6", r.dwState == MIB_TCP_STATE_LISTEN, ntohs(static_cast<u_short>(r.dwLocalPort)), addr});
        }
      }
    }
    size = 0;
    GetExtendedUdpTable(nullptr, &size, FALSE, family, UDP_TABLE_OWNER_PID, 0);
    std::vector<unsigned char> udp(size);
    if (GetExtendedUdpTable(udp.data(), &size, FALSE, family, UDP_TABLE_OWNER_PID, 0) == NO_ERROR) {
      if (family == AF_INET) {
        const auto* table = reinterpret_cast<const MIB_UDPTABLE_OWNER_PID*>(udp.data());
        for (DWORD i = 0; i < table->dwNumEntries; ++i) {
          if (static_cast<long>(table->table[i].dwOwningPid) == pid) rows.push_back({"udp", false, ntohs(static_cast<u_short>(table->table[i].dwLocalPort)), {}});
        }
      } else {
        const auto* table = reinterpret_cast<const MIB_UDP6TABLE_OWNER_PID*>(udp.data());
        for (DWORD i = 0; i < table->dwNumEntries; ++i) {
          if (static_cast<long>(table->table[i].dwOwningPid) == pid) rows.push_back({"udp6", false, ntohs(static_cast<u_short>(table->table[i].dwLocalPort)), {}});
        }
      }
    }
  }
  return rows;
}
inline std::size_t thread_count(const long pid) {
  const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snapshot == INVALID_HANDLE_VALUE) return 0;
  THREADENTRY32 entry{};
  entry.dwSize = sizeof(entry);
  std::size_t n = 0;
  for (BOOL ok = Thread32First(snapshot, &entry); ok; ok = Thread32Next(snapshot, &entry)) {
    if (static_cast<long>(entry.th32OwnerProcessID) == pid) ++n;
  }
  CloseHandle(snapshot);
  return n;
}
inline std::size_t handle_count(const long pid) {
  const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
  if (process == nullptr) return 0;
  DWORD count = 0;
  GetProcessHandleCount(process, &count);
  CloseHandle(process);
  return count;
}
// The handles of `pid` by object type name ("File", "Process", "Event", ...), from
// NtQuerySystemInformation(SystemExtendedHandleInformation) typed through the object
// type table (NtQueryObject ObjectTypesInformation). Empty when either query fails.
inline std::optional<std::map<std::string, std::size_t>> handle_types(const long pid) {
  using QuerySystemFn = LONG(NTAPI*)(ULONG, void*, ULONG, ULONG*);
  using QueryObjectFn = LONG(NTAPI*)(HANDLE, ULONG, void*, ULONG, ULONG*);
  const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
  if (ntdll == nullptr) return std::nullopt;
  const auto query_system =
      reinterpret_cast<QuerySystemFn>(reinterpret_cast<void (*)()>(GetProcAddress(ntdll, "NtQuerySystemInformation")));
  const auto query_object = reinterpret_cast<QueryObjectFn>(reinterpret_cast<void (*)()>(GetProcAddress(ntdll, "NtQueryObject")));
  if (query_system == nullptr || query_object == nullptr) return std::nullopt;
  // Grows the buffer while the call answers STATUS_INFO_LENGTH_MISMATCH.
  const auto fill = [](const auto& call) {
    std::vector<unsigned char> buffer(std::size_t{1} << 20U);
    for (;;) {
      ULONG needed = 0;
      const LONG status = call(buffer.data(), static_cast<ULONG>(buffer.size()), &needed);
      if (status == 0) return buffer;
      if (status != static_cast<LONG>(0xC0000004UL) || buffer.size() >= (std::size_t{1} << 28U)) return std::vector<unsigned char>{};
      buffer.resize(buffer.size() * 2U);
    }
  };
  struct ObjectTypeInformation final {  // OBJECT_TYPE_INFORMATION (Windows 8.1+: TypeIndex)
    USHORT length;
    USHORT maximum_length;
    PWSTR name;
    ULONG counters[12];
    ULONG invalid_attributes;
    GENERIC_MAPPING generic_mapping;
    ULONG valid_access_mask;
    BOOLEAN security_required;
    BOOLEAN maintain_handle_count;
    UCHAR type_index;
    CHAR reserved_byte;
    ULONG pool_type;
    ULONG default_paged_pool_charge;
    ULONG default_non_paged_pool_charge;
  };
  struct HandleEntry final {  // SYSTEM_HANDLE_TABLE_ENTRY_INFO_EX
    void* object;
    ULONG_PTR process;
    ULONG_PTR value;
    ULONG access;
    USHORT backtrace;
    USHORT type_index;
    ULONG attributes;
    ULONG reserved;
  };
  const auto types = fill([&](void* b, ULONG n, ULONG* needed) { return query_object(nullptr, 3 /* ObjectTypesInformation */, b, n, needed); });
  if (types.size() < sizeof(ULONG_PTR)) return std::nullopt;
  std::map<unsigned, std::string> names;
  ULONG type_count = 0;
  std::memcpy(&type_count, types.data(), sizeof(type_count));
  std::size_t offset = sizeof(ULONG_PTR);  // the first entry is pointer-aligned after the count
  for (ULONG i = 0; i < type_count && offset + sizeof(ObjectTypeInformation) <= types.size(); ++i) {
    ObjectTypeInformation type{};
    std::memcpy(&type, types.data() + offset, sizeof(type));
    std::string name;
    for (USHORT c = 0; type.name != nullptr && c < type.length / sizeof(wchar_t); ++c) {
      name.push_back(static_cast<char>(type.name[c] < 0x80 ? type.name[c] : L'?'));
    }
    names[type.type_index] = name;
    offset += (sizeof(ObjectTypeInformation) + type.maximum_length + sizeof(ULONG_PTR) - 1U) & ~(sizeof(ULONG_PTR) - 1U);
  }
  const auto handles = fill([&](void* b, ULONG n, ULONG* needed) { return query_system(64 /* SystemExtendedHandleInformation */, b, n, needed); });
  if (handles.size() < 2U * sizeof(ULONG_PTR) || names.empty()) return std::nullopt;
  ULONG_PTR handle_total = 0;
  std::memcpy(&handle_total, handles.data(), sizeof(handle_total));
  std::map<std::string, std::size_t> counts;
  for (ULONG_PTR i = 0; i < handle_total; ++i) {
    const std::size_t at = 2U * sizeof(ULONG_PTR) + static_cast<std::size_t>(i) * sizeof(HandleEntry);
    if (at + sizeof(HandleEntry) > handles.size()) break;
    HandleEntry entry{};
    std::memcpy(&entry, handles.data() + at, sizeof(entry));
    if (entry.process != static_cast<ULONG_PTR>(pid)) continue;
    const auto name = names.find(entry.type_index);
    ++counts[name != names.end() ? name->second : "type#" + std::to_string(entry.type_index)];
  }
  return counts;
}
// DOS-02's per-request kinds: File (sockets, pipes, files) and Process (an engine
// child), exact. The process total also holds the OS thread pool's own Thread, Event
// and WaitCompletionPacket objects, which Winsock and RPC grow and trim on their own
// (measured: File stays put while the total drifts by a few; session log, step 10).
inline std::optional<std::size_t> request_handle_count(const long pid) {
  const auto types = handle_types(pid);
  // A live server always holds File handles (its listener, the engine pipes): none
  // means the typing failed, which must not compare as a vacuous 0 == 0.
  if (!types || !types->contains("File")) return std::nullopt;
  std::size_t n = 0;
  for (const char* kind : {"File", "Process"}) {
    if (const auto it = types->find(kind); it != types->end()) n += it->second;
  }
  return n;
}
#else
inline std::set<std::string> socket_inodes(const long pid) {
  std::set<std::string> inodes;
  std::error_code ec;
  for (const auto& entry : std::filesystem::directory_iterator("/proc/" + std::to_string(pid) + "/fd", ec)) {
    std::error_code link_error;
    const std::string target = std::filesystem::read_symlink(entry.path(), link_error).string();
    if (target.rfind("socket:[", 0) == 0) inodes.insert(target.substr(8, target.size() - 9));
  }
  return inodes;
}
// Ruling T12-scan: the server sets PR_SET_DUMPABLE(0) (SP0 hardening), so
// /proc/<pid>/fd is unreadable from the test even at the same uid. When the fd
// scan yields nothing, rows are attributed by uid instead: the net tables below
// are the process's network-namespace view, and the server shares uid and
// namespace with the test. Dumpable processes (the fake engine) keep the exact
// inode mapping. Either way a listening socket the pid does not own is caught
// as soon as it belongs to this uid/namespace (NET-01, spec §6.2 scan).
inline std::vector<SocketRow> sockets_of(const long pid) {
  const auto inodes = socket_inodes(pid);
  const bool by_inode = !inodes.empty();
  const std::string uid_field = std::to_string(static_cast<long>(geteuid()));
  std::vector<SocketRow> rows;
  for (const std::string kind : {"tcp", "tcp6", "udp", "udp6"}) {
    std::ifstream table("/proc/" + std::to_string(pid) + "/net/" + kind);
    if (!table) table = std::ifstream("/proc/net/" + kind);
    std::string line;
    std::getline(table, line);   // header
    while (std::getline(table, line)) {
      std::istringstream fields(line);
      std::string slot, local, remote, state, queues, timer, retransmit, uid, timeout, inode;
      fields >> slot >> local >> remote >> state >> queues >> timer >> retransmit >> uid >> timeout >> inode;
      if (by_inode ? !inodes.count(inode) : uid != uid_field) continue;
      const auto colon = local.rfind(':');
      rows.push_back({kind, state == "0A", static_cast<std::uint16_t>(std::stoul(local.substr(colon + 1U), nullptr, 16)), local.substr(0, colon)});
    }
  }
  std::ifstream unix_table("/proc/" + std::to_string(pid) + "/net/unix");
  if (!unix_table) unix_table = std::ifstream("/proc/net/unix");
  std::string line;
  std::getline(unix_table, line);
  while (std::getline(unix_table, line)) {
    std::istringstream fields(line);
    std::string num, refs, protocol, flags, type, st, inode;
    fields >> num >> refs >> protocol >> flags >> type >> st >> inode;
    if (by_inode ? inodes.count(inode) : true) rows.push_back({"unix", false, 0, {}});
  }
  return rows;
}
inline std::size_t count_entries(const std::string& dir) {
  std::size_t n = 0;
  std::error_code ec;
  for (auto it = std::filesystem::directory_iterator(dir, ec); !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) ++n;
  return n;
}
inline std::size_t thread_count(const long pid) { return count_entries("/proc/" + std::to_string(pid) + "/task"); }
inline std::size_t handle_count(const long pid) { return count_entries("/proc/" + std::to_string(pid) + "/fd"); }
// Every descriptor is a per-request kind here (sockets, pipes, files).
inline std::optional<std::size_t> request_handle_count(const long pid) { return handle_count(pid); }
#endif

// Loopback in the kernel's /proc hex form (127.x.y.z little-endian ends in "7F"; ::1 is ...01000000) or Windows numbers.
inline bool is_loopback_row(const SocketRow& row) {
#if defined(_WIN32)
  if (row.kind == "tcp") return (std::stoul(row.local_address) & 0xffU) == 127U;
  return row.local_address == std::string("\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\x01", 16);
#else
  if (row.kind == "tcp") return row.local_address.size() == 8U && row.local_address.substr(6) == "7F";
  return row.local_address == "00000000000000000000000001000000";
#endif
}

}  // namespace test_support
