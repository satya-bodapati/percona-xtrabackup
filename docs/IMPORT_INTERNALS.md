# DISCARD / IMPORT TABLESPACE and IMPORT TABLE in PS 8.4: how they work today

Source: percona-server origin/8.4 at ab8db2d1dbe, read with `git show`. Two read-only agents wrote the
analysis. Claims marked **[verified]** I checked myself against the source; the rest are the agents'
readings with file:line references and have not been re-checked one by one.

Decisions so far (2026-10-15):
- Server side: `IMPORT TABLE FROM` takes an .ibd and builds the table from the SDI inside it.
- PXB side: `xtrabackup --restore` on a prepared backup; the input is a table or a whole database.
- Same host only. xtrabackup places the files itself.
- Not atomic for now: each failed table is listed and the restore continues.

---

## A. SQL layer: ALTER TABLE ... DISCARD/IMPORT TABLESPACE

- **Execution path.**
  - Grammar is at `sql/sql_yacc.yy:8763` and `:8843` (the PARTITION form).
  - `Sql_cmd_discard_import_tablespace::execute` (`sql/sql_alter.cc:391`) checks ALTER_ACL, refuses log tables, and records the database for multi-threaded replicas.
  - `mysql_discard_or_import_tablespace` (`sql/sql_table.cc:11634-11869`) does the work. Only LOCK=DEFAULT and ALGORITHM=DEFAULT are accepted.
- **Locking.**
  - MDL_EXCLUSIVE on the table is taken directly through `open_and_lock_tables`; there is no upgrade.
  - `lock_table_names` also takes schema IX, GRL protection, Percona's LOCK TABLES FOR BACKUP protection, and BACKUP_LOCK IX. It then re-checks read_only and schema read-only.
  - Under LOCK TABLES the lock is upgraded from SNRW to X and downgraded again afterwards.
- **DD flow.**
  - `acquire_for_modification` → `handler::ha_discard_or_import_tablespace(discard, table_def)` → on IMPORT, **all SDIs in the tablespace are dropped** (`sql_table.cc:11765-11812`) → `dd_client()->update(table_def)` writes the target's SDI → `write_bin_log` → commit → `post_ddl`.
  - On failure everything is rolled back. After a failed IMPORT the table stays discarded and the command can be retried.
- **Binlog.** The statement is logged as text into the transactional cache, together with the DD change.
- **Table cache not flushed.** The `close_all_tables_for_name` call is compiled out (`#ifdef NEEDS_SUPPORT_FROM_INNODB`, around `sql_table.cc:11833`).
- **Percona-specific.** Tables with compressed columns are refused (`sql_table.cc:11747`, PS-4805), plus the slow-log and backup-lock hooks.
- **Partitions.** The partition list or ALL is turned into a bitmap of partitions to act on. `ha_innopart` loops over them and stops at the first error.

## B. SQL layer: IMPORT TABLE FROM 'x.sdi' (the base for the new feature)

- **Privileges.** FILE_ACL, plus CREATE_ACL on each target table.
- **File rules.** Patterns must end in `*.sdi` and resolve inside `secure_file_priv`. An .sdi inside the datadir is renamed to `*_import.sdi` while loading, deleted on commit and renamed back on rollback.
- **Locking.** X MDL on each table, IX on the schema, IX on BACKUP_LOCK, and a GRL IX lock (transaction scope). It does **not** take Percona's backup-tables protection, unlike `lock_table_names`.
- **Version checks** (`sql/dd/impl/sdi.cc:379-412`):
  - mysqld_version_id must be at most the server's version;
  - **dd_version must be equal**;
  - sdi_version must be equal.
  - The checker can be replaced: `SdiCompatibilityChecker` is a parameter of `deserialize` (`sdi.h:153-186`).
- **What `store_in_dd` does:**
  - refuses if the schema is missing or the table already exists;
  - requires .MYD/.MYI files, so in practice it works only for MyISAM;
  - `dc.store` (writes a new .sdi file), updates views that reference the table, and test-opens the table with errors turned into warnings.
  - No engine create or import call is made.
- **Not binlogged.** It is still flagged CF_POTENTIAL_ATOMIC_DDL.

## C. InnoDB: discard

Code: `ha_innobase::discard_or_import_tablespace` (`ha_innodb.cc:16027`) and `row_discard_tablespace_for_mysql` (`row0mysql.cc:3919`).

- Refused for read-only mode, temporary tables, the system tablespace and general tablespaces.
- Refused if another table has a foreign key to this one, unless foreign_key_checks=0.
- Assigns a new table id, drops the FTS auxiliary tables, and **deletes the .ibd right away**, before the DD commits.
- So DISCARD is not atomic for the file, even though the comment at `sql_table.cc:11816` says it is.
- `dd_table_discard_tablespace` (`dict0dd.cc:879`) writes the index roots, the discard flag and the tablespace state.

## D. InnoDB: import (`row_import_for_mysql`, `row0import.cc:4413-4925`)

- **Order:** read .cfp, read .cfg, check the schema against the .cfg, Phase I page conversion, then under the dict lock:
  - instant metadata into the DD;
  - open the space, encryption, compression;
  - fix the clustered index, purge delete-marked records, set the max row id, fix the SDI root;
  - flush, mark the space imported, rotate the encryption key, set autoinc;
  - validate the clustered index (an error if corrupt) and the secondary indexes (marked corrupted if bad).
- **.cfg versions:** V1 through V7; export always writes V7. V7 adds row versions, column phy_pos, version_added/dropped, and the full DD metadata of dropped columns.
- **Schema match:** table flags (DATA_DIR ignored by Percona PS-8116), column count, index count, compression, each column by name, each index's fields, page size, encryption.
- **Page conversion** (`fil_tablespace_iterate`, `PageConverter`):
  - Done **in place on the user's .ibd**, outside the buffer pool, with no redo and no doublewrite.
  - Rewrites the space id, maps source index ids to target ids, sets DB_TRX_ID/PAGE_MAX_TRX_ID to the import transaction, resets roll pointers, rewrites BLOB space ids, purges delete-marked rows, and sets the LSN and checksum.
- **Without a .cfg:**
  - Index roots are found by scanning every page; a root is any index page with no prev or next page.
  - Roots are matched to indexes by **order**, not by name.
  - No column or type checks are done.
  - Any record with a row version fails with "CFG file is must".
  - Autoinc becomes 0, which later triggers a MAX() scan.
- **Failure:** the in-memory state is reset and the DD still says discarded. The .ibd is left **partly converted**. A retry with the same .cfg then fails because the rewritten pages carry the target's index ids.
- **No log_ddl** in import or discard **[verified]**. Durability comes from a flush and a checkpoint before the DD commit.

## E. Bugs and gaps found (not filed)

1. **AUTO_INCREMENT is set in memory only on import** **[verified]** (`row0import.cc:4893-4906`).
   - Import never calls `dd_set_autoinc` and never updates `autoinc_persisted`.
   - After a restart the stale pre-import DD value wins. This is the root cause of **PS-8687**.
2. **A V7 .cfg loses the old V1-style INSTANT ADD metadata** **[verified the dispatch]**.
   - `set_instant_info` always takes the `_v2` path for V7 (`row0import.cc:2045`).
   - `_v2` returns early when neither side has *row versions* (1883). A table that used INSTANT ADD on 8.0.12-8.0.28 has instant columns but no row versions, so its instant_cols and defaults are never applied.
   - This matches the **PS-9459** reporter's analysis (8.0.21 INSTANT ADD, then upgrade, then export/import), and is a likely cause of PXB-3751.
3. **Dead check:** `sdi_tablespace.cc:229` looks for the key `"discarded"` **[verified]**. The real key is `discard`; "discarded" is one of the values of `state`.
4. **DISCARD deletes the .ibd before the DD commit** (see C).
5. Import without a .cfg can misread rows that physically contain an INSTANT-dropped column but have no version bit.
6. **A late failure leaves the cache wrong.** The `discard_after_ddl` FIXME is commented out, so in-memory instant changes are not undone if import fails later. The cache is also marked not-discarded before the SQL layer commits.
7. `page_delete_rec` may delete delete-marked records with external fields without freeing their LOB pages, which would leak space.
8. The error ER_IMP_INCOMPATIBLE_CFG_VERSION names V5 as the highest version, but V7 is supported. `space` is used after `fil_space_release` (around 4813/4858/4914).
9. **PXB, from the earlier survey:** `xtrabackup.cc:7565` tests the `dberr_t` returned by `xb_export_cfp_write` with `!`, so `.cfp` write errors are ignored **[verified]**.

## F. SDI handling, and what the new feature must add

- **Today's import never reads the SDI.** It only converts SDI pages (the SDI index id is a constant, so no remapping is needed) and fixes the SDI root. The SQL layer then **drops the source SDI** and writes the target's.
- **No InnoDB API can read SDI from an .ibd that is not registered:**
  - `dict_sdi_get` is `#if 0` (WL#9761) **[verified]**;
  - `dict_sdi_get_keys` and `ib_sdi_*` need fil_system and the buffer pool;
  - the buffer pool checks the space id on each page, so a loose file would collide.
  - A new reader must read pages directly, as `utilities/ibd2sdi.cc` does, or reuse import's page-callback loop.
  - `ibd2sdi` has no encryption support, and the SDI pages of an encrypted table are encrypted, so the key (.cfp or backup keyring) is needed **before** the SDI can be read.
- **What the SDI gives that the .cfg gives today:**
  - column se_private_data: defaults, version_added/dropped, physical_pos, and the hidden dropped columns;
  - index id and root, which give an **exact** source-to-target index mapping instead of the order heuristic;
  - table instant_col and table options.
- **What the SDI cannot give:**
  - the tablespace key (still needs a .cfp, or the key from the backup's keyring);
  - a current AUTO_INCREMENT (the SDI value is from the last DDL);
  - FTS auxiliary tables. Today import never recreates them: the FTS index is marked corrupted and the user has to drop and re-add it.
- **Fit with "list failed tables":** in the new path the DD row and the InnoDB import are in one statement. On failure the DD rolls back and no table exists; xtrabackup then deletes the half-converted file it copied (the backup still has the original) and lists the table.

## G. Permissions, paths and file handling in ALTER TABLE ... IMPORT TABLESPACE (checked 2026-10-15) **[verified]**

PS origin/8.4 ab8db2d1dbe.

- **Privilege:** ALTER on the table only (`sql_alter.cc:410-415`). No FILE and no CREATE (the table already exists).
- **secure_file_priv:** never consulted. There is no reference to it in `sql_table.cc`, `row0import.cc`,
  `ha_innodb.cc` or `ha_innopart.cc`.
- **Paths:** the user never names a file. The server derives every path from the table:
  - `.ibd`: `<datadir>/<db>/<table>.ibd`, or the DATA DIRECTORY path stored in the DD
    (`row0import.cc:4637-4647`);
  - `.cfg` and `.cfp`: next to the `.ibd` (`row0import.cc:4226`, `:4348`);
  - `fil_adjust_name_import` handles the case of partition names.
- **Ownership and mode:** the server does nothing. If mysqld can't open the `.ibd` read-write, the import fails
  with ER_FILE_NOT_FOUND (`row0import.cc:4669-4683`).
- **No .cfp:** the table is assumed unencrypted (`row0import.cc:4357-4361`).
- **Cleanup:** `.cfg` and `.cfp` are left behind after the import; nothing deletes them.
- **Refused:** read-only server, temporary tables, the system tablespace, general tablespaces, and Percona
  compressed columns.

Other SDI facts checked on 2026-10-15:
- **Partitioned tables:** the table SDI is stored only in the first partition's tablespace
  (`store_tbl_sdi` → `fetch_first_tablespace_id`, `sql/dd/impl/sdi_tablespace.cc:76,138`).
- **Triggers:** not part of the table SDI (`Table_impl::serialize`, `sql/dd/impl/types/table_impl.cc:547-584`).
  They must come from the backup's `mysql.triggers`.

## H. Replication of DISCARD / IMPORT TABLESPACE (checked 2026-10-15) **[verified]**

- **What is logged.** `mysql_discard_or_import_tablespace` calls `write_bin_log` with the statement text
  (`sql_table.cc:11830`). It goes into the transactional cache with the DD update, so it commits or rolls back
  together with it. `binlog_query` skips the write when `sql_log_bin=0`.
- **No file data is replicated.** The replica gets only the statement text and runs it against its own datadir.
- **DISCARD on the replica** deletes the replica's own `.ibd` (`row_discard_tablespace_for_mysql`).
- **IMPORT on the replica** looks for `<replica datadir>/<db>/<table>.ibd`.
  - If the file isn't there, `fil_tablespace_iterate` fails with "Trying to import a tablespace, but could not
    open the tablespace file" and DB_TABLESPACE_NOT_FOUND (`fil0fil.cc:8671-8677`).
  - The applier then stops with that error. The atomic DDL rolls back, so the table stays discarded.
  - Copying the files onto the replica and running START REPLICA retries the event, and it then succeeds.
- **In practice** users either copy the files to every replica (or wait for the stop, copy, then START REPLICA),
  or run the whole procedure on each server with `sql_log_bin=0`.
- **IMPORT TABLE (.sdi)** has no `write_bin_log` call (`sql_import.cc`), so it is never logged.
