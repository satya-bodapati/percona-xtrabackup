# PXB Delta Backup — clone-style backup via server tracking component

Status: DRAFT for review
Related: PXB-2901 (evaluate page tracking for full backup), MySQL bug #110663,
lock-ddl=REDUCED, --page-tracking

## 1. Problem

PXB copies the full redo stream for the duration of the backup and parses it
record-by-record (DDL tracking, MLOG_INDEX_LOAD detection). On write-heavy
servers this causes:

1. Huge `xtrabackup_logfile` (redo volume ∝ backup duration × write rate).
2. Long prepare (parse + apply the whole stream).
3. A keep-up race: if redo generation outruns the copy/parse thread, the
   backup aborts.
4. A permanent maintenance liability: PXB's record-level redo parser must
   track every upstream redo format change, and encrypted redo drags keyring
   requirements into backup time.

MySQL clone does not have these problems because it uses a three-stage design:
FILE COPY → PAGE COPY (tracked page IDs) → REDO COPY (short tail). This
design ports clone's architecture to PXB while keeping PXB's product surface
(streaming, xbcloud, compression/encryption pipelines, incrementals, PITR,
no recipient instance needed).

## 2. High-level design

A new **Percona Server component** owns all change tracking. PXB stops doing
record-level redo parsing entirely in this mode.

```
 PXB                                  Server component
 ───                                  ────────────────
 backup_tracking_start() ──────────►  start non-durable page-track session
                                      (arch_page_sys client, like clone)
                                      + DDL event journal begins
        S = tracking start LSN  ◄──── returns {session_id, S}

 [file copy phase — NO redo activity at all: no copy, no parse.
  Redo consumer held at the server's checkpoint so [C1,∞) is retained.]

 file copy done
 backup_tracking_cut() ────────────►  request checkpoint (ensure C1 ≥ S),
                                      fsync DDL journal, materialize
                                      tracked page-ID list [S, C1]
        journal + page list    ◄────  returns {cut_lsn = C1, paths}
 start block-level redo copy from C1 (no record parsing, ever)
 LOCK INSTANCE FOR BACKUP
 apply reduced-lock fixups from journal (.del/.ren/recopy new tables)
 recopy tracked pages from disk → .delta/.meta (checksum-retry read path;
   skip spaces superseded by fixups/drops)
 L = log_status.lsn  (captured AFTER delta write completes)
 redo copy runs until it has [C1, L]; stop
 backup_tracking_stop() ───────────►  detach session; non-durable group
                                      auto-deletes tracking files
 UNLOCK INSTANCE
```

Prepare order (mostly existing machinery):

1. Reduced-lock file fixups (`.del` / `.ren` / new tables) — existing.
2. Apply `.delta` files — existing incremental apply code, plus one new
   supersedence rule (§7.3).
3. Apply redo [C1, L], clamped at exactly `to_lsn = L` — existing
   `to_lsn` clamp in the patched `recv_scan_log_recs` (log0recv.cc:3878).

## 3. Correctness invariants

These four rules carry the whole design. Every review of an implementation
change should check against them.

**I1 — Redo tail starts at a checkpoint.** The backup's redo begins at C1 =
server checkpoint LSN at switch time, never at an arbitrary parse/copy
position. Checkpoint invariant: every modification with LSN < C1 is flushed
to disk by the time the checkpoint reaches C1. Server always retains redo
≥ checkpoint, so the bytes at C1 are guaranteed readable.

**I2 — No page in the backup is newer than the stop LSN L.** L is captured
only after ALL page recopying (deltas, DDL-fixup recopies) has completed.
Pages newer than C1 are fine (redo apply skips per-page via FIL_PAGE_LSN);
pages newer than L would leave cross-page inconsistency that nothing can
repair. Order is always: recopy → capture L → stop redo at L.

**I3 — Tracking completeness fence.** Page tracking records at flush time
(IO layer — per clone author, this is spec, not implementation detail;
MySQL bug #110663 is invalid). The tracked set for [S, X] is complete only
when checkpoint ≥ X. Therefore the cut queries [S, C1] with C1 = checkpoint,
and requires C1 ≥ S — the component actively calls log_request_checkpoint
instead of PXB's current passive 1s polling (backup_copy.cc:1546).
Consequences that come free:
  - Pages dirtied *before* S but unflushed are tracked at their later flush.
  - Pages flushed before S were on disk before file copy read them.
  - Pages unflushed at cut have mod LSN ≥ C1 → covered by the redo tail.
  Together: no lost writes, no flush-list sweep needed, file copy starts
  immediately (the C1 ≥ S wait exists only at cut time).

**I4 — Exact stop at L.** Prepare applies redo up to exactly L (existing
clamp). L pairs with the log_status snapshot (binlog file+pos, GTID,
wsrep seqno). Applying past L would desync data state from the recorded
replication coordinates. Unchanged from today; the tail block past L still
ships (block checksums cover 512-byte blocks whole) and is clamped at
prepare, as today.

Case analysis for any page P copied during file copy with disk image at LSN x,
later modified at m:
  - m ≥ C1        → in redo tail. ✔
  - m < C1, flushed after S  → tracked → in delta; delta image ≥ m
    (checkpoint invariant at C1 read time, or BP-read is current). ✔
  - m < C1, flushed before S → on disk before file copy read P → x ≥ m. ✔

## 4. Server component

### 4.1 Page tracking session

Reuses `arch_page_sys` unchanged. Per-backup sessions use a **non-durable**
client (`is_durable = false`), exactly like clone (`m_page_ctx(false)`,
clone0snapshot.cc:62). Verified lifecycle: stop() + release() detaches the
client; when the group loses its last reference, `~Arch_Group` deletes the
on-disk tracking files (arch0page.cc:107); non-durable groups are not
recovered after restart and stale dirs are swept at archiver init. A crashed
backup leaves no residue.

Policy:
  - Tracking already active at backup start → attach (borrow politely);
    never stop or purge what we did not start. Durable + non-durable
    clients coexist on a group (arch0page.cc:2511/2624) — no special case.
  - Not active → start non-durable session; stop + release at backup end.
  - Persistent tracking (durable, auto-restart at boot, retention purge) is
    an explicit opt-in sysvar for page-tracking **incremental** chaining
    only. Never a side effect of running a backup.
  - Sessions have IDs + heartbeat; `backup_tracking_start()` reaps expired
    sessions (GC for PXB killed mid-backup).

### 4.2 DDL event journal

New thin broadcast added **inside the `Clone_notify` ctor/dtor** (before its
early-returns — SPACE_ALTER_INPLACE bails at clone0api.cc:2501 and must
still be journaled). The call sites fire unconditionally on every DDL
whether or not clone runs (constructor merely checks clone_sys state);
piggybacking means upstream merges keep the event map current automatically.

Event set = the `Clone_notify::Type` enum (clone0api.h:182): SPACE_CREATE /
DROP / RENAME / IMPORT, SPACE_ALTER_ENCRYPT(_GENERAL)(_FLAGS),
SPACE_ALTER_INPLACE, SPACE_ALTER_INPLACE_BULK (the MLOG_INDEX_LOAD
equivalent), SPACE_UNDO_DDL, SYSTEM_REDO_DISABLE (→ backup must abort).
This enum is literally the curated list of "what a physical backup must be
told" — clone is a physical backup.

Journal record: `(seqno, event_type, space_id, old_name, new_name,
begin_lsn, end_lsn)` — begin/end events from ctor/dtor bracket the true
record LSN (`log_get_lsn()` at append). Exact record LSN is unknowable at
notify time (assigned at mtr commit); consumers must rely on ordering +
presence + conservative bracket checks only. Per-space ordering is
guaranteed by MDL serialization. Append-only file under
`#ib_backup_tracking/`, fsynced at cut. Encryption-info changes and undo
truncate are journaled explicitly even though page tracking would cover
them — redundancy is cheap, diagnosing a missed rotation is not.

### 4.3 Delta creation: PXB-side from disk (component delta writer dropped)

Decision: the component does **not** write delta files. BP-served deltas
required re-encrypting plaintext frames for encrypted spaces and re-running
flush-time transforms for compressed/page-compressed spaces — complexity
not worth it. Instead, at `backup_tracking_cut()` the component only:

1. Calls `log_request_checkpoint` if checkpoint < S; waits (bounded — this
   flushing was owed anyway; after a long file copy it is usually done).
2. Fixes C1 = checkpoint LSN; materializes the tracked page-ID list
   [S, C1] to a file (existing page-track get-pages mechanics; a superset
   is fine — recopying an extra page is harmless).
3. Fsyncs and closes out the DDL journal segment.

**PXB reads the tracked pages from the data files on disk** with the normal
backup read path (checksum validation + retry for in-flux pages) and writes
PXB `.delta`/`.meta` files into the backup. Correctness needs no buffer
pool: every tracked modification below C1 is flushed by the checkpoint
invariant (I1/I3), so the on-disk image at recopy time is current up to C1,
and anything newer is covered by the redo tail. Encrypted, zip, and
page-compressed spaces ship their on-disk bytes as-is — exactly like the
main file copy, no transforms, no special cases.

Cost accepted: recopy is disk reads (no BP fast path) and pays
checksum-retry like any external read. BP-served deltas remain a possible
later optimization (§10 Phase 3) if profiling justifies re-opening the
encryption/compression questions.

## 5. PXB changes

- **No record-level redo parsing in component mode.** The tail [C1, L] is
  block-level copy only (`Redo_Log_Reader::scan_log_recs` checksum/hdr_no
  validation; block-level decrypt if redo encryption on). Record semantics
  live only in prepare, which uses the embedded server recovery code —
  version-matched by definition. Everything the parser currently discovers
  at backup time (INDEX_LOAD, file ops, encryption changes, redo-disable)
  arrives via the journal instead.
- Reduced-lock fixup engine (`ddl_tracker` consumers) is fed from the
  journal instead of the parser. Audit required: fixup decisions must not
  need exact record LSNs — brackets force the conservative branch (worst
  case a redundant recopy, never a miss).
- Backup lock is **kept** (see §7.1). Optional pre-lock delta pre-pass
  (§7.2) bounds lock-held time.
- Prepare: apply order fixups → deltas → redo, one new supersedence rule.

## 6. Mode selection: explicit only (no adaptive mode)

Decision: **no adaptive/auto mode** — rejected as too complicated. Delta
mode is an explicit opt-in and is a new flavor of reduced-lock mode: it is
valid only with `--lock-ddl=REDUCED` semantics (only DDL blocked, briefly),
requires the server component, and is off by default.

Workload guidance is documentation, not code: delta mode wins on hot
working sets and long backups (redo grows with time × TPS, the page set
saturates); classic mode remains better for wide single-touch workloads
(batch UPDATE over a huge table). Per-page-touch redo (~100–500 B) is
30–100× cheaper than a 16 KB page recopy — users pick the mode per their
workload.

## 7. Locking

### 7.1 Backup lock is kept

Lock-free is theoretically possible (convergence loop: handle DDL events,
re-check, stop when a pass is clean) but has no termination bound under
DDL-heavy load and multiplies race-supersedence proofs (TRUNCATE mid-read,
rename mid-read, INDEX_LOAD just under the stop LSN). `LOCK INSTANCE FOR
BACKUP` blocks only DDL, DML flows freely, and clone itself chose sync
points over full optimism. Revisit only as an opt-in if a workload can't
tolerate a seconds-long DDL pause.

### 7.2 Pre-lock delta pre-pass (Phase 2)

Bulk of the delta can be written before taking the lock (safe: redo copy
from C1 already running covers concurrent changes; vanishing files are
skipped and superseded by journal events). Under lock only: catch-up pages
+ frozen DDL backlog. Lock-held time becomes seconds regardless of delta
size.

### 7.3 Supersedence rule

A space fully recopied under lock (new table, INDEX_LOAD recopy) must have
its earlier delta skipped — applying an older delta page over a newer full
copy loses the window between them (redo below C1 is gone). Implementation:
the delta pass skips {new_tables ∪ recopy_tables ∪ drops ∪ renames} and runs
before handle_ddl_operations() (which reinitializes the fil system), so no
prepare-side rule is needed — superseded deltas are simply never written.

**Renamed spaces are fully recopied in v1**: the delta pass reads through
PXB's fil nodes, whose names are stale after a server-side rename (the old
path no longer exists on disk). Routing renames through the existing
recopy-under-lock path (which reopens by current name) is correct and
reuses tested machinery, at the cost of a full copy of a renamed table.
Phase 2 optimization: rename the fil node and write the delta under the
new name instead.

## 8. Failure modes

| Event | Behavior |
|---|---|
| Server restart mid-backup | Backup aborts; non-durable group self-deletes; no recovery attempted. Session UUID detects staleness at cut — never silently serve a truncated journal. |
| PXB killed mid-backup | Session heartbeat expires; next `backup_tracking_start()` reaps it; archiver refcounting reclaims disk. |
| `ALTER INSTANCE DISABLE INNODB REDO_LOG` | SYSTEM_REDO_DISABLE journal event → abort backup with clear error. |
| Checkpoint far behind at cut | Active checkpoint request; bounded wait; progress logged (replaces today's 1 s polling loop). |
| Redo outruns nothing | Keep-up race is gone in delta mode (no phase-1 record parsing; auto mode block-copies as today until/unless crossover). |
| Datadir short on space for deltas | Pre-check + status variable; delta burst size is the tracked-set size, known before writing. |

## 9. Support matrix

- Component ships with **Percona Server** only. Upstream MySQL targets keep
  exactly today's behavior (full redo copy; redo-parse REDUCED unchanged,
  frozen in scope). The delta feature is **not** reimplemented on the
  redo-parse path — one new path, one legacy path in maintenance mode.
- The record parser cannot be deleted while upstream REDUCED is supported;
  the win is that the *new* feature doesn't deepen the bet on it.
- Long shot: contribute the DDL-notify service upstream (page-track service
  already is), which would eventually dissolve the split.

## 10. Phasing

### Phase 1 — component + delta backup (the feature)

Server (Percona Server, branch ps-delta-backup):
  - DDL journal: broadcast in Clone_notify ctor/dtor (before its
    early-returns), bracketed-LSN append-only journal under
    `#ib_backup_tracking/`, active only while a session is on,
    fsync-at-cut. Includes SYSTEM_REDO_DISABLE.
  - Session UDFs in component_mysqlbackup: backup_tracking_start
    (non-durable arch client + journal on, returns S) /
    backup_tracking_cut (log_request_checkpoint for C1 ≥ S, fsync
    journal, materialize page-ID list [S, C1], returns C1) /
    backup_tracking_stop. Single active session is acceptable for v1;
    stale-session guard minimal.
  - NO delta writer in the component (§4.3).
PXB (branch pxb-delta-backup) — a new version of reduced-lock mode,
gated on `--lock-ddl=REDUCED`:
  - No record-level redo parsing, no phase-1 redo copy; consumer held at
    checkpoint; block-level tail copy [C1, L].
  - Journal consumer feeds the existing ddl_tracker maps → fixup engine
    (`handle_ddl_operations`) reused unchanged.
  - Delta recopy from disk (checksum-retry) → `.delta`/`.meta` +
    supersedence (skip spaces fully recopied by fixups / dropped).
  - Prepare: fixups → deltas (from the backup dir itself) → redo from C1
    clamped at exactly L.
Exit criteria: correctness test matrix (§11) green; delta-mode backup of a
write-heavy workload shows the expected redo/prepare reduction.

### Phase 2 — lifecycle + lock window

  - Pre-lock delta pre-pass; lock-held time target: seconds.
  - Full session hygiene: heartbeat + GC, format-version handshake,
    multi-session arbitration.
  - Persistent-tracking opt-in (durable session, boot auto-restart,
    retention purge) + wire page-tracking incrementals to the component.
  - Page-compressed delta handling aligned with PXB-3671.

### Phase 3 — parity polish (each item independent)

  - BP-served delta writer in the component (re-opens encryption /
    compressed-frame questions; only if profiling justifies it).
  - Skip never-used extents during file copy (server-side info).
  - Concurrency autotune / bandwidth throttling parity with clone.

## 11. Verification list & test plan

Pre-implementation audits:
  1. arch0page: confirm bulk-load pages (flush-observer writes) pass the
     IO-layer tracking hook; confirm entry attribution within a group makes
     [S, C1] a superset query. (Flush-time semantics + completeness fence
     already confirmed by clone author.)
  2. ddl_tracker consumers: no decision requires exact record LSN; brackets
     always degrade to the conservative branch.

Targeted tests (each maps to an invariant or wrinkle):
  - Autoinc bump during phase 1, restore, verify counter (dynamic metadata
    persisted before checkpoint → I1/I3 interplay).
  - Encryption: master-key rotation and tablespace re-encryption during
    backup; encrypted-space delta round-trip (re-encrypt path).
  - Bulk index build (ALTER ADD INDEX) landing just before cut and just
    after lock — page-granular recopy via tracking + journal event.
  - Undo truncate during phase 1.
  - Short backup + large checkpoint age: cut-time checkpoint request path
    (today's sleep scenario; DBUG hook exists —
    `page_tracking_checkpoint_behind`).
  - Replica provisioning from delta-mode backup: GTID/binlog coordinates
    exact at L (I4).
  - auto-mode crossover under synthetic hot-spot vs single-touch workloads;
    verify never-crossed == byte-comparable to today's backup.
  - Crash PXB mid-backup → session GC; restart server mid-backup → clean
    abort + no tracking residue.
  - Page-compressed and zip tablespaces through delta apply.

## 12. Open questions

1. α default and whether auto mode should also weigh prepare-time (RTO)
   explicitly or leave that to the tunable.
2. Delta spool location for streamed backups (datadir tracking dir vs
   direct hand-off) — disk headroom story for very large deltas.
3. Journal + delta directory naming/permissions; SELinux/AppArmor notes.
4. Exact SQL surface: UDFs (precedent: mysqlbackup component) vs admin
   commands; privilege = BACKUP_ADMIN.
5. Upstream contribution of the DDL-notify service — file the intent?
