-- 关键用例：表是空的。列名照样要查出来——
-- 如果先扫行再验列名，"表里没数据"就会把这个错误吞掉，
-- 变成静悄悄返回 0 行。
CREATE TABLE t (id INT, name TEXT);
SELECT * FROM t WHERE age > 1;
