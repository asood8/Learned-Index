// Phase 8: a small binary row format. Each row is an ordered list of
// typed values; each value is encoded as a one-byte type tag followed
// by its bytes -- 8 raw bytes for an int, a 4-byte length prefix plus
// the raw bytes for text. This is a simplified version of what
// SQLite calls its "record format" -- real ones support more types
// and variable-length integer encoding, but the core idea (tag +
// bytes, concatenated) is the same.
#pragma once

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

enum class ColumnType : uint8_t { INT64 = 0, TEXT = 1 };

struct Value {
  ColumnType type;
  int64_t int_val = 0;
  std::string text_val;

  static Value make_int(int64_t v) { return Value{ColumnType::INT64, v, ""}; }
  static Value make_text(std::string v) { return Value{ColumnType::TEXT, 0, std::move(v)}; }
};

inline std::string encode_row(const std::vector<Value>& values) {
  std::string out;
  for (const auto& v : values) {
    out.push_back(static_cast<char>(v.type));
    if (v.type == ColumnType::INT64) {
      out.append(reinterpret_cast<const char*>(&v.int_val), sizeof(v.int_val));
    } else {
      const uint32_t len = static_cast<uint32_t>(v.text_val.size());
      out.append(reinterpret_cast<const char*>(&len), sizeof(len));
      out.append(v.text_val);
    }
  }
  return out;
}

inline std::vector<Value> decode_row(const std::string& data) {
  std::vector<Value> values;
  size_t pos = 0;
  while (pos < data.size()) {
    if (pos + 1 > data.size()) throw std::runtime_error("truncated row: missing type tag");
    const ColumnType type = static_cast<ColumnType>(data[pos]);
    pos += 1;
    if (type == ColumnType::INT64) {
      if (pos + sizeof(int64_t) > data.size()) throw std::runtime_error("truncated row: missing int bytes");
      int64_t v;
      std::memcpy(&v, data.data() + pos, sizeof(v));
      pos += sizeof(v);
      values.push_back(Value::make_int(v));
    } else {
      if (pos + sizeof(uint32_t) > data.size()) throw std::runtime_error("truncated row: missing text length");
      uint32_t len;
      std::memcpy(&len, data.data() + pos, sizeof(len));
      pos += sizeof(len);
      if (pos + len > data.size()) throw std::runtime_error("truncated row: missing text bytes");
      values.push_back(Value::make_text(data.substr(pos, len)));
      pos += len;
    }
  }
  return values;
}
