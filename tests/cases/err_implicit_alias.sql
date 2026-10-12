-- AS 只能用在 SELECT 的选择列表里。
-- 这里故意不支持省略 AS 的写法（`SELECT score s FROM t`），
-- 因为那个会让一个常见笔误静悄悄地合法：
--     SELECT name score FROM t     -- 少打了一个逗号
-- 用户想选两列，实际得到一列叫 score 的 name，而且不报错。
-- 所以下面这条**应该**报语法错误，而不是被当成别名。
CREATE TABLE t (id INT, name TEXT);
SELECT name id FROM t;
