# xtrabackup direct cloud streaming

Phase 2 of the cloud redesign (PXB-3671 / PXB-3787) adds three new
modes to `xtrabackup`:

- `--backup` with `--cloud-storage=...` -- write each backup file as a
  separate object in the cloud bucket, bypassing the
  `xtrabackup --stream | xbcloud put` pipeline.
- `--download` -- fetch a previously-uploaded backup back to a local
  `--target-dir`. The resulting tree is identical to a local backup so
  `--prepare` and `--copy-back` work unchanged.
- `--delete` -- remove a backup from the cloud.

`xbcloud` is unchanged and remains available for testing and
side-by-side comparison.

## Backup

```bash
xtrabackup --backup \
  --user=root --socket=/var/lib/mysql/mysql.sock \
  --target-dir=2026-06-21-full \
  --cloud-storage=s3 \
  --cloud-bucket=my-backups \
  --cloud-region=us-east-2 \
  --cloud-endpoint=s3.us-east-2.amazonaws.com \
  --cloud-access-key=AKIA... \
  --cloud-secret-key=...
```

The basename of `--target-dir` becomes the prefix inside the bucket.
Backup files land at `my-backups/2026-06-21-full/<file>` -- one object
per file, identical bytes to a local backup. `aws s3 cp` returns the
original file directly with no unwrap.

Before any data-copy thread starts, xtrabackup runs a `ds_cloud_probe()`
against the bucket. If credentials are bad or the bucket is unreachable,
the backup aborts with an actionable error (no half-uploaded backups,
no broken-pipe cascade).

## Download

```bash
xtrabackup --download \
  --target-dir=/restore/2026-06-21-full \
  --cloud-storage=s3 \
  --cloud-bucket=my-backups \
  --cloud-region=us-east-2 \
  --cloud-endpoint=s3.us-east-2.amazonaws.com \
  --cloud-access-key=AKIA... --cloud-secret-key=...
```

Lists every object under `my-backups/2026-06-21-full/` and writes each
to the matching relative path inside `--target-dir`. Then run
`xtrabackup --prepare --target-dir=/restore/2026-06-21-full` as you
would for any local backup.

## Delete

```bash
xtrabackup --delete \
  --target-dir=2026-06-21-full \
  --cloud-storage=s3 \
  --cloud-bucket=my-backups \
  --cloud-region=us-east-2 \
  ...
```

Interactive confirmation by default. (A `--force` flag will follow.)

## CLI option catalog

| Option                            | Purpose                                   |
|-----------------------------------|-------------------------------------------|
| `--cloud-storage`                 | `s3` / `gcs` / `azure` / `swift`          |
| `--cloud-url`                     | Cloud target URL (Swift / custom backends)|
| `--cloud-bucket`                  | Bucket name                               |
| `--cloud-region`                  | Region                                    |
| `--cloud-endpoint`                | Endpoint host                             |
| `--cloud-access-key`              | Access key                                |
| `--cloud-secret-key`              | Secret key                                |
| `--cloud-session-token`           | AWS STS session token                     |
| `--cloud-bucket-lookup`           | `auto` / `path` / `dns`                   |
| `--cloud-storage-class`           | Storage tier                              |
| `--cloud-azure-account`           | Azure storage account                     |
| `--cloud-azure-access-key`        | Azure account key                         |
| `--cloud-azure-endpoint`          | Azure endpoint                            |
| `--cloud-insecure`                | Skip TLS verification                     |
| `--cloud-cacert`                  | CA bundle path                            |
| `--cloud-timeout`                 | Per-request timeout (s)                   |
| `--cloud-max-retries`             | Retry budget                              |
| `--cloud-max-backoff`             | Max retry backoff (ms)                    |
| `--cloud-parallel`                | Concurrent in-flight requests             |
| `--cloud-multipart-upload`        | Enable multipart                          |
| `--cloud-multipart-part-size`     | Fixed part size (0 = dynamic schedule)    |
| `--cloud-multipart-memory-budget` | Per-file in-flight buffer cap             |
| `--cloud-multipart-threshold`     | Single-PUT fast-path threshold            |
| `--cloud-multipart-rollover-threshold` | Per-object cap (5 TiB default)       |
| `--cloud-rate-log-interval`       | Throughput log cadence (s, 0 = off)       |
| `--cloud-http-timing`             | Curl phase timing dump                    |

## Architecture

The same multipart upload machinery powers both xbcloud and xtrabackup's
ds_cloud, courtesy of the `xbcloud_internal` CMake OBJECT library.
`Multipart_uploader` is one-per-file; `Event_handler` is one-per-
ds_cloud-ctxt (i.e. one libev thread per xtrabackup data-copy chain);
`Http_client` carries the per-process curl handles and `CURLSH` share.

`Stream_multipart_writer` (defined in `xbcloud/multipart.h`) is the
single class that drives the streaming-multipart protocol — buffer
bytes, flush parts at `dynamic_part_size()` boundaries, take the small-
file fast path at close. Both xbcloud's `put_func` and ds_cloud's
`cloud_write`/`cloud_close` use it; there is no duplicated multipart
state machine.

### Why xbcloud uses async small-file PUT but ds_cloud uses sync

`Stream_multipart_writer::set_async_small_file_uploader()` lets the
small-file fast path go async; without it, the writer uses sync
`upload_object`. The two callers choose differently on purpose:

- **xbcloud's `put_func` is single-producer**. One thread reads
  xbstream frames in a tight loop. Blocking on each small file's PUT
  would stall the pipe, so xbcloud installs the async uploader that
  fires-and-forgets through `Event_handler`. Errors bubble back via a
  `has_errors` atomic in the caller's callback, and `h.stop(); ev.join()`
  drains everything before exit.

- **ds_cloud is multi-worker**. xtrabackup spins up `--parallel=N`
  data-copy threads, each iterating files independently. Worker N
  blocking in `cloud_close` on a sync upload does NOT prevent the
  other N−1 workers from progressing — parallelism comes from having
  multiple workers, not from per-worker async. So sync is fine, and
  it brings three concrete simplifications: `cloud_close` returns the
  true upload result directly (no separate atomic / drain machinery
  on the ctxt), no risk of `xtrabackup_checkpoints` racing ahead of
  a still-in-flight data file (the commit-marker hazard), and one
  fewer state machine in ds_cloud's per-file lifecycle.

Connection reuse (CURLSH sharing DNS / TLS / connection pool) is the
same for sync and async paths — both route through one `Http_client`
with the share handle installed. So this choice is purely about whose
thread blocks, not about wire efficiency.

```
xtrabackup --backup
   |
   v
  ds_cloud (per-file Multipart_uploader)
   |       \-- shared Event_handler / libev thread
   v
  S3/GCS/Azure/Swift bucket
```

```
xtrabackup --download
   |
   v
  ds_cloud_lifecycle::xb_cloud_download()
   |   -- list bucket prefix
   |   -- per-object: download_object + write to target-dir
   v
  local --target-dir (identical layout to local backup)
   |
   v
  xtrabackup --prepare --target-dir=...
   |
   v
  xtrabackup --copy-back / start mysqld
```

## Limitations and follow-ups

This is the Phase 2 MVP. Known gaps:

- **Sparse files**: ds_cloud's `cloud_write_sparse` currently falls
  back to a dense write (the bytes go up; the sparse map is not
  carried). Sparse fidelity needs the unified `backup_meta.json`
  manifest from PXB-3754 to carry per-file `sparse_map`; that work is
  the next ticket. As a workaround, sparse files restore as dense
  files (full size, no holes), which is functionally correct.

- **Streaming rollover** (single file >5 TiB on the wire): aborts with
  a clear error pointing at `--cloud-multipart-from-file`. Streaming
  rollover via the unified manifest is queued behind PXB-3754.

- **No --force on --delete yet**: confirmation is always interactive.

- **No parallel range GETs in --download**: each object is fetched
  sequentially as a whole. Range-GET split for large files comes with
  the per-object segment list in the manifest.

- **No ListMultipartUploads cleanup in --delete**: orphaned multipart
  sessions from crashed backups aren't garbage-collected by --delete.

## Testing

The harness lives at `storage/innobase/xtrabackup/test/cloud/`. See
its README. Quick check (no mysqld needed):

```bash
bash test/cloud/upload_download_round_trip.sh
```

Full round-trip with a real mysqld:

```bash
MYSQL_USER=root MYSQL_SOCKET=/var/lib/mysql/mysql.sock \
  bash test/cloud/backup_s3.sh
```
