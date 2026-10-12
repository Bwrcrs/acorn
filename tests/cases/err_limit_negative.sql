CREATE TABLE t (id INT);
INSERT INTO t VALUES (1);
-- LIMIT 只接受非负整数。负数在解析阶段就拦住了。
SELECT id FROM t LIMIT -1;
