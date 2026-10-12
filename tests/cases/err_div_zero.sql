CREATE TABLE t (a INT, b INT);
INSERT INTO t VALUES (1, 0);
-- 除零**报错**，不返回 NULL。
-- 这是跟 Postgres 学的；SQLite / MySQL 会给 NULL。
-- 选报错是因为 NULL 会静悄悄地传染到后面的计算里，
-- 而"我的查询里有个除以零"是用户需要立刻知道的事。
SELECT a / b FROM t;
