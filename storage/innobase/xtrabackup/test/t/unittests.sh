#
# run the XtraBackup unit tests
#

for test in xbcloud-t xb_page_group-t; do
  if ! which $test > /dev/null 2>&1; then
    skip_test "$test is not built, configure with -DWITH_UNIT_TESTS=ON"
  fi
done

run_cmd xbcloud-t
run_cmd xb_page_group-t
