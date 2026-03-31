########################################################################
# Test ibd2sdi --create-sql: generates CREATE TABLE SQL from SDI JSON
# directly parsed from .ibd files using pure RapidJSON (no dd::deserialize).
#
# Strategy:
#   1. Start server, create diverse tables
#   2. Capture reference schemas via mysqldump --no-data
#   3. Shutdown server cleanly (ensures .ibd files are flushed)
#   4. Run ibd2sdi --create-sql on each .ibd file
#   5. Start server, drop all tables, replay generated .sql
#   6. Capture schemas again via mysqldump
#   7. Compare: reference vs regenerated must match
########################################################################

. inc/common.sh

IBD2SDI=$(dirname "$(which xtrabackup)")/ibd2sdi

TABLES=""
PARTITION_TABLES=""

function add_test_table() {
    local tbl=$1
    TABLES="$TABLES $tbl"
}

function add_partition_table() {
    local tbl=$1
    local ibd=$2
    TABLES="$TABLES $tbl"
    PARTITION_TABLES="$PARTITION_TABLES $tbl"
    eval "PART_IBD_${tbl}=${ibd}"
}

start_server

DATADIR=$(${MYSQL} ${MYSQL_ARGS} -N -B -e "SELECT @@datadir")

# =====================================================================
# Create test tables
# =====================================================================

# t1: basic columns, AUTO_INCREMENT, PRIMARY KEY, secondary KEY
mysql -e "CREATE TABLE t1 (
  a INT NOT NULL AUTO_INCREMENT,
  b INT NOT NULL,
  c VARCHAR(100),
  PRIMARY KEY (a),
  KEY idx_b (b)
) ENGINE=InnoDB" test
add_test_table t1

# t2: generated columns (virtual and stored)
mysql -e "CREATE TABLE t2 (
  a INT NOT NULL,
  b INT GENERATED ALWAYS AS (a * 2) VIRTUAL,
  c INT GENERATED ALWAYS AS (a + 1) STORED,
  PRIMARY KEY (a)
) ENGINE=InnoDB" test
add_test_table t2

# t3: DEFAULT values - literals, CURRENT_TIMESTAMP, expressions, ON UPDATE
mysql -e "CREATE TABLE t3 (
  id INT NOT NULL AUTO_INCREMENT,
  d_int INT DEFAULT 42,
  d_bigint BIGINT DEFAULT 0,
  d_float FLOAT DEFAULT 3.14,
  d_varchar VARCHAR(50) DEFAULT 'hello world',
  d_ts TIMESTAMP DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
  d_dt DATETIME(3) DEFAULT CURRENT_TIMESTAMP(3) ON UPDATE CURRENT_TIMESTAMP(3),
  d_nullable INT DEFAULT NULL,
  d_expr INT DEFAULT (d_int * 2),
  PRIMARY KEY (id)
) ENGINE=InnoDB" test
add_test_table t3

# t4: column COMMENT and GEOMETRY SRID
mysql -e "CREATE TABLE t4 (
  id INT NOT NULL AUTO_INCREMENT COMMENT 'primary key',
  name VARCHAR(100) COMMENT 'user name',
  location POINT NOT NULL SRID 4326,
  plain_geom POINT NOT NULL,
  PRIMARY KEY (id)
) ENGINE=InnoDB" test
add_test_table t4

# t5: index features - prefix length, DESC, functional index
mysql -e "CREATE TABLE t5 (
  id INT NOT NULL AUTO_INCREMENT,
  name VARCHAR(255) NOT NULL,
  data TEXT,
  a INT NOT NULL,
  b INT NOT NULL,
  PRIMARY KEY (id),
  KEY idx_prefix (name(50)),
  KEY idx_desc (a DESC, b),
  KEY idx_func ((a + b)),
  KEY idx_combo (name(20), a DESC)
) ENGINE=InnoDB" test
add_test_table t5

# t6: index options - algorithm, visibility, comment
mysql -e "CREATE TABLE t6 (
  id INT NOT NULL AUTO_INCREMENT,
  a INT NOT NULL,
  b INT NOT NULL,
  PRIMARY KEY (id),
  KEY idx_btree (a) USING BTREE,
  KEY idx_invisible (b) /*!80000 INVISIBLE */,
  KEY idx_comment (a, b) COMMENT 'composite index'
) ENGINE=InnoDB" test
add_test_table t6

# t7_parent + t7: foreign key constraints
mysql -e "CREATE TABLE t7_parent (
  id INT NOT NULL AUTO_INCREMENT,
  code VARCHAR(10) NOT NULL,
  PRIMARY KEY (id),
  UNIQUE KEY uk_code (code)
) ENGINE=InnoDB" test
add_test_table t7_parent

mysql -e "CREATE TABLE t7 (
  id INT NOT NULL AUTO_INCREMENT,
  parent_id INT NOT NULL,
  parent_code VARCHAR(10),
  PRIMARY KEY (id),
  KEY idx_parent (parent_id),
  KEY idx_code (parent_code),
  CONSTRAINT fk_parent FOREIGN KEY (parent_id) REFERENCES t7_parent (id) ON DELETE CASCADE ON UPDATE CASCADE,
  CONSTRAINT fk_code FOREIGN KEY (parent_code) REFERENCES t7_parent (code) ON DELETE SET NULL
) ENGINE=InnoDB" test
add_test_table t7

# t8: check constraints (enforced and not enforced)
mysql -e "CREATE TABLE t8 (
  id INT NOT NULL AUTO_INCREMENT,
  age INT NOT NULL,
  status ENUM('active','inactive') NOT NULL DEFAULT 'active',
  PRIMARY KEY (id),
  CONSTRAINT chk_age CHECK (age >= 0 AND age <= 150),
  CONSTRAINT chk_status CHECK (status IN ('active','inactive')) /*!80016 NOT ENFORCED */
) ENGINE=InnoDB" test
add_test_table t8

# t9: table options - ROW_FORMAT, KEY_BLOCK_SIZE, COMMENT, STATS_*
mysql -e "CREATE TABLE t9 (
  id INT NOT NULL AUTO_INCREMENT,
  data TEXT,
  PRIMARY KEY (id)
) ENGINE=InnoDB ROW_FORMAT=COMPRESSED KEY_BLOCK_SIZE=8
  STATS_PERSISTENT=1 STATS_AUTO_RECALC=0 STATS_SAMPLE_PAGES=100
  COMMENT='compressed table'" test
add_test_table t9

# t10: FULLTEXT index and SPATIAL index
mysql -e "CREATE TABLE t10 (
  id INT NOT NULL AUTO_INCREMENT,
  title VARCHAR(200),
  body TEXT,
  location POINT NOT NULL SRID 0,
  PRIMARY KEY (id),
  FULLTEXT KEY ft_body (body),
  SPATIAL KEY sp_loc (location)
) ENGINE=InnoDB" test
add_test_table t10

# t11: RANGE partitioning
mysql -e "CREATE TABLE t11 (
  id INT NOT NULL,
  created DATE NOT NULL,
  PRIMARY KEY (id, created)
) ENGINE=InnoDB
PARTITION BY RANGE (YEAR(created)) (
  PARTITION p0 VALUES LESS THAN (2021),
  PARTITION p1 VALUES LESS THAN (2022),
  PARTITION pmax VALUES LESS THAN MAXVALUE
)" test
add_partition_table t11 't11#p#p0.ibd'

# t12: HASH partitioning
mysql -e "CREATE TABLE t12 (
  id INT NOT NULL AUTO_INCREMENT,
  val INT,
  PRIMARY KEY (id)
) ENGINE=InnoDB
PARTITION BY HASH (id)
PARTITIONS 4" test
add_partition_table t12 't12#p#p0.ibd'

# t13: LIST partitioning
mysql -e "CREATE TABLE t13 (
  id INT NOT NULL,
  region INT NOT NULL,
  PRIMARY KEY (id, region)
) ENGINE=InnoDB
PARTITION BY LIST (region) (
  PARTITION p_east VALUES IN (1, 2, 3),
  PARTITION p_west VALUES IN (4, 5, 6),
  PARTITION p_other VALUES IN (7, 8, 9)
)" test
add_partition_table t13 't13#p#p_east.ibd'

# t14: KEY partitioning
mysql -e "CREATE TABLE t14 (
  id INT NOT NULL AUTO_INCREMENT,
  name VARCHAR(100),
  PRIMARY KEY (id)
) ENGINE=InnoDB
PARTITION BY KEY (id)
PARTITIONS 4" test
add_partition_table t14 't14#p#p0.ibd'

# t15: RANGE COLUMNS partition (string values)
mysql -e "CREATE TABLE t15 (
  fname VARCHAR(50),
  lname VARCHAR(50),
  id INT NOT NULL,
  PRIMARY KEY (id, lname)
) ENGINE=InnoDB
PARTITION BY RANGE COLUMNS(lname) (
  PARTITION p_a_m VALUES LESS THAN ('N'),
  PARTITION p_n_z VALUES LESS THAN (MAXVALUE)
)" test
add_partition_table t15 't15#p#p_a_m.ibd'

# t16: LIST COLUMNS partition
mysql -e "CREATE TABLE t16 (
  id INT NOT NULL,
  city VARCHAR(25) NOT NULL,
  PRIMARY KEY (id, city)
) ENGINE=InnoDB
PARTITION BY LIST COLUMNS(city) (
  PARTITION p_nyc VALUES IN ('New York'),
  PARTITION p_la VALUES IN ('Los Angeles'),
  PARTITION p_other VALUES IN ('Chicago','Houston')
)" test
add_partition_table t16 't16#p#p_nyc.ibd'

# t17: RANGE with HASH subpartitioning
mysql -e "CREATE TABLE t17 (
  id INT NOT NULL,
  purchased DATE NOT NULL,
  PRIMARY KEY (id, purchased)
) ENGINE=InnoDB
PARTITION BY RANGE (YEAR(purchased))
SUBPARTITION BY HASH (TO_DAYS(purchased))
SUBPARTITIONS 2 (
  PARTITION p0 VALUES LESS THAN (2020),
  PARTITION p1 VALUES LESS THAN (2025),
  PARTITION pmax VALUES LESS THAN MAXVALUE
)" test
add_partition_table t17 't17#p#p0#sp#p0sp0.ibd'

# =====================================================================
# Step 2: Capture reference schemas via mysqldump
# =====================================================================

for tbl in $TABLES; do
    ${MYSQLDUMP} ${MYSQL_ARGS} --no-data --skip-comments --skip-dump-date \
        test "$tbl" > $topdir/original_${tbl}.sql
done

vlog "Reference schemas captured"

# =====================================================================
# Step 3: Shutdown server cleanly so .ibd files are fully flushed
# =====================================================================

shutdown_server

vlog "Server shut down cleanly"

# =====================================================================
# Step 4: Run ibd2sdi --create-sql on each .ibd file
# =====================================================================

for tbl in $TABLES; do
    # Determine the .ibd file: partitioned tables use a specific partition file
    is_part=0
    for pt in $PARTITION_TABLES; do
        if [ "$pt" = "$tbl" ]; then is_part=1; break; fi
    done

    if [ $is_part -eq 1 ]; then
        eval "ibd_basename=\${PART_IBD_${tbl}}"
        ibd_file="${DATADIR}test/${ibd_basename}"
    else
        ibd_file="${DATADIR}test/${tbl}.ibd"
    fi

    if [ ! -f "$ibd_file" ]; then
        die "ERROR: .ibd file not found: ${ibd_file}"
    fi

    # The .sql is written next to the .ibd -- for partitioned tables it goes
    # next to the partition file. We'll capture from stdout instead.
    vlog "Running: $IBD2SDI --create-sql ${ibd_file}"
    run_cmd $IBD2SDI --create-sql "${ibd_file}" \
        > $topdir/generated_${tbl}.sql 2>$topdir/stderr_${tbl}.log

    if ! grep -q "CREATE TABLE" $topdir/generated_${tbl}.sql; then
        vlog "ERROR: stdout for ${tbl} does not contain CREATE TABLE"
        cat $topdir/stderr_${tbl}.log >&2
        exit -1
    fi

    vlog "Generated SQL for ${tbl}:"
    cat $topdir/generated_${tbl}.sql >&2
done

vlog "All ibd2sdi --create-sql runs completed"

# =====================================================================
# Step 5: Start server, drop all tables, replay generated .sql
# =====================================================================

start_server

for tbl in $TABLES; do
    mysql -e "SET foreign_key_checks=0; DROP TABLE IF EXISTS \`$tbl\`" test
done

for tbl in $TABLES; do
    sql_file="$topdir/generated_${tbl}.sql"

    vlog "Recreating $tbl from generated SQL"
    (echo "SET foreign_key_checks=0;"; cat "$sql_file") | mysql test
done

vlog "All tables recreated from generated SQL"

# =====================================================================
# Step 6: Capture schemas again and compare
# =====================================================================

for tbl in $TABLES; do
    ${MYSQLDUMP} ${MYSQL_ARGS} --no-data --skip-comments --skip-dump-date \
        test "$tbl" > $topdir/recreated_${tbl}.sql

    run_cmd diff -u $topdir/original_${tbl}.sql $topdir/recreated_${tbl}.sql
    vlog "ibd2sdi_create_sql: $tbl roundtrip passed"
done

# =====================================================================
# Cleanup
# =====================================================================

for tbl in $TABLES; do
    mysql -e "SET foreign_key_checks=0; DROP TABLE IF EXISTS \`$tbl\`" test 2>/dev/null || true
done

vlog "All ibd2sdi --create-sql tests passed"
