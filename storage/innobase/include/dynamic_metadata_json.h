/******************************************************
Copyright (c) 2026 Percona LLC and/or its affiliates.

PXB-2865: persist MLOG_TABLE_DYNAMIC_META records across prepare
invocations via a JSON sidecar so that incremental chains preserve
dynamic metadata even when the source server's checkpoint never
flushed mysql.innodb_dynamic_metadata to mysql.ibd between backups.

Schema (file: <target-dir>/xtrabackup_dynamic_metadata.json):

  {
    "schema_version": 1,
    "tables": {
      "<table_id>": {
        "version":  <uint64>,
        "autoinc":  <uint64>,
        "corrupt_indexes": [
          { "space_id": <uint32>, "index_id": <uint64> },
          ...
        ]
      },
      ...
    }
  }

Merge semantics across prepares follow AutoIncPersister::aggregate
(dict0dict.cc:5687): strictly-higher version wins; tied versions
keep the larger autoinc and merge the corrupt-index list. This is
also the policy MetadataRecover::inject_metadata applies when the
sidecar is loaded back into a live recovery instance.

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.
*******************************************************/

#ifndef XB_DYNAMIC_METADATA_JSON_H
#define XB_DYNAMIC_METADATA_JSON_H

class MetadataRecover;

namespace xb {
namespace dyn_meta {

/** Filename for the sidecar inside xtrabackup_target_dir. */
constexpr const char *kSidecarFile = "xtrabackup_dynamic_metadata.json";

/** Load the JSON sidecar from xtrabackup_target_dir (if present) and
inject every entry into @p mr via MetadataRecover::inject_metadata.
Higher-version entries win when merged with anything already collected
from this prepare's redo scan.

Safe to call when no sidecar exists (no-op). Logs and returns false
on parse failure; caller should treat that as a soft failure and
continue (the records will simply not be applied; behavior degrades
to pre-PXB-2865).

@param[in,out] mr  recovery metadata collector
@return true on success or absent file; false on parse failure */
bool load_into(MetadataRecover *mr);

/** Serialize the contents of @p mr to the JSON sidecar in
xtrabackup_target_dir, merging with any existing file (keeping the
higher version per table_id). Called at end of a --apply-log-only
prepare so the records survive into the next prepare invocation.

@param[in] mr  recovery metadata collector
@return true on success; false on serialization or I/O failure */
bool save_from(const MetadataRecover *mr);

/** Remove the JSON sidecar (called after a final prepare that has
successfully applied the contents via dict_metadata->store()). Safe to
call when the file does not exist. */
void remove();

}  // namespace dyn_meta
}  // namespace xb

#endif /* XB_DYNAMIC_METADATA_JSON_H */
