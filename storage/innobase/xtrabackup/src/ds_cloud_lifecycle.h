/******************************************************
Copyright (c) 2026 Percona LLC and/or its affiliates.

xtrabackup cloud lifecycle helpers: download / delete entry points.
ds_cloud.cc handles backup-side upload; this header exposes the
companion read-side operations that close the loop so xbcloud can be
removed in a future release.

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; version 2 of the License.

*******************************************************/

#ifndef XB_DS_CLOUD_LIFECYCLE_H
#define XB_DS_CLOUD_LIFECYCLE_H

#include <string>

/* Download every object under the configured bucket/prefix into the
   supplied target_dir. Returns true on success. Sparse reconstruction
   and rollover-segment concatenation will land with backup_meta.json
   (PXB-3754); this MVP downloads whole objects sequentially. */
bool xb_cloud_download(const std::string &target_dir);

/* Delete every object under the configured bucket/prefix. Confirmation
   is interactive unless --force is set on the command line. */
bool xb_cloud_delete(bool force);

#endif /* XB_DS_CLOUD_LIFECYCLE_H */
