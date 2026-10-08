################################################################################
# PXB-3896: xbcloud does not retry synchronous requests
#
# Chunk uploads and downloads are retried, but the other requests were not:
# the md5 upload, the listings and the deletes. One transient error in any of
# them failed the whole put, get or delete.
#
# A proxy in front of the S3 endpoint (inc/xbcloud_fault_proxy.py) answers
# chosen requests with 503 twice and forwards everything else. Each operation
# must retry and succeed. A wrong endpoint must still fail at once.
################################################################################

. inc/xbcloud_common.sh
is_xbcloud_credentials_set

start_server

write_credentials

# path style, so that the request lines carry the backup name
opts="--defaults-file=$topdir/xbcloud.cnf --s3-bucket-lookup=path"
retry_opts="--parallel=4 --max-retries=5 --max-backoff=1000"
put_name=${full_backup_name}-put
list_name=${full_backup_name}-list

vlog "Take a backup"
xtrabackup --backup --stream=xbstream --target-dir=$topdir/backup \
  > $topdir/backup.xbs

failed=""
function check() {
  local what=$1 rc=$2 log=$3
  local retries
  retries=$(grep -c "before retrying" $log || true)
  vlog "$what: exit code $rc, retries: $retries"
  if [ $rc -ne 0 ] || [ $retries -eq 0 ]; then
    grep -v "successfully uploaded\|Download successfull" $log | tail -5
    failed="$failed '$what'"
  fi
}

vlog "put --md5, the md5 upload gets 503 twice"
start_fault_proxy --match "^PUT [^ ]*/${put_name}\.md5 " --count 2
rc=0
xbcloud $opts --s3-endpoint=$fault_proxy_endpoint put --md5 $retry_opts \
  $put_name < $topdir/backup.xbs 2> $topdir/put.log || rc=$?
check "md5 upload" $rc $topdir/put.log

vlog "put, the final listing gets 503 twice"
start_fault_proxy --match "^GET [^ ]*prefix=${list_name}" --skip 1 --count 2
rc=0
xbcloud $opts --s3-endpoint=$fault_proxy_endpoint put $retry_opts \
  $list_name < $topdir/backup.xbs 2> $topdir/put_list.log || rc=$?
check "final listing" $rc $topdir/put_list.log

vlog "get, the listing gets 503 twice"
start_fault_proxy --match "^GET [^ ]*prefix=${put_name}" --count 2
mkdir $topdir/downloaded
rc=0
xbcloud $opts --s3-endpoint=$fault_proxy_endpoint get $retry_opts \
  $put_name 2> $topdir/get.log > $topdir/downloaded.xbs || rc=$?
check "get listing" $rc $topdir/get.log

vlog "delete, two object deletes get 503 twice"
start_fault_proxy --match "^DELETE [^ ]*/${put_name}/" --skip 1 --count 2
rc=0
xbcloud $opts --s3-endpoint=$fault_proxy_endpoint delete $retry_opts \
  $put_name 2> $topdir/delete.log || rc=$?
check "delete" $rc $topdir/delete.log

vlog "A wrong endpoint fails at once"
start_fault_proxy --match "NO REQUEST MATCHES"
dead_endpoint=$fault_proxy_endpoint
kill $fault_proxy_pid
wait $fault_proxy_pid 2>/dev/null || true
fault_proxy_pid=
start=$(date +%s)
rc=0
xbcloud $opts --s3-endpoint=$dead_endpoint put $retry_opts \
  ${full_backup_name}-dead < $topdir/backup.xbs 2> $topdir/dead.log || rc=$?
took=$(( $(date +%s) - start ))
vlog "Wrong endpoint: exit code $rc after ${took}s"
if [ $rc -eq 0 ] || [ $took -gt 10 ]; then
  failed="$failed 'wrong endpoint'"
fi

xbcloud $opts delete --parallel=4 $list_name || true

if [ -n "$failed" ]; then
  xbcloud $opts delete --parallel=4 $put_name || true
  die "Not retried or not failing fast:$failed"
fi

vlog "Prepare the downloaded backup"
xbstream -x -C $topdir/downloaded < $topdir/downloaded.xbs
xtrabackup --prepare --target-dir=$topdir/downloaded
