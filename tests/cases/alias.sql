CREATE TABLE t (id INT, name TEXT, score REAL);
INSERT INTO t VALUES (1, 'alice', 91.5);
INSERT INTO t VALUES (2, 'bob', 78.0);
INSERT INTO t VALUES (3, 'carol', NULL);

-- 给聚合列起名字。没有别名的话这一列的表头是 `(count *)`——
-- 能看，但没法在别处引用，也没法当表头给人看。
SELECT COUNT(*) AS 人数, AVG(score) AS 平均分, SUM(score) AS 总分 FROM t;

-- 给表达式列起名字
SELECT id AS 编号, score * 2 AS 双倍分 FROM t;

-- 别名可以盖掉原本的列名。这一列的内容还是 name，表头变成 id。
-- 允许这么写是因为"表头叫什么"和"值从哪来"本来就是两件事。
SELECT name AS id FROM t;

-- **别名在 ORDER BY 里可见**，这是起名最实际的用处
SELECT id, score * 2 AS 双倍 FROM t ORDER BY 双倍 DESC;

-- 按别名排一个聚合……只有一行，排了等于没排，但不该报错
SELECT COUNT(*) AS n FROM t ORDER BY n;

-- 别名和真实列名撞了：**别名赢**（和标准 SQL 一致）。
-- 这里 ORDER BY id 排的是 score 那一列的别名，不是真的 id 列。
-- 结果顺序 91.5 / 78 / NULL 就是 DESC 的证据——
-- 按真的 id 降序会是 3 / 2 / 1，完全不一样。
-- 这是个会咬人的歧义，所以专门测一下，让它的行为至少是确定的。
SELECT score AS id, name FROM t ORDER BY id DESC;

-- 别名只在 ORDER BY 里可见，WHERE 里看不见。
-- 原因：WHERE 在 SELECT 之前执行，那时候别名还不存在。
-- （这一条的报错版本在 err_alias_in_where.sql 里。）
SELECT id AS 编号 FROM t ORDER BY 编号;
