// strata-unified-engine: read-only positional file access (no mapping, no writes).
// Mirrored from engines/gemma4/src/g4_file.hpp (the reviewed reference) into our tree.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace u {

class ReadOnlyFile final {
 public:
  ReadOnlyFile() = default;
  ReadOnlyFile(const ReadOnlyFile&) = delete;
  ReadOnlyFile& operator=(const ReadOnlyFile&) = delete;
  ~ReadOnlyFile();

  // `path` is UTF-8. False (and the file stays closed) when it cannot be opened for reading.
  [[nodiscard]] bool open(const std::string& path);
  [[nodiscard]] bool is_open() const noexcept;
  [[nodiscard]] std::uint64_t size() const noexcept { return size_; }
  // Reads exactly `bytes` at `offset` into `out`; false on any short read or error.
  [[nodiscard]] bool read_at(std::uint64_t offset, void* out, std::size_t bytes) const;
  void close() noexcept;

 private:
#if defined(_WIN32)
  void* handle_{nullptr};
#else
  int fd_{-1};
#endif
  std::uint64_t size_{0};
};

}  // namespace u
