-- 阶段 4：索引，先测"查出来的东西对不对"。
-- 索引长什么样、树怎么分裂、页怎么回收，都在别的地方测。
--
-- 【这一份真正想钉死的一句话】
-- 加索引只能让查询变快，**结果一个字都不能变**。
-- 下面每一组都在索引列上写条件，所以每一组都会走索引。
-- 规划器要是算错了一个开闭区间，这里就会少行或者多行——
-- 而那种错**不报异常**，只是答案悄悄不对。
--
-- 所以每一条查询都同时给了"该有多少行"的直觉：
-- 区间一律把两头都列出来，让人一眼能数。

CREATE TABLE t (id INT, score REAL, name TEXT);
INSERT INTO t VALUES (1, 10.5, 'a'), (2, 20.0, 'b'), (3, 30.5, 'c'), (4, 40.0, 'd'), (5, 50.5, 'e');

CREATE INDEX t_id ON t (id);

-- 等值：两头都是同一个值、两头都闭
SELECT * FROM t WHERE id = 3;

-- 四个比较符各来一次。开闭搞反的话，这里一眼就能看出来：
--   >  排掉 3      >= 留着 3
--   <  排掉 3      <= 留着 3
SELECT id FROM t WHERE id > 3;
SELECT id FROM t WHERE id >= 3;
SELECT id FROM t WHERE id < 3;
SELECT id FROM t WHERE id <= 3;

-- 两头都写
SELECT id FROM t WHERE id > 1 AND id < 5;
SELECT id FROM t WHERE id >= 1 AND id <= 5;
SELECT id FROM t WHERE id > 2 AND id <= 4;

-- 反过来写：值在左边、列在右边。
-- 规划器要认得出 `3 = id` 和 `id = 3` 是同一个条件，
-- 而且 `3 < id` 是 id > 3（**不是** id < 3）。
SELECT id FROM t WHERE 3 = id;
SELECT id FROM t WHERE 3 < id;
SELECT id FROM t WHERE 5 >= id;

-- 一个都命不中
SELECT id FROM t WHERE id = 99;
SELECT id FROM t WHERE id > 100 AND id < 200;

-- 索引列上的条件 + 别的条件：
-- 索引只负责把候选行找出来，别的条件照样逐行算。
-- （id >= 2 选出 2/3/4/5，再滤掉 name = 'c'，剩 2/4/5）
SELECT id FROM t WHERE id >= 2 AND name <> 'c';

-- 三个条件串在一起，其中两个在索引列上
SELECT id FROM t WHERE id >= 2 AND id < 5 AND name <> 'b';

-- 条件里**没有**索引列：退化成全表扫，结果照样得对
SELECT id FROM t WHERE name = 'c';
SELECT id FROM t WHERE score = 20.0;

-- ORDER BY / LIMIT 和索引扫描一起用。
-- 注意结果顺序由 ORDER BY 说了算，不是由索引的遍历顺序说了算。
SELECT id FROM t WHERE id >= 2 ORDER BY id DESC;
SELECT id FROM t WHERE id >= 2 ORDER BY id DESC LIMIT 2;
SELECT id FROM t ORDER BY id DESC LIMIT 3;

-- 聚合也走同一条路
SELECT COUNT(*), SUM(id), MIN(id), MAX(id) FROM t WHERE id >= 2;

-- 空结果上的聚合还是空集那套答案，不能变成 0 行或崩掉
SELECT COUNT(*), SUM(id), MIN(id) FROM t WHERE id > 100;

-- 同一张表上再来一个索引：两个索引并存，各管各的
CREATE INDEX t_score ON t (score);
SELECT id FROM t WHERE score >= 30.0;
SELECT id FROM t WHERE score > 10 AND score < 50;
SELECT COUNT(*) FROM t WHERE score > 0;
