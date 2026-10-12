CREATE TABLE t (id INT);
INSERT INTO t VALUES (1);
-- 聚合不能套着写。COUNT 的结果是**一个数**，
-- 对"一个数"再数一遍个数永远是 1，没有意义。
SELECT COUNT(COUNT(*)) FROM t;
