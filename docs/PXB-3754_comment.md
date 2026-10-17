backup_files.jsonl: record where each file ends up, for selective download (PXB-2745)

**Use case.** `xbcloud get --restore-include=db1.t1` downloads only the files of the selected tables plus the files
every restore needs. The user then runs `--prepare` and `xtrabackup --restore`. xbcloud picks the files by name.

**Problem.** Filtering by name is exact with `--lock-ddl=ON`. With `--lock-ddl=REDUCED`, a DDL during the backup leaves
the file under the name it had when it was copied. It also leaves markers that prepare applies later: `.new`, `.crpt`,
`<space_id>.ren` and `<space_id>.del`. After a swap of t1 and t2, `db1/t1.ibd` holds t2. A dropped table's file is
still in the backup. Filtering by name then downloads the wrong file, misses the right one, or brings a dropped table
back. The DDL tracker already knows the final state under the backup lock, but the backup doesn't record it anywhere a
downloader can read.

**Proposal: a second kind of line in backup_files.jsonl.**

1. File lines, as planned: one per file the backup has, markers included.
2. DDL lines: one for each file whose final path differs from its path in the backup. The DDL tracker writes them at
   the end of `handle_ddl_operations()`, under the backup lock, after its own copies are closed. `final` is the path
   the file has after prepare, or `null` when no restore needs it.

```
{"ddl": {"path": "db1/t1.ibd",      "final": "db1/t2.ibd"}}
{"ddl": {"path": "db1/10.ren",      "final": "db1/t2.ibd"}}
{"ddl": {"path": "db1/t2.ibd",      "final": "db1/t1.ibd"}}
{"ddl": {"path": "db1/11.ren",      "final": "db1/t1.ibd"}}
{"ddl": {"path": "db1/t5.ibd",      "final": null}}
{"ddl": {"path": "db1/13.del",      "final": null}}
{"ddl": {"path": "db1/t7.ibd",      "final": null}}
{"ddl": {"path": "db1/t7.ibd.new",  "final": "db1/t7.ibd"}}
{"ddl": {"path": "db1/t8.ibd.crpt", "final": null}}
```

These lines say: t1 and t2 were swapped, t5 was dropped, t7 was recopied, and t8's copy was unusable.

**Reader.** A file's final path is the `final` from its DDL line if it has one, otherwise its own path. The reader makes
one pass. On a file line, it adds the file if the path matches the patterns or the file is at the backup root. On a DDL
line, it adds the file if `final` matches and removes it otherwise. That works because every DDL line comes after the
file line it corrects. Memory grows with the selection, not with the backup, which can have 500k to 1M files.

For `db1.t1` the example selects `db1/t2.ibd` and `db1/11.ren`. Prepare is unchanged: it applies the `.ren` and the
file ends up as `db1/t1.ibd`.

**Cost.** DDL lines exist only for files the tracker touched: usually none, at most a few hundred. File lines are
unchanged. With `--lock-ddl=ON`, nothing is added.

**Also needed for selective download:**

- **Object names.** Each file line gives the name the file is stored under, with its `.zst`, `.lz4` or `.xbcrypt`
  suffix. xbcloud then doesn't have to parse suffixes.
- **No full listing.** Each file line gives the number of chunks or segments the file was stored as. xbcloud can then
  name the objects it needs instead of listing a backup with millions of chunk objects.
- **Completeness.** `backup_files.jsonl` is complete before `backup_metadata.json` is written, so the metadata file's
  presence means the backup finished.

**xbcloud rules:**

- Without `backup_metadata.json`, refuse selective download and say to download the whole backup.
- `lock_ddl_type` ON: `backup_metadata.json` is enough. Names are final, so xbcloud selects by name. File lines, when
  present, only spare it the listing.
- `lock_ddl_type` REDUCED: needs `backup_files.jsonl` with its DDL lines. Without it, refuse.
- `lock_ddl_type` OFF: refuse.
- Refuse incremental backups for now. A rename between the full and an incremental changes names across the chain,
  which needs a chain-level mapping.
