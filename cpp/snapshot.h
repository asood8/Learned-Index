// A snapshot is the whole store written out in key order. Checkpointing
// saves one so the write-ahead log can start over empty, and startup
// loads it before replaying whatever the log has gathered since.
//
// Layout: "LIDXSNP1", the entry count (8 bytes), then for each entry
//   key (8) | value length (4) | value
// and finally a CRC-32 (4) of everything after the magic string.
//
// A snapshot is written to a temporary file and only renamed into place
// once it's complete and fsync'd, so a crash can't leave a half-written
// snapshot where startup will read it. The CRC is there for damage that
// happens afterwards. A snapshot that fails it is refused, not skipped:
// skipping it would silently lose everything the emptied log no longer
// has.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

#include "write_ahead_log.h"  // crc32, fsync_parent_dir

inline constexpr char SNAPSHOT_MAGIC[] = "LIDXSNP1";
inline constexpr size_t SNAPSHOT_MAGIC_LEN = 8;

inline void write_snapshot(const std::string& path, const std::vector<std::pair<int64_t, std::string>>& entries) {
  std::string buf(SNAPSHOT_MAGIC, SNAPSHOT_MAGIC_LEN);
  const uint64_t count = entries.size();
  buf.append(reinterpret_cast<const char*>(&count), sizeof(count));
  for (const auto& [key, value] : entries) {
    const uint32_t len = static_cast<uint32_t>(value.size());
    buf.append(reinterpret_cast<const char*>(&key), sizeof(key));
    buf.append(reinterpret_cast<const char*>(&len), sizeof(len));
    buf.append(value);
  }
  const uint32_t crc = crc32(reinterpret_cast<const unsigned char*>(buf.data()) + SNAPSHOT_MAGIC_LEN,
                             buf.size() - SNAPSHOT_MAGIC_LEN);
  buf.append(reinterpret_cast<const char*>(&crc), sizeof(crc));

  const std::string tmp = path + ".tmp";
  const int fd = open(tmp.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0644);
  if (fd < 0) throw std::runtime_error("could not create snapshot file: " + tmp);
  const char* data = buf.data();
  size_t left = buf.size();
  while (left > 0) {
    const ssize_t w = write(fd, data, left);
    if (w < 0) {
      if (errno == EINTR) continue;
      close(fd);
      throw std::runtime_error("could not write snapshot file: " + tmp);
    }
    data += w;
    left -= static_cast<size_t>(w);
  }
  if (fsync(fd) != 0) {
    close(fd);
    throw std::runtime_error("could not fsync snapshot file: " + tmp);
  }
  close(fd);
  if (std::rename(tmp.c_str(), path.c_str()) != 0) throw std::runtime_error("could not rename snapshot into place: " + path);
  fsync_parent_dir(path);
}

// Reads the snapshot at `path` into keys and values (sorted by key).
// Returns false if there's no snapshot; throws if there is one but it's
// damaged.
inline bool read_snapshot(const std::string& path, std::vector<int64_t>& keys, std::vector<std::string>& values) {
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return false;
  std::string buf;
  char chunk[1 << 16];
  size_t got;
  while ((got = std::fread(chunk, 1, sizeof(chunk), f)) > 0) buf.append(chunk, got);
  std::fclose(f);

  auto damaged = [&path](const char* why) { return std::runtime_error("snapshot " + path + " is damaged (" + why + ")"); };
  const size_t min_size = SNAPSHOT_MAGIC_LEN + sizeof(uint64_t) + sizeof(uint32_t);
  if (buf.size() < min_size || buf.compare(0, SNAPSHOT_MAGIC_LEN, SNAPSHOT_MAGIC, SNAPSHOT_MAGIC_LEN) != 0) {
    throw damaged("bad header");
  }
  uint32_t stored_crc;
  std::memcpy(&stored_crc, buf.data() + buf.size() - sizeof(stored_crc), sizeof(stored_crc));
  const size_t body_end = buf.size() - sizeof(stored_crc);
  if (stored_crc != crc32(reinterpret_cast<const unsigned char*>(buf.data()) + SNAPSHOT_MAGIC_LEN,
                          body_end - SNAPSHOT_MAGIC_LEN)) {
    throw damaged("checksum mismatch");
  }

  uint64_t count;
  std::memcpy(&count, buf.data() + SNAPSHOT_MAGIC_LEN, sizeof(count));
  size_t pos = SNAPSHOT_MAGIC_LEN + sizeof(count);
  keys.clear();
  values.clear();
  keys.reserve(count);
  values.reserve(count);
  for (uint64_t i = 0; i < count; i++) {
    if (body_end - pos < sizeof(int64_t) + sizeof(uint32_t)) throw damaged("entry cut short");
    int64_t key;
    uint32_t len;
    std::memcpy(&key, buf.data() + pos, sizeof(key));
    std::memcpy(&len, buf.data() + pos + sizeof(key), sizeof(len));
    pos += sizeof(key) + sizeof(len);
    if (len > body_end - pos) throw damaged("value cut short");
    if (!keys.empty() && key <= keys.back()) throw damaged("keys out of order");
    keys.push_back(key);
    values.push_back(buf.substr(pos, len));
    pos += len;
  }
  if (pos != body_end) throw damaged("trailing bytes");
  return true;
}
