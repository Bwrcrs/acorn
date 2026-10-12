CREATE TABLE t (id INT, name TEXT);
INSERT INTO t VALUES (1, 'a');
-- REAL 不能塞进 INT 列（反过来可以，见 types.sql）
INSERT INTO t VALUES (1.5, 'b');
