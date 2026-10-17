# Decisions made without Satya (he asked me to keep working without stopping for questions)

Each entry gives the date, the decision, why, and how to undo it. Design decisions he made himself are in
DESIGN.md. This file holds only the calls I made on my own while he was away.

- **2026-10-16: worktree name.** A worktree containing submodules can't be moved or renamed, so I created a fresh
  worktree `~/WORK/ps/ps-8.4-import-refactor` on branch `ps-8.4-import-refactor` from origin/8.4 5db7de6b67b. The
  earlier worktree `~/WORK/ps/ps-8.4-import-repros` stays, because `repros/r2_*.sh` reads a test datadir zip
  from it.
- **2026-10-16: baseline test list.** These are the 69 MTR tests that run IMPORT TABLESPACE, IMPORT PARTITION or
  `import.inc` (`ps-8.4-import-refactor/bld/import_tests.txt`). I dropped `common/rpl/rpl_mta_rpco_partition_commands`,
  which is a shared file other tests include, not a test MTR can run on its own.
- **2026-10-16: MTR runs here use `--mysqld=--innodb-numa-interleave=0`.** The container doesn't allow `mbind`, so
  every server logs a NUMA warning and MTR fails the test on it. This is environmental, not a code issue.
- **2026-10-16: two baseline tests excluded or noted.**
  - `max_parts.innodb-import-partition-1` is excluded. Upstream disables it (BUG#27413612), and here it runs for
    more than 7 minutes.
  - `innodb.partition_upgrade_create` fails before and after the change because the container has no `zip` binary.
  - The baseline is 68 tests: all pass or skip apart from that one (`ps-8.4-import-refactor/bld/baseline_results.txt`).
- [PXB] **How `--restore` reads the backup's DD.** It runs the existing `--prepare` machinery on the full-prepared
  backup: prepare is a no-op there, and the collection hooks in where `--export` does. Like re-running `--prepare`,
  this touches the backup's redo, checkpoint and xtrabackup_tablespaces. A restore against a read-only backup
  therefore fails.
- [PXB] **AUTO_INCREMENT counter** = `dict_table_t::autoinc` after loading the table from the SDI, as `--export` uses
  for the `.cfg`. Test: 5 rows inserted, top 2 deleted; the backup shows 6. Partitioned tables take the maximum over
  partitions.
- [PXB] **Triggers.** The body is sent with the original character_set_client and collation_connection, which is
  exact for a utf8 client. For a non-utf8 client with non-ASCII names, the client switches to
  action_statement_utf8 under utf8mb4. sql_mode is set from the stored SET bitmap as a number.
- [PXB] **The client runs `USE db` before IMPORT TABLE.** The current server returns "No database selected" for a
  relative IMPORT path without a current database.
- [PXB] **xbcloud matches names ignoring case,** since it doesn't know lower_case_table_names; at worst it downloads
  an extra table. **Core set** = everything at the backup root except general tablespaces (mysql.ibd is kept), plus
  `.ren` markers in db dirs.
- [PXB] **Option names:** `--restore`, `--restore-include`, `--restore-exclude`, `--restore-include-file`,
  `--restore-exclude-file`, `--restore-report`, `--dry-run` on xtrabackup; `--restore-include`/`--restore-exclude`
  (+file variants) on xbcloud get only.
- [PXB] **Exit codes.** Failures inside the prepare machinery under `--restore` exit with 2; a few `exit(EXIT_FAILURE)`
  paths deep in prepare still exit with 1.
- [PXB] **Config files under `--restore`.** InnoDB options come from the backup's `backup-my.cnf`, as for `--prepare`,
  and client/connection options come from the user's normal config. Found by `xb_restore_data_dir.sh`: with the live
  `my.cnf`, `innodb_directories` made prepare scan the server's own DATA DIRECTORY file, giving a duplicate space id.
- [PXB] **Safety: a broken connection during IMPORT.** On error CR_SERVER_LOST or CR_SERVER_GONE_ERROR the copied files
  are left in place and the outcome is reported as unknown, because IMPORT may have committed. For any server error
  the files are removed. `*`, `?` and `\` in IMPORT paths are escaped, since IMPORT TABLE treats them as glob
  characters. Neither has a test that fails when reverted; both are folded into the --restore commit as safeguards.
- [PXB] **E2E tests skip** on a server without IMPORT TABLE FROM .ibd. The helper `require_import_table_ibd` in
  `inc/common.sh` places a dummy `.ibd` and looks for the old server's "Incorrect File Name". The parts that need no
  IMPORT (dry-run with encryption and DATA DIRECTORY, the known-directory refusal) run anyway.
- **2026-10-16 [PS] the server branch `ps-8.4-import-refactor` has four local commits:** 743d45b7e2a refactor,
  acf462f1221 SDI file reader, 9aa49a44b7c attach mechanism, e63cbe51348 IMPORT TABLE FROM .ibd with 5 MTR tests.
  Nothing is pushed.
- **2026-10-16 [PS] old INSTANT tables are refused.** Tables with INSTANT columns added before 8.0.29 (instant
  columns without row versions) get a clear error. The shared .cfg reader still has the PS-9459 bug, and you kept
  that fix separate. Once PS-9459 is fixed, drop the refusal in `Import_sdi_source::read()`.
- **2026-10-16 [PS] AUTO_INCREMENT is persisted at attach.** The attach path computes MAX(col) and persists
  max+1, instead of leaving 0 for the first open to compute. Leaving 0 logged "Reading max(auto_inc_col) ...
  because there was an IMPORT without cfg file" on every first open, which is misleading, and left nothing
  persisted. The client's ALTER TABLE AUTO_INCREMENT=N still raises it to the backup's counter.
- **2026-10-16 [PS] FULLTEXT.** The index is marked corrupted after import, and searches fail with
  ER_TABLE_HAS_NO_FT, so there are no silent wrong results. The rebuild needs two statements (DROP INDEX, then ADD
  FULLTEXT INDEX). One ALTER that does both leaves it unusable. INTERFACE.md and the PXB code are updated.
- **2026-10-16 [PS] tablespace references in the SDI are not resolved.** The SDI is deserialized with no THD, so
  references to tablespaces of the server that wrote it are skipped. The new table always gets a tablespace of its
  own.
- **2026-10-16 [PS] INSTANT-dropped columns stay in the new table's DD**, as hidden columns with their version
  keys. The .cfg reader matches them instead of adding them again.
- **2026-10-16 [PS] existing errors are reused rather than adding new ones:**
  - ER_TABLE_SCHEMA_MISMATCH for "file not in its schema directory", "not named after its table" and missing or
    extra partition files;
  - ER_NOT_SUPPORTED_YET for general tablespaces and compressed columns;
  - ER_IMP_NO_FILES_MATCHED, whose text says "No SDI files matched".

  Dedicated errors would be nicer, but they need new error numbers, which is a review question.
- **2026-10-16 [PS] the table-exists check needs a readable SDI.** IMPORT reads the SDI before checking whether the
  table exists, so a live table whose page 0 is not flushed yet can give "no SDI" instead of "already exists".
  `--restore` checks for the table before copying, and the MTR test flushes first.
- **2026-10-16 [PXB] FULLTEXT rebuild.** `restore.cc` now rebuilds FULLTEXT with two statements (see above). Not
  committed yet: it waits for the end-to-end run.
- **2026-10-16 [PXB] FULLTEXT rebuild, corrected (commit 2a3a0543a61c).** Satya asked for an ALTER that rebuilds
  only the FTS indexes. Probing that found a bug: with two FULLTEXT indexes, ADD of the first fails while the second
  is still corrupted, so the one-by-one DROP+ADD left the first index dropped. Now one ALTER drops them all, then
  one ALTER per index adds it back (InnoDB adds one FULLTEXT index per ALTER). A single statement is not possible
  on 8.4 (the same-definition DROP+ADD is a no-op). The table is not rebuilt (table_id and inode unchanged).
