CREATE TABLE t (a INT);
INSERT INTO t VALUES (9223372036854775807);
INSERT INTO t VALUES (9223372036854775807);
-- 聚合求和同样会溢出，同样报错。
-- 注意报错信息里给出的是"累加到多少时又加了多少"——
-- 聚合是个累积过程，指不出"哪两个数"加爆的，
-- 但给出这两个数足够用户去表里定位了。
SELECT SUM(a) FROM t;
