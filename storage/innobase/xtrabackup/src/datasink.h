/******************************************************
Copyright (c) 2011-2023 Percona LLC and/or its affiliates.

Data sink interface.

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

#ifndef XB_DATASINK_H
#define XB_DATASINK_H

#include <my_dir.h>
#include <atomic>
#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif

struct datasink_struct;
typedef struct datasink_struct datasink_t;

/** Aggregate counters for a backup pipeline head.  New metrics go here;
ds_ctxt_t keeps a single pointer and is never touched when a new counter
is added.

ds_metrics encapsulates its own synchronization: callers only see methods
(add_bytes, get_bytes, ...).  The synchronization primitive (atomic,
mutex, lock-free queue, ...) is the implementation detail of ds_metrics
and is never exposed to the call site.  This keeps ds_write() /
ds_write_sparse() and the reporting helpers oblivious to how each metric
is stored, so a future counter that needs more than a single atomic word
(e.g. a histogram or composite map under a mutex) does not push locks
into the framework write path. */
struct ds_metrics {
  /* Add raw bytes that flowed through the pipeline. */
  void add_bytes(uint64_t n) { bytes_.fetch_add(n, std::memory_order_relaxed); }
  /* Read the current raw byte count. */
  uint64_t get_bytes() const { return bytes_.load(std::memory_order_relaxed); }
  /* Future counters (files, pages, elapsed time, ...) add their own
     methods + private storage here.  Pick the synchronization primitive
     that fits the data: atomic for scalar counters, mutex for composite
     state -- callers do not need to know. */
 private:
  std::atomic<uint64_t> bytes_{0};
};

/** Datasink context.  Holds an optional pointer to a ds_metrics instance
so the framework can account per-pipeline counters from ds_write() /
ds_write_sparse() without any call-site instrumentation.  Today all four
backup heads (data, uncompressed_data, redo, meta) share one global
ds_metrics (xb_backup_metrics); a head can later point at its own
instance for per-pipeline breakdowns. */
typedef struct ds_ctxt {
  datasink_t *datasink;
  char *root;
  void *ptr;
  struct ds_ctxt *pipe_ctxt;
  bool fs_support_punch_hole = false;
  ds_metrics *metrics = nullptr;
} ds_ctxt_t;

/** Every ds_file_t holds a back-pointer to its owning ds_ctxt_t so the
framework can reach per-pipeline state (e.g. metrics) from the hot write
path without having to thread extra parameters through the vtable.
Pipeline heads are the natural owners of pipeline-wide config; files just
remember which head they came from. */
typedef struct {
  void *ptr;
  char *path;
  datasink_t *datasink;
  ds_ctxt_t *ctxt = nullptr;
  /* Metrics binding for this file.  Propagated from ctxt->metrics by
  ds_open() so the user-visible head file gets the metrics.  Wrapper-internal
  files (those returned by ds_open_internal() inside compress/buffer/encrypt/
  tmpfile/xbstream open()) carry a NULL metrics so passthrough writes are
  not double-counted.  ds_write/ds_write_sparse fire the counter only when
  this is non-null. */
  ds_metrics *metrics = nullptr;
} ds_file_t;

typedef struct {
  size_t skip;
  size_t len;
} ds_sparse_chunk_t;

struct datasink_struct {
  ds_ctxt_t *(*init)(const char *root);
  ds_file_t *(*open)(ds_ctxt_t *ctxt, const char *path, MY_STAT *stat);
  int (*write)(ds_file_t *file, const void *buf, size_t len);
  int (*write_sparse)(ds_file_t *file, const void *buf, size_t len,
                      size_t sparse_map_size,
                      const ds_sparse_chunk_t *sparse_map,
                      bool punch_hole_supported);
  int (*close)(ds_file_t *file);
  void (*deinit)(ds_ctxt_t *ctxt);
  unsigned long long (*get_bytes_written)(const ds_ctxt_t *ctxt);
};

/* Supported datasink types */
typedef enum {
  DS_TYPE_STDOUT,
  DS_TYPE_FIFO,
  DS_TYPE_LOCAL,
  DS_TYPE_XBSTREAM,
  DS_TYPE_COMPRESS_QUICKLZ,
  DS_TYPE_COMPRESS_LZ4,
  DS_TYPE_COMPRESS_ZSTD,
  DS_TYPE_DECOMPRESS_QUICKLZ,
  DS_TYPE_DECOMPRESS_LZ4,
  DS_TYPE_DECOMPRESS_ZSTD,
  DS_TYPE_ENCRYPT,
  DS_TYPE_DECRYPT,
  DS_TYPE_TMPFILE,
  DS_TYPE_BUFFER
} ds_type_t;

/************************************************************************
Create a datasink of the specified type */
ds_ctxt_t *ds_create(const char *root, ds_type_t type);

/************************************************************************
Open a datasink file.  This is the user-facing entry point: the returned
file inherits ctxt->metrics so writes through it are accounted in the
bound ds_metrics instance (typically only the four backup heads -- data,
uncompressed_data, redo, meta -- have a non-null metrics binding). */
ds_file_t *ds_open(ds_ctxt_t *ctxt, const char *path, MY_STAT *stat);

/************************************************************************
Open a datasink file for wrapper-internal use (compress, buffer, encrypt,
tmpfile, xbstream).  Same as ds_open() but the returned file's metrics is
forced to NULL so passthrough writes from a wrapper into its inner
dest_file are not double-counted when the inner ctxt also happens to be
a metrics-bound head (e.g. ds_local serving both ds_data's compressed
leaf and ds_uncompressed_data's bypass top). */
ds_file_t *ds_open_internal(ds_ctxt_t *ctxt, const char *path, MY_STAT *stat);

/************************************************************************
Write to a datasink file.
@return 0 on success, 1 on error. */
int ds_write(ds_file_t *file, const void *buf, size_t len);

/************************************************************************
Check if sparse files are supported.
@return 1 if yes. */
int ds_is_sparse_write_supported(ds_file_t *file);

/************************************************************************
Write sparse chunk if supported.
@return 0 on success, 1 on error. */
int ds_write_sparse(ds_file_t *file, const void *buf, size_t len,
                    size_t sparse_map_size, const ds_sparse_chunk_t *sparse_map,
                    bool punch_hole_supported);

/************************************************************************
Close a datasink file.
@return 0 on success, 1, on error. */
int ds_close(ds_file_t *file);

/************************************************************************
Destroy a datasink handle */
void ds_destroy(ds_ctxt_t *ctxt);

/************************************************************************
Set the destination pipe for a datasink (only makes sense for compress and
tmpfile). */
void ds_set_pipe(ds_ctxt_t *ctxt, ds_ctxt_t *pipe_ctxt);

const char *ds_type_to_str(const datasink_t *ds);

const ds_ctxt_t *ds_leaf(const ds_ctxt_t *ctxt);

#ifdef __cplusplus
} /* extern "C" */
#endif

/** Global aggregate metrics for any head pipeline whose ctxt has been
bound to it (see ds_ctxt_t::metrics).  xb_backup_metrics.get_bytes() is
reported as uncompressed_size when --compress is used. */
extern ds_metrics xb_backup_metrics;

#endif /* XB_DATASINK_H */
