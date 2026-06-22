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

## Choosing the transport

xtrabackup has three ways to ship a backup off the host. Pick by destination:

| Destination                            | Use                              |
|----------------------------------------|----------------------------------|
| S3 / Azure / GCS / Swift bucket        | `--cloud-storage=<backend>` (this doc) |
| Another host (over SSH / custom pipe)  | `--fifo-streams=N --fifo-dir=...` + downstream reader |
| Local filesystem / fast NAS / NVMe     | default `--target-dir` (or `--fifo-streams` for parallel local writers) |

`--cloud-storage` and `--fifo-streams` are mutually exclusive — both
replace the default datasink chain, so you can only pick one. The
legacy `--stream=xbstream | xbcloud put` pipeline still works but is
~2× slower than `--cloud-storage` (see Benchmark below); prefer
`--cloud-storage` for any new cloud-destination workflow.

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
| `--cloud-max-concurrent-requests` | Max concurrent in-flight HTTP requests (default 16) |
| `--cloud-upload-buffer-size`      | Total upload-memory cap (default 0 = unlimited; aws-cli-like) |
| `--cloud-multipart-part-size`     | Part size in bytes (0 = auto: `max(16 MiB, ceil(filesize/10K))`) |
| `--cloud-multipart-threshold`     | Single-PUT fast-path threshold (default 16 MiB) |
| `--cloud-multipart-rollover-threshold` | Advanced: per-object cap (5 TiB default; lower for parallel-download workflows) |
| `--cloud-rate-log-interval`       | Throughput log cadence (s, 0 = off)       |
| `--cloud-http-timing`             | Curl phase timing dump                    |

Two flags from earlier prototypes are intentionally NOT on this list:

- **`--cloud-multipart-upload=ON|OFF`** — removed. ds_cloud is multipart-only
  by design (the `Stream_multipart_writer` already short-circuits to a single
  PUT for files below `--cloud-multipart-threshold`, so there's no
  non-multipart code path to fall back to). xbcloud keeps the equivalent
  option because xbcloud has a legacy chunk-per-PUT path for backward
  compatibility; ds_cloud never did.

- **`--cloud-multipart-memory-budget`** — removed. The control was per-writer
  (one writer per file held by an xtrabackup data-copy thread), which means
  setting it to N silently multiplied to `N × --parallel` total peak memory.
  With the default 4 GiB and `--parallel=8`, that's 32 GiB → OOM. Hardcoded
  today to 64 MiB per writer; the eventual single global cap will be
  `--cloud-upload-buffer-size` (see Memory model below).

## Memory model

Cloud upload memory follows the **aws-cli model**:

```
memory_peak  =  effective_concurrent  ×  part_size
```

Both quantities are derived from the user's flags at `cloud_open` time
per file. The defaults trade some memory headroom for full throughput on
typical workloads; explicit knobs let memory-tight hosts dial back.

### Auto-sizing algorithm (one line per file in xtrabackup's log)

```
effective_size       = min(filesize, --cloud-multipart-rollover-threshold)
if --cloud-multipart-part-size != 0:
    part_size = --cloud-multipart-part-size          # user override
else:
    part_size = max(16 MiB, ceil(effective_size / 10000))   # auto, S3-10K-cap safe

if --cloud-upload-buffer-size != 0:
    effective_concurrent = min(--cloud-max-concurrent-requests,
                               max(1, --cloud-upload-buffer-size / part_size))
else:
    effective_concurrent = --cloud-max-concurrent-requests
```

For each file the backup log emits a diagnostic line so users can see
what was chosen:

```
ds_cloud: bench/big.ibd: filesize=42.00 MiB, part_size=16.00 MiB (auto), concurrent=16
ds_cloud: bench/tiny.ibd: filesize=112.00 KiB, single-PUT fast path
ds_cloud: xtrabackup_logfile: filesize=unknown (streaming), part_size=16.00 MiB (auto), concurrent=16
```

`(auto)` vs `(user)` shows whether the part_size came from the formula or
a user override. When `--cloud-upload-buffer-size` causes concurrency to
shrink, an additional `(shrunk by --cloud-upload-buffer-size)` suffix
appears.

### Memory by file size at defaults

`--cloud-max-concurrent-requests=16`, `--cloud-upload-buffer-size=0`
(unlimited), no overrides:

| File size | part_size | concurrent | memory peak |
|-----------|----------:|-----------:|------------:|
| < 16 MiB  | n/a (single-PUT fast path) | n/a | ~ filesize |
| 16 MiB – 100 GiB | 16 MiB (floor) | 16 | 256 MiB |
| 1 TiB     | 100 MiB | 16 | 1.6 GiB |
| 5 TiB     | 500 MiB | 16 | 8 GiB |

Memory grows linearly with file size for very large files (matches
aws-cli behavior; aws-cli's `max_concurrent_requests=10` with auto-bumped
chunks reaches similar memory peaks).

### Capping memory on small hosts

For memory-tight hosts (containers, small VMs), set
`--cloud-upload-buffer-size` to cap total in-flight bytes:

| `--cloud-upload-buffer-size` | 1 TiB file behavior | Memory | Throughput vs default |
|-----------------------------:|---------------------|-------:|----------------------:|
| 0 (unlimited, default)       | 100 MiB × 16 conc.  | 1.6 GiB | 100% |
| 1 GiB                        | 100 MiB × 10 conc.  | ~1 GiB  | 62% |
| 512 MiB                      | 100 MiB × 5 conc.   | 500 MiB | 31% |
| 256 MiB                      | 100 MiB × 2 conc.   | 200 MiB | 12% |

For extreme cases (5 TiB file in a 4 GiB container), the trade-off
between memory and throughput becomes painful. The escape hatch is to
lower `--cloud-multipart-rollover-threshold` so the file splits into
smaller cloud objects, each of which gets sized for the smaller effective
object size — bringing `part_size` back down to the 16 MiB floor and
restoring full concurrency:

```bash
# 5 TiB IBD, 4 GiB container: backup into 50-100 GiB chunks per object,
# keep concurrency at 16 with ~256 MiB peak memory.
xtrabackup --backup --cloud-storage=s3 ... \
    --cloud-upload-buffer-size=2g \
    --cloud-multipart-rollover-threshold=100g
```

The backup ends up as 50 objects in the bucket (5 TiB / 100 GiB) instead
of one; `--download` re-assembles them via the manifest's `segments[]`
field.

### `--cloud-max-concurrent-requests` is independent of `--parallel`

`--parallel` controls how many xtrabackup data-copy threads read source
IBDs in parallel — that's a separate resource from how many HTTP requests
fly to the cloud concurrently. They no longer inherit: bumping
`--parallel=64` does NOT bump `--cloud-max-concurrent-requests`. Set the
latter explicitly if you want more cloud-side parallelism than the
default 16.

## Benchmark: legacy pipeline vs direct ds_cloud

Real backup against a running mysqld, 9.3 GB schema (1 large ~10 MB seed,
5 empty IBDs, 5×112 MB, 4×560 MB, 4×1.6 GB tables — mixed sizes to
exercise both the small-file fast path and the multipart tiers).
LocalStack S3 target via `fault_proxy.py` for controlled RTT injection.
`--parallel=8`, `--cloud-max-concurrent-requests=8` (matching). 2 timed iterations
per path per RTT after a warm-up run; bucket reset between iterations so
LocalStack disk stays bounded. Lower is better.

| RTT inject | legacy (ms) | direct (ms) | direct − legacy | direct vs legacy |
|------------|------------:|------------:|----------------:|-----------------:|
|   0 ms     |       73495 |       38502 |          −34993 |       −47.6%     |
|  50 ms     |       76336 |       38321 |          −38015 |       −49.8%     |
| 200 ms     |       84102 |       43410 |          −40692 |       −48.4%     |

Direct beats legacy by ~48–50% across all RTTs with low per-iteration
variance (max−min < 3% on both paths). The legacy path is bounded by the
single xbstream pipe serializing all data-copy threads through one xbcloud
process; the direct path uploads each file in parallel from the data-copy
thread that owns it. The gap holds at 200 ms RTT because both paths use
the same multipart pipelining underneath; what direct saves is the
serialization overhead, not RTT-per-part.

Harness: `storage/innobase/xtrabackup/test/cloud/bench_legacy_vs_direct.sh`.

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

### Both xbcloud and ds_cloud use async small-file PUT

`Stream_multipart_writer::set_async_small_file_uploader()` is the
seam between the writer and the cloud transport. Both xbcloud and
ds_cloud install it; the small-file fast path goes through
`Event_handler` in both cases.

Empirical measurement (`prototype/perf_wan.sh` + real AWS backups)
showed sync small-file PUT bottlenecks at WAN RTT in BOTH single-
producer (xbcloud's xbstream-reader) and multi-worker
(xtrabackup's `--parallel=N` data-copy threads) cases:

- **Multi-worker doesn't save you.** Worker N still walks its assigned
  files in sequence and stalls ~RTT per small file. At
  `--parallel=4` to `--parallel=16` (common in practice, not just
  the `--parallel=256` extreme), small-file-heavy backups halve in
  throughput on the sync path. At lower parallelism the cost is
  proportionally worse.

- **ds_redo cannot block.** xtrabackup's Redo Log reader thread is a
  single producer feeding ds_cloud's redo writer. If `cloud_close` on
  the redo file's small final part (or any internal block) ever
  blocks, the reader stops consuming, and the backup either stalls
  or overflows redo space depending on workload.

So both callers go async. The `set_async_small_file_uploader()` seam
remains because in principle a future caller without an
`Event_handler` (e.g., a small CLI utility) could use the sync
fallback — but no current caller does. Failures from the async path
land in a `has_errors` atomic on the caller's ctxt; the
`Event_handler` drain at deinit ensures all async PUTs complete
before the process declares the backup done.

### Known async-failure-reporting gap

`ds_destroy` returns void in the datasink API, so when `cloud_deinit`
discovers a failed async PUT during its drain, it logs the failure
but has no clean channel to refuse the backup's commit step
(`xtrabackup_checkpoints`). Phase 3 cleanup: add a `ds_drain()` or
return-bearing `ds_destroy` variant so xtrabackup's backup_finish
can refuse to write the commit marker when uploads have failed.

Connection reuse (CURLSH sharing DNS / TLS / connection pool) is the
same for sync and async paths — both route through one `Http_client`
with the share handle installed. So the async choice is about whose
thread blocks, not about wire efficiency per call.

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
