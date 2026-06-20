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
#include <string>
#include <thread>
#include <vector>

#include <mysql/service_mysql_alloc.h>
#include <my_sys.h>

#include "common.h"
#include "datasink.h"
#include "msg.h"

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
};

/* Per-file state. file->ptr points here. */
struct ds_cloud_file_t {
  std::string object_name;                 /* "<backup_prefix>/<path>" */
  std::unique_ptr<Object_store_multipart_helper> helper;
  std::unique_ptr<Multipart_uploader> uploader;
  Http_buffer part_buf;
  int next_part_num{1};
  uint64_t bytes_appended{0};
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
      g_ds_cloud_config.parallel > 0 ? g_ds_cloud_config.parallel : 1);
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
  cf->helper = std::make_unique<Object_store_multipart_helper>(
      cc->object_store.get(), cc->container, object);
  cf->uploader = std::make_unique<Multipart_uploader>(
      cf->helper.get(), cc->event_handler.get(),
      g_ds_cloud_config.multipart_memory_budget);
  /* NOTE: do NOT call uploader->start() here -- lazy Init means small
     files take the single-PUT fast path with zero control-plane RTTs. */

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

/* Flush accumulated parts while we have enough buffered. The flush
   threshold is dynamic (re-evaluated against bytes_appended so the
   tier grows with stream size) unless --cloud-multipart-part-size set
   an explicit override. */
bool flush_parts_if_full(ds_cloud_file_t *cf) {
  while (true) {
    size_t part_size =
        g_ds_cloud_config.multipart_part_size != 0
            ? static_cast<size_t>(g_ds_cloud_config.multipart_part_size)
            : dynamic_part_size(cf->bytes_appended);
    if (cf->part_buf.size() < part_size) break;
    if (!cf->uploader->upload_part(cf->next_part_num, cf->part_buf.begin(),
                                   part_size)) {
      msg_ts("ds_cloud: upload_part %d failed for %s\n", cf->next_part_num,
             cf->object_name.c_str());
      return false;
    }
    Http_buffer leftover;
    if (cf->part_buf.size() > part_size) {
      leftover.append(cf->part_buf.begin() + part_size,
                      cf->part_buf.size() - part_size);
    }
    cf->part_buf = std::move(leftover);
    cf->bytes_appended += part_size;
    ++cf->next_part_num;

    /* Safety: streaming rollover not yet supported (size unknown up
       front). If the stream is about to cross the per-object hard cap,
       abort with a clear message. The Phase 2 manifest will fold
       streaming rollover later. */
    if (cf->bytes_appended > g_ds_cloud_config.multipart_rollover_threshold) {
      msg_ts(
          "ds_cloud: file '%s' exceeded --cloud-multipart-rollover-threshold "
          "(%llu bytes); streaming rollover is not yet implemented\n",
          cf->object_name.c_str(),
          g_ds_cloud_config.multipart_rollover_threshold);
      return false;
    }
  }
  return true;
}

int cloud_write(ds_file_t *file, const void *buf, size_t len) {
  auto *cf = static_cast<ds_cloud_file_t *>(file->ptr);
  if (len > 0) {
    cf->part_buf.append(static_cast<const char *>(buf), len);
  }
  if (!flush_parts_if_full(cf)) return 1;
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
  int rc = 0;

  /* Small-file fast path: if no parts have been submitted yet AND the
     entire buffer is at-or-below the threshold, ship as a single PUT.
     Avoids Init + UploadPart + Complete round-trips for tiny files. */
  bool small_file_single_put =
      cf->next_part_num == 1 &&
      cf->part_buf.size() <=
          static_cast<size_t>(g_ds_cloud_config.multipart_threshold);

  if (small_file_single_put) {
    if (cf->uploader->started()) {
      cf->uploader->abort();
    }
    Http_buffer payload;
    if (cf->part_buf.size() > 0) {
      payload.append(cf->part_buf.begin(), cf->part_buf.size());
    }
    /* Use sync upload_object -- xtrabackup's per-file close runs on a
       data-copy thread already, so blocking briefly here is fine. The
       Http_client's CURLSH share keeps the TCP connection warm. */
    if (!cf->ctxt->object_store->upload_object(
            cf->ctxt->container, cf->object_name, payload)) {
      msg_ts("ds_cloud: small-file PUT failed for %s\n",
             cf->object_name.c_str());
      rc = 1;
    }
  } else {
    /* Final part: whatever bytes remain. May be < 5 MiB; S3 allows the
       last part to be any size. */
    if (cf->part_buf.size() > 0) {
      if (!cf->uploader->upload_part(cf->next_part_num, cf->part_buf.begin(),
                                     cf->part_buf.size())) {
        msg_ts("ds_cloud: final upload_part %d failed for %s\n",
               cf->next_part_num, cf->object_name.c_str());
        rc = 1;
      }
    }
    if (rc == 0) {
      if (!cf->uploader->commit()) {
        msg_ts("ds_cloud: commit failed for %s\n", cf->object_name.c_str());
        rc = 1;
      }
    } else {
      cf->uploader->abort();
    }
  }

  delete cf;
  my_free(file);
  return rc;
}

void cloud_deinit(ds_ctxt_t *ctxt) {
  auto *cc = static_cast<ds_cloud_ctxt_t *>(ctxt->ptr);
  if (cc->event_handler) {
    cc->event_handler->stop();
  }
  if (cc->event_thread.joinable()) {
    cc->event_thread.join();
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
