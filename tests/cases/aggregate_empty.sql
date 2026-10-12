CREATE TABLE t (id INT, name TEXT);
INSERT INTO t VALUES (1, 'a');
INSERT INTO t VALUES (2, 'b');

-- 空集上的聚合。这里有一个**必须记住的不对称**：
--   COUNT 返回 0（"有几个"确实是 0，这是个确定的答案）
--   SUM / AVG / MIN / MAX 返回 NULL（"一堆空值的和"没有答案）
--
-- 如果 SUM 在空集上给 0，那 `SELECT SUM(x) FROM t WHERE 1=0`
-- 和"这一列真的加起来等于 0"就分不开了。NULL 是这里唯一诚实的结果。
SELECT COUNT(*), SUM(id), AVG(id), MIN(id), MAX(id) FROM t WHERE id > 100;

-- 表一行都没有的时候也一样
CREATE TABLE empty (v INT);
SELECT COUNT(*), SUM(v), AVG(v), MIN(v), MAX(v) FROM empty;

-- 全是 NULL 的列：COUNT(*) 数的是行，所以是 2；
-- COUNT(v) 数的是非 NULL，所以是 0。SUM 因为没有非 NULL 值而给 NULL。
CREATE TABLE allnull (v INT);
INSERT INTO allnull VALUES (NULL);
INSERT INTO allnull VALUES (NULL);
SELECT COUNT(*), COUNT(v), SUM(v), AVG(v) FROM allnull;

-- SUM 空集给 NULL，所以 NULL 会继续传染到算术里
SELECT SUM(v) + 1 FROM allnull;
