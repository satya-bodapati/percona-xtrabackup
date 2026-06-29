/******************************************************
Copyright (c) 2026 Percona LLC and/or its affiliates.

backup_metadata.json writer.

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA

*******************************************************/

#ifndef XB_MANIFEST_H
#define XB_MANIFEST_H

#include <string>
#include "datasink.h"

/*
backup_metadata.json is a per-backup plain-JSON manifest written
through ds_open_single_object so it lands without compress/encrypt
transforms regardless of how the backup pipeline is configured.  It
duplicates the data carried by the legacy xtrabackup_info /
xtrabackup_binlog_info / xtrabackup_checkpoints / xtrabackup_galera_info
/ xtrabackup_replica_info / xtrabackup_slave_info files in a
structured form readable by `jq`, `cat`, and any operator tool, in
local, xbstream, and xbcloud-stored backups alike.  The legacy files
keep being written exactly as today; backup_metadata.json is purely
additive.
*/

constexpr const char *XB_BACKUP_METADATA_JSON = "backup_metadata.json";

namespace xb_manifest {

/* Cache the verbatim textual content of a legacy info file as it is
generated.  Called by the writers of xtrabackup_info,
xtrabackup_binlog_info, xtrabackup_checkpoints,
xtrabackup_galera_info, xtrabackup_replica_info, and
xtrabackup_slave_info just before they pipe the same text to the
datasink chain.  Used at backup_finish to build backup_metadata.json
without going back to disk -- which would fail in stream/cloud
modes where the legacy files never land locally. */
void set_legacy_text(const char *name, const std::string &text);

/* Read back what set_legacy_text() recorded for @p name.  Empty
string if nothing was recorded.  Used by xtrabackup_write_info to
write the --extra-lsndir copy of xtrabackup_info with the same
content as the target-dir copy (so backup_size matches across
both). */
const std::string &get_legacy_text(const char *name);

/* Build backup_metadata.json from the cached legacy texts and a
fresh top-level manifest_version constant.  Returns the JSON string
ready to write.  Idempotent given the same inputs. */
std::string build_json();

/* Publish backup_metadata.json through @p ds_root via
ds_open_single_object so the file lands plain in any output mode
(local target dir, xbstream stream, xbcloud bucket).  Returns true
on success. */
bool publish(ds_ctxt_t *ds_root, const std::string &json_text);

/* Write a byte-identical copy of backup_metadata.json directly to
@p dir (not through any datasink chain).  Used to mirror the
manifest under --extra-lsndir.  Returns true on success. */
bool write_to_dir(const char *dir, const std::string &json_text);

}  // namespace xb_manifest

#endif
