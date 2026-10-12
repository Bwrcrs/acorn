CREATE TABLE t (a INT, b INT);
INSERT INTO t VALUES (1, 0);
-- 取模的除数同样不能是 0（它不是"除不尽"，是根本没定义）
SELECT a % b FROM t;
