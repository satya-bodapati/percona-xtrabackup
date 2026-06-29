/******************************************************
Copyright (c) 2026 Percona LLC and/or its affiliates.

backup_files.jsonl streaming writer.

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

#ifndef XB_FILES_JSONL_H
#define XB_FILES_JSONL_H

#include "datasink.h"

/*
backup_files.jsonl is the streaming companion to backup_metadata.json.
One JSONL line per file in the backup, appended as the file closes.
The first line carries the format version; subsequent lines are
file entries enriched by every datasink in the open chain.

Lifecycle:
  backup_start ->  xb_files_jsonl::begin(staging_dir)
                      opens the staging file, writes header
  per-file ds_open_with_ctx(... new_file_ctx) attaches the
        per-file Document
  per-file ds_close()
                      top-level ds_close calls
                      xb_files_jsonl::append_and_release(file_ctx),
                      which serialises one line via atomic O_APPEND
                      and frees the Document
  backup_finish -> xb_files_jsonl::finalize()
                      closes the staging file
                ->  xb_files_jsonl::publish(ds_root)
                      writes through ds_open_single_object so the
                      file lands plain in any output mode
                ->  xb_files_jsonl::write_to_dir(extra_lsndir)
                      mirror copy
*/

constexpr const char *XB_BACKUP_FILES_JSONL = "backup_files.jsonl";

namespace xb_files_jsonl {

/* Open the streaming staging file under @p staging_dir.  Writes
the header line ({"manifest_version": 1}\n).  After begin(),
new_file_ctx() and append_and_release() may be called concurrently
from worker threads. */
bool begin(const char *staging_dir);

/* True between begin() and finalize().  When this returns false,
new_file_ctx() returns nullptr and append_and_release() is a
no-op -- the zero-overhead path for backup modes that have not
opted into file-list tracking. */
bool is_active();

/* Allocate a fresh per-file context document, populate top-level
"path", and return an opaque pointer.  The caller binds the
pointer to the ds_file_t at open time via ds_open_with_ctx.
Returns nullptr if !is_active().  Caller does not free the
returned pointer -- ownership transfers to xb_files_jsonl, which
frees it inside append_and_release(). */
void *new_file_ctx(const char *path);

/* Annotators (the per-datasink close ops) call set_member to add
their JSON section to the per-file document.  @p file_ctx is the
opaque pointer returned by new_file_ctx().  @p section_name is the
datasink's identifier (compress_zstd, encrypt_aes256_cbc, ...);
the value is the JSON object/value to record.  No-op when
file_ctx is null. */
void set_string(void *file_ctx, const char *key, const char *value);
void set_uint64(void *file_ctx, const char *key, uint64_t value);
void set_uint32(void *file_ctx, const char *key, uint32_t value);

/* Begin a nested object section keyed by @p name on file_ctx, then
return an opaque pointer for the caller to populate via further
set_* calls scoped to the section.  The section object stays
alive until append_and_release is invoked on the parent
file_ctx.  No-op (returns nullptr) when file_ctx is null. */
void *open_section(void *file_ctx, const char *name);

/* Serialise file_ctx to one JSONL line and append it via atomic
O_APPEND.  Frees the document afterwards.  Called once per file
by the top-level ds_close hook, after the close chain has fully
unwound (so every datasink has had a chance to annotate). */
void append_and_release(void *file_ctx);

/* Flush + close the staging file.  Idempotent; safe to call when
!is_active(). */
void finalize();

/* Publish the staging backup_files.jsonl through @p ds_root using
ds_open_single_object so it lands plain in any output mode.
Returns true on success. */
bool publish(ds_ctxt_t *ds_root);

/* Write a byte-identical copy of the staging file to @p dir as
plain bytes (direct filesystem write, no datasinks).  Used to
mirror under --extra-lsndir.  Returns true on success. */
bool write_to_dir(const char *dir);

/* Unlink the staging file and forget its path.  Call once at the
very end (after publish + write_to_dir).  Idempotent. */
void cleanup_staging();

}  // namespace xb_files_jsonl

#endif
