#include "journal.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "error.hpp"
#include "utf8path.hpp"

#ifdef _WIN32
#include <io.h>  // _commit / _fileno
#else
#include <unistd.h>  // fsync / fileno
#endif

namespace {

// 日志头的字段位置。**这几个数是文件格式的一部分**，
// 改了就读不出旧日志了——所以写成常量，而不是散在代码里的字面量。
constexpr size_t H_MAGIC = 0;
constexpr size_t H_VERSION = 5;
constexpr size_t H_RESERVED_1 = 6;
constexpr size_t H_PAGE_COUNT = 8;
constexpr size_t H_CHECKSUM = 12;

// 到 H_CHECKSUM 为止（不含）的字节数——校验和覆盖的范围。
constexpr size_t CHECKSUM_SPAN = 12;

constexpr char JOURNAL_MAGIC[5] = {'A', 'C', 'O', 'R', 'J'};
constexpr uint32_t JOURNAL_FORMAT_VERSION = 1;

// 头这 12 个字节加起来。
//
// 【为什么用加法这么弱的校验】因为它要防的不是"有人恶意改日志"，
// 是"这个头写完整了没有"。半个头、错位的头、一串随机字节，
// 加起来对不上原值的概率是 1/2^32——对这件事足够了。
// 用 CRC 或者哈希不会让它更安全，只会让代码多一块看不懂的东西。
uint32_t headerChecksum(const char* header) {
    uint32_t sum = 0;
    for (size_t i = 0; i < CHECKSUM_SPAN; i++) {
        sum += static_cast<unsigned char>(header[i]);
    }
    return sum;
}

// 拼一个头。pageCount 是事务开始时库文件有几页。
std::vector<char> makeHeader(uint32_t pageCount) {
    std::vector<char> header(JOURNAL_HEADER_SIZE, 0);
    std::memcpy(header.data() + H_MAGIC, JOURNAL_MAGIC, sizeof(JOURNAL_MAGIC));
    header[H_VERSION] = static_cast<char>(JOURNAL_FORMAT_VERSION);
    writeU16(header.data() + H_RESERVED_1, 0);
    writeU32(header.data() + H_PAGE_COUNT, pageCount);
    writeU32(header.data() + H_CHECKSUM,
             headerChecksum(header.data()));
    return header;
}

// 这一页的原始内容在日志文件里的偏移。
size_t recordOffset(size_t index) {
    return JOURNAL_HEADER_SIZE + index * JOURNAL_RECORD_SIZE;
}

}  // namespace

// ------------------------------------------------------------
//  落盘
// ------------------------------------------------------------

void syncFileToDisk(std::FILE* file) {
    if (file == nullptr) return;

    // 先让 C 库把它的缓冲区交给操作系统……
    if (std::fflush(file) != 0) {
        throw AcornError("把数据交给操作系统时失败（磁盘满了？）");
    }

    // ……再让操作系统交给磁盘。
    // 【这一句才是重点】少了它，前面那句只是"看起来写了"。
#ifdef _WIN32
    if (_commit(_fileno(file)) != 0) {
        throw AcornError("把数据刷到磁盘时失败（磁盘满了？）");
    }
#else
    if (fsync(fileno(file)) != 0) {
        throw AcornError("把数据刷到磁盘时失败（磁盘满了？）");
    }
#endif
}

std::string journalPathFor(const std::string& dbPath) {
    if (dbPath.empty()) return std::string();
    return dbPath + "-journal";
}

// ------------------------------------------------------------
//  Journal
// ------------------------------------------------------------

Journal::Journal(const std::string& dbPath) : path_(journalPathFor(dbPath)) {
    // 打开库的时候，日志文件**不该**还在——上一次正常结束的话，
    // 提交或回滚的最后一步就是把它删掉。要是它还在，说明上一次
    // 没走完。这件事由 recoverJournal 在 Pager 打开文件之前处理掉，
    // 走到这里时应该是干净的。
    //
    // 【这里不做恢复】因为恢复要动库文件，而那时候 Pager 还没
    // 算好页数、还没建缓存。顺序反了会读到一半的状态。
    // 恢复必须发生在"Pager 认为文件是什么样"之前，见 pager.cpp。
}

Journal::~Journal() {
    if (file_ != nullptr) {
        std::fclose(file_);
    }
}

void Journal::begin(uint32_t pageCount) {
    originalPageCount_ = pageCount;
    pageNos_.clear();
    seen_.clear();
    active_ = true;
    opened_ = false;
    needsSync_ = false;
    memory_.clear();

    // 上一次的日志文件理论上不在了（forget 删过，或者 recoverJournal
    // 删过）。真在的话这里覆盖它——反正新的这个事务马上要重写它。
    if (file_ != nullptr) {
        std::fclose(file_);
        file_ = nullptr;
    }
}

void Journal::openIfNeeded() {
    if (opened_) return;

    const std::vector<char> header = makeHeader(originalPageCount_);

    if (path_.empty()) {
        memory_.assign(JOURNAL_HEADER_SIZE, 0);
    } else {
        // ★ 必须是 "w+b"，不能是 "wb" ★
        //
        // "wb" 是**只写**。回滚的时候要把它读回来（forEachRecord），
        // 而从一个只写流里 fread，标准说这是未定义行为——具体到
        // 实现上就是**一个字节都读不到，返回 0**，而且**不报错**。
        // 症状：回滚把全 0 写回库文件，然后查询报一句
        // "不认识的类型标签 255"——离肇事现场十万八千里。
        //
        // （那个 "w" 负责截断：一个事务一份日志，上一次的残留
        //   不该混进来。所以是 "w+b" 而不是 "r+b"。）
        file_ = openPath(path_, "w+b");
        if (file_ == nullptr) {
            throw AcornError("建不了日志文件：" + path_ +
                             "（目录不存在？没有写权限？）");
        }
    }

    opened_ = true;
    writeAt(0, header.data(), header.size());
}

void Journal::record(uint32_t pageNo, const char* page) {
    if (!active_) return;
    if (seen_.count(pageNo) != 0) return;  // 只记第一次，见头文件里的说明
    seen_.insert(pageNo);

    openIfNeeded();

    const size_t offset = recordOffset(pageNos_.size());
    char pageBytes[4];
    writeU32(pageBytes, pageNo);
    writeAt(offset, pageBytes, sizeof(pageBytes));
    writeAt(offset + sizeof(pageBytes), page, PAGE_SIZE);

    pageNos_.push_back(pageNo);
    needsSync_ = true;
}

void Journal::syncIfNeeded() {
    if (!needsSync_) return;
    needsSync_ = false;

    if (path_.empty()) return;  // 内存库：没有磁盘可刷
    if (file_ == nullptr) return;

    syncFileToDisk(file_);
}

void Journal::forget() {
    if (file_ != nullptr) {
        std::fclose(file_);
        file_ = nullptr;
    }

    // 【删文件失败也不报错】因为删不掉不影响正确性——下次打开会
    // 看到这个日志，照它退回去。退回去的结果是一样的（那一页本来
    // 就该是那个样子），只是白做一遍。为一个"白做一遍"报错，
    // 会把一次成功的提交说成失败，那更糟。
    if (!path_.empty()) {
        removePath(path_);
    }

    pageNos_.clear();
    seen_.clear();
    memory_.clear();
    active_ = false;
    opened_ = false;
    needsSync_ = false;
}

bool Journal::fileExists() const {
    if (path_.empty()) return !memory_.empty();
    if (file_ != nullptr) return true;
    return journalFileExists(path_);
}

bool journalFileExists(const std::string& dbPath) {
    // 内存库没有日志文件这回事（它的日志记在内存里）。
    if (dbPath.empty()) return false;

    // 开一下再关掉，就是"存在吗"这个问题的答案。
    // 【为什么不用 std::filesystem】它要的是一个路径，而我们
    // 这儿关心的其实是"这个名字现在能不能打开"——一个目录、
    // 一个没权限的文件，"查一下"和"打开"给的答案不一样。
    // 用 openPath 是因为后面真的要 openPath 它，问的是同一件事。
    const std::string path = journalPathFor(dbPath);
    std::FILE* probe = openPath(path, "rb");
    if (probe == nullptr) return false;
    std::fclose(probe);
    return true;
}

void Journal::forEachRecord(
    const std::function<void(uint32_t, const char*)>& fn) const {
    std::vector<char> buffer(PAGE_SIZE);
    for (size_t i = 0; i < pageNos_.size(); i++) {
        readAt(recordOffset(i) + 4, buffer.data(), PAGE_SIZE);
        fn(pageNos_[i], buffer.data());
    }
}

// ------------------------------------------------------------
//  两种落点
// ------------------------------------------------------------

void Journal::writeAt(size_t offset, const char* data, size_t length) {
    if (path_.empty()) {
        if (memory_.size() < offset + length) {
            memory_.resize(offset + length, 0);
        }
        std::memcpy(memory_.data() + offset, data, length);
        return;
    }

    if (std::fseek(file_, static_cast<long>(offset), SEEK_SET) != 0) {
        throw AcornError("日志文件定位失败：" + path_);
    }
    if (std::fwrite(data, 1, length, file_) != length) {
        throw AcornError("写日志文件失败：" + path_ + "（磁盘满了？）");
    }
}

void Journal::readAt(size_t offset, char* out, size_t length) const {
    if (path_.empty()) {
        if (offset + length <= memory_.size()) {
            std::memcpy(out, memory_.data() + offset, length);
        } else {
            std::memset(out, 0, length);
        }
        return;
    }

    if (std::fseek(file_, static_cast<long>(offset), SEEK_SET) != 0) {
        throw AcornError("日志文件定位失败：" + path_);
    }
    const size_t got = std::fread(out, 1, length, file_);
    if (got < length) {
        std::memset(out + got, 0, length - got);
    }
}

// ------------------------------------------------------------
//  崩溃恢复
// ------------------------------------------------------------

// 日志文件不存在就什么也不做。
// 存在但读不出来（头对不上）就报错——**不猜，也不动库文件**。
bool recoverJournal(const std::string& dbPath) {
    const std::string journalPath = journalPathFor(dbPath);
    if (journalPath.empty()) return false;  // 内存库没有崩溃恢复这回事

    std::FILE* journal = openPath(journalPath, "rb");
    if (journal == nullptr) return false;  // 没有日志 = 上次走完了

    // 从文件尾问出大小。要它是因为下面要判断"记录有没有写完整"。
    if (std::fseek(journal, 0, SEEK_END) != 0) {
        std::fclose(journal);
        throw AcornError("读不出日志文件的大小：" + journalPath);
    }
    const long journalSize = std::ftell(journal);

    // ---- 只有头，或者连头都没有 ----
    //
    // 【为什么这种情况可以直接删掉】因为记录是**一条一条追加**的：
    // 有记录就一定有头。反过来说，文件是空的就说明一次 record 都没发生过，
    // 也就是没有任何旧内容需要退回去。而事务没结束就崩了，
    // 那些改动根本没写进库文件（写库文件之前一定先刷日志）——
    // 所以什么也不用做。
    if (journalSize < static_cast<long>(JOURNAL_HEADER_SIZE)) {
        std::fclose(journal);
        removePath(journalPath);
        return false;
    }

    char header[JOURNAL_HEADER_SIZE];
    if (std::fseek(journal, 0, SEEK_SET) != 0 ||
        std::fread(header, 1, JOURNAL_HEADER_SIZE, journal) !=
            JOURNAL_HEADER_SIZE) {
        std::fclose(journal);
        throw AcornError("日志文件读不出来：" + journalPath);
    }

    // ---- 头看着对不对 ----
    const bool magicOk =
        std::memcmp(header + H_MAGIC, JOURNAL_MAGIC, sizeof(JOURNAL_MAGIC)) == 0;
    const bool checksumOk =
        readU32(header + H_CHECKSUM) == headerChecksum(header);

    if (!magicOk || !checksumOk) {
        std::fclose(journal);
        // ★ 这里绝不"猜"★
        // 头对不上，意味着我们**不知道**这个日志里记了什么、
        // 也不知道库文件被改到哪一步了。这时候唯一安全的动作是停手：
        // 库文件一个字节都不动，让用户来决定。
        // 自作聪明地"当成没有日志"接着跑，可能正好把一个
        // 改了一半的事务当成完整的接受下来——那是真正的数据损坏。
        throw AcornError(
            "日志文件 " + journalPath +
                " 的头读不出来（不是一个完整的回滚日志）。\n"
                "        这说明上一次退出时正在写这个日志，而库文件"
                "可能停在一半的状态。\n"
                "        为了不把数据改得更坏，程序不会动这个库。\n"
                "        如果确认库文件是好的，把这个日志文件删掉再试。");
    }

    const uint32_t originalPageCount = readU32(header + H_PAGE_COUNT);

    // 日志文件里除了头，剩下多少字节。除不尽的那一截是**写了一半的
    // 最后一条记录**——它对应的那一页一定还没进库文件（进之前会先刷
    // 日志，而刷过的记录不会是半截的），所以直接丢掉不管。
    const long bodySize = journalSize - static_cast<long>(JOURNAL_HEADER_SIZE);
    const size_t recordCount =
        static_cast<size_t>(bodySize) / JOURNAL_RECORD_SIZE;
    const size_t droppedBytes =
        static_cast<size_t>(bodySize) % JOURNAL_RECORD_SIZE;

    // 【记录数为 0 也要往下走】因为事务可能只是**让文件变长**了
    // （新分配的页在事务期间就写进了库文件）。那些页一条记录都没有
    // ——它们原来"不存在"，没有什么旧内容可记——但回滚时得把它们切掉。
    // 这个切的动作就在下面，所以不能在这里提前返回。

    // ---- 打开库文件，准备退回去 ----
    std::FILE* db = openPath(dbPath, "r+b");
    if (db == nullptr) {
        std::fclose(journal);
        throw AcornError("有日志要回滚，但打不开库文件：" + dbPath);
    }

    std::vector<char> page(PAGE_SIZE);
    char pageNoBytes[4];
    size_t restored = 0;

    for (size_t i = 0; i < recordCount; i++) {
        const long offset = static_cast<long>(recordOffset(i));
        if (std::fseek(journal, offset, SEEK_SET) != 0 ||
            std::fread(pageNoBytes, 1, 4, journal) != 4 ||
            std::fread(page.data(), 1, PAGE_SIZE, journal) != PAGE_SIZE) {
            std::fclose(journal);
            std::fclose(db);
            throw AcornError("日志文件读了一半失败了：" + journalPath);
        }

        const uint32_t pageNo = readU32(pageNoBytes);
        const long dbOffset = static_cast<long>(pageNo) * static_cast<long>(PAGE_SIZE);

        if (std::fseek(db, dbOffset, SEEK_SET) != 0 ||
            std::fwrite(page.data(), 1, PAGE_SIZE, db) != PAGE_SIZE) {
            std::fclose(journal);
            std::fclose(db);
            throw AcornError("把日志里的旧内容写回库文件时失败：" + dbPath);
        }
        restored++;
    }

    // ---- 事务期间长出来的页，整个切掉 ----
    //
    // 【为什么必须切】不切的话那些页会留在文件里，而空闲链上
    // 根本没有它们——谁也拿不到，谁也不认识，纯粹是垃圾。
    // 而且"文件多大就是几页"这条等式会让它们**看起来是合法的页**，
    // 于是下次分配新页时它们可能被当成自由空间用掉——
    // 里面却躺着上一个事务的残骸。
    const long originalSize =
        static_cast<long>(originalPageCount) * static_cast<long>(PAGE_SIZE);

    // 【顺序：先冲缓冲区，再截断】反过来的话，缓冲区里那些还没写下去的
    // 旧内容会在截断之后被写出去，文件又被撑回原来那么长。
    // 混用 stdio 缓冲和文件描述符级操作时，这是最常见的那个坑。
    if (std::fflush(db) != 0) {
        std::fclose(journal);
        std::fclose(db);
        throw AcornError("把回滚的内容交给操作系统时失败：" + dbPath);
    }

    if (std::fseek(db, 0, SEEK_END) != 0) {
        std::fclose(journal);
        std::fclose(db);
        throw AcornError("读不出库文件的大小：" + dbPath);
    }
    const long sizeNow = std::ftell(db);
    if (sizeNow > originalSize) {
#ifdef _WIN32
        const int rc = _chsize(_fileno(db), originalSize);
#else
        const int rc = ftruncate(fileno(db), originalSize);
#endif
        if (rc != 0) {
            std::fclose(journal);
            std::fclose(db);
            throw AcornError("把库文件截回原来的长度时失败：" + dbPath);
        }
    }

    // ★ 退回去的内容必须先真正落盘，才能删日志 ★
    // 反过来的话：日志没了，而退回去的那些页还在操作系统的缓存里，
    // 这时候掉电——数据就永远停在"改了一半"的状态上，而且
    // 再也没有日志能把它救回来了。删日志必须是最后一步。
    syncFileToDisk(db);

    std::fclose(db);
    std::fclose(journal);

    // 到这儿才算真的退完了，删日志。
    removePath(journalPath);

    // 【为什么这件事要让用户看见】因为"上次崩过"是个重要的事实：
    // 用户需要知道哪一次改动没了，才能判断要不要重做。
    // 悄悄退回去的话，他会以为那批数据插进去了。
    std::fprintf(stderr,
                 "警告：上次退出时有个没做完的事务，已经照日志退回去了"
                 "（%zu 页恢复原样，库文件截回 %u 页）\n",
                 restored, originalPageCount);
    if (droppedBytes != 0) {
        std::fprintf(stderr,
                     "      日志末尾有 %zu 字节没写完，忽略了"
                     "（那一页还没来得及进库文件）\n",
                     droppedBytes);
    }

    return true;
}
