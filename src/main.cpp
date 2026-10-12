// ============================================================
//  main —— REPL / 跑脚本 / 调试开关
//
//  三种用法：
//      acorn                        进 REPL，一行行敲
//      acorn script.sql             跑一个脚本文件
//      acorn -e "SELECT 1 + 1"      直接跑一段 SQL
//
//  上面三种默认都是**内存库**，进程一退数据就没了。要落盘就加 -d：
//      acorn -d my.acorn                     进 REPL，数据存到 my.acorn
//      acorn -d my.acorn -e "INSERT ..."     跑一段，结果留在 my.acorn
//      acorn -d my.acorn script.sql          跑个脚本，结果留在 my.acorn
//
//  【为什么默认是内存库，而不是"当前目录下建一个文件"】
//  因为测试。每个 .sql 用例都要跑一遍，要是默认落盘，第二次跑就会
//  看见上一次的数据，基准输出根本没法写。默认内存 = 每次都是干净的
//  一张白纸。SQLite 里对应的东西是 `:memory:`，它本来也是正经用法。
//
//  两个调试开关（Nova 里也有同款，做语法分析时救命）：
//      acorn --tokens script.sql    只做词法分析，打印 Token 流
//      acorn --ast    script.sql    只做语法分析，打印语法树
//
//  事务（阶段 5）：
//      BEGIN;  ...  COMMIT;         显式事务，中间的改动一起生效
//      BEGIN;  ...  ROLLBACK;       或者一起撤销
//      不写 BEGIN 的话，每条语句自己就是一个事务（自动提交）。
//
//  还有一个**测试专用**的崩溃开关：
//      acorn -d x.acorn --crash-at=commit script.sql
//  它会让程序在一个事务提交到一半的地方直接死掉（不跑析构、不刷缓冲），
//  用来验证崩溃恢复真的能把库退回去。正常用不到，见 pager.hpp 的说明。
//
//  并发（阶段 6）：同一个库文件可以被好几个进程同时打开——
//  读的可以一起读，写的独占。抢不到就报错，不想立刻失败可以等：
//      acorn -d x.acorn --lock-wait=2000 -e "INSERT ..."
//  机制是操作系统给的文件锁，见 lock.hpp。
// ============================================================

#include "ast.hpp"
#include "ast_printer.hpp"
#include "database.hpp"
#include "error.hpp"
#include "executor.hpp"
#include "lock.hpp"
#include "parser.hpp"
#include "result.hpp"
#include "text.hpp"
#include "token.hpp"
#include "tokenizer.hpp"
#include "utf8path.hpp"
#include "value.hpp"

#include <cstddef>
#include <cstdio>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
// ★ 这两个头文件放在所有**项目头文件之后**，这个顺序不能随便动。
//
// <windows.h> 会定义一大堆宏（TRUE / FALSE / NULL / min / max ...），
// 而宏是编译前的纯文本替换，会污染它后面出现的**每一个**头文件——
// 它不认命名空间、不认类、不认 enum。
//
// 放在最后，项目头文件就完全碰不到这些宏。
// （Nova 里 `enum class TokenKind { ... FALSE ... }` 被
//   <windows.h> 的 `#define FALSE 0` 换成了 `0`，直接编译不过，
//   最后靠改名叫 TRUE_KW / FALSE_KW 才绕过去。Acorn 的
//   token.hpp 一开始就用了 _KW 后缀，value.hpp 也把 NULL 叫成
//   NULL_VALUE，是同一个教训的两次应用。）
//
// 这三个头文件的**顺序**也有讲究：<windows.h> 必须排第一。
// <shellapi.h> 里用到了 windows.h 定义的 EXTERN_C、DECLARE_HANDLE
// 这些宏，排在前面会一路报"XX does not name a type"——
// 错误信息全指向 shellapi.h 自己，看不出真正的原因是顺序。
// （习惯性地按字母序排 include，在这里会踩坑。）
#include <windows.h>

#include <io.h>
#include <shellapi.h>
#endif

#include <cwchar>

namespace {

// ------------------------------------------------------------
//  输出
// ------------------------------------------------------------

// 往标准错误打一行。
//
// 【为什么要先 flush std::cout】
// cout 是**带缓冲**的，cerr 是**不带缓冲**的。两个流汇到同一个地方
//（管道、文件、`2>&1`）时，cerr 的内容会立刻写出去，
// 而 cout 里还攒着一批没发的。结果就是错误信息跑到它前面那批
// 正常输出的**前面**去了——看起来像"错误发生在还没执行到的地方"，
// 能把人查疯。
//
// 先 flush 一下 cout，两条流的先后顺序就对了。
void printError(const std::string& message) {
    std::cout.flush();
    std::cerr << message << "\n";
}

// ------------------------------------------------------------
//  平台相关的小事
// ------------------------------------------------------------

void setupConsole() {
#ifdef _WIN32
    // Windows 控制台默认用 GBK（本机是简体中文），直接输出 UTF-8 会是乱码。
    // 这两行把控制台的**输出**和**输入**都切成 UTF-8，
    // 配合编译参数 -fexec-charset=UTF-8 才能正常显示中文。
    //
    // 输入那行（SetConsoleCP）同样不能少：不然 REPL 里敲中文会坏掉。
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif
}

// 标准输入是不是连着终端（而不是被重定向/被管道接了）。
//
// 用来决定 REPL 要不要打提示符。被重定向时（`acorn < script.sql`
// 或者测试脚本喂数据）打出来的 "acorn> " 会混进正常输出里，
// 让"比对输出"这件事变得很麻烦——测试得去过滤提示符，
// 而过滤规则本身又可能出错。
// 干脆不打，让重定向时的输出就是纯粹的查询结果。
bool stdinIsTerminal() {
#ifdef _WIN32
    return _isatty(_fileno(stdin)) != 0;
#else
    return isatty(fileno(stdin)) != 0;
#endif
}

// ------------------------------------------------------------
//  命令行参数
// ------------------------------------------------------------

#ifdef _WIN32

// 宽字符（UTF-16）转成 UTF-8。
std::string wideToUtf8(const wchar_t* wide) {
    if (wide == nullptr) return std::string();

    const int length = static_cast<int>(std::wcslen(wide));
    if (length == 0) return std::string();

    // 先问需要多少字节，再开缓冲区——不能拍一个长度猜。
    // 中文在 UTF-16 里占 1 个 wchar_t，转成 UTF-8 要 3 个字节，
    // 按 wcslen 开缓冲区会短三倍。
    const int size = WideCharToMultiByte(CP_UTF8, 0, wide, length,
                                         nullptr, 0, nullptr, nullptr);
    if (size <= 0) return std::string();

    std::string out(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide, length, &out[0], size, nullptr,
                        nullptr);
    return out;
}

// 重新取一遍命令行参数，这次走宽字符。
//
// ★ 为什么要多此一举 ★
//
// Windows 上 `int main(int argc, char** argv)` 收到的 argv 是
// **窄字符**版本，而它是把命令行的 UTF-16 按**当前 ANSI 代码页**
// 转过来的。本机代码页是 936（GBK），所以：
//
//     acorn -e "SELECT * FROM 学生;"
//
// 里面的「学生」在程序里就成了乱码——而且是那种报错信息里
// 显示成一堆问号的乱码，让人完全摸不着头脑。
//
// 命令行本身其实一直是好的（Windows 内部是 UTF-16），
// 信息是在"转成窄字符"那一步丢的，而且**丢了就找不回来**。
// 所以唯一的办法是绕开 argv，自己去拿宽字符版：
// GetCommandLineW() 拿原始命令行，
// CommandLineToArgvW() 按 Windows 的规则切开它。
//
// 这也是为什么 `acorn <文件.sql>` 读中文没事，只有 `-e` 会坏：
// 文件是按字节读的，压根不经过 argv。
std::vector<std::string> commandLineArgs() {
    int count = 0;
    LPWSTR* wideArgv = CommandLineToArgvW(GetCommandLineW(), &count);
    if (wideArgv == nullptr) return std::vector<std::string>();

    std::vector<std::string> args;
    args.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; i++) {
        args.push_back(wideToUtf8(wideArgv[i]));
    }

    // CommandLineToArgvW 是用 LocalAlloc 分配的，得用 LocalFree 还回去。
    // 它返回的是个 LPWSTR*（指针数组），不是个对象，
    // 所以没有析构函数帮你做这件事——必须手写。
    LocalFree(wideArgv);
    return args;
}

#endif  // _WIN32

// 拿到全部的**非 argv[0]** 参数。
std::vector<std::string> collectArgs(int argc, char** argv) {
    std::vector<std::string> args;

#ifdef _WIN32
    args = commandLineArgs();
    if (!args.empty()) {
        args.erase(args.begin());  // 去掉第 0 个，那是程序自己的路径
        return args;
    }
    // 万一拿不到宽字符命令行（理论上不会），退回普通的 argv。
    // 大不了中文乱码，总比什么都不做好。
#endif

    for (int i = 1; i < argc; i++) {
        args.push_back(argv[i]);
    }
    return args;
}

// ------------------------------------------------------------
//  读文件
// ------------------------------------------------------------

// 剥掉 UTF-8 BOM（开头那三个字节 EF BB BF）。
//
// 记事本存 UTF-8 文件时会自动加上这三个字节，用户在屏幕上
// **完全看不见**。不剥的话，第一行会多出三个隐形字符，
// 报错会说"认不出来的字符"——而那个字符根本不在屏幕上，
// 盯着看半小时也找不出来。
std::string stripUtf8Bom(const std::string& text) {
    if (text.size() >= 3 &&
        static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB &&
        static_cast<unsigned char>(text[2]) == 0xBF) {
        return text.substr(3);
    }
    return text;
}

std::string readFile(const std::string& path) {
    // 【为什么不用 std::ifstream(path)】和库文件那边是同一个坑：
    // Windows 的窄字符文件接口按系统 ANSI 代码页（本机 GBK）解释
    // 路径，而这里传的是 UTF-8。于是 `acorn 中文脚本.sql` 会去找
    // 一个名字完全不同的文件，报"打不开"。见 utf8path.hpp。
    //
    // （ifstream 在 MinGW 上确实有个收 wchar_t* 的重载，但那是
    //   实现给的，不是标准里的——标准里没有可移植的"用宽路径开流"
    //   的办法。直接读字节反而是最稳的。）
    std::FILE* file = openPath(path, "rb");
    if (file == nullptr) {
        throw AcornError("打不开文件：" + path);
    }

    std::string text;
    char buffer[4096];
    std::size_t got = 0;
    while ((got = std::fread(buffer, 1, sizeof(buffer), file)) > 0) {
        text.append(buffer, got);
    }
    const bool failed = std::ferror(file) != 0;
    std::fclose(file);

    if (failed) {
        throw AcornError("读文件读到一半失败了：" + path);
    }
    return stripUtf8Bom(text);
}

// ------------------------------------------------------------
//  输出
// ------------------------------------------------------------

// 把查询结果画成一张表格。
//
//     +----+-------+-------+
//     | id | name  | score |
//     +----+-------+-------+
//     | 1  | alice | 95.5  |
//     | 2  | bob   | 88    |
//     +----+-------+-------+
//     (2 行)
//
// 这里**不能用 printf 的 %-10s 或者 std::setw** 来对齐：
// 那两个都是按字节数补空格的，而中文在终端里占两格、内存里占三字节，
// 三者对不上。所以宽度得自己用 displayWidth 算，补空格也得自己补。
void printResultSet(const ResultSet& result) {
    if (!result.isQuery) {
        // CREATE / INSERT 之类的反馈话术
        std::cout << result.message << "\n";
        return;
    }

    // ---- 先算出每一列要多宽 ----
    // 取"表头宽度"和"这一列所有单元格宽度"里的最大值。
    std::vector<int> widths(result.columnNames.size(), 0);
    for (size_t i = 0; i < result.columnNames.size(); i++) {
        widths[i] = displayWidth(result.columnNames[i]);
    }

    for (const std::vector<Value>& row : result.rows) {
        for (size_t i = 0; i < row.size() && i < widths.size(); i++) {
            const int width = displayWidth(row[i].toString());
            if (width > widths[i]) widths[i] = width;
        }
    }

    // 单元格左右各留一个空格，所以每列至少要有 1 格内容宽，
    // 这样空表头也不会画出一个没有宽度的怪框
    for (int& width : widths) {
        if (width < 1) width = 1;
    }

    const auto separator = [&widths]() {
        std::cout << "+";
        for (int width : widths) {
            std::cout << std::string(static_cast<size_t>(width + 2), '-') << "+";
        }
        std::cout << "\n";
    };

    const auto line = [&widths](const std::vector<std::string>& cells) {
        std::cout << "|";
        for (size_t i = 0; i < widths.size(); i++) {
            const std::string text = i < cells.size() ? cells[i] : std::string();
            std::cout << " " << padRight(text, widths[i]) << " |";
        }
        std::cout << "\n";
    };

    separator();

    std::vector<std::string> header;
    header.reserve(result.columnNames.size());
    for (const std::string& name : result.columnNames) header.push_back(name);
    line(header);

    separator();
    for (const std::vector<Value>& row : result.rows) {
        std::vector<std::string> cells;
        cells.reserve(row.size());
        for (const Value& value : row) {
            // toString 而不是 toSqlLiteral：给用户看的表格里
            // 不希望文本值两边挂着一对引号。
            cells.push_back(value.toString());
        }
        line(cells);
    }
    separator();

    std::cout << "(" << result.rows.size() << " 行)\n";
}

// ------------------------------------------------------------
//  执行一段 SQL
// ------------------------------------------------------------

void runSource(Database& db, const std::string& source, bool showTokens,
               bool showAst) {
    Tokenizer tokenizer(source);
    std::vector<Token> tokens = tokenizer.scanTokens();

    if (showTokens) {
        for (const Token& token : tokens) {
            // 末尾那个 END_OF_FILE 打出来只是噪音
            if (token.kind == TokenKind::END_OF_FILE) continue;
            std::cout << tokenToString(token) << "\n";
        }
        return;
    }

    Parser parser(std::move(tokens));
    std::vector<StmtPtr> statements = parser.parseProgram();

    if (showAst) {
        AstPrinter printer;
        for (const StmtPtr& stmt : statements) {
            std::cout << printer.printStmt(*stmt) << "\n";
        }
        return;
    }

    Executor executor(db);
    for (const StmtPtr& stmt : statements) {
        // ---- 自动提交：一条语句 = 一个事务 ----
        //
        // 【事务是在哪开的】不在这儿。是这条语句第一次要改页的时候，
        // 由 Pager::getForWrite 顺手开的（见那边的说明）。
        // 这样写有个很实在的好处：**`BEGIN` 不会被包在一个事务里**。
        // 要是这里无脑 `db.begin()`，用户敲的 BEGIN 就会撞上
        // "已经在事务里了"。
        //
        // 【这里负责的是收尾】语句跑完了，如果那个事务是"语句自己开的"
        //（而不是用户 BEGIN 的），就替它提交。
        // 用户 BEGIN 开的那个绝不能替它提交——那是灾难：
        // 后面所有该被撤销的改动会当场生效。
        try {
            printResultSet(executor.execute(*stmt));
        } catch (...) {
            // ★ 阶段 5 兑现阶段 3 的那张欠条 ★
            //
            // 阶段 3 的注释在这儿写着："`UPDATE t SET x = 'abc'` 改到
            // 第 3 行报类型错时，前 2 行已经改了，而且下一次 flush
            // 会把它们一起写出去。回滚要等阶段 5。"
            // 现在就是阶段 5：改过的页都在回滚日志里，退得回去。
            //
            // 【只回滚隐式事务】显式事务（用户敲了 BEGIN）出错时不动它——
            // 那是用户的事务，可能只是这一条语句写错了，
            // 改一下重敲就行，前面的改动还该留着。这也和 SQLite 一致。
            if (db.inTransaction() && db.transactionIsImplicit()) {
                db.rollback();
            }
            throw;
        }

        if (db.inTransaction() && db.transactionIsImplicit()) {
            db.commit();
        }
    }
}

// 跑一段 SQL 并处理错误，返回进程退出码。
//
// 【catch 的顺序有讲究】SyntaxError 是 AcornError 的子类，
// 所以必须先 catch 子类。反过来的话，语法错误会掉进
// AcornError 那个分支，丢掉行列号和源码高亮——
// 而且编译器不会提醒你，它只会说"这个 catch 永远不会执行"，
// 那不是错误，是提示。
int execute(Database& db, const std::string& source, bool showTokens,
            bool showAst) {
    try {
        runSource(db, source, showTokens, showAst);
        return 0;
    } catch (const SyntaxError& error) {
        // 语法错误有精确位置，把出错那行和箭头一起打出来
        printError(formatSyntaxError(source, error));
        return 1;
    } catch (const BusyError& error) {
        // 【为什么它要排在 AcornError 前面】因为它**是**一种 AcornError，
        // 顺序反了就会被上面那条接走，退出码变成 1。
        //
        // 用 2 而不是 1，理由和"开库时被占着"完全一样：这不是 SQL 的错，
        // 是"现在轮不上"。一个脚本据此决定"过一会儿再来"还是"别白费劲了"，
        // 而同一个问题在开库时和在这里报出不同的数，那个判断就没法写。
        // （"库被占着"为什么单独一个类型，见 error.hpp 里的 BusyError。）
        printError("错误：" + std::string(error.what()));
        return 2;
    } catch (const AcornError& error) {
        // 执行错误没有位置可言（SQL 本身是合法的，只是跑不下去），
        // 只给一句话
        printError("错误：" + std::string(error.what()));
        return 1;
    }
}

// 退出前，事务还开着就提醒一句。
//
// 【为什么值得专门提醒】
// 因为"改动被撤销"这件事**屏幕上没有任何地方说过**：
// 用户敲了一堆 INSERT、看一眼数据都在、退出、再进来——没了。
// 而这期间没有一句报错、没有一个不正常的提示。
// （SQLite 的语义就是这样，它也确实是这么做的：析构时回滚。）
//
// 【为什么不干脆替他提交】那更糟。一个"退出就自动保存"的数据库，
// 会让"没提交 = 还没定"这句话变成谎话——而事务的全部价值
// 就在于"没提交的改动是可以不要的"。所以只提醒，不动手。
void warnOpenTransaction(const Database& db) {
    if (!db.inTransaction()) return;
    printError("提醒：事务还开着，没有 COMMIT。进程退出会把里面的改动"
               "全部撤销（想留下来就敲 COMMIT）。");
}

// 给 -e 和 REPL 用：句末没写分号就补一个。
//
// 人机交互时末尾那个 ';' 是纯噪音——敲 `SELECT * FROM users`
// 就该能跑。但跑脚本文件不补：文件里应该写完整的 SQL，
// 少个分号是用户该发现的错。
std::string ensureTrailingSemicolon(const std::string& sql) {
    const size_t end = sql.find_last_not_of(" \t\r\n");
    if (end == std::string::npos) return sql;      // 全是空白
    if (sql[end] == ';') return sql;
    return sql + ";";
}

// 解析一个非负整数，用来读 --lock-wait= 和 --hold= 后面那个毫秒数。
//
// 【为什么不用 std::stoi】因为它把 "12abc" 解析成 12 而不报错，
// 也把 "" 抛成异常。这两种在这儿都得是**错误**：
// `--lock-wait=12abc` 不是"等 12 毫秒"，是打字打歪了，
// 而按 12 毫秒跑完一个"通过"的测试，比直接报错危险得多。
//
// 【为什么要封顶】因为 `--lock-wait=999999999` 这种多半也是打错了，
// 而它真的会让程序在那里干等好几天。一小时的等待已经远超
// 任何正经用途，超过就当错误报出来。
bool parseMilliseconds(const std::string& text, int& out) {
    if (text.empty() || text.size() > 9) return false;

    long long value = 0;
    for (const char ch : text) {
        if (ch < '0' || ch > '9') return false;
        value = value * 10 + (ch - '0');
    }
    if (value > 3600000) return false;  // 一小时

    out = static_cast<int>(value);
    return true;
}

// 测试专用：干完活之后先别退，把库多开一会儿（阶段 6）。
//
// 【为什么不直接 sleep，非要打一行字】因为测试脚本要**等到**
// 这个进程真的把锁拿到手了，才能让第二个进程上去撞。
// 靠"睡个 500 毫秒大概就够了吧"来对齐两个进程，是在写一个
// 有时过有时不过的测试——那种测试比没有测试更糟（它训练人
// 不信测试）。所以这里明说一句"我准备好了"，测试去等这句话。
//
// 【那一句 flush 是必须的】stdout 被重定向到文件时是**块缓冲**的，
// 那句话会一直待在缓冲区里，等进程退出才写出去——而那时候
// "等另一句话"的人早就等超时了。一个"我准备好了"的信号
// 如果发不出去，那它就不是信号。
void holdIfRequested(int holdMs) {
    if (holdMs <= 0) return;

    std::cout << "[--hold] 库还开着，锁还拿着，等 " << holdMs
              << " 毫秒再退。\n";
    std::cout.flush();
    sleepMilliseconds(holdMs);
}

// ------------------------------------------------------------
//  REPL 的元命令（以 . 开头，不是 SQL）
// ------------------------------------------------------------

std::string createTableSql(const TableSchema& schema) {
    std::string out = "CREATE TABLE " + schema.name + " (";
    for (size_t i = 0; i < schema.columns.size(); i++) {
        if (i > 0) out += ", ";
        out += schema.columns[i].name;
        out += " ";
        out += sqlTypeName(schema.columns[i].type);
    }
    out += ");";
    return out;
}

// 把一条索引定义还原成 CREATE INDEX 语句。
//
// 【为什么用 CREATE INDEX 这种写法，而不是 describeIndex 那句话】
// `.schema` 这个命令的用途是"把库的结构原样抄出来，让我能照着
// 重建一个"。那就得是**能直接跑的 SQL**——`索引 i（建在...）`
// 读起来舒服，但粘不回命令行。
// （describeIndex 那句是给报错用的，那里要的是好读，不是能跑。
//   两种用途，两种写法，各用各的。）
std::string createIndexSql(const IndexSchema& index,
                           const TableSchema& table) {
    std::string columnName = "?";
    if (index.column >= 0 &&
        static_cast<size_t>(index.column) < table.columns.size()) {
        columnName = table.columns[static_cast<size_t>(index.column)].name;
    }
    return "CREATE INDEX " + index.name + " ON " + table.name + " (" +
           columnName + ");";
}

// 打印一张表的建表语句，后面跟着它的每一条索引。
// `.schema` 的两个分支（全部表 / 某一张表）共用这一份。
void printTableSchema(const Database& db, const TableSchema& schema) {
    std::cout << createTableSql(schema) << "\n";
    for (const IndexSchema& index : db.indexesOf(schema.name)) {
        std::cout << createIndexSql(index, schema) << "\n";
    }
}

void printHelp() {
    std::cout << "命令（都以点开头，后面可以跟分号也可以不跟）：\n"
                 "  .help            显示这份帮助\n"
                 "  .tables          列出所有表\n"
                 "  .indexes         列出所有索引\n"
                 "  .schema          显示所有表和索引的定义\n"
                 "  .schema <表名>   只显示某一张表（连着它的索引）\n"
                 "  .info            当前的库是哪个文件、页缓存什么状况\n"
                 "  .quit            退出（也可以直接敲 exit 或 quit）\n"
                 "\n"
                 "阶段 5 支持的 SQL：\n"
                 "  CREATE TABLE 名 (列名 类型, ...)     类型有 INT / REAL / TEXT / BOOL\n"
                 "  INSERT INTO 名 VALUES (...), (...);\n"
                 "  SELECT * | 列 [AS 别名], ... FROM 名\n"
                 "         [WHERE 条件] [ORDER BY 键 [DESC], ...] [LIMIT n [OFFSET m]];\n"
                 "  UPDATE 名 SET 列 = 值, ... [WHERE 条件];\n"
                 "  DELETE FROM 名 [WHERE 条件];\n"
                 "  CREATE INDEX 索引名 ON 表名 (列名);\n"
                 "  DROP INDEX 索引名;\n"
                 "  DROP TABLE 名;                      连带删掉它的所有索引\n"
                 "  BEGIN;  COMMIT;  ROLLBACK;          事务\n"
                 "\n"
                 "  选择列表里可以写表达式：SELECT score * 2 + 1 FROM 名\n"
                 "  聚合函数：COUNT(*) / COUNT(列) / SUM / AVG / MIN / MAX\n"
                 "  别名只在 ORDER BY 里能用（WHERE 里看不见，标准 SQL 也这样）\n"
                 "  判空：IS NULL / IS NOT NULL（不能写 = NULL，那是恒为 UNKNOWN 的）\n"
                 "  注意 ORDER / AS / DROP / INDEX / ON / BEGIN / COMMIT / ROLLBACK\n"
                 "  是保留字，不能拿来做名字\n"
                 "\n"
                 "事务是干什么的：\n"
                 "  BEGIN 和 COMMIT 之间的改动是一个整体——要么全算数，\n"
                 "  要么（敲 ROLLBACK，或者中途出错）全部当没发生过。\n"
                 "  没写 BEGIN 时，每条语句自己就是一个事务：跑到一半出错\n"
                 "  的那条会整个撤销，不会留下改了一半的数据。\n"
                 "  事务不能嵌套（BEGIN 里面不能再 BEGIN）。\n"
                 "\n"
                 "同时打开一个库的几个进程：\n"
                 "  读的能一起读，写的独占——任何时刻最多一个进程在写。\n"
                 "  别人占着的时候你这条语句会直接报错（不会静静地卡住），\n"
                 "  想等一会儿就加 --lock-wait=<毫秒>。\n"
                 "  ★ 这是加锁，不是快照：读会挡住写，写也会挡住读。\n"
                 "  敲 .info 能看到这个进程现在拿着什么锁。\n"
                 "\n"
                 "索引是干什么的：\n"
                 "  建了索引之后，`WHERE 那一列 = 值` 和 `WHERE 那一列 > 值`\n"
                 "  这种条件就不用把整张表读一遍了。查询结果和没建索引时\n"
                 "  **一模一样**，只是快——这件事是保证的，测在测试里。\n"
                 "  只有单列索引，而且只用在顶层 AND 里的条件上。\n"
                 "\n"
                 "数据存哪：不加 -d 就是内存库，关掉就没了。\n"
                 "          acorn -d my.acorn 才会存到文件里。\n"
                 "\n"
                 "调试开关（在命令行上用）：--tokens 看词法分析，--ast 看语法树\n"
                 "  （还有个测试专用的 --crash-at=commit|exit，故意让程序崩，\n"
                 "    用来验证崩溃恢复，正常用不上）\n";
}

// 返回 true 表示"该退出了"
bool handleMetaCommand(Database& db, const std::string& rawLine) {
    // 允许 `.tables;` 这种写法——用户写 SQL 写顺手了会带分号
    std::string line = rawLine;
    while (!line.empty() && (line.back() == ';' || line.back() == ' ')) {
        line.pop_back();
    }

    if (line == ".help" || line == ".h" || line == ".?") {
        printHelp();
        return false;
    }

    if (line == ".quit" || line == ".exit" || line == ".q" || line == "exit" ||
        line == "quit") {
        return true;
    }

    if (line == ".tables") {
        const std::vector<std::string> names = db.tableNames();
        if (names.empty()) {
            std::cout << "一张表都还没有。用 CREATE TABLE 建一张吧。\n";
            return false;
        }
        for (const std::string& name : names) {
            std::cout << name << "\n";
        }
        return false;
    }

    if (line == ".indexes") {
        const std::vector<std::string> names = db.indexNames();
        if (names.empty()) {
            std::cout << "一个索引都还没有。用 CREATE INDEX 建一个吧。\n"
                         "（表小的时候不建也行——索引是为了让查询不用把整张表读一遍）\n";
            return false;
        }
        // 一条索引一行，写成能直接粘回去的 CREATE INDEX。
        // 表结构查不出来这只可能是文件坏了，但那种情况下
        // schemaOf 会抛出一句能看懂的话，比在这儿打一串问号强。
        for (const std::string& name : names) {
            const IndexSchema* index = db.findIndex(name);
            if (index == nullptr) continue;
            std::cout << createIndexSql(*index, db.schemaOf(index->tableName))
                      << "\n";
        }
        return false;
    }

    if (line == ".schema") {
        const std::vector<std::string> names = db.tableNames();
        if (names.empty()) {
            std::cout << "一张表都还没有。\n";
            return false;
        }
        for (const std::string& name : names) {
            printTableSchema(db, db.schemaOf(name));
        }
        return false;
    }

    if (line.rfind(".schema ", 0) == 0) {
        // 把 ".schema " 后面那段当表名
        const std::string tableName = line.substr(8);
        printTableSchema(db, db.schemaOf(tableName));
        return false;
    }

    if (line == ".info") {
        // 阶段 3 加这个命令，是因为"数据到底存哪了"变成了一个
        // 真的会搞错的问题：内存库和文件库从 SQL 上看一模一样，
        // 敲 INSERT 都回一句"插入了 1 行"。等到关掉再打开才发现
        // 数据没了，那时候已经晚了。
        // 所以留一个能当场问清楚的地方。
        if (db.inMemory()) {
            std::cout << "库的位置：内存（进程一退就没了）\n"
                         "想留住数据，用 acorn -d <文件名> 重开\n";
        } else {
            std::cout << "库的位置：" << db.path() << "\n";
        }
        // 表数和索引数顺手报一下。索引是阶段 4 之后"查询为什么快
        // 或者为什么慢"的第一个该看的地方，放在这里比让人去数
        // CREATE INDEX 语句方便。
        std::cout << "表：" << db.tableNames().size() << " 张，索引："
                  << db.indexNames().size() << " 个\n";
        // 事务状态在 cacheStats 那句里（它本来就管着"页现在什么状况"，
        // 而"有个事务开着，改过的页退得回去"正是页的状况之一）。
        // 不在这儿另打一行：同一个事实有两处输出，就有一处会忘了改。
        std::cout << db.cacheStats() << "\n";

        // 锁单独一行，因为它回答的是**另一个**问题：
        // 上面那些说的都是"这个进程自己怎么样"，而锁说的是
        // "这个库现在能不能被别人用"。两件事，两行。
        // （阶段 6 之后，"我这条语句为什么写不进去"最常见的原因
        //   就是这一行——比翻来覆去检查 SQL 快多了。）
        std::cout << "锁：" << db.lockState() << "\n";
        return false;
    }

    std::cout << "不认识的命令：" << line << "。敲 .help 看看有哪些。\n";
    return false;
}

// ------------------------------------------------------------
//  REPL
// ------------------------------------------------------------

// 现在攒的这段，算不算一个写完的语句？
//
// 【为什么不看"文本里有没有分号"】
// 那个判断会把字符串字面量里的分号也数进去：
//     INSERT INTO t VALUES ('a;b')      ← 中间那个分号不是语句结束
// 词法分析器**已经知道**哪些分号在字符串里，直接问它就行，
// 不用在 REPL 这边再猜一遍。这也是"只有一处实现"的老原则：
// 分号的语义在 tokenizer 里已经定死了，这里是第二个地方去理解它，
// 两个理解早晚会分叉。
//
// 【返回 false 的两种意思要分开】
//  · IncompleteError —— 字符串或块注释没收尾。用户正打到一半，
//    静静继续攒，一个字都不报。
//  · 别的 SyntaxError —— 是真的写错了（比如孤零零一个 `!`）。
//    这里让它抛出去，由调用方报给用户并丢掉这段。
//    选择"立刻报"而不是"继续等"，是因为反过来更糟：
//    用户会对着一个永远不执行的提示符发呆，而且没有任何线索。
bool isStatementComplete(const std::string& source) {
    Tokenizer tokenizer(source);
    const std::vector<Token> tokens = tokenizer.scanTokens();

    // scanTokens() 保证末尾一定有一个 END_OF_FILE，
    // 所以"最后一个 Token"永远是它，得看倒数第二个。
    if (tokens.size() < 2) return false;
    return tokens[tokens.size() - 2].kind == TokenKind::SEMICOLON;
}

void repl(Database& db) {
    const bool interactive = stdinIsTerminal();

    if (interactive) {
        std::cout << "Acorn —— 从零手写的小数据库（阶段 5：事务）\n";

        // 打开的到底是哪个库、数据会不会留下，是进来第一件该知道的事。
        // 反过来说，"以为存下来了其实没有"是内存库最容易造成的误会，
        // 所以内存库那行要写得显眼一点。
        if (db.inMemory()) {
            std::cout << "当前是**内存库**：数据关掉就没了。"
                         "想留住它就用 -d <文件名> 重开。\n";
        } else {
            std::cout << "数据库文件：" << db.path() << "（改动会立刻落盘）\n";
        }

        std::cout << "输入 SQL，以分号结尾。.help 看帮助，.quit 退出。\n\n";
    }

    std::string pending;  // 还没凑够一个完整语句时的缓冲
    std::string line;

    while (true) {
        if (interactive) {
            // 语句没写完时换个提示符，让人知道它在等后续。
            // （Nova 的 REPL 也是这么做的）
            std::cout << (pending.empty() ? "acorn> " : "   ...> ");
            std::cout.flush();
        }

        if (!std::getline(std::cin, line)) {
            break;  // Ctrl+Z 或者输入流结束
        }

        // ---- 剥掉行尾的 '\r' ----
        // std::getline 只认 '\n' 当分隔符，所以从 Windows 文本文件
        // 重定向进来的每一行末尾都挂着一个 '\r'。
        // 不剥的话 `"exit"` 会变成 `"exit\r"`，和 `"exit"` 比不相等——
        // 表现就是"重定向输入时敲 exit 退不出去"，
        // 而且 `exit` 还会被当成 SQL 去执行，报一句莫名其妙的错。
        // （Nova 在重定向输入上踩过这个坑）
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        if (line.empty() && pending.empty()) continue;

        // ---- 元命令 ----
        // 只在"没在攒语句"的时候才认，否则 SQL 中间一行以点开头
        // （比如换行写的 .5 这种数字）会被误当成命令。
        // 两条退出路径都 break 而不是 return，为的是走到循环后面
        // 那个"事务还开着吗"的检查。要是这儿 return，退出提醒
        // 就只在 Ctrl+Z 时才会响——而那是最不容易被注意到的一条路。
        if (pending.empty() && !line.empty() && line[0] == '.') {
            if (handleMetaCommand(db, line)) break;
            continue;
        }
        // 不带点的 exit / quit 也认，符合直觉
        if (pending.empty() && (line == "exit" || line == "quit")) {
            break;
        }

        // ---- 攒语句 ----
        if (!pending.empty()) pending += "\n";
        pending += line;

        // ---- 攒够了没有 ----
        // 判断依据是**最后一个 Token 是不是分号**，不是"文本里有没有分号"。
        // 为什么要这么改，以及为什么"分析不了"要分两种情况，见上面
        // isStatementComplete 的说明。
        bool complete = false;
        try {
            complete = isStatementComplete(pending);
        } catch (const IncompleteError&) {
            // 字符串或块注释还没收尾。继续等下一行——
            // 这是 REPL 唯一"多敲几个字符就能自愈"的情况。
            continue;
        } catch (const SyntaxError& error) {
            // 真写错了。报出来，并且把这段丢掉：
            // 留着它也没用，REPL 没法回头改已经敲过的行。
            // 丢掉而不是留下，是为了让提示符回到 "acorn>"——
            // 不然用户会以为还在等他补完，其实是卡在一段坏文本上。
            printError(formatSyntaxError(pending, error));
            pending.clear();
            continue;
        }

        if (!complete) continue;

        const std::string source = pending;
        pending.clear();
        execute(db, source, false, false);
    }

    warnOpenTransaction(db);

    if (interactive) std::cout << "\n再见。\n";
}

// ------------------------------------------------------------
//  命令行
// ------------------------------------------------------------

void printUsage() {
    std::cout
        << "Acorn —— 从零手写的小数据库\n"
           "\n"
           "用法：\n"
           "  acorn                      进 REPL（交互式）\n"
           "  acorn <文件.sql>           跑一个 SQL 脚本\n"
           "  acorn -e \"<SQL>\"           直接跑一段 SQL\n"
           "\n"
           "数据库文件：\n"
           "  -d <文件>                  把数据存到这个文件里\n"
           "\n"
           "  ★ 不加 -d 就是内存库：跑完就没了。★\n"
           "    想留住数据，一定要加 -d。\n"
           "\n"
           "事务：\n"
           "  BEGIN; ... COMMIT;         中间的改动一起生效\n"
           "  BEGIN; ... ROLLBACK;       或者一起撤销\n"
           "  （不写 BEGIN 的话，每条语句自己就是一个事务）\n"
           "\n"
           "几个进程一起用同一个库：\n"
           "  读的可以一起读，写的独占——一个库同时只有一个写者。\n"
           "  抢不到锁会直接报错（不会静静地卡住）。愿意等就加：\n"
           "  --lock-wait=<毫秒>         抢不到锁时最多等这么久\n"
           "\n"
           "调试开关：\n"
           "  --tokens                   只做词法分析，打印 Token 流\n"
           "  --ast                      只做语法分析，打印语法树\n"
           "  --crash-at=commit|exit     测试专用：故意在事务中途崩掉\n"
           "  --hold=<毫秒>              测试专用：干完活先别退，把库多开一会儿\n"
           "  -h, --help                 显示这份帮助\n"
           "\n"
           "例子：\n"
           "  acorn -d my.acorn -e \"CREATE TABLE users (id INT, name TEXT);\"\n"
           "  acorn -d my.acorn -e \"INSERT INTO users VALUES (1, 'alice');\"\n"
           "  acorn -d my.acorn -e \"SELECT * FROM users;\"     <- 上一次插的还在\n"
           "  acorn -d my.acorn -e \"CREATE INDEX i ON users (id);\"\n"
           "  acorn -d my.acorn -e \"BEGIN; INSERT INTO users VALUES (2, 'bob'); ROLLBACK;\"\n"
           "  acorn -e \"SELECT COUNT(*) FROM users;\"          <- 内存库，这里会报表不存在\n"
           "  acorn --ast -e \"SELECT * FROM t WHERE a = 1\"\n"
           "\n"
           "进 REPL 后敲 .help 看完整语法。\n";
}

}  // namespace

int main(int argc, char** argv) {
    setupConsole();

    // 不能直接用 argv——Windows 上它已经被转成 ANSI 代码页了，
    // 中文会丢。collectArgs 会绕开它重新取一遍。见上面的说明。
    const std::vector<std::string> args = collectArgs(argc, argv);

    bool showTokens = false;
    bool showAst = false;
    bool hasSource = false;
    std::string source;
    std::string filePath;
    std::string dbPath;  // 空 = 内存库
    CrashPoint crashPoint = CrashPoint::NONE;  // 只有测试会改它
    int lockWaitMs = 0;                        // 抢不到锁时等多久（阶段 6）
    int holdMs = 0;                            // 测试专用：干完活别急着退

    for (size_t i = 0; i < args.size(); i++) {
        const std::string& arg = args[i];

        if (arg == "-h" || arg == "--help") {
            printUsage();
            return 0;
        }
        if (arg == "--tokens") {
            showTokens = true;
            continue;
        }
        if (arg == "--ast") {
            showAst = true;
            continue;
        }
        // ---- 崩溃开关（测试专用）----
        //
        // 【为什么是 --crash-at=commit 这种带等号的长选项】
        // 因为它要带一个值，而带值就有"值缺失"和"值写错"两种错误。
        // `--crash-at commit` 那种写法得再多处理一个 argv 位置，
        // 而 `--crash-at=` 这种写法里，等号后面为空或者写了别的词
        // 都是一眼就能报出来的错。少一种拼错的方式，就少一类
        // "测试跑过了，但测的其实是别的东西"。
        if (arg.rfind("--crash-at=", 0) == 0) {
            const std::string where = arg.substr(11);
            if (where == "commit") {
                crashPoint = CrashPoint::COMMIT;
            } else if (where == "exit") {
                crashPoint = CrashPoint::EXIT;
            } else {
                printError("--crash-at 只认 commit 和 exit，不认 `" + where + "`");
                return 1;
            }
            continue;
        }
        // ---- 抢不到锁时等多久（阶段 6）----
        //
        // 【为什么默认是 0（不等）】因为一个"静静地卡住"的程序比一个
        // 报错的程序难查得多。要等多久是用户明确说出来的事，
        // 不能由我们替他猜——猜短了白等，猜长了像死机。
        if (arg.rfind("--lock-wait=", 0) == 0) {
            const std::string value = arg.substr(12);
            if (!parseMilliseconds(value, lockWaitMs)) {
                printError("--lock-wait 后面要跟一个非负的毫秒数，不认 `" +
                           value + "`");
                return 1;
            }
            continue;
        }
        // ---- 干完活先别退（测试专用，阶段 6）----
        //
        // 【为什么测试需要这种东西】因为"锁"这件事只有在**两个进程
        // 同时在**的时候才看得见，而要造出那个同时，就得让其中一个
        // 慢一点退——不然它早就跑完走人了，第二个进程撞不到任何东西。
        // 它和 --crash-at 是同一类东西：把一个本来靠掐时序才能
        // 复现的状态，变成一个说清楚就能复现的状态。
        if (arg.rfind("--hold=", 0) == 0) {
            const std::string value = arg.substr(7);
            if (!parseMilliseconds(value, holdMs)) {
                printError("--hold 后面要跟一个非负的毫秒数，不认 `" + value +
                           "`");
                return 1;
            }
            continue;
        }
        if (arg == "-e") {
            if (i + 1 >= args.size()) {
                printError("-e 后面要跟一段 SQL");
                return 1;
            }
            source = args[++i];
            hasSource = true;
            continue;
        }
        if (arg == "-d" || arg == "--db") {
            if (i + 1 >= args.size()) {
                printError("-d 后面要跟数据库文件名");
                return 1;
            }
            dbPath = args[++i];
            continue;
        }
        if (!arg.empty() && arg[0] == '-') {
            printError("不认识的选项：" + arg);
            printUsage();
            return 1;
        }
        filePath = arg;  // 不是选项，那就当文件名
    }

    // 打开数据库。
    //
    // 【为什么"打开"要单独包一层 try，"SQL 跑失败"却不用】
    // 因为这两件事的**责任人不同**：
    //   · 打不开文件（不是 Acorn 格式、路径不存在、没权限）
    //     -> 命令行就用错了，和 SQL 一个字都没关系
    //   · SQL 跑失败 -> 命令行没问题，是语句写错了
    // 混在一起用一句"错误：..."糊过去，用户根本分不清
    // 该去检查文件名还是该去检查 SQL。退出码也分开（2 和 1），
    // 脚本里就能据此判断。
    std::unique_ptr<Database> db;
    try {
        db.reset(new Database(dbPath, crashPoint, lockWaitMs));
    } catch (const AcornError& error) {
        // 【阶段 6 起，退出码 2 多了一种情况】"库被别的进程占着"。
        // 它和"文件名写错了"归成一类，理由还是那个：这是**命令行
        // 层面**的问题，和 SQL 一个字都没关系。脚本据此判断
        // "重试一下可能会好"（2）还是"SQL 写错了"（1）。
        printError("错误：" + std::string(error.what()));
        return 2;
    }

    if (hasSource) {
        const int code =
            execute(*db, ensureTrailingSemicolon(source), showTokens, showAst);
        // 【hold 在 warn 之前】因为 hold 的意思是"我还开着这个库"——
        // 那是一个**事实**，而"事务还开着"是一句提醒。
        // 先把事实摆够时间，再提醒。（也因为提醒走 stderr、
        //   hold 那句走 stdout，两条流分开收的时候顺序才不打架。）
        holdIfRequested(holdMs);
        warnOpenTransaction(*db);
        return code;
    }

    if (!filePath.empty()) {
        std::string text;
        try {
            text = readFile(filePath);
        } catch (const AcornError& error) {
            printError("错误：" + std::string(error.what()));
            return 1;
        }
        const int code = execute(*db, text, showTokens, showAst);
        holdIfRequested(holdMs);
        warnOpenTransaction(*db);
        return code;
    }

    // repl 自己会在退出前提醒一次"事务还开着"，这儿不再提醒一遍
    //（同一句话说两次，第二次就没人看了）。
    repl(*db);
    holdIfRequested(holdMs);
    return 0;
}
