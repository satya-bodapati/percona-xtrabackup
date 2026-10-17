# Single-table restore redesign: tickets and current state

Research only, 2026-10-15. No design, code or comments yet.

**Source of the ticket data.** The Atlassian MCP (Rovo) failed to connect in this session
(CONNECT_TIMEOUT), so every ticket below came from anonymous
`perconadev.atlassian.net/rest/api/3` reads. Those reads only return **public** issues and
links. Restricted tickets (security, customer escalations, internal-only) may be missing. Rerun
the searches with the MCP once it reconnects to find them.

JQL used:
- summary searches in PXB and PS for: export, partial, import, cfg, cfp, transportable,
  tables/databases filters, individual, single table, partition, SDI, FOR EXPORT, ibd2sdi,
  instant + import/export
- text searches in PXB (2021 and later) for "IMPORT TABLESPACE", "--export", ".cfg", ".cfp",
  "partial backup", "partial restore", "--tables", "--databases", SDI, "restore table"
- links into and out of PXB-2745 and PXB-3153, and every issue that mentions either key

Raw dumps (description plus comments) are in the session scratchpad. Ask if you want them
copied here.

**Answer to "is there a PS ticket linked from PXB-2745?":** No. The epic links only PXB-3169,
PXB-3025, PXB-3634 and PXB-3662, plus a remote link to a Confluence page that mentions it
(confluence.percona.com pageId=68390286, not readable from here). No PS issue mentions
PXB-2745 or PXB-3153 in its text either. The caveat above about restricted links applies.

---

## 1. Core redesign tickets

| Key | Title | Status | Fix ver | Summary | Relation |
|---|---|---|---|---|---|
| **PXB-2745** | PXB Single Table Restore Redesign | On Hold (Epic) | none | Back up only the data needed for a set of tables, and pull specific tables out of a full backup, then restore them into a **running** instance. Labels: strategic, reviewed. Assignee: Satya. Satya's 2026-02-03 comment: users currently have to include the system files themselves, and xtrabackup should add every needed file automatically. | **The epic.** |
| **PXB-3153** | Generate table definition on xtrabackup export | On Hold (child of 2745) | none | `--prepare --export` would write a `<table>.sql` (CREATE TABLE) per .ibd, built from the SDI as dd::Table, then SHOW CREATE. No new option. 2026-03-30 comment points to the fork branch `generate_schema`. | First step of the epic: removes the "remember the schema" step. |
| PXB-3169 | Efficient partial backups in XtraBackup | Done / Duplicate of 2745 | none | Customer feature request (yunus.uyanik). Hundreds of databases; asks xtrabackup to automate CREATE, DISCARD, copy, IMPORT. Proposed shape: `xtrabackup --restore-partial --datadir --target-dir --tables=... --createtables=1`. | User requirements for the epic. |
| PXB-3025 | xbcloud should support directory downloads | Done / Duplicate (2745) | none | `xbcloud get` can fetch named files but not a whole db directory, which partial recovery from cloud needs. | Cloud side of the epic: fetching only the needed files. |
| PXB-1174 | LP#1027172: include table definition (CREATE TABLE) for exported tables | Done / Not a Bug (2013, LP blueprint) | none | Same idea as PXB-3153, from 2013: save CREATE TABLE at backup time. | Old ancestor of PXB-3153. |

## 2. Bugs and gaps the redesign should fix or absorb

| Key | Title | Status | Fix ver | Summary | Relation |
|---|---|---|---|---|---|
| **PXB-3634** | FTS files aren't copied if the table is passed through --tables or --tables-file | Open (linked to 2745) | none | Filtering by `--tables` skips the table's `fts_*` auxiliary .ibds, so a FULLTEXT table cannot be restored. Reported on 8.0.35-34. | Table selection must pull in dependent tablespaces: FTS aux, and likely partitions and system files. |
| **PXB-3662** | Improve the documentation of mutual exclusion of --tables and --databases | In Review (Satya) | none | QA (2026-09-09) found the docs are **backwards**: `--databases` wins and `--tables` is silently dropped. | The redesign should define how filters combine instead of relying on documented precedence. |
| PXB-2896 | Encrypted partitioned tables are not restored | Open | none | After full plus incrementals with encryption, some partitioned tables are missing. Cause: the 8.0.29 redo parse missed the encryption info, so no .delta was written and prepare deleted the .ibd. Low repro rate. | Encryption plus partitions in restore. Possibly fixed by later redo work; needs rechecking. |
| PXB-2776 | Partitioned tables not restored correctly after crash recovery with pstress | Open | none | 2.4/PS 5.7 report. | Old; likely obsolete for 8.4. |
| PXB-3083 | All RocksDB tables are included with partial xtrabackup | Open | none | Filters ignore MyRocks; the whole RocksDB checkpoint is copied. | Partial-backup scope: decide whether non-InnoDB is out of scope or an error. |
| PXB-3421 | xtrabackup got signal if --databases= very long | Open (verified) | none | SIGSEGV in a vfprintf path with a long `--databases`. | Filter option handling; cheap to fix along the way. |
| PXB-3811 | Reduced-lock backup can miss tablespaces created by ALTER TABLE ... IMPORT TABLESPACE | Open (child of PXB-3818, Jakub) | none | A concurrent IMPORT during a reduced-lock backup is not captured. | Interacts with restoring into a running server that is itself being backed up. Already in the reduced-lock bug set. |
| PXB-2589 | Xtrabackup 8 cannot remove missing tables from the InnoDB DD during prepare | Done / Won't Do | none | Docs say excluded tables are dropped from the DD at prepare; on 8.0 they are not. Prepare prints "Could not find any file associated with tablespace ID". | Partial backup plus copy-back gives an inconsistent DD (same as PXB-2178 and PXB-1741). The docs claim is still in the 8.4 docs. |
| PXB-2178 | Restoring datadir from partial backup results in inconsistent data dictionary | Done / Won't Do (2025 housekeeping) | none | Copy-back of a `--databases` partial backup leaves DD entries with no files. Workaround: DROP DATABASE after start. | Core reason "copy-back a partial backup" is broken in 8.x. The redesign should either refuse it or fix the DD. |
| PXB-1741 | PS startup displays errors for databases excluded during partial backup | Done (docs only, 8.0.27-19) | 8.0.27-19 | Fixed by documenting "PXB 8 does not remove missing tables; DROP them after start". | Same as above. |
| PXB-2264 / PS-8687 | Auto increment information is removed after partial restore of a table / Incorrect autoinc in imported tablespace after restart | PXB Done (8.0.32-26); **PS-8687 Open** | 8.0.32-26 (PXB) | After IMPORT plus a server restart the AUTO_INCREMENT counter is wrong, giving duplicate key errors. PXB part fixed; PS-8687 (server side) is still open. | See section 3. |
| PXB-2797 (dup PXB-3044) | Schema mismatch when importing table with full-text index from xtrabackup | Done | 8.0.35-31 | `.cfg` was wrong for FTS tables (FTS_DOC_ID / hidden index). | Fixed. Its test block in xb_export.sh is regression coverage to keep. |
| PXB-3283 (rel PS-9219) | Column %s precise type mismatch error while importing a tablespace | Done | 8.4.0-1, 8.0.35-31 | Temporal columns keep collation id 8 in the .ibd while the DD has the table charset, so the .cfg prtype disagrees. | Fixed. A generated CREATE TABLE path must reproduce the same quirk. |
| PXB-2543 (dup PXB-2808) | Add IB_EXPORT_CFG_VERSION_V6 for exporting cfg file | Done | 8.0.27-19 | Export was writing cfg V2, which broke instant columns and compression. | Fixed. History of cfg version tracking. |
| PXB-2157 | --prepare --export: generate cfg for partition tables | Done | 8.0.12 | | Fixed. |
| PXB-3751 | IMPORT TABLESPACE fails with "Externally stored column" | Done / Cannot Reproduce | none | Table with INSTANT row versions; import fails with the PXB cfg, the FOR EXPORT cfg, and no cfg. OPTIMIZE fixes it. | Instant DDL plus import, probably server-side (see PS-9459). |
| PXB-2503, PXB-2843, PXB-2393, PXB-2199 | export crash (signal 11) / schema mismatch | Done / Incomplete or Cannot Reproduce | none | Field reports of `--prepare --export` crashes and schema mismatches with no repro. | Shows how fragile the manual flow is. |
| PXB-3173 | Partial download functionality regression (xbcloud) | Done | 8.0.35-30 | The fifo rework broke `xbcloud get` of named files. | Same area as PXB-3025. |
| PXB-2848 | Document how to restore a single table from a full backup | Done | 8.0.35-32 | Lists the files needed per table: ibdata1, undo_*, mysql.ibd, xtrabackup_* and the table .ibd. | That file list is what PXB-2745's "include needed files automatically" would automate. |
| PXB-3791 | Improve "HowTo" section in xtrabackup docs | On Hold (docs, Satya) | none | Includes "how to restore a single table". | The docs need rewriting once the redesign lands. |

## 3. Server-side (PS) dependencies

| Key | Title | Status | Fix ver | Summary | Relation |
|---|---|---|---|---|---|
| **PS-8687** | Incorrect autoinc in imported tablespace after restart server | **Open** | none | Server forgets the imported AUTO_INCREMENT after restart. Has an MTR (`innodb.export_autoinc`). | Any automated restore into a live server hits this. Needs a server fix or a workaround (ALTER TABLE ... AUTO_INCREMENT=). |
| **PS-9459** | import instant ddl table cause data_corruption | **Open** (upstream Bug #116677) | none | Table instant-altered on 8.0.21, upgraded to 8.0.36+, exported (cfg V7), reimported: data corrupt because `set_instant_info_v2` loses instant column offsets. Workaround: OPTIMIZE before export. | Affects the IMPORT step, whether the .cfg comes from PXB or the server. Probably the same issue as PXB-3751. |
| PS-9219 | MySQL converts collation of date data type in ibd but data dictionary | Done | 8.0.37-29, 8.4.0-1 | Collation id of temporal columns differs between SDI and DD after CONVERT TO CHARSET or CREATE LIKE. | Fixed. Matters if CREATE TABLE is generated from the SDI. |
| PS-6883 (rel PXB-2101) | Exchanging partition with tables does not update SDI correctly | Done | 8.0.25-15 | Stale SDI after EXCHANGE PARTITION. | Fixed. SDI must be trustworthy to generate schema from it. |
| PS-8914 | collation_database not honoured on import | Open | none | Concerns mysqldump import (logical), not tablespaces. | Marginal: only relevant if generated SQL omits COLLATE. |
| PS-5322 | Keyring encrypted tablespace import/export is broken | Done / Won't Do (2025 housekeeping) | none | 8.0 export format change broke keyring-encrypted import. Closed with no fix. | Encrypted import path. The keyring test `innodb_encryption_export.sh` passes today, so this may be obsolete. Should be verified with KMIP/component keyrings. |
| PS-7865 | Dropping a table with discarded tablespace crashes the server | Done | 8.0.29-21 | | Fixed. A redesign that leaves tables DISCARDed on failure depends on this. |
| PS-8116 | IMPORT TABLESPACE with different data directory fails | Done | 5.7.38-41, 8.0.29-21 | | Fixed. Matters for tables with DATA DIRECTORY. |
| PS-8930 | server crash on import tablespace | Done / Incomplete | none | Signal 11 importing a PXB-exported FK table. No repro. | Watch item. |
| PS-1104 | IMPORT TABLESPACE failed without cfg file | Done / Won't Do (LP) | none | | Old. Background for importing without a .cfg. |

No open PS ticket asks for a server-side "import from backup" API (for example, IMPORT that
creates the table from the SDI). If the redesign wants one, it needs a new ticket.

## 4. Related or old (background only)

- Partial backup docs and behaviour: PXB-1740, PXB-1742, PXB-2782, PXB-1181, PXB-867, PXB-1465 (prepare does not drop excluded tables, 2.4), PXB-714 (spurious missing .ibd, Won't Do), PXB-807, PXB-1018/PXB-1454815 (warnings for tables not in the partial backup; test bug1454815.sh), PXB-1701 and PXB-1687 (copy-back of partial backups, duplicates), PXB-2285 (MyISAM-only partial backup still copies ibdata/undo, Won't Do), PXB-634 (trigger definitions ignored in partial backups, Won't Do), PXB-2379 (creating an index on an excluded table causes DDL failure, Won't Do), PXB-2860 (`--tables-exclude` plus lock-ddl-per-table still locks, fixed 8.0.34-29).
- Partitions: PXB-354 and PXB-595 (back up individual partitions), PXB-631, PXB-1719, PXB-688, PXB-2239 (partitions changed during backup, fixed 2.4.21), PXB-2742.
- Export history: PXB-1163 (`--export` with `--tables-file`), PXB-1418 (repeat export), PXB-695 (export slow with a large change buffer), PXB-276, PXB-989, PXB-2995 (docs), PXB-2133/PXB-2132 (DECIMAL prtype), PXB-2215 (pack_length), PXB-2298 (transportable tablespaces crash xtrabackup, Won't Do), PXB-2891 (`--export` with incrementals, Not a Bug).
- 5.x expand-import era in PS (`innodb_import_table_from_xtrabackup`): PS-342, PS-2769, PS-3128, PS-1697, PS-1699, PS-2086, PS-3748, PS-911. Historical only.
- Unrelated hits: PS-11427 "DDL tracker for InnoDB Engine" (Open) mentions .cfp; not part of this work, but worth a look if it changes how IMPORT is tracked.

---

## 5. How single-table restore works in 8.4 today (origin/8.4 = 9044713dda03)

Paths are under `storage/innobase/xtrabackup/src/`. No build was run.

**User flow (documented in `doc/docs/xtrabackup_bin/restoring_individual_tables.md`):**
1. Back up, full or partial with `--tables` or `--databases`.
2. `--prepare --export`, which writes `.cfg` (and `.cfp` if encrypted) next to each file-per-table .ibd.
3. On the live server, by hand: CREATE TABLE with the **same schema**, then `ALTER TABLE ... DISCARD TABLESPACE`, copy and chown the .ibd, .cfg and .cfp, then `ALTER TABLE ... IMPORT TABLESPACE`. Partitions are imported one by one, or the whole table at once.

**`--export` (`xtrabackup.cc:7530-7583`):**
- After apply it walks datafiles and skips anything that is not file-per-table, so **general and system tablespace tables get no export**.
- Rebuilds `dict_table_t` from each .ibd's SDI (`xb_dict.cc:583`).
- cfg version comes from `server_version` in xtrabackup_info (V3 through V7). V7 includes INSTANT ADD/DROP row versions and dropped-column metadata taken from dd::Table.
- `.cfp`: a random transfer key plus the tablespace key encrypted with AES-256-ECB. Needs the keyring during prepare.
- `.cfg` hostname is hard-coded as "Hostname unknown".

**Partial backups:**
- Filters are `--tables`, `--tables-exclude` and `--tables-file` (regexes or exact names), and `--databases`, `--databases-exclude` and `--databases-file`.
- `check_if_skip_table` / `check_if_skip_database` at `xtrabackup.cc:3220-3358` are applied in the copy, MDL, DDL-tracker and datadir-scan paths. The system tablespace and mysql.ibd are always copied.
- `partial=Y` is written to xtrabackup_info but **never read back** (`xtrabackup.cc:2867`).
- Prepare has no partial-specific logic; redo for missing spaces is dropped through `recv_sys->missing_ids`.
- `--copy-back` accepts partial backups (then PXB-2178 / PXB-2589 apply) and also copies .cfg and .cfp files.

**Code defects found while reading (not filed):**
- `xtrabackup.cc:7565`: `if (!xb_export_cfp_write(table))`, but that function returns `dberr_t` and `DB_SUCCESS` is 10. The check is always false, so `.cfp` errors are ignored.
- `goto next_node` on a `.cfg` failure jumps to the next statement anyway. Export errors never affect the exit code.
- If `get_dd_Table()` returns null for an INSTANT-dropped column (around line 6598), the writer logs an error with the wrong id and still writes a short `.cfg`.
- When one .ibd maps to several tables (the loop at 7560), each table writes the same `.cfg` path, so later ones overwrite earlier ones.
- TODO at 7585: `--export` and `--check-tables` each load the dictionary separately.

**Tests touching this** (`storage/innobase/xtrabackup/test/`):
- `t/xb_export.sh`: plain, partitioned, INSTANT ADD/DROP, CREATE LIKE plus charset change, and FTS/DESC/SPATIAL tables (PXB-2797). Uses DISCARD, IMPORT and a checksum.
- `t/bug930062.sh`: `--tables` then export and import.
- `t/bug1454815.sh`: `--databases` then export, with no DD warnings.
- `suites/keyring/innodb_encryption_export.sh`: encrypted, full plus incrementals, .cfp import.
- `suites/reducedlock/partial_backup.sh`: `--tables` with lock-ddl=REDUCED, then export and import.
- `t/skip_orphan_table.sh`: orphan FTS .ibd during export.
- `t/ib_databases.sh`, `t/ib_databases_file.sh`, `t/ib_part_databases.sh`: partial backup then copy-back or cp.
- `t/xb_databases_options.sh`: filter option validation.
- `t/xb_export_check_cfg.sh`: guards against a server cfg version above 7; does not run PXB.

**Docs drift:**
- `restoring_individual_tables.md:39-46` still shows `.exp` files.
- `partial_backups.md:118-124` claims excluded tables are removed from the DD; no code does that (PXB-2589).
- The `--tables` / `--databases` precedence statement is backwards (PXB-3662).

## 6. Existing branches and PRs

- **fork `pxb_generate_schema`** (a90f8b03bc3c, last commit 2026-04-01, about 32 commits on an 8.0 base from Nov 2023, f381b30d62bb, **no PR**). This is the PXB-3153 work:
  - `xb_dict.cc` dd::Table to CREATE TABLE generator, used by `--prepare --export`, which writes `.sql`.
  - `ibd2sdi --generate-sql[=FILE]` and `--sdi-to-sql` (`utilities/ibd2sdi.cc`, +1095 lines).
  - Tests: `generate_schema.sh`, `ibd2sdi_generate_sql.sh` (compared with SHOW CREATE TABLE across column types, generated columns, FK, CHECK, partitions and subpartitions, FTS with parser, SPATIAL, general tablespaces, compression, INSTANT ADD/DROP) and `sdi_to_sql.sh`.
  - Needs porting from 8.0 to 8.4.
- **fork `generate_schema`** (d5124ee7d54d, Jan 2024): the 3-commit prototype that the branch above extends.
- No branch or PR on percona or the fork for PXB-2745, PXB-3634 or PXB-3662.
- Merged upstream PRs in this area: #1178 (PXB-2543), #844 (PXB-2133), #1276 (PXB-2264), #1534/#1535 (PXB-2797), #1559/#1560 (PXB-3283), #1520/#1521 (PXB-3173), #1150 and #797 (restore-individual-tables docs), #957/#962 (PXB-2239).
