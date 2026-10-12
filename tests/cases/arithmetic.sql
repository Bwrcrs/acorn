CREATE TABLE t (a INT, b INT, x REAL);
INSERT INTO t VALUES (7, 2, 1.5);

-- 五种算术运算
SELECT a + b, a - b, a * b, a / b, a % b FROM t;

-- 整数除法**向零截断**，不是四舍五入，也不是向下取整。
-- 7/2 = 3（不是 3.5，也不是 4）。
-- 这条和 Postgres / SQLite 一致；Python 的 // 是向下取整（-7//2 = -4），
-- 别把那个习惯带过来。
SELECT a / b, -a / b FROM t;

-- 一边是浮点，结果就是浮点
SELECT a + x, a * x, x / b FROM t;

-- 优先级：* / % 比 + - 高，括号最狠
-- 下面四个的结果全不一样，正好把优先级表钉住
SELECT 1 + 2 * 4, (1 + 2) * 4, 1 + 2 * 4 + 5, (1 + 2) * (4 + 5) FROM t;

-- 取模的符号跟着**被除数**走（和 C++ 的 % 一致）
SELECT -7 % 2, 7 % -2, -7 % -2 FROM t;

-- 一元负号。注意 `-a * b` 是 `(-a) * b` 而不是 `-(a * b)`——
-- 这两个结果一样，看不出区别，所以另找个能区分的：
-- 一元负号的优先级比乘法高，`-a + b` 是 `(-a) + b`
SELECT -a + b FROM t;

-- 连续的一元负号
SELECT - -a, -(-a) FROM t;

-- NULL 传染：任何一边是 NULL，整个算术结果就是 NULL
CREATE TABLE n (a INT);
INSERT INTO n VALUES (NULL);
SELECT a + 1, a * 0, -a, a % 2 FROM n;
