// Where the benchmarks write their CSV files, and how they write a field.
//
// The files go to $LIDB_RESULTS_DIR if it's set, otherwise to results/.
// scripts/run_benchmarks.sh points it at the run's own directory, so the
// rows from one run stay together instead of piling up in one file with
// nothing saying which run each row came from.
#pragma once

#include <cstdlib>
#include <fstream>
#include <string>

inline std::string results_path(const std::string& file) {
  const char* dir = std::getenv("LIDB_RESULTS_DIR");
  return std::string(dir != nullptr && *dir != '\0' ? dir : "results") + "/" + file;
}

// Opens a CSV for appending, writing the header first if the file is new.
inline std::ofstream open_csv(const std::string& file, const char* header) {
  const std::string path = results_path(file);
  const bool new_file = !std::ifstream(path).good();
  std::ofstream csv(path, std::ios::app);
  if (new_file) csv << header << "\n";
  return csv;
}

// Quotes a field that contains a comma, a quote or a newline (RFC 4180).
// Several metric names do, e.g. "durable insert, fsync every 8 (ns)".
inline std::string csv_field(const std::string& s) {
  if (s.find_first_of(",\"\n") == std::string::npos) return s;
  std::string quoted = "\"";
  for (char c : s) {
    if (c == '"') quoted += '"';
    quoted += c;
  }
  return quoted + "\"";
}
