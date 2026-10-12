CREATE TABLE t (i INT, r REAL, s TEXT, b BOOL);
INSERT INTO t VALUES (1, 2, 'three', TRUE);
-- 整数 7 放进 REAL 列：无损加宽，允许
INSERT INTO t VALUES (7, 8, 'nine', FALSE);
SELECT * FROM t;
