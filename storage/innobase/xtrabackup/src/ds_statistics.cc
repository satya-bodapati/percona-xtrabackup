/******************************************************
Copyright (c) 2025 Percona LLC and/or its affiliates.

Statistics datasink implementation for XtraBackup.

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

#include "ds_statistics.h"

#include <atomic>

#include <my_base.h>
#include <mysql/service_mysql_alloc.h>
#include <mysql_version.h>

#include "common.h"
#include "datasink.h"

struct ds_stats_ctxt_t {
  std::atomic<unsigned long long> total_bytes{0};
};

struct ds_stats_file_t {
  ds_file_t *dest_file;
  ds_stats_ctxt_t *stats_ctxt;
};

static ds_ctxt_t *stats_init(const char *root);
static ds_file_t *stats_open(ds_ctxt_t *ctxt, const char *path,
                             MY_STAT *mystat);
static int stats_write(ds_file_t *file, const void *buf, size_t len);
static int stats_write_sparse(ds_file_t *file, const void *buf, size_t len,
                              size_t sparse_map_size,
                              const ds_sparse_chunk_t *sparse_map,
                              bool punch_hole_supported);
static int stats_close(ds_file_t *file);
static void stats_deinit(ds_ctxt_t *ctxt);
static unsigned long long stats_get_bytes_written(ds_ctxt_t *ctxt);

datasink_t datasink_statistics = {&stats_init,
                                  &stats_open,
                                  &stats_write,
                                  &stats_write_sparse,
                                  &stats_close,
                                  &stats_deinit,
                                  &stats_get_bytes_written};

static ds_ctxt_t *stats_init(const char *root) {
  ds_ctxt_t *ctxt = static_cast<ds_ctxt_t *>(
      my_malloc(PSI_NOT_INSTRUMENTED, sizeof(ds_ctxt_t), MYF(MY_FAE)));

  ds_stats_ctxt_t *stats = new (
      my_malloc(PSI_NOT_INSTRUMENTED, sizeof(ds_stats_ctxt_t), MYF(MY_FAE)))
      ds_stats_ctxt_t();

  ctxt->ptr = stats;
  ctxt->root = my_strdup(PSI_NOT_INSTRUMENTED, root, MYF(MY_FAE));

  return ctxt;
}

static ds_file_t *stats_open(ds_ctxt_t *ctxt, const char *path,
                             MY_STAT *mystat) {
  ds_stats_ctxt_t *stats = static_cast<ds_stats_ctxt_t *>(ctxt->ptr);

  ds_file_t *dest_file = ds_open(ctxt->pipe_ctxt, path, mystat);
  if (dest_file == nullptr) {
    return nullptr;
  }

  ds_file_t *file = static_cast<ds_file_t *>(
      my_malloc(PSI_NOT_INSTRUMENTED, sizeof(ds_file_t), MYF(MY_FAE)));

  ds_stats_file_t *sf = static_cast<ds_stats_file_t *>(
      my_malloc(PSI_NOT_INSTRUMENTED, sizeof(ds_stats_file_t), MYF(MY_FAE)));

  sf->dest_file = dest_file;
  sf->stats_ctxt = stats;

  file->ptr = sf;
  file->path = dest_file->path;
  file->datasink = ctxt->datasink;

  return file;
}

static int stats_write(ds_file_t *file, const void *buf, size_t len) {
  ds_stats_file_t *sf = static_cast<ds_stats_file_t *>(file->ptr);
  sf->stats_ctxt->total_bytes.fetch_add(len, std::memory_order_relaxed);
  return ds_write(sf->dest_file, buf, len);
}

static int stats_write_sparse(ds_file_t *file, const void *buf, size_t len,
                              size_t sparse_map_size,
                              const ds_sparse_chunk_t *sparse_map,
                              bool punch_hole_supported) {
  ds_stats_file_t *sf = static_cast<ds_stats_file_t *>(file->ptr);

  /* Count only the actual data bytes from the sparse map, not the skipped
     (hole) portions.  The logical file on disk is larger due to seeks, but
     backup size should reflect bytes that are actually written. */
  size_t data_bytes = 0;
  for (size_t i = 0; i < sparse_map_size; ++i) data_bytes += sparse_map[i].len;

  sf->stats_ctxt->total_bytes.fetch_add(data_bytes, std::memory_order_relaxed);
  return ds_write_sparse(sf->dest_file, buf, len, sparse_map_size, sparse_map,
                         punch_hole_supported);
}

static int stats_close(ds_file_t *file) {
  ds_stats_file_t *sf = static_cast<ds_stats_file_t *>(file->ptr);
  int ret = ds_close(sf->dest_file);
  my_free(sf);
  my_free(file);
  return ret;
}

static void stats_deinit(ds_ctxt_t *ctxt) {
  ds_stats_ctxt_t *stats = static_cast<ds_stats_ctxt_t *>(ctxt->ptr);
  stats->~ds_stats_ctxt_t();
  my_free(stats);
  my_free(ctxt->root);
  my_free(ctxt);
}

static unsigned long long stats_get_bytes_written(ds_ctxt_t *ctxt) {
  ds_stats_ctxt_t *stats = static_cast<ds_stats_ctxt_t *>(ctxt->ptr);
  return stats->total_bytes.load(std::memory_order_relaxed);
}
