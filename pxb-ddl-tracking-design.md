# Use server DDL tracker in XtraBackup (`--ddl-tracking=server`)

Status: DRAFT for review
Branch: `pxb-ddl-tracking-component`
Code: `storage/innobase/xtrabackup/src/ddl_tracker.{cc,h}`, `xtrabackup.cc`,
`backup_copy.cc`, `fil/fil0fil.cc`, `log/log0recv.cc`
Server side: PS-11427 (backup DDL journal component)
Related: `--lock-ddl=REDUCED`

---

## 1. Goal

### 1.1 The problem

`--lock-ddl=REDUCED` lets DDL run during most of the backup, holding the
instance lock only for a short final window. To stay consistent, XtraBackup
must discover every tablespace-level DDL that happened during the unlocked
phase (create, drop, rename, import, encryption change, bulk index build, undo
DDL) and reconcile the copied files with it.

Today XtraBackup discovers these by **parsing the redo log** it copies for the
whole backup, decoding `MLOG_FILE_*` / `MLOG_INDEX_LOAD` records. That parser
is a maintenance liability (every redo-format change), forces
keyring/encrypted-redo handling into backup time, and is fragile for operations
that bypass the redo-logged path — especially `IMPORT` (which emits no
`MLOG_FILE_CREATE`) and undo DDL.

### 1.2 The idea

Percona Server now records those DDL events to a backup DDL journal (PS-11427).
This feature makes XtraBackup **consume that journal** instead of parsing redo
for file operations, under `--lock-ddl=REDUCED`. The redo parser remains as the
fallback for servers that do not provide the journal.

**This is deliberately a small change.** It only swaps the *source* of DDL
events — from redo-record parsing to reading the journal. Everything downstream
is the existing reduced-lock machinery: the same in-memory tracker maps, the
same `.del` / `.ren` / `.new` / recopy fixups, and the same `--prepare`
application. The one genuinely new piece of handling is **IMPORT** (§3.5),
which the redo parser never handled correctly because IMPORT bypasses the
redo-logged create path.

### 1.3 Why it matters beyond reduced-lock

Discovering DDLs from the server journal removes XtraBackup's dependence on the
redo log for tracking file operations. That is a strategic step, not just a
maintenance win: parsing the full-duration redo stream is today the main reason
XtraBackup must copy and decode redo for the entire backup. Once DDL discovery
no longer needs it, the door opens to a **clone-style backup** — file copy plus
a page-delta pass plus only a *short* redo tail, instead of a full-duration
redo copy/parse. This feature is the foundation that later work builds on; that
work is out of scope here and covered separately.

---

## 2. High-level design

```
XtraBackup (--lock-ddl=REDUCED --ddl-tracking=server)      Server (PS-11427)
────────────────────────────────────────────────────      ─────────────────
resolve DDL-tracking source (§3.1)
innodb_backup_ddl_journal_start() ───────────────────────► open session, capture
   ◄── backup_id                                             DDL events

[copy .ibd files, no lock]                                  (DDLs journaled)

LOCK INSTANCE FOR BACKUP
innodb_backup_ddl_journal_cut(backup_id) ────────────────► fsync slice
   ◄── slice path
consume_ddl_journal(slice)   (§3.2) ──► same tracker maps as the redo parser
handle_ddl_operations()      (§3.3) ──► .del / .ren / .new / recopy list
innodb_backup_ddl_journal_stop(backup_id) ───────────────► delete slice
UNLOCK INSTANCE

--prepare: apply .del/.ren/.new + recopies (unchanged), honor .reimport (§3.5)
```

The journal is consumed into the *same* structures the redo parser populates,
so from `handle_ddl_operations()` onward there is a single, shared code path.

---

## 3. Design

### 3.1 Selecting the DDL-tracking source

`--ddl-tracking` (enum, default `auto`) is only meaningful under
`--lock-ddl=REDUCED`:

- `auto` — probe `performance_schema.user_defined_functions` for
  `innodb_backup_ddl_journal_start`; use the journal if present, else the redo
  parser.
- `server` — require the journal; error out if the UDFs are absent.
- `redo` — force the legacy redo parser.

The choice is logged (`DDL tracking source: server DDL journal` /
`... redo log parsing`) and recorded in `xb_ddl_journal_mode`. Every
journal-specific path below is gated on that flag; when false, behavior is
exactly the pre-existing redo parser.

### 3.2 Consuming the journal

`consume_ddl_journal(slice_path)` reads the slice line by line (rapidjson),
skips the header, and pairs each event's BEGIN/END by `(type, space_id)`. The
BEGIN carries the "before" path (needed by DROP and RENAME-old); the END carries
the "after" path (CREATE, RENAME-new); processing happens on the END. Each type
is fed into the **same tracker call the redo parser used**:

| Journal event | Tracker call (shared with redo parser) |
|---------------|----------------------------------------|
| `SPACE_CREATE` | `journal_create()` — new table, or recreate/reimport (§3.5) |
| `SPACE_DROP` | `add_drop_table_from_redo()` (BEGIN path) |
| `SPACE_RENAME` | `add_rename_table_from_redo()` (BEGIN old → END new) |
| `SPACE_ALTER_ENCRYPT*` | `add_to_recopy_tables()` ("encryption") |
| `SPACE_ALTER_INPLACE_BULK` | `add_to_recopy_tables()` ("bulk index load") |
| `SPACE_UNDO_DDL` / `SPACE_IMPORT` | drop-if-before / create-if-after |
| `SPACE_ALTER_INPLACE` | ignored (metadata only) |
| `SYSTEM_REDO_DISABLE` | fatal — the backup is not consistent |

An unpaired BEGIN at end of file is a hard error (impossible once
`LOCK INSTANCE FOR BACKUP` has returned — all DDLs have finished by then).

### 3.3 Reusing the reduced-lock fixups

Nothing new here — this is the point. `handle_ddl_operations()` and `--prepare`
are the existing reduced-lock code, unchanged:

- **`schema/<space_id>.del`** — dropped during the backup.
- **`schema/<space_id>.ren`** — renamed; file content is the new path.
- **`file.ibd.new`** — created during the backup, copied under the lock.
- **recopy list** — spaces re-copied in full under the lock (encryption change,
  bulk index build, reimport) because their unlocked pages are unusable.

Because the journal feeds the same maps, we inherit the existing rename/delete
handling verbatim, including the streaming-friendly `.ren` scheme (a rename is a
`.ren` marker, never a local file move).

### 3.4 Ordering: resolved for free by sequential consumption

Only one reconciliation case is order-sensitive: the same `space_id` appearing
in both a DROP and a CREATE within one window. That pair is genuinely ambiguous
in the abstract — "dropped then re-created" (net exists, a reimport) versus
"created then dropped" (net gone, a transient table) — the same two events,
opposite outcomes.

No extra ordering machinery is needed to disambiguate: the journal is a JSONL
log consumed **in order**, so the order is already in hand. `journal_create()`,
applied to each create/import as it is read, simply asks "has this `space_id`
already been dropped earlier in this journal?":

- **yes** → the create supersedes the prior drop → recreate/reimport (recopied
  in full; the stale drop is erased);
- **no** → a plain new table (a later drop, if any, then applies normally, so a
  transient created-then-dropped table correctly nets to gone).

This is sound because **InnoDB does not recycle `space_id`s** for new user
tablespaces within a server run — a genuine `CREATE` always gets a fresh, higher
id, never a just-freed one. So a create that lands on a previously-dropped id
can only be IMPORT re-stamping that id; "drop then create, same id" uniquely
identifies a reimport.

**A cleaner alternative (not adopted).** The server could emit a distinct event
for import — a real `SPACE_IMPORT` carrying the true final id (or a reimport
flag) — instead of the synthetic `SPACE_CREATE`. The consumer would then tell
reimport from create by **type** rather than position, and the decision becomes
order-free set membership ("is there also a drop for this id?"). This removes
the implicit no-id-recycle assumption from the consumer, but needs a coordinated
change on both the server (PS-11427) and the consumer, so it is noted here as a
future option rather than done.

### 3.5 IMPORT — the only new handling, and the data-loss fix

IMPORT is invisible to the redo parser: it brings a tablespace in **physically**
with no `MLOG_FILE_CREATE`, and may assign a new `space_id`. The journal makes it
visible (PS-11427 §4.4): the DISCARD journals `SPACE_DROP` for the old id, and
after `row_import_for_mysql` succeeds the server journals a synthetic
`SPACE_CREATE` with the imported space's **final id, path, and authoritative
flags**. `journal_create()` (§3.4) sees the prior drop, treats it as a reimport,
recopies the file in full, and records
`reimported_tables[space_id] = {name, flags}`.

**The import data-loss problem.** Even with the file recopied, a naive prepare
loses data. The redo tail XtraBackup copied starts *before* the DISCARD, so it
still contains the DISCARD's `MLOG_FILE_DELETE`. A live server has checkpointed
past that record, but prepare-time recovery would replay it, mark the space
deleted, and drop the freshly imported file — discarding the imported rows **and
any post-import DML**.

**The fix.** For each net-still-existing reimported space,
`handle_ddl_operations()` writes a `schema/<space_id>.reimport` marker. At
prepare:

1. `prepare_handle_reimport_files()` loads the markers into a set
   (`xb_is_reimported_space()`).
2. In `fil0fil.cc`, when recovery encounters `MLOG_FILE_DELETE` for a reimported
   space, it **skips** `recv_sys->deleted.insert()` / `missing_ids.erase()`, so
   the space is not treated as deleted and the post-import redo is applied.
3. `xb_cleanup_reimport_markers()` removes the markers **only after** recovery
   has applied redo and checkpointed (`innodb_end`) — so a crash mid-prepare and
   a re-prepare stay idempotent (the markers are re-read and the logged delete is
   suppressed again).

Both same-id (plain DISCARD+IMPORT) and new-id (DROP+CREATE+IMPORT) cases are
handled.

---

## 4. User interface / deliverables

- **New client option:** `--ddl-tracking=auto|redo|server` (default `auto`;
  only meaningful with `--lock-ddl=REDUCED`).
- **No new server option** — relies on the PS-11427 UDFs, always registered when
  present.
- **Backup-directory artifact:** `schema/<space_id>.reimport` markers, produced
  during backup and consumed (and removed) during prepare. The `.del`/`.ren`/
  `.new` markers are unchanged.
- **Server requirement:** a Percona Server that provides the
  `innodb_backup_ddl_journal_*` UDFs. Without it, `auto` falls back to redo and
  `server` errors out.

---

## 5. Known limitations

- Requires `--lock-ddl=REDUCED`; `--ddl-tracking=server` with any other lock mode
  is rejected.
- The journal is authoritative only for the session's window; XtraBackup still
  scans `.ibd` files at start to establish the baseline space→path catalog the
  fixups build on.

---

## 6. Testing

- `deltabackup/component_ddl_redo_copy` — `auto` resolves to the server journal;
  journal-fed drop/create/rename/bulk fixups; `--ddl-tracking=redo` still uses
  the parser.
- `deltabackup/import_incremental` — DISCARD+IMPORT during backup, same-id and
  new-id, post-import DML, and an incremental follow-up.
- `deltabackup/import_ddl` — import combinations through backup + prepare +
  restore.
