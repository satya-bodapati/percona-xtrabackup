/******************************************************
Copyright (c) 2026 Percona LLC and/or its affiliates.

backup_meta.json manifest framework: per-file metadata that travels
with each backup file through the datasink pipeline and is collected
at backup-finish time into a single JSON manifest written via ds_meta.

This is the minimal framework slice: a FileContext carries a name and
optional per-file extensions (sparse_map regions today; SHA-256 +
transform records later under PXB-3754). Datasinks see the
FileContext only as the opaque ds_file_t.file_ctx pointer; concrete
field access happens here.

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; version 2 of the License.

*******************************************************/

#ifndef XB_FILE_CONTEXT_H
#define XB_FILE_CONTEXT_H

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

/* A single contiguous data region inside a sparse source file.
   Absolute file offsets in the source IBD (not in the packed
   on-the-wire byte stream). */
struct file_region_t {
  uint64_t offset;
  uint64_t length;
};

/* Per-file metadata. Created at top-of-pipeline open by the reader,
   carried via ds_file_t::file_ctx through the chain, finalized into
   the backup-wide manifest at file close. The destructor pushes the
   finalized contents into the global manifest registry; the
   FileContext itself is then released. */
struct FileContext {
  std::string path;             /* relative path inside the backup */
  uint64_t logical_size = 0;    /* source-file size; 0 = unknown */
  std::vector<file_region_t> regions;  /* sparse data regions; empty = dense */

  /* Construct attached to a backup-relative path. The constructor
     does NOT register; call file_context_register() / use
     file_context_create() factory when you want auto-registration. */
  explicit FileContext(const char *p) : path(p) {}
  explicit FileContext(std::string p) : path(std::move(p)) {}
};

/* Factory: allocate a new FileContext for `path` and register it in
   the global manifest builder. The builder owns the resulting
   pointer; the returned bare pointer is stable for the lifetime of
   the backup. Returns nullptr if the manifest is disabled (currently
   never disabled; reserved for future flag). */
FileContext *file_context_create(const char *path);

/* Convenience: shortcut for callers holding a std::string. */
FileContext *file_context_create(const std::string &path);

/* Mark the FileContext as finalized for this backup. After this
   call, the manifest writer at backup_finish() will include it.
   Safe to call multiple times (idempotent). Safe with nullptr. */
void file_context_finalize(FileContext *fc);

/* Build the backup_meta.json text from the currently-registered
   FileContexts and write the result into `out`. Called by the
   backup-finish path in xtrabackup.cc; not intended for direct use
   from datasinks.
   @return true on success, false on out-of-memory or empty registry. */
bool file_context_build_manifest(std::string &out);

/* Tear down the registry. Called once at xtrabackup exit after
   the manifest has been written. Frees every FileContext that was
   created via the factory. */
void file_context_registry_clear();

#endif /* XB_FILE_CONTEXT_H */
