############################################################################
# suites/oidc/oidc_jwt.sh
#
# PXB OIDC authentication — fast-path test with locally-signed JWT.
#
# Uses PS's dummy in-memory JWKS (mysql-test/std_data/oidc/dummy_oidc_conf.json)
# and the create_id_token helper to sign a JWT that the server's
# auth_openid_connect plugin can verify against the baked-in public key.
# No live IdP is contacted. Deterministic and fast — the primary
# regression test for the client plugin, the CLI option, and the
# xb_mysql_connect glue.
#
# For real-IdP variants see:
#   suites/oidc/oidc_percona.sh    (live Percona-hosted Keycloak)
#   suites/oidc/oidc_keycloak.sh   (Docker-bootstrapped Keycloak)
#
# ---------------------------------------------------------------------------
# Required environment variables — set these BEFORE running the test:
#
#   AUTH_OIDC_SERVER_SO
#     Absolute path to auth_openid_connect.so (server-side plugin).
#     Comes from a Percona Server 8.4.10+ build (PR #5941 merged).
#     Example:
#       /home/you/WORK/ps-84/bld/plugin_output_directory/auth_openid_connect.so
#
#   CREATE_ID_TOKEN
#     Absolute path to the create_id_token helper (built alongside the
#     server plugin).  Example:
#       /home/you/WORK/ps-84/bld/runtime_output_directory/create_id_token
#
#   AUTH_OIDC_STD_DATA
#     Directory containing idp_private.pem and dummy_oidc_conf.json.
#     Example:
#       /home/you/WORK/ps-84/mysql-test/std_data/oidc
############################################################################

. inc/common.sh
. inc/oidc_common.sh

# Print required env vars upfront so anyone reading the results log sees
# what the test expects — regardless of which skip path fires.
vlog "oidc_jwt.sh requires: AUTH_OIDC_SERVER_SO CREATE_ID_TOKEN AUTH_OIDC_STD_DATA"

require_server_version_higher_than 8.4.0
oidc_require_server_plugin

if [ -z "${CREATE_ID_TOKEN:-}" ] || [ ! -x "${CREATE_ID_TOKEN}" ]; then
    skip_test "CREATE_ID_TOKEN not set or not executable — export a path to the PS create_id_token helper"
fi
if [ -z "${AUTH_OIDC_STD_DATA:-}" ] || [ ! -d "${AUTH_OIDC_STD_DATA}" ]; then
    skip_test "AUTH_OIDC_STD_DATA not set or dir missing — export the path to PS mysql-test/std_data/oidc/"
fi
if [ ! -f "${AUTH_OIDC_STD_DATA}/idp_private.pem" ] || \
   [ ! -f "${AUTH_OIDC_STD_DATA}/dummy_oidc_conf.json" ]; then
    skip_test "AUTH_OIDC_STD_DATA missing idp_private.pem or dummy_oidc_conf.json"
fi

start_server --plugin-dir="${AUTH_OIDC_PLUGIN_DIR}"

oidc_install_and_configure_server_plugin \
    "FILE://${AUTH_OIDC_STD_DATA}/dummy_oidc_conf.json"

# create_id_token stamps whatever we pass via --sub straight into the JWT,
# so no dynamic sub extraction is needed for the fast path.
IDP_SUB="oidc-user"
MYSQL_OIDC_USER=mysql_oidc_user
oidc_create_backup_user "${MYSQL_OIDC_USER}" "${IDP_SUB}" "oidc-idp"

TOKEN_FILE="${topdir}/oidc_id_token.txt"
vlog "Generating ID token via ${CREATE_ID_TOKEN}"
run_cmd "${CREATE_ID_TOKEN}" \
    --key "${AUTH_OIDC_STD_DATA}/idp_private.pem" \
    --sub "${IDP_SUB}" \
    --iss https://idp-test.com/realms/dummy \
    --aud ee2811b9-10b8 \
    --kid rsa-key-1 \
    --out "${TOKEN_FILE}"
[ -s "${TOKEN_FILE}" ] || die "create_id_token did not produce a token at ${TOKEN_FILE}"

load_dbase_schema sakila
load_dbase_data sakila

########################################################################
# Case 1: happy path — backup + prepare + copy-back + verify
########################################################################
vlog "Case 1: --backup with OIDC id-token (locally-signed JWT)"
oidc_backup_prepare_restore_verify \
    "${MYSQL_OIDC_USER}" \
    "${TOKEN_FILE}" \
    "${topdir}/backup_oidc" \
    "${AUTH_OIDC_PLUGIN_DIR}"

########################################################################
# Case 2: mutual exclusion — --password + OIDC rejected before any
# server contact.  The mutex is in xb_mysql_connect() itself.
########################################################################
vlog "Case 2: rejecting --password alongside OIDC id-token option"
run_cmd_expect_failure xtrabackup \
    --xtrabackup-plugin-dir="${XB_CLIENT_PLUGIN_DIR}" \
    --user="${MYSQL_OIDC_USER}" \
    --password=whatever \
    --authentication-openid-connect-client-id-token-file="${TOKEN_FILE}" \
    --backup \
    --target-dir=${topdir}/backup_should_fail 2>&1 | \
        grep -F -- "--password and --authentication-openid-connect-client-id-token-file are mutually exclusive"

########################################################################
# Case 3: missing token file — client plugin's option() callback rejects
# the path, xb_mysql_connect() catches and errors out.
########################################################################
vlog "Case 3: missing ID-token file must fail"
run_cmd_expect_failure xtrabackup \
    --xtrabackup-plugin-dir="${XB_CLIENT_PLUGIN_DIR}" \
    --user="${MYSQL_OIDC_USER}" \
    --authentication-openid-connect-client-id-token-file="${topdir}/nonexistent_token.txt" \
    --backup \
    --target-dir=${topdir}/backup_should_fail2 2>&1 | \
        grep -F "Failed to set id-token-file"

########################################################################
# Cleanup
########################################################################
run_cmd ${MYSQL} ${MYSQL_ARGS} -e "
    DROP USER '${MYSQL_OIDC_USER}';
    UNINSTALL PLUGIN auth_openid_connect;
"
rm -f "${TOKEN_FILE}"
