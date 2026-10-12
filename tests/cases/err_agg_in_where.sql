CREATE TABLE t (id INT);
INSERT INTO t VALUES (1);
-- WHERE 是逐行判断的，那时候还没有"总数"这个东西。
-- SQL 的执行顺序是先 WHERE 筛选、再聚合统计，
-- 所以 WHERE 里出现聚合函数是逻辑上做不到，不是"没实现"。
SELECT id FROM t WHERE COUNT(*) > 0;
