-- 阶段 5：事务的三条路 —— 提交、回滚、以及"没写 BEGIN 时每条语句自己提交"。
--
-- 内存库跑这个用例，所以看得见的只有**逻辑上**的结果：
-- 提交之后改动在，回滚之后改动不在。页怎么退回去、日志怎么写，
-- 那些是 disk/ 那批用例的事（那边才跨得过进程边界）。

CREATE TABLE t (id INT, name TEXT);
INSERT INTO t VALUES (1, 'base');

-- ---- 提交：改动留下 ----
BEGIN;
INSERT INTO t VALUES (2, 'committed');
UPDATE t SET name = 'updated' WHERE id = 1;
COMMIT;
SELECT * FROM t ORDER BY id;

-- ---- 回滚：改动全没 ----
BEGIN;
INSERT INTO t VALUES (3, 'rolled back');
UPDATE t SET name = 'gone' WHERE id = 1;
DELETE FROM t WHERE id = 1;
ROLLBACK;
SELECT * FROM t ORDER BY id;

-- ---- 没写 BEGIN：每条语句自己提交 ----
-- 这一条和上面两条合起来说明"自动提交"到底是什么意思：
-- 没有 BEGIN 的时候，语句和语句之间就是彼此独立的事务。
INSERT INTO t VALUES (4, 'autocommit');
SELECT * FROM t ORDER BY id;

-- ---- 一个空事务 ----
-- BEGIN 之后什么都不做就 COMMIT，不该改变任何东西，
-- 也不该报错。这是最容易写出边界 bug 的一条路径。
BEGIN;
COMMIT;
BEGIN;
ROLLBACK;
SELECT * FROM t ORDER BY id;
