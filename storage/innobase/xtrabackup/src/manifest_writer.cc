#include "manifest_writer.h"
#include <rapidjson/document.h>
#include <rapidjson/ostreamwrapper.h>
#include <rapidjson/prettywriter.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <iostream>
#include <mutex>
#include <unordered_map>
#include <vector>
#include "my_rapidjson_size_t.h"

bool opt_backup_manifest = false;
ManifestWriter *manifest_writer = nullptr;

ManifestWriter::ManifestWriter(const std::string &filename)
    : ofs(filename), tempFileName(filename + ".tmp") {
  if (!ofs.is_open()) {
    std::cerr << "Error: Failed to open file: " << filename << " ("
              << std::strerror(errno) << ")" << std::endl;
    throw std::runtime_error("Failed to open file: " + filename + " (" +
                             std::strerror(errno) + ")");
  }

  tempFile.open(tempFileName, std::ios::out | std::ios::trunc);
  if (!tempFile.is_open()) {
    throw std::runtime_error("Failed to create temp file for file entries");
  }
}
#if 0
template <typename T>
bool ManifestWriter::addInfoEntry(const std::string &key, T value) {
  std::lock_guard<std::mutex> lock(writeMutex);
  infoEntries[key] = value;
  return true;
}
#endif

bool ManifestWriter::addFileEntry(const std::string &filePath,
                                  FileProperties &prop) {
  std::lock_guard<std::mutex> lock(writeMutex);
  if (!tempFile.is_open()) {
    return false;
  }

  // Start JSON object for this file entry
  tempFile << "{ \"filename\": \"" << filePath << "\", \"properties\": {";

  for (size_t i = 0; i < prop.size(); ++i) {
    const auto &[key, value] = prop[i];
    tempFile << "\"" << key << "\": ";

    std::visit(
        [&](auto &&val) {
          using T = std::decay_t<decltype(val)>;
          if constexpr (std::is_same_v<T, std::string>) {
            tempFile << "\"" << val << "\"";  // Quote strings
          } else if constexpr (std::is_integral_v<T>) {
            tempFile << val;  // Print integers as-is
          } else if constexpr (std::is_floating_point_v<T>) {
            tempFile << val;  // Print floating-point numbers as-is
          } else if constexpr (std::is_same_v<T, bool>) {
            tempFile << (val ? "true"
                             : "false");  // Print booleans as JSON true/false
          }
        },
        value);

    if (i < prop.size() - 1) {
      tempFile << ", ";
    }
  }

  tempFile << "} }\n";  // Close JSON object and write to file

  return true;
}

bool ManifestWriter::updateEntry(const std::string &key,
                                 const std::string &newValue) {
  std::lock_guard<std::mutex> lock(writeMutex);
  auto it = infoEntries.find(key);
  if (it != infoEntries.end()) {
    it->second = newValue;
    return true;
  }
  std::cerr << "Error: Entry not found for update" << std::endl;
  return false;
}

bool ManifestWriter::close() {
  std::lock_guard<std::mutex> lock(writeMutex);

  // Start writing final JSON
  rapidjson::OStreamWrapper osw(ofs);
  rapidjson::PrettyWriter<rapidjson::OStreamWrapper> writer(osw);

  writer.StartObject();

  // Write info entries
  for (const auto &entry : infoEntries) {
    writer.Key(entry.first.c_str());
    std::visit(
        [&](auto &&val) {
          if constexpr (std::is_same_v<std::decay_t<decltype(val)>,
                                       std::string>) {
            writer.String(val.c_str());
          } else if constexpr (std::is_integral_v<
                                   std::decay_t<decltype(val)>>) {
            writer.Uint64(val);
          } else if constexpr (std::is_floating_point_v<
                                   std::decay_t<decltype(val)>>) {
            writer.Double(val);
          } else if constexpr (std::is_same_v<std::decay_t<decltype(val)>,
                                              bool>) {
            writer.Bool(val);
          }
        },
        entry.second);
  }

  // Start file entries array
  writer.Key("files");
  writer.StartArray();

  // Stream file entries from tempfile
  tempFile.close();
  std::ifstream tempFileIn(tempFileName);
  std::string line;
  while (std::getline(tempFileIn, line)) {
    rapidjson::Document doc;
    doc.Parse(line.c_str());
    doc.Accept(writer);
  }
  tempFileIn.close();

  // Finish JSON
  writer.EndArray();
  writer.EndObject();

  ofs.close();
  std::remove(tempFileName.c_str());  // Delete tempfile

  return true;
}

#if 0
// Write to buffer variant
bool ManifestWriter::close() {
  std::lock_guard<std::mutex> lock(writeMutex);
  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
  writer.StartObject();

  for (const auto &entry : infoEntries) {
    writer.Key(entry.first.c_str());
    writer.String(entry.second.c_str());
  }

  writer.Key("files");
  writer.StartArray();
  for (const auto &fileEntry : fileEntries) {
    writer.StartObject();
    for (const auto &kv : fileEntry) {
      writer.Key(kv.first.c_str());
      writer.String(kv.second.c_str());
    }
    writer.EndObject();
  }
  writer.EndArray();
  writer.EndObject();

  ofs << buffer.GetString();
  ofs.flush();
  ofs.close();
  if (ofs.fail()) {
    std::cerr << "Error: Write failure when closing file ("
              << std::strerror(errno) << ")" << std::endl;
    return false;
  }
  return true;
}
#endif
// Example usage:
// try {
//     ManifestWriter writer("backup_manifest.json");
//     writer.addInfoEntry("backup_uuid", "123-abc");
//     writer.addFileEntry("/backup/data/file1.ibd");
//     writer.close();
// } catch (const std::exception& e) {
//     std::cerr << "Error: " << e.what() << std::endl;
// }
