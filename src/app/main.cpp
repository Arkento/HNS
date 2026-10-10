#include "repository/SqliteRepository.h"
#include "ui/MainWindow.h"
#include "ui/UiTheme.h"
#include "utils/ChromiumLogging.h"
#include "utils/Logger.h"
#include "utils/SingleInstanceLock.h"

#include <QApplication>
#include <QDir>
#include <QMessageBox>

#include <cstdlib>

using namespace njulms;

/**
 * 程序入口。
 *
 * 启动顺序（顺序有讲究）：
 *   1. 创建 QApplication（Qt WebEngine 要求在创建任何 QWebEngine 对象之前
 *      完成 QApplication 构造；Qt 6 会自动处理 Chromium 的初始化）。
 *   2. 安装日志系统——必须早于任何模块，否则早期日志会丢失。
 *   3. 取得单实例锁（v2 新增）——多实例会争抢同一个浏览器 Profile
 *      与同一个 SQLite 数据库。
 *   4. 创建主窗口。
 */
int main(int argc, char *argv[])
{
    // 必须在任何 Qt WebEngine 对象创建之前设置：
    // 站点 SPA 的 console.log 会以 INFO:CONSOLE 打到 stderr，Debug 版控制台会刷屏。
    quietChromiumInfoLogs();

    QApplication app(argc, argv);

    QCoreApplication::setOrganizationName(QStringLiteral("NjuLmsCrawler"));
    QCoreApplication::setApplicationName(QStringLiteral("NjuLmsCrawler"));
    QCoreApplication::setApplicationVersion(QStringLiteral("0.2.0"));

    // 界面主题：跟随系统深浅色，统一令牌（必须在创建窗口之前）
    uitheme::apply(&app);

    // 日志：写入运行时创建的 logs/ 目录，不硬编码在源码目录
    Logger::install();

    // 单实例保护：第二个实例直接提示并退出，避免破坏 Profile / 数据库
    SingleInstanceLock instanceLock;
    QString lockError;
    const QString lockPath =
        QDir(SqliteRepository::defaultDataDir()).filePath(QStringLiteral("njulms.lock"));
    if (!instanceLock.tryAcquire(lockPath, &lockError)) {
        qCWarning(lcApp) << "another instance already owns the runtime data:" << lockError;
        QMessageBox::warning(
            nullptr,
            QStringLiteral("NjuLmsCrawler 已在运行"),
            QStringLiteral("检测到另一个实例正在使用同一份运行数据。\n\n"
                           "同一时间只允许运行一个实例：\n"
                           "  - 多个实例会争抢同一个浏览器 Profile（Chromium 会拒绝）\n"
                           "  - 也会争抢同一个 SQLite 数据库\n\n"
                           "请先关闭已运行的窗口后重试。\n\n原因：%1")
                .arg(lockError));
        Logger::uninstall();
        return 1;
    }

    qCInfo(lcApp) << "starting NjuLmsCrawler" << QCoreApplication::applicationVersion();
    qCInfo(lcApp) << "data directory:" << SqliteRepository::defaultDataDir();

    MainWindow window;
    window.show();

    const int code = app.exec();

    qCInfo(lcApp) << "exiting with code" << code;
    Logger::uninstall();
    return code;
}
