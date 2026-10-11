#pragma once

#include "adapters/YuketangAdapter.h"
#include "models/Course.h"

#include <QObject>
#include <QString>

#include <memory>

namespace njulms {

class SqliteRepository;

/**
 * 雨课堂同步运行器。
 *
 * 与 ChaoxingSyncRunner 同构：南大 → 学习通 → **雨课堂**串行执行，
 * 共用同一份周期配置；写课程快照与作业/考试快照、crawl_runs、parse_errors。
 *
 * 失败语义：
 *   - 课程列表取不到 → 整轮失败（不写库）；
 *   - 单门课 chapter/schedule 任一失败 → 该课失败（不写库，保留旧数据）；
 *   - 两份都成功 → 完整快照，允许软删除对账。
 */
class YuketangSyncRunner : public QObject
{
    Q_OBJECT

public:
    enum class Outcome {
        Success,
        Partial,
        Failed,
        WaitingForLogin,
        Cancelled,
    };
    Q_ENUM(Outcome)

    YuketangSyncRunner(SqliteRepository *repository, YuketangAdapter *adapter,
                       QObject *parent = nullptr);
    ~YuketangSyncRunner() override;

    bool isRunning() const;
    qint64 currentRunId() const;
    bool start(const QString &mode, QString *error = nullptr);
    void cancel();

signals:
    void runStarted(qint64 runId, const QString &mode);
    void progressChanged(int percent);
    void statusMessage(const QString &message);
    void courseFinished(const QString &courseName, bool ok, const QString &detail);
    void runFinished(njulms::YuketangSyncRunner::Outcome outcome, const QString &message,
                     int succeeded, int failed);
    void loginRequired(const QString &reason);

private:
    class Impl;
    std::unique_ptr<Impl> d;
};

} // namespace njulms
