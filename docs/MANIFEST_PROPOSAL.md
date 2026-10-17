> **Superseded (2026-10-16).** xbcloud reads only the always-plain manifest pair of the 9.7 framework
> (`backup_metadata.json`, `backup_files.jsonl`), and the DDL tracker adds `{"ddl": {"path", "final"}}` lines to
> `backup_files.jsonl`: see `PXB-3754_comment.md`. The 8.4 xbcloud filter commit was dropped, so phase 1 below
> (refusing on markers) will not be done.

# Selective download from an unprepared backup: proposal (agreed with Satya 2026-10-16)

## Problem

With `--lock-ddl=REDUCED`, a file's name in the backup is the table's name when the file was copied. DDL during the
backup leaves markers that prepare applies later (`.new`, `.crpt`, `<space_id>.ren`, `<space_id>.del`). After a swap
of `t1` and `t2`, `db1/t1.ibd` becomes `t2`; after a cross-database rename, the final `db1.t5` is in `db2/x.ibd`. A
downloader that selects by file name can download the wrong table, miss the right one, or bring back a dropped one.
A prepared backup, or one with no markers, has final names, so name filtering is exact there.

## Principle

A backup describes what it has: one manifest line per file, saying which table the file belongs to at the end of the
backup.

## Manifest

One line per file: the path, and the table it belongs to (`-` for none, `*` for files every restore needs).

1. **While copying:** each file's line is appended when the file is copied, with its table at that moment. Lines are
   never rewritten; they may already be in the stream or uploaded.
2. **At the end of the backup:** the DDL tracker, under the backup lock, writes its markers as today, and appends a
   correction line for each file whose table changed and a line for each marker it created. Same format.
3. **Rule:** for a path, the last line wins. A path has at most two lines: the copy line and the tracker's correction.
   Files the tracker didn't touch, almost all of them, have one line that is already right.

```
db1/t1.ibd   db1.t1      copy time
db1/t2.ibd   db1.t2
db1/t5.ibd   db1.t5
ibdata1      *
...
db1/t1.ibd   db1.t2      tracker: t1/t2 swapped
db1/t2.ibd   db1.t1
db1/t5.ibd   -           tracker: t5 dropped
db1/10.ren   db1.t2      tracker: new marker
db1/11.ren   db1.t1
db1/13.del   -
```

## Reader

One pass, top to bottom, keeping only the selected set: for each line, add the path if its table matches the patterns
or is `*`, otherwise remove it. Corrections come after the copy lines, so the last line decides. Memory grows with the
selection, not with the backup (backups can have 500k to 1M files).

`db1.t1` in the example gives `db1/t2.ibd` and `db1/11.ren` plus the `*` files. Prepare renames `t2.ibd` to `t1.ibd`.

## Scale

- Writer: appends one line per file; nothing is buffered.
- Size: about 50 MB for a million files, less compressed. It replaces the object LIST xbcloud does today.
- Tracker: already holds one entry per copied tablespace (`tables_copied_no_lock`); no change.

## Phasing

1. **8.4, now:** no manifest. `xbcloud get --restore-include` refuses when the listing has `.ren` or `.del` markers
   and says to download the whole backup. Otherwise name filtering is exact. This replaces today's always-download of
   `.ren` (breaks prepare when its table isn't selected) and skip of `.del` (can bring back a table dropped during the
   backup).
2. **With the manifest (9.7 framework, PXB-3754 / PXB-3787):** writer and reader as above.
3. **Tests:** reduced-lock backups with DDL during the backup (rename, swap, cross-database rename, drop, drop and
   recreate, rebuild, truncate, add index, encryption change, create). For each, subset download, prepare and
   `--restore` must give the same tables as the full backup.

## Open

- `--encrypt`: manifest in plain text (object names already show `db/table` paths) or encrypted.
- Incremental backups: refuse in subset mode until tested.
