#include "lock.hpp"

#include <chrono>
#include <cstring>
#include <string>

// 这一层用的路径工具（UTF-8 -> UTF-16）和库文件那边是同一份，
// 见 utf8path.hpp。锁这一层从一开始就走在宽字符 API 上，
// 反而正是它把"库文件那边走的是窄字符"这件事暴露出来的：
// 库文件建不出来的时候，锁文件 测试库.db-lock 却好好地建了出来。
#include "utf8path.hpp"

// 这个文件是 Acorn 里第一处直接跟操作系统打交道的地方
//（前面五个阶段都只用了 C 标准库）。所以下面每一个平台分支
// 都写得比别处啰嗦一点：读的人多半没见过这几个 API。
#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

std::string lockPathFor(const std::string& dbPath) {
    return dbPath + "-lock";
}

void sleepMilliseconds(int milliseconds) {
    if (milliseconds <= 0) return;
#ifdef _WIN32
    Sleep(static_cast<DWORD>(milliseconds));
#else
    usleep(static_cast<useconds_t>(milliseconds) * 1000);
#endif
}

namespace {

// 从某个不确定的起点开始数的毫秒数。
//
// 【只用来算差值】它的起点是什么时候无所谓，因为我们只会拿
// 两个它相减。这一点很重要：GetTickCount 那种"从开机算起"的
// 计数器会在大约 49 天之后回绕，而这个回绕正好是"算差值"
// 这件事最容易踩的坑（两个数一减得到负数，循环就永远不退出）。
// std::chrono 的 steady_clock 保证不会回绕。
long long nowMilliseconds() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch())
        .count();
}

// "这个库现在被别人占着，所以这次不行"——给用户看的那句话。
//
// 【为什么要分成两种说法】因为责任不同、下一步也不同：
// 拿不到共享锁是"有人正在写，你连读都读不了"，
// 拿不到独占锁是"有人开着这个库，你写不了"。
// 一句笼统的"库被占用了"会让人以为是自己的命令写错了。
std::string busyMessage(const std::string& dbPath, LockMode mode, int waitMs) {
    std::string text;
    if (mode == LockMode::EXCLUSIVE) {
        text = "库 " + dbPath +
               " 现在有别的进程开着，写不了它——要改数据就得等那个进程关掉。";
    } else {
        text = "库 " + dbPath +
               " 现在被另一个进程独占着（它正在写），连读都不行。";
    }

    if (waitMs > 0) {
        text += "（等了 " + std::to_string(waitMs) + " 毫秒也没等到）";
    } else {
        text += "（想等一会儿的话，加 --lock-wait=毫秒）";
    }
    return text;
}

#ifdef _WIN32

// 试着锁一下（或者解锁）。
//
// 【为什么只锁 1 个字节】因为要挡的是"整个库"，而不是某一页。
// 文件锁是按**字节范围**给的，锁满整个文件得知道文件多大，
// 而"多大"以后还会变。锁第 0 个字节就够了——所有进程约定的
// 是同一个字节，谁先拿到谁赢。锁文件是空的也不要紧：
// 字节范围锁不要求那个字节真的存在。
//
// 【LOCKFILE_FAIL_IMMEDIATELY 是必须的】不加它，这个调用会
// **阻塞**到别人放锁为止——那就没有超时这回事了，
// 程序会静静地卡在那儿，什么也不打印。
bool tryLockHandle(void* handle, bool exclusive) {
    OVERLAPPED overlapped;
    std::memset(&overlapped, 0, sizeof(overlapped));

    DWORD flags = LOCKFILE_FAIL_IMMEDIATELY;
    if (exclusive) flags |= LOCKFILE_EXCLUSIVE_LOCK;

    return LockFileEx(static_cast<HANDLE>(handle), flags, 0, /*低 32 位*/ 1,
                      /*高 32 位*/ 0, &overlapped) != 0;
}

void unlockHandle(void* handle) {
    OVERLAPPED overlapped;
    std::memset(&overlapped, 0, sizeof(overlapped));
    UnlockFileEx(static_cast<HANDLE>(handle), 0, 1, 0, &overlapped);
}

void closeHandle(void* handle) {
    if (handle != nullptr) CloseHandle(static_cast<HANDLE>(handle));
}

#else  // 以下是 POSIX 分支 —— 本机是 Windows，这一支**没有实测过**

bool tryLockHandle(int fd, bool exclusive) {
    struct flock lock;
    std::memset(&lock, 0, sizeof(lock));
    lock.l_type = exclusive ? F_WRLCK : F_RDLCK;
    lock.l_whence = SEEK_SET;
    lock.l_start = 0;
    lock.l_len = 1;  // 只管第 0 个字节，理由同上

    // F_SETLK 是"试一下，不行就回来"，F_SETLKW 才是"不拿到不罢休"。
    // 我们自己在外面写重试循环，所以用前者——这样才能有超时。
    return fcntl(fd, F_SETLK, &lock) == 0;
}

void unlockHandle(int fd) {
    struct flock lock;
    std::memset(&lock, 0, sizeof(lock));
    lock.l_type = F_UNLCK;
    lock.l_whence = SEEK_SET;
    lock.l_start = 0;
    lock.l_len = 1;
    fcntl(fd, F_SETLK, &lock);
}

void closeHandle(int fd) {
    if (fd >= 0) close(fd);
}

#endif

}  // namespace

FileLock::~FileLock() {
    // 【析构里其实什么都不做也可以】因为进程消失时内核会把所有
    // 锁一起收走，进程正常退出也一样（句柄一关锁就没了）。
    // 留着这一句是为了"用完就还"这件事在代码里有个明确的落点，
    // 而不是靠"反正内核会管"。
    release();
}

bool FileLock::acquire(const std::string& dbPath, LockMode mode, int waitMs,
                       std::string& why) {
    release();

    // 内存库：别的进程根本看不见它，没有"同时用"这回事。
    if (dbPath.empty() || mode == LockMode::NONE) {
        dbPath_.clear();
        return true;
    }

    const std::string lockPath = lockPathFor(dbPath);

#ifdef _WIN32
    // ★ 打开锁文件本身 ★
    //
    // 【FILE_SHARE_READ | FILE_SHARE_WRITE 一个都不能少】
    // 它们说的是"允许别人也以这种方式打开这个文件"。
    // 不给共享的话，第二个进程连**打开**都做不到，
    // 拿到的是一个系统级的"文件被占用"错误——它连试锁的机会
    // 都没有，也就说不出我们想说的那句话。
    //
    // 【OPEN_ALWAYS 而不是 CREATE_ALWAYS】已经有的锁文件要接着用，
    // 没有才建。CREATE_ALWAYS 会把别人正在用的那个文件截掉。
    //
    // 【为什么不删这个文件】它是空的，留着不占地方，而且
    // 删了也没用——锁在内核里，不在文件里。留着反而是个线索：
    // 看见 xxx-lock 就知道 acorn 开过这个库。
    HANDLE handle = CreateFileW(
        utf8ToWide(lockPath).c_str(), GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);

    if (handle == INVALID_HANDLE_VALUE) {
        why = "建不了锁文件：" + lockPath + "（目录不存在？没有写权限？）";
        return false;
    }
    handle_ = handle;
#else
    // 【为什么开成 O_RDWR 而不是 O_RDONLY】因为写锁（F_WRLCK）要求
    // 文件是以可写方式打开的。一个进程可能先拿共享、后来要升级，
    // 每次都重开一遍句柄的话，"关掉-重开-上锁"中间就有一个没锁的缝。
    const int fd = open(lockPath.c_str(), O_RDWR | O_CREAT, 0644);
    if (fd < 0) {
        why = "建不了锁文件：" + lockPath + "（目录不存在？没有写权限？）";
        return false;
    }
    handle_ = fd;
#endif

    dbPath_ = dbPath;

    // ---- 试，试不到就等一会儿再试 ----
    //
    // 【为什么是"睡 10 毫秒再试"而不是让内核帮我等】因为要让
    // "等多久"这件事归我们管：内核的阻塞版本没有超时，
    // 一旦别人不放锁，我们就永远醒不过来。
    const bool exclusive = (mode == LockMode::EXCLUSIVE);
    const long long started = nowMilliseconds();

    for (;;) {
        if (tryLockHandle(handle_, exclusive)) {
            mode_ = mode;
            return true;
        }

        if (waitMs <= 0) break;
        if (nowMilliseconds() - started >= waitMs) break;

        // 睡一小会儿再试。睡太久会让"刚好在这一刻放锁"的情况
        // 白等一整个间隔；睡太短就是空转烧 CPU。10 毫秒是个
        // 两头都能接受的值（SQLite 在忙等里用的也是这个量级）。
        sleepMilliseconds(10);
    }

    // 没拿到。句柄和路径都放掉，回到"什么都没有"的状态。
    closeHandle(handle_);
    handle_ = NO_HANDLE;
    dbPath_.clear();
    mode_ = LockMode::NONE;

    why = busyMessage(dbPath, mode, waitMs);
    return false;
}

bool FileLock::upgrade(int waitMs, std::string& why) {
    if (mode_ == LockMode::EXCLUSIVE) return true;
    if (mode_ != LockMode::SHARED) {
        // 手里什么都没有。这不是"升级"，是"去拿"——调用方该用 acquire。
        // 走到这儿说明调用方的逻辑错了，但不该在这儿崩：
        // 返回 false 之后调用方会报一句给用户看的话。
        return false;
    }

    // ★ 先把共享锁放掉 ★
    //
    // 【同一个句柄上没法同时拿着两种锁】而且"共享 + 独占"本来就
    // 等于独占，所以正确的动作就是放开再抢。
    //
    // 【这一步意味着什么】意味着有那么一小段时间，这个进程
    // 手里没有锁。别的进程可以趁这个机会拿到独占——然后我们
    // 就抢不到了，只好报错。这是**故意的**：宁可偶尔失败，
    // 也不做"两个读者互相等对方放开共享锁"的死锁。
    // 那一等就是永远，因为它们谁都不会先放手。
    unlockHandle(handle_);
    mode_ = LockMode::NONE;

    const long long started = nowMilliseconds();
    for (;;) {
        if (tryLockHandle(handle_, /*exclusive=*/true)) {
            mode_ = LockMode::EXCLUSIVE;
            return true;
        }
        if (waitMs <= 0) break;
        if (nowMilliseconds() - started >= waitMs) break;
        sleepMilliseconds(10);
    }

    why = busyMessage(dbPath_, LockMode::EXCLUSIVE, waitMs);

    // ---- 抢不到，尽量把共享锁拿回来 ----
    //
    // 【为什么值得试这一下】因为一次写失败不该让整个进程变成残废。
    // 用户在 REPL 里敲错一条语句、撞上别的进程在写，接下来他应该
    // 还能 SELECT。拿不回共享锁（那一刻正好有人在写）也没关系，
    // 状态是 NONE，下次取页会重新拿（Pager::ensureSharedLock）。
    if (tryLockHandle(handle_, /*exclusive=*/false)) {
        mode_ = LockMode::SHARED;
    }
    return false;
}

void FileLock::downgrade() {
    if (mode_ != LockMode::EXCLUSIVE) return;

    unlockHandle(handle_);
    mode_ = LockMode::NONE;

    // 【拿回共享锁是尽力而为】失败说明此刻有别的进程正在写——
    // 那个情况下我们本来也不该读。退到 NONE，下一次要取页的时候
    // Pager::ensureSharedLock 会再拿一次，拿不到就报错。
    // 关键在于**不会出现"以为有锁其实没有"**：没有锁就是没有锁。
    if (tryLockHandle(handle_, /*exclusive=*/false)) {
        mode_ = LockMode::SHARED;
    }
}

void FileLock::release() {
    if (handle_ == NO_HANDLE) {
        mode_ = LockMode::NONE;
        dbPath_.clear();
        return;
    }

    if (mode_ != LockMode::NONE) {
        unlockHandle(handle_);
    }
    closeHandle(handle_);
    handle_ = NO_HANDLE;
    mode_ = LockMode::NONE;
    dbPath_.clear();
}
