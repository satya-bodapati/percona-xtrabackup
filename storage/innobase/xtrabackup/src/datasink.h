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

/** Aggregate xtrabackup counters accumulated by the datasink framework.
add_uncomp_size() is called from ds_write() / ds_write_sparse() when
the owning file has tracking enabled; it records the raw
(pre-compression) byte count of each write into the instance bound to
that file.  Encapsulates its own synchronization so new counters can
pick their own primitive without changing the write path.  Not a
datasink -- lives alongside ds_file_t and is referenced by an
optional pointer on each file. */
struct xb_metrics {
  void add_uncomp_size(uint64_t n) {
    uncomp_size_.fetch_add(n, std::memory_order_relaxed);
  }
  uint64_t get_uncomp_size() const {
    return uncomp_size_.load(std::memory_order_relaxed);
  }

 private:
  std::atomic<uint64_t> uncomp_size_{0};
};

typedef struct ds_ctxt {
  datasink_t *datasink;
  char *root;
  void *ptr;
  struct ds_ctxt *pipe_ctxt;
  bool fs_support_punch_hole = false;
} ds_ctxt_t;

typedef struct {
  void *ptr;
  char *path;
  datasink_t *datasink;
  ds_ctxt_t *ctxt = nullptr;
  /* Optional metrics binding.  NULL by default; set by the top-level
  caller via ds_track_metrics() (or the ds_tracked_open()
  convenience helper).  When non-null, ds_write / ds_write_sparse add
  the raw byte count of each write to *metrics. */
  xb_metrics *metrics = nullptr;
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
Open a datasink file.  Pure dispatcher; the returned file has metrics
tracking disabled (file->metrics == NULL).  Callers that want per-byte
accounting use ds_tracked_open() or call ds_track_metrics() on
the returned file. */
ds_file_t *ds_open(ds_ctxt_t *ctxt, const char *path, MY_STAT *stat);

/** Initialize framework-owned fields on a freshly allocated ds_file_t.
Every *_open() implementation must call this once before returning the
file.  Most *_open implementations my_malloc() the ds_file_t (no
MY_ZEROFILL), so the struct's default member initializers do not run. */
static inline void ds_init_file(ds_file_t *file, ds_ctxt_t *ctxt) {
  file->datasink = ctxt->datasink;
  file->ctxt = ctxt;
  file->metrics = nullptr;
}

/** Start tracking per-file byte counts into *metrics for a ds_file_t
returned by ds_open().  After this call, every ds_write /
ds_write_sparse on this file adds its raw byte count to *metrics.
Safe to call with a null file (no-op). */
static inline void ds_track_metrics(ds_file_t *file, xb_metrics *metrics) {
  if (file != nullptr) {
    file->metrics = metrics;
  }
}

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

/** Open a top-level backup output file.  Same as ds_open() but, when
--compress is in effect, also calls ds_track_metrics(file,
&xb_backup_metrics) so writes to this file accumulate pre-compression
bytes into the global counter.  Use at backup-side top-level ds_open
sites on ds_data / ds_redo / ds_meta / ds_uncompressed_data;
pipeline-internal opens inside wrappers keep calling ds_open()
directly so each logical byte is counted exactly once.

Defined only in the xtrabackup binary (refers to xtrabackup_compress);
standalone tools (xbstream, xbcrypt) must not call this. */
ds_file_t *ds_tracked_open(ds_ctxt_t *ctxt, const char *path, MY_STAT *stat);

#ifdef __cplusplus
} /* extern "C" */
#endif

/** Global aggregate metrics for the backup pipelines.  Reported as
uncompressed_backup_size in the xtrabackup error log and
xtrabackup_info when --compress is used. */
extern xb_metrics xb_backup_metrics;

#endif /* XB_DATASINK_H */
