CREATE TABLE t (name TEXT);
INSERT INTO t VALUES ('a');
-- SUM / AVG 只能用在数字列上。
-- 注意 COUNT 不在此列——COUNT(name) 是合法的，
-- 它数的是非 NULL 的个数，跟类型无关。见 aggregate.sql。
SELECT SUM(name) FROM t;
