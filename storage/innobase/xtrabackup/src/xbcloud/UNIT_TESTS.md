# xbcloud unit tests

`xbcloud-t` tests the HTTP client and the request signing of xbcloud. It is
built with the other XtraBackup unit tests when the build is configured with
`-DWITH_UNIT_TESTS=ON`, against the bundled googletest; no system packages
are needed.

    cmake .. -DWITH_UNIT_TESTS=ON ...
    make xbcloud-t
    runtime_output_directory/xbcloud-t

`t/unittests.sh` in the run.sh test suite runs it as well.

## Signer tests

The `s3v4_signer`, `s3v2_signer` and `azure_signer` tests sign a fixed
request with a fixed time and compare the `Authorization` header with an
expected value. They pin the signing: any change to what xbcloud signs
(a new header, a new API version, a different path) changes the value and
fails the test.

The expected values are not made up and must not simply be copied from the
failing test. They are the values the providers' own signing code produces
for the same request:

| Test            | Checked with                                        |
| --------------- | --------------------------------------------------- |
| `s3v4_signer.*` | botocore `SigV4Auth` (AWS CLI and boto3; GCS too)    |
| `s3v2_signer.*` | botocore `HmacV1Auth`                                |
| `azure_signer.*`| azure-storage-blob `SharedKeyCredentialPolicy`       |

## When a signer test fails after a change

1. Install the official signing code once, in a virtual environment:

       python3 -m venv ~/sigref
       ~/sigref/bin/pip install botocore azure-storage-blob

2. Run the signer tests and dump the requests they signed. Each line of the
   file holds a request exactly as xbcloud signed it: method, URL, every
   header (including the ones the change added) and xbcloud's
   `Authorization`.

       XBCLOUD_DUMP_SIGNED=/tmp/signed.jsonl \
         runtime_output_directory/xbcloud-t --gtest_filter='*signer*'

3. Sign the same requests with the official code:

       ~/sigref/bin/python \
         storage/innobase/xtrabackup/src/xbcloud/expected_signatures.py \
         /tmp/signed.jsonl

   * `OK`: xbcloud signs the request like the provider's own code. Put the
     printed `SDK:` value in `xbcloud-t.cc` as the expected value.
   * `DIFFER`: xbcloud signs the request wrongly. Fix the code, not the test.

4. Rerun `xbcloud-t`; all tests pass.

5. The SDK check proves the signature is computed correctly for the headers
   xbcloud chose. Whether the provider accepts those headers (a new header,
   a new API version) only the real service can tell: run xbcloud against
   real AWS, GCS and Azure buckets before the change is merged.

Remove `/tmp/signed.jsonl` before the next run: the tests append to it.
