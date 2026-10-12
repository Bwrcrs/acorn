CREATE TABLE t (id INT, name TEXT, score REAL);
INSERT INTO t VALUES (1, 'alice', 91.5);
INSERT INTO t VALUES (2, 'bob', 78.0);
INSERT INTO t VALUES (3, 'carol', 88.25);

-- 不带 WHERE：改所有行
UPDATE t SET score = 0;
SELECT * FROM t;

-- 带 WHERE：只改命中的
UPDATE t SET score = 60 WHERE id = 2;
SELECT * FROM t;

-- 几个奇怪的但必须支持的写法
-- 1) SET 右边可以引用旧值
UPDATE t SET score = score + 10 WHERE id = 2;
SELECT id, score FROM t;

-- 2) SET 里同时改两列，右边的 score 读到的还是**改之前**的值。
--    这就是 SQL 的语义：整条 UPDATE 是一个原子动作，
--    不能出现"改完 name 才发现 score 已经被改了"的中间态。
UPDATE t SET name = 'CHANGED', score = score + 1 WHERE id = 3;
SELECT * FROM t;

-- 3) 同一列写两次，后面的赢
UPDATE t SET score = 1, score = 2 WHERE id = 1;
SELECT id, score FROM t;

-- 4) 一列都没命中：算成功，但报 0 行
UPDATE t SET score = 99 WHERE id = 999;
SELECT * FROM t;

-- 5) 没有 WHERE 也没有匹配问题：整表清空式更新
UPDATE t SET name = 'x';
SELECT * FROM t;
