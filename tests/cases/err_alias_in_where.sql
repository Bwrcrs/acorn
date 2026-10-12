CREATE TABLE t (id INT, score REAL);
INSERT INTO t VALUES (1, 91.5);
-- 别名在 WHERE 里**看不见**。
-- 这不是没实现，是标准 SQL 就这么规定的，而且有道理：
-- WHERE 在 SELECT 之前执行（先筛选、再算列），
-- 所以筛选的时候 `编号` 这个名字还不存在。
--
-- 报的是"没有叫 编号 的列"，对用户来说够用了——
-- 他多半是把 ORDER BY 的用法记串了。
SELECT id AS 编号 FROM t WHERE 编号 = 1;
