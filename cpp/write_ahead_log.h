// Phase 5: a write-ahead log. Every insert gets appended to a plain
// file and forced to disk *before* the in-memory structure is
// touched -- if the process dies a millisecond later, the insert
// still happened as far as anyone restarting is concerned. This is
// the literal "D" in ACID: durability, meaning survives a restart,
// not just "doesn't crash while running."
#pragma once

#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>
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

  // Appends one insert record and forces it to disk before
  // returning. fsync is what actually makes this durable -- without
  // it, the write could still be sitting in an OS buffer, not on the
  // physical disk, when the process dies. This is also the expensive
  // part: fsync is a real, measurable cost, not a free operation.
  void log_insert(int64_t key) {
    ssize_t written = write(fd_, &key, sizeof(key));
    if (written != static_cast<ssize_t>(sizeof(key))) {
      throw std::runtime_error("WAL write failed");
    }
    if (fsync(fd_) != 0) throw std::runtime_error("WAL fsync failed");
  }

  // Reads every logged key back in the order it was written. Used on
  // startup to replay history into a fresh, empty structure. A
  // missing file just means a fresh start, not an error; a partial
  // trailing record (from a crash mid-write) is silently dropped
  // rather than misread, since fread simply returns short on it.
  static std::vector<int64_t> replay(const std::string& path) {
    std::vector<int64_t> keys;
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return keys;
    int64_t key;
    while (fread(&key, sizeof(key), 1, f) == 1) keys.push_back(key);
    fclose(f);
    return keys;
  }

 private:
  int fd_ = -1;
};
