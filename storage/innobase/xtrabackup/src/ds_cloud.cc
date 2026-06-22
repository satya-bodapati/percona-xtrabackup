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

#include <atomic>
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
#include "msg.h"
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

/* ----- Object_store factory based on g_ds_cloud_config ----- */

std::unique_ptr<Object_store> build_object_store(Http_client *http_client) {
  const auto &c = g_ds_cloud_config;
  if (c.storage == "s3" || c.storage == "gcs" || c.storage == "google") {
    s3_bucket_lookup_t lookup = LOOKUP_AUTO;
    if (c.bucket_lookup == "path") lookup = LOOKUP_PATH;
    else if (c.bucket_lookup == "dns") lookup = LOOKUP_DNS;

    std::string region_copy = c.region;
    auto store = std::make_unique<S3_object_store>(
        http_client, region_copy, c.access_key, c.secret_key,
        c.session_token, c.storage_class, c.max_retries, c.max_backoff,
        c.endpoint, lookup, S3_V_AUTO);
    /* probe will set version + lookup on success */
    return store;
  }
  if (c.storage == "azure") {
    return std::make_unique<Azure_object_store>(
        http_client, c.azure_account, c.azure_access_key, false /* dev */,
        c.storage_class, c.max_retries, c.max_backoff, c.azure_endpoint);
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

bool probe_and_setup_store(ds_cloud_ctxt_t *cc) {
  /* For S3/GCS, probe runs the api-version + bucket-lookup detection that
     xbcloud's probe_api_version_and_lookup does. */
  if (g_ds_cloud_config.storage == "s3" ||
      g_ds_cloud_config.storage == "gcs" ||
      g_ds_cloud_config.storage == "google") {
    auto *s3_store = static_cast<S3_object_store *>(cc->object_store.get());
    if (!s3_store->probe_api_version_and_lookup(cc->container)) {
      msg_ts("ds_cloud: probe failed for bucket '%s'\n",
             cc->container.c_str());
      return false;
    }
  } else {
    /* For Azure/Swift, container_exists is sufficient. */
    bool exists;
    if (!cc->object_store->container_exists(cc->container, exists)) {
      msg_ts("ds_cloud: container_exists check failed for '%s'\n",
             cc->container.c_str());
      return false;
    }
    if (!exists) {
      msg_ts("ds_cloud: container '%s' does not exist\n",
             cc->container.c_str());
      return false;
    }
  }
  return true;
}

/* ----- datasink ops ----- */

ds_ctxt_t *cloud_init(const char *root) {
  if (g_ds_cloud_config.storage.empty()) {
    msg_ts("ds_cloud: --cloud-storage is not set; cannot initialize\n");
    return nullptr;
  }
  if (g_ds_cloud_config.container.empty()) {
    msg_ts("ds_cloud: --cloud-bucket / container is not set\n");
    return nullptr;
  }

  auto cc = std::make_unique<ds_cloud_ctxt_t>();
  cc->container = g_ds_cloud_config.container;
  cc->backup_prefix = (root != nullptr ? root : "");

  cc->http_client = std::make_unique<Http_client>();
  cc->http_client->set_max_retries(g_ds_cloud_config.max_retries);
  cc->http_client->set_max_backoff(g_ds_cloud_config.max_backoff);
  cc->http_client->set_timeout(g_ds_cloud_config.timeout);
  if (g_ds_cloud_config.insecure) cc->http_client->set_insecure(true);
  if (!g_ds_cloud_config.cacert.empty())
    cc->http_client->set_cacaert(g_ds_cloud_config.cacert);

  cc->object_store = build_object_store(cc->http_client.get());
  if (!cc->object_store) {
    msg_ts("ds_cloud: unknown storage backend '%s'\n",
           g_ds_cloud_config.storage.c_str());
    return nullptr;
  }

  if (!probe_and_setup_store(cc.get())) {
    return nullptr;
  }

  cc->event_handler = std::make_unique<Event_handler>(
      g_ds_cloud_config.http_parallel_requests > 0
          ? g_ds_cloud_config.http_parallel_requests
          : 1);
  if (!cc->event_handler->init()) {
    msg_ts("ds_cloud: Event_handler init failed\n");
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

  std::string object = cc->backup_prefix;
  if (!object.empty() && object.back() != '/') object.append("/");
  object.append(path);

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
      g_ds_cloud_config.http_parallel_requests > 0
          ? g_ds_cloud_config.http_parallel_requests
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
     Files below --cloud-multipart-threshold take the small-file
     fast path inside Stream_multipart_writer::close() -- one single
     PUT, no multipart Initiate/Abort, so the part_size and
     concurrent numbers don't apply.  We log accordingly so the user
     isn't confused by "part_size=16 MiB" on a 112 KiB file. */
  const uint64_t small_threshold = g_ds_cloud_config.multipart_threshold;
  if (filesize > 0 && filesize <= small_threshold) {
    msg_ts("ds_cloud: %s: filesize=%s, single-PUT fast path\n",
           object.c_str(),
           xtrabackup::utils::human_readable(filesize).c_str());
  } else {
    msg_ts(
        "ds_cloud: %s: filesize=%s, part_size=%s%s, concurrent=%llu%s\n",
        object.c_str(),
        xtrabackup::utils::human_readable(filesize).c_str(),
        xtrabackup::utils::human_readable(part_size).c_str(),
        part_size_auto ? " (auto)" : " (user)",
        (unsigned long long)effective_concurrent,
        buf != 0 && effective_concurrent < user_concurrent
            ? " (shrunk by --cloud-upload-buffer-size)"
            : "");
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
                msg_ts("ds_cloud: small-file PUT done: %s (%zu bytes)\n",
                       name.c_str(), length);
              } else {
                msg_ts(
                    "ds_cloud: small-file PUT FAILED: %s (%zu bytes)\n",
                    name.c_str(), length);
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
    msg_ts("ds_cloud: append failed for %s\n", cf->object_name.c_str());
    return 1;
  }
  /* Streaming rollover not yet supported (size unknown up front). If
     the writer's running bytes_appended crosses the configured per-
     object cap, fail fast with a clear message. */
  if (cf->writer->bytes_appended() >
      g_ds_cloud_config.multipart_rollover_threshold) {
    msg_ts(
        "ds_cloud: file '%s' exceeded --cloud-multipart-rollover-threshold "
        "(%llu bytes); streaming rollover is not yet implemented\n",
        cf->object_name.c_str(),
        g_ds_cloud_config.multipart_rollover_threshold);
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
    msg_ts("ds_cloud: close failed for %s\n", cf->object_name.c_str());
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
    msg_ts(
        "ds_cloud: one or more async small-file PUTs failed during this "
        "backup -- the bucket may be missing files. Re-check the bucket "
        "listing and the log lines above for the specific filenames.\n");
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
    msg_ts("ds_cloud: --cloud-bucket / container is not set\n");
    return false;
  }

  Http_client hc;
  hc.set_max_retries(g_ds_cloud_config.max_retries);
  hc.set_max_backoff(g_ds_cloud_config.max_backoff);
  hc.set_timeout(g_ds_cloud_config.timeout);
  if (g_ds_cloud_config.insecure) hc.set_insecure(true);
  if (!g_ds_cloud_config.cacert.empty())
    hc.set_cacaert(g_ds_cloud_config.cacert);

  auto store = build_object_store(&hc);
  if (!store) {
    msg_ts("ds_cloud: unknown storage backend '%s'\n",
           g_ds_cloud_config.storage.c_str());
    return false;
  }

  if (g_ds_cloud_config.storage == "s3" ||
      g_ds_cloud_config.storage == "gcs" ||
      g_ds_cloud_config.storage == "google") {
    auto *s3 = static_cast<S3_object_store *>(store.get());
    if (!s3->probe_api_version_and_lookup(g_ds_cloud_config.container)) {
      return false;
    }
  } else {
    bool exists;
    if (!store->container_exists(g_ds_cloud_config.container, exists)) {
      return false;
    }
    if (!exists) {
      msg_ts("ds_cloud: bucket '%s' does not exist\n",
             g_ds_cloud_config.container.c_str());
      return false;
    }
  }

  return true;
}
