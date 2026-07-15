# Bring MySQL clone's three-stage copy to XtraBackup (`--copy-strategy=clone`)

Status: DRAFT for review
Branch: `pxb-copy-strategy-page-tracking` (stacked on `pxb-ddl-tracking-component`)
Depends on: the server page-tracking component; and, under `--lock-ddl=REDUCED`,
the backup DDL journal (`--ddl-tracking=server`, PS-11427). This document covers
only *how data is copied*; §5 covers the supported option combinations.
Related: `pxb-delta-backup-design.md` (fuller background), MySQL clone.

---

## 1. Introduction

### 1.1 The problem

Classic XtraBackup copies the **entire redo stream** for the whole backup and
parses it record by record. The redo volume is proportional to
`backup_duration × write_rate`, which causes:

1. A huge `xtrabackup_logfile`.
2. A long `--prepare` (apply the whole stream).
3. A keep-up race: if redo generation outruns the copy thread, the backup
   aborts.

MySQL clone avoids all three because it copies data in **three stages** — a full
file copy, then a page-delta pass for the pages that changed *during* that copy,
then only a **short** redo tail — so the redo it keeps is bounded by the length
of the page-delta copy phase, not by the whole backup.

### 1.2 The idea

`--copy-strategy=clone` ports clone's three-stage model to XtraBackup while
keeping XtraBackup's product surface (streaming, xbcloud, compression/encryption
pipelines, incrementals, PITR, no recipient instance):

1. **File copy** — copy all tablespace files, unlocked, as today.
2. **Page-delta copy** — recopy just the pages that changed during the file
   copy (from a server page-tracking session), as `.delta`/`.meta` files.
3. **Short redo tail** — copy (and parse) redo only for the bounded window
   `[C1, L]` (see §1.4); a bounded volume instead of the whole-backup stream.

After `--prepare` the on-disk result is a normal full backup.

### 1.3 Two independent axes

Data copy and DDL discovery are separate axes. `--copy-strategy` decides *how
data is copied* (`redo` = classic full-redo, `clone` = three-stage). It no
longer parses redo to *discover* file operations, so under `--lock-ddl=REDUCED`
it leans on `--ddl-tracking=server` to learn about DDL that runs during the copy;
under `--lock-ddl=ON` the instance lock blocks DDL, so there is nothing to
discover. Moving DDL discovery off redo is what makes the short tail possible.
§5 lists the combinations that are actually allowed.

### 1.4 LSN landmarks (`S`, `C1`, `L`)

Three log-sequence numbers anchor everything below. They always occur in the
order **`S ≤ C1 ≤ L`**:

- **`S` — page-tracking start LSN.** Where page tracking is turned on, at the
  *start* of the backup (returned by `set_page_tracking(1)`). Everything the
  backup must treat as "changed during the copy" is measured from `S`.
- **`C1` — the cut checkpoint LSN.** The server checkpoint LSN fixed *after* the
  file copy, once the checkpoint has advanced to `≥ S`. It is both the top of the
  tracked page set `[S, C1]` **and** the start of the redo tail. Every change
  below `C1` is already flushed to disk (I1), so the on-disk page images the
  delta pass reads are current up to `C1`.
- **`L` — the stop LSN.** `log_status.lsn`, captured *after* all recopying is
  done. It is the end of the redo tail; `--prepare` applies redo up to **exactly
  `L`** (I4), the point that matches the recorded replication coordinates.

So the **page delta carries `[S, C1]`** and the **redo tail carries `[C1, L]`**;
together they account for every change from tracking-start to the consistent
stop point.

---

## 2. How a clone backup runs — `--lock-ddl=ON`

`ON` is the simplest case, so it shows the core mechanism cleanly: the instance
backup lock is taken **up front and held for the whole backup**, so no DDL can
happen and there is nothing to reconcile. What still changes during the long,
multi-threaded file copy is ordinary **DML** — and that is exactly what the
page-delta and short redo tail capture.

1. **Take the backup lock up front.** `LOCK INSTANCE FOR BACKUP` is issued
   before any data file is read and held to the end. It blocks DDL only; DML
   proceeds normally.
2. **Do *not* start the redo-copy thread.** Unlike `--copy-strategy=redo`, clone
   does not stream redo from the start. No checkpoint LSN is noted here either —
   the only landmark fixed so far is `S`, next. *(Answering "do we note the
   checkpoint at lock time?" — no; `C1` is fixed only at step 6.)*
3. **Start page tracking; record `S`.** After scanning and loading the
   tablespaces, XtraBackup turns on the server page tracking, which returns the
   start LSN `S`. From now on the server records every page the instance flushes.
4. **Copy all data files — multi-threaded.** `--parallel` threads copy every
   tablespace file in full, as a classic backup does; redo is not being copied.
   Because DML runs concurrently, some copied pages are already stale by the time
   they land in the backup — the next two stages fix that.
5. **Wait for the checkpoint to cross `S`.** Page tracking records a page at
   *flush* time, so the changed-page list for a range is complete only once the
   checkpoint has passed the top of the range. XtraBackup **passively polls**
   `log_status` until `checkpoint ≥ S`. (It does not force a checkpoint; §6.4.)
6. **Fix `C1` and start the redo-copy thread.** With `checkpoint ≥ S`, XtraBackup
   starts the redo copy; its start point is the current checkpoint `C1`. The tail
   now runs in the background, keeping up through the rest of the backup.
7. **Ask page tracking for the changed pages in `[S, C1]`.** These are the pages
   flushed between tracking-start and the cut — precisely the ones the full file
   copy may have captured stale.
8. **Page-delta copy — multi-threaded — into `#xb_page_delta/`.** `--parallel`
   `copy_for_delta` threads read those tracked pages from the data files **on
   disk** and write `.delta`/`.meta` files under the backup's `#xb_page_delta`
   directory (§5.3). *(Answering "multiple threads?" — yes, same `--parallel`
   degree as the file copy.)*
9. **Capture `L`.** After the delta copy finishes, XtraBackup reads the server's
   current LSN as the stop LSN `L`, together with the replication coordinates
   (binlog pos, GTID, wsrep seqno).
10. **Stop the redo tail at `L`; release the lock.** The redo copy is told to
    stop exactly at `L`, so the tail covers `[C1, L]`. The lock is released.
    Backup done.

The three outputs compose cleanly: the file copy is a whole-instance image
current as of the copy; the `[S, C1]` delta refreshes everything that changed
during it; the `[C1, L]` redo tail carries the rest to a single consistent stop
point. `--prepare` replays them in that order (§4). The redo kept is only
`[C1, L]` — the span of the delta copy — never the whole backup duration.

---

## 3. How a clone backup runs — `--lock-ddl=REDUCED` + `--ddl-tracking=server`

`REDUCED` changes exactly one thing: **DDL is allowed to run during the unlocked
file copy**, so XtraBackup must discover and reconcile it. It does so with the
server's backup DDL journal (which is why `--ddl-tracking=server` is required).
The three-stage copy and the `S`/`C1`/`L` windows are **identical** to §2; only
the DDL handling is added. Relative to §2:

- **The backup lock is taken *late*, not up front.** The entire file copy runs
  unlocked (that is the point of REDUCED); the instance lock is taken only near
  the end, inside the cut, just before the delta.
- **A DDL journal session is opened at the start** (`innodb_backup_ddl_journal_start`),
  so the server records every DDL that occurs during the unlocked copy.
- Steps 3–6 of §2 — page-tracking `S`, the multi-threaded file copy, the
  checkpoint wait, and starting the redo tail at `C1` — happen **unchanged and
  unlocked**.
- **Then the cut runs under the (late) lock**, in this order:
  1. Take `LOCK INSTANCE FOR BACKUP`; flush engine logs and record the DDL fence;
     wait for the redo thread to catch up to that point.
  2. **Cut and consume the DDL journal** (`…_cut → consume_ddl_journal → …_stop`)
     to learn every create / drop / rename / import / bulk-load that happened
     during the copy.
  3. **Page-delta copy `[S, C1]`** — exactly as §2 step 8, but with a *skip set*:
     spaces the DDL fixups will fully recopy or delete (new tables, index-load
     recopies, drops, renames) are excluded, so a stale delta is never written
     over a fresh full copy (§6.2).
  4. **Apply the DDL fixups** (`handle_ddl_operations`): `.del` for drops, `.ren`
     for renames, `.new`/full recopy for created, rebuilt or imported spaces.
     This runs *after* the delta copy because it reinitialises the fil layer.
- **Capture `L` and stop the redo tail at `L`**, as in §2.

In short: **REDUCED = §2 + a DDL journal session + a late lock + (consume journal
→ fixups) wrapped around the delta copy.** The copy mechanics are the same.

---

## 4. Prepare

`--prepare` order (mostly existing machinery, one new rule):

1. Apply DDL fixups (`.del`/`.ren`/`.new`, honour `.reimport`) — reinitialises
   the fil system.
2. Apply the `#xb_page_delta` `.delta`/`.meta` files over the copied bases, then
   remove `#xb_page_delta`.
3. Apply the redo tail up to **exactly `L`**.

The only new rule is supersedence (§6.2), handled at backup time by not writing
superseded deltas, so prepare needs no special case for it.

---

## 5. User interface & on-disk layout

### 5.1 Options and requirements

- **New client option:** `--copy-strategy=redo|clone` (default `redo`).
- **Lock mode:** requires `--lock-ddl=REDUCED` or `--lock-ddl=ON` (not `OFF`).
- **Full backups only** (not incremental — see below).
- **Requires** a Percona Server providing the page-tracking component (for the
  changed-page set). Under `--lock-ddl=REDUCED` it *additionally* requires the
  backup DDL journal (`--ddl-tracking=server`, or `auto` when the UDFs are
  present); under `--lock-ddl=ON` the journal is not used.
- Streaming, xbcloud, compression and encryption pipelines are unchanged — the
  `.delta`/`.meta` files flow through them like any copied file.

**Full backups only.** `--copy-strategy` applies only to *full* backups and the
combination with `--incremental-*` is rejected at startup. A full backup's cost
is dominated by copying entire tablespace files over a long, unlocked window —
the phase clone's three stages exist to bound. An incremental backup does not
copy full files at all: it already ships only the pages changed since the base
(via `--page-tracking` or a changed-page scan), so it is short and generates
little redo. There is no long full-copy window to shorten and no runaway redo to
bound — an incremental is, in effect, already "page-delta only."

### 5.2 Supported combinations

| Combination | Result |
|---|---|
| `clone` + `--lock-ddl=ON` | **Works.** The lock blocks DDL for the whole backup, so no DDL journal is needed. |
| `clone` + `--lock-ddl=REDUCED` + `--ddl-tracking=server` | **Works.** DDL during the unlocked copy is discovered via the server DDL journal. |
| `clone` + `--lock-ddl=REDUCED` + `--ddl-tracking=redo` | **Rejected.** Redo-based DDL tracking needs the full-duration redo stream; clone's tail starts at `C1` (after the file copy) and would miss DDL from during the copy. |
| `clone` + `--lock-ddl=OFF` | **Rejected.** No way to reconcile concurrent DDL. |

(`clone` always needs the page-tracking component for the changed-page set; the
difference above is only whether a *DDL* journal is also required.) Validated by
`deltabackup/validation.sh`.

### 5.3 On-disk layout

The full file copy lands at the top level of the backup exactly as today. The
page-delta stage writes intermediate files under a single directory,
`#xb_page_delta`, mirroring the datadir tree:

```
backup/
  ibdata1                              ← full file copy (stage 1)
  sbtest/
    t1.ibd
    t2.ibd
  #xb_page_delta/                           ← page-delta stage (stage 2), intermediate
    sbtest/
      t1.ibd.delta                     ← changed pages in [S, C1]
      t1.ibd.meta                      ← delta metadata (page size, space id, …)
      t2.ibd.delta
      t2.ibd.meta
  xtrabackup_logfile                   ← redo tail [C1, L] (stage 3)
  xtrabackup_checkpoints, backup-my.cnf, … (standard metadata)
```

- `.delta`/`.meta` use the **same format as incremental backups** (same write
  filter); a `.delta` holds the changed 16 KB pages and its `.meta` the page
  size / space id / row format needed to apply them.
- The `#` prefix keeps these intermediate files **out of the full backup's
  top-level namespace**; `--prepare` applies them over the bases and then deletes
  `#xb_page_delta`, leaving a normal full backup.
- The directory is named `#xb_page_delta` (constant `XB_PAGE_DELTA_DIR`) — the
  `page_delta` distinguishing it from *incremental* "delta" backups, even though
  the per-file `.delta`/`.meta` format is shared with them.

---

## 6. Notes & rationale

Supporting detail behind the walkthroughs above.

### 6.1 Correctness invariants

**I1 — Redo tail starts at a checkpoint.** The tail begins at `C1` = a server
checkpoint, never an arbitrary LSN. Every modification `< C1` is flushed by the
time the checkpoint reaches `C1`, and the server always retains redo ≥
checkpoint, so the bytes at `C1` are readable.

**I2 — No page in the backup is newer than `L`.** `L` is captured only *after*
all recopying (deltas + DDL-fixup recopies). Pages newer than `C1` are fine (redo
apply skips per-page via `FIL_PAGE_LSN`); a page newer than `L` would be
unrepairable. Order is always **recopy → capture `L` → stop redo at `L`**.

**I3 — Tracking completeness fence.** Page tracking records at *flush* time, so
`[S, X]` is complete only once checkpoint ≥ X. The cut uses `C1 = checkpoint` and
requires `C1 ≥ S`, reached by passively polling (§6.4). Then: pages dirtied
before `S` but unflushed are tracked at their later flush; pages flushed before
`S` were on disk before the file copy read them; pages unflushed at the cut have
mod-LSN ≥ `C1` and are covered by the redo tail.

**I4 — Exact stop at `L`.** Prepare applies redo up to exactly `L`, which pairs
with the `log_status` snapshot (binlog pos, GTID, wsrep seqno); applying past `L`
would desync data from the recorded coordinates.

Per-page check (page P, disk image LSN x, later modified at m): `m ≥ C1` → in
redo tail ✔; `m < C1` flushed after `S` → tracked, in delta ✔; `m < C1` flushed
before `S` → on disk before file copy read it, x ≥ m ✔.

### 6.2 Supersedence and renames

A space recopied in full under the lock (new table, index-load recopy, reimport)
must **not** also get an older delta applied over it — that would lose the window
between them (redo below `C1` is gone). The delta pass therefore skips
`{new_tables ∪ recopy_tables ∪ drops}` and runs *before* `handle_ddl_operations`,
so superseded deltas are never written — no prepare-side rule needed.

**Renames are not recopied.** A rename is a `.ren` marker (no local file move at
backup time — streaming-friendly). The space keeps its cheap delta, and at
prepare the `.ren` handler renames its `.delta`/`.meta` alongside the `.ibd`.

### 6.3 Encrypted tablespaces created during the copy

Because the tail starts at `C1`, redo written before `C1` is not parsed, so an
encryption key whose `MLOG_WRITE_STRING` predates `C1` is absent from
`recv_sys->keys`. This bites the file-per-table two-phase encrypted create
(`CREATE TABLE … ENCRYPTION='Y'`): page 0 is flushed to disk with the encryption
*flag* set but *without* the key (the key goes only to redo and a still-dirty
buffer-pool page 0). A file copy that reads page 0 in that window cannot decode
the key and defers the space into `invalid_encrypted_tablespace_ids`. (General
`ALTER TABLESPACE … ENCRYPTION='Y'` writes flag and key to page 0 in one mtr and
flushes it — no such window.)

The deferral self-heals: the DDL journal reports the create, so the space is
recopied under the lock; by then page 0 has been flushed with the key, so
`Datafile::validate_first_page` succeeds and removes the id from the deferred
set. The end-of-backup `validate_missing_encryption_tablespaces()` then sees
nothing missing. The removal is eager (on successful re-validation) rather than
waiting for the redo-key drain, so it also hardens `--copy-strategy=redo`; the
shared set is mutex-protected because `validate_first_page` can run on parallel
copy threads.

### 6.4 Page tracking is persistent; the checkpoint wait is passive

v1 reuses the **existing persistent page tracking** (`mysqlbackup.udf_set_page_tracking(1)`,
the same infra `--page-tracking` incrementals use), not a clone-style per-backup
session. It is server-global and durable, **not stopped at backup end** (the
incremental-chain model). The cut wait (§2 step 5) is **passive**: XtraBackup
polls for `checkpoint ≥ S` and does not force a checkpoint, so on a low-write
server the wait is bounded only by the server's own checkpointing. A clone-style
non-durable session and an actively-driven cut are future work (§8).

### 6.5 Choosing `redo` vs `clone`

The two strategies trade **redo volume** against **page recopy**; the choice
follows the write pattern (guidance, not adaptive code):

- **`redo` wins when many *distinct* pages change once** — a change is a small
  redo record (tens–hundreds of bytes), far cheaper than recopying a 16 KB page.
  A wide single-touch sweep (batch `UPDATE`/load over a large table) makes many
  changed pages but little redo each.
- **`clone` wins when the *same* pages change repeatedly** — redo records every
  modification (~N records for N touches), while clone copies the page once
  (16 KB) regardless. Crossover ≈ when a page's cumulative redo exceeds one page.
  It also wins on long backups, where redo grows with time while the changed set
  saturates.
- **`clone` also makes `--prepare` far faster** — a bounded redo tail to replay,
  plus multi-threaded `.delta` apply instead of single-stream redo apply.

Rule of thumb: hot / write-heavy / long-running → `clone`; wide one-pass
scans/loads → `redo`.

### 6.6 Failure modes

| Event | Behaviour |
|---|---|
| Server restart mid-backup | Backup aborts; persistent page tracking survives with the server (v1 does not tear it down); the aborted backup is not recovered. |
| PXB killed mid-backup | Page tracking keeps running (persistent, §6.4) — no automatic stop/cleanup; managed as with incremental tracking. Heartbeat GC is future work (§8). |
| `ALTER INSTANCE ROTATE INNODB MASTER KEY` mid-backup | Keyring reloaded under the backup lock before the redo-tail / tablespace-reopen reads, so re-wrapped headers decrypt with the new master key. |
| `ALTER INSTANCE DISABLE INNODB REDO_LOG` | `SYSTEM_REDO_DISABLE` journal event → abort with a clear error. |
| Checkpoint far behind at cut | Passive wait: poll until `checkpoint ≥ S` (§6.4); progress logged; PXB does not force a checkpoint. |
| Datadir short on space for deltas | Pre-check + status variable; the delta burst size is the tracked-set size, known before writing. |

---

## 7. Known limitations (v1)

- Deltas are read from disk, not the buffer pool (§2 step 8) — pays checksum-retry
  like any external read, and requires the checkpoint wait.
- Server-side page tracking is **left running after the backup** (§6.4); it is
  not turned off automatically.
- The cut wait is **passive** (§6.4): PXB does not force a checkpoint, so on a
  low-write server the wait for `checkpoint ≥ S` tracks the server's own
  checkpointing.

---

## 8. Future improvements

- **Shrink (or drop) the lock window with a pre-lock delta pre-pass.** Most of
  the page recopy — and the full recopies of new / index-load-rebuilt tables —
  can run *before* the backup lock (safe: the redo copy from `C1` already covers
  concurrent changes, vanishing files are skipped and superseded by journal
  events). Only catch-up pages and the frozen DDL backlog remain under the lock,
  so lock-held time is seconds regardless of delta size.
- **Buffer-pool-served deltas (clone parity).** Remove the checkpoint wait by
  sourcing changed pages from the InnoDB buffer pool — the current image — rather
  than waiting for them to flush and re-reading from disk. Two shapes:
  **(a)** a `mysqlbackup`-component UDF that dumps the changed BP pages to disk
  for XtraBackup to pick up (keeps the file-based read path); **(b)** stream BP
  pages straight into the XtraBackup output stream, which relays to `xbcloud` /
  cloud (no intermediate disk, closest to clone). Both re-open the
  encryption/compression transform questions (BP pages are
  unencrypted/uncompressed and would need the space's transforms re-applied).
- **Clone-style non-durable tracking session.** Start/stop tracking per backup
  instead of reusing persistent incremental tracking: attach-if-active, heartbeat
  GC to reclaim a PXB killed mid-backup, and stop + release at backup end so a
  backup leaves no server-side residue.

---

## 9. Testing

`deltabackup` suite: `basic_operation`, `encryption`, `undo_ddl`,
`rename_after_copy`, `lock_ddl_on`, `import_ddl`, `validation` (plus the
DDL-tracking tests inherited from the base branch). Each runs a full backup →
prepare → copy-back → verify against a source `mysqldump`.
