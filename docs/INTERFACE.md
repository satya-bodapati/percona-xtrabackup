# Contract between the PS server work and the PXB work (single-table restore v1)

Source of truth for the decisions: DESIGN.md. This file fixes the details both sides code against.
Server branch: ~/WORK/ps/ps-8.4-import-refactor. PXB branch: ~/WORK/pxb/pxb-single-table-restore.

## The statement

    IMPORT TABLE FROM '<path>' [, '<path>' ...]

- **`<path>` names an InnoDB data file the client has already placed.**
  - Relative paths resolve against the datadir, as today's IMPORT TABLE does: `'db1/t1.ibd'`.
  - A DATA DIRECTORY table uses the absolute path of its file: `'/ext/dir/db1/t1.ibd'`.
  - Globs work as in today's IMPORT TABLE: `*`, `?`, with `\` as escape.
- **A partitioned table needs every partition file named,** explicitly or by glob: `'db1/t1#p#*.ibd'`. Partition
  file names are the encoded, lowercase names stored in the backup (`t1#p#p0.ibd`, `t1#p#p0#sp#s0.ibd`). The table
  SDI is read from the first partition. A missing or extra partition is an error.
- **File naming must be exact.** Schema and table come from the SDI. The named file must be exactly the table's own
  path, with filename encoding (`@0024` etc.), otherwise ER error and nothing is imported.
- **The existing `.sdi` form is unchanged.** A statement that names `.sdi` files keeps working, and mixing `.sdi`
  and `.ibd` in one statement is an error.
- **Encrypted tables:** a `<file>.cfp` must sit next to each `.ibd` (same basename, `.cfp` extension), in the same
  format `xtrabackup --export` writes. The server reads it before reading the SDI. It does not delete it, so the
  client deletes it.
- **A `<file>.cfg`, if present, is ignored.**
- **Privileges:** CREATE and ALTER on each target table. FILE is not required. `secure_file_priv` is not checked for
  `.ibd` paths.
- **All-or-nothing:** one statement is one transaction. On failure no table from the statement exists, and the
  server deletes the files it converted.
- **The server never moves or copies files.** It converts them in place and takes ownership.
- **Not binlogged.**

## Server behaviour the client relies on

- **Existing table:** the statement fails with ER_IMP_TABLE_ALREADY_EXISTS. A missing schema fails with
  ER_IMP_SCHEMA_DOES_NOT_EXIST.
- **Foreign keys** are kept as defined. Under `foreign_key_checks=0` a missing parent is allowed.
- **AUTO_INCREMENT:** the server sets and **persists** MAX(col)+1. The client then raises it with
  `ALTER TABLE ... AUTO_INCREMENT=N`.
- **FULLTEXT:** each FULLTEXT index is marked corrupted after import, and searches fail with ER_TABLE_HAS_NO_FT.
  The client rebuilds them: **one ALTER that drops all of them**, then **one ALTER per index** that adds it back.
  Neither step rebuilds the table (table_id and inode unchanged). Verified 2026-10-16:
  - One ALTER that drops and adds the same index is a no-op: the SQL layer sees no change.
  - Adding a FULLTEXT index fails while another one on the table is corrupted ("drop this index first").
  - InnoDB adds one FULLTEXT index per ALTER (ER_INNODB_FT_LIMIT).
  - OPTIMIZE TABLE with innodb_optimize_fulltext_only does not repair it.
- **Refused, so the client reports a per-table error:**
  - compressed columns (v2);
  - general and system tablespaces;
  - non-InnoDB tables.
- **Version:** the SDI must come from the same series. dd_version and sdi_version must be equal, and
  mysqld_version_id must be no newer than the server.
- **Triggers are not created by the server.** The client creates them.

## Client (`xtrabackup --restore`) sequence per table

The session sets `sql_log_bin=0` and `foreign_key_checks=0`.

1. Run the early checks:
   - the table doesn't exist on the server;
   - a DATA DIRECTORY path passes the CREATE TABLE known-directory rule.
2. Copy the `.ibd` (and partition files) from the prepared backup to the exact target path. Set owner and group
   from `<datadir>/<db>/`, mode 0640.
3. For an encrypted table, write `<file>.cfp`, reusing `xb_export_cfp_write`.
4. Run `IMPORT TABLE FROM '<db>/<t>.ibd'`, listing every partition file for a partitioned table.
5. Delete the `.cfp`.
6. Run `ALTER TABLE ... AUTO_INCREMENT=<backup counter>`, if the table has an AUTO_INCREMENT column.
7. Rebuild the FULLTEXT indexes: one ALTER drops them all, then one ALTER per index adds it back.
8. Run CREATE TRIGGER for each trigger, in action_order, with the original definer, sql_mode and charsets.
9. If IMPORT fails, delete whatever the client copied that is still present, and record the table as failed.

Missing databases are created first with the backup's charset, collation and default encryption.
