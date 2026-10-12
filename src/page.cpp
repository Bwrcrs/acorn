#include "page.hpp"

#include <cstring>
#include <string>

#include "error.hpp"
#include "token.hpp"  // lowercase（索引用它把表名转成查找用的 key）

// ------------------------------------------------------------
//  页头
// ------------------------------------------------------------

PageType pageType(const char* page) {
    return static_cast<PageType>(static_cast<unsigned char>(page[0]));
}

void setPageType(char* page, PageType type) {
    page[0] = static_cast<char>(static_cast<unsigned char>(type));
    // 第 1、2、3 字节是保留位。**只在建页的时候清一次**，
    // 不在每次改类型的时候清——因为将来这些位是给别的东西用的
    //（比如"这一页上有没有未提交的改动"），现在动它们是在破坏数据。
    // 清 0 的活儿交给 init*Page()，那些函数本来就是"这张白纸重新开始"。
}

uint32_t pageNext(const char* page) {
    return readU32(page + 4);
}

void setPageNext(char* page, uint32_t next) {
    writeU32(page + 4, next);
}

// 把一个页头初始化成"空白"。
// 所有 init*Page 都从这里开始，保留位只在这一个地方被清掉。
static void initPageHeader(char* page, PageType type, uint32_t next) {
    std::memset(page, 0, PAGE_SIZE);
    setPageType(page, type);
    setPageNext(page, next);
}

// ------------------------------------------------------------
//  文件头页
// ------------------------------------------------------------

namespace {

constexpr size_t FH_MAGIC = 8;
constexpr size_t FH_VERSION = 13;
constexpr size_t FH_PAGE_SIZE = 16;
constexpr size_t FH_CATALOG_FIRST = 20;
constexpr size_t FH_FREE_LIST = 24;
constexpr size_t FH_INDEX_CATALOG = 28;

const char MAGIC[5] = {'A', 'C', 'O', 'R', 'N'};

}  // namespace

void initFileHeader(char* page) {
    initPageHeader(page, PageType::FILE_HEADER, INVALID_PAGE);

    std::memcpy(page + FH_MAGIC, MAGIC, sizeof(MAGIC));
    page[FH_VERSION] = static_cast<char>(ACORN_FORMAT_VERSION);
    writeU32(page + FH_PAGE_SIZE, static_cast<uint32_t>(PAGE_SIZE));
    writeU32(page + FH_CATALOG_FIRST, INVALID_PAGE);
    writeU32(page + FH_FREE_LIST, INVALID_PAGE);
    writeU32(page + FH_INDEX_CATALOG, INVALID_PAGE);
}

bool fileHeaderIsEmpty(const char* page) {
    // 刚新建的文件里全是 0（Pager 分配新页时会清零）。
    // 靠这个区分"文件还不存在内容"和"文件是别的东西"——
    // 前者应该初始化，后者应该拒绝。
    for (size_t i = 0; i < PAGE_SIZE; i++) {
        if (page[i] != 0) return false;
    }
    return true;
}

bool checkFileHeader(const char* page, std::string& why) {
    if (std::memcmp(page + FH_MAGIC, MAGIC, sizeof(MAGIC)) != 0) {
        why = "开头的 5 个字节不是 ACORN";
        return false;
    }

    const unsigned char version =
        static_cast<unsigned char>(page[FH_VERSION]);
    if (version != ACORN_FORMAT_VERSION) {
        why = "格式版本是 " + std::to_string(version) + "，本程序只认 " +
              std::to_string(ACORN_FORMAT_VERSION);
        return false;
    }

    const uint32_t pageSize = readU32(page + FH_PAGE_SIZE);
    if (pageSize != PAGE_SIZE) {
        // 这个错必须拦死。页大小不对的话，后面每一个偏移都是错的，
        // 读出来的全是垃圾字节——而且**不会崩**，只会给出莫名其妙的
        // 数据和莫名其妙的报错。宁可现在就说清楚。
        why = "页大小是 " + std::to_string(pageSize) + " 字节，本程序只认 " +
              std::to_string(PAGE_SIZE);
        return false;
    }

    return true;
}

uint32_t fileCatalogFirst(const char* page) {
    return readU32(page + FH_CATALOG_FIRST);
}

void setFileCatalogFirst(char* page, uint32_t pageNo) {
    writeU32(page + FH_CATALOG_FIRST, pageNo);
}

uint32_t fileFreeListHead(const char* page) {
    return readU32(page + FH_FREE_LIST);
}

void setFileFreeListHead(char* page, uint32_t pageNo) {
    writeU32(page + FH_FREE_LIST, pageNo);
}

uint32_t fileIndexCatalogFirst(const char* page) {
    return readU32(page + FH_INDEX_CATALOG);
}

void setFileIndexCatalogFirst(char* page, uint32_t pageNo) {
    writeU32(page + FH_INDEX_CATALOG, pageNo);
}

// ------------------------------------------------------------
//  堆页
// ------------------------------------------------------------

namespace {

constexpr size_t HP_SLOT_COUNT = 8;
constexpr size_t HP_CELL_TOP = 10;

uint16_t slotOffset(const char* page, uint16_t slot) {
    return readU16(page + HEAP_HEADER_SIZE + static_cast<size_t>(slot) * SLOT_SIZE);
}

uint16_t slotLength(const char* page, uint16_t slot) {
    return readU16(page + HEAP_HEADER_SIZE +
                   static_cast<size_t>(slot) * SLOT_SIZE + 2);
}

void setSlot(char* page, uint16_t slot, uint16_t offset, uint16_t length) {
    char* at = page + HEAP_HEADER_SIZE + static_cast<size_t>(slot) * SLOT_SIZE;
    writeU16(at, offset);
    writeU16(at + 2, length);
}

}  // namespace

void initHeapPage(char* page, uint32_t next) {
    initPageHeader(page, PageType::HEAP, next);
    writeU16(page + HP_SLOT_COUNT, 0);
    // cellTop 一开始指向页尾：cell 区是空的，往页头方向长。
    writeU16(page + HP_CELL_TOP, static_cast<uint16_t>(PAGE_SIZE));
}

uint16_t heapSlotCount(const char* page) {
    return readU16(page + HP_SLOT_COUNT);
}

size_t heapFreeBytes(const char* page) {
    const size_t slotArrayEnd =
        HEAP_HEADER_SIZE + static_cast<size_t>(heapSlotCount(page)) * SLOT_SIZE;
    const size_t cellTop = readU16(page + HP_CELL_TOP);
    // cellTop 只会 >= slotArrayEnd，不会反过来——除非文件被改坏了。
    // 真出现那种情况时这里会回绕成一个巨大的数，然后 heapInsert
    // 会以为空间无限大，往页外写。所以卡一道。
    if (cellTop < slotArrayEnd) return 0;
    return cellTop - slotArrayEnd;
}

bool heapPlace(char* page, const char* cell, size_t length, uint16_t& offsetOut) {
    // 这一行本身就太大，换哪一页都没用。调用方会查这个上限，
    // 报一句"这一行太大了"而不是"磁盘满了"。
    if (length > MAX_CELL_SIZE) return false;

    if (heapFreeBytes(page) < length) {
        // 空间不够。可能是真的满了，也可能只是**碎片**：
        // 删过几行留下墓碑，字节散在页里用不上。
        // 先压实一次再试——这是墓碑能真正换来空间的地方。
        if (!heapCompact(page)) return false;
        if (heapFreeBytes(page) < length) return false;
    }

    // ---- 把 cell 放到页尾方向 ----
    // cellTop 往下移 length，新的 cell 就落在 [cellTop, cellTop+length)。
    const size_t newCellTop = readU16(page + HP_CELL_TOP) - length;
    std::memcpy(page + newCellTop, cell, length);
    writeU16(page + HP_CELL_TOP, static_cast<uint16_t>(newCellTop));

    offsetOut = static_cast<uint16_t>(newCellTop);
    return true;
}

bool heapInsert(char* page, const char* cell, size_t length, uint16_t& slotOut) {
    if (length > MAX_CELL_SIZE) return false;

    const uint16_t slotCount = heapSlotCount(page);

    // 【为什么这里要自己算一遍空间，不能直接调 heapPlace】
    // 因为新行除了字节，还要多占一个槽位（4 字节）。
    // heapPlace 只关心字节放不放得下，不知道还要给槽位留位置。
    // 不管的话，最后那一页会出现"字节放得下、槽位数组和它撞上"，
    // 双方互相覆盖——而且是**偶尔**才撞，平时看着都好。
    const size_t needed = length + SLOT_SIZE;

    if (heapFreeBytes(page) < needed) {
        if (!heapCompact(page)) return false;
        if (heapFreeBytes(page) < needed) return false;
    }

    uint16_t offset = 0;
    if (!heapPlace(page, cell, length, offset)) return false;

    // ---- 槽位排在槽位数组的末尾 ----
    // 【为什么是"追加到末尾"而不是"复用墓碑的槽位"】
    // 复用的话，两个不同的 RowId 会指向同一个槽位——扫描的时候
    // 刚删掉的行会"复活"成新插入的行。而且顺序也乱了：
    // 新行出现在老行前面。用追加，槽位的下标顺序 = 插入顺序，
    // 扫描出来的行序才是稳定的（测试可比对，用户也能预期）。
    // 代价是墓碑的槽位空间（4 字节）不回收，那点开销换确定性，值。
    setSlot(page, slotCount, offset, static_cast<uint16_t>(length));
    writeU16(page + HP_SLOT_COUNT, static_cast<uint16_t>(slotCount + 1));
    slotOut = slotCount;
    return true;
}

bool heapInsertAt(char* page, uint16_t index, const char* cell, size_t length,
                  uint16_t& slotOut) {
    if (length > MAX_CELL_SIZE) return false;

    const uint16_t slotCount = heapSlotCount(page);
    if (index > slotCount) return false;  // 越界：调用方算错了

    // 和 heapInsert 一样，槽位本身也要占 4 字节——
    // 只算字节不算槽位的话，最后那几行会出现"槽位数组和 cell 撞上"。
    const size_t needed = length + SLOT_SIZE;
    if (heapFreeBytes(page) < needed) {
        if (!heapCompact(page)) return false;
        if (heapFreeBytes(page) < needed) return false;
    }

    uint16_t offset = 0;
    if (!heapPlace(page, cell, length, offset)) return false;

    // ---- 把 index 及以后的槽位整体后移一格 ----
    //
    // 方向必须是**从后往前**搬：从前往后的话，第 index+1 个槽位
    // 会先盖掉第 index+2 个，然后把那个已经变脏的值搬到 index+2……
    // 一路搬下去，后面全是被覆盖过的痕迹。
    // memmove 能处理重叠，但**重叠方向搞反了它也没用**——
    // memmove 保证的是"结果和先拷贝到临时缓冲一样"，
    // 而"从前往后拷贝"这件事本身在语义上就是错的。
    // （heapCompact 里那块临时缓冲，和这里是同一个道理的两种解法：
    //   那边靠"先全拷出来"，这边靠"倒着搬"。）
    for (uint16_t slot = slotCount; slot > index; slot--) {
        const uint16_t source = static_cast<uint16_t>(slot - 1);
        setSlot(page, slot, slotOffset(page, source), slotLength(page, source));
    }

    setSlot(page, index, offset, static_cast<uint16_t>(length));
    writeU16(page + HP_SLOT_COUNT, static_cast<uint16_t>(slotCount + 1));
    slotOut = index;
    return true;
}

bool heapUpdate(char* page, uint16_t slot, const char* cell, size_t length) {
    if (length > MAX_CELL_SIZE) return false;
    if (slot >= heapSlotCount(page)) return false;

    const uint16_t oldLength = slotLength(page, slot);
    if (oldLength == 0) return false;  // 墓碑：调用方逻辑错了，不该走到这
    const uint16_t oldOffset = slotOffset(page, slot);

    if (length <= oldLength) {
        // ---- 最常见的情况：没变长 ----
        // 字节位置一个都不动，直接覆盖，然后把长度改小。
        // 长出来的尾巴留在原地没人管——它已经不属于任何行了
        //（长度说了算），下次压实的时候顺手收走。
        std::memcpy(page + oldOffset, cell, length);
        setSlot(page, slot, oldOffset, static_cast<uint16_t>(length));
        return true;
    }

    // ---- 变长了 ----
    // 先把老位置立成墓碑，把它的字节让出来。
    // 然后重新找地方——heapPlace 发现空间不够时会压实，
    // 而压实正好会把刚让出来的那块收回去。
    setSlot(page, slot, oldOffset, 0);

    uint16_t offset = 0;
    if (!heapPlace(page, cell, length, offset)) {
        // 这一页真的装不下。**把墓碑改回活口**：
        // 上面只改了长度字段，原来的字节一个都没动，所以能完整还原。
        // 少了这一步，调用方以为"更新失败、数据没变"，
        // 实际上那一行已经被抹掉了——这是最恶劣的一种 bug：
        // 报错说没事，其实出事了。
        setSlot(page, slot, oldOffset, oldLength);
        return false;
    }

    // 槽号没变，只是它指向的字节换了地方。RowId 依然是这一行。
    setSlot(page, slot, offset, static_cast<uint16_t>(length));
    return true;
}

const char* heapCell(const char* page, uint16_t slot, size_t& lengthOut) {
    if (slot >= heapSlotCount(page)) return nullptr;

    const uint16_t length = slotLength(page, slot);
    if (length == 0) return nullptr;  // 墓碑

    const uint16_t offset = slotOffset(page, slot);
    if (static_cast<size_t>(offset) + length > PAGE_SIZE) return nullptr;  // 保险丝

    lengthOut = length;
    return page + offset;
}

void heapDelete(char* page, uint16_t slot) {
    if (slot >= heapSlotCount(page)) return;
    // 【只改长度，不改偏移】
    // 偏移留着，"这行原来在哪"这个信息就还在——将来做调试工具
    // 或者数据库修复的时候用得上。而且改成 0 之后 heapCell 一定
    // 返回 nullptr，读的人绕不过去。
    setSlot(page, slot, slotOffset(page, slot), 0);
}

bool heapCompact(char* page) {
    const uint16_t slotCount = heapSlotCount(page);

    // 先看有没有墓碑。一个都没有就什么都不用做——
    // 压实一个已经紧实的页是纯浪费时间，而且写入次数会变多。
    // （这里先扫一遍，比"压完发现没变"要诚实：调用方拿到的
    //   返回值能直接说明"我到底帮上忙没有"。）
    bool hasTombstone = false;
    for (uint16_t slot = 0; slot < slotCount; slot++) {
        if (slotLength(page, slot) == 0) {
            hasTombstone = true;
            break;
        }
    }
    if (!hasTombstone) return false;

    // 【为什么先拷到临时缓冲，而不是就地 memmove】
    // 就地搬要处理"源和目标重叠、且方向不确定"的情况——
    // 往页尾搬和往页头搬，memmove 的方向正好相反，而起始位置
    // 又取决于第一块放哪。这种代码能写对，但读的人得在脑子里
    // 模拟一遍才敢确认。4096 字节的栈上缓冲换掉这段心智负担，
    // 是笔划算的买卖。
    std::vector<char> live;
    live.reserve(PAGE_SIZE);

    // 目标位置从页尾往回排。先算好每块的新偏移。
    struct Placed {
        uint16_t slot;
        uint16_t newOffset;
        uint16_t length;
    };
    std::vector<Placed> placed;
    placed.reserve(slotCount);

    size_t cursor = PAGE_SIZE;
    for (uint16_t slot = 0; slot < slotCount; slot++) {
        const uint16_t length = slotLength(page, slot);
        if (length == 0) continue;  // 墓碑，扔掉

        cursor -= length;
        placed.push_back(Placed{slot, static_cast<uint16_t>(cursor), length});

        const char* source = page + slotOffset(page, slot);
        live.insert(live.end(), source, source + length);
    }

    // live 里的顺序就是 placed 的顺序（都是按 slot 从小到大），
    // 所以写回时按同样的顺序从头切。
    std::memcpy(page + cursor, live.data(), live.size());

    for (const Placed& item : placed) {
        setSlot(page, item.slot, item.newOffset, item.length);
    }

    writeU16(page + HP_CELL_TOP, static_cast<uint16_t>(cursor));

    // ---- 墓碑的槽位本身不删 ----
    // 长度已经是 0 了，占的 4 字节留着。原因见 heapInsert 里那段。
    return true;
}

// ------------------------------------------------------------
//  一行的编解码
// ------------------------------------------------------------

namespace {

enum class ValueTag : unsigned char {
    NULL_TAG = 0x00,
    INTEGER = 0x01,
    REAL = 0x02,
    TEXT = 0x03,
    BOOLEAN = 0x04,
};

void putBytes(std::string& out, const void* data, size_t length) {
    out.append(static_cast<const char*>(data), length);
}

void putU32(std::string& out, uint32_t value) {
    char buffer[4];
    writeU32(buffer, value);
    putBytes(out, buffer, 4);
}

void putU16(std::string& out, uint16_t value) {
    char buffer[2];
    writeU16(buffer, value);
    putBytes(out, buffer, 2);
}

void putU64(std::string& out, uint64_t value) {
    char buffer[8];
    writeU64(buffer, value);
    putBytes(out, buffer, 8);
}

// 从字节流里按类型取一个值。
// length 是这一行的总长，每一步都要检查还剩多少——
// 文件坏了的时候不能一路读出缓冲区外面去。
struct Reader {
    const char* data;
    size_t length;
    size_t at = 0;

    bool remaining(size_t count) const { return at + count <= length; }
};

}  // namespace

std::string encodeValue(const Value& value) {
    std::string out;
    out.reserve(9);

    switch (value.type) {
        case Value::Type::NULL_VALUE:
            out.push_back(static_cast<char>(ValueTag::NULL_TAG));
            break;

        case Value::Type::INTEGER:
            out.push_back(static_cast<char>(ValueTag::INTEGER));
            putU64(out, static_cast<uint64_t>(value.integer));
            break;

        case Value::Type::REAL: {
            out.push_back(static_cast<char>(ValueTag::REAL));
            // 先取 double 的位模式，再按小端写。
            // 【不能直接对 double 取地址按字节写】那样子节顺序
            // 就跟着本机走了，换个大端机器读出来是另一个数。
            // 转成 uint64_t 之后走和整数同一条路，字节序是
            // 我们自己在 writeU64 里写死的。
            uint64_t bits = 0;
            static_assert(sizeof(double) == sizeof(uint64_t),
                          "double 必须是 8 字节，否则这个格式不成立");
            std::memcpy(&bits, &value.real, sizeof(bits));
            putU64(out, bits);
            break;
        }

        case Value::Type::TEXT:
            out.push_back(static_cast<char>(ValueTag::TEXT));
            putU32(out, static_cast<uint32_t>(value.text.size()));
            putBytes(out, value.text.data(), value.text.size());
            break;

        case Value::Type::BOOLEAN:
            out.push_back(static_cast<char>(ValueTag::BOOLEAN));
            out.push_back(static_cast<char>(value.boolean ? 1 : 0));
            break;
    }

    return out;
}

std::string encodeRow(const Row& row) {
    std::string out;
    out.reserve(row.size() * 9);

    // 一行就是"把每个值编一遍，接起来"。就这么简单——
    // 因为单个值的格式本身是自描述的（标签说了后面多长）。
    // 要是编码不自描述，这里就还得写一份"每个值多长"的表格。
    for (const Value& value : row) {
        out += encodeValue(value);
    }

    return out;
}

Value decodeValue(const char* data, size_t length, size_t& usedOut) {
    Reader reader{data, length, 0};
    Value value;

    if (!reader.remaining(1)) {
        throw AcornError("数据文件坏了：这里应该有一个值的类型标签，但没有了");
    }
    const auto tag = static_cast<ValueTag>(
        static_cast<unsigned char>(data[reader.at]));
    reader.at++;

    switch (tag) {
        case ValueTag::NULL_TAG:
            value = Value::makeNull();
            break;

        case ValueTag::INTEGER: {
            if (!reader.remaining(8)) {
                throw AcornError("数据文件坏了：读整数时字节不够了");
            }
            value = Value::makeInt(
                static_cast<long long>(readU64(data + reader.at)));
            reader.at += 8;
            break;
        }

        case ValueTag::REAL: {
            if (!reader.remaining(8)) {
                throw AcornError("数据文件坏了：读浮点数时字节不够了");
            }
            const uint64_t bits = readU64(data + reader.at);
            reader.at += 8;
            double real = 0.0;
            std::memcpy(&real, &bits, sizeof(real));
            value = Value::makeReal(real);
            break;
        }

        case ValueTag::TEXT: {
            if (!reader.remaining(4)) {
                throw AcornError("数据文件坏了：读文本长度时字节不够了");
            }
            const uint32_t textLength = readU32(data + reader.at);
            reader.at += 4;
            if (!reader.remaining(textLength)) {
                throw AcornError("数据文件坏了：文本说有 " +
                                 std::to_string(textLength) +
                                 " 字节，但剩下的字节不够");
            }
            value = Value::makeText(
                std::string(data + reader.at, textLength));
            reader.at += textLength;
            break;
        }

        case ValueTag::BOOLEAN: {
            if (!reader.remaining(1)) {
                throw AcornError("数据文件坏了：读布尔值时字节不够了");
            }
            value = Value::makeBool(data[reader.at] != 0);
            reader.at += 1;
            break;
        }

        default:
            // 【这里必须报错，不能"猜一个"】
            // 读到不认识的标签说明这段字节不是我们以为的东西。
            // 猜下去的话，后面每一个值的边界都是错的——
            // 结果是一堆看着挺正常、但内容是错的行。
            // 一个数据库最怕的不是报错，是不报错但答案不对。
            throw AcornError("数据文件坏了：不认识的类型标签 " +
                             std::to_string(static_cast<int>(
                                 static_cast<unsigned char>(data[0]))) +
                             "（在解一个值的时候）");
    }

    usedOut = reader.at;
    return value;
}

Row decodeRow(const char* data, size_t length) {
    Row row;
    size_t at = 0;

    while (at < length) {
        size_t used = 0;
        // 这里传的是**剩下的**字节数，不是总长——
        // decodeValue 拿它当边界，才能发现"文本说还有 100 字节，
        // 但这行已经到头了"这种坏文件。
        row.push_back(decodeValue(data + at, length - at, used));
        if (used == 0) {
            // decodeValue 不可能返回 0（每个值至少 1 个字节的标签），
            // 但万一将来有人改坏了它，这里会变成一个**死循环**。
            // 死循环比报错难查得多，所以卡一道。
            throw AcornError("内部错误：解一个值时一个字节都没往前走");
        }
        at += used;
    }

    return row;
}


// ------------------------------------------------------------
//  目录页
// ------------------------------------------------------------

void initCatalogPage(char* page, uint32_t next) {
    // 布局和堆页一模一样（通用页头 + slotCount + cellTop），
    // 所以直接借堆页的初始化。见 catalogAppend 里的说明。
    initHeapPage(page, next);
    setPageType(page, PageType::CATALOG);
}

void initBtreePage(char* page, PageType type, uint32_t next) {
    // 同上：B+ 树的节点也是槽位页，只是页类型不同。
    //
    // 【这里就是"页"这个抽象兑现价值的地方】
    // 三种完全不同的东西——表的数据、表结构、B+ 树节点——
    // 用的是同一套槽位页的代码：同一个 initHeapPage、
    // 同一个 heapInsertAt、同一个 heapCompact、同一个 heapDelete。
    // 区别只有第 0 个字节的页类型。
    initHeapPage(page, next);
    setPageType(page, type);
}

bool catalogFits(const char* page, size_t length) {
    return heapFreeBytes(page) >= length + SLOT_SIZE;
}

bool catalogAppend(char* page, const char* record, size_t length,
                   uint16_t& offsetOut) {
    // 目录页和堆页的物理布局完全一样，所以直接借堆页的那套代码。
    // 【为什么可以借】因为"往一页里塞一段变长字节"这个问题和
    // "塞的是行还是表定义"毫无关系。这里没有为了复用而扭曲什么——
    // 布局一样就用同一份实现，这是布局一样本来就该有的好处。
    //
    // 代价是返回值里那个"槽号"对目录页没意义（我们不按槽号找记录），
    // 真正有用的是**偏移**，所以下面再取一次偏移。
    uint16_t slot = 0;
    if (!heapInsert(page, record, length, slot)) return false;

    // 偏移和槽号不是一回事：槽号是"第几个槽位"，偏移是"字节在哪"。
    // （槽位数组里 4 字节一项，偏移是页内绝对位置。）
    size_t cellLength = 0;
    const char* cell = heapCell(page, slot, cellLength);
    if (cell == nullptr) return false;
    offsetOut = static_cast<uint16_t>(cell - page);
    return true;
}

void catalogScan(const char* page,
                 const std::function<void(const char*, size_t, uint16_t)>& visit) {
    const uint16_t slotCount = heapSlotCount(page);
    for (uint16_t slot = 0; slot < slotCount; slot++) {
        size_t length = 0;
        const char* cell = heapCell(page, slot, length);
        if (cell == nullptr) continue;  // 墓碑不可能出现，但别崩
        // 【为什么用偏移而不是槽号当"这条记录在哪"的句柄】
        // 因为 DROP TABLE 要回头把这条记录的标志位改成已删，
        // 而标志位是记录里的第 0 个字节——得知道它在页内的绝对位置。
        // 槽号还得再查一次表，偏移是直接的。
        visit(cell, length, static_cast<uint16_t>(cell - page));
    }
}

void catalogMarkDeleted(char* page, uint16_t offset) {
    if (offset >= PAGE_SIZE) return;
    page[offset] = 1;
}

namespace {

constexpr unsigned char CATALOG_ALIVE = 0;
constexpr unsigned char CATALOG_DELETED = 1;

void putStr(std::string& out, const std::string& text) {
    putU32(out, static_cast<uint32_t>(text.size()));
    putBytes(out, text.data(), text.size());
}

bool takeStr(const char* data, size_t length, size_t& at, std::string& out) {
    if (at + 4 > length) return false;
    const uint32_t size = readU32(data + at);
    at += 4;
    if (at + size > length) return false;
    out.assign(data + at, size);
    at += size;
    return true;
}

}  // namespace

std::string encodeCatalogRecord(const TableSchema& schema, uint32_t firstPage) {
    std::string out;
    out.push_back(static_cast<char>(CATALOG_ALIVE));
    putU32(out, firstPage);
    putStr(out, schema.name);
    putU32(out, static_cast<uint32_t>(schema.columns.size()));
    for (const Column& column : schema.columns) {
        // 【只存 name，不存 key】key 就是 name 转小写，
        // 存两份的话，将来 lowercase 的实现一变（比如改成支持
        // 土耳其语的 i），磁盘上就有一份永远追不上的旧数据。
        // 凡是能算出来的东西都不存——这是"同一个东西存两处，
        // 早晚有一处忘了更新"的又一次应用。
        putStr(out, column.name);
        out.push_back(static_cast<char>(static_cast<unsigned char>(column.type)));
    }
    return out;
}

bool decodeCatalogRecord(const char* data, size_t length, TableSchema& schemaOut,
                         uint32_t& firstPageOut) {
    // 长度不够就当"不是一条合法记录"，返回 false 让调用方决定怎么办。
    // 这里不抛异常：扫目录的时候遇到一条坏记录，该报错还是该跳过，
    // 是调用方的事（现在是报错，但将来做修复工具就得跳过）。
    size_t at = 1;  // 跳过标志字节

    if (at + 4 > length) return false;
    firstPageOut = readU32(data + at);
    at += 4;

    std::string tableName;
    if (!takeStr(data, length, at, tableName)) return false;

    if (at + 4 > length) return false;
    const uint32_t columnCount = readU32(data + at);
    at += 4;

    std::vector<Column> columns;
    columns.reserve(columnCount);
    for (uint32_t i = 0; i < columnCount; i++) {
        std::string columnName;
        if (!takeStr(data, length, at, columnName)) return false;
        if (at + 1 > length) return false;
        const auto type =
            static_cast<Value::Type>(static_cast<unsigned char>(data[at]));
        at += 1;

        if (type != Value::Type::INTEGER && type != Value::Type::REAL &&
            type != Value::Type::TEXT && type != Value::Type::BOOLEAN) {
            return false;  // 认不出来的列类型
        }
        columns.push_back(makeColumn(std::move(columnName), type));
    }

    schemaOut = makeTableSchema(std::move(tableName), std::move(columns));
    return true;
}

// ------------------------------------------------------------
//  索引目录记录
// ------------------------------------------------------------

void catalogSetRecordPage(char* page, uint16_t offset, uint32_t pageNo) {
    if (static_cast<size_t>(offset) + 5 > PAGE_SIZE) return;
    // 偏移 1~4，和表记录里"第一个数据页"的位置重合。
    // 两个格式在这里对齐是**故意**的——见 page.hpp 里的说明。
    writeU32(page + offset + 1, pageNo);
}

std::string encodeIndexRecord(const IndexSchema& schema, uint32_t rootPage) {
    std::string out;
    out.push_back(static_cast<char>(CATALOG_ALIVE));
    putU32(out, rootPage);
    putStr(out, schema.name);
    // 表名存**用户写的原样**（就是 TableSchema 里那个 name）——
    // 打开库的时候要拿它去 tables_ 里找，而 tables_ 的 key 是小写，
    // 所以真正参与查找的是它的 lowercase，不是它本身。
    putStr(out, schema.tableName);
    // 列号用 2 字节。列数不可能超过 65535（一页的目录记录都装不下
    // 那么多列名），而且列号是**非负**的，所以用 uint16 足够。
    // 存 -1（"没有这一列"）是不可能的：建索引的时候已经查过了。
    putU16(out, static_cast<uint16_t>(schema.column));    return out;
}

bool decodeIndexRecord(const char* data, size_t length, IndexSchema& schemaOut,
                       uint32_t& rootPageOut) {
    size_t at = 1;  // 跳过标志字节

    if (at + 4 > length) return false;
    rootPageOut = readU32(data + at);
    at += 4;

    std::string indexName;
    if (!takeStr(data, length, at, indexName)) return false;

    std::string tableName;
    if (!takeStr(data, length, at, tableName)) return false;

    if (at + 2 > length) return false;
    const uint16_t column = readU16(data + at);
    at += 2;

    // 列类型**不存在记录里**——打开库的时候从表结构里现查。
    // 存一份的话，将来加 `ALTER TABLE`（改列类型）就会出现
    // "索引记录里说是 INT，表里说是 TEXT"这种两个真相打架的局面。
    // 这里先填 NULL_VALUE 占位，由 Database 拿到表结构之后再补上。
    schemaOut = makeIndexSchema(std::move(indexName), std::move(tableName),
                                lowercase(tableName), static_cast<int>(column),
                                Value::Type::NULL_VALUE);
    return true;
}

// ------------------------------------------------------------
//  小工具
// ------------------------------------------------------------

std::string pageName(uint32_t pageNo) {
    if (pageNo == INVALID_PAGE) return "（没有）";
    return "第 " + std::to_string(pageNo) + " 页";
}