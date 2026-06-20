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

#include <string>

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
  std::string container;     /* bucket name */
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
  ulong parallel{8};

  /* Multipart knobs. */
  bool multipart_upload{true};
  ulonglong multipart_part_size{0};  /* 0 = dynamic schedule */
  ulonglong multipart_memory_budget{4ULL * 1024 * 1024 * 1024};
  ulonglong multipart_threshold{16ULL * 1024 * 1024};
  ulonglong multipart_rollover_threshold{5ULL * 1024 * 1024 * 1024 * 1024};

  /* Observability. */
  ulong rate_log_interval{10};
  bool http_timing{false};
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

#endif /* XB_DS_CLOUD_H */
