########################################################################
# Test that --prepare --export generates valid .sql schema files
# from SDI (Serialized Dictionary Information) in InnoDB tablespaces.
#
# Verification approach:
#   1. Create table on server
#   2. mysqldump --no-data to capture original schema
#   3. Backup + prepare --export (generates .sql from SDI)
#   4. Drop table, recreate from generated .sql
#   5. mysqldump --no-data to capture recreated schema
#   6. diff the two mysqldump outputs
########################################################################

. inc/common.sh

start_server

# Create test tables exercising features the current code handles:
# - NOT NULL / NULL columns
# - AUTO_INCREMENT
# - PRIMARY KEY, secondary KEY
# - Multiple column types

mysql -e "CREATE TABLE t1 (
  a INT NOT NULL AUTO_INCREMENT,
  b INT NOT NULL,
  c VARCHAR(100),
  PRIMARY KEY (a),
  KEY idx_b (b)
) ENGINE=InnoDB" test

# Capture original schema
${MYSQLDUMP} ${MYSQL_ARGS} --no-data --skip-comments --skip-dump-date \
  test t1 > $topdir/original_schema.sql

# Backup
xtrabackup --backup --target-dir=$topdir/backup

# Prepare with --export to generate .sql files from SDI
xtrabackup --prepare --export --target-dir=$topdir/backup

# Verify the .sql file was generated
if [ ! -f "$topdir/backup/test/t1.sql" ]; then
    vlog "ERROR: t1.sql was not generated in backup directory"
    exit -1
fi

vlog "Generated t1.sql contents:"
cat $topdir/backup/test/t1.sql >&2

# Drop original table and recreate from generated .sql
mysql -e "DROP TABLE t1" test
mysql test < $topdir/backup/test/t1.sql

# Capture schema of the recreated table
${MYSQLDUMP} ${MYSQL_ARGS} --no-data --skip-comments --skip-dump-date \
  test t1 > $topdir/recreated_schema.sql

# Compare: if generated DDL is correct, both schemas must be identical
run_cmd diff -u $topdir/original_schema.sql $topdir/recreated_schema.sql

vlog "generate_schema: schema roundtrip test passed"
