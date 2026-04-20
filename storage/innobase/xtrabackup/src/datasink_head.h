/******************************************************
Copyright (c) 2026 Percona LLC and/or its affiliates.

Head datasink: a thin pass-through ctxt that sits at the top of each
backup pipeline (data, uncompressed_data, redo, meta) and de-aliases
the metrics binding from the underlying real pipeline.

Design summary
==============

The legacy code aliased ds_data / ds_redo / ds_meta / ds_uncompressed_data
directly onto whatever ctxt happened to be at the top of each chain
(compress_lz4_ctxt, encrypt_ctxt, ds_local, ds_xbstream, ...).  Worse,
the same underlying ctxt could be reused across multiple heads
(canonical case: ds_uncompressed_data == ds_local, which is also the
leaf of ds_data's compress chain).  Setting metrics directly on those
shared ctxts caused double-counting.

A head ctxt fixes this by acting as a pipe-only proxy:

  ds_data ─► head_ctxt ─pipe_ctxt─► real_pipeline_top
                   │                          │
                   │                          └─► ... ─► leaf
                   └─► metrics binding lives here

Key property: head_open() does NOT allocate its own ds_file_t.  It
opens the real pipeline via ds_open(head->pipe_ctxt, ...) and returns
the inner stage's file unchanged, then optionally overrides
file->metrics from head_ctxt->metrics.  Because the returned file's
datasink/ctxt point at the inner stage (set by ds_init_file in that
*_open), every subsequent ds_write / ds_write_sparse / ds_close on
that file dispatches directly into the inner stage with zero head
function in the call path -- the head is invisible on the per-byte
write path, by construction.

Consequence: datasink_head's write / write_sparse / close /
get_bytes_written slots are all NULL.  If anything ever holds a
ds_file_t whose datasink points at &datasink_head, that is a bug in
the head_open contract and the next ds_write will crash through a
null function pointer (caught as a SIGSEGV in test).

Roles
=====

Each backup has exactly four heads, one per logical pipeline.  The
role enum is carried in the head's private ctxt for diagnostics only;
it does not change any code path. */

#ifndef XB_DATASINK_HEAD_H
#define XB_DATASINK_HEAD_H

#include "datasink.h"

/** Logical role of a head ctxt.  Diagnostics only -- the head
behaviour does not branch on this; the only thing distinguishing one
head from another at runtime is which real pipeline it points at via
ctxt->pipe_ctxt and which ds_metrics it is bound to via ctxt->metrics. */
enum ds_head_role_t {
  DS_HEAD_DATA = 0,
  DS_HEAD_REDO,
  DS_HEAD_META,
  DS_HEAD_UNCOMPRESSED_DATA
};

/** The head datasink vtable.  Only init/open/deinit are non-null;
write / write_sparse / close / get_bytes_written are deliberately NULL
because head_open returns the inner stage's file untouched -- no
ds_file_t ever ends up with file->datasink == &datasink_head. */
extern datasink_t datasink_head;

/** Allocate and initialize a head ctxt.  The returned ctxt is wired to
the head datasink vtable but has no pipe yet -- the caller must call
ds_set_pipe(head_ctxt, real_pipeline_top) before opening any file
through it.  metrics defaults to NULL; bind via head_ctxt->metrics =
&xb_backup_metrics if accounting is desired for this head. */
ds_ctxt_t *ds_create_head(const char *root, ds_head_role_t role);

/** Read the role tag from a head ctxt (diagnostics).  Caller must
ensure ctxt->datasink == &datasink_head. */
ds_head_role_t ds_head_get_role(const ds_ctxt_t *head_ctxt);

/** Human-readable name for a head role, for log/error messages. */
const char *ds_head_role_to_str(ds_head_role_t role);

#endif /* XB_DATASINK_HEAD_H */
