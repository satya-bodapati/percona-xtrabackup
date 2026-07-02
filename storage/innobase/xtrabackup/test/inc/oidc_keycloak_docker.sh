############################################################################
# Docker-based Keycloak bootstrap for PXB OIDC tests.
#
# When sourced by a test with OIDC_BOOTSTRAP_KEYCLOAK=1, this script:
#   1. Skips if `docker` isn't on PATH.
#   2. Starts a Keycloak container (image below) exposing 8080.
#   3. Provisions a realm/client/user via kcadm.sh inside the container.
#   4. Fetches an ID token for the user via ROPC.
#   5. Exports the KEYCLOAK_* env vars the test expects.
#   6. Installs an EXIT trap so the container is torn down when the test
#      script exits (pass, fail, or interrupted).
#
# When OIDC_BOOTSTRAP_KEYCLOAK is unset, this file does nothing beyond
# expose its helper functions — the test then relies on an
# externally-provisioned Keycloak, matching how xbcloud tests treat
# externally-provisioned MinIO/Azurite/etc.
#
# Recommended for Jenkins:
#   * Either run this file's setup once at the start of the Jenkins job
#     and export env vars for all downstream tests, OR
#   * Set OIDC_BOOTSTRAP_KEYCLOAK=1 in the job env so every test that
#     sources this file gets an isolated container.
#
# The Keycloak image is the official upstream community build under
# Apache 2.0:  https://www.keycloak.org
############################################################################

# Override any of these before sourcing to customise the bootstrapped
# container.  Defaults are chosen to be low-conflict with existing
# containers on developer laptops.
OIDC_KEYCLOAK_IMAGE=${OIDC_KEYCLOAK_IMAGE:-quay.io/keycloak/keycloak:26.0}
OIDC_KEYCLOAK_CONTAINER=${OIDC_KEYCLOAK_CONTAINER:-pxb-oidc-keycloak}
OIDC_KEYCLOAK_PORT=${OIDC_KEYCLOAK_PORT:-8080}
OIDC_KEYCLOAK_ADMIN=${OIDC_KEYCLOAK_ADMIN:-admin}
OIDC_KEYCLOAK_ADMIN_PASSWORD=${OIDC_KEYCLOAK_ADMIN_PASSWORD:-admin}
OIDC_KEYCLOAK_REALM=${OIDC_KEYCLOAK_REALM:-pxb}
OIDC_KEYCLOAK_CLIENT=${OIDC_KEYCLOAK_CLIENT:-pxb-xtrabackup}
OIDC_KEYCLOAK_USER=${OIDC_KEYCLOAK_USER:-xtrabackup-oidc}
OIDC_KEYCLOAK_PASSWORD=${OIDC_KEYCLOAK_PASSWORD:-pxb-test}
OIDC_KEYCLOAK_READY_TIMEOUT=${OIDC_KEYCLOAK_READY_TIMEOUT:-90}

##########################################################################
# Start a Keycloak container in dev mode and wait for it to become
# reachable.  Sets OIDC_KEYCLOAK_BASE_URL on success.
##########################################################################
function oidc_keycloak_docker_start()
{
    if ! command -v docker >/dev/null 2>&1; then
        skip_test "docker not available; cannot bootstrap Keycloak"
    fi

    # If a previous run left a container behind, get rid of it before
    # binding to the same port again.
    docker rm -f "${OIDC_KEYCLOAK_CONTAINER}" >/dev/null 2>&1 || true

    vlog "Starting Keycloak container ${OIDC_KEYCLOAK_CONTAINER} from ${OIDC_KEYCLOAK_IMAGE}"
    run_cmd docker run --rm -d \
        --name "${OIDC_KEYCLOAK_CONTAINER}" \
        -p "${OIDC_KEYCLOAK_PORT}:8080" \
        -e KC_BOOTSTRAP_ADMIN_USERNAME="${OIDC_KEYCLOAK_ADMIN}" \
        -e KC_BOOTSTRAP_ADMIN_PASSWORD="${OIDC_KEYCLOAK_ADMIN_PASSWORD}" \
        "${OIDC_KEYCLOAK_IMAGE}" \
        start-dev >/dev/null

    OIDC_KEYCLOAK_BASE_URL="http://localhost:${OIDC_KEYCLOAK_PORT}"
    export OIDC_KEYCLOAK_BASE_URL

    # Poll the OIDC discovery doc until Keycloak is ready to serve.
    local waited=0
    while true; do
        if curl --fail --silent --show-error --max-time 3 --output /dev/null \
           "${OIDC_KEYCLOAK_BASE_URL}/realms/master/.well-known/openid-configuration"; then
            break
        fi
        waited=$((waited + 2))
        if [ ${waited} -ge ${OIDC_KEYCLOAK_READY_TIMEOUT} ]; then
            docker logs "${OIDC_KEYCLOAK_CONTAINER}" >&2 || true
            die "Keycloak did not become ready within ${OIDC_KEYCLOAK_READY_TIMEOUT}s"
        fi
        sleep 2
    done
    vlog "Keycloak ready at ${OIDC_KEYCLOAK_BASE_URL} after ${waited}s"

    # Arrange for teardown even on test failure or interrupt.
    trap 'oidc_keycloak_docker_stop' EXIT INT TERM
}

##########################################################################
# Stop and remove the Keycloak container.  Safe to call multiple times.
##########################################################################
function oidc_keycloak_docker_stop()
{
    if command -v docker >/dev/null 2>&1; then
        docker rm -f "${OIDC_KEYCLOAK_CONTAINER}" >/dev/null 2>&1 || true
    fi
}

##########################################################################
# Provision a realm, a public client with direct-grant enabled (for
# ROPC), a user, and an audience mapper so the ID token carries the
# client_id in its 'aud' claim.
##########################################################################
function oidc_keycloak_docker_provision()
{
    local kcadm="docker exec ${OIDC_KEYCLOAK_CONTAINER} /opt/keycloak/bin/kcadm.sh"

    vlog "Authenticating kcadm to Keycloak master realm"
    run_cmd bash -c "${kcadm} config credentials \
        --server http://localhost:8080 \
        --realm master \
        --user '${OIDC_KEYCLOAK_ADMIN}' \
        --password '${OIDC_KEYCLOAK_ADMIN_PASSWORD}'"

    vlog "Creating realm ${OIDC_KEYCLOAK_REALM}"
    run_cmd bash -c "${kcadm} create realms \
        -s realm='${OIDC_KEYCLOAK_REALM}' \
        -s enabled=true"

    vlog "Creating public client ${OIDC_KEYCLOAK_CLIENT} with directAccessGrants"
    run_cmd bash -c "${kcadm} create clients \
        -r '${OIDC_KEYCLOAK_REALM}' \
        -s clientId='${OIDC_KEYCLOAK_CLIENT}' \
        -s publicClient=true \
        -s directAccessGrantsEnabled=true \
        -s standardFlowEnabled=false \
        -s 'redirectUris=[]'"

    vlog "Creating user ${OIDC_KEYCLOAK_USER}"
    run_cmd bash -c "${kcadm} create users \
        -r '${OIDC_KEYCLOAK_REALM}' \
        -s username='${OIDC_KEYCLOAK_USER}' \
        -s enabled=true"
    run_cmd bash -c "${kcadm} set-password \
        -r '${OIDC_KEYCLOAK_REALM}' \
        --username '${OIDC_KEYCLOAK_USER}' \
        --new-password '${OIDC_KEYCLOAK_PASSWORD}'"

    vlog "Adding audience mapper so ID tokens carry aud=${OIDC_KEYCLOAK_CLIENT}"
    local client_iid
    client_iid=$(docker exec "${OIDC_KEYCLOAK_CONTAINER}" \
        /opt/keycloak/bin/kcadm.sh get clients \
        -r "${OIDC_KEYCLOAK_REALM}" \
        -q clientId="${OIDC_KEYCLOAK_CLIENT}" \
        --fields id --format csv --noquotes 2>/dev/null | tail -1)
    [ -n "${client_iid}" ] || die "could not resolve internal id of client ${OIDC_KEYCLOAK_CLIENT}"

    run_cmd bash -c "${kcadm} create clients/${client_iid}/protocol-mappers/models \
        -r '${OIDC_KEYCLOAK_REALM}' \
        -s name=aud-mapper \
        -s protocol=openid-connect \
        -s protocolMapper=oidc-audience-mapper \
        -s 'config.\"included.client.audience\"=${OIDC_KEYCLOAK_CLIENT}' \
        -s 'config.\"id.token.claim\"=true' \
        -s 'config.\"access.token.claim\"=false'"
}

##########################################################################
# Convenience: run start + provision, then export the KEYCLOAK_* vars the
# tests read.  Callers just:
#
#     . inc/oidc_keycloak_docker.sh
#     oidc_keycloak_docker_bootstrap
#
# and are ready to invoke the rest of the OIDC test flow.  ID token is
# written to $1 (defaults to ${TEST_VAR_ROOT}/keycloak_id_token.txt) so
# that KEYCLOAK_ID_TOKEN points at it.
##########################################################################
function oidc_keycloak_docker_bootstrap()
{
    local token_file="${1:-${TEST_VAR_ROOT}/keycloak_id_token.txt}"

    oidc_keycloak_docker_start
    oidc_keycloak_docker_provision

    local realm_url="${OIDC_KEYCLOAK_BASE_URL}/realms/${OIDC_KEYCLOAK_REALM}"
    KEYCLOAK_ISSUER="${realm_url}"
    KEYCLOAK_JWKS_URL="${realm_url}/protocol/openid-connect/certs"
    KEYCLOAK_AUDIENCE="${OIDC_KEYCLOAK_CLIENT}"
    KEYCLOAK_MYSQL_USER="${OIDC_KEYCLOAK_USER}"
    KEYCLOAK_ID_TOKEN="${token_file}"
    export KEYCLOAK_ISSUER KEYCLOAK_JWKS_URL KEYCLOAK_AUDIENCE \
           KEYCLOAK_MYSQL_USER KEYCLOAK_ID_TOKEN

    oidc_fetch_ropc_token \
        "${realm_url}/protocol/openid-connect/token" \
        "${OIDC_KEYCLOAK_CLIENT}" \
        "${OIDC_KEYCLOAK_USER}" \
        "${OIDC_KEYCLOAK_PASSWORD}" \
        "${token_file}"
}

# If the caller opts into auto-bootstrap, do it now.  The test then only
# needs to reference KEYCLOAK_* vars; nothing extra to invoke.
if [ "${OIDC_BOOTSTRAP_KEYCLOAK:-0}" = "1" ]; then
    oidc_keycloak_docker_bootstrap
fi
