#pragma once

// ============================================================
//  Page —— 4KB 页的字节布局
//
//  ★ 阶段 3 的核心文件 ★
//
//  阶段 2 之前，一行数据就是内存里的一个 std::vector<Value>。
//  从这一层开始，一行数据是**一页里的一段字节**。
//
//  【这个文件管什么，不管什么】
//  只管"这 4096 个字节里，第几个字节是什么意思"。
//  不管"这 4096 个字节从哪来"——那是 pager.hpp 的事。
//  分开的理由：布局规则是纯粹的数据格式，改它要非常谨慎（改了旧文件
//  就读不出来了）；而"从哪来"将来会变（加缓存、加日志、加并发锁）。
//  把善变的东西和不该变的东西放在一个文件里，早晚会一起变。
//
//  【为什么是 4096】
//  操作系统一页就是 4KB。读写正好一页的时候，内核不用做"读一部分、
//  留着剩下的"这种拆分。SQLite 默认也是 4096。
//  这不是随便定的数——它是操作系统和磁盘之间的最小搬运单位。
//
//  【和一行的关系】
//  一行存不跨页。也就是说一行的字节必须能塞进一页。
//  代价是 TEXT 列不能无限长（本项目里最大一行约 4KB），
//  收益是"读一行只需要读一页"，不用把好几页拼起来。
//  SQLite 的做法是溢出页（一行跨多页），那是后面的优化，不是必需的。
// ============================================================

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "schema.hpp"
#include "value.hpp"

// ------------------------------------------------------------
//  基本常量
// ------------------------------------------------------------

constexpr size_t PAGE_SIZE = 4096;

// "没有这一页"。用 uint32 的最大值而不是 0——
// 0 是文件头页，是个合法页号，拿它当"没有"会出大问题。
// 这和"用 -1 表示没找到"是同一个坑，只是换了个类型。
constexpr uint32_t INVALID_PAGE = 0xFFFFFFFFu;

// 文件里存页大小的位置也存了这个值。打开文件时对不上就报错：
// 换个页大小读同一个文件，所有偏移全错，读出来的是垃圾——
// 与其给一堆莫名其妙的数，不如当场说"这不是我认识的格式"。
//
// 【版本 2：阶段 4 加的索引目录】
// 版本 1 的文件头里，偏移 28 那 4 个字节是**保留位**（写的 0）。
// 版本 2 把那 4 个字节用成了"索引目录链的第一页"。
// 所以必须升版本号，不能装作没变——一个版本 1 的旧文件里，
// 那个位置是 0，而 **0 是一个合法的页号**（第 0 页是文件头）。
// 不升版本的话，打开旧文件会读到"索引目录从第 0 页开始"，
// 然后拿文件头当目录页扫，扫出来的是垃圾。
//
// 这就是"保留位"的真实成本：**它只是暂时没人用，不代表可以随便用**。
// 动它就要动版本号，让旧文件明确地被拒绝，而不是被误解。
constexpr uint32_t ACORN_FORMAT_VERSION = 2;

// ------------------------------------------------------------
//  Row / RowId
// ------------------------------------------------------------

// 一行数据：一个值的数组，下标和 schema.columns 一一对应。
//
// 用类型别名而不是包一个类。包一层 Row 类的话，每一处都要多写
// `.values`，而且拦不住任何人绕过它——不带来任何约束的封装是纯开销。
using Row = std::vector<Value>;

// 一行的身份证。
//
// 【阶段 3 换了实现，但换了之后调用方一行都没改】
// 阶段 2 它是 `size_t`（第几行），因为行就摆在 vector 里。
// 现在它是 (页号, 页内槽号)——行真正在文件里的位置。
//
// 「第几行」是要从头数才能算出来的，代价 O(n)；而 (页号, 槽号)
// 是直接寻址，代价 O(1)。这不是优化，是**换了个正确的东西**：
// 内存里"第几行"是有意义的，磁盘上它根本不存在。
//
// 执行器从头到尾只把它当不透明的句柄用（从 scan 接过来、原样传回去），
// 所以把它从 size_t 换成这个 struct，执行器一个字都没改。
// 这就是阶段 1 那个 `using RowId = size_t;` 留下的缝。
struct RowId {
    uint32_t pageNo = INVALID_PAGE;
    uint16_t slot = 0;

    bool operator==(const RowId& other) const {
        return pageNo == other.pageNo && slot == other.slot;
    }
    bool operator!=(const RowId& other) const { return !(*this == other); }
};

// ------------------------------------------------------------
//  小端读写
// ------------------------------------------------------------

// 【为什么用 memcpy 而不是 reinterpret_cast】
// 直觉写法是 `*reinterpret_cast<uint16_t*>(p)`。它有两个问题：
//
//   1. 对齐。p 是 char*，可能落在奇数地址上，某些 CPU 上直接崩。
//      本机的 x86 容忍，所以**现在这么写也能跑**——然后换台机器就炸。
//   2. 严格别名规则。编译器有权假设"uint16_t* 指向的东西和 char*
//      指向的东西不是同一块内存"，从而把读写重排掉。
//
// memcpy 两头都不占：编译器把它当成逐字节复制，没有任何假设。
// 而且它**认字节序**——下面显式按小端拼字节，所以在小端机上
// 和大端机上读出来的数一样。数据库文件是要跨机器读的，
// 不能依赖本机字节序。
//
// 这几行看着笨，但一个数据库文件格式的字节序必须是写死的，
// 不能是"碰巧和本机一样"。
inline uint16_t readU16(const char* p) {
    const unsigned char* b = reinterpret_cast<const unsigned char*>(p);
    return static_cast<uint16_t>(b[0] | (b[1] << 8));
}

inline uint32_t readU32(const char* p) {
    const unsigned char* b = reinterpret_cast<const unsigned char*>(p);
    return static_cast<uint32_t>(b[0]) | (static_cast<uint32_t>(b[1]) << 8) |
           (static_cast<uint32_t>(b[2]) << 16) |
           (static_cast<uint32_t>(b[3]) << 24);
}

inline uint64_t readU64(const char* p) {
    const unsigned char* b = reinterpret_cast<const unsigned char*>(p);
    uint64_t value = 0;
    for (int i = 7; i >= 0; i--) {
        value = (value << 8) | b[i];
    }
    return value;
}

inline void writeU16(char* p, uint16_t value) {
    unsigned char* b = reinterpret_cast<unsigned char*>(p);
    b[0] = static_cast<unsigned char>(value & 0xFF);
    b[1] = static_cast<unsigned char>((value >> 8) & 0xFF);
}

inline void writeU32(char* p, uint32_t value) {
    unsigned char* b = reinterpret_cast<unsigned char*>(p);
    b[0] = static_cast<unsigned char>(value & 0xFF);
    b[1] = static_cast<unsigned char>((value >> 8) & 0xFF);
    b[2] = static_cast<unsigned char>((value >> 16) & 0xFF);
    b[3] = static_cast<unsigned char>((value >> 24) & 0xFF);
}

inline void writeU64(char* p, uint64_t value) {
    unsigned char* b = reinterpret_cast<unsigned char*>(p);
    for (int i = 0; i < 8; i++) {
        b[i] = static_cast<unsigned char>((value >> (8 * i)) & 0xFF);
    }
}

// ------------------------------------------------------------
//  页头（所有页共用的前 8 字节）
// ------------------------------------------------------------
//
//   偏移  长度  内容
//   ----  ----  ------------------------------------------
//   0     1     页类型（见 PageType）
//   1     1     保留，必须写 0
//   2     2     保留（对齐用，将来放标志位）
//   4     4     链表的下一个页号，没有就是 INVALID_PAGE
//
// 【为什么所有页都留这 8 个字节】
// 因为"空闲页链"要用 next，"一个表的所有数据页"也要用 next。
// 三种页各自的其余部分完全不一样，但这 8 个字节的形状是一样的，
// 于是"沿着链走"这件事可以写一份代码。这就是"页"这个概念的用处：
// 不管里面装什么，从外面看都是一样大的、可以互相串起来的一块。
enum class PageType : unsigned char {
    FREE = 0,         // 空闲页，等着被分配
    FILE_HEADER = 1,  // 第 0 页，整个文件的头
    CATALOG = 2,      // 目录页：存表结构
    HEAP = 3,         // 数据页：存行
    // 阶段 4 加的两种：B+ 树的节点。
    //
    // 【为什么是两种而不是一种】
    // 叶子页存的是"键 -> 行在哪"，内部页存的是"键 -> 往哪个孩子走"。
    // 两者的内容不同、操作也不同（叶子要串成链供范围扫描走，
    // 内部页不串）。混成一种的话，每个操作里都要先问一句
    // "这一页是哪种"，问漏一处就是静悄悄地读错东西。
    BTREE_LEAF = 4,      // B+ 树的叶子：一条条 (键, RowId)
    BTREE_INTERNAL = 5,  // B+ 树的内部节点：一条条 (键, 子页号)
};

constexpr size_t PAGE_HEADER_SIZE = 8;

PageType pageType(const char* page);
void setPageType(char* page, PageType type);

uint32_t pageNext(const char* page);
void setPageNext(char* page, uint32_t next);

// ------------------------------------------------------------
//  文件头页（第 0 页）
// ------------------------------------------------------------
//
//   8    5     magic "ACORN"
//   13   1     格式版本
//   14   2     保留
//   16   4     页大小（4096）
//   20   4     目录链的第一页
//   24   4     空闲页链的第一页
//   28   4     索引目录链的第一页（阶段 4 开始用；版本 1 里是保留位）
//   32   4     保留（阶段 5 的事务日志起点会用）
//
// 【为什么索引单独有一条目录链，不和表挤在一起】
// 两条链里的记录形状完全不同（表记录有列定义，索引记录有"建在哪张表
// 的哪一列上"），而记录格式里**没有"这条是什么"的判别字节**。
// 挤在一条链上的话，要么给每条记录加一个判别字节（改已有记录的格式），
// 要么让读的人靠猜。分开两条链，各自的记录格式就是自己的事。
//
// 【这里**没有**"文件总共多少页"】
// 一开始写了，后来删掉了。因为文件有多大，文件系统自己就知道——
// 再记一份在文件里，就有了两个真相来源。两者一旦对不上（写到一半
// 断电、手工改了文件大小），程序该信谁？没有好答案。
// 凡是能问出来的信息就别存。这条和 schema 里"不存 key、只存 name"
// 是同一个原则，只是这次删掉的是一整个字段。
//
// 【第 0 页为什么不能拿来存数据】
// 因为它是"这个文件是什么"的答案本身。所有别的页都能靠它解释，
// 它自己不能靠别的页解释。所以它必须待在固定的位置——
// 文件的最开头，偏移 0。改成别的页号也行，但那样每次打开都要
// 先知道"头在哪"，而那个信息本身又需要一个头……只能靠约定。
// 干脆就用 0。
void initFileHeader(char* page);

// 检查这一页是不是一个能读的 Acorn 文件头。
// 不是的话把原因写进 why，返回 false。
// （返回原因而不是抛异常：调用方要区分"文件是空的，新建一个"
//   和"文件是别的东西，别乱动它"，这两种处理完全不同。）
bool checkFileHeader(const char* page, std::string& why);

bool fileHeaderIsEmpty(const char* page);  // 全 0，说明是刚新建的文件

uint32_t fileCatalogFirst(const char* page);
void setFileCatalogFirst(char* page, uint32_t pageNo);

uint32_t fileFreeListHead(const char* page);
void setFileFreeListHead(char* page, uint32_t pageNo);

uint32_t fileIndexCatalogFirst(const char* page);
void setFileIndexCatalogFirst(char* page, uint32_t pageNo);

// ------------------------------------------------------------
//  堆页（数据页）—— 槽位页结构
// ------------------------------------------------------------
//
//   ★ 这是整份代码里最值得看懂的一张图 ★
//
//   +--------------------------------------------------+
//   | 页头 12 字节                                      |  12
//   +--------------------------------------------------+
//   | 槽位0 | 槽位1 | 槽位2 | ...        →  往页尾长 →  |
//   +--------------------------------------------------+
//   |                    ↑ 自由空间 ↓                   |
//   |  ← 往页头长 ←        cell2 | cell1 | cell0        |
//   +--------------------------------------------------+
//                                                     4096
//
//   槽位（4 字节）= 偏移(2) + 长度(2)，指向某个 cell 在哪。
//   cell 就是一行编码之后的字节（见下面的 encodeRow）。
//
//  【为什么要分成"槽位数组 + cell 区"两块，而不是直接顺序排】
//  因为一张表里的行**长度不一样**（TEXT 列有长有短）。
//  如果行挨着行排，删掉中间一行，后面的全得往前挪，
//  而且 RowId（第几行）会跟着变——所有拿着 RowId 的人全傻了。
//
//  槽位页把这两件事拆开：
//    · 槽位数组只管"第 n 行在哪"，槽位本身**永不移位**
//    · cell 区只管"字节放哪"，随便挪，反正槽位知道新地址
//  于是删除 = 把槽位长度改成 0（墓碑），别的一动不动。
//  第 3 行永远是第 3 个槽位，不管中间删了多少行。
//
//  【墓碑为什么不能直接把槽位删掉】
//  槽位数组里删一项，后面所有槽位的下标都减一，RowId 全部失效。
//  留个长度 0 的墓碑，下标就稳了。空间靠压实回收（见 heapCompact）。
//
//  【页头 12 字节怎么来的】
//   8 字节通用页头（type / next）
//   + 2 字节 slotCount  —— 槽位数组里有几项（含墓碑）
//   + 2 字节 cellTop    —— cell 区已经用到的**最低**偏移
//  自由空间 = cellTop - (12 + slotCount * 4)
constexpr size_t HEAP_HEADER_SIZE = 12;
constexpr size_t SLOT_SIZE = 4;

// 一行的字节数上限。超过它，这行永远塞不进任何一页。
// 调用方靠它区分"这一页满了（换一页就行）"和"这行根本放不下（得报错）"。
constexpr size_t MAX_CELL_SIZE = PAGE_SIZE - HEAP_HEADER_SIZE - SLOT_SIZE;

void initHeapPage(char* page, uint32_t next);

// 初始化一个 B+ 树节点页（叶子或内部）。
// type 必须是 BTREE_LEAF 或 BTREE_INTERNAL——写别的话，
// 后面那些"这一页是哪种节点"的判断就全错了。
void initBtreePage(char* page, PageType type, uint32_t next);

uint16_t heapSlotCount(const char* page);
size_t heapFreeBytes(const char* page);

// 在页里放一段 cell 字节，返回它在页内的偏移。
// **不碰槽位数组**——这是 heapInsert 和 heapUpdate 共用的那一半。
//
// 拆出这一半，是因为"把字节放进去"和"记下它在哪"是两件事：
// 插入是"放一段新的 + 追加一个槽位"，
// 更新是"放一段新的 + 改一个已有的槽位"。
// 前一半完全一样，后一半不一样。写成两份的话，
// "放不下就先压实"那套逻辑就要抄两遍。
bool heapPlace(char* page, const char* cell, size_t length, uint16_t& offsetOut);

// 往这一页塞一行编码后的字节。成功返回 true 并把槽号写进 slotOut。
//
// 放不下返回 false（**不是**抛异常）。因为"这一页放不下"是调用方
// 正常要处理的情况——换个页就行。用异常表达一件天天发生的事，
// 异常就不再是异常了。
bool heapInsert(char* page, const char* cell, size_t length, uint16_t& slotOut);

// 把一段字节插到**第 index 个槽位**上，后面所有的槽位往后挪一格。
//
// 【为什么 B+ 树需要它，而表不需要】
// 表里行的顺序没有意义（第几行就是第几个槽位，那是身份不是次序），
// 所以插入永远是"追加到末尾"。
// B+ 树的节点是**排好序**的：新条目要插到它该在的位置，
// 后面那些槽位必须跟着挪——不然顺序就断了，而顺序正是树能二分查找的前提。
//
// 【挪槽位在这里是安全的，这是整个设计里非常特别的一点】
// 槽位的下标在表里是 RowId 的一部分，动不得（见 heapCompact 的说明）。
// 但 B+ 节点的第几个槽位**没有任何人记得**——外面只认页号，
// 进了页之后一切重新找。所以这里可以放心地挪。
// 「哪个东西的身份是它的位置」这个判据，在这里给出了相反的答案，
// 因为对象换了。
//
// index 可以是 heapSlotCount（= 追加到末尾）。
// 放不下返回 false，并且**一个字节都没改**。
bool heapInsertAt(char* page, uint16_t index, const char* cell, size_t length,
                  uint16_t& slotOut);

// 更新第 slot 个槽位指向的那段字节。
//
// 【这个函数是"槽位页"这个设计兑现价值的地方】
// 新值比旧值短或一样长 -> 原地的字节直接覆盖，位置都不动。
// 新值变长了       -> 把老位置立成墓碑，在同一页里另找地方放，
//                     再把槽位指向新位置。
//
// 两种情况下**槽号都没变**，所以 RowId 还指着同一行。
// 这就是把"槽位数组"和"cell 区"分开的回报：
// 行的**身份**（第几个槽位）和行的**位置**（字节在哪）是两件事，
// 所以搬动字节不会动到身份。
//
// 整页都装不下时返回 false，并且**保证原来的数据完好**——
// 调用方可以安心地去别的页想办法。
bool heapUpdate(char* page, uint16_t slot, const char* cell, size_t length);

// 读第 slot 个槽位指向的字节。墓碑或越界返回 nullptr。
// 返回指针而不是拷贝：调用方马上要解码，中间没必要复制一份。
// 指针的生命周期 = 这一页在缓存里待着的时间，调用方解码完就还回来。
const char* heapCell(const char* page, uint16_t slot, size_t& lengthOut);

// 删除：只把槽位的长度改成 0，字节留在原地。见上面墓碑的说明。
void heapDelete(char* page, uint16_t slot);

// 压实：把所有活着的 cell 重新紧贴页尾排一遍，把墓碑占的空间收回来。
//
// 【为什么只动 cell，不动槽位数组】
// 槽位的**下标**就是 RowId 里的那个数，动一下所有 RowId 就失效了。
// 而 cell 的**偏移**没人记得住（每次都是现查槽位），所以随便挪。
// 「哪个东西的身份是它的位置」——分清楚这个，才知道什么能动。
//
// 返回是否真的收回了空间（一个墓碑都没有的话是白跑一趟）。
bool heapCompact(char* page);

// ------------------------------------------------------------
//  一行的编解码
// ------------------------------------------------------------
//
//  每个值前面带 1 个字节的类型标签：
//
//   0x00  NULL      后面没有内容
//   0x01  整数      8 字节小端
//   0x02  浮点      8 字节小端（IEEE754 的位模式）
//   0x03  文本      4 字节小端长度 + 这些字节
//   0x04  布尔      1 字节（0 或 1）
//
//  【为什么每个值都带标签，而不是存一个"空值位图"】
//  位图方案省 1 字节/列，但那样解码时必须拿着表结构才能读——
//  字节流本身不自描述。哪天表结构变了（加了一列），
//  旧数据用新结构去解，就会把字节解释成完全不同的东西，
//  而且**读得出来**，只是值是错的。
//
//  自带标签的话，读到不认识的标签就能立刻报"数据文件坏了"，
//  而不是静静地给出一堆错数字。一个数据库最怕的不是报错，
//  是**不报错但答案不对**。多花的那 1 字节/列，买的是这个。
//
//  （浮点用位模式存，不用文本。文本存法要处理"打印几位小数"，
//   而 double 有 17 位有效数字，打印成文本再读回来经常差一点点——
//   差一点点在钱和数据上就是事故。）
std::string encodeRow(const Row& row);
Row decodeRow(const char* data, size_t length);

// 单个值的编解码。**encodeRow 就是把它对每个值调一遍**——
// 一份实现，两处用法。
//
// 【为什么 B+ 树必须复用这一份，不能自己写一个】
// 索引里存的键是从行里读出来的那个值，页里存的键是编码之后的字节。
// 两边要是各写一份编码，就得保证它们**永远**一模一样——
// 一旦有一个字节不同（比如浮点的写法、文本长度是 4 字节还是 2 字节），
// 索引里的键就再也匹配不上行里的值，表现是"索引里明明有，就是查不到"。
// 而且**不会报错**。
//
// 比较顺序也一样受编码影响：如果索引里的键是按字节比较的，
// 那编码的字节序就决定了"谁大谁小"。所以这里连比较都不能自己搞一套。
std::string encodeValue(const Value& value);

// 从字节流开头解出一个值。
// usedOut 收这次用掉了几个字节——有了它，调用方才知道下一个值从哪开始。
// 字节不够或者标签不认识时抛 AcornError（数据文件坏了）。
Value decodeValue(const char* data, size_t length, size_t& usedOut);

// ------------------------------------------------------------
//  目录页 —— 存表结构
// ------------------------------------------------------------
//
// 布局和堆页一样是槽位页，只是槽位指向的 cell 换成了"表的定义"。
// 为什么不另写一套：因为"一堆变长的东西存在一页里"这个问题
// 一模一样，解法也一模一样。抄一遍只会多一处要改的地方。
//
// 一条记录的内容：
//   1 字节   标志（0 = 活着，1 = 已删）
//   4 字节   这张表的第一个数据页
//   2 字节   表名长度 + 表名（原始大小写）
//   2 字节   列数
//   每列：2 字节列名长度 + 列名 + 1 字节类型
//
// 【为什么删表只打个标志，不真删】
// 和行删除一样：目录页里删一条，后面记录的下标全变。
// 另外目录页还会被追着往后写，就地删了反而不好处理"页空了"这种情况。
// 打个标志，打开时跳过，代价是一条记录几十字节。
// （这是张很划算的欠条：现在不用为"目录页回收"写任何代码。）
constexpr size_t CATALOG_HEADER_SIZE = HEAP_HEADER_SIZE;

constexpr size_t MAX_CATALOG_RECORD =
    PAGE_SIZE - CATALOG_HEADER_SIZE - SLOT_SIZE;

void initCatalogPage(char* page, uint32_t next);

// 这一页还能不能塞下长度为 length 的记录
bool catalogFits(const char* page, size_t length);

// 追加一条记录，返回它在页内的偏移（将来要改它的时候用这个位置）。
// 放不下返回 false。
bool catalogAppend(char* page, const char* record, size_t length,
                   uint16_t& offsetOut);

// 遍历这一页里的所有目录记录。visit 收 (记录字节, 长度, 记录在页内的偏移)。
// 已删的记录也会回调——**由调用方决定**跳不跳，因为"打开时跳过"和
// "DROP TABLE 时要把标志改成已删"是两种不同的需要。
void catalogScan(const char* page,
                 const std::function<void(const char*, size_t, uint16_t)>& visit);

// 把某条记录的第 0 个字节（标志）改成"已删"。
void catalogMarkDeleted(char* page, uint16_t offset);

// 把某条记录里存的那个**页号**改成新值。
//
// 【表记录和索引记录共用这一个函数，因为它们那个字段在同一位置】
// 两种记录的第 1~4 字节都是"这条记录指向的页"：
//   表记录   -> 第一个数据页
//   索引记录 -> B+ 树的根页
// 这个重合是**刻意对齐**的：它让"改这个数"只有一份实现。
// （B+ 树的根页会因为树长高而变，所以索引这一路是必须要能改的。）
void catalogSetRecordPage(char* page, uint16_t offset, uint32_t pageNo);

std::string encodeCatalogRecord(const TableSchema& schema, uint32_t firstPage);
bool decodeCatalogRecord(const char* data, size_t length, TableSchema& schemaOut,
                         uint32_t& firstPageOut);

// ------------------------------------------------------------
//  索引目录记录
// ------------------------------------------------------------
//
// 一条记录的内容：
//   1 字节   标志（0 = 活着，1 = 已删）
//   4 字节   B+ 树的根页
//   2 字节   索引名长度 + 索引名（原始大小写）
//   2 字节   表名长度 + 表名
//   2 字节   列号（在这张表的第几列上）
//
// 【为什么存列号而不是列名】
// 列名是个可能变长的东西，而列号是 2 个字节。更重要的理由：
// 列号是**这张表内部**的说法，改列名不影响它。
// 存列名的话，将来加 `ALTER TABLE RENAME COLUMN` 就得把
// 所有索引记录改一遍——而索引本来就不该关心列叫什么。
//
// 反过来说，表名必须存名字（不能存页号）：表是可以被删掉再建的，
// 而**索引跟着表走**（DROP TABLE 要连带删掉它的索引），
// 这件事得靠名字来判断"这个索引是谁的"。
constexpr size_t MAX_INDEX_RECORD =
    PAGE_SIZE - CATALOG_HEADER_SIZE - SLOT_SIZE;

std::string encodeIndexRecord(const IndexSchema& schema, uint32_t rootPage);
bool decodeIndexRecord(const char* data, size_t length, IndexSchema& schemaOut,
                       uint32_t& rootPageOut);

// ------------------------------------------------------------
//  报错用的小工具
// ------------------------------------------------------------

// 页号的显示形式："第 7 页"。
// 只是为了让报错信息不用在十个地方各写一遍拼接。
std::string pageName(uint32_t pageNo);
