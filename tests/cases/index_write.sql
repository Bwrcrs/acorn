-- 阶段 4：写路径上的索引维护。
--
-- 【为什么这一份最要紧】
-- 索引和表对不上是**不报错**的坏：查询静悄悄地少返回几行，
-- 或者返回一行根本不该匹配的行，程序一切正常。
-- 所以每一次写之后都要**用索引把它们查回来**——
-- 只查表（全表扫）是看不出索引坏没坏的，那等于没测。

CREATE TABLE t (id INT, k INT, pad TEXT);
INSERT INTO t VALUES (1, 10, 'a'), (2, 20, 'b'), (3, 30, 'c'), (4, 40, 'd'), (5, 50, 'e');

CREATE INDEX t_k ON t (k);

-- 改索引列：老键要按**老值**删掉，新键要按新值插进去。
-- 只做一半的话：老键还在（查 k = 30 会捞出 id = 3，可它已经不是 30 了），
-- 或者新键没进（查 k = 100 什么都查不到）。
UPDATE t SET k = 100 WHERE id = 3;
SELECT id FROM t WHERE k = 100;
SELECT id FROM t WHERE k = 30;
SELECT id, k FROM t WHERE k >= 10;

-- 改一个**不是**索引列的列：索引项一个字都不该动
UPDATE t SET pad = 'zzz' WHERE id = 1;
SELECT id FROM t WHERE k = 10;
SELECT id, pad FROM t WHERE k = 10;

-- 一次改多行，而且改的正是用来找它们的那个列。
-- （执行器是**先把要改的行全收集起来**再改的。边扫边改的话，
--   索引在被扫描的过程中就变了，会漏行或者重复改——见 executor.cpp。）
UPDATE t SET k = k + 1 WHERE k >= 40;
SELECT id, k FROM t WHERE k >= 40 ORDER BY k;

-- 用索引找到行，再把它们删掉；删完索引里也不能剩下
DELETE FROM t WHERE k = 100;
SELECT id FROM t WHERE k = 100;
SELECT COUNT(*) FROM t WHERE k >= 0;

-- 删掉最边上的那一行（索引里第一个键），链子不能断
SELECT MIN(k) FROM t;
DELETE FROM t WHERE k = 10;
SELECT id, k FROM t WHERE k >= 0 ORDER BY k;

-- 一个都删不掉的时候，索引也不该被动过
DELETE FROM t WHERE k = 999;
SELECT id, k FROM t WHERE k >= 0 ORDER BY k;

-- 全删光，再用索引查：空表不能崩
DELETE FROM t;
SELECT COUNT(*) FROM t WHERE k >= 0;
SELECT id FROM t WHERE k = 20;

-- 删空之后再插，索引照样能用
INSERT INTO t VALUES (9, 9, 'nine');
SELECT id FROM t WHERE k = 9;
SELECT COUNT(*) FROM t WHERE k >= 0;

-- 表都删了，索引跟着走。这条要是没连带删，索引里存的行号
-- 指向的就是还给空闲链的页——那些页随时会被别的表拿去用。
DROP TABLE t;
CREATE TABLE t (z INT);
INSERT INTO t VALUES (7);
SELECT * FROM t;
