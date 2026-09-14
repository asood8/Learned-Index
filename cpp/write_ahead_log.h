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
//
// Format 2, after crash recovery turned out to be broken. The first
// version skipped a torn last record on replay but left its bytes in
// the file, and new writes were appended after them. On the restart
// after that, replay read the torn record's leftover bytes together
// with the next record's, so writes made after a crash were lost or
// misread. It also had no checksums: a zero-filled tail read back as
// real records, and a damaged byte in the middle was accepted as data.
// Now:
//   - the file starts with an 8-byte magic string, and a file in any
//     other format is refused rather than "recovered" into nothing;
//   - every record ends with a CRC-32 of its own bytes;
//   - opening the log finds the last record that checks out and cuts
//     the file back to it before anything new is appended.
// Recovery stops at the first bad record even if later bytes look
// fine, since nothing after a damaged record can be trusted to line
// up. That's the usual rule for write-ahead logs. One thing still not
// handled: after creating a new log, the parent directory isn't
// fsync'd, which some filesystems need before the new file itself is
// guaranteed to survive a crash.
//
// Layout: "LIDXWAL2", then for each record
//   type (1 byte) | key (8) | value length (4) | value | CRC-32 (4)
// with the CRC covering everything before it in that record. DELETE
// records have an empty value.
#pragma once

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

enum class WalRecordType : uint8_t { PUT = 0, DELETE = 1 };

struct WalRecord {
  WalRecordType type;
  int64_t key;
  std::string value;  // empty and unused for DELETE records
};

// The standard CRC-32 (the polynomial zlib and PNG use), table-driven.
inline uint32_t crc32(const unsigned char* data, size_t len) {
  static const std::array<uint32_t, 256> table = [] {
    std::array<uint32_t, 256> t{};
    for (uint32_t i = 0; i < 256; i++) {
      uint32_t c = i;
      for (int bit = 0; bit < 8; bit++) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
      t[i] = c;
    }
    return t;
  }();
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < len; i++) c = table[(c ^ data[i]) & 0xFF] ^ (c >> 8);
  return c ^ 0xFFFFFFFFu;
}

class WriteAheadLog {
 public:
  static constexpr char MAGIC[] = "LIDXWAL2";
  static constexpr size_t MAGIC_LEN = 8;
  static constexpr size_t HEADER_LEN = 1 + 8 + 4;  // type, key, value length
  static constexpr size_t CRC_LEN = 4;

  // Opens the log, creating it if needed, and recovers it: anything
  // after the last intact record is cut off so new appends start on a
  // clean boundary. Throws if the file exists but isn't a log in this
  // format, without modifying it.
  explicit WriteAheadLog(const std::string& path) {
    fd_ = open(path.c_str(), O_CREAT | O_RDWR | O_APPEND, 0644);
    if (fd_ < 0) throw std::runtime_error("could not open WAL file: " + path);
    try {
      recover(path);
    } catch (...) {
      close(fd_);
      fd_ = -1;
      throw;
    }
  }

  ~WriteAheadLog() {
    if (fd_ >= 0) close(fd_);
  }

  WriteAheadLog(const WriteAheadLog&) = delete;
  WriteAheadLog& operator=(const WriteAheadLog&) = delete;

  // Appends one record and forces it to disk before returning. fsync
  // is what actually makes this durable: without it, the write could
  // still be sitting in an OS buffer, not on the physical disk, when
  // the process dies. It's also the expensive part, measured back in
  // Phase 5.
  void log_put(int64_t key, const std::string& value) { append(WalRecordType::PUT, key, value); }
  void log_delete(int64_t key) { append(WalRecordType::DELETE, key, std::string()); }

  // Every intact record in order, for replay on startup. A missing
  // file means a fresh start. Reading stops at the first record that's
  // cut short or fails its checksum.
  static std::vector<WalRecord> replay(const std::string& path) {
    std::vector<WalRecord> records;
    uint64_t valid_end = 0;
    if (!scan(path, &records, &valid_end)) {
      throw std::runtime_error("not a write-ahead log in the expected format: " + path);
    }
    return records;
  }

 private:
  int fd_ = -1;
  uint64_t size_ = 0;  // bytes of intact log on disk

  void recover(const std::string& path) {
    struct stat st;
    if (fstat(fd_, &st) != 0) throw std::runtime_error("could not stat WAL file: " + path);
    const uint64_t file_size = static_cast<uint64_t>(st.st_size);

    if (file_size < MAGIC_LEN) {
      // Either brand new, or a crash hit while the header itself was
      // being written. Anything else this short isn't ours.
      char existing[MAGIC_LEN];
      const ssize_t got = pread(fd_, existing, file_size, 0);
      if (got != static_cast<ssize_t>(file_size) || std::memcmp(existing, MAGIC, file_size) != 0) {
        throw std::runtime_error("not a write-ahead log in the expected format: " + path);
      }
      if (ftruncate(fd_, 0) != 0 || !write_all(MAGIC, MAGIC_LEN) || fsync(fd_) != 0) {
        throw std::runtime_error("could not initialize WAL file: " + path);
      }
      size_ = MAGIC_LEN;
      return;
    }

    uint64_t valid_end = 0;
    if (!scan(path, nullptr, &valid_end)) {
      throw std::runtime_error("not a write-ahead log in the expected format: " + path);
    }
    if (valid_end < file_size) {
      if (ftruncate(fd_, static_cast<off_t>(valid_end)) != 0 || fsync(fd_) != 0) {
        throw std::runtime_error("could not cut damaged tail off WAL file: " + path);
      }
    }
    size_ = valid_end;
  }

  // Parses the log at `path`. Returns false if the file exists but
  // doesn't start with the magic string. Otherwise collects every
  // record up to the first bad one into `out` (if given) and sets
  // `valid_end` to the offset just past the last good record.
  static bool scan(const std::string& path, std::vector<WalRecord>* out, uint64_t* valid_end) {
    *valid_end = 0;
    std::string buf;
    if (!read_file(path, buf)) return true;  // no file yet: nothing to replay
    if (buf.size() < MAGIC_LEN) return buf.compare(0, buf.size(), MAGIC, buf.size()) == 0;
    if (buf.compare(0, MAGIC_LEN, MAGIC, MAGIC_LEN) != 0) return false;

    size_t pos = MAGIC_LEN;
    while (buf.size() - pos >= HEADER_LEN + CRC_LEN) {
      const auto* rec = reinterpret_cast<const unsigned char*>(buf.data() + pos);
      int64_t key;
      uint32_t len;
      std::memcpy(&key, rec + 1, sizeof(key));
      std::memcpy(&len, rec + 9, sizeof(len));
      // Check the length against what's actually left before trusting
      // it, so a damaged length can't claim gigabytes.
      if (len > buf.size() - pos - HEADER_LEN - CRC_LEN) break;
      uint32_t stored_crc;
      std::memcpy(&stored_crc, rec + HEADER_LEN + len, sizeof(stored_crc));
      if (stored_crc != crc32(rec, HEADER_LEN + len)) break;
      if (rec[0] != static_cast<uint8_t>(WalRecordType::PUT) && rec[0] != static_cast<uint8_t>(WalRecordType::DELETE)) {
        break;
      }
      if (out) {
        out->push_back({static_cast<WalRecordType>(rec[0]), key, buf.substr(pos + HEADER_LEN, len)});
      }
      pos += HEADER_LEN + len + CRC_LEN;
    }
    *valid_end = pos;
    return true;
  }

  static bool read_file(const std::string& path, std::string& buf) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    char chunk[1 << 16];
    size_t got;
    while ((got = std::fread(chunk, 1, sizeof(chunk), f)) > 0) buf.append(chunk, got);
    std::fclose(f);
    return true;
  }

  // One record goes out in a single write() and is then fsync'd. If the
  // write fails partway, the file is cut back to where it was, so a
  // failed append can't leave half a record for the next one to land
  // behind.
  void append(WalRecordType type, int64_t key, const std::string& value) {
    const uint32_t len = static_cast<uint32_t>(value.size());
    std::string rec(HEADER_LEN + len + CRC_LEN, '\0');
    rec[0] = static_cast<char>(type);
    std::memcpy(&rec[1], &key, sizeof(key));
    std::memcpy(&rec[9], &len, sizeof(len));
    if (len > 0) std::memcpy(&rec[HEADER_LEN], value.data(), len);
    const uint32_t crc = crc32(reinterpret_cast<const unsigned char*>(rec.data()), HEADER_LEN + len);
    std::memcpy(&rec[HEADER_LEN + len], &crc, sizeof(crc));

    if (!write_all(rec.data(), rec.size())) {
      if (ftruncate(fd_, static_cast<off_t>(size_)) != 0) {
        // nothing more to do here; recovery on the next open cuts it
      }
      throw std::runtime_error("WAL write failed");
    }
    if (fsync(fd_) != 0) throw std::runtime_error("WAL fsync failed");
    size_ += rec.size();
  }

  bool write_all(const char* data, size_t n) {
    while (n > 0) {
      const ssize_t w = write(fd_, data, n);
      if (w < 0) {
        if (errno == EINTR) continue;
        return false;
      }
      data += w;
      n -= static_cast<size_t>(w);
    }
    return true;
  }
};
