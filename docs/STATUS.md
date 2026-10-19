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
| `782d48b3429` .. `b2edad29d1b` | **Corner-case fixes (2026-10-17):** row-version check in the page converter (the crash), exact partition files checked before any change (`handlerton::sdi_check_import_files`), all tables checked before any is created, SRID, index-mismatch message. | `innodb.import_table_ibd_mismatch`. Details in `IMPORT_CORNER_CASES.md`. |
| `f6656e96ab5` .. `c98c8bd46f0` | **CREATE TABLE's checks (2026-10-18):** FK parents created first, REFERENCES on the parents, `table_encryption_privilege_check`, `disabled_storage_engines`. | `innodb.import_table_ibd_checks`. Details in `IMPORT_CORNER_CASES.md`. |

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

## Full suites (2026-10-18)

**PS `innodb`, `main`, `rpl` (debug build at `c98c8bd46f0`):** 3063 tests, 6 failed:
`innodb.tablespace_portability`, `rpl.rpl_deprecations`, `rpl.rpl_semi_sync_alias`, `rpl.rpl_show_replica_auth_info`,
`rpl.rpl_shutdown_with_optimize_for_static_plugin_config` and MTR's `shutdown_report`. All five tests fail in
`assert_error_log.inc` on the same three warnings the container writes at every server start, "Failed to set NUMA
memory policy ... Operation not permitted" (MY-011873/11879/11875). Rerun with `--mysqld=--innodb-numa-interleave=0`,
all six pass. Not ours. Logs: `ps-8.4-import-refactor/bld/suites_ps*.log`.

**PXB default suites (`run.sh -f -j 16`, xtrabackup at `e033612bcedc`, against the new PS server):** 443 run, 348 passed,
91 skipped (no Galera, KMIP, KMS, Vault), 4 failed. All `xb_restore*` tests passed. The four fail the same way against
the old `server/` copy and against the untouched upstream base `5db7de6b67b` (built in `ps-8.4-import-repros/bld`,
installed to its `bld/install`), so none is caused by the PS changes; the PXB changes are in `restore*.cc`, `xb_dict.cc`
and the options in `xtrabackup.cc`, none of which these tests run.
- `main.bug1002688`, `main.bug759701` (flaky, passed alone): after copy-back of a full + incremental backup, the debug
  server asserts at startup: `Duplicate entry '402' for key 'tables.PRIMARY'` while `innobase_post_recover` creates the
  P_S native table (`log_pfs_create_tables`), then `THD::send_statement_status(): Assertion '0'`, a debug-only assert.
  A release server logs the error and starts. The `mysql.tables` AUTO_INCREMENT is stale after the restore.
- `main.estimate_memory`: `host_free_memory()` (procps `kb_main_available`) returns 0 in this container, so
  `--use-free-memory-pct=99` sets the buffer pool to -134217728 and xtrabackup segfaults in
  `buf_pool_t::allocate_chunk`. A PXB robustness bug on top of an environment quirk; the test's own gate read
  MemAvailable 445 GB from /proc/meminfo. Already fixed as PXB-3770 on `pxb-8.4-fix-flaky` (`80fe8061e56f`) and
  `pxb-9.7-fix-flaky` (`8336993ac65e`), 2026-10-10; not on this branch yet.
- `reducedlock.basic_operation`: "xtrabackup did not handle delete table DDL", the DDL tracker printed no line for the
  table dropped while the backup was paused. Deterministic here, same on both servers. Not root-caused.
Logs: `pxb_full.log`, `pxb_rerun4.log`, `pxb_rerun3_oldserver.log`, `pxb_rerun3_baseserver.log`, results in
`pxb_*_results/`.

**Resolution (2026-10-19), from the flaky-tests session, which owns both `pxb-8.4-fix-flaky` and `pxb-9.7-fix-flaky`:**
all four are fixed on both branches; none of the fixes is on this branch yet, they come with the next 8.4 merge.
- `bug1002688`, `bug759701` (and `bug1028949`, `bug932623`): "[9] apply the last incremental without --apply-log-only",
  8.4 `2c0ace864649`, 9.7 `bc6b533cdf14`. Applying the last incremental with `--apply-log-only` loses the
  AUTO_INCREMENT of `mysql.tables` (PXB-2865), hence the duplicate key in `log_pfs_create_tables`.
- `estimate_memory`: PXB-3770, 8.4 `80fe8061e56f`, 9.7 `8336993ac65e`.
- `reducedlock.basic_operation`: "fix reducedlock/basic_operation.sh without a UTF-8 locale", 8.4 `81a9c60397ef` ([19]),
  9.7 `d9ef682d0b92` ([20]), both 2026-10-19. The container has no locale (POSIX), so in the test's special_char pass
  every mysql client statement fails with ERROR 1064 near '?table'; no DDL runs while the backup is paused, so the
  tracker has nothing to print. My log shows the same 7 ERROR 1064 lines right after "Running test with special_char".
  The fix passes `--default-character-set=utf8mb4`.

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
- **Full suites:** run 2026-10-18, see "Full suites" above. Nothing fails because of these branches.
- **Option name (parked, 2026-10-16):** `--backup-dir` as an alias of `--target-dir`, accepted by every mode that
  reads a backup. Satya: OK in principle, but first check how the name fits `--copy-back`, `--move-back`,
  `--prepare` and `--decompress`. Then: one generic alias for all of them. Not started.
