################################################################################
# PXB-3895: xbcloud retries keep using the old address of a host that moved
#
# xbcloud reaches the S3 endpoint through a host name, as with a proxy or load
# balancer. In the middle of the upload the backend behind the name goes away
# and the name moves to a new address. curl caches addresses for 60 seconds.
# Retries must look the name up again instead of trying the old address until
# the cache expires: the new address must be looked up within 30 seconds and
# the backup, including its final listing, must succeed.
#
# The name exists only inside xbcloud: inc/xbcloud_fake_dns.c is preloaded and
# answers lookups of it from a file the test rewrites. The two backends are
# inc/xbcloud_fault_proxy.py on 127.0.0.2 and 127.0.0.3.
################################################################################

. inc/xbcloud_common.sh
is_xbcloud_credentials_set

if ! command -v cc > /dev/null; then
  skip_test "Requires a C compiler to build inc/xbcloud_fake_dns.c"
fi
if ldd $(command -v xbcloud) | grep -q libasan; then
  skip_test "LD_PRELOAD of inc/xbcloud_fake_dns.c does not work with ASAN"
fi
upstream=$(fault_proxy_upstream)
if [ -z "$upstream" ]; then
  skip_test "Requires a plain http --s3-endpoint in XBCLOUD_CREDENTIALS"
fi

start_server

write_credentials

cc -shared -fPIC -o $topdir/fake_dns.so inc/xbcloud_fake_dns.c -ldl || \
  die "Cannot build inc/xbcloud_fake_dns.c"
echo 127.0.0.2 > $topdir/fake_dns_ip

function wait_for_file() {
  for i in $(seq 1 50); do
    [ -s $1 ] && return
    sleep 0.1
  done
  die "$1 was not created"
}

vlog "Backend A on 127.0.0.2 goes away after 20MB, backend B on 127.0.0.3"
python3 inc/xbcloud_fault_proxy.py --bind 127.0.0.2 --upstream $upstream \
  --port-file $topdir/port_a --mode refuse \
  --after-bytes $((20 * 1024 * 1024)) --duration 3600 > $topdir/proxy_a.log 2>&1 &
pid_a=$!
wait_for_file $topdir/port_a
port=$(cat $topdir/port_a)
python3 inc/xbcloud_fault_proxy.py --bind 127.0.0.3 --port $port \
  --upstream $upstream --port-file $topdir/port_b > $topdir/proxy_b.log 2>&1 &
pid_b=$!
wait_for_file $topdir/port_b

# When backend A goes away, the name moves to backend B
(
  until grep -q "fault refuse start" $topdir/proxy_a.log; do
    sleep 0.05
  done
  echo 127.0.0.3 > $topdir/fake_dns_ip
  date +%s > $topdir/moved_at
  echo "$(date +%T) s3lb.test moved to 127.0.0.3" >> $topdir/fake_dns.log
) &
pid_mover=$!
trap "kill $pid_a $pid_b $pid_mover 2>/dev/null || true" EXIT

vlog "Create about 64MB of table data"
mysql -e "CREATE TABLE t (id INT AUTO_INCREMENT PRIMARY KEY, b LONGBLOB)" test
mysql -e "INSERT INTO t (b) VALUES (REPEAT('a', 1048576))" test
for i in $(seq 1 6); do
  mysql -e "INSERT INTO t (b) SELECT b FROM t" test
done

vlog "Upload through s3lb.test, with up to 5 retries of 1 second each"
if xtrabackup --backup --stream=xbstream --target-dir=$topdir/backup | \
     FAKE_DNS_NAME=s3lb.test FAKE_DNS_FILE=$topdir/fake_dns_ip \
     FAKE_DNS_LOG=$topdir/fake_dns.log LD_PRELOAD=$topdir/fake_dns.so \
     xbcloud --defaults-file=$topdir/xbcloud.cnf put \
       --s3-endpoint=http://s3lb.test:$port/ --s3-bucket-lookup=path \
       --parallel=4 --max-retries=5 --max-backoff=1000 \
       ${full_backup_name} 2> $topdir/put.log; then
  result=ok
else
  result=failed
fi

grep -q "fault refuse start" $topdir/proxy_a.log || \
  die "Backend A never went away"
vlog "Name lookups:"
cat $topdir/fake_dns.log
retries=$(grep -c "before retrying" $topdir/put.log || true)
vlog "Upload $result, retries: $retries"

if [ $result != ok ]; then
  grep -v "successfully uploaded" $topdir/put.log | tail -5
  xbcloud --defaults-file=$topdir/xbcloud.cnf delete --parallel=4 \
    ${full_backup_name} || true
  die "Upload did not recover after s3lb.test moved to a new address"
fi

# The shim logs HH:MM:SS.mmm; take the first lookup of the new address.
first_new=$(grep -m1 "s3lb.test -> 127.0.0.3" $topdir/fake_dns.log | cut -c1-8)
[ -n "$first_new" ] || die "xbcloud never looked up the new address"
delay=$(( $(date -d "$first_new" +%s) - $(cat $topdir/moved_at) ))
vlog "New address looked up $delay seconds after the move"
if [ $delay -gt 30 ]; then
  die "Retries kept using the old address for $delay seconds"
fi

vlog "Download and prepare the backup"
mkdir $topdir/downloaded
run_cmd xbcloud --defaults-file=$topdir/xbcloud.cnf get --parallel=4 \
  ${full_backup_name} | xbstream -x -C $topdir/downloaded
xtrabackup --prepare --target-dir=$topdir/downloaded
run_cmd xbcloud --defaults-file=$topdir/xbcloud.cnf delete --parallel=4 \
  ${full_backup_name}
