############################################################################
# suites/oidc/oidc_percona.sh
#
# PXB OIDC authentication — live Percona-hosted Keycloak.
#
# Counterpart of PS's mysql-test/suite/auth_openid_connect/t/idp.test.
# Hits Percona's shared Keycloak (https://keycloak.int.percona.com) and
# fetches an ID token via ROPC password grant, then drives xtrabackup
# through backup + prepare + copy-back + row-count verification.
#
# Skips cleanly when the IdP endpoint isn't reachable (no VPN, network
# outage, IdP maintenance) — the test doesn't require the operator to
# tunnel network access.
#
# The client_id / username / password below are the same test-only
# credentials used by PS's idp.test.  Do not reuse for anything else.
#
# ---------------------------------------------------------------------------
# Required environment variables — set these BEFORE running the test:
#
#   AUTH_OIDC_SERVER_SO
#     Absolute path to auth_openid_connect.so (server-side plugin from
#     a PS 8.4.10+ build).
#
# Also required on PATH:  curl, jq
#
# Also required at runtime:
#   VPN reachability to keycloak.int.percona.com.
############################################################################

. inc/common.sh
. inc/oidc_common.sh

vlog "oidc_percona.sh requires: AUTH_OIDC_SERVER_SO + curl + jq + VPN to keycloak.int.percona.com"

require_server_version_higher_than 8.4.0
oidc_require_server_plugin

# Hardcoded Percona-hosted IdP configuration (mirrors PS idp.test).
IDP_BASE="https://keycloak.int.percona.com/realms/master"
IDP_URL="${IDP_BASE}/protocol/openid-connect"
IDP_ISSUER="${IDP_BASE}"
IDP_JWKS_URL="${IDP_URL}/certs"
IDP_TOKEN_URL="${IDP_URL}/token"
IDP_CLIENT_ID="myclient"
IDP_USER="kkuser"
IDP_PASSWORD="alamakota1"

# Reachability probe — skip if the IdP is unreachable (VPN down, outage).
if ! curl --fail --silent --show-error --max-time 5 \
        --output /dev/null "${IDP_JWKS_URL}"; then
    skip_test "Percona OIDC IdP not reachable at ${IDP_JWKS_URL} — check VPN"
fi

TOKEN_FILE=${TEST_VAR_ROOT}/id_token.txt
oidc_fetch_ropc_token "${IDP_TOKEN_URL}" "${IDP_CLIENT_ID}" \
                      "${IDP_USER}" "${IDP_PASSWORD}" "${TOKEN_FILE}"

# Keycloak stamps a UUID into 'sub', not the login username, so the
# CREATE USER … AS clause must reference the UUID.  Extracting it
# dynamically means the test survives Keycloak re-provisioning without
# any code change (PS's idp.test hardcodes it in set_idp_vars.inc).
IDP_SUB=$(oidc_extract_sub "${TOKEN_FILE}")
vlog "Token sub (IdP user id) = ${IDP_SUB}"

CONFIG_JSON=$(cat <<EOF
{
  "oidc-idp": {
    "issuer-name": "${IDP_ISSUER}",
    "jwks-url": "${IDP_JWKS_URL}",
    "audiences": ["${IDP_CLIENT_ID}"]
  }
}
EOF
)

start_server --plugin-dir="${AUTH_OIDC_PLUGIN_DIR}"

oidc_install_and_configure_server_plugin "JSON://${CONFIG_JSON}"

MYSQL_OIDC_USER=mysql_oidc_user
oidc_create_backup_user "${MYSQL_OIDC_USER}" "${IDP_SUB}" "oidc-idp"

load_dbase_schema sakila
load_dbase_data sakila

########################################################################
# Backup + prepare + restore, authenticating via the Percona-issued token
########################################################################
oidc_backup_prepare_restore_verify \
    "${MYSQL_OIDC_USER}" \
    "${TOKEN_FILE}" \
    "${topdir}/backup_percona_idp" \
    "${AUTH_OIDC_PLUGIN_DIR}"

########################################################################
# Negative case: an invalid/missing token file must fail before connect
########################################################################
vlog "Negative: missing id-token-file must fail"
run_cmd_expect_failure ${XB_BIN} ${XB_ARGS} \
    --user="${MYSQL_OIDC_USER}" \
    --authentication-openid-connect-client-id-token-file="${TEST_VAR_ROOT}/nonexistent_token.txt" \
    --backup \
    --target-dir=${topdir}/backup_negative

########################################################################
# Cleanup
########################################################################
run_cmd ${MYSQL} ${MYSQL_ARGS} -e "
    DROP USER '${MYSQL_OIDC_USER}';
    UNINSTALL PLUGIN auth_openid_connect;
"
