#include "u_file.hpp"

#include <algorithm>
#include <limits>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace u {

ReadOnlyFile::~ReadOnlyFile() { close(); }

#if defined(_WIN32)
namespace {
std::wstring widen(const std::string& utf8) {
  if (utf8.empty()) return {};
  const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
  if (n <= 0) return {};
  std::wstring wide(static_cast<std::size_t>(n), L'\0');
  if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), static_cast<int>(utf8.size()), wide.data(), n) != n) return {};
  return wide;
}
}  // namespace

bool ReadOnlyFile::open(const std::string& path) {
  close();
  if (path.size() > 32767) return false;
  const std::wstring wide = widen(path);
  if (wide.empty()) return false;
  HANDLE h = CreateFileW(wide.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                         FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
  if (h == INVALID_HANDLE_VALUE) return false;
  BY_HANDLE_FILE_INFORMATION info{};
  LARGE_INTEGER size{};
  if (!GetFileInformationByHandle(h, &info) || (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 ||
      !GetFileSizeEx(h, &size) || size.QuadPart < 0) {
    CloseHandle(h);
    return false;
  }
  handle_ = h;
  size_ = static_cast<std::uint64_t>(size.QuadPart);
  return true;
}

bool ReadOnlyFile::is_open() const noexcept { return handle_ != nullptr; }

bool ReadOnlyFile::read_at(std::uint64_t offset, void* out, std::size_t bytes) const {
  if (handle_ == nullptr || offset > size_ || bytes > size_ - offset) return false;
  auto* dst = static_cast<unsigned char*>(out);
  while (bytes > 0) {
    const DWORD chunk = static_cast<DWORD>(std::min<std::size_t>(bytes, 1u << 30));
    OVERLAPPED at{};
    at.Offset = static_cast<DWORD>(offset & 0xFFFFFFFFull);
    at.OffsetHigh = static_cast<DWORD>(offset >> 32);
    DWORD got = 0;
    if (!ReadFile(static_cast<HANDLE>(handle_), dst, chunk, &got, &at) || got != chunk) return false;
    dst += got;
    offset += got;
    bytes -= got;
  }
  return true;
}

void ReadOnlyFile::close() noexcept {
  if (handle_ != nullptr) CloseHandle(static_cast<HANDLE>(handle_));
  handle_ = nullptr;
  size_ = 0;
}
#else
bool ReadOnlyFile::open(const std::string& path) {
  close();
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) return false;
  struct stat st {};
  if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0) {
    ::close(fd);
    return false;
  }
  fd_ = fd;
  size_ = static_cast<std::uint64_t>(st.st_size);
  return true;
}

bool ReadOnlyFile::is_open() const noexcept { return fd_ >= 0; }

bool ReadOnlyFile::read_at(std::uint64_t offset, void* out, std::size_t bytes) const {
  if (fd_ < 0 || offset > size_ || bytes > size_ - offset) return false;
  auto* dst = static_cast<unsigned char*>(out);
  while (bytes > 0) {
    const std::size_t chunk = std::min<std::size_t>(bytes, 1u << 30);
    const ssize_t got = ::pread(fd_, dst, chunk, static_cast<off_t>(offset));
    if (got <= 0) return false;
    dst += got;
    offset += static_cast<std::uint64_t>(got);
    bytes -= static_cast<std::size_t>(got);
  }
  return true;
}

void ReadOnlyFile::close() noexcept {
  if (fd_ >= 0) ::close(fd_);
  fd_ = -1;
  size_ = 0;
}
#endif

}  // namespace u
