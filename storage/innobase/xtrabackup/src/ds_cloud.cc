/******************************************************
Copyright (c) 2026 Percona LLC and/or its affiliates.

ds_cloud: xtrabackup datasink that uploads each file directly to an
S3 / Azure / Swift / GCS bucket via the shared xbcloud multipart
machinery. See ds_cloud.h for the configuration surface.

Each open() spins up one Multipart_uploader for the file. write() feeds
bytes into a per-file accumulator; when the accumulator crosses the
dynamic_part_size threshold, a part is flushed asynchronously through
the shared Event_handler. close() flushes the remainder + commits the
multipart upload. Small files (entire stream <= --cloud-multipart-
threshold) take a single-PUT fast path; the Multipart_uploader's lazy
Init means no InitiateMultipartUpload / Abort round-trips happen for
those.

A single Http_client, Object_store, and Event_handler thread are
shared across all files in the ds_cloud ctxt -- ds_init() spins them
up, ds_deinit() tears them down.

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; version 2 of the License.

*******************************************************/

#include "ds_cloud.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <mysql/service_mysql_alloc.h>
#include <my_sys.h>

#include "common.h"
#include "datasink.h"
#include "file_utils.h"
#include "msg.h"
#include "srv0srv.h"
#include "ut0log.h"
#include "utils.h"

#include "xbcloud/http.h"
#include "xbcloud/multipart.h"
#include "xbcloud/object_store.h"
#include "xbcloud/s3.h"
#include "xbcloud/azure.h"
#include "xbcloud/swift.h"

#include "xb_files_jsonl.h"
#include <my_rapidjson_size_t.h>
#include <rapidjson/document.h>

#include <algorithm>
#include <unordered_map>

using namespace xbcloud;

ds_cloud_config_t g_ds_cloud_config;

namespace {

/* One hole recorded in backup_files.jsonl's per-file sparse_map array:
{offset, length} are absolute positions in the LOGICAL (unpacked) file
where the upload skipped zeroes. */
struct sparse_hole_t {
  uint64_t offset;
  uint64_t length;
};

struct segment_ref_t {
  std::string path;   /* backup-root-relative, e.g. "test/big.ibd.r1" */
  uint64_t size;
};

struct file_index_entry_t {
  std::vector<sparse_hole_t> holes;
  std::vector<segment_ref_t> segments;
};

using file_index_t = std::unordered_map<std::string, file_index_entry_t>;

/* Parse a backup_files.jsonl byte buffer into a per-path index that
captures sparse_map (per-file holes) and segments (rollover splits).
Lines with neither field still get an empty entry so the caller can
distinguish "file declared in manifest" from "missing".  The header
line {"manifest_version":1} carries no "path" field and is skipped
silently. */
file_index_t parse_files_index(const char *data, size_t size) {
  file_index_t out;
  size_t i = 0;
  while (i < size) {
    /* Find end of the JSONL line. */
    size_t j = i;
    while (j < size && data[j] != '\n') ++j;
    const size_t line_len = j - i;
    if (line_len == 0) {
      i = j + 1;
      continue;
    }
    rapidjson::Document doc;
    doc.Parse(data + i, line_len);
    i = j + 1;
    if (doc.HasParseError() || !doc.IsObject()) continue;
    if (!doc.HasMember("path") || !doc["path"].IsString()) continue;
    std::string path = doc["path"].GetString();
    file_index_entry_t entry;
    if (doc.HasMember("sparse_map") && doc["sparse_map"].IsArray()) {
      const auto &arr = doc["sparse_map"];
      entry.holes.reserve(arr.Size());
      for (auto it = arr.Begin(); it != arr.End(); ++it) {
        if (!it->IsObject()) continue;
        if (!it->HasMember("offset") || !it->HasMember("length")) continue;
        const auto &off = (*it)["offset"];
        const auto &len = (*it)["length"];
        if (!off.IsUint64() || !len.IsUint64()) continue;
        entry.holes.push_back({off.GetUint64(), len.GetUint64()});
      }
      std::sort(entry.holes.begin(), entry.holes.end(),
                [](const sparse_hole_t &a, const sparse_hole_t &b) {
                  return a.offset < b.offset;
                });
    }
    if (doc.HasMember("segments") && doc["segments"].IsArray()) {
      const auto &arr = doc["segments"];
      entry.segments.reserve(arr.Size());
      for (auto it = arr.Begin(); it != arr.End(); ++it) {
        if (!it->IsObject()) continue;
        if (!it->HasMember("path") || !(*it)["path"].IsString()) continue;
        if (!it->HasMember("size") || !(*it)["size"].IsUint64()) continue;
        entry.segments.push_back(
            {(*it)["path"].GetString(), (*it)["size"].GetUint64()});
      }
    }
    out.emplace(std::move(path), std::move(entry));
  }
  return out;
}

/* Write the dense cloud body into @p fd at the logical offsets implied
by @p holes (recorded during upload).  When the lookup has no holes for
this file, this is a single pwrite covering the whole body.

The cloud object is the packed (hole-excluded) representation, so we
expand on the fly: walk the holes sorted by offset, write each data
run from the body between consecutive holes, leave the hole ranges
unwritten (ftruncate already produced an all-zero / sparse file), and
finally fallocate(PUNCH_HOLE) each hole so the file system records
them as real holes on disk.

Returns true on success, false on I/O error. */
bool restore_sparse_file(int fd, const char *body, size_t body_size,
                         const std::vector<sparse_hole_t> &holes) {
  uint64_t total_holes = 0;
  for (const auto &h : holes) total_holes += h.length;
  const uint64_t logical_size = body_size + total_holes;

  if (::ftruncate(fd, (off_t)logical_size) != 0) {
    xb::error() << "--download: ftruncate(" << logical_size
                << ") failed: " << strerror(errno);
    return false;
  }

  uint64_t src = 0;          /* byte offset in body */
  uint64_t dst = 0;          /* logical offset in target */
  for (const auto &h : holes) {
    if (h.offset < dst) {
      xb::error() << "--download: sparse_map not sorted / overlaps at "
                  << h.offset;
      return false;
    }
    const uint64_t run = h.offset - dst;
    if (run > 0) {
      if (src + run > body_size) {
        xb::error() << "--download: sparse_map run exceeds body size";
        return false;
      }
      ssize_t n = ::pwrite(fd, body + src, run, (off_t)dst);
      if (n != (ssize_t)run) {
        xb::error() << "--download: pwrite data run failed: "
                    << strerror(errno);
        return false;
      }
      src += run;
      dst += run;
    }
    dst += h.length;
  }
  /* Tail data after the last hole. */
  if (src < body_size) {
    const uint64_t tail = body_size - src;
    ssize_t n = ::pwrite(fd, body + src, tail, (off_t)dst);
    if (n != (ssize_t)tail) {
      xb::error() << "--download: pwrite tail failed: " << strerror(errno);
      return false;
    }
    src += tail;
    dst += tail;
  }
  if (src != body_size || dst != logical_size) {
    xb::error() << "--download: sparse expansion mismatch: src=" << src
                << "/" << body_size << " dst=" << dst << "/" << logical_size;
    return false;
  }

#ifdef HAVE_FALLOC_PUNCH_HOLE_AND_KEEP_SIZE
  for (const auto &h : holes) {
    if (::fallocate(fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE,
                    (off_t)h.offset, (off_t)h.length) != 0) {
      /* Not fatal: ftruncate already left the range unwritten so the
      file is logically equivalent; we just may use a bit more disk on
      filesystems that don't lazily keep ranges sparse.  Log and
      continue. */
      xb::info() << "--download: fallocate(PUNCH_HOLE) at " << h.offset
                 << "+" << h.length << " failed (non-fatal): "
                 << strerror(errno);
    }
  }
#endif
  return true;
}



/* Per-datasink (ctxt) state. Owns the long-lived HTTP / event-loop /
   object-store handles. One instance per ds_create(DS_TYPE_CLOUD). */
struct ds_cloud_ctxt_t {
  std::unique_ptr<Http_client> http_client;
  std::unique_ptr<Object_store> object_store;
  std::unique_ptr<Event_handler> event_handler;
  std::thread event_thread;
  std::string container;     /* bucket name */
  std::string backup_prefix; /* root passed to ds_init -- prefix within bucket */
  std::atomic<unsigned long long> bytes_written{0};
  /* Set true by any async small-file PUT callback that observed a
     failure. Because the async path returns from cloud_close before the
     PUT completes, ds_close cannot surface those errors directly; we
     accumulate them here and the libev thread drain at deinit logs +
     reports them. Same pattern xbcloud's put_func uses with its
     has_errors atomic. */
  std::atomic<bool> async_upload_failed{false};
};

/* Per-file state. file->ptr points here.
   Stream_multipart_writer encapsulates the part_buf + flush loop +
   small-file fast path; we just feed it bytes. */
struct ds_cloud_file_t {
  std::string object_name;                 /* "<backup_prefix>/<path>" for
                                              non-segmented files; for
                                              segmented files this holds
                                              the CURRENT segment's full
                                              object key (".rN"-suffixed). */
  std::unique_ptr<Stream_multipart_writer> writer;
  ds_cloud_ctxt_t *ctxt{nullptr};
  void *file_ctx{nullptr};                 /* backup_files.jsonl Document */

  /* Rollover state (only meaningful when n_segments > 1).

  When the per-file stat->st_size is known at cloud_open and exceeds
  --cloud-multipart-rollover-threshold, we pre-compute how many
  segments the file will be split into.  Segment i (1-based) is the
  byte-range [(i-1)*threshold, min(i*threshold, st_size)) in the
  logical file, uploaded as "<backup_prefix>/<rel_path>.r<i>".

  cloud_write splits the incoming buffer at the segment boundary, so
  each segment object holds exactly threshold bytes (last one ≤).
  The parent file's backup_files.jsonl entry gets a `segments` array
  via xb_files_jsonl::add_segment for restoration. */
  uint64_t threshold{0};
  uint64_t bytes_in_segment{0};
  uint64_t n_segments{1};
  uint64_t current_segment{0};             /* 0 if no rollover, else 1.. */
  std::string base_object;                 /* "<backup_prefix>/<rel_path>" */
  std::string base_path;                   /* caller-visible relative path */
  uint64_t seg_part_size{0};               /* part_size for next segment */
  uint64_t seg_per_writer_budget{0};       /* budget for next segment */
};

/* Wire the async-small-file PUT uploader callback on a freshly-built
Stream_multipart_writer.  Same wiring whether the writer is the first
segment or a rolled-over follow-on segment, so factor it out. */
void install_async_small_file_uploader(Stream_multipart_writer *writer,
                                       ds_cloud_ctxt_t *cc) {
  auto *cc_ptr = cc;
  auto *store = cc->object_store.get();
  auto *event_handler = cc->event_handler.get();
  const std::string &container = cc->container;
  writer->set_async_small_file_uploader(
      [cc_ptr, store, event_handler, container](
          const std::string &name, const Http_buffer &body) -> bool {
        size_t length = body.size();
        return store->async_upload_object(
            container, name, body, event_handler,
            [cc_ptr, name, length](bool ok, const Http_buffer &) {
              if (ok) {
                xb::info() << "ds_cloud: small-file PUT done: " << name
                           << " (" << xtrabackup::utils::human_readable(length)
                           << ")";
              } else {
                xb::error() << "ds_cloud: small-file PUT FAILED: " << name
                            << " (" << xtrabackup::utils::human_readable(length)
                            << ")";
                cc_ptr->async_upload_failed.store(true,
                                                  std::memory_order_relaxed);
              }
            });
      });
}

/* Build a Stream_multipart_writer for one segment / one object key.
Returns the writer fully wired (async small-file uploader installed,
display_name set). */
std::unique_ptr<Stream_multipart_writer> make_segment_writer(
    ds_cloud_ctxt_t *cc, const std::string &object_name,
    const std::string &display_name, uint64_t part_size,
    uint64_t per_writer_budget) {
  auto w = std::make_unique<Stream_multipart_writer>(
      cc->object_store.get(), cc->container, object_name,
      cc->event_handler.get(), per_writer_budget,
      static_cast<size_t>(g_ds_cloud_config.multipart_threshold),
      static_cast<size_t>(part_size));
  w->set_display_name(display_name);
  install_async_small_file_uploader(w.get(), cc);
  return w;
}

/* ----- Shared helpers (used by both backup-time cloud_init and the
         lifecycle CLI commands xb_cloud_download/delete) ----- */

/* Build an Http_client configured from g_ds_cloud_config (retries,
   backoff, timeout, TLS).  Caller takes ownership. */
std::unique_ptr<Http_client> make_cloud_http_client() {
  auto hc = std::make_unique<Http_client>();
  hc->set_max_retries(g_ds_cloud_config.max_retries);
  hc->set_max_backoff(g_ds_cloud_config.max_backoff);
  hc->set_timeout(g_ds_cloud_config.timeout);
  if (g_ds_cloud_config.insecure) hc->set_insecure(true);
  if (!g_ds_cloud_config.cacert.empty()) {
    hc->set_cacaert(g_ds_cloud_config.cacert);
  }
  /* xbcloud parity wiring (PXB-3671 commit 2). */
  if (g_ds_cloud_config.verbose) hc->set_verbose(true);
  for (long code : g_ds_cloud_config.curl_retriable_errors) {
    hc->set_curl_retriable_errors(static_cast<CURLcode>(code));
  }
  for (long code : g_ds_cloud_config.http_retriable_errors) {
    hc->set_http_retriable_errors(code);
  }
  return hc;
}

/* Build an Object_store from g_ds_cloud_config + the supplied
   Http_client.  Does NOT probe (see probe_object_store below);
   callers run the probe at the point that makes sense for them. */
std::unique_ptr<Object_store> build_object_store(Http_client *http_client) {
  const auto &c = g_ds_cloud_config;
  if (c.storage == "s3" || c.storage == "gcs" || c.storage == "google") {
    s3_bucket_lookup_t lookup = LOOKUP_AUTO;
    if (c.bucket_lookup == "path") lookup = LOOKUP_PATH;
    else if (c.bucket_lookup == "dns") lookup = LOOKUP_DNS;

    /* --cloud-s3-api-version: 0=AUTO 1=v2 2=v4 (matches the typelib
       order in cloud_s3_api_version_names[] / xbcloud's identical
       option). */
    s3_api_version_t api_v = S3_V_AUTO;
    if (c.s3_api_version == 1) api_v = S3_V2;
    else if (c.s3_api_version == 2) api_v = S3_V4;

    std::string region_copy = c.region;
    auto store = std::make_unique<S3_object_store>(
        http_client, region_copy, c.access_key, c.secret_key,
        c.session_token, c.storage_class, c.max_retries, c.max_backoff,
        c.endpoint, lookup, api_v);
    if (!c.extra_http_headers.empty()) {
      store->set_extra_http_headers(c.extra_http_headers);
    }
    /* probe will set version + lookup on success */
    return store;
  }
  if (c.storage == "azure") {
    auto store = std::make_unique<Azure_object_store>(
        http_client, c.azure_account, c.azure_access_key,
        c.azure_development_storage, c.storage_class, c.max_retries,
        c.max_backoff, c.azure_endpoint);
    if (!c.extra_http_headers.empty()) {
      store->set_extra_http_headers(c.extra_http_headers);
    }
    return store;
  }
  if (c.storage == "swift") {
    /* Swift Keystone auth (PXB-3671 commit 3).  Mirrors xbcloud's flow:
       normalize the auth URL to ".../v<N>/", run the temp_auth / v2 /
       v3 dance, then construct the Swift_object_store from the returned
       (storage_url, token).  --cloud-swift-storage-url overrides the
       URL returned by Keystone (useful when the auth response points
       to an internal endpoint). */
    if (c.swift_auth_url.empty()) {
      xb::error()
          << "ds_cloud: --cloud-swift-auth-url is required when "
             "--cloud-storage=swift";
      return nullptr;
    }
    std::string auth_url = c.swift_auth_url;
    if (auth_url.back() != '/') auth_url.push_back('/');

    /* If the URL already carries /v<N>/ at the end, take that as the
       auth version.  Otherwise append /v<auth_version>/.  Default is
       v1 (TempAuth). */
    std::string auth_version = c.swift_auth_version;
    const char *valid_versions[] = {"/v1/",  "/v2/",   "/v3/",
                                    "/v1.0/", "/v2.0/", "/v3.0/"};
    bool versioned_url = false;
    for (const char *v : valid_versions) {
      size_t vl = strlen(v);
      if (auth_url.size() >= vl &&
          auth_url.compare(auth_url.size() - vl, vl, v) == 0) {
        /* extract the version digit between '/v' and '/' (or '.') */
        auth_version.assign(v + 2, strlen(v + 2) - 1);
        versioned_url = true;
        break;
      }
    }
    if (!versioned_url) {
      if (auth_version.empty()) auth_version = "1.0";
      auth_url.append("v").append(auth_version).append("/");
    }

    Keystone_client keystone(http_client, auth_url);
    if (!c.swift_key.empty()) keystone.set_key(c.swift_key);
    if (!c.swift_user.empty()) keystone.set_user(c.swift_user);
    if (!c.swift_password.empty()) keystone.set_password(c.swift_password);
    if (!c.swift_tenant.empty()) keystone.set_tenant(c.swift_tenant);
    if (!c.swift_tenant_id.empty()) keystone.set_tenant_id(c.swift_tenant_id);
    if (!c.swift_domain.empty()) keystone.set_domain(c.swift_domain);
    if (!c.swift_domain_id.empty()) keystone.set_domain_id(c.swift_domain_id);
    if (!c.swift_project_domain.empty())
      keystone.set_project_domain(c.swift_project_domain);
    if (!c.swift_project_domain_id.empty())
      keystone.set_project_domain_id(c.swift_project_domain_id);
    if (!c.swift_project.empty()) keystone.set_project(c.swift_project);
    if (!c.swift_project_id.empty())
      keystone.set_project_id(c.swift_project_id);

    Keystone_client::auth_info_t auth_info;
    bool ok = false;
    if (auth_version.empty() || auth_version[0] == '1') {
      ok = keystone.temp_auth(auth_info);
    } else if (auth_version[0] == '2') {
      ok = keystone.auth_v2(c.swift_region, auth_info);
    } else if (auth_version[0] == '3') {
      ok = keystone.auth_v3(c.swift_region, auth_info);
    } else {
      xb::error() << "ds_cloud: unsupported --cloud-swift-auth-version '"
                  << auth_version << "'";
      return nullptr;
    }
    if (!ok) {
      xb::error() << "ds_cloud: Keystone authentication failed (version "
                  << auth_version << ")";
      return nullptr;
    }

    if (!c.swift_storage_url.empty()) {
      auth_info.url = c.swift_storage_url;
    }
    xb::info() << "ds_cloud: Swift object-store URL " << auth_info.url;

    return std::make_unique<Swift_object_store>(http_client, auth_info.url,
                                                auth_info.token, c.max_retries,
                                                c.max_backoff);
  }
  return nullptr;
}

/* Probe the store: for S3/GCS the SDK's probe_api_version_and_lookup
   adjusts the api version / bucket-lookup based on what the server
   responds with; for Azure/Swift a container_exists check is enough.
   Returns true iff the bucket is reachable with the configured
   credentials. */
bool probe_object_store(Object_store *store, const std::string &container) {
  if (g_ds_cloud_config.storage == "s3" ||
      g_ds_cloud_config.storage == "gcs" ||
      g_ds_cloud_config.storage == "google") {
    auto *s3_store = static_cast<S3_object_store *>(store);
    if (!s3_store->probe_api_version_and_lookup(container)) {
      xb::error() << "ds_cloud: probe failed for bucket '" << container << "'";
      return false;
    }
  } else {
    bool exists;
    if (!store->container_exists(container, exists)) {
      xb::error() << "ds_cloud: container_exists check failed for '"
                  << container << "'";
      return false;
    }
    if (!exists) {
      xb::error() << "ds_cloud: container '" << container
                  << "' does not exist";
      return false;
    }
  }
  return true;
}

/* Strip the bucket-prefix from a returned object key.  Object listings
   return "<prefix>/<rel-path>"; lifecycle CLI commands write to disk
   at "<target_dir>/<rel-path>", so they need the rel-path part. */
std::string strip_prefix(const std::string &obj, const std::string &prefix) {
  if (prefix.empty()) return obj;
  if (obj.rfind(prefix, 0) == 0) {
    size_t off = prefix.size();
    if (off < obj.size() && obj[off] == '/') ++off;
    return obj.substr(off);
  }
  return obj;
}

/* Create any missing parent directories under target_dir for a file
   landing at target_dir + "/" + rel.  Used by xb_cloud_download. */
bool mkdir_for(const std::string &target_dir, const std::string &rel) {
  std::string full = target_dir;
  if (full.empty() || full.back() != '/') full.push_back('/');
  full.append(rel);
  size_t slash = full.find_last_of('/');
  if (slash == std::string::npos) return true;
  std::string dir = full.substr(0, slash);
  return mkdirp(dir.c_str(), 0755, MYF(0)) == 0;
}

/* ----- datasink ops ----- */

ds_ctxt_t *cloud_init(const char *root) {
  if (g_ds_cloud_config.storage.empty()) {
    xb::error() << "ds_cloud: --cloud-storage is not set; cannot initialize";
    return nullptr;
  }
  if (g_ds_cloud_config.container.empty()) {
    xb::error() << "ds_cloud: bucket / container is not set (use one of "
                   "--cloud-s3-bucket / --cloud-google-bucket / "
                   "--cloud-azure-container-name / --cloud-swift-container "
                   "matching --cloud-storage)";
    return nullptr;
  }

  auto cc = std::make_unique<ds_cloud_ctxt_t>();
  cc->container = g_ds_cloud_config.container;
  /* Prefix WITHIN the bucket: prefer the explicit one parsed from the
     provider-explicit bucket option (BUCKET/PREFIX form), and fall back
     to whatever ds_create's caller passed as `root`.  In practice the
     ds_cloud ds_create call passes g_ds_cloud_config.prefix.c_str() so
     these are the same string; the fallback keeps the API contract
     intact for any future internal caller. */
  cc->backup_prefix = (root != nullptr ? root : "");
  if (cc->backup_prefix.empty() && !g_ds_cloud_config.prefix.empty()) {
    cc->backup_prefix = g_ds_cloud_config.prefix;
  }

  cc->http_client = make_cloud_http_client();
  cc->object_store = build_object_store(cc->http_client.get());
  if (!cc->object_store) {
    xb::error() << "ds_cloud: unknown storage backend '"
                << g_ds_cloud_config.storage << "'";
    return nullptr;
  }

  if (!probe_object_store(cc->object_store.get(), cc->container)) {
    return nullptr;
  }

  cc->event_handler = std::make_unique<Event_handler>(
      g_ds_cloud_config.max_concurrent_requests > 0
          ? g_ds_cloud_config.max_concurrent_requests
          : 1);
  if (!cc->event_handler->init()) {
    xb::error() << "ds_cloud: Event_handler init failed";
    return nullptr;
  }
  if (g_ds_cloud_config.rate_log_interval > 0) {
    cc->event_handler->install_rate_logger(
        static_cast<double>(g_ds_cloud_config.rate_log_interval));
  }
  cc->event_thread = cc->event_handler->run();

  /* Caller (xtrabackup) frees with my_free; allocate ds_ctxt_t the same way
     ds_local does so ds_destroy works uniformly. */
  ds_ctxt_t *ctxt = static_cast<ds_ctxt_t *>(my_malloc(
      PSI_NOT_INSTRUMENTED, sizeof(ds_ctxt_t), MYF(MY_FAE | MY_ZEROFILL)));
  ctxt->ptr = cc.release();
  ctxt->root = my_strdup(PSI_NOT_INSTRUMENTED,
                         root != nullptr ? root : "", MYF(MY_FAE));
  ctxt->fs_support_punch_hole = false;
  return ctxt;
}

ds_file_t *cloud_open(ds_ctxt_t *ctxt, const char *path, MY_STAT *stat
                      [[maybe_unused]],
                      void *file_ctx) {
  auto *cc = static_cast<ds_cloud_ctxt_t *>(ctxt->ptr);

  /* Normalize the relative path: strip any leading "./" so the cloud
     object key is plain "<prefix>/dbname/t1.ibd" rather than
     "<prefix>/./dbname/t1.ibd".  Leading slashes are also stripped,
     for HNS-safety. */
  const char *rel = path;
  while (rel[0] == '.' && rel[1] == '/') rel += 2;
  while (rel[0] == '/') ++rel;

  std::string object = cc->backup_prefix;
  if (!object.empty() && object.back() != '/') object.append("/");
  object.append(rel);

  auto *cf = new ds_cloud_file_t;
  cf->object_name = object;
  cf->ctxt = cc;

  /* Pick (part_size, effective_concurrent) for THIS file based on
     its size, the user's --cloud-multipart-part-size override (if
     any), --cloud-upload-buffer-size, and --cloud-multipart-rollover-
     threshold.

     Auto-sizing rule (when --cloud-multipart-part-size=0):

         effective_size = min(filesize, rollover_threshold)
         part_size      = max(16 MiB, ceil(effective_size / 10000))

     Using effective_size (not filesize) means a 10 TiB file with a
     user-set 100 GiB rollover threshold gets sized for a 100 GiB
     object (the actual cloud-object cap), not for the full 10 TiB.
     That keeps parts small (=> low memory) and the file simply
     splits into more objects per the rollover plumbing.

     If user_part != 0, the override is respected unchanged.

     If --cloud-upload-buffer-size is set, effective_concurrent is
     reduced to fit the memory budget at the chosen part_size.  This
     is the simple "shrink concurrency" model (mirrors aws-cli's
     max_concurrent_requests = budget / chunksize).  A future
     optimization is to use even smaller parts in this case to
     preserve concurrency; tracked as a follow-up. */
  const uint64_t filesize =
      stat ? static_cast<uint64_t>(stat->st_size) : 0;
  static constexpr uint64_t kDefaultPart = 16ULL * 1024 * 1024;  /* 16 MiB floor */
  static constexpr uint64_t kMaxParts = 10000;
  const uint64_t user_part = g_ds_cloud_config.multipart_part_size;
  const uint64_t user_concurrent =
      g_ds_cloud_config.max_concurrent_requests > 0
          ? g_ds_cloud_config.max_concurrent_requests
          : 16;
  const uint64_t rollover = g_ds_cloud_config.multipart_rollover_threshold;
  const uint64_t buf = g_ds_cloud_config.upload_buffer_size;

  uint64_t part_size;
  uint64_t effective_concurrent = user_concurrent;
  bool part_size_auto = false;

  if (user_part != 0) {
    part_size = user_part;
  } else {
    const uint64_t effective_size =
        filesize == 0 ? kDefaultPart * kMaxParts
                       : std::min<uint64_t>(filesize, rollover);
    const uint64_t needed = (effective_size + kMaxParts - 1) / kMaxParts;
    part_size = std::max(kDefaultPart, needed);
    part_size_auto = true;
  }

  if (buf != 0) {
    effective_concurrent =
        std::max<uint64_t>(1, std::min<uint64_t>(user_concurrent,
                                                  buf / part_size));
  }

  /* Diagnostic log so users can see what the auto-sizer chose.
     Three log shapes:

     (1) filesize == 0  -> streaming case (e.g., xtrabackup_logfile
         grows during write, redo log writer, anything with unknown
         final size).  Show "unknown (streaming)".  Auto-sizer fell
         back to the 16 MiB floor for part_size.
     (2) filesize <= --cloud-multipart-threshold  -> small-file fast
         path (single PUT, no multipart).  part_size/concurrent
         don't apply.
     (3) otherwise  -> multipart with the chosen sizing. */
  const uint64_t small_threshold = g_ds_cloud_config.multipart_threshold;
  const char *part_origin = part_size_auto ? " (auto)" : " (user)";
  const char *shrink_note =
      (buf != 0 && effective_concurrent < user_concurrent)
          ? " (shrunk by --cloud-upload-buffer-size)"
          : "";
  if (filesize == 0) {
    xb::info() << "ds_cloud: " << path << ": filesize=unknown (streaming), "
               << "part_size="
               << xtrabackup::utils::human_readable(part_size) << part_origin
               << ", concurrent=" << effective_concurrent << shrink_note;
  } else if (filesize <= small_threshold) {
    xb::info() << "ds_cloud: " << path << ": filesize="
               << xtrabackup::utils::human_readable(filesize)
               << ", single-PUT fast path";
  } else {
    xb::info() << "ds_cloud: " << path << ": filesize="
               << xtrabackup::utils::human_readable(filesize) << ", part_size="
               << xtrabackup::utils::human_readable(part_size) << part_origin
               << ", concurrent=" << effective_concurrent << shrink_note;
  }

  /* Compute the segments plan up-front when filesize is known.  Each
     segment is exactly threshold bytes (last one ≤), uploaded as
     "<base>.r<seg>".  When filesize <= threshold we stay in the
     single-object path and keep the original object name. */
  cf->threshold = rollover;
  cf->base_object = object;
  cf->base_path = path;
  if (filesize > rollover && rollover > 0) {
    cf->n_segments = (filesize + rollover - 1) / rollover;
    cf->current_segment = 1;
    cf->object_name = object + ".r1";
    xb::info() << "ds_cloud: " << path << ": rollover engaged ("
               << cf->n_segments << " segments of up to "
               << xtrabackup::utils::human_readable(rollover) << ")";
  } else {
    cf->n_segments = 1;
    cf->current_segment = 0;
    cf->object_name = object;
  }

  /* Stream_multipart_writer's existing memory_budget parameter is the
     per-writer in-flight byte cap; we pass effective_concurrent *
     part_size so a writer can pipeline its own parts up to the
     budget.  The global buffer cap is enforced inside the multipart
     writer's atomic-counter backpressure (when implemented) -- for
     now, per-writer is sufficient because writers don't share. */
  const uint64_t per_writer_budget = effective_concurrent * part_size;
  /* make_segment_writer wires the async small-file PUT uploader and
     the per-writer display name; the small-file uploader is critical
     for ds_redo's small final part on close (mustn't block the redo
     log reader). */
  cf->writer = make_segment_writer(cc, cf->object_name, path, part_size,
                                    per_writer_budget);
  cf->seg_part_size = part_size;
  cf->seg_per_writer_budget = per_writer_budget;

  size_t path_len = strlen(path) + 1;
  auto *file = static_cast<ds_file_t *>(
      my_malloc(PSI_NOT_INSTRUMENTED, sizeof(ds_file_t) + path_len,
                MYF(MY_FAE | MY_ZEROFILL)));
  file->path = reinterpret_cast<char *>(file + 1);
  memcpy(file->path, path, path_len);
  file->ptr = cf;
  file->datasink = ctxt->datasink;
  file->ctxt = ctxt;
  /* Threaded through the open chain so the top-level ds_close can
  serialize the per-file Document into backup_files.jsonl. */
  file->file_ctx = file_ctx;
  cf->file_ctx = file_ctx;
  return file;
}

/* Rotate ds_cloud_file_t to the next segment: close the current
Multipart_uploader (uploads the in-flight bytes), record the
{path, size} pair on the parent's file_ctx, then build a new writer
for "<base>.r<seg+1>".  Called from cloud_write the moment a write
would cross the segment boundary.

Returns true on success.  On close failure the current writer is left
in place so the caller logs+returns; we don't try to recover. */
bool rotate_segment(ds_cloud_file_t *cf, uint64_t part_size,
                    uint64_t per_writer_budget) {
  if (!cf->writer->close()) {
    xb::error() << "ds_cloud: failed to close segment '" << cf->object_name
                << "'";
    return false;
  }
  /* Record the just-finished segment onto the parent file's
  backup_files.jsonl entry as {path: <rel>.rN, size: <seg_bytes>}.
  The cloud_object key uses <backup_prefix>/<rel>.rN; in the manifest
  we record the rel path so downloaders don't see the prefix. */
  const std::string seg_rel = cf->base_path + ".r" +
                              std::to_string(cf->current_segment);
  xb_files_jsonl::add_segment(cf->file_ctx, seg_rel.c_str(),
                              cf->bytes_in_segment);

  ++cf->current_segment;
  cf->bytes_in_segment = 0;
  cf->object_name = cf->base_object + ".r" +
                    std::to_string(cf->current_segment);
  cf->writer = make_segment_writer(cf->ctxt, cf->object_name,
                                    cf->base_path, part_size,
                                    per_writer_budget);
  return true;
}

/* Single-object open: ds_cloud already uploads each file as one whole
   cloud object (multipart-with-CompleteMultipartUpload or single PUT
   on the small-file fast path), so the "single-object" semantics are
   identical to a regular open from the caller's point of view.  No
   chunking-bypass step needed.  rc2 callers that want to publish a
   plain operator-facing file (backup_metadata.json, backup_files.jsonl)
   reach this through ds_open_single_object(). */
static ds_file_t *cloud_open_single_object(ds_ctxt_t *ctxt, const char *path,
                                           MY_STAT *stat) {
  return cloud_open(ctxt, path, stat, nullptr);
}

int cloud_write(ds_file_t *file, const void *buf, size_t len) {
  auto *cf = static_cast<ds_cloud_file_t *>(file->ptr);

  /* Single-object path (no rollover planned).  If we accumulate past
     the threshold while filesize was unknown / undeclared at open
     time, fail loud -- we'd need to retroactively rename the bare
     object to ".r1" which the cloud APIs don't support cheaply. */
  if (cf->n_segments <= 1) {
    if (!cf->writer->append(static_cast<const char *>(buf), len)) {
      xb::error() << "ds_cloud: append failed for " << file->path;
      return 1;
    }
    if (cf->threshold > 0 &&
        cf->writer->bytes_appended() > cf->threshold) {
      xb::error() << "ds_cloud: file '" << file->path
                  << "' exceeded --cloud-multipart-rollover-threshold ("
                  << xtrabackup::utils::human_readable(cf->threshold)
                  << ") with unknown size at open time; streaming "
                     "rollover requires the size to be known up front";
      return 1;
    }
    cf->ctxt->bytes_written.fetch_add(len, std::memory_order_relaxed);
    return 0;
  }

  /* Rollover-active path: split the incoming buffer at segment
     boundaries so each cloud object contains exactly `threshold`
     bytes (last one may be less). */
  const char *cursor = static_cast<const char *>(buf);
  size_t remaining = len;
  while (remaining > 0) {
    const uint64_t space_in_segment =
        cf->threshold > cf->bytes_in_segment
            ? cf->threshold - cf->bytes_in_segment
            : 0;
    if (space_in_segment == 0) {
      /* Boundary landed exactly; rotate before appending anything. */
      if (cf->current_segment >= cf->n_segments) {
        xb::error() << "ds_cloud: file '" << file->path
                    << "' grew beyond the planned " << cf->n_segments
                    << " segments at open time -- aborting upload";
        return 1;
      }
      if (!rotate_segment(cf, cf->seg_part_size,
                          cf->seg_per_writer_budget)) {
        return 1;
      }
      continue;
    }
    const size_t chunk = static_cast<size_t>(
        std::min<uint64_t>(remaining, space_in_segment));
    if (!cf->writer->append(cursor, chunk)) {
      xb::error() << "ds_cloud: append failed for segment '"
                  << cf->object_name << "'";
      return 1;
    }
    cf->bytes_in_segment += chunk;
    cursor += chunk;
    remaining -= chunk;
  }

  cf->ctxt->bytes_written.fetch_add(len, std::memory_order_relaxed);
  return 0;
}

int cloud_write_sparse(ds_file_t *file, const void *buf, size_t len,
                       size_t sparse_map_size,
                       const ds_sparse_chunk_t *sparse_map,
                       [[maybe_unused]] bool punch_hole_supported) {
  /* Sparse handling in Phase 2: skip holes in the upload payload, BUT
     accumulate {offset, len} into a per-file sparse_map that
     ds_meta will fold into backup_meta.json. For the MVP commit, just
     concatenate data (NOT preserving sparse layout) so the basic
     backup-restore loop works for dense files. Sparse fidelity is the
     subject of a follow-up commit (Phase 2.12). */
  (void)sparse_map_size;
  (void)sparse_map;
  return cloud_write(file, buf, len);
}

int cloud_close(ds_file_t *file) {
  auto *cf = static_cast<ds_cloud_file_t *>(file->ptr);
  int rc = cf->writer->close() ? 0 : 1;
  if (rc != 0) {
    xb::error() << "ds_cloud: close failed for " << file->path;
  } else if (cf->n_segments > 1) {
    /* Final segment closed cleanly: record it alongside its earlier
       siblings.  cf->current_segment is the segment number whose
       writer just closed; cf->bytes_in_segment is its byte count. */
    const std::string seg_rel = cf->base_path + ".r" +
                                std::to_string(cf->current_segment);
    xb_files_jsonl::add_segment(cf->file_ctx, seg_rel.c_str(),
                                cf->bytes_in_segment);
  }
  delete cf;
  my_free(file);
  return rc;
}

void cloud_deinit(ds_ctxt_t *ctxt) {
  auto *cc = static_cast<ds_cloud_ctxt_t *>(ctxt->ptr);
  /* stop() drains the Event_handler -- any small-file PUTs whose
     async submissions returned during cloud_close but whose HTTP
     completion hadn't fired yet drain here. By the time the libev
     thread joins, every async upload has either succeeded or failed. */
  if (cc->event_handler) {
    cc->event_handler->stop();
  }
  if (cc->event_thread.joinable()) {
    cc->event_thread.join();
  }
  if (cc->async_upload_failed.load(std::memory_order_relaxed)) {
    xb::error() << "ds_cloud: one or more async small-file PUTs failed "
                << "during this backup -- the bucket may be missing files. "
                << "Re-check the bucket listing and the log lines above "
                << "for the specific filenames.";
    /* ds_destroy doesn't have a return value, so the failure surfaces
       in the log only. Phase 3 cleanup: add an out-parameter or a
       ds_drain() API so xtrabackup can refuse to write
       xtrabackup_checkpoints when uploads have failed. */
  }
  delete cc;
  my_free(ctxt->root);
  my_free(ctxt);
}

void cloud_report_metrics(const ds_ctxt_t *ctxt,
                          std::vector<ds_metric> &out) {
  const auto *cc = static_cast<const ds_cloud_ctxt_t *>(ctxt->ptr);
  out.push_back(
      {"bytes_written", cc->bytes_written.load(std::memory_order_relaxed)});
}

}  // namespace

datasink_t datasink_cloud = {&cloud_init,
                             &cloud_open,
                             &cloud_write,
                             &cloud_write_sparse,
                             &cloud_close,
                             &cloud_deinit,
                             &cloud_report_metrics,
                             &cloud_open_single_object};

/* ----- pre-backup probe ----- */

bool ds_cloud_probe() {
  if (g_ds_cloud_config.storage.empty()) return true;  /* cloud disabled */
  if (g_ds_cloud_config.container.empty()) {
    xb::error() << "ds_cloud: --cloud-bucket / container is not set";
    return false;
  }

  auto hc = make_cloud_http_client();
  auto store = build_object_store(hc.get());
  if (!store) {
    xb::error() << "ds_cloud: unknown storage backend '"
                << g_ds_cloud_config.storage << "'";
    return false;
  }
  return probe_object_store(store.get(), g_ds_cloud_config.container);
}

/* =====================================================================
   Lifecycle CLI commands (--download / --delete).  These ARE NOT
   datasink operations; they're standalone entry points called from
   xtrabackup's main() when the corresponding mode is set.  They share
   the helpers above (build_object_store, make_cloud_http_client,
   probe_object_store) with the backup-time cloud_init path.
   ===================================================================== */

bool xb_cloud_download(const std::string &target_dir) {
  if (g_ds_cloud_config.storage.empty()) {
    xb::error() << "--download requires --cloud-storage to be set";
    return false;
  }
  if (g_ds_cloud_config.container.empty()) {
    xb::error() << "--download requires the bucket / container option for "
                   "the chosen --cloud-storage (one of --cloud-s3-bucket / "
                   "--cloud-google-bucket / --cloud-azure-container-name / "
                   "--cloud-swift-container)";
    return false;
  }
  if (mkdirp(target_dir.c_str(), 0755, MYF(0)) < 0 && errno != EEXIST) {
    xb::error() << "--download: cannot create target dir " << target_dir
                << ": " << strerror(errno);
    return false;
  }

  auto hc = make_cloud_http_client();
  auto store = build_object_store(hc.get());
  if (!store) {
    xb::error() << "--download: unknown storage backend '"
                << g_ds_cloud_config.storage << "'";
    return false;
  }
  if (!probe_object_store(store.get(), g_ds_cloud_config.container)) {
    return false;
  }

  /* Cloud-side prefix: BUCKET/PREFIX as configured.  Local destination
     for downloaded files: target_dir (the function arg).  The two are
     decoupled. */
  const std::string prefix = g_ds_cloud_config.prefix;

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

  /* Locate the rc2 manifest (backup_metadata.json) and the per-file
     index (backup_files.jsonl) in the listing.  Both are written by
     xb_manifest / xb_files_jsonl during backup_finish and land plain
     (single-object) in the bucket.  Fetched first so we can drive the
     restore from them rather than walking the listing blindly. */
  std::string manifest_obj;
  for (const auto &obj : objects) {
    const std::string rel = strip_prefix(obj, prefix);
    if (rel == "backup_metadata.json") {
      manifest_obj = obj;
      break;
    }
  }
  if (manifest_obj.empty()) {
    xb::error() << "--download: backup_metadata.json missing from bucket "
                << "under " << g_ds_cloud_config.container << "/" << prefix
                << "; refusing to restore (manifest is mandatory in "
                << "rc2 backups)";
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
    full.append("backup_metadata.json");
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
  /* Locate backup_files.jsonl alongside the manifest and fetch it
  next.  We need it before the main download loop so each downloaded
  file knows whether it has a sparse_map to drive punch_hole restore. */
  std::string files_jsonl_obj;
  for (const auto &obj : objects) {
    if (strip_prefix(obj, prefix) == "backup_files.jsonl") {
      files_jsonl_obj = obj;
      break;
    }
  }
  file_index_t file_index;
  std::unordered_set<std::string> segment_objects;  /* full cloud keys */
  if (!files_jsonl_obj.empty()) {
    bool ok = false;
    Http_buffer body = store->download_object(g_ds_cloud_config.container,
                                              files_jsonl_obj, ok);
    if (!ok) {
      xb::error() << "--download: failed to fetch " << files_jsonl_obj;
      return false;
    }
    std::string full = target_dir;
    if (full.empty() || full.back() != '/') full.push_back('/');
    full.append("backup_files.jsonl");
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
    file_index = parse_files_index(body.begin(), body.size());
    /* Build the set of full cloud object keys that are segments of
    some parent file; the bare-object loop below skips these and the
    segmented-file loop reconstructs the parent from them. */
    for (const auto &kv : file_index) {
      for (const auto &seg : kv.second.segments) {
        std::string seg_full = prefix;
        if (!seg_full.empty() && seg_full.back() != '/') seg_full.push_back('/');
        seg_full.append(seg.path);
        segment_objects.insert(std::move(seg_full));
      }
    }
    xb::info() << "--download: backup_files.jsonl loaded ("
               << file_index.size() << " file entries, "
               << segment_objects.size() << " segment objects)";
  } else {
    xb::info() << "--download: no backup_files.jsonl in bucket -- "
                  "downloaded files will be dense (no hole restore, no "
                  "segment concatenation)";
  }

  /* Helper that stages a logical file from one or more dense byte
  blobs and applies sparse-map expansion before atomic rename.  Used
  by both the bare-object path (one body) and the segmented-file path
  (concatenate segment bodies, then expand). */
  auto stage_and_publish =
      [&](const std::string &rel,
          const std::vector<Http_buffer> &bodies,
          const std::vector<sparse_hole_t> &holes) -> bool {
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
    uint64_t dense_total = 0;
    for (const auto &b : bodies) dense_total += b.size();
    bool ok = true;
    if (holes.empty()) {
      /* Dense write: stream every body in order at the current
      file offset. */
      for (const auto &b : bodies) {
        if (write(fd, b.begin(), b.size()) !=
            static_cast<ssize_t>(b.size())) {
          xb::error() << "--download: short write to " << staging;
          ok = false;
          break;
        }
      }
    } else {
      /* Concatenate bodies into a single contiguous dense buffer so
      restore_sparse_file can pwrite at the right logical offsets.
      This keeps the algorithm simple; for very-large multi-segment
      files we could stream segment-by-segment but that requires
      tracking partial sparse-map progress, which is a follow-up. */
      std::string dense;
      dense.reserve(dense_total);
      for (const auto &b : bodies) dense.append(b.begin(), b.size());
      ok = restore_sparse_file(fd, dense.data(), dense.size(), holes);
    }
    close(fd);
    if (!ok) {
      ::unlink(staging.c_str());
      return false;
    }
    if (::rename(staging.c_str(), full.c_str()) != 0) {
      xb::error() << "--download: rename " << staging << " -> " << full
                  << " failed: " << strerror(errno);
      ::unlink(staging.c_str());
      return false;
    }
    return true;
  };

  /* Step 1: download bare-object files (non-segmented).  Iterate the
  listing; skip the manifest, the file index, and any object that
  belongs to a segmented file (those are handled in step 2). */
  for (const auto &obj : objects) {
    if (obj == manifest_obj) continue;
    if (obj == files_jsonl_obj) continue;
    if (segment_objects.count(obj) != 0) continue;

    bool ok = false;
    Http_buffer body =
        store->download_object(g_ds_cloud_config.container, obj, ok);
    if (!ok) {
      xb::error() << "--download: failed to fetch " << obj;
      return false;
    }
    std::string rel = strip_prefix(obj, prefix);

    auto it = file_index.find(rel);
    const std::vector<sparse_hole_t> &holes =
        (it != file_index.end()) ? it->second.holes
                                  : std::vector<sparse_hole_t>{};
    std::vector<Http_buffer> bodies;
    bodies.emplace_back(std::move(body));
    const uint64_t dense_size = bodies.front().size();
    if (!stage_and_publish(rel, bodies, holes)) return false;

    if (!holes.empty()) {
      uint64_t total = 0;
      for (const auto &h : holes) total += h.length;
      xb::info() << "--download: wrote " << rel << " ("
                 << xtrabackup::utils::human_readable(dense_size) << " dense, "
                 << holes.size() << " holes restored, +"
                 << xtrabackup::utils::human_readable(total)
                 << " logical)";
    } else {
      xb::info() << "--download: wrote " << rel << " ("
                 << xtrabackup::utils::human_readable(dense_size) << ")";
    }
  }

  /* Step 2: reconstruct segmented files.  For each file_index entry
  with a non-empty segments[] array, fetch every segment object in
  order, concatenate, then publish (with sparse expansion if the
  parent has a sparse_map). */
  for (const auto &kv : file_index) {
    const std::string &rel = kv.first;
    const file_index_entry_t &entry = kv.second;
    if (entry.segments.empty()) continue;

    std::vector<Http_buffer> bodies;
    bodies.reserve(entry.segments.size());
    uint64_t total_dense = 0;
    for (const auto &seg : entry.segments) {
      std::string seg_obj = prefix;
      if (!seg_obj.empty() && seg_obj.back() != '/') seg_obj.push_back('/');
      seg_obj.append(seg.path);
      bool ok = false;
      Http_buffer body = store->download_object(g_ds_cloud_config.container,
                                                 seg_obj, ok);
      if (!ok) {
        xb::error() << "--download: failed to fetch segment " << seg_obj
                    << " of " << rel;
        return false;
      }
      if (body.size() != seg.size) {
        xb::error() << "--download: segment " << seg.path
                    << " size mismatch: cloud=" << body.size()
                    << " manifest=" << seg.size;
        return false;
      }
      total_dense += body.size();
      bodies.emplace_back(std::move(body));
    }
    if (!stage_and_publish(rel, bodies, entry.holes)) return false;

    if (!entry.holes.empty()) {
      uint64_t total = 0;
      for (const auto &h : entry.holes) total += h.length;
      xb::info() << "--download: wrote " << rel << " ("
                 << entry.segments.size() << " segments, "
                 << xtrabackup::utils::human_readable(total_dense)
                 << " dense, " << entry.holes.size() << " holes restored, +"
                 << xtrabackup::utils::human_readable(total)
                 << " logical)";
    } else {
      xb::info() << "--download: wrote " << rel << " ("
                 << entry.segments.size() << " segments, "
                 << xtrabackup::utils::human_readable(total_dense)
                 << " dense)";
    }
  }
  return true;
}

bool xb_cloud_delete(bool force) {
  if (g_ds_cloud_config.storage.empty()) {
    xb::error() << "--delete requires --cloud-storage to be set";
    return false;
  }
  if (g_ds_cloud_config.container.empty()) {
    xb::error() << "--delete requires the bucket / container option for "
                   "the chosen --cloud-storage (one of --cloud-s3-bucket / "
                   "--cloud-google-bucket / --cloud-azure-container-name / "
                   "--cloud-swift-container)";
    return false;
  }

  auto hc = make_cloud_http_client();
  auto store = build_object_store(hc.get());
  if (!store) {
    xb::error() << "--delete: unknown storage backend '"
                << g_ds_cloud_config.storage << "'";
    return false;
  }
  if (!probe_object_store(store.get(), g_ds_cloud_config.container)) {
    return false;
  }

  /* Prefix WITHIN the bucket comes from the BUCKET/PREFIX form of the
     provider-explicit bucket option (same as backup / download). */
  const std::string prefix = g_ds_cloud_config.prefix;

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
