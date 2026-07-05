#!/bin/sh
# Mock `az account get-access-token` for xbcloud's --azure-use-cli
# + --azure-cli-command tests.
#
# Emits the JSON shape the real `az` CLI produces on stdout.  This
# shim exists for symmetry with the AWS / Swift / GCP shims; the
# actual emu_azure_cli_roundtrip.sh test skips because Azurite (the
# local Azure Blob emulator) doesn't accept Bearer tokens on the
# same endpoints — same reason emu_gcs_oauth2_roundtrip.sh and the
# Managed-Identity path can't be exercised locally either.
#
# Env vars:
#   MOCK_AZURE_TOKEN      — the Bearer token to return
#   MOCK_AZURE_EXPIRES_ON — unix timestamp; default: 2099-01-01
set -eu
token="${MOCK_AZURE_TOKEN:-mock-azure-token}"
expires_on="${MOCK_AZURE_EXPIRES_ON:-4102444800}"
cat <<EOF
{
  "accessToken": "${token}",
  "expiresOn": "2099-01-01 00:00:00.000000",
  "expires_on": ${expires_on},
  "subscription": "mock-subscription-uuid",
  "tenant": "mock-tenant-uuid",
  "tokenType": "Bearer"
}
EOF
