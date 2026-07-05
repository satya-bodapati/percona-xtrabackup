#!/bin/sh
# Mock `gcloud auth application-default print-access-token` for
# xbcloud's --google-use-cli + --google-cli-command tests.
#
# The real gcloud CLI prints just the raw access token on stdout —
# no expiry.  This shim does the same.
#
# Env vars:
#   MOCK_GCP_TOKEN — the Bearer token to return
#
# Same story as mock_azure_cli.sh: this exists for symmetry, but the
# fake-gcs-server emulator has an <IsTruncated> compat gap that
# prevents xbcloud's list-bucket parser from working, so the
# emu_gcs_cli_roundtrip.sh test skips.  In a real GCP environment,
# the real `gcloud` CLI is what xbcloud shells out to.
set -eu
echo "${MOCK_GCP_TOKEN:-mock-gcp-token}"
