# Clone-style copy strategy in XtraBackup (`--copy-strategy=clone`)

Status: DRAFT for review
Branch: `pxb-copy-strategy-page-tracking` (stacked on `pxb-ddl-tracking-component`)
Depends on: `--ddl-tracking=server` (server DDL journal, PS-11427) for DDL
discovery; this document covers only *how data is copied*.
Related: `pxb-delta-backup-design.md` (fuller background), MySQL clone.

---

## 1. Goal

### 1.1 The problem

Classic XtraBackup copies the **entire redo stream** for the whole backup and
parses it record by record. The redo volume is proportional to
`backup_duration × write_rate`, which causes:

1. A huge `xtrabackup_logfile`.
2. A long `--prepare` (apply the whole stream).
3. A keep-up race: if redo generation outruns the copy thread, the backup
   aborts.

MySQL clone does not have these problems because it copies data in **three
stages** — full file copy, then a page-delta pass for pages that changed during
the copy, then only a **short** redo tail — so the redo it keeps is bounded by
the length of the copy, not by the whole backup.

### 1.2 The idea

`--copy-strategy=clone` ports clone's three-stage model to XtraBackup while
keeping XtraBackup's product surface (streaming, xbcloud, compression/encryption
pipelines, incrementals, PITR, no recipient instance):

1. **File copy** — copy all tablespace files, unlocked, as today.
2. **Page-delta copy** — recopy just the pages that changed during the file
   copy (from a server page-tracking session), written as XtraBackup
   `.delta`/`.meta` files.
3. **Short redo tail** — copy redo only for the bounded window `[C1, L]`
   (checkpoint at switch → stop LSN), block-level, no record parsing.

The on-disk result after `--prepare` is a normal full backup.

### 1.3 Relationship to `--ddl-tracking`

Data copy and DDL discovery are separate axes. `--copy-strategy=clone` handles
*data*; it relies on `--ddl-tracking=server` to learn about *DDL* during the
backup (create/drop/rename/import/…), because it no longer parses redo to find
them. `clone` therefore requires the server DDL journal — it is the payoff that
makes moving DDL discovery off redo worthwhile (see the DDL-tracking design).

---

## 2. High-level design

```
start page-tracking session (S = tracking start LSN)     ── server retains redo ≥ checkpoint
[file copy — unlocked, no redo parsing]
LOCK INSTANCE FOR BACKUP
cut: request checkpoint so checkpoint ≥ S; fix C1 = checkpoint;
     materialize tracked page-id list [S, C1]
consume DDL journal → fixups (.del/.ren/.new/recopy)     (see --ddl-tracking design)
page-delta copy: read tracked pages from disk → .delta/.meta   (skip superseded, §4.5)
L = log_status.lsn                                       ── captured AFTER all recopy (I2)
copy redo tail until it covers [C1, L]; stop
stop page-tracking session
UNLOCK INSTANCE

--prepare: apply fixups → apply deltas → apply redo up to exactly L (§5)
```

Redo kept = `[C1, L]`, bounded by the copy/lock window, not the whole backup.

---

## 3. Correctness invariants

These four rules carry the design; every change should be checked against them.

**I1 — Redo tail starts at a checkpoint.** The tail begins at `C1` = the server
checkpoint LSN at switch time, never an arbitrary position. By the checkpoint
invariant, every modification with LSN `< C1` is flushed to disk by the time the
checkpoint reaches `C1`; the server always retains redo ≥ checkpoint, so the
bytes at `C1` are readable.

**I2 — No page in the backup is newer than the stop LSN `L`.** `L` is captured
only *after* all page recopying (deltas + DDL-fixup recopies) completes. Pages
newer than `C1` are fine (redo apply skips per-page via `FIL_PAGE_LSN`); a page
newer than `L` would leave cross-page inconsistency nothing can repair. Order is
always: **recopy → capture L → stop redo at L**.

**I3 — Tracking completeness fence.** Page tracking records at *flush* time, so
the tracked set for `[S, X]` is complete only once checkpoint ≥ X. The cut
therefore queries `[S, C1]` with `C1 = checkpoint` and requires `C1 ≥ S`
(actively requesting a checkpoint rather than polling). Consequences fall out
for free: pages dirtied before S but unflushed are tracked at their later flush;
pages flushed before S were on disk before file copy read them; pages unflushed
at cut have mod-LSN ≥ C1 and are covered by the redo tail. No lost writes, no
flush-list sweep, file copy starts immediately.

**I4 — Exact stop at `L`.** Prepare applies redo up to exactly `L`. `L` pairs
with the `log_status` snapshot (binlog file+pos, GTID, wsrep seqno); applying
past `L` would desync the data from the recorded replication coordinates.

Per-page case analysis (page P, disk image LSN x, later modified at m):
`m ≥ C1` → in redo tail ✔; `m < C1` flushed after S → tracked, in delta ✔;
`m < C1` flushed before S → on disk before file copy read it, x ≥ m ✔.

---

## 4. Design

### 4.1 Page-tracking session (server)

Reuses `arch_page_sys` unchanged, with a **non-durable** client (like clone):

- Tracking already active → **attach** (borrow); never stop or purge what we
  did not start.
- Not active → start a non-durable session; stop + release at backup end. When
  the group loses its last reference the on-disk tracking files are deleted; a
  crashed backup leaves no residue.
- Sessions have IDs + heartbeat; start reaps expired sessions (GC for a PXB
  killed mid-backup).
- Durable/persistent tracking (auto-restart, retention) stays an explicit
  opt-in for incremental chaining, never a side effect of a backup.

### 4.2 The cut

Under the lock, at `backup_tracking_cut()` the server:

1. Requests a checkpoint if checkpoint `< S` and waits (bounded — this flushing
   was owed anyway; after a long file copy it is usually already done).
2. Fixes `C1 = checkpoint LSN` and materializes the tracked page-id list
   `[S, C1]` to a file (a superset is safe — recopying an extra page is
   harmless).

The component does **not** write delta files (see §4.3).

### 4.3 Page-delta copy — PXB-side, from disk

XtraBackup reads the tracked pages **from the data files on disk** using the
normal backup read path (checksum validation + retry for in-flux pages) and
writes `.delta`/`.meta` files into a `#xb_delta` directory in the backup. No
buffer pool is involved: by I1/I3 every tracked modification below `C1` is
flushed, so the on-disk image is current up to `C1`, and anything newer is in
the redo tail.

Encrypted, zip, and page-compressed spaces ship their on-disk bytes **as-is**,
exactly like the main file copy — no re-encryption, no flush-time transforms, no
special cases. (A buffer-pool-served delta path is a possible later
optimization if profiling justifies re-opening the encryption/compression
questions.)

### 4.4 Short redo tail

The tail `[C1, L]` is a **block-level copy only** (checksum/header validation,
block-level decrypt if redo encryption is on) — no record-level parsing at
backup time. All record semantics live in `--prepare`, which uses the embedded,
version-matched server recovery code. Everything the old parser discovered at
backup time (file ops, index load, encryption changes, redo-disable) now arrives
via the DDL journal instead.

### 4.5 Supersedence

A space that is recopied in full under the lock (new table, index-load recopy,
reimport, rename) must **not** also have an older delta applied over it —
applying an older delta page over a newer full copy would lose the window
between them (redo below `C1` is gone). The delta pass therefore skips
`{new_tables ∪ recopy_tables ∪ drops ∪ renames}` and runs *before*
`handle_ddl_operations()`, so superseded deltas are simply never written — no
prepare-side rule needed.

Renamed spaces are **fully recopied in v1**: the delta pass reads through PXB's
fil nodes, whose names are stale after a server-side rename, so renames are
routed through the recopy-under-lock path (reopen by current name). Correct and
reuses tested machinery, at the cost of a full copy of a renamed table (a Phase
2 optimization can write the delta under the new name instead).

### 4.6 Locking

The backup lock (`LOCK INSTANCE FOR BACKUP`, DDL-only) is **kept**. Lock-free is
theoretically possible (a convergence loop) but has no termination bound under
DDL-heavy load and multiplies race-supersedence proofs. A Phase 2 **pre-lock
delta pre-pass** can write the bulk of the delta before the lock (safe: the redo
copy from `C1` already covers concurrent changes), leaving only catch-up pages +
frozen DDL backlog under the lock, so lock-held time is seconds regardless of
delta size.

---

## 5. Prepare

`--prepare` order (mostly existing machinery, one new rule):

1. Apply DDL fixups (`.del`/`.ren`/`.new`, honor `.reimport`) — reinitializes
   the fil system.
2. Apply the `#xb_delta` `.delta`/`.meta` files over the copied bases, then
   remove `#xb_delta`.
3. Apply the redo tail up to **exactly `L`** (existing clamp).

The new rule is supersedence (§4.5), handled at backup time by not writing
superseded deltas, so prepare needs no special case.

---

## 6. Mode selection

Explicit opt-in, no adaptive mode: `--copy-strategy=redo` (default, classic) or
`--copy-strategy=clone`. `clone` requires the server page-tracking + DDL journal
and `--ddl-tracking=server`; it composes with both `--lock-ddl=REDUCED` and
`--lock-ddl=ON`.

Guidance (documentation, not code): `clone` wins on hot working sets and long
backups (redo grows with time × TPS while the page set saturates); classic
`redo` remains better for wide single-touch workloads (a per-page touch's redo
is far cheaper than a 16 KB page recopy).

---

## 7. Failure modes

| Event | Behavior |
|---|---|
| Server restart mid-backup | Backup aborts; non-durable tracking group self-deletes; no recovery attempted. |
| PXB killed mid-backup | Session heartbeat expires; next start reaps it; archiver refcounting reclaims disk. |
| `ALTER INSTANCE DISABLE INNODB REDO_LOG` | `SYSTEM_REDO_DISABLE` journal event → abort with a clear error. |
| Checkpoint far behind at cut | Active checkpoint request; bounded wait; progress logged. |
| Datadir short on space for deltas | Pre-check + status variable; the delta burst size is the tracked-set size, known before writing. |

---

## 8. User interface / deliverables

- **New client option:** `--copy-strategy=redo|clone` (default `redo`).
- **Requires:** `--ddl-tracking=server` and a Percona Server providing page
  tracking + the backup DDL journal UDFs. `clone` errors out otherwise.
- **Backup-directory artifact:** a `#xb_delta` directory holding intermediate
  `.delta`/`.meta` files, consumed and removed during `--prepare`.
- Streaming, xbcloud, compression, and encryption pipelines are unchanged
  (deltas flow through them like any copied file).

---

## 9. Known limitations (v1)

- Renamed spaces are fully recopied (§4.5).
- Deltas are read from disk, not the buffer pool (§4.3) — pays checksum-retry
  like any external read.
- Lock is held for the cut + under-lock recopy window; the pre-lock pre-pass
  that shortens it is Phase 2 (§4.6).

---

## 10. Testing

`deltabackup` suite: `basic_operation`, `incremental_chain`, `encryption`,
`undo_ddl`, `rename_after_copy`, `lock_ddl_on`, `import_ddl`, `validation`
(plus the DDL-tracking tests inherited from the base branch). Each runs a full
backup → prepare → copy-back → verify against a source `mysqldump`.
