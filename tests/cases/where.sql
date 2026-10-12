CREATE TABLE t (id INT, name TEXT, score REAL);
INSERT INTO t VALUES (1, 'alice', 91.5);
INSERT INTO t VALUES (2, 'bob', 78.0);
INSERT INTO t VALUES (3, 'carol', 88.25);
SELECT * FROM t WHERE score > 80;
SELECT * FROM t WHERE score > 80 AND name <> 'carol';
SELECT * FROM t WHERE name = 'bob' OR name = 'carol';
SELECT * FROM t WHERE NOT (score >= 88);
-- 括号改变结合方式：AND 比 OR 紧，这里必须先算右边的 OR
SELECT * FROM t WHERE (score > 90 OR score < 80) AND id = 2;
