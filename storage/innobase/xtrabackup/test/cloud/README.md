# xtrabackup direct-cloud test harness

End-to-end tests for the `--cloud-storage` / `--download` / `--delete`
modes added to xtrabackup as part of PXB-3787 (Phase 2 of the cloud
redesign).

The tests share `cloud_common.sh` which boots the docker-compose stack
under `src/xbcloud/prototype/docker-compose.yml` (LocalStack, Azurite,
openstackswift/saio, fake-gcs-server). Each test creates / cleans its
own bucket within that stack.

## Quick start

```bash
# Build first
(cd ../../bld && make xtrabackup xbcloud xbstream -j8)

# Round-trip without mysqld (validates --download against synthetic data
# uploaded via xbcloud put --multipart-from-file):
bash upload_download_round_trip.sh

# Real backup against a running mysqld:
MYSQL_USER=root MYSQL_SOCKET=/tmp/mysql.sock bash backup_s3.sh
```

## Test inventory

| Test                            | Needs mysqld | Coverage                                  |
|---------------------------------|--------------|-------------------------------------------|
| `upload_download_round_trip.sh` | no           | xbcloud upload + xtrabackup --download    |
| `backup_s3.sh`                  | yes          | xtrabackup --backup to LocalStack         |
| (planned) round_trip_s3.sh      | yes          | backup -> download -> --prepare -> verify |
| (planned) backup_azure.sh       | yes          | same for Azure                            |
| (planned) backup_swift.sh       | yes          | same for Swift                            |

## Environment variables

| Variable              | Default               | Purpose                                |
|-----------------------|------------------------|----------------------------------------|
| `PXB_CLOUD_BACKEND`   | `s3`                  | `s3` / `azure` / `swift` / `gcs`       |
| `PXB_BUCKET`          | `pxb-cloud-test`      | bucket / container name                |
| `BLD`                 | `$REPO_ROOT/bld`      | build directory                        |
| `MYSQL_USER` etc.     | `root` / unset        | client connection args (backup tests)  |
| `AWS_ACCESS_KEY_ID`   | `test`                | LocalStack accepts any                 |
| `AWS_SECRET_ACCESS_KEY` | `test`              | "                                      |

## What's NOT yet covered

- Sparse `.ibd` round-trip (sparse_map in manifest is PXB-3754 follow-up).
- Rollover for files >5 TiB (streaming path; needs unified manifest).
- Per-backend smokes for Azure/Swift backup (planned).
- Production round-trip with `--prepare` + diff against source mysqld
  (planned `round_trip_s3.sh`).
