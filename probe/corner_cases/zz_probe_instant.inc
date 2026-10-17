--source include/have_innodb_16k.inc
--disable_abort_on_error
let $D = `SELECT @@datadir`;
let $A = $MYSQL_TMP_DIR/zza;
let $B = $MYSQL_TMP_DIR/zzb;
--mkdir $A
--mkdir $B
CREATE TABLE it (id INT PRIMARY KEY, a INT, x INT) PARTITION BY RANGE (id)
 (PARTITION p0 VALUES LESS THAN (10), PARTITION p1 VALUES LESS THAN (20));
INSERT INTO it VALUES (1,1,1),(11,11,11);
FLUSH TABLES it FOR EXPORT;
--copy_files_wildcard $D/test $A it#p#*.ibd
UNLOCK TABLES;
eval $zz_ddl;
eval $zz_dml;
SELECT * FROM it ORDER BY id;
FLUSH TABLES it FOR EXPORT;
--copy_files_wildcard $D/test $B it#p#*.ibd
UNLOCK TABLES;
DROP TABLE it;
