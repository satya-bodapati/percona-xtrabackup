#!/bin/sh
# Mock `openstack token issue -f json` for xbcloud's --swift-use-cli
# + --swift-cli-command tests.
#
# openstackswift/saio uses Swift TempAuth, not Keystone.  The real
# openstack CLI wouldn't work against it out of the box.  This shim
# bridges the gap: it does the TempAuth handshake via curl and then
# formats the returned X-Auth-Token as the openstack CLI's JSON
# output shape so xbcloud's SwiftCliProvider can consume it
# unchanged.
#
# Env vars set by the test:
#
#   MOCK_SWIFT_AUTH_URL   — TempAuth URL (e.g. http://.../auth/v1.0)
#   MOCK_SWIFT_USER       — X-Auth-User header value
#   MOCK_SWIFT_KEY        — X-Auth-Key header value
#   MOCK_SWIFT_EXPIRES    — optional ISO-8601 expiry; default 2099-01-01T00:00:00Z
#
# On a real (Keystone-enabled) OpenStack cloud, an operator would use
# the real `openstack` CLI directly via --swift-cli-command="openstack
# token issue -f json".  This shim exists only because the local test
# emulator doesn't speak Keystone.
set -eu
: "${MOCK_SWIFT_AUTH_URL:?MOCK_SWIFT_AUTH_URL must be set}"
: "${MOCK_SWIFT_USER:?MOCK_SWIFT_USER must be set}"
: "${MOCK_SWIFT_KEY:?MOCK_SWIFT_KEY must be set}"
expires="${MOCK_SWIFT_EXPIRES:-2099-01-01T00:00:00Z}"
token=$(curl -fs -D- \
  -H "X-Auth-User: ${MOCK_SWIFT_USER}" \
  -H "X-Auth-Key: ${MOCK_SWIFT_KEY}" \
  "${MOCK_SWIFT_AUTH_URL}" | \
  awk 'BEGIN{IGNORECASE=1} /^X-Auth-Token:/ { sub(/^[^:]*: */,""); sub(/[\r\n]+$/,""); print; exit }')
[ -n "$token" ] || { echo "mock_openstack_cli: no token from TempAuth" >&2; exit 1; }
cat <<EOF
{"id":"$token","expires":"$expires"}
EOF
