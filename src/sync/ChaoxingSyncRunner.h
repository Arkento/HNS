#pragma once

#include "adapters/ChaoxingAdapter.h"
#include "models/Course.h"

#include <QObject>
#include <QString>

#include <memory>

namespace njulms {

class SqliteRepository;

/**
 * 学习通（超星）同步运行器。
 *
 * 为什么单独一个类而不是并入 SyncManager：SyncManager 的全部逻辑围绕
 * NjuLmsAdapter 的 JSON 接口设计（快速增量、跨页快照、断点续传、说明补全），
 * 与学习通「浏览器导航 + HTML 列表」的采集形态差异太大。把两者强行合并
 * 会让任何一侧的改动都牵连另一侧；这里按「南大同步结束后串行跑学习通」
 * 的方式解耦（与用户确认的方案一致），共享同一份定时周期配置。
 *
 * 职责：取课程 → 写课程快照 → 逐门取作业列表 → 写作业快照（可软删除）→
 * 把字段级解析提示写入 parse_errors → 汇总本次运行结果（crawl_runs）。
 *
 * 失败语义（与既有约定一致）：
 *   - 课程列表为空 / 未按模板渲染 → 失败且**不写库**（绝不拿空快照软删已有课程）；
 *   - 作业列表解析为空 → 跳过该课程（保留旧数据），计入 skipped，不软删；
 *   - 疑似分页（页数线索）→ 正常入库但 snapshotComplete=false，不软删漏项之外的条目。
 */
class ChaoxingSyncRunner : public QObject
{
    Q_OBJECT

public:
    enum class Outcome {
        Success,          ///< 全部课程成功
        Partial,          ///< 有失败/跳过，但有成功
        Failed,           ///< 没有成功（课程列表都取不到）
        WaitingForLogin,  ///< 需要扫码登录，本次中止
        Cancelled,        ///< 用户取消
    };
    Q_ENUM(Outcome)

    ChaoxingSyncRunner(SqliteRepository *repository, ChaoxingAdapter *adapter,
                       QObject *parent = nullptr);
    ~ChaoxingSyncRunner() override;

    bool isRunning() const;
    qint64 currentRunId() const;

    /**
     * 开始一次学习通全量采集（学习通侧无 JSON 变更信号，全量/增量目前同一路径，
     * mode 仅用于运行记录，便于后续实现真正的增量）。
     */
    bool start(const QString &mode, QString *error = nullptr);

    void cancel();

signals:
    void runStarted(qint64 runId, const QString &mode);
    void progressChanged(int percent);
    void statusMessage(const QString &message);
    void courseFinished(const QString &courseName, bool ok, const QString &detail);
    void runFinished(njulms::ChaoxingSyncRunner::Outcome outcome, const QString &message,
                     int succeeded, int failed, int skipped);
    void loginRequired(const QString &reason);

private:
    class Impl;
    std::unique_ptr<Impl> d;
};

} // namespace njulms
