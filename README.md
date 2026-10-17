# PXB-2745 single-table restore: design notes

Working notes for single-table restore (Percona Server `IMPORT TABLE FROM '<file>.ibd'` and `xtrabackup --restore`).
Notes only, no code. The code is on the fork branches `pxb-single-table-restore` (PXB) and `ps-8.4-import-refactor` (PS).

- `docs/STATUS.md`: what is implemented, tested and still open.
- `docs/DESIGN.md`: v1 design decisions.
- `docs/INTERFACE.md`: the contract between the server and xtrabackup.
- `docs/IMPORT_INTERNALS.md`: how InnoDB import works today, and the bugs found.
- `docs/IMPORT_CORNER_CASES.md`: corner cases of IMPORT TABLE FROM '.ibd' (partitions, misplaced and foreign files).
- `docs/AUTONOMOUS_DECISIONS.md`: calls made without review.
- `docs/TICKETS.md`: draft ticket texts, not filed.
- `docs/REPLICATION_IDEAS.md`: v2 replication (idea A2: the server ships the file through the binlog).
- `docs/PXB-3754_comment.md`: draft Jira comment: DDL lines in `backup_files.jsonl` for selective download.
- `docs/MANIFEST_PROPOSAL.md`: an earlier manifest proposal, superseded by the comment above.
- `repros/`: scripts that reproduce the import bugs.
- `probe/corner_cases/`: the MTR probes behind IMPORT_CORNER_CASES.md, with recorded results.
- `probe/`: an MTR probe showing that IMPORT TABLE is not binlogged, unlike ALTER TABLE ... IMPORT TABLESPACE.
