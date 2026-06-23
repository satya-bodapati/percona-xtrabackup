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
#include "file_context.h"
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

using namespace xbcloud;

ds_cloud_config_t g_ds_cloud_config;

namespace {



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
  std::string object_name;                 /* "<backup_prefix>/<path>" */
  std::unique_ptr<Stream_multipart_writer> writer;
  ds_cloud_ctxt_t *ctxt{nullptr};
};

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
    /* Swift requires Keystone auth first; not exercised on the Phase 2
       MVP path. Skeleton stays here so the ctor compiles. */
    return std::make_unique<Swift_object_store>(http_client, c.url,
                                                c.session_token, c.max_retries,
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
                      [[maybe_unused]]) {
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

  /* Stream_multipart_writer's existing memory_budget parameter is the
     per-writer in-flight byte cap; we pass effective_concurrent *
     part_size so a writer can pipeline its own parts up to the
     budget.  The global buffer cap is enforced inside the multipart
     writer's atomic-counter backpressure (when implemented) -- for
     now, per-writer is sufficient because writers don't share. */
  const uint64_t per_writer_budget = effective_concurrent * part_size;
  cf->writer = std::make_unique<Stream_multipart_writer>(
      cc->object_store.get(), cc->container, object, cc->event_handler.get(),
      per_writer_budget,
      static_cast<size_t>(g_ds_cloud_config.multipart_threshold),
      static_cast<size_t>(part_size));
  /* Use the short relative path as the display name in multipart
     log messages -- the full object key includes the bucket prefix
     and is noisy in logs (a 1 TiB backup at 16 MiB parts would emit
     ~65K lines with the full key each). */
  cf->writer->set_display_name(path);

  /* Empirical (perf_wan.sh + real-AWS benchmarks): sync small-file PUT
     bottlenecks at WAN RTT even when xtrabackup runs many data-copy
     workers, because each worker still serializes through its own
     file sequence. Two specific cases hurt:
       - Lots of small .ibd files: worker N stalls 200ms-per-file on
         every close while ds_redo and other workers wait.
       - ds_redo's small final part on close: blocking it stalls the
         Redo Log reader thread, which we cannot let happen.
     So install an async small-file uploader that submits through the
     Event_handler and returns immediately. Failures land in the ctxt's
     async_upload_failed atomic; cloud_deinit's drain checks it. */
  auto *cc_ptr = cc;
  auto *store = cc->object_store.get();
  auto *event_handler = cc->event_handler.get();
  const std::string &container = cc->container;
  cf->writer->set_async_small_file_uploader(
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

  size_t path_len = strlen(path) + 1;
  auto *file = static_cast<ds_file_t *>(
      my_malloc(PSI_NOT_INSTRUMENTED, sizeof(ds_file_t) + path_len,
                MYF(MY_FAE | MY_ZEROFILL)));
  file->path = reinterpret_cast<char *>(file + 1);
  memcpy(file->path, path, path_len);
  file->ptr = cf;
  file->datasink = ctxt->datasink;
  file->ctxt = ctxt;
  return file;
}

int cloud_write(ds_file_t *file, const void *buf, size_t len) {
  auto *cf = static_cast<ds_cloud_file_t *>(file->ptr);
  if (!cf->writer->append(static_cast<const char *>(buf), len)) {
    xb::error() << "ds_cloud: append failed for " << file->path;
    return 1;
  }
  /* Streaming rollover not yet supported (size unknown up front). If
     the writer's running bytes_appended crosses the configured per-
     object cap, fail fast with a clear message. */
  if (cf->writer->bytes_appended() >
      g_ds_cloud_config.multipart_rollover_threshold) {
    xb::error() << "ds_cloud: file '" << file->path
                << "' exceeded --cloud-multipart-rollover-threshold ("
                << xtrabackup::utils::human_readable(
                       g_ds_cloud_config.multipart_rollover_threshold)
                << "); streaming rollover is not yet implemented";
    return 1;
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

datasink_t datasink_cloud = {&cloud_init,         &cloud_open,
                             &cloud_write,        &cloud_write_sparse,
                             &cloud_close,        &cloud_deinit,
                             &cloud_report_metrics};

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

  /* Locate backup_meta.json in the listing.  It MUST be present --
     the new-format manifest is mandatory and drives sparse-restore
     decisions.  Fetch it first, write to target_dir, then load the
     lookup table before processing data files. */
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

  /* For each non-manifest object:
       1. Download body into memory.
       2. Write to <full>.de-sparse (atomic-rename staging).
       3. If the manifest has a sparse_map for this file, apply the
          manifest-driven punch_hole on the .de-sparse copy.
       4. rename(.de-sparse -> canonical).
     The rename pattern keeps a partial fetch from leaving a half-
     written canonical-named file on disk. */
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
       (no-op) on filesystems without PUNCH_HOLE support; file stays
       dense, only disk-space reclaim is lost. */
    const auto *regions = file_context_lookup_regions(rel.c_str());
    if (regions != nullptr) {
      uint64_t logical_size = file_context_lookup_logical_size(rel.c_str());
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
