-- 整行注释
CREATE TABLE t (id INT, /* 行内注释 */ name TEXT);  -- 行尾注释
INSERT INTO t VALUES (1, 'a'); /* 跨
   多行
   的注释 */
SELECT * FROM t;
