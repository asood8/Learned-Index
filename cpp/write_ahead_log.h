// Phase 5 (extended in Phase 8, extended again for the DELETE stretch
// goal): a write-ahead log. Every mutation gets appended to a plain
// file and forced to disk *before* the in-memory structure is
// touched -- if the process dies a millisecond later, the write still
// happened as far as anyone restarting is concerned. This is the
// literal "D" in ACID.
//
// Originally logged just a key (Phase 5's gapped-array inserts had no
// associated value). Phase 8 added a length-prefixed value blob for
// real key->value storage. DELETE needed a real second record type --
// a plain "key with no value" is indistinguishable from a genuinely
// empty value, so every record now carries an explicit one-byte tag.
#pragma once

#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

enum class WalRecordType : uint8_t { PUT = 0, DELETE = 1 };

struct WalRecord {
  WalRecordType type;
  int64_t key;
  std::string value;  // empty and unused for DELETE records
};

class WriteAheadLog {
 public:
  explicit WriteAheadLog(const std::string& path) {
    fd_ = open(path.c_str(), O_CREAT | O_WRONLY | O_APPEND, 0644);
    if (fd_ < 0) throw std::runtime_error("could not open WAL file: " + path);
  }

  ~WriteAheadLog() {
    if (fd_ >= 0) close(fd_);
  }

  // Appends one PUT record -- type tag, key (8 bytes), value length (4
  // bytes), then the value bytes -- and forces it to disk before
  // returning. fsync is what actually makes this durable: without it,
  // the write could still be sitting in an OS buffer, not on the
  // physical disk, when the process dies. This is also the expensive
  // part, measured back in Phase 5: a real, non-free cost.
  void log_put(int64_t key, const std::string& value) {
    const uint8_t type = static_cast<uint8_t>(WalRecordType::PUT);
    const uint32_t len = static_cast<uint32_t>(value.size());
    const ssize_t w0 = write(fd_, &type, sizeof(type));
    const ssize_t w1 = write(fd_, &key, sizeof(key));
    const ssize_t w2 = write(fd_, &len, sizeof(len));
    const ssize_t w3 = value.empty() ? 0 : write(fd_, value.data(), value.size());
    if (w0 != static_cast<ssize_t>(sizeof(type)) || w1 != static_cast<ssize_t>(sizeof(key)) ||
        w2 != static_cast<ssize_t>(sizeof(len)) || w3 != static_cast<ssize_t>(value.size())) {
      throw std::runtime_error("WAL write failed");
    }
    if (fsync(fd_) != 0) throw std::runtime_error("WAL fsync failed");
  }

  // Appends one DELETE record -- just the type tag and the key. No
  // value to write, but still fsync'd before returning, for the same
  // reason a PUT is: durability means the deletion survives a crash
  // that happens immediately after this call returns.
  void log_delete(int64_t key) {
    const uint8_t type = static_cast<uint8_t>(WalRecordType::DELETE);
    const ssize_t w0 = write(fd_, &type, sizeof(type));
    const ssize_t w1 = write(fd_, &key, sizeof(key));
    if (w0 != static_cast<ssize_t>(sizeof(type)) || w1 != static_cast<ssize_t>(sizeof(key))) {
      throw std::runtime_error("WAL write failed");
    }
    if (fsync(fd_) != 0) throw std::runtime_error("WAL fsync failed");
  }

  // Reads every logged record back in order, for replay on startup. A
  // missing file just means a fresh start, not an error. A record
  // torn by a mid-write crash -- a partial type tag, key, length, or
  // value cut short -- is silently discarded rather than misread as
  // something else: real WAL recovery always drops a torn trailing
  // record and trusts everything before it.
  static std::vector<WalRecord> replay(const std::string& path) {
    std::vector<WalRecord> entries;
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return entries;

    while (true) {
      uint8_t type_byte;
      if (fread(&type_byte, sizeof(type_byte), 1, f) != 1) break;
      int64_t key;
      if (fread(&key, sizeof(key), 1, f) != 1) break;

      if (type_byte == static_cast<uint8_t>(WalRecordType::DELETE)) {
        entries.push_back({WalRecordType::DELETE, key, ""});
        continue;
      }
      if (type_byte != static_cast<uint8_t>(WalRecordType::PUT)) break;  // corrupt/unknown tag: stop here

      uint32_t len;
      if (fread(&len, sizeof(len), 1, f) != 1) break;
      std::string value(len, '\0');
      if (len > 0 && fread(&value[0], 1, len, f) != len) break;
      entries.push_back({WalRecordType::PUT, key, std::move(value)});
    }
    fclose(f);
    return entries;
  }

 private:
  int fd_ = -1;
};
