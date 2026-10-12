CREATE TABLE t (id INT, name TEXT);
INSERT INTO t VALUES (1, 'a');
-- 数字和文本不许比。别家数据库会定一套类型优先级然后给你个答案，
-- Acorn 选择报错。
SELECT * FROM t WHERE id = 'a';
