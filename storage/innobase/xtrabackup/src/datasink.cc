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

#include "datasink.h"
#include <my_base.h>
#include <atomic>
#include <cassert>
#include <cstdint>
#include "common.h"
#include "ds_buffer.h"
#include "ds_compress.h"
#include "ds_compress_lz4.h"
#include "ds_compress_zstd.h"
#include "ds_decompress.h"
#include "ds_decompress_lz4.h"
#include "ds_decompress_zstd.h"
#include "ds_decrypt.h"
#include "ds_encrypt.h"
#include "ds_fifo.h"
#include "ds_local.h"
#include "ds_stdout.h"
#include "ds_tmpfile.h"
#include "ds_xbstream.h"
#include "msg.h"

/** Global aggregate metrics for the backup pipelines.  Incremented by
ds_write() / ds_write_sparse() whenever the file's metrics pointer is
non-null.  Armed at top-level backup open sites via ds_tracked_open()
with xb_get_metrics() (xtrabackup.h), which returns &xb_backup_metrics
when --compress is active and nullptr otherwise. */
xb_metrics xb_backup_metrics;

/** See datasink.h for contract.
@param[in] root  root path / destination the datasink writes into
@param[in] type  which datasink to instantiate
@return a datasink context, or nullptr for unknown type. */
ds_ctxt_t *ds_create(const char *root, ds_type_t type) {
  datasink_t *ds;
  ds_ctxt_t *ctxt;

  switch (type) {
    case DS_TYPE_STDOUT:
      ds = &datasink_stdout;
      break;
    case DS_TYPE_FIFO:
      ds = &datasink_fifo;
      break;
    case DS_TYPE_LOCAL:
      ds = &datasink_local;
      break;
    case DS_TYPE_XBSTREAM:
      ds = &datasink_xbstream;
      break;
    case DS_TYPE_COMPRESS_QUICKLZ:
      ds = &datasink_compress;
      break;
    case DS_TYPE_COMPRESS_LZ4:
      ds = &datasink_compress_lz4;
      break;
    case DS_TYPE_COMPRESS_ZSTD:
      ds = &datasink_compress_zstd;
      break;
    case DS_TYPE_DECOMPRESS_QUICKLZ:
      ds = &datasink_decompress;
      break;
    case DS_TYPE_DECOMPRESS_LZ4:
      ds = &datasink_decompress_lz4;
      break;
    case DS_TYPE_DECOMPRESS_ZSTD:
      ds = &datasink_decompress_zstd;
      break;
    case DS_TYPE_ENCRYPT:
      ds = &datasink_encrypt;
      break;
    case DS_TYPE_DECRYPT:
      ds = &datasink_decrypt;
      break;
    case DS_TYPE_TMPFILE:
      ds = &datasink_tmpfile;
      break;
    case DS_TYPE_BUFFER:
      ds = &datasink_buffer;
      break;
    default:
      msg("Unknown datasink type: %d\n", type);
      xb_ad(0);
      return NULL;
  }

  ctxt = ds->init(root);
  if (ctxt != NULL) {
    ctxt->datasink = ds;
    /* Per-datasink init() routines use my_malloc() which does not zero
    memory, so fields not explicitly assigned by init() (notably
    pipe_ctxt and fs_support_punch_hole) can hold garbage.  Normalize
    them here so every caller sees a clean ds_ctxt_t regardless of
    which leaf/wrapper produced it.  In particular ds_leaf() relies on
    pipe_ctxt == nullptr at the terminal node to stop walking, and
    callers that query fs_support_punch_hole on non-local leaves
    (stdout/fifo/xbstream) must see a sane default. */
    ctxt->pipe_ctxt = nullptr;
    ctxt->fs_support_punch_hole = false;
  } else {
    msg("Error: failed to initialize datasink.\n");
    exit(EXIT_FAILURE);
  }

  return ctxt;
}

/** Pure dispatcher.  Each *_open() initializes the framework-owned
fields on its freshly allocated ds_file_t via ds_init_file() (see
datasink.h).  The debug assertion catches any *_open implementation
that forgets to call ds_init_file(): ds_write / ds_close would later
chase a NULL datasink/ctxt and crash with no clue about the root
cause.
@param[in] ctxt  pipeline to open through
@param[in] path  path relative to the pipeline root
@param[in] stat  size/mode hints for downstream datasinks
@return newly opened file, or nullptr on error. */
ds_file_t *ds_open(ds_ctxt_t *ctxt, const char *path, MY_STAT *stat) {
  ds_file_t *file = ctxt->datasink->open(ctxt, path, stat);
  assert(file == nullptr ||
         (file->datasink != nullptr && file->ctxt != nullptr));
  return file;
}

/** ds_open() + ds_track_metrics(), in one call.  See the declaration
in datasink.h for when to prefer this over a raw ds_open().
@param[in]     ctxt     pipeline to open through
@param[in]     path     path relative to the pipeline root
@param[in]     stat     size/mode hints for downstream datasinks
@param[in,out] metrics  metrics instance to bind; nullptr disables tracking
@return newly opened file, or nullptr on error. */
ds_file_t *ds_tracked_open(ds_ctxt_t *ctxt, const char *path, MY_STAT *stat,
                           xb_metrics *metrics) {
  ds_file_t *file = ds_open(ctxt, path, stat);
  ds_track_metrics(file, metrics);
  return file;
}

/** Write a contiguous buffer through the owning datasink's write slot.
Adds @p len to file->metrics only on success so a failed write never
overcounts logical bytes.
@param[in,out] file  ds_file_t previously returned by ds_open
@param[in]     buf   bytes to write
@param[in]     len   number of bytes at @p buf
@return 0 on success, 1 on error. */
int ds_write(ds_file_t *file, const void *buf, size_t len) {
  const int rc = file->datasink->write(file, buf, len);
  if (rc == 0 && file->metrics != nullptr) {
    file->metrics->add_uncomp_size(len);
  }
  return rc;
}

/** Check whether @p file's datasink supports sparse writes.
@param[in] file  file to probe
@return 1 if write_sparse is implemented, 0 otherwise. */
int ds_is_sparse_write_supported(ds_file_t *file) {
  if (file->datasink->write_sparse != nullptr) {
    return 1;
  }
  return 0;
}

/** Forward a sparse chunk through the owning datasink's write_sparse
slot.  Adds the sum of sparse_map[i].len (the packed payload, holes
excluded) to file->metrics only on success so a failed write never
overcounts logical bytes.
@param[in,out] file                  ds_file_t previously returned by ds_open
@param[in]     buf                   packed buffer containing the data bytes
@param[in]     len                   size of @p buf
@param[in]     sparse_map_size       number of entries in @p sparse_map
@param[in]     sparse_map            per-chunk (skip, len)
@param[in]     punch_hole_supported  true if the destination filesystem
                                     can physically punch holes
@return 0 on success, 1 on error. */
int ds_write_sparse(ds_file_t *file, const void *buf, size_t len,
                    size_t sparse_map_size, const ds_sparse_chunk_t *sparse_map,
                    bool punch_hole_supported) {
  if (file->datasink->write_sparse == nullptr) {
    return 1;
  }
  const int rc = file->datasink->write_sparse(file, buf, len, sparse_map_size,
                                              sparse_map, punch_hole_supported);
  if (rc == 0 && file->metrics != nullptr) {
    /* Count the packed payload only: holes do not occupy disk. */
    size_t packed_len = 0;
    for (size_t i = 0; i < sparse_map_size; i++) {
      packed_len += sparse_map[i].len;
    }
    file->metrics->add_uncomp_size(packed_len);
  }
  return rc;
}

/** Close a datasink file.
@param[in,out] file  ds_file_t previously returned by ds_open
@return 0 on success, 1 on error. */
int ds_close(ds_file_t *file) { return file->datasink->close(file); }

/** Destroy a datasink handle.
@param[in,out] ctxt  datasink ctxt returned by ds_create / ds_set_pipe */
void ds_destroy(ds_ctxt_t *ctxt) { ctxt->datasink->deinit(ctxt); }

/** Wire @p ctxt's output into @p pipe_ctxt.
@param[in,out] ctxt       wrapper datasink context
@param[in,out] pipe_ctxt  next-stage datasink the wrapper writes into */
void ds_set_pipe(ds_ctxt_t *ctxt, ds_ctxt_t *pipe_ctxt) {
  ctxt->pipe_ctxt = pipe_ctxt;
}

/** Walk a datasink pipeline to its terminal node by following
pipe_ctxt.
@param[in] ctxt  any node in a pipeline
@return the leaf ctxt, or @p ctxt itself when it has no pipe_ctxt. */
const ds_ctxt_t *ds_leaf(const ds_ctxt_t *ctxt) {
  while (ctxt && ctxt->pipe_ctxt) ctxt = ctxt->pipe_ctxt;
  return ctxt;
}
