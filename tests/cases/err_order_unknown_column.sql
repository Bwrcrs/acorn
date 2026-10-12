CREATE TABLE t (id INT);
INSERT INTO t VALUES (1);
-- ORDER BY 的键也必须是表里真有的列（或合法的表达式）
SELECT id FROM t ORDER BY nope;
