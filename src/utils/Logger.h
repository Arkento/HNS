#pragma once

#include <QLoggingCategory>
#include <QObject>
#include <QString>

#include <memory>

namespace njulms {

/// 日志分类。按需求第五节，可以记录任务 ID、批次、成功/失败数量、错误类型、时间范围、
/// 断点是否更新等运行信息；敏感信息由 Redact 模块集中拦截。
Q_DECLARE_LOGGING_CATEGORY(lcApp)
Q_DECLARE_LOGGING_CATEGORY(lcBrowser)
Q_DECLARE_LOGGING_CATEGORY(lcAuth)
Q_DECLARE_LOGGING_CATEGORY(lcAdapter)
Q_DECLARE_LOGGING_CATEGORY(lcParser)
Q_DECLARE_LOGGING_CATEGORY(lcRepo)
Q_DECLARE_LOGGING_CATEGORY(lcSync)
Q_DECLARE_LOGGING_CATEGORY(lcScheduler)

/**
 * 日志前端。
 *
 * 设计要点：
 *   - 所有格式化的消息**再次**经过 redact::text()，形成第二道防线。
 *     第一道防线是调用方自觉不传敏感值；第二道是这里。
 *   - 日志文件写入 logs/ 目录（运行时创建，不硬编码在源码目录）。
 *   - 通过 Qt 消息处理器接管 qDebug/qInfo/qWarning/qCritical。
 *   - 提供 Qt 信号把日志行送到 UI，便于「错误日志与重新登录提示」面板。
 */
class Logger : public QObject
{
    Q_OBJECT

public:
    enum class Level { Debug, Info, Warning, Critical };

    /// 安装全局消息处理器。可重复调用，幂等。
    /// @param logDir 日志目录；为空则使用 defaultLogDir()
    /// @param maxBytesPerFile 单文件上限，超过后轮转
    /// @param maxFiles 保留的历史文件数
    static Logger *install(const QString &logDir = QString(),
                           qint64 maxBytesPerFile = 4 * 1024 * 1024,
                           int maxFiles = 5);

    /// 卸载并释放
    static void uninstall();

    static Logger *instance();

    /// 解析默认日志目录：环境变量 NJULMS_LOG_DIR 优先，
    /// 否则使用可执行文件同级 logs/，再退回系统本地数据目录。
    /// 目录不存在时创建。
    static QString defaultLogDir();

    /// 已写入的日志文件绝对路径
    QString logFilePath() const;

signals:
    /// 一行日志（已脱敏），供 UI 显示
    void messageLogged(njulms::Logger::Level level, const QString &line);

private:
    explicit Logger(QString logDir, qint64 maxBytesPerFile, int maxFiles);
    ~Logger() override;

    /// Qt 全局消息处理器入口。必须能访问私有成员，故作为静态成员而非自由函数。
    static void handleMessage(QtMsgType type,
                              const QMessageLogContext &context,
                              const QString &msg);

    /// 写入一行已脱敏的日志
    void writeLine(Level level, const QString &line);

    class Impl;
    std::unique_ptr<Impl> d;
};

} // namespace njulms
