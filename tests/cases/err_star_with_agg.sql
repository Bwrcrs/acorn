CREATE TABLE t (id INT);
INSERT INTO t VALUES (1);
-- `SELECT *, COUNT(*)` 是语法错误：
-- 星号已经把"所有列"都放进来了，再写一个聚合就自相矛盾。
SELECT *, COUNT(*) FROM t;
