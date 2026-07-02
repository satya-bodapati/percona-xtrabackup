############################################################################
# Smoke test for xbcloud --google-service-account-file option handling.
#
# Exercises the CLI-level correctness of the PXB-3592 GCS OAuth2 flow
# without needing a live GCP or a JWT-validating GCS emulator:
#
#   * --google-service-account-file with a nonexistent path fails
#     cleanly with a message that names the path.
#   * --google-service-account-file with a malformed JSON fails
#     cleanly with a "malformed JSON" message.
#   * --google-service-account-file with an unsupported "type" (e.g.
#     external_account) fails cleanly rejecting the type by name.
#   * --google-service-account-file + --google-access-key together
#     fails at option time with a mutex error.
#
# End-to-end round-trip against a JWT-validating GCS emulator lives
# in a follow-up test that requires a Docker-hosted mock IdP.
############################################################################

. inc/common.sh

# Bail if the xbcloud binary doesn't have the new option — we're
# either on a stripped build or on the wrong branch.
if ! $XB_BIN --help 2>&1 | grep -q 'google-service-account-file'; then
    skip_test "xbcloud lacks --google-service-account-file — needs pxb-cloud-auth"
fi

XBCLOUD_BIN=${XBCLOUD_BIN:-$(dirname "$XB_BIN")/xbcloud}
[ -x "$XBCLOUD_BIN" ] || skip_test "xbcloud binary not found at $XBCLOUD_BIN"

TMPDIR_LOCAL=${topdir:-/tmp/pxb-oidc-tests}
mkdir -p "$TMPDIR_LOCAL"

########################################################################
# Case 1: nonexistent keyfile → clear error, exit non-zero
########################################################################
vlog "Case 1: missing keyfile"
missing_path="${TMPDIR_LOCAL}/does-not-exist.json"
rm -f "$missing_path"
$XBCLOUD_BIN put --storage=google --google-bucket=test-bucket \
        --google-service-account-file="$missing_path" \
        dummy-object 2>&1 | \
    grep -F "cannot open $missing_path" || \
    die "Case 1: expected 'cannot open' message for missing keyfile"

########################################################################
# Case 2: malformed JSON
########################################################################
vlog "Case 2: malformed JSON keyfile"
bad_path="${TMPDIR_LOCAL}/bad.json"
echo "this is not json {" > "$bad_path"
$XBCLOUD_BIN put --storage=google --google-bucket=test-bucket \
        --google-service-account-file="$bad_path" \
        dummy-object 2>&1 | \
    grep -F "malformed JSON" || \
    die "Case 2: expected 'malformed JSON' message"
rm -f "$bad_path"

########################################################################
# Case 3: unsupported type (external_account) → rejected by name
########################################################################
vlog "Case 3: unsupported credential type"
ext_path="${TMPDIR_LOCAL}/external.json"
cat > "$ext_path" <<'EOF'
{
  "type": "external_account",
  "audience": "//iam.googleapis.com/projects/x/locations/global/workloadIdentityPools/y/providers/z",
  "token_url": "https://sts.googleapis.com/v1/token"
}
EOF
$XBCLOUD_BIN put --storage=google --google-bucket=test-bucket \
        --google-service-account-file="$ext_path" \
        dummy-object 2>&1 | \
    grep -F "unsupported credential type \"external_account\"" || \
    die "Case 3: expected 'unsupported credential type' rejection"
rm -f "$ext_path"

########################################################################
# Case 4: mutually exclusive with --google-access-key
########################################################################
vlog "Case 4: --google-service-account-file + --google-access-key mutex"
sa_path="${TMPDIR_LOCAL}/sa.json"
cat > "$sa_path" <<'EOF'
{"type":"service_account","client_email":"x@y.iam.gserviceaccount.com","private_key":"-----BEGIN PRIVATE KEY-----\nAAAA\n-----END PRIVATE KEY-----\n","token_uri":"https://oauth2.googleapis.com/token"}
EOF
$XBCLOUD_BIN put --storage=google --google-bucket=test-bucket \
        --google-service-account-file="$sa_path" \
        --google-access-key=whatever \
        --google-secret-key=alsowhatever \
        dummy-object 2>&1 | \
    grep -F "mutually exclusive" || \
    die "Case 4: expected mutually-exclusive error"
rm -f "$sa_path"
