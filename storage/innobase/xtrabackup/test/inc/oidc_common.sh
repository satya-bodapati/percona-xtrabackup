############################################################################
# Common helpers for PXB OpenID Connect authentication tests.
#
# Sourced by t/oidc_backup.sh, t/oidc_backup_percona_idp.sh, and
# t/oidc_backup_keycloak.sh.  All functions assume `. inc/common.sh` has
# already been sourced by the caller.
#
# The functions here concentrate the awkward bits — installing the PS
# server plugin at a specific plugin_dir, computing SONAMEs, extracting
# the 'sub' claim from a JWT, and issuing CREATE USER … IDENTIFIED WITH
# 'auth_openid_connect'.  Docker/Keycloak lifecycle helpers live in
# inc/oidc_keycloak_docker.sh so tests that don't need Docker don't pull
# it in.
############################################################################

##########################################################################
# Skip the current test unless a Percona Server 8.4.10+ build has been
# provisioned and its auth_openid_connect.so path is exported in the
# AUTH_OIDC_SERVER_SO env var.  Also derives the plugin_dir and .so
# basename into globals for the caller.
##########################################################################
function oidc_require_server_plugin()
{
    if [ -z "${AUTH_OIDC_SERVER_SO:-}" ] || [ ! -f "${AUTH_OIDC_SERVER_SO}" ]; then
        skip_test "AUTH_OIDC_SERVER_SO not set or file missing;" \
                  "requires Percona Server 8.4.10+ with auth_openid_connect plugin"
    fi
    AUTH_OIDC_PLUGIN_DIR=$(dirname "${AUTH_OIDC_SERVER_SO}")
    AUTH_OIDC_SO_NAME=$(basename "${AUTH_OIDC_SERVER_SO}")
    export AUTH_OIDC_PLUGIN_DIR AUTH_OIDC_SO_NAME
}

##########################################################################
# INSTALL PLUGIN auth_openid_connect and SET GLOBAL
# auth_openid_connect_configuration.
# Args:
#   $1 - a config string, either FILE://... or JSON://... (server accepts both)
# Requires the server was started with --plugin-dir="${AUTH_OIDC_PLUGIN_DIR}".
##########################################################################
function oidc_install_and_configure_server_plugin()
{
    local config="$1"
    [ -n "${config}" ] || die "oidc_install_and_configure_server_plugin: config arg required"

    vlog "Installing auth_openid_connect server plugin"
    run_cmd ${MYSQL} ${MYSQL_ARGS} -e \
        "INSTALL PLUGIN auth_openid_connect SONAME '${AUTH_OIDC_SO_NAME}'"

    vlog "Configuring auth_openid_connect (${config:0:60}...)"
    run_cmd ${MYSQL} ${MYSQL_ARGS} -e \
        "SET GLOBAL auth_openid_connect_configuration = '${config}'"
}

##########################################################################
# CREATE USER … IDENTIFIED WITH 'auth_openid_connect' AS '{...}' and grant
# it the privileges xtrabackup needs for --backup.
#
# Args:
#   $1 - mysql user name (LHS of CREATE USER)
#   $2 - IdP-side subject (goes into the AS clause's "user" field; must
#        match the 'sub' claim in the ID token)
#   $3 - IdP name used inside the plugin config (default: "oidc-idp")
##########################################################################
function oidc_create_backup_user()
{
    local mysql_user="$1"
    local idp_sub="$2"
    local idp_name="${3:-oidc-idp}"
    [ -n "${mysql_user}" ] || die "oidc_create_backup_user: mysql user required"
    [ -n "${idp_sub}" ]    || die "oidc_create_backup_user: IdP subject required"

    vlog "Creating OIDC user ${mysql_user} (sub=${idp_sub}, idp=${idp_name})"
    run_cmd ${MYSQL} ${MYSQL_ARGS} -e "
        CREATE USER '${mysql_user}' IDENTIFIED WITH 'auth_openid_connect'
            AS '{\"identity_provider\":\"${idp_name}\",\"user\":\"${idp_sub}\"}';
        GRANT BACKUP_ADMIN, RELOAD, LOCK TABLES, PROCESS, REPLICATION CLIENT,
              SELECT ON *.* TO '${mysql_user}';
        GRANT SYSTEM_VARIABLES_ADMIN ON *.* TO '${mysql_user}';
        FLUSH PRIVILEGES;
    "
}

##########################################################################
# Extract the 'sub' claim from a JWT file.  Echoes it to stdout.
#
# The `sub` claim carries whatever identifier the IdP uses to identify
# the user internally.  For hand-rolled tokens produced by
# `create_id_token --sub <name>` this is the human name.  Real IdPs
# (Keycloak, Okta, Auth0) issue a UUID here — hence the need to extract
# it dynamically before CREATE USER.
#
# Uses `base64 -d` and `jq`; both are required prerequisites.
##########################################################################
function oidc_extract_sub()
{
    local token_file="$1"
    [ -s "${token_file}" ] || die "oidc_extract_sub: token file ${token_file} empty"

    local b64
    b64=$(awk -F. '{print $2}' "${token_file}")
    [ -n "${b64}" ] || die "oidc_extract_sub: could not split JWT"

    # Add base64 padding, translate URL-safe alphabet, decode, extract sub.
    local pad=$(( (4 - ${#b64} % 4) % 4 ))
    local sub
    sub=$(printf '%s%*s' "${b64}" "${pad}" '' | tr ' ' '=' | tr -- '-_' '+/' | \
          base64 -d 2>/dev/null | jq -er '.sub') \
        || die "oidc_extract_sub: failed to decode JWT or extract 'sub'"

    echo "${sub}"
}

##########################################################################
# Fetch an ID token from an OIDC token endpoint via ROPC (Resource Owner
# Password Credentials) grant.  ROPC is convenient for automated tests
# because it's non-interactive; never use it in production.
#
# Args:
#   $1 - token endpoint URL (e.g. http://…/protocol/openid-connect/token)
#   $2 - client_id
#   $3 - username
#   $4 - password
#   $5 - output file (the ID token JWT is written here)
##########################################################################
function oidc_fetch_ropc_token()
{
    local token_url="$1"
    local client_id="$2"
    local username="$3"
    local password="$4"
    local out_file="$5"
    [ -n "${token_url}" ] && [ -n "${client_id}" ] && \
    [ -n "${username}" ] && [ -n "${password}" ] && \
    [ -n "${out_file}" ] || die "oidc_fetch_ropc_token: 5 args required"

    if ! command -v curl >/dev/null 2>&1; then
        skip_test "curl not available; required to fetch an OIDC token"
    fi
    if ! command -v jq >/dev/null 2>&1; then
        skip_test "jq not available; required to parse the OIDC token response"
    fi

    vlog "Fetching ID token from ${token_url} for ${username}"
    curl --fail --silent --show-error --max-time 15 \
         -X POST "${token_url}" \
         -H "Content-Type: application/x-www-form-urlencoded" \
         -d "grant_type=password" \
         -d "client_id=${client_id}" \
         -d "username=${username}" \
         --data-urlencode "password=${password}" \
         -d "scope=openid" \
        | jq -er '.id_token' > "${out_file}" \
        || die "failed to fetch ID token from ${token_url}"

    [ -s "${out_file}" ] || die "fetched ID token file ${out_file} is empty"
}

##########################################################################
# Run an end-to-end backup → prepare → restore-and-verify pass while
# authenticating via an OIDC id-token file.  Fails via die() if any step
# doesn't behave.
#
# Args:
#   $1 - mysql user
#   $2 - id-token file path
#   $3 - backup target directory
#   $4 - --plugin-dir value for the restarted server after copy-back
#        (usually "${AUTH_OIDC_PLUGIN_DIR}")
#   $5 - a table to spot-check before/after (default: sakila.actor)
##########################################################################
function oidc_backup_prepare_restore_verify()
{
    local mysql_user="$1"
    local token_file="$2"
    local backup_dir="$3"
    local plugin_dir="$4"
    local sentinel_table="${5:-sakila.actor}"

    local src_count
    src_count=$(${MYSQL} ${MYSQL_ARGS} -Ns -e "SELECT COUNT(*) FROM ${sentinel_table}")
    [ "${src_count}" -gt 0 ] || \
        die "sentinel table ${sentinel_table} is empty; nothing to prove restore works"

    vlog "xtrabackup --backup as ${mysql_user} using OIDC id-token"
    xtrabackup \
        --user="${mysql_user}" \
        --authentication-openid-connect-client-id-token-file="${token_file}" \
        --backup \
        --target-dir="${backup_dir}"

    # Sanity: the info line must show up.  Otherwise something else
    # authenticated us and the test is a false positive.
    grep -q "Using OpenID Connect id-token-file" ${OUTFILE} || \
        die "expected 'Using OpenID Connect id-token-file' in xtrabackup log"

    vlog "xtrabackup --prepare"
    xtrabackup --prepare --target-dir="${backup_dir}"

    vlog "Restore and verify row count matches source"
    stop_server
    rm -rf ${mysql_datadir}
    xtrabackup --copy-back --target-dir="${backup_dir}"
    start_server --plugin-dir="${plugin_dir}"

    local dst_count
    dst_count=$(${MYSQL} ${MYSQL_ARGS} -Ns -e "SELECT COUNT(*) FROM ${sentinel_table}")
    [ "${dst_count}" = "${src_count}" ] || \
        die "restored ${sentinel_table} row count ${dst_count} != source ${src_count}"
}
