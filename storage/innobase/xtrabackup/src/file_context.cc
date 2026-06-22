/******************************************************
Copyright (c) 2026 Percona LLC and/or its affiliates.

backup_meta.json manifest framework -- implementation.

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; version 2 of the License.

*******************************************************/

#include "file_context.h"

#include <my_rapidjson_size_t.h>
#include <rapidjson/document.h>
#include <rapidjson/prettywriter.h>
#include <rapidjson/stringbuffer.h>

#include <memory>

namespace {

/* Global manifest registry. One per xtrabackup invocation. Lives
   here as a function-local static to dodge static-init-order
   surprises across translation units. The registry owns every
   FileContext created via file_context_create(); finalized entries
   end up in the JSON, non-finalized ones are silently dropped (a
   conservative default for failed copies). */
struct ManifestRegistry {
  std::mutex mu;
  std::vector<std::unique_ptr<FileContext>> all;
  std::vector<FileContext *> finalized;
};

ManifestRegistry &registry() {
  static ManifestRegistry r;
  return r;
}

}  // namespace

FileContext *file_context_create(const char *path) {
  if (path == nullptr) return nullptr;
  auto fc = std::make_unique<FileContext>(path);
  FileContext *bare = fc.get();
  {
    auto &r = registry();
    std::lock_guard<std::mutex> lock(r.mu);
    r.all.push_back(std::move(fc));
  }
  return bare;
}

FileContext *file_context_create(const std::string &path) {
  return file_context_create(path.c_str());
}

void file_context_finalize(FileContext *fc) {
  if (fc == nullptr) return;
  auto &r = registry();
  std::lock_guard<std::mutex> lock(r.mu);
  /* Idempotent: only push if not already in finalized. The list is
     short and bounded by the number of files in the backup, so a
     linear scan is fine; this runs once per file close. */
  for (auto *seen : r.finalized) {
    if (seen == fc) return;
  }
  r.finalized.push_back(fc);
}

bool file_context_build_manifest(std::string &out) {
  rapidjson::Document doc;
  doc.SetObject();
  auto &alloc = doc.GetAllocator();

  doc.AddMember("version", 1, alloc);

  rapidjson::Value files(rapidjson::kArrayType);
  {
    auto &r = registry();
    std::lock_guard<std::mutex> lock(r.mu);
    for (FileContext *fc : r.finalized) {
      rapidjson::Value entry(rapidjson::kObjectType);

      entry.AddMember("name",
                      rapidjson::Value(fc->path.c_str(),
                                       static_cast<rapidjson::SizeType>(
                                           fc->path.size()),
                                       alloc),
                      alloc);
      if (fc->logical_size != 0) {
        entry.AddMember("logical_size", fc->logical_size, alloc);
      }
      if (!fc->regions.empty()) {
        rapidjson::Value regions(rapidjson::kArrayType);
        for (const auto &r_ent : fc->regions) {
          rapidjson::Value reg(rapidjson::kObjectType);
          reg.AddMember("offset", r_ent.offset, alloc);
          reg.AddMember("length", r_ent.length, alloc);
          regions.PushBack(reg, alloc);
        }
        entry.AddMember("sparse_map", regions, alloc);
      }

      files.PushBack(entry, alloc);
    }
  }
  doc.AddMember("files", files, alloc);

  rapidjson::StringBuffer buf;
  rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(buf);
  doc.Accept(writer);

  out.assign(buf.GetString(), buf.GetSize());
  return true;
}

void file_context_registry_clear() {
  auto &r = registry();
  std::lock_guard<std::mutex> lock(r.mu);
  r.finalized.clear();
  r.all.clear();
}
