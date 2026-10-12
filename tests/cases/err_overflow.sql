CREATE TABLE t (a INT);
INSERT INTO t VALUES (1);
-- 9223372036854775807 是 64 位有符号整数的上限。
-- 加 1 溢出——这里主动停下来报错，而不是回绕成负数。
-- C++ 里 signed 溢出是**未定义行为**（不是"回绕"，
-- 回绕是 unsigned 才有的保证），所以不能放着不管。
SELECT 9223372036854775807 + 1 FROM t;
