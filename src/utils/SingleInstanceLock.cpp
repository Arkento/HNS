#include "utils/SingleInstanceLock.h"

#include <QDir>
#include <QFileInfo>
#include <QLockFile>

#include "utils/Logger.h"

namespace njulms {
namespace {

/// 进程内守卫：QLockFile 只认「锁文件里的 PID 是不是还活着」，
/// 同一个进程的第二次 tryAcquire() 会把第一次的锁文件当陈旧文件回收掉，
/// 所以单实例语义必须由这个标志兜底。
bool &processGuard()
{
    static bool held = false;
    return held;
}

} // namespace

SingleInstanceLock::SingleInstanceLock() = default;

SingleInstanceLock::~SingleInstanceLock()
{
    release();
}

bool SingleInstanceLock::tryAcquire(const QString &lockFilePath, QString *errorOut)
{
    const auto fail = [errorOut](const QString &reason) {
        if (errorOut) {
            *errorOut = reason;
        }
        return false;
    };

    if (isHeld()) {
        return fail(QStringLiteral("本对象已持有锁"));
    }
    if (processGuard()) {
        return fail(QStringLiteral("同一进程内已有一个实例持锁"));
    }

    const QString path = lockFilePath.trimmed();
    if (path.isEmpty()) {
        return fail(QStringLiteral("锁文件路径为空"));
    }

    const QFileInfo info(path);
    const QDir dir(info.absolutePath());
    if (!dir.exists() && !dir.mkpath(QStringLiteral("."))) {
        return fail(QStringLiteral("无法创建锁文件目录: %1").arg(dir.absolutePath()));
    }

    auto lock = std::make_unique<QLockFile>(info.absoluteFilePath());

    // tryLock(0)：只尝试一次，不等待。失败时按 QLockFile 的错误码给出可读原因。
    if (!lock->tryLock(0)) {
        switch (lock->error()) {
        case QLockFile::PermissionError:
            return fail(QStringLiteral("没有权限创建锁文件: %1").arg(info.absoluteFilePath()));
        case QLockFile::UnknownError:
            return fail(QStringLiteral("创建锁文件失败（未知错误）: %1").arg(info.absoluteFilePath()));
        case QLockFile::LockFailedError:
        case QLockFile::NoError:
        default:
            return fail(QStringLiteral("另一个实例正在运行"));
        }
    }

    m_lockFile = std::move(lock);
    processGuard() = true;
    qCInfo(lcApp) << "single-instance lock acquired:" << info.absoluteFilePath();
    return true;
}

bool SingleInstanceLock::isHeld() const
{
    return m_lockFile != nullptr;
}

void SingleInstanceLock::release()
{
    if (!m_lockFile) {
        return;
    }
    m_lockFile->unlock();
    m_lockFile.reset();
    processGuard() = false;
}

} // namespace njulms
