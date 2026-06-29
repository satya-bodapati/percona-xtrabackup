# cloud-direct test suite

Tests for xtrabackup's direct-cloud interface (`--cloud-storage`, `--download`,
`--delete`) -- i.e., ds_cloud and its multipart upload path, manifest-driven
sparse restore, and lifecycle ops. **No tests in this suite use xbcloud** --
the legacy `xtrabackup --stream=xbstream | xbcloud put` pipeline is covered
elsewhere.

## Running

The tests need an emulator stack on localhost. A docker-compose file in
`src/xbcloud/prototype/docker-compose.yml` brings up:

  - `localstack/localstack` -- S3 emulator on port 4566
  - `fsouza/fake-gcs-server` -- GCS-compatible emulator on port 4443
  - `mcr.microsoft.com/azure-storage/azurite` -- Azure Blob emulator on 10000
  - `openstackswift/saio` -- Swift all-in-one on port 8080

The `inc/cloud_emu.sh` helper handles bring-up/tear-down per test.

Each test is parametric on `PXB_CLOUD_BACKEND` (default: `s3`):

```sh
PXB_CLOUD_BACKEND=s3    ./run.sh suites/cloud-direct/cloud_round_trip.sh
PXB_CLOUD_BACKEND=azure ./run.sh suites/cloud-direct/cloud_round_trip.sh
PXB_CLOUD_BACKEND=gcs   ./run.sh suites/cloud-direct/cloud_round_trip.sh
PXB_CLOUD_BACKEND=swift ./run.sh suites/cloud-direct/cloud_round_trip.sh
```

Tests skip themselves if docker, docker-compose, curl, or the aws CLI are
missing.

## Why all four backends

Each cloud's multipart-upload protocol has its own quirks:

  - **S3**: ETag-keyed parts, sorted part list at CompleteMultipartUpload.
  - **GCS**: similar S3 XML API surface in fake-gcs-server.
  - **Azure**: block IDs (different identifier shape from S3 part numbers),
    CommitBlockList at finalize.
  - **Swift**: SLO (Static Large Object) manifests; segment naming rules.

Running the same logical test against each backend gives us confidence that
ds_cloud's multipart machinery handles every backend's protocol correctly,
not just S3.

## Adding a new test

The pattern is:

```sh
. inc/common.sh
. inc/cloud_emu.sh

PROVIDER="${PXB_CLOUD_BACKEND:-s3}"

cloud_emu_require_docker
cloud_emu_start
trap cloud_emu_stop EXIT
cloud_emu_wait_for "$PROVIDER"

BUCKET="my-test-$(date +%s)"
cloud_emu_make_bucket "$PROVIDER" "$BUCKET"

CLOUD_FLAGS=$(cloud_emu_xb_flags "$PROVIDER" "$BUCKET")
eval xtrabackup --backup --target-dir=$topdir/B $CLOUD_FLAGS
```

`cloud_emu_xb_flags` returns the right `--cloud-*` arguments for each
backend (endpoint, credentials, bucket-lookup, etc).  Use `eval` so the
flag string expands correctly into the xtrabackup invocation.

## What's NOT covered yet

  - Failure injection (cloud-side 5xx, network errors).  Add via
    `fault_proxy.py` from the prototype harness.
  - Rollover (single object > 5 TiB).  Needs a slow-test variant.
  - Side-by-side comparison with xbcloud.  Tracked in the existing
    `suites/xbcloud/` suite; intentionally not duplicated here.
