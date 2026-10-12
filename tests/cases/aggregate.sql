CREATE TABLE t (id INT, name TEXT, score REAL);
INSERT INTO t VALUES (1, 'alice', 90.0);
INSERT INTO t VALUES (2, 'bob', 80.0);
INSERT INTO t VALUES (3, 'carol', NULL);
INSERT INTO t VALUES (4, 'dave', 70.0);

-- 五种聚合一起来
SELECT COUNT(*), COUNT(score), SUM(score), AVG(score), MIN(score), MAX(score) FROM t;

-- COUNT(*) 数的是**行数**，COUNT(列) 数的是**这一列里非 NULL 的个数**。
-- 上面那个查询里这两个数不一样（4 和 3），这是它们唯一但关键的区别。
-- 用 COUNT(列) 去数行数，是 NULL 一多就出错的经典写法。
SELECT COUNT(*) FROM t;
SELECT COUNT(score) FROM t;
SELECT COUNT(name) FROM t;

-- 整数列求和，结果还是整数（不是浮点的 10.0）
SELECT SUM(id) FROM t;

-- AVG 永远返回浮点。
-- 强行返回整数的话，AVG(1, 2) 会截断成 1——差了整整 0.5，
-- 这种"看起来像对的"错误最难发现。
CREATE TABLE small (v INT);
INSERT INTO small VALUES (1);
INSERT INTO small VALUES (2);
SELECT AVG(v), SUM(v) FROM small;

-- 聚合和 WHERE 一起用：先筛选、再统计
SELECT COUNT(*) FROM t WHERE score > 75;

-- 聚合的结果可以参与算术（聚合表达式是普通表达式的一种）
SELECT SUM(score) / COUNT(score) FROM t;

-- MIN/MAX 对文本也能用（比的是字典序）
SELECT MIN(name), MAX(name) FROM t;

-- 聚合列名用表达式文本，所以 COUNT(*) 那一列叫 `(count *)`
SELECT MAX(id) FROM t;
