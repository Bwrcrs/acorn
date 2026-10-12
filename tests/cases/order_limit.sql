CREATE TABLE t (id INT, name TEXT, score REAL);
INSERT INTO t VALUES (1, 'alice', 91.5);
INSERT INTO t VALUES (2, 'bob', 78.0);
INSERT INTO t VALUES (3, 'carol', 88.25);
INSERT INTO t VALUES (4, 'dave', NULL);
INSERT INTO t VALUES (5, 'eve', 78.0);

-- 默认升序
SELECT id, score FROM t ORDER BY score;

-- 降序。NULL 落在最后——因为它被当成最小值，
-- 升序时冒头，降序时沉底。
--
-- 【为什么 NULL 当最小值】
-- 和 SQLite 一致。注意 Postgres 的默认正好相反（NULL 最大）。
-- 这条没有"对错"，但必须选一个并且写下来——不然实现者随手
-- 写成"NULL 不参与排序"或者"NULL 跟谁都不相等于是位置随机"，
-- 结果就没法测了。
SELECT id, score FROM t ORDER BY score DESC;

-- NULL 显式排最后：ORDER BY 的键用 IS NULL 包一层就是常见写法
SELECT id, score FROM t ORDER BY score IS NULL, score DESC;

-- 多键排序：score 相同的按 id 降序
-- （2 和 5 的 score 都是 78，看它们谁先出来）
SELECT id, score FROM t ORDER BY score DESC, id DESC;

-- 按文本排
SELECT id, name FROM t ORDER BY name;

-- 按表达式排（表达式当排序键）
SELECT id, score FROM t ORDER BY score * -1;

-- LIMIT
SELECT id FROM t ORDER BY id LIMIT 2;

-- LIMIT + OFFSET。没有 ORDER BY 时顺序就是插入顺序，
-- 这里加上 ORDER BY 让结果稳定可测。
SELECT id FROM t ORDER BY id LIMIT 2 OFFSET 1;

-- OFFSET 超出总行数：空结果，不报错
SELECT id FROM t ORDER BY id LIMIT 3 OFFSET 99;

-- LIMIT 0：合法，就是一行都不要
SELECT id FROM t ORDER BY id LIMIT 0;

-- LIMIT 比总行数大：全都出来
SELECT id FROM t ORDER BY id LIMIT 100;
