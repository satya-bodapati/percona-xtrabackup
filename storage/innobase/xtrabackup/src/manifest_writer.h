#ifndef MANIFEST_WRITER_H
#define MANIFEST_WRITER_H

#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <mutex>
#include <unordered_map>
#include <variant>
#include <vector>

/** FileProperties */
using ValueType = std::variant<std::string, int, uint64_t, double, bool>;
using FileProperties = std::vector<std::tuple<std::string, ValueType>>;
using FileEntry = std::tuple<std::string, FileProperties>;

class ManifestWriter {
 private:
  std::mutex writeMutex;
  std::ofstream ofs;
  std::map<std::string, ValueType> infoEntries;
  std::ofstream tempFile;
  std::string tempFileName;

 public:
  explicit ManifestWriter(const std::string &filename);
  template <typename T>
  bool addInfoEntry(const std::string &key, T value) {
    std::lock_guard<std::mutex> lock(writeMutex);
    infoEntries[key] = value;
    return true;
  }
  bool addFileEntry(const std::string &filePath, FileProperties &prop);
  bool updateEntry(const std::string &key, const std::string &newValue);
  bool close();
};

extern bool opt_backup_manifest;
extern ManifestWriter *manifest_writer;

#endif  // MANIFEST_WRITER_H
