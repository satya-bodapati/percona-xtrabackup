# Single-table restore: implementation status (2026-10-16)

Both branches are pushed to `fork` (2026-10-16). No PRs, no Jira. Decisions I made on my own are in
`AUTONOMOUS_DECISIONS.md`; the design is in `DESIGN.md`; the server/client contract is in `INTERFACE.md`.

## Percona Server 8.4: `~/WORK/ps/ps-8.4-import-refactor`, branch `ps-8.4-import-refactor`

Built on origin/8.4 5db7de6b67b.

| Commit | What | Tests |
|---|---|---|
| `743d45b7e2a` | **Split InnoDB tablespace import into reusable steps** (Import_ctx, Import_metadata_source, Phase I, Phases II–IV). ALTER TABLE ... IMPORT TABLESPACE runs on top of them. | No behaviour change: the 68 existing import tests give the same results as the baseline. |
| `acf462f1221` | **Read the SDI of an unattached tablespace file** (`handlerton::sdi_get_from_file`). Pages are read directly; encrypted (.cfp), ROW_FORMAT=COMPRESSED and SDI BLOBs are handled; corrupt files give errors, not asserts. | Mechanism only; exercised by the tests of 3debe50b2f9. |
| `a188f7737df` | **InnoDB creates a table on top of an existing file and attaches it** (`HA_CREATE_INFO::m_import_source`, `row_import_attach()`, `Import_sdi_source`). | Mechanism only. |
| `3debe50b2f9` | **`IMPORT TABLE FROM '<file>.ibd'`** (SQL layer) | 5 new MTR tests, all pass (see below). |

New MTR tests, all in `innodb.import_table_ibd*`:
- **basic:** BLOBs, a secondary index, AUTO_INCREMENT across a restart, an existing table, wrong location, a missing
  schema, mixing .sdi and .ibd, a general tablespace, privileges (CREATE+ALTER, no FILE);
- **features:** partitions, subpartitions, ROW_FORMAT=COMPRESSED, INSTANT ADD/DROP (row versions), the FULLTEXT
  rebuild, foreign keys (both directions, foreign_key_checks off with a missing parent);
- **debug:** a failure after conversion rolls back and deletes the file; a crash mid-import is recovered and the
  file deleted; a re-import works;
- **encrypt:** keyring component, .cfp required, restart, master key rotation;
- **datadir:** DATA DIRECTORY via innodb_directories; an unknown directory is refused.

**Regression:**
- The 73 tests that run IMPORT TABLESPACE, IMPORT PARTITION or IMPORT TABLE (.sdi) pass, except
  `innodb.partition_upgrade_create`, which fails before and after the change because the container has no `zip`
  binary.
- Full `innodb`, `innodb_zip` and `parts` suites (787 passed). The only failures were my own new tests, run while
  their files were being edited (they pass now), and `innodb.trigger_function_lock_compare`, which timed out
  under load and passes when run alone.
- Each intermediate commit builds on its own.

## PXB 8.4: `~/WORK/pxb/pxb-single-table-restore`, branch `pxb-single-table-restore`

| Commit | What |
|---|---|
| `e43452816c63` | **`xtrabackup --restore`** with `--restore-include/--restore-exclude` (globs on real names), `--dry-run`, `--parallel`, `--restore-report` (JSON) and exit codes 0/1/2. It creates missing databases with their original charset, copies, chowns, writes the .cfp, runs IMPORT, deletes the .cfp, then sets AUTO_INCREMENT, rebuilds FULLTEXT and creates triggers. |
| `e033612bcedc` | **Rebuild every FULLTEXT index of a table.** With two FULLTEXT indexes, the one-by-one DROP+ADD failed ("index ftt is corrupt, drop it first") and left the first index dropped. Now one ALTER drops them all, then one ALTER per index adds it back; the table is not rebuilt. `xb_restore.sh` fails without it. |

**End-to-end against the new server** (installed at `ps-8.4-import-refactor/bld/install`), all pass:
- `t/xb_restore.sh`: a whole database restored, with charset, triggers, FULLTEXT and foreign keys;
- `t/xb_restore_selection.sh`;
- `t/xb_restore_data_dir.sh`;
- `suites/keyring/xb_restore_encrypted.sh`;

**Related existing PXB tests** pass on the stock 8.4.11 server (23 of 23): dictionary, partial backup, export,
encryption and DATA DIRECTORY tests.

## Not done / open

- **xbcloud selective download dropped (2026-10-16).** The commit `d1ae120578f8` (`xbcloud get --restore-include`)
  was removed from the branch and the branch force-pushed: filtering by name is wrong for `--lock-ddl=REDUCED` backups
  with DDL during the backup. It comes back with the manifest (`backup_metadata.json` + `backup_files.jsonl` with DDL
  lines). Draft Jira comment for PXB-3754: `PXB-3754_comment.md`, not posted yet.
  Decided 2026-10-17: no interim gate for `--lock-ddl=ON` backups (no `lock_ddl_type` line in `xtrabackup_checkpoints`,
  no reading `xtrabackup_info` from xbcloud). Selective download waits for the unified `backup_metadata.json`.
  Stages: `backup_metadata.json` unlocks `--lock-ddl=ON` backups; `backup_files.jsonl` with DDL lines unlocks REDUCED.

- **Step 6** (9.7 `--download` filter) is deferred, as you said.
- **PS-9459, PS-8687, the PXB `--export` exit code and the DISCARD ordering** are kept separate, as you said. The
  repros are in `repros/`. Because PS-9459 is not fixed, the attach path refuses tables with pre-8.0.29 INSTANT
  columns.
- **Tickets:** none created; branches and commits have no ticket ids yet.
- **Error codes:** existing ones are reused. Dedicated errors would need new numbers.
- **Not run:** the full innodb/main MTR suites, and the full PXB suite. Only the import-related tests and a PXB
  subset were run.
- **Option name (parked, 2026-10-16):** `--backup-dir` as an alias of `--target-dir`, accepted by every mode that
  reads a backup. Satya: OK in principle, but first check how the name fits `--copy-back`, `--move-back`,
  `--prepare` and `--decompress`. Then: one generic alias for all of them. Not started.
