CREATE TABLE t (name TEXT);
INSERT INTO t VALUES ('a');
-- 文本不能参与算术，不做隐式转换。
-- 这跟 Nova 里 `1 + "a"` 直接报错是同一条原则：
-- 静悄悄地给个答案，错误会跑到离现场很远的地方才发作。
SELECT name + 1 FROM t;
