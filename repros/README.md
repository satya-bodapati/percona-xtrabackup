# Import bug repros on Percona Server 8.4

All repros were run on 2026-10-16 against the **Percona Server 8.4.11-11 debug tarball**. 8.0 was not tried
(it is EOL).

How the tarball was fetched:
- with the xtrabackup `bootstrap.sh`, copied as `bootstrap-local.sh`;
- the only change in the copy maps glibc 2.43 (this container) to the glibc2.35 tarball, since the original
  script refuses 2.43;
- command: `./bootstrap-local.sh --type=xtradb80 --pxb-type=debug --version=8.4.11-11 --destdir=./server`.

Nothing was built.

Layout:
- `common.sh`: helpers. Each repro initialises its own datadir under `run/<name>/`, picks a free port, uses
  `--innodb-use-native-aio=0` and stops its own mysqld by PID.
- `logs/`: script output (`rN.out`), server error logs (`<name>.err`), xtrabackup logs.
- `server/bin/mysqld.my` (`{"read_local_manifest": true}`) was added for R5. Datadirs without their own
  manifest are not affected by it.

To run any repro: `cd bld/repros && ./rN_*.sh`. Each script prints EXPECTED and ACTUAL at the end.

| # | Bug | Script | Reproduced on 8.4.11 |
|---|-----|--------|----------------------|
| R1 | IMPORT doesn't persist AUTO_INCREMENT (PS-8687) | `r1_autoinc_restart.sh` | **yes**: duplicate key after restart (cases B, C) |
| R2 | V7 `.cfg` drops pre-8.0.29 INSTANT metadata (PS-9459, upstream #116677, likely PXB-3751) | `r2_v7_cfg_v1_instant.sh` | **yes**: debug assertion during IMPORT |
| R3 | DISCARD deletes the `.ibd` before the DD commit (not atomic) | `r3_discard_not_atomic.sh` | **yes**: table data lost after a failure in that window |
| R4 | ER_IMP_INCOMPATIBLE_CFG_VERSION says the current version is 5 | `r4_cfg_version_msg.sh` | **yes** |
| R5 | `xtrabackup --prepare --export` ignores `.cfp`/`.cfg` write errors | `r5_pxb_export_errors.sh` | **yes**: exit 0 with missing files |

---

## R1: AUTO_INCREMENT is lost when the server restarts after IMPORT (PS-8687)

- **Command:** `./r1_autoinc_restart.sh`, output in `logs/r1.out`.
- **Cases.** In every case: FLUSH TABLES ... FOR EXPORT, copy the `.ibd` and `.cfg`, DISCARD, put the files
  back, IMPORT TABLESPACE.
  - **A**, the reporter's MTR test: the same table, one INSERT after IMPORT, then a restart, then an INSERT.
  - **B**: export ids 1..3, DROP, CREATE a fresh table, IMPORT, restart, INSERT.
  - **C**: like B, but the fresh table took one row (counter 1) and lost it again before DISCARD.
  - **D**, the control: like B, but the INSERT runs before any restart.
- **EXPECTED:** every INSERT gets the next id (B: 4, C: 6), with no duplicate key.
- **ACTUAL:**
  ```
  B: AUTO_INCREMENT before restart: 4 / after restart: 4
     INSERT INTO t2 VALUES (NULL) -> ERROR 1062 (23000): Duplicate entry '1' for key 't2.i'
  C: AUTO_INCREMENT before restart: 6 / after restart: 6
     INSERT INTO t3 VALUES (NULL) -> ERROR 1062 (23000): Duplicate entry '2' for key 't3.i'
  D (no restart): INSERT gets id 4, OK
  A: INSERT after restart gets id 3, OK (I_S shows AUTO_INCREMENT=2 after restart, which is stale)
  ```
- **Reproduced:** yes, in B and C, the realistic restore flow: import into a new table, and the server restarts
  before the first insert.
- **Notes:**
  - The reporter's exact case (A) no longer fails on 8.4.11, because its INSERT before the restart persists the
    counter through redo.
  - After the restart the engine counter falls back to the target table's pre-IMPORT value: 0+1 in B, 1+1 in C.
    This matches the code: import sets the counter in memory only (`row0import.cc:4893-4906`) and never persists
    it.
  - `information_schema.tables.AUTO_INCREMENT` keeps showing 4 and 6, which disagrees with what InnoDB actually
    hands out.

## R2: a V7 `.cfg` loses pre-8.0.29 INSTANT ADD metadata (PS-9459 / upstream Bug #116677)

- **Command:** `./r2_v7_cfg_v1_instant.sh`, output in `logs/r2.out`, server log in `logs/r2_v1_instant.err`.
- **Data.** MySQL's MTR datadir `std_data/instant_ddl/data_instant_ddl_upgrade.zip`, read from the PS 8.4
  source worktree; the path can be overridden with `ZIP=`.
  - The table `test.t1_dynamic` got two INSTANT ADD COLUMNs on an 8.0 release before 8.0.29.
  - After upgrading to 8.4, `INSTANT_COLS=2` and `TOTAL_ROW_VERSIONS=0`, and SELECT and CHECK TABLE are fine.
- **Steps:**
  1. FLUSH TABLES ... FOR EXPORT, which writes a `.cfg` of version `00 00 00 07`.
  2. DROP, then CREATE the table with all 4 columns.
  3. DISCARD, put the files back, IMPORT TABLESPACE.
- **EXPECTED:** IMPORT succeeds and the 3 rows come back with their defaults: r1 gets c3_def and c4_def, r2 gets
  c4_def.
- **ACTUAL:**
  ```
  ALTER TABLE t1_dynamic IMPORT TABLESPACE -> ERROR 2013 Lost connection
  [ERROR] [MY-013183] [InnoDB] Assertion failure: rec.h:1041:!rec_get_instant_flag_new(rec)
   #5 rec_init_null_and_len_comp            rem/rec.h:1041
   #9 PageConverter::update_records         row/row0import.cc:2475
   #12 PageConverter::operator()            row/row0import.cc:2773
   #14 fil_tablespace_iterate               fil/fil0fil.cc:8775
   #15 row_import_for_mysql                 row/row0import.cc:4595
  ```
- **Reproduced:** yes. This is the same assertion as the PS-9459 report.
- **Notes:**
  - The crash is in phase I page conversion. The target index has no instant information while it converts
    records that carry the instant flag.
  - This fits the code reading: for V7, `set_instant_info` always takes the `_v2` path, which returns early
    when neither side has row versions.
  - The reporter says a release build imports "successfully" and CHECK TABLE then reports corruption. Only the
    debug tarball was run here.

## R3: DISCARD TABLESPACE is not atomic, so a failure before the DD commit loses the table's data

- **Command:** `./r3_discard_not_atomic.sh`, output in `logs/r3.out`, server log in `logs/r3_discard.err`.
- **Steps:**
  1. CREATE TABLE t1 and insert 2 rows.
  2. `SET SESSION debug='+d,fail_while_storing_dd_object'; ALTER TABLE t1 DISCARD TABLESPACE`.
  3. Restart, then `SELECT * FROM t1`.
- **EXPECTED:** a DISCARD that fails or crashes before its DD commit leaves t1 intact: the `.ibd` is still on
  disk and SELECT returns 2 rows.
- **ACTUAL:**
  ```
  ALTER TABLE t1 DISCARD TABLESPACE -> ERROR 2013 Lost connection
  [ERROR] [InnoDB] Assertion failure: dict0dd.cc:6873   (dd_tablespace_set_state <- dd_table_discard_tablespace)
  files after the failed DISCARD: (none, t1.ibd is gone)
  after restart: SELECT * FROM t1 -> ERROR 1812 (HY000): Tablespace is missing for table `test`.`t1`.
  error log: Tablespace 2, name 'test/t1', file './test/t1.ibd' is missing!
  ```
- **Reproduced:** yes. The DD still describes a normal, not discarded, table, but its data file is gone.
- **Notes:**
  - The injected DD store failure first hits InnoDB's own `dd_tablespace_set_state`, which asserts on any
    failure instead of returning an error. So the repro shows the crash window, not the pure statement-rollback
    path.
  - Any crash or kill between `fil_discard_tablespace` (the file deletion) and the DD commit has the same
    outcome.
  - There is no DDL log entry for the delete. With atomic DDL, the delete would be deferred to post-DDL.

## R4: ER_IMP_INCOMPATIBLE_CFG_VERSION names 5 as the current version

- **Command:** `./r4_cfg_version_msg.sh`, output in `logs/r4.out`.
- **Steps:** export t1, which writes a `.cfg` of version 7. Patch the version (the first 4 bytes, big-endian)
  to 8, then DISCARD and IMPORT.
- **EXPECTED:** "... the CFG file version (8) is not compatible with the current version (7)".
- **ACTUAL:**
  `ERROR 3949 (HY000): Failed to import test/t1 because the CFG file version (8) is not compatible with the current version (5)`
- **Reproduced:** yes. `row0import.cc` around 4204 passes `IB_EXPORT_CFG_VERSION_V5`.

## R5: `xtrabackup --prepare --export` exits 0 when it can't write `.cfp` or `.cfg`

- **Command:** `./r5_pxb_export_errors.sh`, output in `logs/r5.out`, xtrabackup logs in `logs/r5_backup.log`
  and `logs/r5_prepare.log`.
- **Binaries:** `~/WORK/pxb/pxb-8.4.11/bld_rel/runtime_output_directory/xtrabackup` (8.4.11-8), which has the
  same `if (!xb_export_cfp_write(table))` as origin/8.4. Override with `XB=`.
- **Keyring:** component_keyring_file. Its global config is written to `server/lib/plugin/` for the run and
  removed afterwards.
- **Steps:**
  1. Create three encrypted tables, t1 to t3, and take a backup.
  2. Before `--prepare --export`, create `test/t1.cfp` and `test/t2.cfg` as **directories** in the backup.
     t3 is the control.
- **EXPECTED:** `--prepare --export` fails with a non-zero exit.
- **ACTUAL:**
  ```
  [ERROR] [Xtrabackup] Can't create file './test/t1.cfp' (errno: 21 - Is a directory)
  [ERROR] [Xtrabackup] cannot open ./test/t2.ibd          <- the .cfg write failed; the message names the .ibd
  [Note] [Xtrabackup] completed OK!
  prepare --export exit code: 0
  files: t1.cfg file, t1.cfp missing, t2.cfg missing, t2.cfp file, t3.cfg+t3.cfp files
  importing t1 with the backup's t1.ibd + t1.cfg ->
    ERROR 1808 (HY000): Schema mismatch (Table is in an encrypted tablespace, but the encryption meta-data
    file cannot be found while importing.)
  ```
- **Reproduced:** yes, for both the `.cfp` and the `.cfg` failure.
- **Cause:**
  - `xb_export_cfp_write` returns a `dberr_t` (`DB_SUCCESS` = 10), and the caller tests it with `!`, so errors
    are never seen;
  - a `.cfg` failure jumps with `goto next_node` to the very next line;
  - neither failure sets a failure exit code.
- **Extra defect:** the `.cfg` error message prints the `.ibd` path (`node->name`) instead of the `.cfg` path.

## Not reproduced here

These came from code reading only; there is no user-visible repro for them.
- **Dead `"discarded"` key check in `sdi_tablespace.cc:229`:** the real key is `discard`.
- **`space` used after `fil_space_release`** in `row0import.cc`, around 4813/4858/4914. ASAN would be needed.
- **xtrabackup `--export` writes a short `.cfg` when `get_dd_Table` returns null, and tables sharing one IBD
  overwrite each other's `.cfg`:** the second claim was not confirmed. The `.cfg` path is taken from each
  table's own space, so a repro needs a case where one tablespace really holds several tables.
