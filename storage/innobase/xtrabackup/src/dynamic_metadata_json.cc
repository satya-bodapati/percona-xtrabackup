/******************************************************
Copyright (c) 2026 Percona LLC and/or its affiliates.

PXB-2865 JSON sidecar implementation. See dynamic_metadata_json.h.

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; version 2 of the License.
*******************************************************/

#include "dynamic_metadata_json.h"

#include <my_rapidjson_size_t.h>
#include <rapidjson/document.h>
#include <rapidjson/istreamwrapper.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include <cstdio>
#include <fstream>
#include <string>

#include <vector>

#include "log0recv.h"     /* MetadataRecover */
#include "dict0mem.h"     /* PersistentTableMetadata, corrupted_ids_t */
#include "dict0types.h"   /* index_id_t */
#include "xtrabackup.h"   /* xtrabackup_target_dir */
#include "msg.h"

namespace xb {
namespace dyn_meta {

namespace {

std::string sidecar_path() {
  std::string p(xtrabackup_target_dir != nullptr ? xtrabackup_target_dir : ".");
  p += "/";
  p += kSidecarFile;
  return p;
}

}  // namespace

bool load_into(MetadataRecover *mr) {
  if (mr == nullptr) return true;
  const std::string path = sidecar_path();

  std::ifstream in(path);
  if (!in.good()) {
    /* Absent file -> nothing to load, not an error. */
    return true;
  }

  rapidjson::IStreamWrapper isw(in);
  rapidjson::Document doc;
  doc.ParseStream(isw);
  if (doc.HasParseError()) {
    xb::warn() << "dynamic-metadata JSON sidecar parse error at " << path
               << " (offset " << doc.GetErrorOffset() << "); ignoring sidecar";
    return false;
  }
  if (!doc.IsObject() || !doc.HasMember("tables") ||
      !doc["tables"].IsObject()) {
    xb::warn() << "dynamic-metadata JSON sidecar has unexpected shape at "
               << path << "; ignoring";
    return false;
  }

  size_t loaded = 0;
  const auto &tables = doc["tables"];
  for (auto it = tables.MemberBegin(); it != tables.MemberEnd(); ++it) {
    table_id_t id = std::strtoull(it->name.GetString(), nullptr, 10);
    if (!it->value.IsObject()) continue;
    const auto &entry = it->value;
    uint64_t version =
        entry.HasMember("version") && entry["version"].IsUint64()
            ? entry["version"].GetUint64()
            : 0;
    uint64_t autoinc =
        entry.HasMember("autoinc") && entry["autoinc"].IsUint64()
            ? entry["autoinc"].GetUint64()
            : 0;
    /* corrupt_indexes: array of {space_id, index_id} objects. */
    std::vector<index_id_t> corrupt;
    if (entry.HasMember("corrupt_indexes") &&
        entry["corrupt_indexes"].IsArray()) {
      for (const auto &cobj : entry["corrupt_indexes"].GetArray()) {
        if (!cobj.IsObject()) continue;
        if (!cobj.HasMember("space_id") || !cobj["space_id"].IsUint64())
          continue;
        if (!cobj.HasMember("index_id") || !cobj["index_id"].IsUint64())
          continue;
        corrupt.emplace_back(
            static_cast<space_id_t>(cobj["space_id"].GetUint64()),
            static_cast<space_index_t>(cobj["index_id"].GetUint64()));
      }
    }
    mr->inject_metadata(id, version, autoinc, corrupt.data(), corrupt.size());
    ++loaded;
  }

  xb::info() << "dynamic-metadata JSON sidecar loaded " << loaded
             << " entries from " << path;
  return true;
}

bool save_from(const MetadataRecover *mr) {
  if (mr == nullptr) return true;

  /* First, load any existing sidecar so we can merge (latest version
  per table_id wins). We round-trip through a fresh in-memory map
  rather than re-loading into the live mr (which is about to be
  destroyed anyway). */
  struct Entry {
    uint64_t version;
    uint64_t autoinc;
    std::vector<index_id_t> corrupt;
  };
  std::map<table_id_t, Entry> merged;

  /* Read existing JSON if present. */
  {
    const std::string path = sidecar_path();
    std::ifstream in(path);
    if (in.good()) {
      rapidjson::IStreamWrapper isw(in);
      rapidjson::Document doc;
      doc.ParseStream(isw);
      if (!doc.HasParseError() && doc.IsObject() && doc.HasMember("tables") &&
          doc["tables"].IsObject()) {
        const auto &tables = doc["tables"];
        for (auto it = tables.MemberBegin(); it != tables.MemberEnd(); ++it) {
          table_id_t id = std::strtoull(it->name.GetString(), nullptr, 10);
          if (!it->value.IsObject()) continue;
          const auto &e = it->value;
          Entry r;
          r.version = (e.HasMember("version") && e["version"].IsUint64())
                          ? e["version"].GetUint64()
                          : 0;
          r.autoinc = (e.HasMember("autoinc") && e["autoinc"].IsUint64())
                          ? e["autoinc"].GetUint64()
                          : 0;
          if (e.HasMember("corrupt_indexes") &&
              e["corrupt_indexes"].IsArray()) {
            for (const auto &cobj : e["corrupt_indexes"].GetArray()) {
              if (!cobj.IsObject()) continue;
              if (!cobj.HasMember("space_id") || !cobj["space_id"].IsUint64())
                continue;
              if (!cobj.HasMember("index_id") || !cobj["index_id"].IsUint64())
                continue;
              r.corrupt.emplace_back(
                  static_cast<space_id_t>(cobj["space_id"].GetUint64()),
                  static_cast<space_index_t>(cobj["index_id"].GetUint64()));
            }
          }
          merged.emplace(id, std::move(r));
        }
      }
    }
  }

  /* Now merge in the in-memory metadata collected from this prepare's
  redo scan. Higher version wins. */
  size_t live = 0;
  mr->for_each_metadata(
      [&merged, &live](table_id_t id, const PersistentTableMetadata &pm) {
        Entry r;
        r.version = pm.get_version();
        r.autoinc = pm.get_autoinc();
        for (const auto &cid : pm.get_corrupted_indexes()) {
          r.corrupt.push_back(cid);
        }
        /* Merge using AutoIncPersister::aggregate semantics:
           - strictly newer version → replace.
           - same version           → keep larger autoinc, append corrupt.
           - older                  → drop new. */
        auto it = merged.find(id);
        if (it == merged.end()) {
          merged.emplace(id, std::move(r));
        } else if (r.version > it->second.version) {
          it->second = std::move(r);
        } else if (r.version == it->second.version) {
          if (r.autoinc > it->second.autoinc) {
            it->second.autoinc = r.autoinc;
          }
          for (auto &cid : r.corrupt) {
            it->second.corrupt.push_back(cid);
          }
        }
        ++live;
      });

  if (merged.empty()) {
    /* Nothing to persist; remove any stale file. */
    remove();
    return true;
  }

  /* Serialize merged map. */
  rapidjson::StringBuffer buf;
  rapidjson::Writer<rapidjson::StringBuffer> w(buf);
  w.StartObject();
  w.Key("schema_version");
  w.Uint(1);
  w.Key("tables");
  w.StartObject();
  for (const auto &kv : merged) {
    char id_str[32];
    std::snprintf(id_str, sizeof(id_str), "%llu",
                  (unsigned long long)kv.first);
    w.Key(id_str);
    w.StartObject();
    w.Key("version");
    w.Uint64(kv.second.version);
    w.Key("autoinc");
    w.Uint64(kv.second.autoinc);
    w.Key("corrupt_indexes");
    w.StartArray();
    for (const auto &cid : kv.second.corrupt) {
      w.StartObject();
      w.Key("space_id");
      w.Uint64(static_cast<uint64_t>(cid.m_space_id));
      w.Key("index_id");
      w.Uint64(static_cast<uint64_t>(cid.m_index_id));
      w.EndObject();
    }
    w.EndArray();
    w.EndObject();
  }
  w.EndObject();
  w.EndObject();

  /* Atomic-ish write: tmp + rename. */
  const std::string path = sidecar_path();
  const std::string tmp = path + ".tmp";
  {
    std::ofstream out(tmp, std::ios::trunc);
    if (!out.good()) {
      xb::warn() << "dynamic-metadata JSON sidecar: cannot open " << tmp
                 << " for writing";
      return false;
    }
    out.write(buf.GetString(), buf.GetSize());
    if (!out.good()) {
      xb::warn() << "dynamic-metadata JSON sidecar: write failed to " << tmp;
      return false;
    }
  }
  if (std::rename(tmp.c_str(), path.c_str()) != 0) {
    xb::warn() << "dynamic-metadata JSON sidecar: rename " << tmp << " -> "
               << path << " failed";
    return false;
  }

  xb::info() << "dynamic-metadata JSON sidecar wrote " << merged.size()
             << " entries (" << live << " from this prepare) to " << path;
  return true;
}

void remove() {
  const std::string path = sidecar_path();
  if (std::remove(path.c_str()) == 0) {
    xb::info() << "dynamic-metadata JSON sidecar removed: " << path;
  }
  /* If the file didn't exist, std::remove returns nonzero; that's fine
  and not worth logging. */
}

}  // namespace dyn_meta
}  // namespace xb
