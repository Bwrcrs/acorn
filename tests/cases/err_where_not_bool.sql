CREATE TABLE t (id INT);
INSERT INTO t VALUES (1);
-- WHERE 后面必须是布尔表达式。别的语言里 1 是真，SQL 里不是。
SELECT * FROM t WHERE id;
