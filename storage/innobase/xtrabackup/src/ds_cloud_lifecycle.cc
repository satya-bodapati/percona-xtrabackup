/******************************************************
Copyright (c) 2026 Percona LLC and/or its affiliates.

xtrabackup cloud lifecycle helpers. See ds_cloud_lifecycle.h.

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; version 2 of the License.

*******************************************************/

#include "ds_cloud_lifecycle.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include "common.h"
#include "ds_cloud.h"
#include "file_context.h"
#include "file_utils.h"
#include "msg.h"
#include "srv0srv.h"
#include "ut0log.h"
#include "utils.h"

#include "xbcloud/http.h"
#include "xbcloud/object_store.h"
#include "xbcloud/s3.h"
#include "xbcloud/azure.h"
#include "xbcloud/swift.h"

using namespace xbcloud;

/* Re-built per call: lifecycle ops are one-shot, no need to share with
   ds_cloud's Http_client. */
static std::unique_ptr<Object_store> build_store(Http_client *hc) {
  const auto &c = g_ds_cloud_config;
  if (c.storage == "s3" || c.storage == "gcs" || c.storage == "google") {
    s3_bucket_lookup_t lookup = LOOKUP_AUTO;
    if (c.bucket_lookup == "path") lookup = LOOKUP_PATH;
    else if (c.bucket_lookup == "dns") lookup = LOOKUP_DNS;
    std::string region_copy = c.region;
    auto store = std::make_unique<S3_object_store>(
        hc, region_copy, c.access_key, c.secret_key, c.session_token,
        c.storage_class, c.max_retries, c.max_backoff, c.endpoint, lookup,
        S3_V_AUTO);
    if (!store->probe_api_version_and_lookup(c.container)) return nullptr;
    return store;
  }
  if (c.storage == "azure") {
    return std::make_unique<Azure_object_store>(
        hc, c.azure_account, c.azure_access_key, false, c.storage_class,
        c.max_retries, c.max_backoff, c.azure_endpoint);
  }
  if (c.storage == "swift") {
    return std::make_unique<Swift_object_store>(hc, c.url, c.session_token,
                                                 c.max_retries, c.max_backoff);
  }
  return nullptr;
}

static Http_client *make_http_client() {
  auto *hc = new Http_client();
  hc->set_max_retries(g_ds_cloud_config.max_retries);
  hc->set_max_backoff(g_ds_cloud_config.max_backoff);
  hc->set_timeout(g_ds_cloud_config.timeout);
  if (g_ds_cloud_config.insecure) hc->set_insecure(true);
  if (!g_ds_cloud_config.cacert.empty())
    hc->set_cacaert(g_ds_cloud_config.cacert);
  return hc;
}

/* Create any missing parent directories under target_dir for a file
   that will land at target_dir + "/" + rel. */
static bool mkdir_for(const std::string &target_dir, const std::string &rel) {
  std::string full = target_dir;
  if (full.empty() || full.back() != '/') full.push_back('/');
  full.append(rel);
  size_t slash = full.find_last_of('/');
  if (slash == std::string::npos) return true;
  std::string dir = full.substr(0, slash);
  return mkdirp(dir.c_str(), 0755, MYF(0)) == 0;
}

/* Strip the prefix used as the backup name. The S3 list returns
   "<prefix>/<rel-path>"; we want just rel-path so the local layout
   under target-dir matches a "normal" backup. */
static std::string strip_prefix(const std::string &obj,
                                const std::string &prefix) {
  if (prefix.empty()) return obj;
  if (obj.rfind(prefix, 0) == 0) {
    size_t off = prefix.size();
    if (off < obj.size() && obj[off] == '/') ++off;
    return obj.substr(off);
  }
  return obj;
}

bool xb_cloud_download(const std::string &target_dir) {
  if (g_ds_cloud_config.storage.empty()) {
    xb::error() << "--download requires --cloud-storage to be set";
    return false;
  }
  if (g_ds_cloud_config.container.empty()) {
    xb::error() << "--download requires --cloud-bucket to be set";
    return false;
  }
  if (mkdirp(target_dir.c_str(), 0755, MYF(0)) < 0 && errno != EEXIST) {
    xb::error() << "--download: cannot create target dir " << target_dir
                << ": " << strerror(errno);
    return false;
  }

  std::unique_ptr<Http_client> hc(make_http_client());
  auto store = build_store(hc.get());
  if (!store) {
    xb::error() << "--download: store init failed";
    return false;
  }

  /* The backup prefix is in --target-dir (after the local path strip);
     for download we use the basename of target_dir as the prefix in the
     bucket. xtrabackup_target_dir is also passed -- caller must align. */
  std::string prefix = target_dir;
  while (!prefix.empty() && prefix.back() == '/') prefix.pop_back();
  size_t slash = prefix.find_last_of('/');
  if (slash != std::string::npos) prefix = prefix.substr(slash + 1);

  std::vector<std::string> objects;
  if (!store->list_objects_in_directory(g_ds_cloud_config.container, prefix,
                                         objects)) {
    xb::error() << "--download: list_objects_in_directory failed";
    return false;
  }
  if (objects.empty()) {
    xb::error() << "--download: no objects found under "
                << g_ds_cloud_config.container << "/" << prefix;
    return false;
  }
  xb::info() << "--download: " << objects.size() << " objects under "
             << g_ds_cloud_config.container << "/" << prefix;

  /* Locate backup_meta.json in the listing.  It MUST be present --
     the new-format manifest is mandatory and drives sparse-restore
     decisions.  We fetch it first, write it to target_dir, then load
     the lookup table before processing data files. */
  std::string manifest_obj;
  for (const auto &obj : objects) {
    if (strip_prefix(obj, prefix) == "backup_meta.json") {
      manifest_obj = obj;
      break;
    }
  }
  if (manifest_obj.empty()) {
    xb::error() << "--download: backup_meta.json missing from bucket under "
                << g_ds_cloud_config.container << "/" << prefix
                << "; refusing to restore (manifest is mandatory in "
                << "new-format backups)";
    return false;
  }
  {
    bool ok = false;
    Http_buffer body =
        store->download_object(g_ds_cloud_config.container, manifest_obj, ok);
    if (!ok) {
      xb::error() << "--download: failed to fetch " << manifest_obj;
      return false;
    }
    std::string full = target_dir;
    if (full.empty() || full.back() != '/') full.push_back('/');
    full.append("backup_meta.json");
    int fd = open(full.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
      xb::error() << "--download: cannot open " << full << ": "
                  << strerror(errno);
      return false;
    }
    if (write(fd, body.begin(), body.size()) !=
        static_cast<ssize_t>(body.size())) {
      xb::error() << "--download: short write to " << full;
      close(fd);
      return false;
    }
    close(fd);
    xb::info() << "--download: wrote " << full << " ("
               << xtrabackup::utils::human_readable(body.size()) << ")";
  }
  if (!file_context_load_manifest_from(target_dir.c_str())) {
    xb::error() << "--download: failed to parse backup_meta.json after "
                << "fetch; aborting restore";
    return false;
  }

  /* Now fetch every other object.  For each file:
     1. Download the body into memory.
     2. Write it to <full>.de-sparse (atomic-rename staging).
     3. If the manifest says this file has a sparse_map AND the
        filesystem supports PUNCH_HOLE, apply the manifest-driven
        hole punch on the .de-sparse copy.
     4. Rename .de-sparse -> final path.

     The rename pattern keeps a partial / failed download from
     leaving a half-written file with the canonical name, and
     guarantees that a successfully-named file has been punched
     (or determined to need no punching).  --prepare and
     --copy-back can therefore trust the layout they find. */
  for (const auto &obj : objects) {
    if (obj == manifest_obj) continue;  /* already fetched */

    bool ok = false;
    Http_buffer body =
        store->download_object(g_ds_cloud_config.container, obj, ok);
    if (!ok) {
      xb::error() << "--download: failed to fetch " << obj;
      return false;
    }
    std::string rel = strip_prefix(obj, prefix);
    if (!mkdir_for(target_dir, rel)) {
      xb::error() << "--download: mkdir_for failed for " << rel;
      return false;
    }
    std::string full = target_dir;
    if (full.empty() || full.back() != '/') full.push_back('/');
    full.append(rel);
    std::string staging = full + ".de-sparse";

    int fd = open(staging.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
      xb::error() << "--download: cannot open " << staging << ": "
                  << strerror(errno);
      return false;
    }
    if (write(fd, body.begin(), body.size()) !=
        static_cast<ssize_t>(body.size())) {
      xb::error() << "--download: short write to " << staging;
      close(fd);
      ::unlink(staging.c_str());
      return false;
    }
    close(fd);

    /* Manifest-driven punch_hole when applicable.  Returns true
       (no-op) on filesystems without PUNCH_HOLE support; file
       stays dense, only disk-space reclaim is lost. */
    const auto *regions = file_context_lookup_regions(rel.c_str());
    if (regions != nullptr) {
      uint64_t logical_size =
          file_context_lookup_logical_size(rel.c_str());
      if (!file_context_punch_holes_from_regions(staging.c_str(),
                                                  logical_size, *regions)) {
        xb::warn() << "--download: manifest-driven punch failed for " << rel
                   << " (continuing without sparse reclaim)";
      }
    }

    if (::rename(staging.c_str(), full.c_str()) != 0) {
      xb::error() << "--download: rename " << staging << " -> " << full
                  << " failed: " << strerror(errno);
      ::unlink(staging.c_str());
      return false;
    }
    xb::info() << "--download: wrote " << rel << " ("
               << xtrabackup::utils::human_readable(body.size()) << ")";
  }
  return true;
}

bool xb_cloud_delete(bool force) {
  if (g_ds_cloud_config.storage.empty()) {
    xb::error() << "--delete requires --cloud-storage to be set";
    return false;
  }
  if (g_ds_cloud_config.container.empty()) {
    xb::error() << "--delete requires --cloud-bucket to be set";
    return false;
  }

  std::unique_ptr<Http_client> hc(make_http_client());
  auto store = build_store(hc.get());
  if (!store) {
    xb::error() << "--delete: store init failed";
    return false;
  }

  /* Prefix = basename of target-dir, same as download. */
  extern char *xtrabackup_target_dir;
  std::string prefix = xtrabackup_target_dir;
  while (!prefix.empty() && prefix.back() == '/') prefix.pop_back();
  size_t slash = prefix.find_last_of('/');
  if (slash != std::string::npos) prefix = prefix.substr(slash + 1);

  std::vector<std::string> objects;
  if (!store->list_objects_in_directory(g_ds_cloud_config.container, prefix,
                                         objects)) {
    xb::error() << "--delete: list failed";
    return false;
  }
  if (objects.empty()) {
    xb::info() << "--delete: no objects to remove under "
               << g_ds_cloud_config.container << "/" << prefix;
    return true;
  }

  if (!force) {
    std::cerr << "About to delete " << objects.size() << " objects under "
              << g_ds_cloud_config.container << "/" << prefix
              << ". Type 'yes' to confirm: " << std::flush;
    std::string line;
    std::getline(std::cin, line);
    if (line != "yes") {
      xb::info() << "--delete: cancelled by user";
      return false;
    }
  }

  for (const auto &obj : objects) {
    if (!store->delete_object(g_ds_cloud_config.container, obj)) {
      xb::error() << "--delete: failed to delete " << obj;
      return false;
    }
  }
  xb::info() << "--delete: removed " << objects.size() << " objects";
  return true;
}
