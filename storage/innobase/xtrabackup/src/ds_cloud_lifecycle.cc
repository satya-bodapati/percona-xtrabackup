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
#include "file_utils.h"
#include "msg.h"

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
    msg_ts("--download requires --cloud-storage to be set\n");
    return false;
  }
  if (g_ds_cloud_config.container.empty()) {
    msg_ts("--download requires --cloud-bucket to be set\n");
    return false;
  }
  if (mkdirp(target_dir.c_str(), 0755, MYF(0)) < 0 && errno != EEXIST) {
    msg_ts("--download: cannot create target dir %s: %s\n",
           target_dir.c_str(), strerror(errno));
    return false;
  }

  std::unique_ptr<Http_client> hc(make_http_client());
  auto store = build_store(hc.get());
  if (!store) {
    msg_ts("--download: store init failed\n");
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
    msg_ts("--download: list_objects_in_directory failed\n");
    return false;
  }
  if (objects.empty()) {
    msg_ts("--download: no objects found under %s/%s\n",
           g_ds_cloud_config.container.c_str(), prefix.c_str());
    return false;
  }
  msg_ts("--download: %zu objects under %s/%s\n", objects.size(),
         g_ds_cloud_config.container.c_str(), prefix.c_str());

  for (const auto &obj : objects) {
    bool ok = false;
    Http_buffer body =
        store->download_object(g_ds_cloud_config.container, obj, ok);
    if (!ok) {
      msg_ts("--download: failed to fetch %s\n", obj.c_str());
      return false;
    }
    std::string rel = strip_prefix(obj, prefix);
    if (!mkdir_for(target_dir, rel)) {
      msg_ts("--download: mkdir_for failed for %s\n", rel.c_str());
      return false;
    }
    std::string full = target_dir;
    if (full.empty() || full.back() != '/') full.push_back('/');
    full.append(rel);
    int fd = open(full.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
      msg_ts("--download: cannot open %s: %s\n", full.c_str(),
             strerror(errno));
      return false;
    }
    ssize_t w = write(fd, body.begin(), body.size());
    if (w != static_cast<ssize_t>(body.size())) {
      msg_ts("--download: short write to %s\n", full.c_str());
      close(fd);
      return false;
    }
    close(fd);
    msg_ts("--download: wrote %s (%zu bytes)\n", full.c_str(), body.size());
  }
  return true;
}

bool xb_cloud_delete(bool force) {
  if (g_ds_cloud_config.storage.empty()) {
    msg_ts("--delete requires --cloud-storage to be set\n");
    return false;
  }
  if (g_ds_cloud_config.container.empty()) {
    msg_ts("--delete requires --cloud-bucket to be set\n");
    return false;
  }

  std::unique_ptr<Http_client> hc(make_http_client());
  auto store = build_store(hc.get());
  if (!store) {
    msg_ts("--delete: store init failed\n");
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
    msg_ts("--delete: list failed\n");
    return false;
  }
  if (objects.empty()) {
    msg_ts("--delete: no objects to remove under %s/%s\n",
           g_ds_cloud_config.container.c_str(), prefix.c_str());
    return true;
  }

  if (!force) {
    std::cerr << "About to delete " << objects.size() << " objects under "
              << g_ds_cloud_config.container << "/" << prefix
              << ". Type 'yes' to confirm: " << std::flush;
    std::string line;
    std::getline(std::cin, line);
    if (line != "yes") {
      msg_ts("--delete: cancelled by user\n");
      return false;
    }
  }

  for (const auto &obj : objects) {
    if (!store->delete_object(g_ds_cloud_config.container, obj)) {
      msg_ts("--delete: failed to delete %s\n", obj.c_str());
      return false;
    }
  }
  msg_ts("--delete: removed %zu objects\n", objects.size());
  return true;
}
