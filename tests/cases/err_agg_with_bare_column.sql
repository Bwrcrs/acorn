CREATE TABLE t (id INT, name TEXT);
INSERT INTO t VALUES (1, 'a');
INSERT INTO t VALUES (2, 'b');
-- 一旦用了聚合，结果就从很多行塌缩成了一行。
-- 那一行里的 `id` 指的是原来的哪一行？没法回答。
-- 这正是 GROUP BY 要解决的问题——阶段 3 之前先直接报错，
-- 不猜。
SELECT id, COUNT(*) FROM t;
