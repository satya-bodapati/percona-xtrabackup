# IMPORT TABLE FROM '.ibd': corner cases probed (2026-10-17)

Probed on `ps-8.4-import-refactor` (3debe50b2f9), debug build, with MTR tests kept in `probe2/` of the PS build dir
(`zz_probe_part`, `zz_probe_instant1-4`, `zz_probe_files`, `zz_probe_files2`, `zz_probe_dd`).

## Fixed (2026-10-17, PS branch `ps-8.4-import-refactor`)

Every fix comes with cases in the new test `innodb.import_table_ibd_mismatch`, which fails without it.

- `782d48b3429` **The crash (problem 1).** The page converter checks the row version, and the old INSTANT flag, of
  each row before it parses the row. A partition file newer than the definition gives "page N of the tablespace
  file has rows written by a later definition of the table". This also covers ALTER TABLE ... IMPORT TABLESPACE
  with a `.cfg` that doesn't match the file.
- `8fe865aadae` **Exact partition files (problems 2 and 3).**
  - A new handlerton call, `sdi_check_import_files`, runs for every table before any file is locked or changed.
    InnoDB requires one named file per partition, named after it, and holding the tablespace whose space id the
    definition records for that partition.
  - Swapped partitions give "holds partition `p1`, not partition `p0`". A partition of another table is named as
    "a file of another table". A named file that is no partition is refused, and an unnamed file is never imported.
  - All named files are kept.
  - A partition file in another directory is reported as such.
- `913cccd2b4e` **Checks before creating (problem 2).** The schema, the table name and the FK parents outside the
  statement are checked for every table before the first is created, so a later table's failure no longer
  deletes the earlier tables' files.
- `cf80e2d0608` **SRID (problem 5).** A column whose spatial reference system is missing is refused
  (ER_SRS_NOT_FOUND), as CREATE TABLE does.
- `b2edad29d1b` **Message.** A page of an index the definition doesn't have, such as a partition copied after an
  ADD INDEX, gives a schema mismatch naming the table, partition and page, instead of "Cannot reset LSNs ... Data
  structure corruption".

**Regression.** All `import_table_ibd*` tests and the 68 existing IMPORT TABLESPACE, IMPORT PARTITION and IMPORT
TABLE tests pass, except three that also fail on the upstream base `5db7de6b67b` today: `innodb.sdi_compressed`
(stale-page assertion), `clone.error_ddl` and `innodb.partition_upgrade_create` (no `zip` binary). The first two
passed in the baseline of 2026-10-16. The original probes, rerun on the fixed build, are in `probe/corner_cases/after/`.

## Fixed (2026-10-18): the checks CREATE TABLE makes

Probed first with `zz_probe_checks` (log in `probe3/` of the PS build dir): all four cases reproduced on
`b2edad29d1b`. Every fix adds its cases to the new test `innodb.import_table_ibd_checks`; the probe shows each case
behaving differently without its fix.

- `f6656e96ab5` **FK parents first.** `IMPORT TABLE FROM 'a1.ibd', 'c.ibd', 'p.ibd'` with `c` referencing `p` failed
  with "Failed to open the referenced table 'p'" and the rollback deleted `a1.ibd`. The targets are now ordered so
  that parents imported by the same statement are created before their children. Tables referencing each other are
  refused before any file is changed when `foreign_key_checks` is on, and imported in the named order when it is off.
- `8444be7ee69` **REFERENCES privilege.** CREATE and ALTER on the schema were enough to import a table with a foreign
  key to any table. Now REFERENCES on each parent table is needed, with CREATE TABLE's error.
- `ec0e68f602d` **`table_encryption_privilege_check`.** An unencrypted file could be imported into a schema with
  DEFAULT ENCRYPTION='Y' by anyone who may create tables there. Now, as for CREATE TABLE: with the check on, a table
  encrypted unlike the schema's default needs TABLE_ENCRYPTION_ADMIN (ER_CANNOT_SET_TABLE_ENCRYPTION); with it off,
  an unencrypted table in an encrypted schema gets warning 3824.
- `c98c8bd46f0` **`disabled_storage_engines`.** With InnoDB listed, CREATE TABLE is refused but IMPORT went through.
  Now IMPORT is refused with the same error, before any file is read.

Not fixed, by analysis: a file whose SDI names `performance_schema` or `information_schema` as its schema (only a
crafted SDI can) is refused by `check_access`, which denies CREATE in those schemas to everyone; a crafted SDI of a
data dictionary table is refused as "already exists". `mysql` and `sys` accept user tables, as CREATE TABLE does.

For `xtrabackup --restore`: it creates missing databases with their charset but without DEFAULT ENCRYPTION, so with
`table_encryption_privilege_check` on, restoring encrypted tables needs TABLE_ENCRYPTION_ADMIN. To do: create the
database with the same DEFAULT ENCRYPTION, if the backup records it.

**Still open:**
- Partitions from different copies with no DDL between them can't be detected (problem 4). Document it.
- A failure found while converting (corrupt pages, a newer row, an incompatible FK) still deletes the files that
  the statement already converted. Those files are half converted and can't be imported again. Document that
  IMPORT consumes its files.
- Messages: "general tablespace" for mysql.ibd and ibdata1; "no SDI" for a live table's file; absolute paths.

## How partitions were imported before the fixes

- **Which file has the definition.** Only the first partition's file holds the table's SDI. The other partition
  files hold none, so they are tied to the table by name only: same directory, `<table>#p#` prefix, and the
  count of named files must equal the number of leaf partitions.
- **What InnoDB does per partition.** It opens the file at the partition's standard path. It builds that
  partition's metadata from the one SDI, with the file's own space id, then converts the pages.
- **What catches a wrong file.** Two checks:
  - the page converter refuses pages whose index id isn't in the partition's definition (each partition has its
    own index ids);
  - after conversion, `btr_validate_index` checks every index.

## Problems found

1. **Crash: partitions from both sides of an INSTANT ADD/DROP COLUMN.**
   - Case: p0, which holds the SDI, was copied before the ALTER, and p1 after rows were written with the new
     column.
   - The debug server asserts during IMPORT (`rec.h:1042 !rec_new_is_versioned(rec)`, in
     `PageConverter::update_records`). INSTANT DROP gives the same assertion.
   - Cause: every partition's metadata comes from p0's SDI, and the converter doesn't check a record's row version
     against the table's. ALTER TABLE ... IMPORT TABLESPACE is safe here only because each partition has its own
     `.cfg`.
   - Release build (verified): the assertion is `ut_ad`, so the converter parses the rows with the wrong layout
     and writes the system fields at the wrong offsets. Here the index validation after conversion caught it
     ("Summed data size 58, returned by func 59", then "Clustered index validation failed, due to data file
     corruption"), and the import was refused. Nothing guarantees that for other row layouts: parsing a row with
     the wrong lengths can also read past it.
   - The same exposure exists for one file whose SDI page is older than its data pages, such as a file copied
     while the server runs. Not reproduced: I couldn't force that flush order.
   - Fix: the converter refuses a versioned record when the table has no row versions, or when the record's
     version is newer than the table's current one. The converter already visits every record.
2. **A failed import deletes the files it had already converted.**
   - Cases:
     - two partition files swapped: p0's file was deleted;
     - a partition renamed to a name that is no partition: p0 and p1 were deleted;
     - two tables, the second refused for a missing FK parent: the first table's file was deleted.
   - Files refused before conversion are kept.
   - With `--restore` the files are copies, so nothing is lost. Someone importing by hand loses their copy.
   - Fix:
     - run every check for every table (names, partition names, FK parents) before converting any file;
     - document that IMPORT consumes the files it imports.
3. **A file that wasn't named gets imported.**
   - Case: named `p0`, `p1` and `zz`, while the real `p2` file was in the directory. IMPORT succeeded using the
     unnamed `p2` file and ignored `zz`. A stale file at an unnamed path would be imported silently.
   - Fix: each named file must be exactly a leaf partition's file, and every leaf partition's file must be named.
4. **Partitions from different points in time are accepted silently.**
   - Case: the same table, with no DDL between the copies, or with p0 the newer copy.
   - The result mixes rows from both copies: one row from the later copy is there, one is missing.
   - The files can't show this.
   - Fix: document that every file of a table must come from one copy (one backup, or one FLUSH TABLES ... FOR
     EXPORT).
5. **The table is created with an SRID that doesn't exist.**
   - CREATE TABLE refuses this with ER_SRS_NOT_FOUND. IMPORT bypasses CREATE TABLE's checks.
   - Reads warn. Spatial index lookups and INSERT fail until the SRS is created again. No crash.
   - Fix: run the SRID check, and review the other CREATE TABLE checks IMPORT skips: system schemas,
     `table_encryption_privilege_check`, `disabled_storage_engines`.

## Refused correctly, but the messages could be better

- These give "Internal error: Cannot reset LSNs ... Data structure corruption":
  - swapped partition files;
  - a partition file from after an ADD INDEX;
  - a corrupt clustered or secondary index root page.

  The message should say which file doesn't belong to the table.
- A partitioned table split across two schema directories gives "needs all 3 partition files, 2 were named",
  although 3 were named.
- mysql.ibd and ibdata1 give "general tablespace".
- A hard link or symlink to a live table's file gives "the tablespace has no SDI; it was not created by MySQL 8.0".
  The real cause: the SDI page wasn't flushed yet.
- Errors print absolute datadir paths.

## Refused correctly, with clear messages

- **Placement:**
  - a whole table in another schema's directory, or renamed to another table name;
  - a partition file named as a whole table;
  - only the first partition, or no first partition;
  - a file defining a table that another named file defines too;
  - the live table's own file ("already exists").
- **Damaged or foreign files:** zero-length, truncated, random bytes, a corrupt SDI page, an undo tablespace,
  FULLTEXT aux files.
- **Dictionary conflicts:**
  - an FK parent with another column type, or without an index ("Cannot add foreign key constraint");
  - an FK name or CHECK constraint name already used in the schema.

## Worked

- The correct partition files.
- Empty partitioned and empty plain tables: AUTO_INCREMENT starts at 1.
- The first partition dropped before the export: the SDI is in the new first partition.
- A table that went through EXCHANGE PARTITION, on both sides.
- Names that need encoding (`t-1 ñ`).
- A table with a generated invisible primary key.

## Not covered yet

- `lower_case_table_names` 1 and 2, which need their own server, and name case collisions.
- A partition file from another server whose index ids match, such as a replica with the same DDL history. The
  index id check passes, so only fix 1 and the index validation would protect it.
- Two sessions importing the same file at once.
- Crafted SDI content.
