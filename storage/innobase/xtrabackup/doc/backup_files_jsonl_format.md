# `backup_files.jsonl` — per-file manifest

Every backup that completes successfully writes a `backup_files.jsonl`
file alongside `backup_metadata.json`. The format is **newline-delimited
JSON (NDJSON)**: one JSON object per line, generated incrementally as
each backup file finishes. Like `backup_metadata.json` it lands **plain
UTF-8** in every output mode (`--target-dir`, `--stream=xbstream`,
`--stream=... | xbcloud put`, `--extra-lsndir`) regardless of
`--encrypt` / `--compress`.

## Where it lands

| Output mode | Location |
|---|---|
| `--target-dir=/path/...` | `/path/.../backup_files.jsonl` |
| `--stream=xbstream` | A single bare-named chunk inside the stream; `xbstream -x` extracts to the target dir as `backup_files.jsonl` |
| `--stream=xbstream \| xbcloud put ...` | `<prefix>/backup_files.jsonl` as one bare-named cloud object |
| `--extra-lsndir=DIR` (any of the above) | `DIR/backup_files.jsonl` (byte-identical to the target-dir copy) |

## Wire format

The first line is a header:

```jsonl
{"manifest_version":1}
```

Every subsequent line is a per-file entry:

```jsonl
{"path":"sakila/film.ibd","space_id":42,"page_size":16384,"compress":"zstd"}
{"path":"sakila/film_actor.ibd","space_id":43,"page_size":16384,"encrypt":"AES256"}
{"path":"xtrabackup_info"}
```

Fields:

| Field | Type | When set |
|---|---|---|
| `path` | string | Always. Backup-root-relative POSIX path |
| `space_id` | uint64 | InnoDB tablespace files (data + redo headers, IBDs, undo) |
| `page_size` | uint32 | Same set as `space_id` |
| `compress` | string | When `--compress` is on: `quicklz` / `lz4` / `zstd` |
| `encrypt` | string | When `--encrypt` is on: `AES128` / `AES192` / `AES256` |

Top-level fields are forward-additive: unknown fields must be
ignored by readers so the format can grow without bumping
`manifest_version`. The header line's `manifest_version` is reserved
for **breaking** schema changes only.

## How the file is produced

`xb_files_jsonl::begin()` is called once at backup start, right after
`xtrabackup_init_datasinks()`. It opens a hidden staging file
`<target_dir>/.backup_files.jsonl.staging` and writes the header line
through atomic `O_APPEND`. The staging file uses a hidden name so it
does not collide with the published `backup_files.jsonl` later in the
same directory.

For each data file copied, `xtrabackup_copy_datafile_func` calls
`ds_open_with_ctx` + `xb_files_jsonl::new_file_ctx(path)` to attach
a fresh `rapidjson::Document` to the outermost `ds_file_t`. The
document travels with the file through the datasink chain. Each
wrapper datasink's close op annotates the document via
`xb_files_jsonl::set_string` before invoking the inner `ds_close`,
e.g. `ds_compress_zstd` stamps `"compress":"zstd"`, `ds_encrypt`
stamps `"encrypt":"AES256"`. Inner pipes carry `nullptr` for the
file context so no datasink double-fires the manifest append. When
the user-level `ds_close` returns, the document is serialised to
one JSONL line, appended to the staging file atomically (via
`O_APPEND` for lines under `PIPE_BUF`; a mutex guards larger
lines), and the document is freed. An `fdatasync` is issued every
1 MiB or 1 second, whichever comes first.

At `backup_finish` (after `backup_metadata.json` has been
published), `xb_files_jsonl::finalize()` closes the staging file
and `publish(ds_meta)` writes it through `ds_open_single_object`
so it lands plain across every output mode. When `--extra-lsndir`
is set, `write_to_dir()` also writes a byte-identical filesystem
copy under that directory. The staging file is then unlinked.

## Concurrency

`backup_files.jsonl` is appended from any thread that calls
`ds_close`. The producer guarantees:

* Lines `<= PIPE_BUF` (4 KiB on Linux) reach disk via a single
  `write(2)` on an `O_APPEND` fd — POSIX-atomic, no mutex needed.
* Lines `> PIPE_BUF` take a process-wide mutex around the multi-call
  write so concurrent threads do not interleave bytes.
* The per-file `rapidjson::Document` is owned by one thread for its
  entire lifetime (creation in `new_file_ctx`, annotations during
  the close chain, serialisation + free in `append_and_release`).
* `fdatasync` is rate-limited so worker threads doing many small
  closes are not bottlenecked on the disk.

## Consumers

Today the file is informational — operators and tooling can read it
without involving xtrabackup or xbcloud:

```sh
jq -c 'select(.encrypt)' backup_files.jsonl    # which files were encrypted
jq -r '.path' backup_files.jsonl | wc -l       # count files in the backup
```

`xbstream -x` produces it as a regular extracted file. `xbcloud get`
fetches it as a single bare-named object alongside
`backup_metadata.json` so cloud consumers can read both with raw
SDK calls.

## Future direction

Forthcoming releases extend this file with:

* **`sparse_map`** — per-file `[{offset, length}, ...]` recording
  hole regions, written before the transform pipeline so a
  consumer can reconstruct the sparse layout with `pwrite()`.
* **`segments`** — per-file `[{path: "...r1", size}, ...]` for
  files split at ds_cloud rollover, listed in concatenation order.
* **`sha256`** — opt-in per-file logical-file digest under
  `--sha256`, validated by a dedicated `--verify` mode.
* **Per-datasink stats sections** — uncompressed bytes counter,
  encryption key id, etc., as nested objects under the relevant
  transform key.

See PXB-3754 for the manifest framework design and PXB-3787 for the
overarching cloud-direct epic.
