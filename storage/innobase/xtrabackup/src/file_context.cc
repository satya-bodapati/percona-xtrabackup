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
#include <rapidjson/istreamwrapper.h>
#include <rapidjson/prettywriter.h>
#include <rapidjson/stringbuffer.h>

#include <fcntl.h>
#include <unistd.h>

#include <fstream>
#include <memory>
#include <string>
#include <unordered_map>

#ifdef HAVE_FALLOC_PUNCH_HOLE_AND_KEEP_SIZE
#include <linux/falloc.h>
#include <sys/types.h>
#include <sys/stat.h>
#endif

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

/* ------------------------------------------------------------------
   Restore-side lookup: load backup_meta.json once, answer queries
   by relative path. Independent from the backup-time registry above
   to avoid coupling the two ownership models.
   ------------------------------------------------------------------ */

namespace {

struct RestoreEntry {
  uint64_t logical_size = 0;
  std::vector<file_region_t> regions;
};

struct RestoreLookup {
  std::mutex mu;
  std::string loaded_from_dir;  /* empty => not loaded */
  bool ok = false;
  std::unordered_map<std::string, RestoreEntry> by_path;
};

RestoreLookup &restore_lookup() {
  static RestoreLookup l;
  return l;
}

}  // namespace

bool file_context_load_manifest_from(const char *target_dir) {
  if (target_dir == nullptr) return false;

  auto &l = restore_lookup();
  std::lock_guard<std::mutex> lock(l.mu);

  /* Idempotent if called again for the same dir. */
  if (l.loaded_from_dir == target_dir) return l.ok;

  l.loaded_from_dir = target_dir;
  l.ok = false;
  l.by_path.clear();

  std::string manifest_path(target_dir);
  if (!manifest_path.empty() && manifest_path.back() != '/') {
    manifest_path.push_back('/');
  }
  manifest_path.append("backup_meta.json");

  std::ifstream in(manifest_path);
  if (!in.is_open()) return false;

  rapidjson::IStreamWrapper isw(in);
  rapidjson::Document doc;
  doc.ParseStream(isw);
  if (doc.HasParseError() || !doc.IsObject()) return false;
  if (!doc.HasMember("files") || !doc["files"].IsArray()) return false;

  for (auto &entry : doc["files"].GetArray()) {
    if (!entry.IsObject()) continue;
    if (!entry.HasMember("name") || !entry["name"].IsString()) continue;
    const char *name = entry["name"].GetString();

    RestoreEntry re;
    if (entry.HasMember("logical_size") && entry["logical_size"].IsUint64()) {
      re.logical_size = entry["logical_size"].GetUint64();
    }
    if (entry.HasMember("sparse_map") && entry["sparse_map"].IsArray()) {
      for (auto &r : entry["sparse_map"].GetArray()) {
        if (!r.IsObject()) continue;
        if (!r.HasMember("offset") || !r.HasMember("length")) continue;
        re.regions.push_back(
            {r["offset"].GetUint64(), r["length"].GetUint64()});
      }
    }
    l.by_path.emplace(std::string(name), std::move(re));
  }

  l.ok = true;
  return true;
}

const std::vector<file_region_t> *file_context_lookup_regions(const char *path) {
  if (path == nullptr) return nullptr;
  auto &l = restore_lookup();
  std::lock_guard<std::mutex> lock(l.mu);
  if (!l.ok) return nullptr;
  auto it = l.by_path.find(path);
  if (it == l.by_path.end()) return nullptr;
  if (it->second.regions.empty()) return nullptr;  /* dense; no holes */
  return &it->second.regions;
}

uint64_t file_context_lookup_logical_size(const char *path) {
  if (path == nullptr) return 0;
  auto &l = restore_lookup();
  std::lock_guard<std::mutex> lock(l.mu);
  if (!l.ok) return 0;
  auto it = l.by_path.find(path);
  if (it == l.by_path.end()) return 0;
  return it->second.logical_size;
}

bool file_context_punch_holes_from_regions(
    const char *file_path, uint64_t logical_size,
    const std::vector<file_region_t> &regions) {
  if (file_path == nullptr) return false;

#ifndef HAVE_FALLOC_PUNCH_HOLE_AND_KEEP_SIZE
  /* No FS support for punch_hole.  Treat as success: the file stays
     dense on disk, which is functionally correct; only disk-space
     reclaim is lost. */
  (void)logical_size;
  (void)regions;
  return true;
#else
  int fd = ::open(file_path, O_WRONLY);
  if (fd < 0) return false;

  /* Walk consecutive regions and punch the gaps between them.  Also
     punch the trailing tail from the last region's end to
     logical_size, if any.  Regions are stored in file-offset order
     by the backup-time write loop, but we don't assume sorted to be
     safe -- the manifest is user-readable and could in principle be
     rewritten; not worth a sort here, but we tolerate disorder by
     requiring gap > 0. */
  uint64_t cursor = 0;
  bool any_err = false;
  for (const auto &r : regions) {
    if (r.offset > cursor) {
      const uint64_t gap = r.offset - cursor;
      if (::fallocate(fd,
                      FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE,
                      static_cast<off_t>(cursor),
                      static_cast<off_t>(gap)) != 0) {
        any_err = true;
        break;
      }
    }
    /* If regions are not sorted or overlap, just advance the cursor
       monotonically -- we never want to walk backwards. */
    if (r.offset + r.length > cursor) cursor = r.offset + r.length;
  }
  if (!any_err && logical_size > cursor) {
    const uint64_t tail = logical_size - cursor;
    if (::fallocate(fd,
                    FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE,
                    static_cast<off_t>(cursor),
                    static_cast<off_t>(tail)) != 0) {
      any_err = true;
    }
  }
  ::close(fd);
  return !any_err;
#endif
}
