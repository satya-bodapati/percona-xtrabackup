# Single-table restore v1: design decisions (PXB-2745)

Status: decisions from the design discussion with Satya on 2026-10-15/16. Not reviewed by anyone else. All v1
questions are answered.

Background:
- code internals: `IMPORT_INTERNALS.md`
- ticket survey: `TICKETS.md`
- v2 replication ideas: `REPLICATION_IDEAS.md`

## Scope

- **Product and version:** Percona Server only, 8.4 only, same series only (no 8.0 to 8.4).
  `DD_VERSION` 80300 and `SDI_VERSION` 80019 are constant across PS 8.4.
- **Same host:** xtrabackup places the files in the server's datadir.
- **Not atomic:** whatever can be restored is restored, and the failed tables are listed. Atomic restore is a later
  goal.
- **In scope:**
  - InnoDB file-per-table tables;
  - partitioned tables, as whole tables;
  - FULLTEXT: v1 rebuilds the index after import. Importing the aux tablespaces is **v2** (Satya, 2026-10-16);
  - DATA DIRECTORY tables;
  - triggers;
  - encrypted tables.
- **Out of scope:**
  - general and system tablespaces;
  - non-InnoDB tables;
  - views, routines and events;
  - **v2:** Percona compressed columns (Satya, 2026-10-16). The SDI holds only `zip_dict_id`, and the
    compressed data depends on the exact dictionary bytes. In v1 such a table gets a per-table
    'not supported until v2' error.
  - importing a single partition into an existing table, which stays `ALTER TABLE ... IMPORT PARTITION`.

## Server side (PS): `IMPORT TABLE FROM '<db>/<table>.ibd'`

This extends the existing IMPORT TABLE, which today takes `.sdi` files and supports MyISAM only.

- **Syntax (decided 2026-10-16): the path form**, as in the MyISAM-era IMPORT TABLE:
  `IMPORT TABLE FROM 'db1/t1.ibd' [, ...]`.
  - For a partitioned table, **all** partition files must be named, explicitly or by a pattern.
  - The server checks that the named set equals the partition set in the SDI. A missing or extra partition is an
    error.
  - **Patterns:** the existing IMPORT TABLE patterns are globs, not regexes: `*` and `?`, with `\` as escape
    (`my_wildcmp`, `sql/dd/impl/sdi_file.cc:214-225`). So `'db1/t1#p#*.ibd'` names every partition.
  - **Several tables in one statement: all-or-nothing** (decided 2026-10-16), the same as today's IMPORT TABLE.
    It is one transaction. On any failure the DD rolls back and log_ddl deletes every converted file. `--restore`
    sends one table per statement.
  - **FTS aux files (v2) are not named.** The server derives `fts_<table_id>_<index_id>_*.ibd` from the source SDI's ids,
    the way `.MYD`/`.MYI` are found today. A missing aux file is an error. (Decided 2026-10-16.)

- **SDI.** The server reads the SDI embedded in the `.ibd` and builds the `dd::Table` from it.
  - The existing version checks apply: `mysqld_version_id` ≤ the server's, and `dd_version` and `sdi_version`
    equal.
  - For a partitioned table, the table SDI is stored only in the **first** partition's `.ibd`.
- **File location.** The file must already be at the table's own path: `<datadir>/<db>/<table>.ibd`, the
  partition names, or the DATA DIRECTORY path from the SDI. Otherwise IMPORT fails. The server never moves files.
  MyISAM's `.MYD`/`.MYI` work the same way today.
- **Permissions.** Same as `ALTER TABLE ... IMPORT TABLESPACE`, because both rely on files someone placed in the
  datadir:
  - `secure_file_priv` is not consulted;
  - the privileges are CREATE plus ALTER on the table; no FILE.
- **Encryption.** The server reads the `.cfp` next to the `.ibd` **before** reading the SDI, because the SDI pages
  are encrypted.
- **Existing table.** If the table already exists: error, with no replace and no rename. The check is at run time,
  because nothing can lock the name in advance.
- **Missing database:** error. `--restore` creates the database first.
- **Foreign keys.** The DD is linked both ways, the same way CREATE TABLE does:
  - to parents, via `adjust_fk_parents`;
  - to existing children, via `adjust_fk_children_after_parent_def_change`.

  Neither exists in IMPORT TABLE today. A missing parent is allowed under `foreign_key_checks=0`.
- **FULLTEXT.** Importing the backup's `fts_*.ibd` aux tablespaces (renamed to the new ids, with hidden DD entries)
  is **v2**. In v1 the attach path leaves the FULLTEXT index marked corrupted, as import does today, so searches
  fail rather than return wrong results. `--restore` then rebuilds only the FULLTEXT indexes, with existing server
  code: one ALTER drops them all, then one ALTER per index adds it back (see INTERFACE.md for why it can't be one
  statement). The table is not rebuilt. A failed rebuild is a warning.
- **Compressed columns: v2.** The attach path refuses them, like PS-4805 does for IMPORT TABLESPACE. v2 needs
  a dictionary id remap (the SDI holds only `zip_dict_id`, `dd_table_share.cc:1102`) and inserts into
  `mysql.compression_dictionary_cols`.
- **AUTO_INCREMENT.** The import **persists** `MAX(col)+1`. Today import sets the counter in memory only
  (`row0import.cc:4893-4906`), which is the root cause of PS-8687.
- **Binlog.** Not binlogged. IMPORT TABLE never is, today. Confirmed for v1 (Satya, 2026-10-18); replication is v2.
- **InnoDB core (decided 2026-10-16): a new attach path built on a refactored import.**
  - **No fake states.** The attach path never puts the table through "create, then discard".
  - **The existing import code splits into three reusable layers:**
    1. **Metadata source:** produces the in-memory `row_import`, meaning the indexes with ids, roots and fields,
       the columns with instant metadata, row versions, dropped columns, page size and encryption.
       - One source reads the `.cfg` (V1-V7), for ALTER TABLE ... IMPORT TABLESPACE.
       - The new source takes a source `dd::Table` deserialized from the SDI, turns it into a `dict_table_t`,
         and fills `row_import` the way the `.cfg` writer serializes it, but in memory with no file.
       - PXB `--export` already does SDI → `dd::Table` → `dict_table_t` → `.cfg` (`xb_dict.cc:325-440`), so
         that path is proven.
    2. **Conversion engine:** `fil_tablespace_iterate` + `PageConverter`, unchanged.
    3. **Phase II:** instant info, open the space, encryption, clustered-index fixes, purge, max row id, SDI
       root, flush, autoinc, validation. It becomes a function over `(dict_table_t, dd::Table, row_import)`.
  - **The attach entry point** creates the `dict_table_t` and DD entry for the target, assigns a space id, runs
    layers 2 and 3, and registers the tablespace inside one atomic DDL. Being new code, it can write log_ddl
    records for crash safety; today's import has none.
  - **ALTER TABLE ... IMPORT TABLESPACE moves onto the shared core in the same refactor** (decided 2026-10-16), with
    its `.cfg` source. Its existing MTR tests guard the refactor.
  - **SDI reader (decided 2026-10-16):** a new handlerton call, for example `sdi_get_from_file(path, cfp_key)`.
    It walks the SDI B-tree from page 0 with direct file reads, like `fil_tablespace_iterate`, and handles page
    size, compressed pages and decryption with the `.cfp` key. It uses no buffer pool and registers no space.
  - **Failure (decided 2026-10-16):** IMPORT consumes the file and converts it in place. On failure, or during
    crash recovery, a log_ddl entry deletes the half-converted file, as a rolled-back CREATE TABLE does. The docs
    say: IMPORT consumes the file, keep a copy. `--restore` always copies, so the backup is unaffected.
  - **The bug fixes** in the shared layers (V1 instant metadata, persisting AUTO_INCREMENT) then benefit both
    paths.
- **Concurrent xtrabackup backups (confirmed 2026-10-16).**
  - With `--lock-ddl=ON`, the BACKUP_LOCK IX that IMPORT already takes blocks it for the whole backup.
  - With `--lock-ddl=REDUCED`, the DDL tracker only sees MLOG_FILE_CREATE, RENAME, DELETE, INDEX_LOAD and
    WRITE_STRING redo records (`ddl_tracker.cc`). Page conversion writes no redo. So the attach path must keep
    today's on-disk behaviour: flush and checkpoint before commit, and a new space id that the final ibd scan picks
    up as a new table and copies under the lock.
  - Add a reduced-lock test that runs the new IMPORT during a backup, next to `suites/reducedlock/partial_backup.sh`.
- **Locking (confirmed 2026-10-16).** The same locks as IMPORT TABLESPACE, including Percona's LOCK TABLES FOR BACKUP
  protection. The existing IMPORT TABLE does not take that one.
- **Failure.** The DD transaction rolls back and no table exists afterwards. The file is deleted (see InnoDB core).

## PXB side: `xtrabackup --restore`

### Inputs

- A prepared backup directory and a server connection, using the usual connection options.
- **Selection.**
  - `--restore-include` and `--restore-exclude`, plus file variants. Each takes `db.table` glob patterns (`*`, `?`)
    on real names, compared using the server's `lower_case_table_names`.
  - A bare `db` means `db.*`. There is deliberately **no** separate database option: the old
    `--tables`/`--databases` pair is confusing because a database listed without tables takes all of its tables
    and the regex doesn't narrow it (`xtrabackup.cc:3239`).
  - Selected = matches any include AND matches no exclude.
  - Patterns pick tables only; partitions and FTS aux files always follow their table.
  - A pattern that matches nothing is an error.
- **`--dry-run`** prints the resolved list and stops.
- **`--parallel=N`**, default 1.
- **`--restore-report=FILE`** writes a JSON report.

### Preconditions (confirmed 2026-10-16, checked up front)

- The backup is prepared.
- The server is Percona Server 8.4.
- The server's datadir is local.
- The connection user has the privileges listed below.

### Session settings

- `sql_log_bin=0`: nothing is replicated in v1.
- `foreign_key_checks=0`: the mysqldump precedent; foreign keys are kept as defined.

### Steps

1. Resolve the table list from the backup's DD (`mysql.tables`).
2. For each missing database, run `CREATE DATABASE` with the original charset, collation and default encryption
   from the backup's `mysql.schemata`. `xb_dict.cc` already scans that table.
3. Each worker then handles one table at a time:
   1. **Early checks**, each of which is a per-table error:
      - the table already exists;
      - a DATA DIRECTORY path is not the datadir, not the dedicated undo directory, and not under the datadir or a
        known InnoDB directory (the CREATE TABLE rule, `ha_innodb.cc:13101-13146`; the known directories are
        `datadir`, `innodb_data_home_dir`, `innodb_undo_directory` and `innodb_directories`).
   2. **Copy** (never move) the `.ibd` and the partition files. The FTS aux files are v2.
   3. **Ownership:** set owner and group to those of `<datadir>/<db>/`, and mode 0640. A chown failure is a
      per-table error.
   4. **Encrypted table:** load the keyring as `--prepare` does and write the `.cfp`, reusing
      `xb_export_cfp_write`.
   5. Run `IMPORT TABLE FROM '<db>/<table>.ibd'`.
   6. Delete the `.cfp`: it holds the transfer key in plain text.
   7. Run `ALTER TABLE ... AUTO_INCREMENT=N`, where N is the backup's persisted counter (from
      `mysql.innodb_dynamic_metadata`). InnoDB raises it to at least `MAX+1`.
   8. **FULLTEXT indexes:** one `ALTER TABLE ... DROP INDEX a, DROP INDEX b`, then one `ADD FULLTEXT INDEX` per
      index. A failure is reported as a warning that gives the statement to add the index back.
   9. Run `CREATE TRIGGER` for each trigger as-is, the way mysqldump does: original definer, `sql_mode`, charsets
      and FOLLOWS/PRECEDES order. A failure reports "table restored, trigger X not created".
   10. **If IMPORT fails:** delete the copied, partly converted files; the backup still has the originals. Record
      the table as failed.

### Report

- A printed table of restored, failed and warned tables, with the reasons.
- **Exit codes:**
  - 0: everything was restored;
  - 1: some tables failed, or there were warnings (a trigger not created, a missing FK parent);
  - 2: a fatal error before any table was restored.

### Privileges the connection user needs (confirmed 2026-10-16)

- CREATE and ALTER on the tables;
- CREATE on new databases;
- TRIGGER;
- SESSION_VARIABLES_ADMIN, for `sql_log_bin`;
- for triggers whose definer is someone else, SET_ANY_DEFINER, plus ALLOW_NONEXISTENT_DEFINER if that definer
  account no longer exists.

## Backups in the cloud, and what a restore needs (decided 2026-10-16)

- **The whole datadir is not needed.** A restore needs:
  - the selected tables' files;
  - for an **unprepared** backup, also the core set: `ibdata1`, the undo tablespaces, `mysql.ibd`,
    `xtrabackup_logfile`, the metadata files and the keyring files.
  - Prepare skips redo for absent spaces, the same way partial `--tables` backups work. It runs on that subset in a
    scratch directory, never inside a full backup.
- **A prepared backup** (`backup_type = full-prepared`) needs only `mysql.ibd`, the metadata, the keyring and the
  table files.
- **Unprepared files are refused.** `--restore` refuses an unprepared `.ibd`: it is a fuzzy copy, with uncommitted
  rows and missing redo changes. The server can't tell, so xtrabackup enforces it through `xtrabackup_checkpoints`.
- **`mysql.ibd` is needed anyway**, for triggers, schema defaults and prepare itself. So the backup's AUTO_INCREMENT
  counter costs nothing extra.
- **8.4 legacy pipeline (xbcloud):**
  - `xbcloud get` gains `--restore-include` / `--restore-exclude`. It fetches the core set plus the matching table
    files, as a pattern filter in `skip_file()` (`xbcloud.cc:798`). Today's `[FILES...]` takes exact names only.
  - The user passes the same selection to `xbcloud get` and to `xtrabackup --restore`.
  - Both tools link **one shared matcher** (globs on decoded names, partitions follow their table), so they select
    identically.
- **9.7 native cloud (PXB-3671 / PXB-3787 epic), only a proposal, deferred:** `xtrabackup --download` could gain the same flags and selects from
  `backup_files.jsonl`, with no LIST. One object per file means one GET (or range GETs) per table. The prototype is on
  fork branches `97_redesign_proto` and `pxb-9.7.1-rc1-manifest-framework`, with no PRs.

## Replication

- **v1 is local only.** Run `--restore` on every server.
- **Docs must say:**
  - every server must restore the **same** content before **any** write reaches the table;
  - in the usual case the DROP already replicated, so a write to the restored table stops the replicas with "table
    doesn't exist".
- **v2 ideas** are in `REPLICATION_IDEAS.md`. The current pick is A2: stream the file as many small GTID
  transactions into a stage file, then a final IMPORT. That works for async replication and for single-primary
  Group Replication.

## Future

- **The `.ibd` should carry everything import needs** (Satya). FLUSH TABLES ... FOR EXPORT, and xtrabackup's
  `--prepare` on the backup copy, would refresh the SDI:
  - AUTO_INCREMENT goes into the table SDI's `se_private_data`;
  - triggers and the schema defaults go into a new SDI record type, 3, so the table SDI stays
    upstream-compatible;
  - the key stays in the `.cfp`.

  Then `--restore` no longer needs the ALTER and CREATE TRIGGER steps.
- **Restoring to a group of servers** (credentials are the problem), and **S3 bundles** (idea E).
- **Atomic restore.**

## Implementation phasing (decided 2026-10-16)

Each step lands with tests that fail when its change is reverted.

1. **PS 8.4:** refactor `row_import` into three layers (metadata source, conversion engine, phase II), with ALTER
   TABLE ... IMPORT TABLESPACE on top. No behaviour change.
2. **PS 8.4:** the SDI file reader, a new handlerton call.
3. **PS 8.4:** the `IMPORT TABLE FROM '.ibd'` attach path, with FK linking and log_ddl.
4. **PXB 8.4:** `xtrabackup --restore` with the shared matcher.
5. **PXB 8.4:** `xbcloud get --restore-include` / `--restore-exclude`.
6. ~~PXB 9.7: `--download` filter~~ **Deferred** (Satya, 2026-10-16). The 9.7 redesign (PXB-3671) is only a
   proposal. The work stays on 8.4, and the merge to 9.7 is handled later.

## Discarded: FLUSH TABLES ... FOR EXPORT backups (TTS mode), 2026-10-16

The FOR EXPORT lock is `MDL_SHARED_NO_WRITE` (`sql_yacc.yy:14339`), so writes to the exported tables block from
FLUSH, including its dirty-page flush, until UNLOCK TABLES. This was verified on 8.4.11: INSERT and UPDATE hit
ER_LOCK_WAIT_TIMEOUT (`repros/t1_flush_for_export_writes.sh`).

Satya: blocking writes is a no-go. Not decided: a non-blocking alternative that reaches the same fast restore by
running a partial hot backup with the prepare done at backup time, keeping only the prepared table files plus a
metadata sidecar.

## Separate from the feature (decided 2026-10-16)

The reproduced bugs (`repros/README.md`) are handled separately from the feature, not in its phasing:
- PS-9459 (V7 `.cfg` drops V1 instant metadata);
- PS-8687 (AUTO_INCREMENT not persisted);
- the PXB `--export` exit code;
- DISCARD deleting the file before the DD commit.

The attach path still has to get both behaviours right on its own: it must persist the counter, and its SDI
source must carry the instant metadata.

## Still open

- Whether to file tickets for the bugs found (`IMPORT_INTERNALS.md` section E). Any Jira text needs Satya's
  approval first.
- Multi-primary Group Replication (v2).
