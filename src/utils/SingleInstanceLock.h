#pragma once

#include <QString>

#include <memory>

QT_BEGIN_NAMESPACE
class QLockFile;
QT_END_NAMESPACE

namespace njulms {

/**
 * 单实例保护（v2 新增）。
 *
 * 背景：同一份运行数据被两个实例同时使用会互相破坏——
 *   - Chromium 对同一个 QWebEngineProfile 存储路径有全局唯一性检查，
 *     同一进程里第二次创建会直接 FATAL 崩溃，多进程争抢会拿不到 Profile 锁；
 *   - 同一个 SQLite 文件被两个写入者同时使用会放大锁竞争。
 *
 * 因此程序启动时必须先取得独占锁（默认位于数据目录的 njulms.lock）。
 *
 * 实现要点：
 *   - 跨进程：QLockFile 依据「PID + 主机名」判断已有锁是否仍然有效，
 *     进程崩溃留下的锁文件会被自动回收（不会永久卡死启动）；
 *   - 同进程：QLockFile 会把「PID 等于自己」的锁文件当作陈旧文件回收，
 *     所以额外维护一个进程内守卫，保证同一进程里第二次 tryAcquire()
 *     一定失败。
 *
 * 线程约定：只能在创建它的线程（主线程）里使用。
 */
class SingleInstanceLock
{
public:
    SingleInstanceLock();
    ~SingleInstanceLock();

    SingleInstanceLock(const SingleInstanceLock &) = delete;
    SingleInstanceLock &operator=(const SingleInstanceLock &) = delete;

    /// 尝试取得锁。失败时通过 errorOut 返回可读原因（不含敏感信息）。
    bool tryAcquire(const QString &lockFilePath, QString *errorOut = nullptr);

    bool isHeld() const;

    /// 释放锁；重复调用安全。
    void release();

private:
    std::unique_ptr<QLockFile> m_lockFile;
};

} // namespace njulms
