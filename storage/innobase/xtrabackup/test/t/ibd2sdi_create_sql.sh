########################################################################
# Test ibd2sdi --create-sql: generates CREATE TABLE SQL from SDI JSON
# directly parsed from .ibd files using pure RapidJSON (no dd::deserialize).
#
# Strategy:
#   1. Start server, create diverse tables (file-per-table + general TS)
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
GS_TABLES=""
GC_TABLES=""

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
# Create test tables (file-per-table)
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

# t10: FULLTEXT index (basic, multi-column, WITH PARSER ngram), SPATIAL index
mysql -e "CREATE TABLE t10 (
  id INT NOT NULL AUTO_INCREMENT,
  title VARCHAR(200),
  body TEXT,
  location POINT NOT NULL SRID 0,
  PRIMARY KEY (id),
  FULLTEXT KEY ft_body (body),
  FULLTEXT KEY ft_title_body (title, body),
  FULLTEXT KEY ft_ngram (body) WITH PARSER ngram,
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

# t18: kitchen-sink column types not covered elsewhere
mysql -e "CREATE TABLE t18 (
  id INT NOT NULL AUTO_INCREMENT,
  c_tinyint TINYINT,
  c_tinyint_u TINYINT UNSIGNED,
  c_smallint SMALLINT,
  c_smallint_u SMALLINT UNSIGNED,
  c_mediumint MEDIUMINT,
  c_mediumint_u MEDIUMINT UNSIGNED,
  c_bigint_u BIGINT UNSIGNED,
  c_decimal DECIMAL(10,2),
  c_decimal_u DECIMAL(10,2) UNSIGNED,
  c_numeric NUMERIC(5,3),
  c_double DOUBLE,
  c_real REAL,
  c_bit BIT(1),
  c_bit64 BIT(64),
  c_char CHAR(50) NOT NULL,
  c_binary BINARY(16),
  c_varbinary VARBINARY(255),
  c_tinyblob TINYBLOB,
  c_mediumblob MEDIUMBLOB,
  c_longblob LONGBLOB,
  c_tinytext TINYTEXT,
  c_mediumtext MEDIUMTEXT,
  c_longtext LONGTEXT,
  c_bool BOOLEAN DEFAULT TRUE,
  c_year YEAR,
  c_time TIME,
  c_time_fsp TIME(6),
  c_date DATE,
  c_set SET('a','b','c','d'),
  c_json JSON,
  c_varchar_cs VARCHAR(100) CHARACTER SET utf8mb3 COLLATE utf8mb3_bin,
  c_text_cs TEXT CHARACTER SET latin1,
  PRIMARY KEY (id),
  KEY idx_decimal (c_decimal),
  KEY idx_char (c_char(20)),
  KEY idx_varbinary (c_varbinary(30))
) ENGINE=InnoDB" test
add_test_table t18

# t19: INVISIBLE columns
mysql -e 'CREATE TABLE t19 (
  id INT NOT NULL AUTO_INCREMENT,
  visible_col VARCHAR(100) NOT NULL,
  invisible_col INT /*!80023 INVISIBLE */,
  val INT NOT NULL DEFAULT 0,
  invisible_gen INT GENERATED ALWAYS AS (val * 10) STORED /*!80023 INVISIBLE */,
  PRIMARY KEY (id),
  KEY idx_vis (visible_col(30))
) ENGINE=InnoDB' test
add_test_table t19

# t20: JSON column with functional index and multi-valued index
${MYSQL} ${MYSQL_ARGS} test <<'EOSQL'
CREATE TABLE t20 (
  id INT NOT NULL AUTO_INCREMENT,
  doc JSON,
  tags JSON,
  PRIMARY KEY (id),
  KEY idx_name ((CAST(doc->>'$.name' AS CHAR(50)))),
  KEY idx_age ((CAST(doc->>'$.age' AS UNSIGNED))),
  KEY idx_tags ((CAST(tags->'$[*]' AS UNSIGNED ARRAY)))
) ENGINE=InnoDB;
EOSQL
add_test_table t20

# =====================================================================
# Create general tablespace tables (regular)
# =====================================================================

mysql -e "CREATE TABLESPACE ts_regular ADD DATAFILE 'ts_regular.ibd' ENGINE=InnoDB"
GS_TABLES="gs_t1 gs_t2 gs_t3 gs_t4 gs_t5 gs_t6_parent gs_t6 gs_t7 gs_t8"

mysql -e "CREATE TABLE gs_t1 (
  a INT NOT NULL AUTO_INCREMENT,
  b INT NOT NULL,
  c VARCHAR(100),
  PRIMARY KEY (a),
  KEY idx_b (b)
) ENGINE=InnoDB TABLESPACE ts_regular" test

mysql -e "CREATE TABLE gs_t2 (
  a INT NOT NULL,
  b INT GENERATED ALWAYS AS (a * 2) VIRTUAL,
  c INT GENERATED ALWAYS AS (a + 1) STORED,
  PRIMARY KEY (a)
) ENGINE=InnoDB TABLESPACE ts_regular" test

mysql -e "CREATE TABLE gs_t3 (
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
) ENGINE=InnoDB TABLESPACE ts_regular" test

mysql -e "CREATE TABLE gs_t4 (
  id INT NOT NULL AUTO_INCREMENT COMMENT 'primary key',
  name VARCHAR(100) COMMENT 'user name',
  location POINT NOT NULL SRID 4326,
  plain_geom POINT NOT NULL,
  PRIMARY KEY (id)
) ENGINE=InnoDB TABLESPACE ts_regular" test

mysql -e "CREATE TABLE gs_t5 (
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
) ENGINE=InnoDB TABLESPACE ts_regular" test

mysql -e "CREATE TABLE gs_t6_parent (
  id INT NOT NULL AUTO_INCREMENT,
  code VARCHAR(10) NOT NULL,
  PRIMARY KEY (id),
  UNIQUE KEY uk_code (code)
) ENGINE=InnoDB TABLESPACE ts_regular" test

mysql -e "CREATE TABLE gs_t6 (
  id INT NOT NULL AUTO_INCREMENT,
  parent_id INT NOT NULL,
  parent_code VARCHAR(10),
  PRIMARY KEY (id),
  KEY idx_parent (parent_id),
  KEY idx_code (parent_code),
  CONSTRAINT gs_fk_parent FOREIGN KEY (parent_id) REFERENCES gs_t6_parent (id) ON DELETE CASCADE ON UPDATE CASCADE,
  CONSTRAINT gs_fk_code FOREIGN KEY (parent_code) REFERENCES gs_t6_parent (code) ON DELETE SET NULL
) ENGINE=InnoDB TABLESPACE ts_regular" test

mysql -e "CREATE TABLE gs_t7 (
  id INT NOT NULL AUTO_INCREMENT,
  age INT NOT NULL,
  status ENUM('active','inactive') NOT NULL DEFAULT 'active',
  PRIMARY KEY (id),
  CONSTRAINT gs_chk_age CHECK (age >= 0 AND age <= 150),
  CONSTRAINT gs_chk_status CHECK (status IN ('active','inactive')) /*!80016 NOT ENFORCED */
) ENGINE=InnoDB TABLESPACE ts_regular" test

mysql -e "CREATE TABLE gs_t8 (
  id INT NOT NULL AUTO_INCREMENT,
  title VARCHAR(200),
  body TEXT,
  location POINT NOT NULL SRID 0,
  PRIMARY KEY (id),
  FULLTEXT KEY ft_body (body),
  FULLTEXT KEY ft_title_body (title, body),
  FULLTEXT KEY ft_ngram (body) WITH PARSER ngram,
  SPATIAL KEY sp_loc (location)
) ENGINE=InnoDB TABLESPACE ts_regular" test

vlog "Regular general tablespace tables created"

# =====================================================================
# Create compressed general tablespace tables
# =====================================================================

mysql -e "CREATE TABLESPACE ts_compressed ADD DATAFILE 'ts_compressed.ibd' FILE_BLOCK_SIZE=8192 ENGINE=InnoDB"
GC_TABLES="gc_t1 gc_t2 gc_t3"

mysql -e "CREATE TABLE gc_t1 (
  a INT NOT NULL AUTO_INCREMENT,
  b INT NOT NULL,
  c VARCHAR(100),
  PRIMARY KEY (a),
  KEY idx_b (b)
) ENGINE=InnoDB ROW_FORMAT=COMPRESSED KEY_BLOCK_SIZE=8 TABLESPACE ts_compressed" test

mysql -e "CREATE TABLE gc_t2 (
  a INT NOT NULL,
  b INT GENERATED ALWAYS AS (a * 2) VIRTUAL,
  c INT GENERATED ALWAYS AS (a + 1) STORED,
  PRIMARY KEY (a)
) ENGINE=InnoDB ROW_FORMAT=COMPRESSED KEY_BLOCK_SIZE=8 TABLESPACE ts_compressed" test

mysql -e "CREATE TABLE gc_t3 (
  id INT NOT NULL AUTO_INCREMENT,
  d_int INT DEFAULT 42,
  d_varchar VARCHAR(50) DEFAULT 'hello world',
  d_text TEXT,
  d_ts TIMESTAMP DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP,
  PRIMARY KEY (id)
) ENGINE=InnoDB ROW_FORMAT=COMPRESSED KEY_BLOCK_SIZE=8 TABLESPACE ts_compressed" test

vlog "Compressed general tablespace tables created"

# =====================================================================
# Step 2: Capture reference schemas via mysqldump
# =====================================================================

ALL_USER_TABLES="$TABLES $GS_TABLES $GC_TABLES"

for tbl in $ALL_USER_TABLES; do
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

# --- File-per-table .ibd files ---
for tbl in $TABLES; do
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

# --- Regular general tablespace .ibd ---
vlog "Running ibd2sdi --create-sql on ts_regular.ibd"
run_cmd $IBD2SDI --create-sql "${DATADIR}ts_regular.ibd" \
    > $topdir/generated_ts_regular.sql 2>$topdir/stderr_ts_regular.log

if ! grep -q "CREATE TABLESPACE" $topdir/generated_ts_regular.sql; then
    vlog "ERROR: generated output for ts_regular does not contain CREATE TABLESPACE"
    cat $topdir/stderr_ts_regular.log >&2
    exit -1
fi
vlog "Generated SQL for ts_regular:"
cat $topdir/generated_ts_regular.sql >&2

# --- Compressed general tablespace .ibd ---
vlog "Running ibd2sdi --create-sql on ts_compressed.ibd"
run_cmd $IBD2SDI --create-sql "${DATADIR}ts_compressed.ibd" \
    > $topdir/generated_ts_compressed.sql 2>$topdir/stderr_ts_compressed.log

if ! grep -q "CREATE TABLESPACE" $topdir/generated_ts_compressed.sql; then
    vlog "ERROR: generated output for ts_compressed does not contain CREATE TABLESPACE"
    cat $topdir/stderr_ts_compressed.log >&2
    exit -1
fi
if ! grep -q "FILE_BLOCK_SIZE" $topdir/generated_ts_compressed.sql; then
    vlog "ERROR: generated output for ts_compressed does not contain FILE_BLOCK_SIZE"
    cat $topdir/generated_ts_compressed.sql >&2
    exit -1
fi
vlog "Generated SQL for ts_compressed:"
cat $topdir/generated_ts_compressed.sql >&2

vlog "All ibd2sdi --create-sql runs completed"

# =====================================================================
# Step 5: Start server, drop everything, replay generated .sql
# =====================================================================

start_server

# Drop all tables (FK child tables first, then all others)
mysql -e "SET foreign_key_checks=0; DROP TABLE IF EXISTS t7, gs_t6" test

for tbl in $ALL_USER_TABLES; do
    mysql -e "SET foreign_key_checks=0; DROP TABLE IF EXISTS \`$tbl\`" test
done
mysql -e "DROP TABLESPACE ts_regular"
mysql -e "DROP TABLESPACE ts_compressed"

vlog "All tables and tablespaces dropped"

# Replay file-per-table tables
for tbl in $TABLES; do
    sql_file="$topdir/generated_${tbl}.sql"
    vlog "Recreating $tbl from generated SQL"
    (echo "SET foreign_key_checks=0;"; cat "$sql_file") | mysql test
done

# Replay regular general tablespace (CREATE TABLESPACE + CREATE TABLEs)
vlog "Replaying generated SQL for ts_regular"
(echo "SET foreign_key_checks=0;"; cat "$topdir/generated_ts_regular.sql") | mysql test

# Replay compressed general tablespace
vlog "Replaying generated SQL for ts_compressed"
(echo "SET foreign_key_checks=0;"; cat "$topdir/generated_ts_compressed.sql") | mysql test

vlog "All tables recreated from generated SQL"

# =====================================================================
# Step 6: Capture schemas again and compare
# =====================================================================

for tbl in $ALL_USER_TABLES; do
    ${MYSQLDUMP} ${MYSQL_ARGS} --no-data --skip-comments --skip-dump-date \
        test "$tbl" > $topdir/recreated_${tbl}.sql

    run_cmd diff -u $topdir/original_${tbl}.sql $topdir/recreated_${tbl}.sql
    vlog "ibd2sdi_create_sql: $tbl roundtrip passed"
done

# =====================================================================
# Cleanup
# =====================================================================

vlog "All ibd2sdi --create-sql tests passed"
