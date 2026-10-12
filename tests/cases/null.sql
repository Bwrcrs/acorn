CREATE TABLE t (id INT, name TEXT);
INSERT INTO t VALUES (1, 'alice');
INSERT INTO t VALUES (2, NULL);
INSERT INTO t VALUES (3, 'carol');
-- NULL = NULL 是 NULL，不是 TRUE，所以一行都不返回
SELECT * FROM t WHERE name = NULL;
-- 反过来也一样：NOT(NULL) 还是 NULL，第 2 行依然被滤掉
SELECT * FROM t WHERE NOT (name = 'alice');
