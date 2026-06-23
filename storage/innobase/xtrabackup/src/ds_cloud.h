/******************************************************
Copyright (c) 2026 Percona LLC and/or its affiliates.

ds_cloud: xtrabackup datasink that uploads each file directly to an
S3 / Azure / Swift / GCS bucket via the shared xbcloud multipart
machinery. Replaces the legacy `--stream=xbstream | xbcloud put`
pipeline; xbcloud keeps working unchanged for testing and comparison.

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; version 2 of the License.

*******************************************************/

#ifndef XB_DS_CLOUD_H
#define XB_DS_CLOUD_H

#include <map>
#include <string>
#include <vector>

#include "datasink.h"
#include "my_inttypes.h"

extern datasink_t datasink_cloud;

/**
  Global configuration for ds_cloud. Populated from xtrabackup's
  --cloud-* CLI options BEFORE ds_create(DS_TYPE_CLOUD) is called.
  Read-only after that point.

  Kept as a free-standing struct (not a member of the per-context
  state) so option parsing in xtrabackup.cc can fill it without needing
  a ds_ctxt_t reference.
*/
struct ds_cloud_config_t {
  std::string storage;       /* "s3" | "gcs" | "azure" | "swift" */
  std::string url;           /* full bucket URL or endpoint */
  std::string bucket;        /* if separate from url; usually empty */
  std::string region;
  std::string endpoint;      /* alt endpoint host (defaults from storage) */
  std::string access_key;
  std::string secret_key;
  std::string session_token; /* AWS STS */
  std::string container;     /* bucket / container name (NO trailing /) */
  /* Optional sub-prefix WITHIN the bucket; parsed from the provider-
     explicit bucket option's BUCKET/PREFIX form by
     parse_cloud_bucket_with_prefix() in xtrabackup.cc.  Stored without
     leading or trailing '/'.  Backup objects are PUT at
     <container>/<prefix>/<file>, or just <container>/<file> when prefix
     is empty.  HNS-safe: never ends in '/'. */
  std::string prefix;
  std::string bucket_lookup; /* "auto" | "path" | "dns" */
  std::string storage_class; /* S3/Azure storage class hint */

  /* Azure-specific. */
  std::string azure_account;
  std::string azure_access_key;
  std::string azure_endpoint;

  /* HTTP knobs. */
  bool insecure{false};
  std::string cacert;
  ulong timeout{120};
  ulong max_retries{10};
  ulong max_backoff{300000};
  /* Max concurrent in-flight HTTP requests inside the libev / curl-multi
     Event_handler. This is HTTP-side concurrency, NOT the count of
     data-copy threads (that's --parallel). Inherits --parallel by
     default; override with --cloud-max-concurrent-requests. */
  ulong max_concurrent_requests{8};

  /* Multipart knobs. ds_cloud is multipart-only by design -- there is
     no chunk-PUT fallback like xbcloud's legacy mode -- so there's no
     enable/disable switch. */
  ulonglong multipart_part_size{0};  /* 0 = dynamic schedule */
  /* In-flight byte cap per Stream_multipart_writer is hardcoded to
     64 MiB at the writer construction site in ds_cloud.cc -- NOT a
     config field, by design. Exposing it as a tunable historically
     led to the 4 GiB-per-writer * --parallel = 32 GiB OOM footgun.
     Followup: when we add a shared pool across all writers in one
     ds_cloud_ctxt, expose it as --cloud-upload-buffer-size (default
     256 MiB total, not per-writer). Until then, peak memory is
     --parallel * 64 MiB which is a sensible 256-512 MiB at typical
     parallelism. */
  ulonglong multipart_threshold{16ULL * 1024 * 1024};
  ulonglong multipart_rollover_threshold{5ULL * 1024 * 1024 * 1024 * 1024};
  /* Total cloud-upload memory cap across all writers in this ctxt.
     0 = unlimited.  When non-zero, the per-file algorithm at
     cloud_open time chooses (part_size, effective_concurrent,
     object_size_cap) to honor this cap.  See cloud_pick_upload_plan()
     in ds_cloud.cc. */
  ulonglong upload_buffer_size{0};

  /* Observability. */
  ulong rate_log_interval{10};
  bool http_timing{false};

  /* xbcloud parity (PXB-3671 commit 2):
       - verbose: CURLOPT_VERBOSE; libcurl prints request/response to
         stderr.  Useful for debugging signing or TLS issues.
       - s3_api_version: 0=AUTO, 1=v2, 2=v4.  Mirrors xbcloud's
         --s3-api-version typelib position-for-position (kept in sync
         via cloud_s3_api_version_names[] in xtrabackup.cc).
       - azure_development_storage: shortcut for Azurite emulator
         credentials; equivalent to passing the well-known dev account
         / key triple.  Endpoint defaults to http://127.0.0.1:10000.
       - extra_http_headers: extra Name->Value pairs added to every
         cloud HTTP request.
       - curl_retriable_errors / http_retriable_errors: extra error
         codes that the retry/backoff loop should treat as transient
         (in ADDITION to the built-in defaults). */
  bool verbose{false};
  ulong s3_api_version{0};
  bool azure_development_storage{false};
  std::map<std::string, std::string> extra_http_headers;
  std::vector<long> curl_retriable_errors;
  std::vector<long> http_retriable_errors;

  /* Swift / OpenStack Keystone v3 auth chain (PXB-3671 commit 3).
     1:1 with the equivalent xbcloud --swift-* options.  build_object_
     store() (ds_cloud.cc) runs the Keystone dance from these at backup-
     time to resolve (storage_url, token), then constructs the
     Swift_object_store -- same flow as xbcloud's main(). */
  std::string swift_user;
  std::string swift_user_id;
  std::string swift_password;
  std::string swift_tenant;
  std::string swift_tenant_id;
  std::string swift_project;
  std::string swift_project_id;
  std::string swift_domain;
  std::string swift_domain_id;
  std::string swift_project_domain;
  std::string swift_project_domain_id;
  std::string swift_region;
  std::string swift_storage_url;
  std::string swift_auth_url;
  std::string swift_key;
  std::string swift_auth_version;  /* "1" | "2" | "3"; empty=1/TempAuth */
};

extern ds_cloud_config_t g_ds_cloud_config;

/**
  Run the cloud-side probe (credentials / bucket reachable) without
  starting any uploads. Returns true on success. xtrabackup --backup
  with --cloud-storage set calls this before the file-copy threads
  start so a misconfigured destination doesn't waste a backup attempt.

  On failure, an actionable error message is printed (ExpiredToken /
  AccessDenied / NoSuchBucket / etc).
*/
bool ds_cloud_probe();

/* ---- Lifecycle ops (CLI commands, not datasink interface) ---- */

/* Download every object under the configured bucket/prefix into the
   supplied target_dir. Returns true on success. Manifest
   (backup_meta.json) is fetched first; sparse files are
   reconstructed via lseek+pwrite + manifest-driven punch_hole. */
bool xb_cloud_download(const std::string &target_dir);

/* Delete every object under the configured bucket/prefix. Confirmation
   is interactive unless @p force is true. */
bool xb_cloud_delete(bool force);

#endif /* XB_DS_CLOUD_H */
