# Direct cloud upload in xtrabackup -- design overview

A concise design summary of PXB-3671 (direct cloud streaming) and the
manifest framework from PXB-3754 it depends on. For the full user-
facing reference, see [`cloud_direct.md`](cloud_direct.md).

## 1. xbcloud lifecycle — keep for a few releases, then remove

**Proposal**: keep xbcloud for the first few 9.7 releases, then remove
it entirely. We don't want two ways to do the same job.

### Why keep it for a window

Existing customer scripts often use `xtrabackup --stream=xbstream |
xbcloud put`. Removing xbcloud immediately would break those scripts
on upgrade. A migration window (one or two release cycles) lets
operators run side-by-side and switch when they're ready. The
migration is mechanical — see §4 for the literal command
transformations.

### Why remove it eventually

Maintaining two paths doubles the test matrix, doubles the docs
surface, and forces every new feature into an "implement in both or
one is incomplete" bind. Long-term we want one transport for cloud
backups.

## 2. Why fold xbcloud into xtrabackup

Today users back up to the cloud via a two-process pipe:

```
xtrabackup --stream=xbstream | xbcloud put ...
```

Direct integration means xtrabackup itself opens cloud connections and
uploads each file as a discrete object, in parallel from the same
data-copy thread that read it. Concrete benefits:

- **Simpler to use.** Only xtrabackup is needed for cloud backups —
  no `xbcloud` + `xbstream` chain to glue together. (xbstream is
  still required at restore time to extract a downloaded backup;
  the cloud *upload* path itself becomes single-binary.)

- **One file per object.** Each `.ibd` / `.ibd.zst` /
  `.ibd.zst.xbcrypt` in the bucket is the real file's bytes;
  `aws s3 cp` returns it ready to use. No `xbcloud get | xbstream -x`
  unwrap chain to invert the wire format on the way back.

- **No wasted serialization / deserialization, no unnecessary
  piping.** The old chain serialized every chunk into the xbstream
  wire format in xtrabackup (`CHUNK_TYPE_PAYLOAD` header + CRC32 per
  chunk), pushed it through a pipe, then deserialized it in
  `xbcloud put` to reconstruct one object per file. The restore side
  (`xbcloud get | xbstream -x`) reversed both halves. Direct upload
  skips that wire format entirely — the bytes that hit the cloud are
  the file's bytes (post-transform).

  `--fifo-streams=N` (parallel-pipe pumping with N TCP streams) does
  NOT solve this. It parallelizes the wire — multiple TCP
  connections, multiple xbcloud workers — but each byte still pays
  the serialize cost on the way out and the deserialize cost on the
  way back. For 1 TB backups the CPU saved on the wrap/unwrap layer
  is non-trivial.

- **Easy direct single-file restore.** A downloaded `.ibd` is the
  actual InnoDB tablespace; you can hand it to any compatible mysqld
  without xbstream deserialization. Single-table / single-tablespace
  recoveries become a single `aws s3 cp` of the one file you need.

## 3. Option migration: `--cloud-<provider>-<opt>`

Existing xbcloud option names move into xtrabackup with a `--cloud-`
prefix and stay provider-explicit, so scripts can be ported by a
mechanical rename:

```
--s3-bucket=B            -> --cloud-s3-bucket=B
--azure-container-name=C -> --cloud-azure-container-name=C
--google-bucket=B        -> --cloud-google-bucket=B
--swift-container=C      -> --cloud-swift-container=C
```

Common knobs (retries, timeouts, TLS, headers, parallelism) drop the
provider prefix because they aren't backend-specific:
`--cloud-max-retries`, `--cloud-header`, `--cloud-verbose`, etc.
Full mapping table in [`cloud_direct.md#xbcloud--xtrabackup-option-migration`](cloud_direct.md#xbcloud--xtrabackup-option-migration).

Provider-explicit names (rather than one generic `--cloud-bucket`)
match the vocabulary each cloud's own CLI uses (`aws s3` says
"bucket", `az storage` says "container", `gsutil` says "bucket") --
users carry their existing mental model over.

## 4. Old way vs new way

### Backup (PUT)

```
# OLD
xtrabackup --backup --stream=xbstream --target-dir=. | \
  xbcloud put backup-name --storage=s3 --s3-bucket=my-bucket ...

# NEW
xtrabackup --backup --target-dir=/tmp/local-only \
  --cloud-storage=s3 \
  --cloud-s3-bucket=my-bucket/backup-name \
  --cloud-region=us-east-2 ...
```

`--target-dir` is now local-semantic only (no files written for cloud
mode); the bucket prefix is part of `--cloud-s3-bucket` as
`BUCKET/PREFIX`.

### Restore (GET)

```
# OLD
xbcloud get backup-name --storage=s3 --s3-bucket=my-bucket ... | \
  xbstream -xv -C /restore/path

# NEW
xtrabackup --download --target-dir=/restore/path \
  --cloud-storage=s3 \
  --cloud-s3-bucket=my-bucket/backup-name ...
```

`--download` lists the bucket, fetches every object into the matching
local path, and applies the manifest's `sparse_map` so sparse files
restore sparse. The output directory is identical to a local backup,
so `--prepare` and `--copy-back` work unchanged.

### Delete

```
# OLD
xbcloud delete backup-name --storage=s3 --s3-bucket=my-bucket ...

# NEW
xtrabackup --delete --target-dir=any \
  --cloud-storage=s3 \
  --cloud-s3-bucket=my-bucket/backup-name ...
```

Lists the prefix, deletes each object, confirms interactively (force-
delete flag is a follow-up). HNS-enabled Azure containers go through
xbcloud's partitioned listing API (PR #1726) so directory placeholders
are removed bottom-up.

## 5. `backup_meta.json` manifest

A per-backup JSON document (one line per file, NDJSON-style) written
into the bucket alongside the data objects. Schema per file:

```json
{
  "name":         "test/t1.ibd",
  "logical_size": 1610612736,
  "regions":      [{"offset": 0, "len": 16384}, {"offset": 1048576, "len": 16384}, ...]
}
```

`regions` is the list of DATA extents (everything else is a hole); the
backup-side reader records these whenever it skips a sparse range. On
restore, `--download` reads the manifest first, then for each file:

1. Downloads the dense bytes into a staging path.
2. Punches the manifest's listed holes via `fallocate(PUNCH_HOLE)`.
3. Renames into place.

This is what makes the design work for:

- **Sparse `.ibd` files** -- compressed-page sparse-region info is no
  longer recoverable by walking pages once a file has been
  decompressed/decrypted; the manifest carries it independently.
- **Files >5 TiB** -- a later iteration will record per-object segment
  lists in the manifest so a single file can span multiple cloud
  objects and be reassembled on download.
- **HNS safety** -- the manifest declares what objects exist; the
  restore side reads it instead of inferring from `LIST`.

The manifest is always written when `--cloud-storage` is set; users
cannot opt out (sparse restores would silently break).

---

## 6. Advanced — feature interactions, tuning, removed options, deferred work

*This section is not required for general design understanding. It
covers operational tuning, feature interactions (reduced-lock),
options that were intentionally dropped, and work deferred to
follow-up tickets.*

### 6.1 Reduced-lock and incremental file types — cloud-side handling

The reduced-lock backup feature creates several file types in the
backup directory beyond the usual `.ibd`. Their cloud-upload
treatment:

| File | Size | ds_cloud path |
|------|------|---------------|
| `<name>.ibd` (regular full) | usually large | multipart upload (or small-file PUT if below threshold) |
| `<name>.delta` (incremental) | usually large | multipart upload |
| `<name>.new` (reduced-lock recopy) | usually large | multipart upload (same as `.ibd`) |
| `<name>.new.delta` (incremental + recopy) | usually large | multipart upload |
| `<name>.meta` / `<name>.new.meta` (sidecar) | a few hundred bytes | small-file single PUT |
| `<schema>/<space_id>.ren` | a few bytes | small-file single PUT |
| `<space_id>.del` | **zero bytes** | small-file single PUT (with sentinel — see below) |
| `<schema>/<table>.ibd.crpt` | **zero bytes** | small-file single PUT (with sentinel — see below) |

Most of these are transparent to the cloud upload layer — large files
take the multipart path naturally, small non-empty files take the
single-PUT fast path, both already handled by the main multipart
design (see §6.2 below).

#### Zero-byte markers and cloud-store quirks

`.del` and `.crpt` are written as zero-byte files (the marker is
extension-driven; content is intentionally empty). Most cloud object
stores either refuse zero-byte PUTs outright or behave inconsistently:

- **S3 (AWS)**: accepts zero-byte PUTs technically, but some lifecycle
  rules / KMS configurations reject them; multipart upload with zero
  parts fails outright (`InvalidRequest` on `CompleteMultipartUpload`).
- **Azure Blob**: accepts but some auditing tools flag them as
  "anomalous."
- **GCS (S3-compat mode)**: similar to S3.
- **Swift**: accepts.

ds_cloud bypasses the multipart path entirely for files below
`--cloud-multipart-threshold` (default 16 MiB) and emits a single PUT —
but a single PUT of zero bytes is still the problem case.

**Fix**: when ds_cloud detects a zero-byte upload that originated as a
reduced-lock marker (`.del` or `.crpt` suffix), prepend a small
sentinel byte sequence so the object body is non-empty on the wire.
Restore-side recognises the sentinel and strips it before checking the
marker's semantics (the marker semantics are extension-driven, not
content-driven, so stripping the sentinel doesn't lose information).

Sentinel: a fixed 4-byte magic (e.g. `"XBMK"`) at offset 0. ds_cloud's
small-file path writes the magic before any user bytes; on download,
the restore-side checks for it on these specific extensions and
strips the 4 bytes. Files that were already non-zero (.new, .ren) pass
through unmodified; the sentinel only kicks in for the zero-byte
extensions where we explicitly know the original content was empty.

This is a small wire-format add, contained to the small-file PUT path
in ds_cloud + a matching strip on download. The parsing layer in
`prepare_handle_del_files` / `prepare_handle_corrupt_files` continues
to ignore content (matching today's behaviour).

### 6.2 Multipart-only upload + memory-aware options (ds_cloud only)

In the **ds_cloud path** (`xtrabackup --cloud-storage`), every file
goes through a shared `Stream_multipart_writer`:

- **Files below `--cloud-multipart-threshold` (16 MiB default)** go in
  a single PUT (small-file fast path).
- **Files above** are split into parts and uploaded in parallel via
  the backend's multipart API (S3 `UploadPart`, Azure `Put Block`,
  Swift SLO segment).

**xbcloud is unchanged**. It continues to use its existing
chunk-per-PUT upload model from before PXB-3671. The
`xbcloud_internal` library is where the new multipart machinery
lives, but it is only invoked by ds_cloud (the xtrabackup-linked
side). xbcloud (the standalone binary) does not call it. This is
deliberate — see §1 on the xbcloud lifecycle: feature set is frozen
during the migration window, new functionality only lands in the
direct-cloud path.

Three tuning knobs follow the **aws-cli memory model**:

| Knob | Default | Effect |
|------|---------|--------|
| `--cloud-max-concurrent-requests` | 16 | Max in-flight HTTP requests across all files; decoupled from `--parallel`. |
| `--cloud-multipart-part-size`     | 0 (auto)| Part size; auto-formula is `max(16 MiB, ceil(min(filesize, rollover)/10000))`. |
| `--cloud-upload-buffer-size`      | 0 (unlimited) | Optional total in-flight cap; concurrency shrinks to fit. |
| `--cloud-multipart-rollover-threshold` | 5 TiB | Per-object cap; files larger than this split into multiple cloud objects. |

Memory peak at defaults is `concurrent × part_size ≈ 256 MiB` for
files up to 100 GiB. Larger files use larger parts (the S3 10,000-part
cap forces this); the rollover knob lets users keep memory bounded by
splitting the file across objects instead.

The full set of `--cloud-*` HTTP knobs is in
[`cloud_direct.md#cli-option-catalog`](cloud_direct.md#cli-option-catalog).

### 6.3 Options that fall away (no longer needed for cloud destinations)

Some options exist today only to compensate for the xbstream-pipe
bottleneck or for xbcloud-specific design choices. They are not
needed in the direct-cloud world:

| Option | Why it falls away |
|--------|-------------------|
| `--stream=xbstream` (for cloud destinations) | The pipe itself is gone -- xtrabackup writes directly to the bucket. Mutually exclusive with `--cloud-storage`. Still useful for non-cloud streaming (custom pipelines, host-to-host transfer). |
| `--fifo-streams=N` (for cloud destinations) | Was the workaround for the single-pipe bottleneck: N named pipes, N `xbcloud put` processes, N×serial uploads. ds_cloud has native parallelism via `--cloud-max-concurrent-requests` (default 16) so the workaround is unnecessary. `--fifo-streams` keeps working for non-cloud destinations. |
| xbcloud's `--md5` | Per-chunk MD5 sidecar; tied to the xbstream-chunked filename format. Per-FILE SHA-256 in `backup_meta.json` replaces it (see §6.4). |

### 6.4 TODO: per-file SHA-256 checksums

Deferred to task #54. Current state: `--md5` (xbcloud's per-chunk MD5
sidecar) is dropped; nothing replaces it yet.

Design sketch:

- Tail-position `ds_manifest` pass-through datasink computes streaming
  SHA-256 over the bytes flowing to the leaf (ds_cloud / ds_local /
  ds_xbstream).
- Stamps the hex digest into `backup_meta.json` per file at close.
- `--download` verifies on the way in (always on, no flag).
- A separate `--verify --target-dir=...` mode re-checks files on
  disk **before** `--prepare` runs; verifying after `--prepare` or
  `--copy-back` is rejected (those modes rewrite the bytes, so the
  on-disk hash diverges from the manifest by design -- this is the
  PR #1726 / PXB-3643 lesson applied).

Two open design questions before coding:

1. **Sparse-frame hashing semantics**: when ds_xbstream emits sparse
   frames, does the hash cover dense bytes (with zero-fill for holes)
   or packed bytes (data regions only)? Reader currently sends packed;
   verifier on extract reconstructs sparse. Needs explicit resolution
   so the two sides agree.
2. **Pre- vs post-transform**: hashing AFTER compress+encrypt (what
   sits on disk / in the cloud, what the user verifies before reverting
   the transformations) vs hashing the source bytes. The on-disk
   semantic is the more useful one but the design needs a clean write-
   up before implementation.

Scope is not S3-specific. Same hashing covers `ds_local`, `ds_xbstream`,
and `ds_cloud` uniformly, by virtue of `ds_manifest` sitting between
the transforms and the leaf in all three pipelines.
