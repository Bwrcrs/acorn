CREATE TABLE t (id INT);
INSERT INTO t VALUES (1);
-- UPDATE 的 SET 也要过类型检查。
-- 而且**必须先查再改**：反过来的话这一行会先被改坏，
-- 再在类型检查里抛异常——那一行就永久脏了。
UPDATE t SET id = 'abc' WHERE id = 1;
