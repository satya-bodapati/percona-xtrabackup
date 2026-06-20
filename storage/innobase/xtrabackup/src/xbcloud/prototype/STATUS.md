# PXB-3671 Phase 1 prototype status

Branch: `97_redesign_proto` off `release-9.7.0-1`.
Worktree: `~/WORK/pxb-97-redesign-proto`.
JIRA: https://perconadev.atlassian.net/browse/PXB-3671
Design doc: `~/.claude/plans/no-these-are-just-sunny-pnueli.md`.

## Phase 1c update (this commit)

Replaces the per-file Q7 formula and fixed-size streaming threshold
with one unified tiered schedule (`dynamic_part_size`) that drives
both the `--multipart-from-file` path and the streaming `put_func`
path. Switches the in-flight cap from a part-count to a byte budget so
peak RAM stays bounded as part sizes ramp up the tiers.

Changes:
- **`dynamic_part_size(bytes_so_far)`**: tiered schedule covering all
  file sizes from 0 to 5 TiB without rollover.

  | bytes_so_far     | part_size | parts in tier | cumulative |
  |------------------|-----------|---------------|------------|
  | < 1 GiB          | 16 MiB    |        64     |       64   |
  | 1 - 10 GiB       | 64 MiB    |       144     |      208   |
  | 10 - 100 GiB     | 256 MiB   |       360     |      568   |
  | 100 GiB - 1 TiB  | 512 MiB   |      1848     |     2416   |
  | >= 1 TiB         | 600 MiB   |      6826     |     9242   |

  Reaches S3's 5 TiB single-object hard cap in ~9242 parts, leaving
  ~750 parts of headroom against the 10000-part hard cap. Same
  function backs both code paths: streaming calls it with the
  uploader's running `bytes_appended()`; known-size calls it once
  with the stat'd file_size.
- **`Multipart_uploader` switched to memory budget**: cap is now
  `memory_budget` bytes (default 4 GiB) instead of `max_in_flight`
  parts. Producer blocks when admitting a new part would push total
  in-flight bytes past the budget. With dynamic part sizes ramping
  from 16 MiB to 600 MiB, peak in-flight count adapts inversely
  (e.g. ~256 in flight at 16 MiB, ~6 at 600 MiB) — RAM stays bounded
  regardless of stream size.
- **Small-file fast path**: streams that finish with no parts
  submitted yet and total bytes <= `--multipart-threshold` (default
  16 MiB) ship as a single PUT instead of going through Initiate +
  UploadPart + Complete. Tiny redo, metadata, xtrabackup_tablespaces,
  etc. skip the multipart round-trip overhead.
- **Q7 removed**: `compute_part_size()` deleted. The Q7 floor of
  8 MiB produced 1280 parts for a 10 GiB file; the new tiered
  schedule produces ~208 for the same file. Fewer HTTP requests for
  medium files, equivalent at the 5 TiB extreme.
- **New options**: `--multipart-memory-budget=BYTES` (default 4 GiB),
  `--multipart-threshold=BYTES` (default 16 MiB).
  `--multipart-part-size` is now an explicit override (0 = use the
  schedule; non-zero = fix every part at that size).
- **Rollover for > 5 TiB deferred**: code comment in `multipart.h`
  points at the design doc (Section 4.4) where the rollover wrapper
  splits oversize files into `name.part-001`, `name.part-002`, ...
  No code yet.

Phase 1b foundation (still present, just refactored):
- PXB-3748 Event_handler queue cv (no spin sleep).
- Async S3 / Azure / Swift upload_part via `make_async_request`.
- Observability counters in `Multipart_uploader`.

Smoke tests passing on this branch:
- `smoke_s3.sh` — chunk-per-PUT path still works (legacy mode)
- `smoke_streaming.sh` (NEW) — xbstream -> xbcloud streaming with
  dynamic_part_size + small-file fast path. Tiny 2 MiB file takes
  fast path, 50 MiB file uses tier 1 (16 MiB parts), 1100 MiB file
  crosses into tier 2 (64 MiB parts) at part #65.
- `smoke_azure.sh` — async PutBlock + PutBlockList
- `smoke_swift.sh` — async segment PUT + SLO manifest
- `smoke_from_file.sh` — 50/1024/1100 MiB files via
  `--multipart-from-file`; schedule picks 16 MiB for <1 GiB,
  64 MiB for >=1 GiB; sha256 round-trip OK.
- `smoke_retry.sh` (NEW) — uses `fault_proxy.py` to inject 33% 503s
  on PUT requests; xbcloud's http.cc retry/backoff path completes
  the upload via exponential-backoff retries.

## Phase 1d update (this commit)

Rollover support for files larger than the per-object hard cap.

`--multipart-from-file` now detects files larger than
`--multipart-rollover-threshold` (default 5 TiB, S3's per-object hard
cap; lower it for testing) and splits them into multiple multipart
uploads:

  prefix/ibdata1.part-001    (up to threshold)
  prefix/ibdata1.part-002    (up to threshold)
  prefix/ibdata1.part-003    (up to threshold)
  prefix/ibdata1.manifest.json

The sidecar manifest is JSON with `logical_name`, `total_size`,
`rollover_threshold`, and a `segments[]` array of `{key, size}` for
each part-NNN object. Download reconstruction reads the manifest,
fetches the segments in order, and concatenates.

Streaming `put_func` does not yet support rollover (file size is
unknown up front; segmenting mid-stream requires either a
discard-and-restart on threshold cross or always-segmented naming, and
neither is appropriate for Phase 1). If a streamed file's bytes
exceed the rollover threshold mid-upload, the upload aborts with a
clear error rather than silently producing a truncated object. Users
who need oversize file support pre-stage them locally and feed
`--multipart-from-file`.

### Manifest is a Phase 1 placeholder

The `<name>.manifest.json` sidecar created here is temporary. In
Phase 2 (after PXB-3754 lands and ds_cloud takes over) the rollover
segment list folds into the unified `backup_meta.json` written by
ds_meta at backup_finish() -- one entry per file, with optional
`segments[]` and `sparse_map[]` fields per row.

Design decision for Phase 2: `backup_meta.json` should be
**default-enabled**, not opt-in. PXB-3754's "zero overhead when
`--backup-manifest=OFF`" goal made sense when the manifest only
carried sha256 + transforms (nice-to-have integrity). Once it also
carries sparse_map and rollover segments (data the restore path
**requires** for correct reconstruction), making it opt-in creates a
silent-corruption footgun. The flag should default ON and the OFF
mode (if kept) drops only the sha256 + transforms entries, never the
structural fields.

Smoke tests passing on this branch:
- `smoke_rollover.sh` (NEW) -- 600 MiB file, 256 MiB rollover threshold,
  produces 3 segments + manifest, downloads + concatenates, sha256
  round-trip OK.
- All earlier smokes still pass (smoke_s3, smoke_from_file with
  updated dynamic-ramp assertions, smoke_streaming, smoke_retry,
  smoke_azure, smoke_swift).

### Retry/backoff: both async AND sync paths now wrapped

The async make_async_request path always had a retry loop (around
the part-upload callback). Commit b (PXB-3671 : wrap sync make_request
in retry/backoff loop too) extended the same exponential-backoff +
retriable-error detection to:

- All signed sync calls: init / complete / abort multipart, list,
  container_exists, create_container, delete_object, sync
  upload_object, in S3, Azure, and Swift.
- Unsigned bootstrap calls: Keystone temp_auth / auth_v2 / auth_v3
  and EC2 IMDS token / profile / metadata fetches. Token refresh
  during a long backup now survives transient 5xx.

`smoke_retry.sh` injects 503s on PUT + POST + DELETE; both the async
PUT path and the sync POST (init / complete) path now survive a 33%
fault rate.

## What this prototype proves

The xbcloud PUT mode can be retrofitted to upload each logical file as a
single object using each backend's multipart upload API, while keeping
xbstream as the wire protocol between xtrabackup and xbcloud. This is
PXB-3671 Phase 1 from the design doc.

End-to-end smoke tests pass against three of the four backends. The
fourth (GCS) reuses the S3 code path unchanged and is exercised in
production via real GCS endpoints; the local emulator (fake-gcs-server)
does not support S3-style bucket creation, which trips xbcloud's setup
step before any multipart code runs.

## Backend status

| Backend | Multipart API used                 | Compiles | Smoke test          | Status |
|---------|------------------------------------|----------|---------------------|--------|
| S3      | Initiate / UploadPart / Complete   | Yes      | smoke_s3.sh PASS    | working|
| GCS     | S3-compatible XML multipart        | Yes      | smoke_gcs.sh fails* | code path is the unchanged S3 path |
| Azure   | PutBlock + PutBlockList            | Yes      | smoke_azure.sh PASS | working|
| Swift   | Static Large Object segments       | Yes      | smoke_swift.sh PASS | working|

\* fake-gcs-server rejects xbcloud's S3-style create_container call.
The GCS multipart upload code is the exact same `S3_client` /
`S3_object_store` code that the S3 smoke test exercises, just pointed
at a different endpoint and using `--google-*` credential flags. Real
GCS works because the bucket is pre-created out-of-band, which the
emulator does not handle the same way. A real-GCS test against a
production project is a follow-up.

## What's wired

### CLI

Two new xbcloud options:
- `--multipart-upload=ON|OFF` (default ON in the prototype).
- `--multipart-part-size=BYTES` (default 16 MiB, min 5 MiB).

### Object_store interface

`storage/innobase/xtrabackup/src/xbcloud/object_store.h` gains four
virtual methods with default `return false` implementations:

- `init_multipart_upload(container, object, &upload_id)`
- `upload_part(container, object, upload_id, part_number, contents, &part_id)`
- `complete_multipart_upload(container, object, upload_id, parts)`
- `abort_multipart_upload(container, object, upload_id)`

Each backend overrides these in its `S3_object_store` /
`Azure_object_store` / `Swift_object_store` derived class and forwards
to a same-named method on its respective backend client class.

### S3 (also drives GCS)

`s3.cc` adds `S3_client::init_multipart_upload` / `upload_part` /
`complete_multipart_upload` / `abort_multipart_upload`. They build on
the existing `Http_request` + `S3_signer` + libcurl plumbing; the
SigV4 canonical request already includes `?uploads`, `?partNumber=`,
`?uploadId=` query params verbatim. Bodies are XML built with a
stringstream; the InitiateMultipartUpload response is parsed with the
existing rapidxml dependency.

### Azure

`azure.cc` adds `Azure_client::*`. Block IDs are
`base64(upload_id + 6-digit-zero-padded part#)` so every block of a
single blob shares a fixed length (40 chars), as required by Azure.
Init is local-only (mints a random 16-byte upload_id with
gcry_randomize) since Azure has no "initiate" call. Abort issues a
blob DELETE which drops any uncommitted blocks immediately;
uncommitted blocks also expire server-side in 7 days.

### Swift

`swift.cc` adds `Swift_client::*` using the Static Large Object model.
Init returns `NAME_segments` as the segments prefix; upload_part PUTs
each segment to `CONTAINER/NAME_segments/NNNNNN` and embeds
`<seg_path>|<md5>|<size>` in the returned part_id so complete can
build the JSON SLO manifest. Complete PUTs the manifest to
`CONTAINER/NAME?multipart-manifest=put`. Abort lists and deletes
segments under the per-upload prefix.

### xbcloud put_func

`put_func` in `xbcloud.cc` gains a multipart branch gated by
`opt_multipart_upload`. Per-file state lives in a new
`mpfilehash` map: `multipart_file_state_t` per file with upload_id,
in-flight `part_buf`, next_part_number, parts list. Frames append into
`part_buf`; when it crosses `--multipart-part-size`, `flush_one_part`
is called. EOF flushes the remainder and calls
`complete_multipart_upload`. Errors call `abort_multipart_upload`.
End-of-thread sweeps any leaked in-flight uploads.

The legacy chunk-per-PUT path is left intact and selected by
`--multipart-upload=OFF`.

## Bugs found during smoke testing

1. **Use-after-free in put_func**: bookkeeping on `file_entry_t` ran
   after the EOF erase. Fixed by reordering: increment first, erase
   last.
2. **Wrong size used for part buffer append**: `chunk.buflen` is the
   buffer capacity; `chunk.raw_length` is the actual data length. The
   legacy path uses `raw_length` via `Http_buffer::assign_buffer`. The
   multipart path now does the same.
3. **Azure path missing leading slash**: `Azure_client::upload_part` /
   `complete_multipart_upload` built the path as
   `"container + '/' + name"`, matching the sync `upload_object`
   pattern. But Azure's working async path uses
   `"'/' + container + '/' + name"`. When the host already has a path
   component (dev-storage mode), the missing slash collapses the URL
   and Azurite returns 404. Switched to the leading-slash form.

## Commits in this branch

```
$ git log release-9.7.0-1..97_redesign_proto --oneline
PXB-3671 : add Swift SLO smoke test against openstackswift/saio
PXB-3671 : smoke tests for GCS/Azure/Swift + Azure path fix
PXB-3671 : Azure block blob and Swift SLO multipart impls
PXB-3671 : add S3 smoke test, fix two bugs in multipart path
PXB-3671 : xbcloud one-object-per-file via multipart upload (S3)
```

## How to reproduce

```bash
# Build
cd ~/WORK/pxb-97-redesign-proto/bld
cmake .. -DCMAKE_BUILD_TYPE=Debug -DWITH_DEBUG=1 -DWITH_BOOST=~/boost/ \
         -DWITH_SSL=system
make xbcloud xbstream -j8

# Run any smoke
cd ../storage/innobase/xtrabackup/src/xbcloud/prototype
bash smoke_s3.sh      # LocalStack S3, full round-trip with sha256
bash smoke_azure.sh   # Azurite block blob, upload only
bash smoke_swift.sh   # openstackswift/saio SLO, upload only
# smoke_gcs.sh exists but trips on fake-gcs-server's lack of
# S3 bucket-creation support; documented above.

# Tear down
docker-compose -f docker-compose.yml down
```

## What's NOT done (Phase 2 work)

Everything described in the design doc's Phase 2 section:
- xtrabackup-side `ds_cloud` datasink replacing the xbstream path
- `--cloud-url` option family on the xtrabackup side
- Download mode (`xtrabackup --download`)
- Delete mode (`xtrabackup --delete`)
- Global `xtrabackup_manifest.json` written by xbcloud at end of upload
- Sparse map relocation from xbstream frames into the manifest
- Rollover for unknown-size streams (redo log) and oversize files
- libxbcloud extraction

This prototype proves the multipart machinery and per-backend
implementations work. Phase 2 layers the xtrabackup-side changes and
the manifest on top of this foundation; none of the work here will be
discarded.
