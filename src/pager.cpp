#include "pager.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>

#include "error.hpp"
#include "journal.hpp"
#include "utf8path.hpp"

// ------------------------------------------------------------
//  打开 / 关闭
// ------------------------------------------------------------

Pager::Pager(const std::string& path, CrashPoint crash, int lockWaitMs)
    : path_(path),
      lock_(),
      lockWaitMs_(lockWaitMs),
      journal_(path),
      crashPoint_(crash) {
    pool_.resize(PAGER_CACHE_FRAMES * PAGE_SIZE, 0);
    frames_.resize(PAGER_CACHE_FRAMES);

    if (path_.empty()) return;  // 内存库，没有文件这回事

    // ---- ★ 第 0 步：把这个库锁上（阶段 6）★ ----
    //
    // 【为什么排在最前面】因为下面那一步（崩溃恢复）会**写库文件**。
    // 两个进程同时打开一个带着日志的库、同时开始恢复，
    // 就是两个写者同时改一个文件——正是这一阶段要挡的事。
    //
    // 所以顺序是死的：**先锁，再恢复，再打开**。
    // 每一步都在为下一步排除掉一种"别人也在动"的可能。
    //
    // 一开始只要共享锁（"我要读"）：绝大多数调用是 SELECT，
    // 而读者之间是可以并存的。真到了要写的时候再升级（见
    // ensureExclusiveLock）——那时候才是非独占不可。
    std::string lockWhy;
    if (!lock_.acquire(path_, LockMode::SHARED, lockWaitMs_, lockWhy)) {
        throw BusyError(lockWhy);
    }

    // ---- ★ 第一步：收拾上次没走完的事务 ★ ----
    //
    // 【为什么必须是第一步】因为下面马上要从文件大小算出页数。
    // 崩溃留下的文件是**半成品**：它可能比原来长（事务分配的页
    // 已经写进去了）、页的内容也可能是改了一半的。先算页数再恢复的话，
    // pageCount_ 会是那个不对的值，而且恢复要重写的那几页
    // 可能已经在缓存里了。
    //
    // 一句话：**恢复必须发生在"我们认为文件是什么样"之前。**
    // 它抛异常（日志头读不出来）时这里也不该继续——
    // 那种情况下我们不知道文件是什么状态，装作没事是最坏的选择。
    //
    // 【恢复为什么得先升级成独占锁】因为它是彻头彻尾的一次写：
    // 把旧内容写回库文件、再截短。共享锁下做这件事，等于和
    // 另一个也拿着共享锁、也准备恢复的进程同时动库文件。
    if (journalFileExists(path_)) {
        if (!lock_.upgrade(lockWaitMs_, lockWhy)) {
            throw BusyError(lockWhy);
        }

        // ★ 拿到独占锁之后要**再确认一遍日志还在不在** ★
        //
        // 等锁的这段时间里，别人可能已经把这件事做完了
        //（他先拿到了独占锁、恢复完、删了日志、放锁走人）。
        // 这时候我们手里这把锁来得很及时，但要说的事已经没了。
        //
        // 这不是防御性编程，是这一层唯一正确的顺序：
        // **先拿锁、再看状态**。反过来（先看状态、再拿锁）
        // 看到的永远是过期的状态——这个错误有个名字，
        // 叫 check-then-act，是所有并发 bug 的祖宗。
        recoverJournal(path_);

        // 恢复做完了。我们只是打开库，还不知道要不要写，
        // 所以退回共享锁——不然一个刚恢复完的读者会把所有
        // 别的读者都挡在门外。
        lock_.downgrade();
    }

    // ---- 打开文件，没有就建一个 ----
    // 【为什么要分两步】"r+b" 在文件不存在时会失败，而我们要的正是
    // "不存在才算新库"。所以先试 r+b，失败说明文件不在，建一个空文件，
    // 再回来按 r+b 打开。
    //
    // 【为什么不用 std::fstream】因为提交和写日志都需要 fsync，
    // 而 fsync 要的是文件描述符，fstream 不给。用 std::FILE* 就为了这个。
    // （见 journal.hpp 里 syncFileToDisk 的说明：fflush 只到操作系统，
    //   到磁盘还差一步，而那一步才是这一阶段要紧的。）
    file_ = openPath(path_, "r+b");

    if (file_ == nullptr) {
        std::FILE* create = openPath(path_, "wb");
        if (create == nullptr) {
            throw AcornError("建不了数据库文件：" + path_ +
                             "（目录不存在？没有写权限？）");
        }
        std::fclose(create);

        file_ = openPath(path_, "r+b");
        if (file_ == nullptr) {
            throw AcornError("打不开数据库文件：" + path_);
        }
    }

    // ---- 页数 = 文件大小 / 4096 ----
    // 【这里是唯一算页数的地方】文件有多大，文件系统最清楚。
    // 在文件头里再记一份的话就有了两个真相来源，对不上时不知道信谁。
    if (std::fseek(file_, 0, SEEK_END) != 0) {
        throw AcornError("读不出数据库文件的大小：" + path_);
    }
    const long size = std::ftell(file_);
    if (size < 0) {
        throw AcornError("读不出数据库文件的大小：" + path_);
    }
    if (size % static_cast<long>(PAGE_SIZE) != 0) {
        // 文件长度不是 4096 的整数倍，说明它**不是**（或者不再是）
        // 一个 Acorn 数据库。可能是被别的程序改过、或者是拷了一半。
        // 这时候继续读会读到半页数据，全是垃圾——当场拦住。
        throw AcornError(
            "文件 " + path_ + " 的大小是 " + std::to_string(size) +
            " 字节，不是 4096 的整数倍——它不像是一个 Acorn 数据库文件");
    }
    pageCount_ = static_cast<uint32_t>(size / static_cast<long>(PAGE_SIZE));
}

Pager::~Pager() {
    // ---- 退出时事务还开着 = 回滚（阶段 5）----
    //
    // 【为什么不是提交】因为提交必须是一个**明确的动作**。
    // 要是"关程序"就等于"提交"，那用户敲完 BEGIN 反悔了，
    // 唯一的办法是记得敲 ROLLBACK——忘一次，改动就生效了。
    // 真数据库（SQLite、PostgreSQL）全都是这个规矩：没提交就是没发生。
    //
    // 【先说清楚它和崩溃的区别】这条路径是"进程好好地退出了"，
    // 所以能安安稳稳地回滚。真崩溃（掉电、被 kill）走的是
    // 下次打开时的 recoverJournal——两条路，同一个结果。
    if (inTransaction_) {
        if (crashPoint_ == CrashPoint::EXIT) {
            // 测试用：模拟"事务开着的时候进程没了"。
            // _Exit 不走析构、不刷缓冲区，和掉电一样。
            std::_Exit(1);
        }
        try {
            rollbackTransaction();
        } catch (const std::exception& error) {
            std::fprintf(stderr, "警告：退出时回滚失败：%s\n", error.what());
        }
    }

    // 【析构里不能抛异常】析构函数抛异常会让 std::terminate 直接
    // 干掉进程，用户看到的是"程序异常终止"而不是"磁盘满了"。
    // 所以这里把异常吃掉，只把错误打到 stderr。
    // 真正的处理办法是调用方显式 flush()——那才是能报错的地方。
    try {
        flush();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "警告：关闭数据库时写盘失败：%s\n", error.what());
    }
    if (file_ != nullptr) {
        std::fclose(file_);
        file_ = nullptr;
    }

    // ---- 放锁（阶段 6）----
    //
    // 【为什么这一句其实可有可无】进程消失的时候内核会把它的
    // 所有锁一起收走——正常退出是这样，被 kill、掉电也是这样。
    // 这正是选文件锁而不是"自己写个 .lock 文件记进程号"的
    // 全部理由：**放锁这件事不用我们操心。**（见 lock.hpp）
    //
    // 写出来是为了让"这个进程用完了这个库"在代码里有个落点，
    // 也让下面那件事成立：`acorn` 跑完之后另一个进程能立刻进来。
    lock_.release();
}

std::string Pager::lockState() const {
    if (path_.empty()) {
        // 内存库没有"别的进程"这回事——它是一块只属于这个进程的内存。
        return "不适用（内存库别的进程看不见，没有抢的问题）";
    }

    switch (lock_.mode()) {
        case LockMode::EXCLUSIVE:
            return "独占（这个进程能读也能写，别的进程进不来）";
        case LockMode::SHARED:
            return "共享（只能读；别的读者也能同时进来，但谁都写不了）";
        case LockMode::NONE:
        default:
            // 只在一种情况下会走到这儿：事务刚结束、退回共享锁时
            // 正好有别的进程在写，于是暂时没有锁。下一次取页会
            // 重新去拿（Pager::ensureSharedLock），拿不到就报错。
            return "暂时没有（刚被别的写者挤掉，下次取页会重新去拿）";
    }
}

// ------------------------------------------------------------
//  Handle
// ------------------------------------------------------------

Pager::Handle::Handle(Pager* pager, uint32_t pageNo, char* data,
                      size_t frameIndex)
    : pager_(pager), pageNo_(pageNo), data_(data), frameIndex_(frameIndex) {}

void Pager::Handle::release() {
    if (pager_ == nullptr) return;
    Frame& frame = pager_->frames_[frameIndex_];
    // 检查一下是不是还是同一页：正常情况一定是，
    // 但万一有谁把借条存起来用过了头（Pager 已经把它换出去了），
    // 这里至少不会错误地 unpin 掉别人的格子。
    if (frame.pageNo == pageNo_ && frame.pinned) {
        frame.pinned = false;
        frame.referenced = true;  // 刚用过，给它一次机会
    }
}

Pager::Handle::~Handle() {
    release();
}

Pager::Handle::Handle(Handle&& other) noexcept
    : pager_(other.pager_),
      pageNo_(other.pageNo_),
      data_(other.data_),
      frameIndex_(other.frameIndex_) {
    other.pager_ = nullptr;
    other.pageNo_ = INVALID_PAGE;
    other.data_ = nullptr;
}

Pager::Handle& Pager::Handle::operator=(Handle&& other) noexcept {
    if (this == &other) return *this;

    // 【先还掉自己手里这张借条】不然这一页就永远 pin 住了——
    // 一个再也踢不出去的格子，缓存会慢慢被它占满。
    // （这是 unique_ptr 的移动赋值也要做的事，同一个道理。）
    release();

    pager_ = other.pager_;
    pageNo_ = other.pageNo_;
    data_ = other.data_;
    frameIndex_ = other.frameIndex_;
    other.pager_ = nullptr;
    other.pageNo_ = INVALID_PAGE;
    other.data_ = nullptr;
    return *this;
}

void Pager::Handle::markDirty() const {
    if (pager_ == nullptr) return;

    Pager& pager = *pager_;

    // ---- ★ 阶段 5 的那道检查 ★ ----
    //
    // 一句话：**改一个"事务开始前就在、而这个事务里还没记进日志"的页，
    // 是代码写错了。**
    //
    // 为什么这一条就是正确性本身，而不是什么代理指标：
    // 回滚靠的是"把日志里的旧内容写回去"。一个页要是改了却没记，
    // 它就永远退不回去了——而这个错误**不会当场表现成任何症状**，
    // 要等到某次回滚之后，才会看到几行莫名其妙的数据。
    //
    // 所以让它在肇事的那一行炸掉。这条消息是给写这个程序的人看的，
    // 不是给用户看的——用户永远不该看到它。
    if (pager.inTransaction_ && pageNo_ < pager.journalPageCount_ &&
        !pager.journal_.has(pageNo_)) {
        throw AcornError(
            "内部错误：" + pageName(pageNo_) +
            "是被改过的，但它没有先经 getForWrite 记进日志。"
            "改一页之前必须用 getForWrite 拿借条，不能用 get。");
    }

    pager.frames_[frameIndex_].dirty = true;
}

// ------------------------------------------------------------
//  缓存
// ------------------------------------------------------------

Pager::Frame& Pager::load(uint32_t pageNo) {
    // ★ 阶段 6：取页之前先确认自己还拿着锁 ★
    //
    // 【为什么这一句放在最热的路径上】因为取页是"我要看/改数据"
    // 的唯一入口——所有读、所有写、分配页都从这儿过。
    // 锁的状态只要和"我能不能看"有关，就该在这儿把关，
    // 而不是散在十几个调用点上（散出去的那种，早晚有一个会漏）。
    //
    // 【它会不会很慢】不会。已经有锁的时候就是一次枚举比较，
    // 和下面那个遍历缓存找页的循环比起来可以忽略。
    ensureSharedLock();

    // ---- 已经在缓存里 ----
    for (Frame& frame : frames_) {
        if (frame.pageNo == pageNo) {
            frame.referenced = true;
            return frame;
        }
    }

    // ---- 不在，得腾个位置 ----
    const size_t victim = pickVictim();
    Frame& frame = frames_[victim];
    char* slot = dataSlot(victim);

    // 被踢出去的这页如果改过，必须先写回磁盘。
    // 【顺序不能反】先读新的再写旧的，就会把新页的内容
    // 写到旧页的位置上——数据直接串门。
    if (frame.pageNo != INVALID_PAGE && frame.dirty) {
        // ★ 阶段 5：库文件动笔之前，日志必须先落盘 ★
        //
        // 淘汰是一个**不在提交时刻**的写盘动作——缓存满了就写，
        // 什么时候都有可能。所以"日志先于库文件"这条不变式
        // 在每一次淘汰上都要重新成立一次，不能只在提交时管一次。
        //
        // 漏了这一句会怎样：日志还在内存缓冲区里，而库文件里
        // 这一页已经改成新的了。这时候掉电——旧的没了，新的也没落，
        // 这一页就永远修不回来了。（同一个进程里崩不掉，因为
        // 操作系统还留着日志的字节；要掉电才现原形。所以这个 bug
        // 在本机测不出来，是那种"上了生产才炸"的错。）
        journal_.syncIfNeeded();
        writeToDisk(frame.pageNo, slot);
    }

    frame.pageNo = pageNo;
    frame.dirty = false;
    frame.pinned = false;
    frame.referenced = true;

    readFromDisk(pageNo, slot);
    return frame;
}

// 时钟算法（clock / second chance）。
//
// 【为什么不用 LRU】
// 严格 LRU 要在**每一次**取页时调整一个链表——而取页是全表扫描里
// 最热的一行代码，为了淘汰策略让最热的路径多花时间，本末倒置。
// 时钟算法用一个"最近碰过"位近似它：碰到被碰过的就给它第二次机会
// （清掉标志、往下走），碰到没碰过的就选它。代价是偶尔会淘汰一个
// 其实还会用到的页，但这个差别在实践中很小。
//
// 【pin 住的不能踢】这是 handle 存在的全部意义：
// 有人正在用这一页，把它换掉别人就写到别的页上去了。
size_t Pager::pickVictim() {
    size_t unpinned = 0;
    for (const Frame& frame : frames_) {
        if (!frame.pinned) unpinned++;
    }
    if (unpinned == 0) {
        // 一个格子都腾不出来。这不是用户的问题，是内部错误——
        // 说明有 Handle 没被释放（漏了析构，或者借条被存起来了）。
        // 缓存大小是 64，正常代码不可能同时按住 64 页。
        throw AcornError(
            "内部错误：页缓存里的 " + std::to_string(frames_.size()) +
            " 个格子全被占住了。多半是某个 Handle 没被释放。");
    }

    // 最多扫两圈。第一圈把"最近碰过"的位清掉，第二圈必然能选中一个
    //（因为至少有一个没被 pin 住，而它的 referenced 位最多撑过一圈）。
    const size_t limit = frames_.size() * 2 + 1;
    for (size_t step = 0; step < limit; step++) {
        Frame& frame = frames_[clockHand_];
        clockHand_ = (clockHand_ + 1) % frames_.size();

        if (frame.pinned) continue;

        if (frame.referenced) {
            frame.referenced = false;  // 给它第二次机会
            continue;
        }
        return (clockHand_ + frames_.size() - 1) % frames_.size();
    }

    throw AcornError("内部错误：时钟算法转了两圈都没找到可以淘汰的页");
}

Pager::Handle Pager::get(uint32_t pageNo) {
    if (pageNo >= pageCount_) {
        throw AcornError("内部错误：要读" + pageName(pageNo) + "，但这个库只有 " +
                         std::to_string(pageCount_) + " 页");
    }

    Frame& frame = load(pageNo);
    frame.pinned = true;
    const size_t index = static_cast<size_t>(&frame - frames_.data());
    return Handle(this, pageNo, dataSlot(index), index);
}

void Pager::ensureTransaction() {
    // ---- ★ 自动提交（autocommit）★ ----
    //
    // 用户没敲 BEGIN 的时候，"一条语句"就该是一个事务：跑完了自动提交，
    // 中途出错自动回滚。这件事不能靠"语句开始时开一个事务、
    // 结束时提交"来做——因为那样 `BEGIN` 自己会被那个事务包住，
    // 变成"在一个事务里再开一个"。
    //
    // 所以事务是**用的时候才开**的：谁第一个要改东西，谁就顺手开一个。
    // BEGIN 不写页，所以它不会触发这里；等它之后的 INSERT 才触发，
    // 而那时事务已经由用户开好了（inTransaction_ 是 true），
    // 这里就不会再开。**一个 bool 就把两种情况都覆盖了。**
    //
    // 顺带的好处：只读的语句（SELECT 是绝大多数）永远开不出事务，
    // 也就永远建不出日志文件。一份日志都不写。
    //
    // 【谁该调它】凡是"接下来要改动文件状态"的地方。两处：
    //   · getForWrite —— 要改一个已经存在的页
    //   · allocatePage 的新页分支 —— 要让文件变长
    // 第二处容易漏，漏了的后果见那边的说明。
    if (!inTransaction_) {
        beginTransaction(/*implicit=*/true);
    }
}

void Pager::ensureSharedLock() {
    if (path_.empty()) return;  // 内存库：没有别的进程，没有锁
    if (lock_.held()) return;

    // 【什么时候会走到这儿】事务结束后退回共享锁时可能失败
    //（那一刻正好有别的进程在写），于是进程短暂地处于"没锁"状态。
    // 再读之前必须重新拿到锁——不然就是在别人改到一半的时候读。
    // 这一条是"宁可报错也不能读到脏数据"。
    std::string why;
    if (!lock_.acquire(path_, LockMode::SHARED, lockWaitMs_, why)) {
        throw BusyError(why);
    }
}

void Pager::ensureExclusiveLock() {
    if (path_.empty()) return;
    if (lock_.exclusive()) return;

    std::string why;

    if (!lock_.held()) {
        if (!lock_.acquire(path_, LockMode::EXCLUSIVE, lockWaitMs_, why)) {
            throw BusyError(why);
        }
        return;
    }

    // 手里是共享锁。★ 这里是这一阶段唯一一处"可能失败"的升级 ★
    //
    // 失败不是 bug，是设计：两个都拿着共享锁的进程如果都想升级，
    // 就是死锁（谁都不肯先放手）。所以规则是"抢不到就报错让用户
    // 重试"，绝不在内核里干等。见 lock.hpp。
    if (!lock_.upgrade(lockWaitMs_, why)) {
        throw BusyError(why);
    }
}

Pager::Handle Pager::getForWrite(uint32_t pageNo) {
    ensureTransaction();

    // 【顺序要紧】必须先把页读进来，再记日志。load() 会把这一页
    // 从磁盘读到缓存（已经在缓存里的话什么都不做），
    // 所以 handle.data() 里此刻装的**一定**是它在文件里的样子。
    // 反过来先记日志就得自己去读一遍磁盘，多一份读盘代码。
    Handle handle = get(pageNo);

    // ---- 什么情况下不用记 ----
    //
    // 1. 没有事务。没有事务就没有回滚这回事，记了也没人用。
    //    （分配一页、建表时初始化文件头都走这条路。）
    // 2. 这一页是**事务开始之后才长出来的**。它原来不存在，
    //    没有"旧内容"可记。回滚时它的处理办法是把文件截短，
    //    而不是把内容写回去——见 rollbackTransaction。
    // 3. 这个事务里已经记过它了。要的是**最原始**那个样子，
    //    重复记会把中间状态当成原始状态，回滚出来就是错的。
    if (inTransaction_ && pageNo < journalPageCount_ &&
        !journal_.has(pageNo)) {
        journal_.record(pageNo, handle.data());
    }

    return handle;
}

// ------------------------------------------------------------
//  事务（阶段 5）
// ------------------------------------------------------------

void Pager::beginTransaction(bool implicit) {
    if (inTransaction_) {
        throw AcornError("内部错误：事务已经开着了，不能嵌套");
    }

    // ★ 阶段 6：要开事务了，先拿到"写的资格" ★
    //
    // 【为什么是这儿，不是 getForWrite】因为这里才是"一次改动的
    // 开始"。显式 BEGIN 和自动提交（语句自己开事务）都从这个
    // 函数进来——**一个口子管住两种**，不会有一边漏掉。
    //
    // 【为什么不能等到真要写第一页的时候再升级】因为一次改动里
    // "读"和"写"是混在一起的：`UPDATE t SET x = 1 WHERE y > 5`
    // 得先扫一遍表。要是等写到第一页才升级，中间那段扫表就是
    // **没有锁保护的读**——别的进程在此期间提交，我们就是拿着
    // 过期数据在做决定。那就是经典的丢失更新。
    //
    // 所以时机是"事务开始"，不是"第一次写"。代价是 BEGIN 之后
    // 立刻就独占（哪怕这个事务只想读），换来的是绝对不会读到
    // 半路被人改过的数据。一个库同时只有一个写者，粗，但对。
    ensureExclusiveLock();

    // 【必须在开事务这一刻记下页数】因为回滚要把文件切回这个长度，
    // 而事务一开始就可能分配新页。晚一步记，切掉的就少了。
    journalPageCount_ = pageCount_;
    journal_.begin(pageCount_);
    inTransaction_ = true;
    transactionImplicit_ = implicit;
}

void Pager::commitTransaction() {
    if (!inTransaction_) {
        throw AcornError("内部错误：没有事务可以提交");
    }

    // ---- 顺序：日志落盘 → 库文件写页并落盘 → 删日志 ----
    //
    // ★ "提交"这个动作，本质上是最后那一步：删掉日志。★
    //
    // 前面两步只是"把改动搬进库文件"。真正让它变成既成事实的是
    // 日志的消失——因为下次打开时的判据就是"日志还在不在"。
    // 所以哪怕所有页都写完了，只要日志还在，下次打开还是会退回去。
    //
    // 这个设计有一个很舒服的推论：**提交只需要保证一件事做对
    //（删日志），而不是保证一整批写操作全部成功。**
    // 中间任何一步崩掉，结果都是"没有提交"——一个干净利落的答案，
    // 不需要去想"崩在第 3 页和第 5 页有什么区别"。
    journal_.syncIfNeeded();

    if (crashPoint_ == CrashPoint::COMMIT) {
        // 测试用：把该写的都写进库文件、也刷到了磁盘，
        // 然后在**删日志之前**死掉。
        //
        // 这是最刁钻的那个崩溃点：库文件里**全都是新数据**，
        // 从内容上看这个事务已经完成了。但恢复之后它会被整个退回去——
        // 因为日志还在，而"日志还在"的意思就是"这个事务没提交"。
        flush();
        std::_Exit(1);
    }

    flush();

    journal_.forget();
    inTransaction_ = false;

    // ---- 写完了，退回共享锁（阶段 6）----
    //
    // 【为什么不在事务一结束就把整把锁放掉】因为下一个语句很可能
    // 还要用这个库，而"放掉再拿"中间的缝就是别人插进来的机会。
    // 退回共享锁的意思是：我暂时不写了，别人可以进来读；
    // 但我自己还留着读的资格。
    lock_.downgrade();
}

void Pager::rollbackTransaction() {
    if (!inTransaction_) {
        throw AcornError("内部错误：没有事务可以回滚");
    }

    // ---- 1. 把日志里的旧内容写回库文件 ----
    //
    // 走 writeToDisk 而不是自己开个文件写：这一层已经知道
    // "库文件是一个文件还是一块内存"了，日志不该再知道一遍。
    journal_.forEachRecord([this](uint32_t pageNo, const char* page) {
        writeToDisk(pageNo, page);
    });

    // ---- 2. 事务期间长出来的页，切掉 ----
    //
    // 【为什么必须有这一步】那些页在日志里一条记录都没有
    //（它们原来不存在），所以第 1 步管不到它们。不切的话它们会
    // 留在文件里，而空闲链上根本没有它们——谁也不认识，
    // 却因为"文件多大就是几页"而看起来是合法的页。
    const long originalSize =
        static_cast<long>(journalPageCount_) * static_cast<long>(PAGE_SIZE);

    if (path_.empty()) {
        // 内存库：把 memory_ 截短就行
        if (memory_.size() > static_cast<size_t>(originalSize)) {
            memory_.resize(static_cast<size_t>(originalSize));
        }
    } else {
        if (std::fflush(file_) != 0) {
            throw AcornError("回滚时把数据交给操作系统失败：" + path_);
        }
        if (std::fseek(file_, 0, SEEK_END) != 0) {
            throw AcornError("回滚时读不出库文件的大小：" + path_);
        }
        if (std::ftell(file_) > originalSize) {
            // 先 fflush 再截断，顺序不能反：缓冲区里那些还没写下去的
            // 字节会在截断之后被写出去，把文件又撑回原来那么长。
#ifdef _WIN32
            const int rc = _chsize(_fileno(file_), originalSize);
#else
            const int rc = ftruncate(fileno(file_), originalSize);
#endif
            if (rc != 0) {
                throw AcornError("回滚时截短库文件失败：" + path_);
            }
        }
        // 退回去的内容必须真正落盘，才能删日志（和提交那里同一个道理）
        syncFile();
    }

    pageCount_ = journalPageCount_;

    // ---- 3. 缓存整个作废 ----
    //
    // 【为什么不能只改受影响的那几页】因为缓存里那些页装的是
    // "刚刚被回滚掉的内容"。留着它们，下一次查询读到的是
    // 已经不存在的行——而且**不报错**。
    // 判断"哪几页受影响"要重走一遍上面的记录，而那个判断错了
    // 就是静悄悄的错数据。整块扔掉是一句 memset 的事，
    // 代价只是下次查询重新读一遍盘。
    invalidateCache();

    // ---- 4. 删日志，事务结束 ----
    journal_.forget();
    inTransaction_ = false;

    // ---- 5. 退回共享锁 ----
    // 和提交那里同一个理由：不写了的进程不该继续占着写的位置。
    // 【注意 crashPoint_ == EXIT 那个分支不在这儿】它在析构里
    // 直接 _Exit 掉了，根本走不到这一步——而锁照样会被放掉，
    // 因为那是内核干的，不是这段代码干的。
    lock_.downgrade();
}

void Pager::invalidateCache() {
    for (Frame& frame : frames_) {
        // pin 着的页不能作废——有人正拿着一张借条。
        // 正常代码走不到这里（回滚发生在语句的边界上，那时候
        // 所有借条都已经还了），所以这是内部错误。
        if (frame.pinned) {
            throw AcornError(
                "内部错误：回滚的时候还有页被借出着（" +
                pageName(frame.pageNo) + "）。借条应该都在语句结束时就还了。");
        }
        frame.pageNo = INVALID_PAGE;
        frame.dirty = false;
        frame.referenced = false;
    }
    clockHand_ = 0;
}

// ------------------------------------------------------------
//  分配 / 释放
// ------------------------------------------------------------

Pager::Handle Pager::allocatePage() {
    // ---- 先看空闲链上有没有 ----
    const uint32_t head = freeListHead();
    if (head != INVALID_PAGE && head < pageCount_) {
        // 【为什么是 getForWrite 不是 get】这一页接下来会被 memset 清空。
        // 它是"事务开始前就存在"的页（空闲链上的页都在文件里），
        // 所以它原来的样子必须进日志——不然后悔药就没得吃了。
        Handle handle = getForWrite(head);

        // 空闲页的第 4 个字节记着"下一个空闲页是谁"——
        // 用的是所有页都有的那个 next 字段（见 page.hpp 的页头布局）。
        // 这就是"所有页留同样 8 字节头"换来的好处：
        // 空闲链不需要额外的结构，链本身就藏在页头里。
        //
        // 【顺序要紧】必须**先读出 next 再清零**。反过来的话，
        // 链就断在这儿了，后面所有空闲页都找不回来（文件永远变大）。
        const uint32_t next = pageNext(handle.data());

        std::memset(handle.data(), 0, PAGE_SIZE);
        handle.markDirty();

        // 这一句里会去取第 0 页，可能触发淘汰——
        // 但 handle 还 pin 着，它那一格不会被换掉。
        setFreeListHead(next);
        return handle;
    }

    // ---- 空闲链空了，把文件加长一页 ----
    //
    // 【注意文件只会变长，不会变短】就算删光了所有表，
    // 文件还是原来那么大。真数据库也一样（SQLite 要显式 VACUUM）。
    // 把文件截短看起来很节俭，但代价是：一旦截了，就没法用
    // "文件大小 = 页数"这个等式了——而那是我们唯一的真相来源。
    //
    // 【这里不用 getForWrite】因为这一页是**新长出来的**，
    // 它原来不存在，没有"旧内容"可记。事务回滚时它的处理办法是
    // 把文件截短（见 rollbackTransaction），不是把内容写回去。
    // 顺带：journalPageCount_ 是事务开始时的页数，而这里
    // pageNo >= pageCount_ >= journalPageCount_，所以下面那句
    // markDirty 的检查也不会拦它。
    //
    // ★ 但事务还是得开 ★
    //
    // "不用记日志"不等于"不用开事务"。这一句必须在 pageCount_++
    // **之前**：开了事务才有 journalPageCount_，而它就是回滚时
    // 要把文件切回的长度。晚一步开，这个新页就被算进"原来就有"
    // 里了，回滚切不掉它——文件里从此留着一页谁也不认识的垃圾。
    // （不会读到坏数据，因为是**多**出来的一页而不是少了一页；
    //   但每回滚一次建表就漏 4KB，而且没人会发现。）
    ensureTransaction();

    const uint32_t pageNo = pageCount_;
    pageCount_++;

    Frame& frame = load(pageNo);
    frame.pinned = true;
    const size_t index = static_cast<size_t>(&frame - frames_.data());

    std::memset(dataSlot(index), 0, PAGE_SIZE);
    Handle handle(this, pageNo, dataSlot(index), index);
    handle.markDirty();
    return handle;
}

void Pager::freePage(uint32_t pageNo) {
    if (pageNo == 0) {
        throw AcornError("内部错误：第 0 页是文件头，不能释放");
    }
    if (pageNo >= pageCount_) {
        throw AcornError("内部错误：要释放的" + pageName(pageNo) +
                         "超出了文件范围");
    }

    Handle handle = getForWrite(pageNo);

    // 把"下一个空闲页"写进这一页的页头，然后把它挂到链首。
    // 【为什么用链首插入】O(1)，而且不需要知道链尾在哪。
    // 代价是空闲页被复用的顺序是**反的**（最后释放的最先被用），
    // 这对正确性没有任何影响。
    const uint32_t head = freeListHead();
    std::memset(handle.data(), 0, PAGE_SIZE);
    setPageType(handle.data(), PageType::FREE);
    setPageNext(handle.data(), head);
    handle.markDirty();
    setFreeListHead(pageNo);
}

uint32_t Pager::freeListHead() {
    // 空文件：连第 0 页都还没有，当然没有空闲链。
    if (pageCount_ == 0) return INVALID_PAGE;

    Handle header = get(0);
    // 【这里必须验一遍文件头】因为第 0 页可能是刚分配出来、
    // 还没被 initFileHeader 写过的全 0 页。全 0 的话
    // fileFreeListHead 会返回 0——而 0 是第 0 页自己的页号，
    // 于是"分配一页"会分到文件头上，然后把文件头当数据页写掉。
    // 这种错会一路传到很远的地方才发作，所以在这里拦死。
    std::string why;
    if (!checkFileHeader(header.data(), why)) return INVALID_PAGE;
    return fileFreeListHead(header.data());
}

void Pager::setFreeListHead(uint32_t pageNo) {
    if (pageCount_ == 0) {
        throw AcornError("内部错误：文件头都还没有就改空闲链");
    }
    // 第 0 页是文件头，改它当然要记日志——空闲链的头就在里面，
    // 回滚的时候它得跟着一起退回去，不然那些页就永远找不回来了。
    Handle header = getForWrite(0);
    setFileFreeListHead(header.data(), pageNo);
    header.markDirty();
}

// ------------------------------------------------------------
//  落盘
// ------------------------------------------------------------

void Pager::flush() {
    for (size_t i = 0; i < frames_.size(); i++) {
        Frame& frame = frames_[i];
        if (frame.pageNo == INVALID_PAGE || !frame.dirty) continue;
        writeToDisk(frame.pageNo, dataSlot(i));
        frame.dirty = false;
    }

    // 【阶段 5 起这里多了一步 fsync】因为"写回磁盘"这句话的字面意思
    // 就是"磁盘上真的有"。以前只有 fflush（交给操作系统），
    // 严格说叫"写回操作系统的缓存"。提交要靠这一句才算数。
    syncFile();
}

void Pager::syncFile() {
    if (file_ == nullptr) return;  // 内存库没有磁盘可刷
    syncFileToDisk(file_);
}

void Pager::readFromDisk(uint32_t pageNo, char* out) {
    if (path_.empty()) {
        // 内存库：从 memory_ 里拷。越界说明这一页还没被写过，
        // 那就是全 0——保持"新页就是一片空白"的约定。
        const size_t offset = static_cast<size_t>(pageNo) * PAGE_SIZE;
        if (offset + PAGE_SIZE <= memory_.size()) {
            std::memcpy(out, memory_.data() + offset, PAGE_SIZE);
        } else {
            std::memset(out, 0, PAGE_SIZE);
        }
        return;
    }

    // fseek 一个文件位置，然后读。用 std::FILE* 而不是 fstream
    // 是为了能 fsync（见 syncFileToDisk）——读写本身两者没差别。
    if (std::fseek(file_, static_cast<long>(pageNo) * static_cast<long>(PAGE_SIZE),
                   SEEK_SET) != 0) {
        throw AcornError("往" + pageName(pageNo) + "定位失败：" + path_);
    }

    const size_t got = std::fread(out, 1, PAGE_SIZE, file_);
    if (got < PAGE_SIZE) {
        // 读到文件尾了。这不是错误——刚分配出来的新页本来就还没有
        // 对应的字节。把剩下的部分填 0，保证"没写过的字节是 0"
        // 这条约定在任何路径上都成立。
        // （不填的话，缓存里会留着上一页的残留字节，
        //   于是"新页"里会冒出别人写过的数据。这个 bug 极难查。）
        std::memset(out + got, 0, PAGE_SIZE - got);
    }
}

void Pager::writeToDisk(uint32_t pageNo, const char* data) {
    if (path_.empty()) {
        const size_t offset = static_cast<size_t>(pageNo) * PAGE_SIZE;
        if (memory_.size() < offset + PAGE_SIZE) {
            memory_.resize(offset + PAGE_SIZE, 0);
        }
        std::memcpy(memory_.data() + offset, data, PAGE_SIZE);
        return;
    }

    if (std::fseek(file_, static_cast<long>(pageNo) * static_cast<long>(PAGE_SIZE),
                   SEEK_SET) != 0) {
        throw AcornError("往" + pageName(pageNo) + "定位失败：" + path_);
    }
    if (std::fwrite(data, 1, PAGE_SIZE, file_) != PAGE_SIZE) {
        throw AcornError("往" + pageName(pageNo) + "写数据失败：" + path_ +
                         "（磁盘满了？）");
    }
}

// ------------------------------------------------------------
//  调试
// ------------------------------------------------------------

std::string Pager::cacheStats() const {
    size_t used = 0;
    size_t dirty = 0;
    size_t pinned = 0;
    for (const Frame& frame : frames_) {
        if (frame.pageNo != INVALID_PAGE) used++;
        if (frame.dirty) dirty++;
        if (frame.pinned) pinned++;
    }
    std::string text =
        "缓存 " + std::to_string(used) + "/" + std::to_string(frames_.size()) +
        " 格在用，其中 " + std::to_string(dirty) + " 页待写回、" +
        std::to_string(pinned) + " 页被借出；文件共 " +
        std::to_string(pageCount_) + " 页";

    // 事务状态也报一下。"为什么我插的数据查不到"这类问题里，
    // "你现在在一个没提交的事务里"是个很常见的答案。
    if (inTransaction_) {
        text += "；事务开着（已记 " +
                std::to_string(journal_.recordCount()) + " 页的旧内容）";
    }
    return text;
}
