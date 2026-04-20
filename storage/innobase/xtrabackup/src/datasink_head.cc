/******************************************************
Copyright (c) 2026 Percona LLC and/or its affiliates.

Implementation of the head datasink.  See datasink_head.h for the
design rationale and the (non-)contract on the write/close slots. */

#include "datasink_head.h"

#include <my_sys.h>
#include <mysql/service_mysql_alloc.h>

#include "common.h"
#include "msg.h"

namespace {

struct ds_head_ctxt_priv_t {
  ds_head_role_t role;
};

ds_ctxt_t *head_init(const char *root) {
  auto *ctxt = new ds_ctxt_t;
  auto *priv = new ds_head_ctxt_priv_t;

  /* Default role; ds_create_head() overwrites this immediately.
  Initialized here so a head ctxt is always in a defined state even
  if someone bypasses ds_create_head() (which they should not). */
  priv->role = DS_HEAD_DATA;

  ctxt->ptr = priv;
  ctxt->root = my_strdup(PSI_NOT_INSTRUMENTED, root, MYF(MY_FAE));
  /* datasink, pipe_ctxt, fs_support_punch_hole, metrics are set by
  ds_create() / xtrabackup_init_datasinks (datasink) and by the
  caller (everything else). */
  return ctxt;
}

ds_file_t *head_open(ds_ctxt_t *head_ctxt, const char *path, MY_STAT *mystat) {
  /* A head with no pipe is unusable -- xtrabackup_init_datasinks must
  call ds_set_pipe before opening any file through the head. */
  xb_ad(head_ctxt->pipe_ctxt != nullptr);

  /* Open the inner chain and return its file as-is.  We deliberately
  do NOT allocate a head wrapper file and we deliberately do NOT call
  ds_init_file() here -- the inner stage's *_open already initialized
  file->datasink/ctxt to the inner stage's vtable, and we want to keep
  it that way so writes/sparse-checks/closes dispatch directly into
  the inner stage with no head function in the call path.  (If we
  stomped file->datasink to &datasink_head here, ds_write would jump
  to the head's NULL write slot and crash.) */
  ds_file_t *file = ds_open(head_ctxt->pipe_ctxt, path, mystat);
  if (file == nullptr) {
    return nullptr;
  }

  /* Tag the returned file with this head's metrics binding so
  ds_write / ds_write_sparse account every byte the caller writes
  through this file.  Inner ctxts (compress, encrypt, buffer,
  xbstream, local, ...) have ctxt->metrics == NULL, so the inner
  *_open's ds_init_file already left file->metrics at NULL; this
  override fires only because we are coming through a head whose
  ctxt->metrics was bound at init_datasinks time.  Wrappers cannot
  re-fire metrics deeper in the chain because their inner opens go
  through ds_open(inner_ctxt) and inner_ctxt->metrics == NULL. */
  file->metrics = head_ctxt->metrics;

  return file;
}

void head_deinit(ds_ctxt_t *ctxt) {
  delete static_cast<ds_head_ctxt_priv_t *>(ctxt->ptr);
  my_free(ctxt->root);
  delete ctxt;
}

}  // namespace

datasink_t datasink_head = {
    &head_init,
    &head_open,
    /* write          = */ nullptr,
    /* write_sparse   = */ nullptr,
    /* close          = */ nullptr,
    &head_deinit,
    /* get_bytes_written = */ nullptr,
};

ds_ctxt_t *ds_create_head(const char *root, ds_head_role_t role) {
  ds_ctxt_t *ctxt = head_init(root);
  if (ctxt == nullptr) {
    msg("Error: failed to initialize head datasink (role=%s).\n",
        ds_head_role_to_str(role));
    exit(EXIT_FAILURE);
  }
  ctxt->datasink = &datasink_head;
  ctxt->pipe_ctxt = nullptr;
  static_cast<ds_head_ctxt_priv_t *>(ctxt->ptr)->role = role;
  return ctxt;
}

ds_head_role_t ds_head_get_role(const ds_ctxt_t *head_ctxt) {
  xb_ad(head_ctxt->datasink == &datasink_head);
  return static_cast<const ds_head_ctxt_priv_t *>(head_ctxt->ptr)->role;
}

const char *ds_head_role_to_str(ds_head_role_t role) {
  switch (role) {
    case DS_HEAD_DATA:
      return "data";
    case DS_HEAD_REDO:
      return "redo";
    case DS_HEAD_META:
      return "meta";
    case DS_HEAD_UNCOMPRESSED_DATA:
      return "uncompressed_data";
  }
  return "unknown";
}
