CREATE TABLE t (id INT, name TEXT);
INSERT INTO t VALUES (1, 'a');
INSERT INTO t VALUES (2, 'b');
INSERT INTO t VALUES (3, 'c');
INSERT INTO t VALUES (4, 'd');
INSERT INTO t VALUES (5, 'e');

-- 删中间几行。这一步是 deleteRows 从后往前删的实测：
-- 如果实现是从前往后删而不修正行号，这里会删错行，
-- 而且**不会报错**——只会打出一张看着挺正常、但内容全错的表。
DELETE FROM t WHERE id = 2 OR id = 4;
SELECT * FROM t;

-- 再删一批，验证前面删完之后行号还是对的
DELETE FROM t WHERE id = 1;
SELECT * FROM t;

-- 一条都没命中
DELETE FROM t WHERE id = 999;
SELECT * FROM t;

-- 删光
DELETE FROM t;
SELECT * FROM t;

-- 空表上再删一次：不能崩
DELETE FROM t;
SELECT * FROM t;
