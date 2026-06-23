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

## Bucket and prefix syntax

Each backend has its own bucket / container option, taking either
`BUCKET` or `BUCKET/PREFIX` form:

| Backend | Option                          | Example value                       |
|---------|---------------------------------|-------------------------------------|
| S3      | `--cloud-s3-bucket`             | `my-backups/2026-06-21-full`        |
| GCS     | `--cloud-google-bucket`         | `my-backups/2026-06-21-full`        |
| Azure   | `--cloud-azure-container-name`  | `my-container/2026-06-21-full`      |
| Swift   | `--cloud-swift-container`       | `my-container/2026-06-21-full`      |

The portion after the first `/` is the **prefix** -- the sub-directory
where THIS backup's objects live. With the example above, S3 keys land
at `my-backups/2026-06-21-full/<file>`. The prefix may contain multiple
`/` characters for deeper namespacing
(`my-backups/year/2026/06/21-full`).

**HNS safety.** The parser strips leading and trailing `/` from the
prefix, and ds_cloud never PUTs an object whose key ends in `/`. This
matters on Azure Data Lake Storage Gen2 (Hierarchical Namespace
enabled), where a key ending in `/` is a directory placeholder, not a
blob; an upload that PUT such a key would lose the object data.

**`--target-dir` is local-only** in cloud modes. It is required by the
xtrabackup option parser (every mode uses it) but its value does not
affect cloud object keys. The prefix is exclusively driven by
`BUCKET/PREFIX` above.

## Backup

```bash
xtrabackup --backup \
  --user=root --socket=/var/lib/mysql/mysql.sock \
  --target-dir=2026-06-21-full \
  --cloud-storage=s3 \
  --cloud-s3-bucket=my-backups/2026-06-21-full \
  --cloud-region=us-east-2 \
  --cloud-endpoint=s3.us-east-2.amazonaws.com \
  --cloud-access-key=AKIA... \
  --cloud-secret-key=...
```

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
  --cloud-s3-bucket=my-backups/2026-06-21-full \
  --cloud-region=us-east-2 \
  --cloud-endpoint=s3.us-east-2.amazonaws.com \
  --cloud-access-key=AKIA... --cloud-secret-key=...
```

Lists every object under the configured prefix and writes each to the
matching relative path inside `--target-dir`. Then run
`xtrabackup --prepare --target-dir=/restore/2026-06-21-full` as you
would for any local backup.

## Delete

```bash
xtrabackup --delete \
  --target-dir=any-local-path \
  --cloud-storage=s3 \
  --cloud-s3-bucket=my-backups/2026-06-21-full \
  --cloud-region=us-east-2 \
  ...
```

Interactive confirmation by default. (A `--force` flag will follow.)

## CLI option catalog

### Common (all backends)

| Option                            | Purpose                                   |
|-----------------------------------|-------------------------------------------|
| `--cloud-storage`                 | `s3` / `gcs` / `azure` / `swift`          |
| `--cloud-url`                     | Cloud target URL (Swift / custom backends)|
| `--cloud-region`                  | Region (S3 / GCS)                         |
| `--cloud-endpoint`                | Endpoint host (S3 / GCS)                  |
| `--cloud-access-key`              | Access key (S3 / GCS)                     |
| `--cloud-secret-key`              | Secret key (S3 / GCS)                     |
| `--cloud-session-token`           | AWS STS session token                     |
| `--cloud-bucket-lookup`           | `auto` / `path` / `dns`                   |
| `--cloud-storage-class`           | Storage tier (S3 / GCS / Azure)           |
| `--cloud-insecure`                | Skip TLS verification                     |
| `--cloud-cacert`                  | CA bundle path                            |
| `--cloud-verbose`                 | `CURLOPT_VERBOSE`: stream libcurl trace   |
| `--cloud-timeout`                 | Per-request timeout (s)                   |
| `--cloud-max-retries`             | Retry budget                              |
| `--cloud-max-backoff`             | Max retry backoff (ms)                    |
| `--cloud-curl-retriable-errors`   | Extra curl error codes to retry (CSV)     |
| `--cloud-http-retriable-errors`   | Extra HTTP status codes to retry (CSV)    |
| `--cloud-header`                  | Extra `Name: Value` HTTP header (repeatable) |
| `--cloud-max-concurrent-requests` | Max concurrent in-flight HTTP requests (default 16) |
| `--cloud-upload-buffer-size`      | Total upload-memory cap (default 0 = unlimited; aws-cli-like) |
| `--cloud-multipart-part-size`     | Part size in bytes (0 = auto: `max(16 MiB, ceil(filesize/10K))`) |
| `--cloud-multipart-threshold`     | Single-PUT fast-path threshold (default 16 MiB) |
| `--cloud-multipart-rollover-threshold` | Advanced: per-object cap (5 TiB default; lower for parallel-download workflows) |
| `--cloud-rate-log-interval`       | Throughput log cadence (s, 0 = off)       |
| `--cloud-http-timing`             | Curl phase timing dump                    |

### S3

| Option                            | Purpose                                   |
|-----------------------------------|-------------------------------------------|
| `--cloud-s3-bucket`               | Bucket (`BUCKET` or `BUCKET/PREFIX`)      |
| `--cloud-s3-api-version`          | `AUTO` (default) / `2` / `4` -- signing version |

### Google Cloud Storage

| Option                            | Purpose                                   |
|-----------------------------------|-------------------------------------------|
| `--cloud-google-bucket`           | Bucket (`BUCKET` or `BUCKET/PREFIX`)      |

### Azure Blob Storage

| Option                            | Purpose                                   |
|-----------------------------------|-------------------------------------------|
| `--cloud-azure-container-name`    | Container (`CONTAINER` or `CONTAINER/PREFIX`) |
| `--cloud-azure-account`           | Storage account                           |
| `--cloud-azure-access-key`        | Account key                               |
| `--cloud-azure-endpoint`          | Endpoint                                  |
| `--cloud-azure-development-storage` | Use Azurite emulator defaults           |

### Swift (OpenStack)

| Option                            | Purpose                                   |
|-----------------------------------|-------------------------------------------|
| `--cloud-swift-container`         | Container (`CONTAINER` or `CONTAINER/PREFIX`) |
| `--cloud-swift-auth-url`          | Base URL of Keystone / TempAuth           |
| `--cloud-swift-auth-version`      | `1` / `2` / `3` (default 1 / TempAuth)    |
| `--cloud-swift-user`              | User name                                 |
| `--cloud-swift-user-id`           | User ID                                   |
| `--cloud-swift-key`               | TempAuth key                              |
| `--cloud-swift-password`          | User password                             |
| `--cloud-swift-tenant`            | Tenant name                               |
| `--cloud-swift-tenant-id`         | Tenant ID                                 |
| `--cloud-swift-project`           | Project name                              |
| `--cloud-swift-project-id`        | Project ID                                |
| `--cloud-swift-domain`            | User domain name                          |
| `--cloud-swift-domain-id`         | User domain ID                            |
| `--cloud-swift-project-domain`    | Project domain name                       |
| `--cloud-swift-project-domain-id` | Project domain ID                         |
| `--cloud-swift-region`            | Region                                    |
| `--cloud-swift-storage-url`       | Override URL returned by Keystone         |

## xbcloud → xtrabackup option migration

Existing scripts that use `xbcloud put` translate mechanically: rename
`--<provider>-<opt>` to `--cloud-<provider>-<opt>`, fold the positional
backup name into the bucket option as `BUCKET/PREFIX`, drop the
`xtrabackup --stream=xbstream | xbcloud put ...` pipe.

### S3

| xbcloud option            | xtrabackup equivalent                |
|---------------------------|--------------------------------------|
| `--storage=s3`            | `--cloud-storage=s3`                 |
| `--s3-bucket=B`           | `--cloud-s3-bucket=B[/PREFIX]`       |
| `--s3-region=R`           | `--cloud-region=R`                   |
| `--s3-endpoint=E`         | `--cloud-endpoint=E`                 |
| `--s3-access-key=K`       | `--cloud-access-key=K`               |
| `--s3-secret-key=K`       | `--cloud-secret-key=K`               |
| `--s3-session-token=T`    | `--cloud-session-token=T`            |
| `--s3-storage-class=C`    | `--cloud-storage-class=C`            |
| `--s3-bucket-lookup=M`    | `--cloud-bucket-lookup=M`            |
| `--s3-api-version=V`      | `--cloud-s3-api-version=V`           |

### Google

| xbcloud option            | xtrabackup equivalent                |
|---------------------------|--------------------------------------|
| `--storage=google`        | `--cloud-storage=gcs`                |
| `--google-bucket=B`       | `--cloud-google-bucket=B[/PREFIX]`   |
| `--google-region=R`       | `--cloud-region=R`                   |
| `--google-endpoint=E`     | `--cloud-endpoint=E`                 |
| `--google-access-key=K`   | `--cloud-access-key=K`               |
| `--google-secret-key=K`   | `--cloud-secret-key=K`               |
| `--google-session-token=T`| `--cloud-session-token=T`            |
| `--google-storage-class=C`| `--cloud-storage-class=C`            |

### Azure

| xbcloud option                | xtrabackup equivalent                       |
|-------------------------------|---------------------------------------------|
| `--storage=azure`             | `--cloud-storage=azure`                     |
| `--azure-storage-account=A`   | `--cloud-azure-account=A`                   |
| `--azure-container-name=C`    | `--cloud-azure-container-name=C[/PREFIX]`   |
| `--azure-access-key=K`        | `--cloud-azure-access-key=K`                |
| `--azure-endpoint=E`          | `--cloud-azure-endpoint=E`                  |
| `--azure-tier-class=T`        | `--cloud-storage-class=T`                   |
| `--azure-development-storage` | `--cloud-azure-development-storage`         |

### Swift

| xbcloud option                | xtrabackup equivalent                       |
|-------------------------------|---------------------------------------------|
| `--storage=swift`             | `--cloud-storage=swift`                     |
| `--swift-container=C`         | `--cloud-swift-container=C[/PREFIX]`        |
| `--swift-auth-url=U`          | `--cloud-swift-auth-url=U`                  |
| `--swift-auth-version=N`      | `--cloud-swift-auth-version=N`              |
| `--swift-user=U`              | `--cloud-swift-user=U`                      |
| `--swift-user-id=U`           | `--cloud-swift-user-id=U`                   |
| `--swift-key=K`               | `--cloud-swift-key=K`                       |
| `--swift-password=P`          | `--cloud-swift-password=P`                  |
| `--swift-tenant=T`            | `--cloud-swift-tenant=T`                    |
| `--swift-tenant-id=T`         | `--cloud-swift-tenant-id=T`                 |
| `--swift-project=P`           | `--cloud-swift-project=P`                   |
| `--swift-project-id=P`        | `--cloud-swift-project-id=P`                |
| `--swift-domain=D`            | `--cloud-swift-domain=D`                    |
| `--swift-domain-id=D`         | `--cloud-swift-domain-id=D`                 |
| `--swift-project-domain=D`    | `--cloud-swift-project-domain=D`            |
| `--swift-project-domain-id=D` | `--cloud-swift-project-domain-id=D`         |
| `--swift-region=R`            | `--cloud-swift-region=R`                    |
| `--swift-storage-url=U`       | `--cloud-swift-storage-url=U`               |

### Common HTTP / retry knobs

| xbcloud option              | xtrabackup equivalent                |
|-----------------------------|--------------------------------------|
| `--insecure`                | `--cloud-insecure`                   |
| `--cacert=F`                | `--cloud-cacert=F`                   |
| `--verbose`                 | `--cloud-verbose`                    |
| `--timeout=N`               | `--cloud-timeout=N`                  |
| `--max-retries=N`           | `--cloud-max-retries=N`              |
| `--max-backoff=N`           | `--cloud-max-backoff=N`              |
| `--curl-retriable-errors=CSV` | `--cloud-curl-retriable-errors=CSV`|
| `--http-retriable-errors=CSV` | `--cloud-http-retriable-errors=CSV`|
| `--header="K: V"`           | `--cloud-header="K: V"`              |
| `--parallel=N`              | `--cloud-max-concurrent-requests=N`  |

### Multipart knobs

| xbcloud option                       | xtrabackup equivalent                       |
|--------------------------------------|---------------------------------------------|
| `--multipart-upload`                 | (removed; ds_cloud is always multipart)     |
| `--multipart-memory-budget`          | (removed; see Memory model)                 |
| `--multipart-part-size=N`            | `--cloud-multipart-part-size=N`             |
| `--multipart-threshold=N`            | `--cloud-multipart-threshold=N`             |
| `--multipart-rollover-threshold=N`   | `--cloud-multipart-rollover-threshold=N`    |

### Removed options

- **`--cloud-bucket` (generic)** -- replaced by provider-explicit
  `--cloud-s3-bucket` / `--cloud-google-bucket` /
  `--cloud-azure-container-name` / `--cloud-swift-container`.
- **`--md5`** -- xbcloud's per-chunk MD5 sidecar is tied to its
  chunked-filename format. The new one-file-per-object model needs
  per-FILE SHA-256 verification, designed via `backup_meta.json` and
  tracked separately (task #54). Not in this release.
- **`--cloud-multipart-upload=ON|OFF`** -- ds_cloud is multipart-only by
  design (the `Stream_multipart_writer` already short-circuits to a
  single PUT for files below `--cloud-multipart-threshold`, so there's
  no non-multipart code path to fall back to). xbcloud keeps the
  equivalent option because xbcloud has a legacy chunk-per-PUT path
  for backward compatibility; ds_cloud never did.
- **`--cloud-multipart-memory-budget`** -- removed. The control was
  per-writer (one writer per file held by an xtrabackup data-copy
  thread), which means setting it to N silently multiplied to
  `N × --parallel` total peak memory. With the default 4 GiB and
  `--parallel=8`, that's 32 GiB → OOM. Hardcoded today to 64 MiB per
  writer; the eventual single global cap will be
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

- **Per-file SHA-256 verification** is deferred to a separate task (see
  `task #54` in the internal tracker / PXB-3754 acceptance criteria).
  Backups today carry a `backup_meta.json` manifest with `logical_size`
  and `sparse_map`; per-file `sha256` and `--verify`-on-download are
  the next iteration. Open design question: hashing semantics for
  sparse-frame xbstream output need careful resolution before shipping.

- **Streaming rollover** (single file >5 TiB on the wire) aborts with
  a clear error. Streaming rollover via the unified manifest is
  queued behind PXB-3754 follow-up work.

- **No `--force` on `--delete`** yet: confirmation is always
  interactive.

- **No parallel range GETs in `--download`**: each object is fetched
  sequentially as a whole. Range-GET split for large files comes with
  the per-object segment list in the manifest.

- **HNS-aware `--delete`**: Azure HNS-enabled containers need a
  directory-aware bottom-up delete (delete blobs first, then directory
  placeholders). xbcloud already does this via the partitioned listing
  API (`ResourceType` parse in `azure.cc`); ds_cloud's `--delete`
  currently uses the same `list_objects_in_directory` helper, so it
  also walks HNS containers correctly. Confirmed via the PXB-3643 /
  PR #1726 path.

- **No `ListMultipartUploads` cleanup in `--delete`**: orphaned
  multipart sessions from crashed backups aren't garbage-collected by
  `--delete`.

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
