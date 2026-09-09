// Phase 5 (extended in Phase 8): a write-ahead log. Every put gets
// appended to a plain file and forced to disk *before* the in-memory
// structure is touched -- if the process dies a millisecond later,
// the write still happened as far as anyone restarting is concerned.
// This is the literal "D" in ACID.
//
// Originally logged just a key (Phase 5's gapped-array inserts had no
// associated value). Phase 8 needs real key->value storage for rows,
// so each record now also carries a length-prefixed value blob.
#pragma once

#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

class WriteAheadLog {
 public:
  explicit WriteAheadLog(const std::string& path) {
    fd_ = open(path.c_str(), O_CREAT | O_WRONLY | O_APPEND, 0644);
    if (fd_ < 0) throw std::runtime_error("could not open WAL file: " + path);
  }

  ~WriteAheadLog() {
    if (fd_ >= 0) close(fd_);
  }

  // Appends one (key, value) record -- key (8 bytes), value length (4
  // bytes), then the value bytes -- and forces it to disk before
  // returning. fsync is what actually makes this durable: without it,
  // the write could still be sitting in an OS buffer, not on the
  // physical disk, when the process dies. This is also the expensive
  // part, measured back in Phase 5: a real, non-free cost.
  void log_put(int64_t key, const std::string& value) {
    const uint32_t len = static_cast<uint32_t>(value.size());
    const ssize_t w1 = write(fd_, &key, sizeof(key));
    const ssize_t w2 = write(fd_, &len, sizeof(len));
    const ssize_t w3 = value.empty() ? 0 : write(fd_, value.data(), value.size());
    if (w1 != static_cast<ssize_t>(sizeof(key)) || w2 != static_cast<ssize_t>(sizeof(len)) ||
        w3 != static_cast<ssize_t>(value.size())) {
      throw std::runtime_error("WAL write failed");
    }
    if (fsync(fd_) != 0) throw std::runtime_error("WAL fsync failed");
  }

  // Reads every logged (key, value) pair back in order, for replay on
  // startup. A missing file just means a fresh start, not an error.
  // A record torn by a mid-write crash -- a partial key, a partial
  // length, or a value cut short -- is silently discarded rather than
  // misread as something else: real WAL recovery always drops a torn
  // trailing record and trusts everything before it.
  static std::vector<std::pair<int64_t, std::string>> replay(const std::string& path) {
    std::vector<std::pair<int64_t, std::string>> entries;
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return entries;

    while (true) {
      int64_t key;
      if (fread(&key, sizeof(key), 1, f) != 1) break;
      uint32_t len;
      if (fread(&len, sizeof(len), 1, f) != 1) break;
      std::string value(len, '\0');
      if (len > 0 && fread(&value[0], 1, len, f) != len) break;
      entries.emplace_back(key, std::move(value));
    }
    fclose(f);
    return entries;
  }

 private:
  int fd_ = -1;
};
