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
ds_write() / ds_write_sparse() whenever the owning ds_file_t's ctxt has a
non-null metrics pointer bound at init time.  Today all four heads
(data, uncompressed_data, redo, meta) share this one ds_metrics instance;
per-pipeline breakdowns can be introduced later without any framework
changes -- just point a head's ctxt->metrics at a different instance.
New counters (files, pages, elapsed time, ...) are added as new fields on
ds_metrics and ds_ctxt_t is never touched. */
ds_metrics xb_backup_metrics;

/************************************************************************
Create a datasink of the specified type */
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
    ctxt->pipe_ctxt = NULL;
    /* The *_init() implementations allocate ds_ctxt_t with my_malloc
    (no MY_ZEROFILL), so the struct's default member initializers do not
    run.  Normalize framework-owned fields here so callers can rely on
    metrics being null unless explicitly bound by xtrabackup_init_datasinks. */
    ctxt->metrics = nullptr;
  } else {
    msg("Error: failed to initialize datasink.\n");
    exit(EXIT_FAILURE);
  }

  return ctxt;
}

/************************************************************************
Open a datasink file.  Pure dispatcher: invokes the ctxt's *_open and
returns whatever it returns, untouched.

Field initialization (datasink, ctxt, metrics) is the responsibility of
each *_open() implementation that allocates a ds_file_t -- via the
inline ds_init_file() helper in datasink.h.  ds_open() must not
post-initialize the returned file, otherwise wrappers that intentionally
return an already-initialized file from a deeper *_open (e.g. a future
head datasink that returns its inner stage's file unchanged so the head
is invisible on the per-byte write path) would have their carefully set
fields stomped, and writes would jump through the wrong vtable. */
ds_file_t *ds_open(ds_ctxt_t *ctxt, const char *path, MY_STAT *stat) {
  return ctxt->datasink->open(ctxt, path, stat);
}

/************************************************************************
Write to a datasink file.
@return 0 on success, 1 on error. */
int ds_write(ds_file_t *file, const void *buf, size_t len) {
  if (file->metrics != nullptr) {
    file->metrics->add_bytes(len);
  }
  return file->datasink->write(file, buf, len);
}

/************************************************************************
Check if sparse files are supported.
@return 1 if yes. */
int ds_is_sparse_write_supported(ds_file_t *file) {
  if (file->datasink->write_sparse != nullptr) {
    return 1;
  }
  return 0;
}

/************************************************************************
Write sparse chunk if supported.
@return 0 on success, 1 on error. */
int ds_write_sparse(ds_file_t *file, const void *buf, size_t len,
                    size_t sparse_map_size, const ds_sparse_chunk_t *sparse_map,
                    bool punch_hole_supported) {
  if (file->datasink->write_sparse != nullptr) {
    /* Account only bytes that actually occupy disk -- the packed payload
    (sum of chunk lengths), not the logical extent len which includes
    sparse holes that will not consume disk blocks. */
    if (file->metrics != nullptr) {
      size_t packed_len = 0;
      for (size_t i = 0; i < sparse_map_size; i++) {
        packed_len += sparse_map[i].len;
      }
      file->metrics->add_bytes(packed_len);
    }
    return file->datasink->write_sparse(file, buf, len, sparse_map_size,
                                        sparse_map, punch_hole_supported);
  }
  return 1;
}

/************************************************************************
Close a datasink file.
@return 0 on success, 1, on error. */
int ds_close(ds_file_t *file) { return file->datasink->close(file); }

/************************************************************************
Destroy a datasink handle */
void ds_destroy(ds_ctxt_t *ctxt) { ctxt->datasink->deinit(ctxt); }

/************************************************************************
Set the destination pipe for a datasink (only makes sense for compress and
tmpfile). */
void ds_set_pipe(ds_ctxt_t *ctxt, ds_ctxt_t *pipe_ctxt) {
  ctxt->pipe_ctxt = pipe_ctxt;
}

/** Walk a datasink pipeline to its terminal (leaf) node.
Follows the pipe_ctxt chain from the given context until
it reaches the datasink that has no further downstream pipe.
For a pipeline like: statistics -> compress -> buffer -> local,
calling ds_leaf on the statistics context returns the local context.
@param[in]  ctxt  any datasink context in the pipeline
@return the leaf (terminal) datasink context, or ctxt itself if it
has no pipe_ctxt */
const ds_ctxt_t *ds_leaf(const ds_ctxt_t *ctxt) {
  while (ctxt && ctxt->pipe_ctxt) ctxt = ctxt->pipe_ctxt;
  return ctxt;
}

/** Return a human-readable name for a datasink vtable pointer.
@param[in]  ds  datasink vtable pointer
@return static string identifying the datasink type, or "unknown" */
const char *ds_type_to_str(const datasink_t *ds) {
  if (ds == &datasink_local) return "local";
  if (ds == &datasink_stdout) return "stdout";
  if (ds == &datasink_fifo) return "fifo";
  if (ds == &datasink_xbstream) return "xbstream";
  if (ds == &datasink_compress) return "compress(quicklz)";
  if (ds == &datasink_compress_lz4) return "compress(lz4)";
  if (ds == &datasink_compress_zstd) return "compress(zstd)";
  if (ds == &datasink_encrypt) return "encrypt";
  if (ds == &datasink_tmpfile) return "tmpfile";
  if (ds == &datasink_buffer) return "buffer";
  return "unknown";
}
