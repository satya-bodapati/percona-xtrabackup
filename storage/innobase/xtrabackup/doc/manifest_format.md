# `backup_metadata.json` — per-backup manifest

Every backup that completes successfully writes a `backup_metadata.json`
file alongside the existing `xtrabackup_info`, `xtrabackup_binlog_info`,
`xtrabackup_checkpoints` and friends. The file is **plain UTF-8 JSON**:
unencrypted and uncompressed regardless of the backup pipeline's
`--encrypt` / `--compress` settings. Operators can read it directly with
`cat`, `jq`, `aws s3 cp`, etc., without invoking xtrabackup or xbcloud
to decrypt or decompress anything.

This file is in addition to — not a replacement for — the legacy
`xtrabackup_*` info files. Those keep being written the same way they
always have been, with the same transforms applied per CLI flags.

## Where it lands

| Output mode | Location |
|---|---|
| `--target-dir=/path/...` | `/path/.../backup_metadata.json` |
| `--stream=xbstream` | First / last frames inside the stream; `xbstream -x` extracts to the target dir as `backup_metadata.json` |
| `--stream=xbstream \| xbcloud put ...` | `<prefix>/backup_metadata.json` as a single bare-named cloud object |
| `--extra-lsndir=DIR` (any of the above) | `DIR/backup_metadata.json` (byte-identical to the target-dir copy) |

## Schema

```json
{
  "manifest_version": 1,

  "xtrabackup_info": {
    "uuid":              "...",
    "name":              "",
    "tool_name":         "xtrabackup",
    "tool_command":      "xtrabackup --backup ...",
    "tool_version":      "9.7.1",
    "ibbackup_version":  "9.7.1",
    "server_version":    "8.0.42",
    "server_flavor":     "",
    "start_time":        "...",
    "end_time":          "...",
    "lock_time":         0,
    "partial":           "N",
    "incremental":       "Y",
    "format":            "file",
    "compressed":        "Y",
    "encrypted":         "Y",
    "lock_ddl_type":     "ON",
    "backup_size":       12345678
  },

  "xtrabackup_binlog_info": {
    "filename":     "binlog.000123",
    "position":     4567,
    "gtid_executed": "..."
  },

  "xtrabackup_checkpoints": {
    "backup_type":          "incremental",
    "from_lsn":             5000,
    "to_lsn":               12000,
    "last_lsn":             12000,
    "compact":              0,
    "recover_binlog_info":  1,
    "flushed_lsn":          12000
  },

  "xtrabackup_galera_info":  null,
  "xtrabackup_replica_info": null,
  "xtrabackup_slave_info":   null
}
```

* `xtrabackup_info` and `xtrabackup_checkpoints` are parsed from the
  underlying `key = value` files into structured JSON objects.
* `xtrabackup_binlog_info` is parsed from its TSV format (filename, position, optional GTID).
* `xtrabackup_galera_info`, `xtrabackup_replica_info`, and
  `xtrabackup_slave_info` are embedded as raw text when those features
  applied to the backup, `null` otherwise. Structured parsing of these
  three is a candidate for a future schema version.

## `backup_size`

`backup_size` is sampled at backup completion from the leaf
datasink's `bytes_written` counter. It excludes the `xtrabackup_info`
and `backup_metadata.json` files themselves (a chicken-egg avoidance:
each file contains the value that would otherwise count them). The
same sampled value appears in **every** copy:

* `target-dir/xtrabackup_info`'s `backup_size = ...` line
* `--extra-lsndir/xtrabackup_info`'s `backup_size = ...` line
* `target-dir/backup_metadata.json`'s `xtrabackup_info.backup_size`
* `--extra-lsndir/backup_metadata.json`'s `xtrabackup_info.backup_size`

Before this fix, the target-dir and extra-lsndir copies of
`xtrabackup_info` carried different `backup_size` values because the
text was generated independently for each write and the leaf counter
had advanced between them. The text is now generated once and the
same bytes are written to both locations.

For exact total disk usage of a backup, use `du -sh` against the
backup directory (for local backups), `aws s3 ls --recursive` for
cloud, or sum the file sizes the operator's tooling exposes.
`backup_size` is informational and close-but-not-exact — within a
few KB of `du` because of the meta-file exclusion above.

## How the file is produced

xtrabackup caches the text of each legacy info file as it generates
it (via `xb_manifest::set_legacy_text`). At `backup_finish()` it
walks the cache, parses each block, and builds the JSON document
using rapidjson. The result is written through `ds_open_single_object`
so it lands plain across all output modes — see the section below.

## Always-plain semantics — `ds_open_single_object`

`ds_open_single_object()` walks the configured datasink chain past
any wrapper sinks (`ds_compress`, `ds_encrypt`, `ds_buffer`,
`ds_tmpfile`) to the first datasink that implements an
`open_single_object` op. Today:

* **`ds_local`**: writes the file plain to disk. No chunking, so a
  regular file is itself the single-object representation.
* **`ds_xbstream`**: tags every emitted chunk header with
  `XB_STREAM_FLAG_SINGLE_OBJECT` so downstream consumers know to
  accumulate.
* **`ds_fifo`**: same as `ds_xbstream` (it carries xbstream-framed
  bytes to the FIFO).

Wrapper sinks (compress/encrypt) leave `open_single_object` `nullptr`
and the walker skips them. So a file opened via `ds_open_single_object`
takes the bytes straight from the producer to the terminal sink
without ever being compressed or encrypted, even if the rest of the
backup is.

## xbstream — `XB_STREAM_FLAG_SINGLE_OBJECT`

`XB_STREAM_FLAG_SINGLE_OBJECT` (`0x02`) is a flag bit in the xbstream
chunk header alongside the existing `XB_STREAM_FLAG_IGNORABLE` (`0x01`).
Producers set it on every chunk emitted from `ds_open_single_object`.
Consumers interpret it as:

* **`xbstream -x`**: informational. The flag does not change the
  extraction logic — that has always been driven by the file path's
  transform extension (`.qp.xbcrypt`, `.lz4`, etc.). A
  single-object path has no such extension and naturally extracts
  to a plain file on disk.
* **`xbcloud put`**: accumulate all flagged chunks for a given path
  into one cloud object named exactly the path (no chunk-index
  `.NNNN` suffix). See below.
* **`xbcloud get`**: detect bare-named cloud objects, download them
  in one piece, and re-emit as `XB_STREAM_FLAG_SINGLE_OBJECT` chunks
  into the output xbstream.

Old xbstream readers that don't recognise the flag treat the chunk
as a regular `PAYLOAD` chunk and write the bytes plain to disk (the
path lacks transform suffixes, so it falls through the existing
dispatcher cleanly). Backward-compatible by construction.

## xbcloud — `single-object` storage convention

xbcloud's default is one cloud object per xbstream chunk, named
`<backup_name>/<path>.NNNNNNNNNNNNNNNNNNNN` (20-digit
zero-padded chunk index). For chunks that carry
`XB_STREAM_FLAG_SINGLE_OBJECT`, xbcloud instead accumulates the
payload bytes locally and uploads one PUT to
`<backup_name>/<path>` (no suffix). On `xbcloud get`, listing
the bucket distinguishes:

* `<path>.NNNN` (numeric suffix) → chunked file, downloaded
  through the existing parallel chunked path.
* `<path>` (bare name) → single-object file, downloaded
  synchronously and re-emitted as xbstream `PAYLOAD_PLAIN`
  frames before the parallel chunked downloads begin.

Operators can therefore directly read the manifest from any
bucket with standard cloud SDK tooling:

```
aws s3 cp s3://mybucket/myprefix/backup_metadata.json -
```

without needing to invoke xbcloud get.

## Cross-version compatibility

`manifest_version` is reserved for **breaking** schema changes
(renamed fields, removed fields, changed semantics). Adding new
optional fields does not bump the version — readers tolerate
unknown top-level fields, ignore unknown flag bits, etc.

For consumers that want to distinguish "backup taken by 9.7.x" from
"backup taken by 9.8+" (which will add `backup_files.jsonl` and per-
file metadata via the FileContext work — see PXB-3754), check
`xtrabackup_info.tool_version` (semver) or the presence of
`backup_files.jsonl` alongside `backup_metadata.json`. The schema
itself is forward-additive.

## Future direction

This file is the entry point of a larger manifest framework planned
for the next release alongside ds_cloud direct streaming:

* **`backup_files.jsonl`**: a streaming NDJSON file with one entry
  per backup file. Lines carry path, space_id+page_size (InnoDB
  data files), per-datasink section (transform stats), sparse
  regions, segments for ds_cloud rollover, and optional sha256.
* **FileContext on `ds_file_t`**: a per-file rapidjson document
  travels with the file through the datasink pipeline. Each
  transformer enriches it with its own section on close
  (compress_zstd, encrypt_aes256_cbc, ds_cloud, ...) and the
  top-level ds_close serializes it into `backup_files.jsonl`.
* **`--sha256`**: opt-in per-file checksums computed by a
  stats_tail datasink stage on the final transformed bytes.
* **`--verify`**: parses the manifest pair and validates per-file
  presence, integrity, and (with sha256 on) checksums.
* **ds_cloud direct streaming**: takes over the cloud-output path
  with native multipart upload and segment support.

See PXB-3754 for the full design doc and PXB-3787 for the
overarching cloud-direct epic.

### Future: large files split into segments (`.rN`)

When a logical file exceeds a configured rollover threshold (the
ds_cloud per-object cap, e.g. 5 TiB on S3, or an operator-chosen
size for any backend) the producer will split it at write time
and emit **each segment as its own xbstream file**:

```
test/big.ibd (6 TB, threshold 5 TB) becomes
  test/big.ibd.r1   first 5 TB     -- complete xbstream file: PAYLOAD ... EOF
  test/big.ibd.r2   last 1 TB      -- complete xbstream file: PAYLOAD ... EOF
```

The xbstream protocol sees N independent, fully-formed files.
No new chunk type, no PARTIAL_EOF, no inter-segment dependency on
the wire. The wire format does not change.

The "they are one logical file" rollup lives in
`backup_files.jsonl`:

```json
{
  "path": "test/big.ibd",
  "space_id": 42,
  "page_size": 16384,
  "segments": [
    { "name": "test/big.ibd.r1", "start": 0,             "length": 5497558138880 },
    { "name": "test/big.ibd.r2", "start": 5497558138880, "length": 1099511627776 }
  ]
}
```

Restore reads the manifest, downloads each segment in whatever
order is convenient, and `pwrite()`s the segment bytes at `start`
into the destination file.

Parallelism follows the segment boundary, not the chunk boundary:

| Stage | What runs in parallel |
|-------|-----------------------|
| Producer | N writer tasks, one per segment, each emitting its own xbstream file |
| `xbcloud put` | N parallel PUTs, one cloud object per segment |
| `xbcloud get` | N parallel GETs |
| `xbstream -x` | Per-segment write to `.rN`; helper reassembles via the manifest's `segments` list |

Within a single segment the writer is still sequential -- PAYLOAD
chunks in offset order, EOF last. EOF is one per segment, exactly
where it has always been.

`PARTIAL_EOF` was sketched in early design conversations as a
mechanism for many producers contributing to one logical file. It
is NOT needed for the rollover case above -- segments-as-files
covers it without any wire-format change. PARTIAL_EOF stays
parked for a hypothetical future use case where multiple
independent processes feed parts of ONE xbstream file with no
shared producer-side coordination, which we don't have today.
