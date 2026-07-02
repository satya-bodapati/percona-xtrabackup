############################################################################
# suites/oidc/oidc_keycloak.sh
#
# PXB OIDC authentication — dev-provisioned Keycloak via Docker.
#
# This is the "bring your own Keycloak" real-IdP path.  Two operational
# modes are supported:
#
#   Auto-bootstrap  (recommended for dev / Jenkins):
#     Export OIDC_BOOTSTRAP_KEYCLOAK=1.  The test sources
#     inc/oidc_keycloak_docker.sh, which starts a Keycloak container,
#     provisions realm/client/user, fetches an ID token, and installs an
#     EXIT trap to tear the container down when the test finishes
#     (pass, fail, or interrupt).  Requires docker + curl + jq on PATH.
#
#   External Keycloak:
#     Leave OIDC_BOOTSTRAP_KEYCLOAK unset (default) and export the
#     KEYCLOAK_* vars yourself.  Same shape as the xbcloud tests'
#     XBCLOUD_CREDENTIALS pattern — container lifecycle stays with the
#     operator (Jenkins job).
#
# ---------------------------------------------------------------------------
# Required environment variables — set these BEFORE running the test:
#
#   AUTH_OIDC_SERVER_SO
#     Absolute path to auth_openid_connect.so (from PS 8.4.10+ build).
#
#   OIDC_BOOTSTRAP_KEYCLOAK=1    (auto-bootstrap mode; recommended)
#     Requires: docker, curl, jq on PATH.
#
#   OR, for external-Keycloak mode:
#
#   KEYCLOAK_ISSUER
#     OIDC issuer URL, must match the 'iss' claim in the token.
#     Example: http://localhost:8080/realms/pxb
#
#   KEYCLOAK_AUDIENCE
#     Expected 'aud' claim value; usually the client_id.
#
#   KEYCLOAK_JWKS_URL
#     JWKS endpoint URL.
#     Example: http://localhost:8080/realms/pxb/protocol/openid-connect/certs
#
#   KEYCLOAK_ID_TOKEN
#     Path to a file containing a valid ID token (pre-fetched by you).
#
#   KEYCLOAK_MYSQL_USER          (optional)
#     mysql-side user name; defaults to mysql_oidc_user.
############################################################################

. inc/common.sh
. inc/oidc_common.sh

vlog "oidc_keycloak.sh requires: AUTH_OIDC_SERVER_SO plus one of:"
vlog "  (a) OIDC_BOOTSTRAP_KEYCLOAK=1  + docker + curl + jq  — auto-bootstrap"
vlog "  (b) KEYCLOAK_ISSUER + KEYCLOAK_AUDIENCE + KEYCLOAK_JWKS_URL +"
vlog "      KEYCLOAK_ID_TOKEN (+ optional KEYCLOAK_MYSQL_USER)  — external"

require_server_version_higher_than 8.4.0
oidc_require_server_plugin

# Auto-bootstrap a container if the operator opted in.  This sources
# inc/oidc_keycloak_docker.sh, which exports KEYCLOAK_* and installs a
# teardown trap.
if [ "${OIDC_BOOTSTRAP_KEYCLOAK:-0}" = "1" ]; then
    . inc/oidc_keycloak_docker.sh
fi

if [ -z "${KEYCLOAK_ISSUER:-}" ] || [ -z "${KEYCLOAK_AUDIENCE:-}" ] || \
   [ -z "${KEYCLOAK_JWKS_URL:-}" ]; then
    skip_test "KEYCLOAK_ISSUER/AUDIENCE/JWKS_URL not set and OIDC_BOOTSTRAP_KEYCLOAK != 1 — see this test's header"
fi
if [ -z "${KEYCLOAK_ID_TOKEN:-}" ] || [ ! -s "${KEYCLOAK_ID_TOKEN}" ]; then
    skip_test "KEYCLOAK_ID_TOKEN not set or file empty — fetch a real ID token first"
fi

# Extract 'sub' from the token (Keycloak issues UUIDs there) so the
# CREATE USER … AS clause matches.  KEYCLOAK_MYSQL_USER only names the
# mysql-side account, unrelated to what the IdP calls the identity.
IDP_SUB=$(oidc_extract_sub "${KEYCLOAK_ID_TOKEN}")
vlog "Token sub (IdP user id) = ${IDP_SUB}"

MYSQL_OIDC_USER=${KEYCLOAK_MYSQL_USER:-mysql_oidc_user}

CONFIG_JSON=$(cat <<EOF
{
  "oidc-idp": {
    "issuer-name": "${KEYCLOAK_ISSUER}",
    "jwks-url": "${KEYCLOAK_JWKS_URL}",
    "audiences": ["${KEYCLOAK_AUDIENCE}"]
  }
}
EOF
)

start_server --plugin-dir="${AUTH_OIDC_PLUGIN_DIR}"

oidc_install_and_configure_server_plugin "JSON://${CONFIG_JSON}"
oidc_create_backup_user "${MYSQL_OIDC_USER}" "${IDP_SUB}" "oidc-idp"

load_dbase_schema sakila
load_dbase_data sakila

########################################################################
# Backup + prepare + restore, authenticating via the Keycloak token
########################################################################
oidc_backup_prepare_restore_verify \
    "${MYSQL_OIDC_USER}" \
    "${KEYCLOAK_ID_TOKEN}" \
    "${topdir}/backup_keycloak" \
    "${AUTH_OIDC_PLUGIN_DIR}"

########################################################################
# Cleanup.  If auto-bootstrapped, the container itself is torn down by
# the EXIT trap in inc/oidc_keycloak_docker.sh.
########################################################################
run_cmd ${MYSQL} ${MYSQL_ARGS} -e "
    DROP USER '${MYSQL_OIDC_USER}';
    UNINSTALL PLUGIN auth_openid_connect;
"
