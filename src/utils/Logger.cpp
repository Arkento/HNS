#include "Logger.h"

#include "Redact.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMutex>
#include <QMutexLocker>
#include <QStandardPaths>
#include <QTextStream>
#include <QThread>

#include <cstdio>

namespace njulms {

// 日志分类的定义必须与 Logger.h 中的 Q_DECLARE_LOGGING_CATEGORY 处于同一命名空间，
// 否则会生成 njulms::lcXxx 的声明与全局 ::lcXxx 的定义，链接时报未解析外部符号。
Q_LOGGING_CATEGORY(lcApp, "njulms.app")
Q_LOGGING_CATEGORY(lcBrowser, "njulms.browser")
Q_LOGGING_CATEGORY(lcAuth, "njulms.auth")
Q_LOGGING_CATEGORY(lcAdapter, "njulms.adapter")
Q_LOGGING_CATEGORY(lcParser, "njulms.parser")
Q_LOGGING_CATEGORY(lcRepo, "njulms.repo")
Q_LOGGING_CATEGORY(lcSync, "njulms.sync")
Q_LOGGING_CATEGORY(lcScheduler, "njulms.scheduler")

namespace {

Logger *g_instance = nullptr;
QtMessageHandler g_previousHandler = nullptr;

const char *levelName(Logger::Level level)
{
    switch (level) {
    case Logger::Level::Debug:    return "DEBUG";
    case Logger::Level::Info:     return "INFO ";
    case Logger::Level::Warning:  return "WARN ";
    case Logger::Level::Critical: return "ERROR";
    }
    return "?????";
}

Logger::Level fromQtType(QtMsgType type)
{
    switch (type) {
    case QtDebugMsg:    return Logger::Level::Debug;
    case QtInfoMsg:     return Logger::Level::Info;
    case QtWarningMsg:  return Logger::Level::Warning;
    case QtCriticalMsg: return Logger::Level::Critical;
    case QtFatalMsg:    return Logger::Level::Critical;
    }
    return Logger::Level::Info;
}

/// 简单轮转：当前文件超过 maxBytes 时，把 foo.log 依次改名为 foo.log.1 ... foo.log.N
void rotateIfNeeded(const QString &path, qint64 maxBytes, int maxFiles)
{
    QFileInfo info(path);
    if (!info.exists() || info.size() < maxBytes) {
        return;
    }
    for (int i = maxFiles - 1; i >= 1; --i) {
        const QString from = QStringLiteral("%1.%2").arg(path).arg(i);
        const QString to = QStringLiteral("%1.%2").arg(path).arg(i + 1);
        if (QFile::exists(from)) {
            QFile::remove(to);
            QFile::rename(from, to);
        }
    }
    const QString first = path + QStringLiteral(".1");
    QFile::remove(first);
    QFile::rename(path, first);
}

} // namespace

// 注意：Logger 的成员函数不能定义在匿名命名空间内，否则 MSVC 报
// C2888（符号不能在匿名命名空间中定义）。因此所有 Logger:: 成员函数
// 都放在匿名命名空间之外。

void Logger::handleMessage(QtMsgType type, const QMessageLogContext &context, const QString &msg)
{
    Logger *logger = Logger::instance();
    if (logger == nullptr) {
        if (g_previousHandler != nullptr) {
            g_previousHandler(type, context, msg);
        }
        return;
    }

    const Level level = fromQtType(type);

    // 站点/Chromium 自身打印的 JS 控制台消息（context.category == "js"）一律丢弃：
    //   1. 它们是第三方内容，不是本程序的日志，噪音极大；
    //   2. 实测会把**完整的统一身份认证跳转 URL** 打印出来（含 service=
    //      参数里层层 URL 编码的回跳地址）。redact::text() 的关键字规则
    //      只覆盖 "service=" 形式，对 URL 编码后的 "service%3D" 无能为力，
    //      于是认证 URL 会原样落盘，违反需求第五节的隐私要求。
    // 直接不记录第三方控制台输出，比试图穷举各种编码形式更可靠。
    const QLatin1StringView category(context.category != nullptr ? context.category : "default");
    if (category == QLatin1StringView("js")) {
        return;
    }

    // 第二道防线：无论调用方是否自觉，都在写入前统一脱敏。
    const QString safe = redact::text(msg);

    const QString line = QStringLiteral("%1 [%2] %3: %4")
                             .arg(QDateTime::currentDateTime().toString(Qt::ISODateWithMs),
                                  QString::fromLatin1(levelName(level)),
                                  QString::fromLatin1(category.data(), category.size()), safe);

    logger->writeLine(level, line);

    if (type == QtFatalMsg) {
        abort();
    }
}

// ---------------------------------------------------------------------------
// Impl
// ---------------------------------------------------------------------------

class Logger::Impl
{
public:
    Impl(QString dir, qint64 maxBytes, int maxFiles)
        : logDir(std::move(dir)), maxBytesPerFile(maxBytes), maxFiles(maxFiles)
    {
    }

    QString logDir;
    qint64 maxBytesPerFile;
    int maxFiles;
    QString logFilePath;
    QMutex mutex;
};

// Logger 需要访问 Impl::writeLine，这里以自由函数形式实现，声明在 Logger 内部是 friend。
// 为避免在头文件暴露细节，writeLine 作为 Logger 的私有成员由 messageHandler 调用，
// 因此这里把 messageHandler 声明为友元由 Logger::install 注册。

Logger::Logger(QString logDir, qint64 maxBytesPerFile, int maxFiles)
    : d(std::make_unique<Impl>(std::move(logDir), maxBytesPerFile, maxFiles))
{
    QDir().mkpath(d->logDir);
    d->logFilePath = QDir(d->logDir).filePath(QStringLiteral("njulms.log"));
}

Logger::~Logger() = default;

Logger *Logger::install(const QString &logDir, qint64 maxBytesPerFile, int maxFiles)
{
    if (g_instance != nullptr) {
        return g_instance;
    }
    const QString dir = logDir.isEmpty() ? defaultLogDir() : logDir;
    g_instance = new Logger(dir, maxBytesPerFile, maxFiles);
    g_previousHandler = qInstallMessageHandler(&Logger::handleMessage);
    return g_instance;
}

void Logger::uninstall()
{
    if (g_instance == nullptr) {
        return;
    }
    qInstallMessageHandler(g_previousHandler);
    g_previousHandler = nullptr;
    delete g_instance;
    g_instance = nullptr;
}

Logger *Logger::instance()
{
    return g_instance;
}

QString Logger::logFilePath() const
{
    return d->logFilePath;
}

QString Logger::defaultLogDir()
{
    // 1) 环境变量覆盖，便于测试与部署
    const QByteArray env = qgetenv("NJULMS_LOG_DIR");
    if (!env.isEmpty()) {
        return QString::fromLocal8Bit(env);
    }

    // 2) 可执行文件同级的 logs/（开发期直观；发布时随安装目录）
    if (QCoreApplication::instance() != nullptr) {
        const QString appDir = QCoreApplication::applicationDirPath();
        if (!appDir.isEmpty()) {
            return QDir(appDir).filePath(QStringLiteral("logs"));
        }
    }

    // 3) 系统本地数据目录
    const QString base = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    return QDir(base).filePath(QStringLiteral("logs"));
}

void Logger::writeLine(Level level, const QString &line)
{
    QMutexLocker locker(&d->mutex);

    rotateIfNeeded(d->logFilePath, d->maxBytesPerFile, d->maxFiles);

    QFile file(d->logFilePath);
    if (file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        QTextStream out(&file);
        out.setEncoding(QStringConverter::Utf8);
        out << line << '\n';
        out.flush();
    }

    // 控制台输出（调试期有用；发布时可关闭）
    std::fputs(qPrintable(line + QLatin1Char('\n')), stderr);

    emit messageLogged(level, line);
}

} // namespace njulms
