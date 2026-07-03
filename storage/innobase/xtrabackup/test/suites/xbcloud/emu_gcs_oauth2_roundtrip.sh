############################################################################
# xbcloud round-trip against fake-gcs-server using --google-service-
# account-file (OAuth2 Bearer path, PXB-3592).
#
# fake-gcs-server doesn't cryptographically validate the Bearer
# token — it accepts any bearer — so this test exercises the wire
# path (S3 XML API + Authorization: Bearer <...> header) without
# needing a JWT-validating mock IdP.  To exercise real JWT flow we
# would need to start our own OAuth2 token endpoint that hands back
# a signed access_token; deliberately out of scope for a local test.
#
# We do the following:
#   1. Generate a synthetic service-account JSON in-tree.  The
#      "private_key" is a real RSA key so xbcloud's OAuth2 sign
#      step doesn't crash on parse, and we set token_uri to a
#      local endpoint that returns a canned access_token.
#   2. Start a tiny Python HTTP responder on 127.0.0.1:<port> that
#      answers with { "access_token": "fake", "expires_in": 3600 }
#      for any POST.  This is our OAuth2 token endpoint substitute.
#   3. Run xbcloud put with --google-service-account-file pointing
#      at our synthetic JSON.  Verify the round-trip completes.
############################################################################

. inc/common.sh
. inc/cloud_emu.sh

cloud_emu_require_docker
command -v python3 >/dev/null 2>&1 || skip_test "test requires python3"
command -v openssl >/dev/null 2>&1 || skip_test "test requires openssl"

cloud_emu_start
trap 'cloud_emu_stop; kill $token_srv_pid 2>/dev/null' EXIT
cloud_emu_wait_for gcs

bucket="pxb-gcs-oauth-$(date +%s)-$$"
cloud_emu_make_bucket gcs "$bucket"

# ---------------------------------------------------------------
# Set up the OAuth2 token endpoint responder.  Picks a free port,
# writes a canned service_account.json referencing that port, and
# starts a python3 http.server subclass that answers /token with a
# fake access_token.
# ---------------------------------------------------------------
sa_dir=$topdir/sa
mkdir -p "$sa_dir"

# Real RSA key so xbcloud's PEM parse and RS256 sign both succeed
# in production code even though the signature isn't verified by
# our responder.
openssl genrsa -out "$sa_dir/key.pem" 2048 >/dev/null 2>&1

token_port=$(python3 -c "import socket; s=socket.socket(); s.bind(('',0)); print(s.getsockname()[1]); s.close()")
token_uri="http://127.0.0.1:$token_port/token"

# Escape newlines in the private key for JSON embedding.
private_key_json=$(awk '{printf "%s\\n", $0}' "$sa_dir/key.pem")

cat > "$sa_dir/sa.json" <<EOF
{
  "type": "service_account",
  "client_email": "pxb-emu@example.iam.gserviceaccount.com",
  "private_key": "$private_key_json",
  "token_uri": "$token_uri"
}
EOF

# Tiny OAuth2 token responder — always returns a fixed access_token.
python3 -c "
import http.server, socketserver, json, sys
class H(http.server.BaseHTTPRequestHandler):
    def log_message(self, *args, **kw): pass
    def do_POST(self):
        length = int(self.headers.get('Content-Length', 0))
        self.rfile.read(length)
        body = json.dumps({'access_token':'fake-token', 'expires_in':3600, 'token_type':'Bearer'})
        self.send_response(200)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body.encode())
with socketserver.TCPServer(('127.0.0.1', $token_port), H) as srv:
    srv.serve_forever()
" &
token_srv_pid=$!

# Wait for the responder to be listening.
for i in 1 2 3 4 5; do
  if curl -sf -o /dev/null -X POST "$token_uri" 2>/dev/null; then break; fi
  sleep 1
done

# ---------------------------------------------------------------
# Backup round-trip using --google-service-account-file.
# ---------------------------------------------------------------
start_server --innodb_file_per_table
load_dbase_schema sakila
load_dbase_data sakila

src_count=$(${MYSQL} ${MYSQL_ARGS} -Ns -e "SELECT COUNT(*) FROM sakila.actor")
[ "$src_count" -gt 0 ] || die "sakila.actor is empty on the source"

vlog "Full backup → GCS with OAuth2 ADC (--google-service-account-file)"
full_dir=$topdir/full
mkdir -p "$full_dir"
xtrabackup --backup --stream=xbstream --extra-lsndir="$full_dir" \
    --target-dir="$full_dir" \
    | run_cmd xbcloud put --parallel=4 \
        --storage=google \
        --google-endpoint="$CLOUD_EMU_GCS_ENDPOINT" \
        --google-bucket="$bucket" \
        --google-region=auto \
        --google-service-account-file="$sa_dir/sa.json" \
        "$bucket/full"

vlog "Download → xbstream extract"
dl=$topdir/downloaded
mkdir -p "$dl"
run_cmd xbcloud get --parallel=4 \
    --storage=google \
    --google-endpoint="$CLOUD_EMU_GCS_ENDPOINT" \
    --google-bucket="$bucket" \
    --google-region=auto \
    --google-service-account-file="$sa_dir/sa.json" \
    "$bucket/full" \
    | xbstream -xv -C "$dl" --parallel=4

xtrabackup --prepare --target-dir="$dl"

stop_server
rm -rf ${mysql_datadir}
xtrabackup --copy-back --target-dir="$dl"
start_server --innodb_file_per_table

dst_count=$(${MYSQL} ${MYSQL_ARGS} -Ns -e "SELECT COUNT(*) FROM sakila.actor")
[ "$dst_count" = "$src_count" ] || \
    die "restored row count $dst_count != source $src_count"

vlog "GCS OAuth2 round-trip PASSED (row count $src_count preserved)"
