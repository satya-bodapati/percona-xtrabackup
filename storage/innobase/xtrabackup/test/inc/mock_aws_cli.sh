#!/bin/sh
# Mock aws CLI for xbcloud's --s3-use-cli + --s3-cli-command tests.
#
# Emits the AWS SDK credential_process JSON schema on stdout.  Values
# come from env vars set by the test:
#
#   MOCK_AWS_ACCESS_KEY_ID       — access key to return
#   MOCK_AWS_SECRET_ACCESS_KEY   — secret key to return
#   MOCK_AWS_SESSION_TOKEN       — optional session token
#   MOCK_AWS_EXPIRATION          — ISO-8601 expiry; default: 2099-01-01T00:00:00Z
#
# LocalStack accepts any HMAC keys as long as SigV4 is correctly
# computed; typical test values are "test"/"test".
set -eu
: "${MOCK_AWS_ACCESS_KEY_ID:?MOCK_AWS_ACCESS_KEY_ID must be set}"
: "${MOCK_AWS_SECRET_ACCESS_KEY:?MOCK_AWS_SECRET_ACCESS_KEY must be set}"
expiration="${MOCK_AWS_EXPIRATION:-2099-01-01T00:00:00Z}"
session_field=""
if [ -n "${MOCK_AWS_SESSION_TOKEN:-}" ]; then
  session_field="\"SessionToken\": \"${MOCK_AWS_SESSION_TOKEN}\","
fi
cat <<EOF
{
  "Version": 1,
  "AccessKeyId": "${MOCK_AWS_ACCESS_KEY_ID}",
  "SecretAccessKey": "${MOCK_AWS_SECRET_ACCESS_KEY}",
  ${session_field}
  "Expiration": "${expiration}"
}
EOF
