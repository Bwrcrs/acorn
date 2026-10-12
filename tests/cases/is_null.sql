CREATE TABLE t (id INT, name TEXT, score REAL);
INSERT INTO t VALUES (1, 'alice', 91.5);
INSERT INTO t VALUES (2, 'bob', NULL);
INSERT INTO t VALUES (3, NULL, 88.25);
INSERT INTO t VALUES (4, NULL, NULL);

-- IS NULL / IS NOT NULL 是**唯一**能可靠筛出 NULL 的写法。
-- 它返回的是真布尔（TRUE/FALSE），永远不是 UNKNOWN，
-- 所以 WHERE 一定放得过去。
SELECT id FROM t WHERE score IS NULL;
SELECT id FROM t WHERE score IS NOT NULL;
SELECT id FROM t WHERE name IS NULL AND score IS NULL;
SELECT id FROM t WHERE name IS NULL OR score IS NULL;

-- 对照：= NULL 的结果恒为 UNKNOWN，WHERE 只放 TRUE 过去，
-- 所以下面这条一行都筛不出来。**不报错**——这是 SQL 最著名的坑，
-- 新手在这里耗掉一下午，因为语句看起来完全合理。
SELECT id FROM t WHERE score = NULL;

-- 换 <> 也一样是 UNKNOWN
SELECT id FROM t WHERE score <> NULL;

-- 取反也不行：NOT UNKNOWN 还是 UNKNOWN
SELECT id FROM t WHERE NOT (score = NULL);

-- IS NULL 也能出现在选择列表里，当布尔列用
SELECT id, score IS NULL, name IS NOT NULL FROM t;

-- IS NULL 的结果能参与 AND/OR，和普通布尔没区别
SELECT id FROM t WHERE score IS NULL OR id = 1;
