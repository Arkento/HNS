#include "utils/SingleInstanceLock.h"

#include <QDir>
#include <QTemporaryDir>
#include <QtTest>

using namespace njulms;

/**
 * 单实例锁回归测试（v2 新增）。
 *
 * 覆盖：
 *   - 第一次获取成功、同一进程内第二次尝试必定失败；
 *   - 释放之后可以把锁重新拿回来；
 *   - 锁文件所在目录不存在时自动创建；
 *   - 空路径被拒绝；
 *   - 重复 release() 安全。
 */
class SingleInstanceLockTests : public QObject
{
    Q_OBJECT

private slots:
    void acquireThenSecondAttemptFails();
    void releaseAllowsReacquire();
    void createsMissingDirectory();
    void emptyPathIsRejected();
    void releaseIsIdempotent();
};

void SingleInstanceLockTests::acquireThenSecondAttemptFails()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("njulms.lock"));

    SingleInstanceLock first;
    QString error;
    QVERIFY2(first.tryAcquire(path, &error), qPrintable(error));
    QVERIFY(first.isHeld());
    QVERIFY(QFile::exists(path));

    SingleInstanceLock second;
    QString secondError;
    QVERIFY(!second.tryAcquire(path, &secondError));
    QVERIFY(!second.isHeld());
    QVERIFY(!secondError.isEmpty());

    first.release();
    QVERIFY(!first.isHeld());
    QVERIFY(!QFile::exists(path));
}

void SingleInstanceLockTests::releaseAllowsReacquire()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath(QStringLiteral("njulms.lock"));

    SingleInstanceLock lock;
    QString error;
    QVERIFY2(lock.tryAcquire(path, &error), qPrintable(error));
    lock.release();
    QVERIFY(!lock.isHeld());

    QString reacquireError;
    QVERIFY2(lock.tryAcquire(path, &reacquireError), qPrintable(reacquireError));
    QVERIFY(lock.isHeld());
}

void SingleInstanceLockTests::createsMissingDirectory()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = QDir(dir.path()).filePath(QStringLiteral("nested/data/njulms.lock"));
    QVERIFY(!QDir(dir.path() + QStringLiteral("/nested")).exists());

    SingleInstanceLock lock;
    QString error;
    QVERIFY2(lock.tryAcquire(path, &error), qPrintable(error));
    QVERIFY(QFile::exists(path));
}

void SingleInstanceLockTests::emptyPathIsRejected()
{
    SingleInstanceLock lock;
    QString error;
    QVERIFY(!lock.tryAcquire(QString(), &error));
    QVERIFY(!error.isEmpty());
    QVERIFY(!lock.isHeld());
}

void SingleInstanceLockTests::releaseIsIdempotent()
{
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    SingleInstanceLock lock;
    QString error;
    QVERIFY2(lock.tryAcquire(dir.filePath(QStringLiteral("a.lock")), &error), qPrintable(error));
    lock.release();
    lock.release();
    QVERIFY(!lock.isHeld());
}

QTEST_MAIN(SingleInstanceLockTests)
#include "SingleInstanceLockTests.moc"
