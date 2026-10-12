CREATE TABLE t (id INT, flag BOOL, score REAL);
INSERT INTO t VALUES (1, TRUE, 91.5);
INSERT INTO t VALUES (2, FALSE, 78.0);
-- 选择列表里可以不写列名，写表达式：列头用表达式本身来命名
SELECT NOT flag, -score FROM t;
SELECT 1, 'hello' FROM t;
SELECT id, score > 85 FROM t;
