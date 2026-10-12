-- 阶段 4：索引 + 类型 + NULL。
--
-- 值和值比大小，前提是"能比"。这一份把三种类型和 NULL
-- 都过一遍，重点看**边界上那些看着像能比、其实不能比**的组合。

CREATE TABLE p (id INT, name TEXT, score REAL, memo TEXT);
INSERT INTO p VALUES (1, 'apple', 1.5, NULL);
INSERT INTO p VALUES (2, 'banana', 2.5, 'hi');
INSERT INTO p VALUES (3, 'cherry', 10.0, NULL);
INSERT INTO p VALUES (4, 'date', 2.5, 'yo');
INSERT INTO p VALUES (5, NULL, NULL, NULL);

CREATE INDEX p_name ON p (name);
CREATE INDEX p_score ON p (score);
CREATE INDEX p_memo ON p (memo);

-- TEXT 列上的等值和区间。TEXT 比的是字节序（见 value.cpp），
-- 所以 'apple' < 'banana' < 'cherry' < 'date'。
SELECT id FROM p WHERE name = 'banana';
SELECT id FROM p WHERE name > 'banana';
SELECT id FROM p WHERE name >= 'b' AND name < 'd';
SELECT id FROM p WHERE name < 'b';
SELECT id FROM p WHERE name = 'nope';

-- REAL 列
SELECT id FROM p WHERE score = 2.5;
SELECT id FROM p WHERE score > 2.5;
SELECT id FROM p WHERE score >= 2.5 AND score <= 10.0;
SELECT id FROM p WHERE score < 0;

-- 【整数和浮点混着比】score > 2 里的 2 是 INTEGER，
-- 而列是 REAL。规划器必须认这个组合（REAL 列能装下任何数字），
-- 不然这条查询会悄悄退回全表扫——结果对，但白测了。
-- 反过来 `id > 2.5` 里的 2.5 是 REAL，而 id 是 INT 列，
-- 规划器**不该**用它走索引（INT 和 REAL 不是同一个域）。
SELECT id FROM p WHERE score > 2;
SELECT id FROM p WHERE score >= 10;
SELECT id FROM p WHERE id > 2.5;
SELECT id FROM p WHERE id >= 2.5;

-- NULL 键：进得了索引，但等值永远匹配不上（三值逻辑）。
-- 这一条要的结果是**空**，不是崩，也不是把 NULL 行捞出来。
SELECT id FROM p WHERE name = 'apple';
SELECT id FROM p WHERE name IS NULL;
SELECT id FROM p WHERE name IS NOT NULL;
SELECT id FROM p WHERE memo IS NULL;
SELECT id FROM p WHERE memo IS NOT NULL;
SELECT id FROM p WHERE memo = 'hi';

-- NULL 和普通值混在同一列里，区间扫描得跳过 NULL 而不是把它当最小值
SELECT id FROM p WHERE name > 'a';
SELECT COUNT(*) FROM p WHERE name > 'a';

-- 空表上建索引，再用它查：不能崩，也不能查出东西
CREATE TABLE empty (k INT, v TEXT);
CREATE INDEX empty_k ON empty (k);
SELECT COUNT(*) FROM empty WHERE k = 1;
SELECT id FROM p WHERE score > 100;
