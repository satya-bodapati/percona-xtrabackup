################################################################################
# PXB-3895: xbcloud does not retry connection failures, proxy errors and
# throttling by default
#
# A proxy in front of the S3 endpoint (inc/xbcloud_fault_proxy.py) breaks the
# upload for 5 seconds in three ways. With the default retriable errors and
# no extra options, xbcloud must retry each one with backoff and finish:
#
#   refuse    the proxy goes away, connections are refused (curl error 7)
#   http 502  the proxy answers Bad Gateway, its backend is down
#   http 429  the service throttles with Too Many Requests
################################################################################

. inc/xbcloud_common.sh
is_xbcloud_credentials_set

start_server

write_credentials

vlog "Create about 64MB of table data"
mysql -e "CREATE TABLE t (id INT AUTO_INCREMENT PRIMARY KEY, b LONGBLOB)" test
mysql -e "INSERT INTO t (b) VALUES (REPEAT('a', 1048576))" test
for i in $(seq 1 6); do
  mysql -e "INSERT INTO t (b) SELECT b FROM t" test
done

failed=""
backups=""
all_names=""
for fault in "refuse 0" "http 502" "http 429"; do
  read -r mode status <<< "$fault"
  name=${full_backup_name}-${mode}-${status}
  all_names="$all_names $name"

  vlog "Fault $mode $status: proxy breaks the upload after 20MB for 5 seconds"
  start_fault_proxy --mode $mode --status $status \
    --after-bytes $((20 * 1024 * 1024)) --duration 5

  if xtrabackup --backup --stream=xbstream --target-dir=$topdir/backup | \
       xbcloud --defaults-file=$topdir/xbcloud.cnf put \
         --s3-endpoint=$fault_proxy_endpoint \
         --parallel=4 --max-retries=10 --max-backoff=2000 \
         $name 2> $topdir/put.log; then
    result=ok
  else
    result=failed
  fi
  rm -rf $topdir/backup

  grep -q "fault $mode" $topdir/fault_proxy.log || \
    die "Fault $mode $status: the proxy never injected the fault"
  retries=$(grep -c "before retrying" $topdir/put.log || true)
  vlog "Fault $mode $status: upload $result, retries: $retries"

  if [ $result = ok ] && [ $retries -gt 0 ]; then
    backups="$backups $name"
  else
    grep -v "successfully uploaded" $topdir/put.log | tail -5
    failed="$failed '$mode $status'"
  fi
done

if [ -n "$failed" ]; then
  for name in $all_names; do
    xbcloud --defaults-file=$topdir/xbcloud.cnf delete --parallel=4 $name || true
  done
  die "Upload did not recover from:$failed"
fi

vlog "Download and prepare the backups"
for name in $backups; do
  rm -rf $topdir/downloaded
  mkdir $topdir/downloaded
  run_cmd xbcloud --defaults-file=$topdir/xbcloud.cnf get --parallel=4 \
    $name | xbstream -x -C $topdir/downloaded
  xtrabackup --prepare --target-dir=$topdir/downloaded
  run_cmd xbcloud --defaults-file=$topdir/xbcloud.cnf delete --parallel=4 $name
done
