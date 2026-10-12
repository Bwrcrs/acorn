-- 阶段 4：规划器**不该**用索引的那些情况。
--
-- 【这一份和 index_basic.sql 是一对】
-- 那边测"该走索引的时候结果对不对"，这边测"不该走索引的时候，
-- 结果也一个字不能变"。
--
-- 两条都重要，但这一条更容易出事。规划器认不出一个条件就退回全表扫，
-- 答案照样对，只是慢 —— 没人会发现。反过来，它**认错了**
-- （把一个其实不能用的条件当成能用的），答案就直接错了，
-- 而且错得悄无声息。所以下面每一条都是"看着像能用、其实不能用"的条件。

CREATE TABLE t (id INT, k INT, name TEXT);
INSERT INTO t VALUES (1, 1, 'a'), (2, 2, 'b'), (3, 3, 'c'), (4, 4, 'd'), (5, 5, 'e');
CREATE INDEX t_id ON t (id);

-- 两边都是列：没有一个常量可以拿去查索引
SELECT id FROM t WHERE id = k;
SELECT id FROM t WHERE id < k;
SELECT id FROM t WHERE id = k AND name = 'c';

-- 左边是表达式，不是光秃秃一个列名。
-- 要支持这个，规划器得会解一元一次方程 —— 那是另一个量级的活。
SELECT id FROM t WHERE id + 1 = 4;
SELECT id FROM t WHERE id * 2 = 6;
SELECT id FROM t WHERE -id = -3;

-- 自己等于自己：恒真，但不能拿它当 `id = ?` 去查索引
SELECT id FROM t WHERE id = id;

-- 包在 NOT 里面：取反之后区间会变成补集，规划器不认
SELECT id FROM t WHERE NOT (id = 3);
SELECT id FROM t WHERE NOT (id > 3);

-- OR：两段区间合起来是个并集，一个 B+ 树区间表示不了。
-- （不是做不了，是没做 —— 要做就是两趟查找再合并去重。）
SELECT id FROM t WHERE id = 3 OR id = 5;
SELECT id FROM t WHERE id = 3 OR name = 'd';
SELECT id FROM t WHERE (id = 3 OR id = 5) AND name <> 'c';

-- NULL：跟 NULL 比永远是 UNKNOWN，等值查不出任何行。
-- 这里要的是**空结果**，而不是"把 NULL 当成最小值查了一堆出来"。
SELECT id FROM t WHERE id = NULL;
SELECT id FROM t WHERE id > NULL;

-- 不等于：补集是两个洞，一次区间扫不出来
SELECT id FROM t WHERE id != 3;
SELECT id FROM t WHERE id <> 3;
SELECT id FROM t WHERE id != 3 AND name <> 'zzz';

-- 常量是 REAL、列是 INT。这个组合能不能比是 value.cpp 定的；
-- 不管它定成什么，这里的结果都得和全表扫一模一样。
SELECT id FROM t WHERE id = 3.0;
SELECT id FROM t WHERE id > 3.5;

-- 顶层 AND 里混着能用和不能用的：能用的那部分走索引，
-- 不能用的那部分逐行算，合起来还是对的
SELECT id FROM t WHERE id >= 2 AND id + 1 <= 5;
SELECT id FROM t WHERE id >= 2 AND (name = 'b' OR name = 'd');

-- 条件写在 AND 的右边也算数：规划器是两边都看的
SELECT id FROM t WHERE name <> 'zzz' AND id <= 2;
