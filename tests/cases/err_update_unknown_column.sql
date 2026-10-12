CREATE TABLE t (id INT);
INSERT INTO t VALUES (1);
-- SET 里写了个不存在的列名。
-- 报错里带上"有：id"——用户多半是把名字记混了，
-- 直接告诉他表里到底有哪些列，比让他自己去 .schema 查快得多。
UPDATE t SET nope = 1 WHERE id = 1;
