########################################################################
# Test that --prepare --export generates valid .sql schema files
# from SDI (Serialized Dictionary Information) in InnoDB tablespaces.
#
# Verification approach:
#   1. Create tables on server
#   2. mysqldump --no-data to capture original schema
#   3. Backup + prepare --export (generates .sql from SDI)
#   4. Drop tables, recreate from generated .sql
#   5. mysqldump --no-data to capture recreated schema
#   6. diff the two mysqldump outputs per table
########################################################################

. inc/common.sh

TABLES=""

function add_test_table() {
    local tbl=$1
    TABLES="$TABLES $tbl"
}

function verify_roundtrip() {
    # Capture original schema for all test tables
    for tbl in $TABLES; do
        ${MYSQLDUMP} ${MYSQL_ARGS} --no-data --skip-comments --skip-dump-date \
            test "$tbl" > $topdir/original_${tbl}.sql
    done

    # Backup
    xtrabackup --backup --target-dir=$topdir/backup

    # Prepare with --export to generate .sql files from SDI
    xtrabackup --prepare --export --target-dir=$topdir/backup

    # For each table: verify .sql was generated, drop, recreate, diff
    for tbl in $TABLES; do
        if [ ! -f "$topdir/backup/test/${tbl}.sql" ]; then
            vlog "ERROR: ${tbl}.sql was not generated in backup directory"
            exit -1
        fi

        vlog "Generated ${tbl}.sql contents:"
        cat $topdir/backup/test/${tbl}.sql >&2

        mysql -e "SET foreign_key_checks=0; DROP TABLE \`$tbl\`" test
        (echo "SET foreign_key_checks=0;"; cat $topdir/backup/test/${tbl}.sql) | mysql test

        ${MYSQLDUMP} ${MYSQL_ARGS} --no-data --skip-comments --skip-dump-date \
            test "$tbl" > $topdir/recreated_${tbl}.sql

        run_cmd diff -u $topdir/original_${tbl}.sql $topdir/recreated_${tbl}.sql
        vlog "generate_schema: $tbl roundtrip passed"
    done
}

start_server

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

# t10: RANGE partitioning
mysql -e "CREATE TABLE t10 (
  id INT NOT NULL,
  created DATE NOT NULL,
  PRIMARY KEY (id, created)
) ENGINE=InnoDB
PARTITION BY RANGE (YEAR(created)) (
  PARTITION p2020 VALUES LESS THAN (2021),
  PARTITION p2021 VALUES LESS THAN (2022),
  PARTITION pmax VALUES LESS THAN MAXVALUE
)" test
add_test_table t10

# t11: HASH partitioning
mysql -e "CREATE TABLE t11 (
  id INT NOT NULL AUTO_INCREMENT,
  val INT,
  PRIMARY KEY (id)
) ENGINE=InnoDB
PARTITION BY HASH (id)
PARTITIONS 4" test
add_test_table t11

# t12: LIST partitioning
mysql -e "CREATE TABLE t12 (
  id INT NOT NULL,
  region INT NOT NULL,
  PRIMARY KEY (id, region)
) ENGINE=InnoDB
PARTITION BY LIST (region) (
  PARTITION p_east VALUES IN (1, 2, 3),
  PARTITION p_west VALUES IN (4, 5, 6),
  PARTITION p_other VALUES IN (7, 8, 9)
)" test
add_test_table t12

# t13: backtick quoting - reserved words and special identifiers
mysql -e "CREATE TABLE \`select\` (
  \`order\` INT NOT NULL AUTO_INCREMENT,
  \`group\` VARCHAR(100) DEFAULT 'test',
  \`key\` INT,
  PRIMARY KEY (\`order\`),
  KEY \`index\` (\`group\`(50)),
  KEY \`desc\` (\`key\`)
) ENGINE=InnoDB" test
add_test_table 'select'

# t14: string escaping in comments and defaults
mysql -e "CREATE TABLE t14 (
  id INT NOT NULL AUTO_INCREMENT COMMENT 'it''s the primary key',
  name VARCHAR(100) DEFAULT 'O''Brien' COMMENT 'user''s name with quote',
  data VARCHAR(255) DEFAULT 'back\\\\slash',
  PRIMARY KEY (id)
) ENGINE=InnoDB COMMENT='table with ''quotes'' and back\\\\slashes'" test
add_test_table t14

# t15: COLLATE conditional - latin1 (primary collation, no COLLATE printed)
mysql -e "CREATE TABLE t15 (
  id INT NOT NULL AUTO_INCREMENT,
  name VARCHAR(100),
  PRIMARY KEY (id)
) ENGINE=InnoDB DEFAULT CHARSET=latin1" test
add_test_table t15

# t16: COLLATE conditional - non-primary collation (COLLATE must be printed)
mysql -e "CREATE TABLE t16 (
  id INT NOT NULL AUTO_INCREMENT,
  name VARCHAR(100),
  PRIMARY KEY (id)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci" test
add_test_table t16

# t17: COMPRESSION (InnoDB page-level compression)
mysql -e "CREATE TABLE t17 (
  id INT NOT NULL AUTO_INCREMENT,
  data TEXT,
  PRIMARY KEY (id)
) ENGINE=InnoDB COMPRESSION='zlib'" test
add_test_table t17

# t18: FULLTEXT index with parser, SPATIAL index
mysql -e "CREATE TABLE t18 (
  id INT NOT NULL AUTO_INCREMENT,
  title VARCHAR(200),
  body TEXT,
  location POINT NOT NULL SRID 0,
  PRIMARY KEY (id),
  FULLTEXT KEY ft_body (body),
  SPATIAL KEY sp_loc (location)
) ENGINE=InnoDB" test
add_test_table t18

verify_roundtrip

vlog "generate_schema: all tests passed"
