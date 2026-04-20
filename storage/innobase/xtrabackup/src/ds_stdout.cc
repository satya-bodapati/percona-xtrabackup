/******************************************************
Copyright (c) 2013 Percona LLC and/or its affiliates.

Local datasink implementation for XtraBackup.

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

#include <my_base.h>
#include <mysql/service_mysql_alloc.h>
#include <mysys_err.h>
#include <atomic>

#include "common.h"
#include "datasink.h"

typedef struct {
  std::atomic<unsigned long long> bytes_written{0};
} ds_stdout_ctxt_t;

typedef struct {
  File fd;
  ds_stdout_ctxt_t *stdout_ctxt;
} ds_stdout_file_t;

static ds_ctxt_t *stdout_init(const char *root);
static ds_file_t *stdout_open(ds_ctxt_t *ctxt, const char *path,
                              MY_STAT *mystat);
static int stdout_write(ds_file_t *file, const void *buf, size_t len);
static int stdout_close(ds_file_t *file);
static void stdout_deinit(ds_ctxt_t *ctxt);
static unsigned long long stdout_get_bytes_written(const ds_ctxt_t *ctxt);

datasink_t datasink_stdout = {&stdout_init,
                              &stdout_open,
                              &stdout_write,
                              nullptr,
                              &stdout_close,
                              &stdout_deinit,
                              &stdout_get_bytes_written};

static ds_ctxt_t *stdout_init(const char *root) {
  ds_ctxt_t *ctxt;

  ctxt = static_cast<ds_ctxt_t *>(
      my_malloc(PSI_NOT_INSTRUMENTED, sizeof(ds_ctxt_t), MYF(MY_FAE)));

  ds_stdout_ctxt_t *stdout_ctxt = new ds_stdout_ctxt_t{};
  ctxt->ptr = stdout_ctxt;
  ctxt->root = my_strdup(PSI_NOT_INSTRUMENTED, root, MYF(MY_FAE));

  return ctxt;
}

static ds_file_t *stdout_open(ds_ctxt_t *ctxt,
                              const char *path __attribute__((unused)),
                              MY_STAT *mystat __attribute__((unused))) {
  ds_stdout_file_t *stdout_file;
  ds_file_t *file;
  size_t pathlen;
  const char *fullpath = "<STDOUT>";

  pathlen = strlen(fullpath) + 1;

  file = (ds_file_t *)my_malloc(
      PSI_NOT_INSTRUMENTED,
      sizeof(ds_file_t) + sizeof(ds_stdout_file_t) + pathlen, MYF(MY_FAE));
  stdout_file = (ds_stdout_file_t *)(file + 1);

#ifdef __WIN__
  setmode(fileno(stdout), _O_BINARY);
#endif

  stdout_file->fd = fileno(stdout);
  stdout_file->stdout_ctxt = (ds_stdout_ctxt_t *)ctxt->ptr;

  file->path = (char *)stdout_file + sizeof(ds_stdout_file_t);
  memcpy(file->path, fullpath, pathlen);

  file->ptr = stdout_file;

  ds_init_file(file, ctxt);
  return file;
}

static int stdout_write(ds_file_t *file, const void *buf, size_t len) {
  auto stdout_file = (ds_stdout_file_t *)file->ptr;
  File fd = stdout_file->fd;

  if (!my_write(fd, static_cast<const uchar *>(buf), len,
                MYF(MY_WME | MY_NABP))) {
    stdout_file->stdout_ctxt->bytes_written.fetch_add(
        len, std::memory_order_relaxed);
    posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
    return 0;
  }

  return 1;
}

static int stdout_close(ds_file_t *file) {
  my_free(file);

  return 0;
}

static void stdout_deinit(ds_ctxt_t *ctxt) {
  auto *stdout_ctxt = (ds_stdout_ctxt_t *)ctxt->ptr;
  stdout_ctxt->bytes_written.store(0, std::memory_order_relaxed);
  delete stdout_ctxt;
  my_free(ctxt->root);
  my_free(ctxt);
}

static unsigned long long stdout_get_bytes_written(const ds_ctxt_t *ctxt) {
  return static_cast<const ds_stdout_ctxt_t *>(ctxt->ptr)->bytes_written.load(
      std::memory_order_relaxed);
}
