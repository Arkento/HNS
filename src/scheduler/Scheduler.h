#pragma once

#include "repository/SqliteRepository.h"
#include "sync/SyncManager.h"

#include <QObject>
#include <QString>

#include <memory>

namespace njulms {

/**
 * 定时调度模块。
 *
 * 职责边界（需求文档第三节第 6 点）：
 *   - 按用户配置的间隔触发同步
 *   - 避免同一个任务并发执行
 *   - 程序启动后检查未完成的任务并提示
 *   - 提供启用 / 停用 / 立即执行
 *   - **不**自己执行采集（交给 SyncManager），**不**自己写数据库
 *
 * ── 关于 QTimer 的固有限制（需求文档明确要求注意）─────────────────────────
 * 1. 电脑关机或程序退出期间 QTimer 不会运行。因此「每 N 小时一次」只在程序
 *    运行期间有效，不能当作「一定每 N 小时采集一次」的保证。
 * 2. 因此本模块在程序启动时会检查上一次的运行状态：
 *    - 若存在上次异常退出遗留的 running 运行，调用 Repository::reapOrphanedRuns
 *      把它收尾，并发出 orphanedRunDetected，提示用户上次未完成。
 *    - 若距上次成功同步已经超过一个周期，发出 overdueSince 提示可以立即补一次。
 * 3. 绝不并发执行：SyncManager::start 自身会拒绝重复启动，本模块再做一层
 *    「已排程中」的状态判断，避免堆积重复触发。
 */
class Scheduler : public QObject
{
    Q_OBJECT

public:
    /// 默认周期：1 小时。
    /// ⚠️ 该频率尚未与站点管理方确认是否合适；需求第十条要求对无法确认的
    /// 访问方式先暂停并说明，因此 UI 上必须显示该提示，用户可自行调大。
    static constexpr int kDefaultIntervalMinutes = 60;

    /// 允许的周期范围（分钟）
    static constexpr int kMinIntervalMinutes = 15;
    static constexpr int kMaxIntervalMinutes = 24 * 60;

    Scheduler(SqliteRepository *repository, SyncManager *syncManager, QObject *parent = nullptr);
    ~Scheduler() override;

    // ---- 配置 ---------------------------------------------------------------

    /// 周期（分钟）。会被夹取到 [kMinIntervalMinutes, kMaxIntervalMinutes]。
    void setIntervalMinutes(int minutes);
    int intervalMinutes() const;

    /// 用哪种模式做定时同步。默认增量（全量只在用户显式要求时跑）。
    void setMode(SyncManager::Mode mode);
    SyncManager::Mode mode() const;

    /// v2：全量对账间隔（小时）。定时任务下一次触发时，若距上次**成功全量扫描**
    /// 已超过该值，就自动把本轮改成全量（覆盖待办接口看不到的历史变化）。
    /// 设为 0 表示不做自动全量对账。
    void setFullReconcileHours(int hours);
    int fullReconcileHours() const;

    /// 是否在程序启动时，若发现上次同步已超期则立即补一次
    void setCatchUpOnStartup(bool enabled);

    // ---- 控制 ---------------------------------------------------------------

    /// 启用定时同步。会立即启动计时器。
    void start();

    /// 停用定时同步（不影响正在进行的同步任务）
    void stop();

    bool isEnabled() const;

    /// 距离下次触发的毫秒数；未启用时返回 -1
    qint64 msUntilNextRun() const;

    /// 下一次触发的预计时刻（UTC）；未启用时返回无效值
    QDateTime nextRunAt() const;

    /// 立即触发一次（忽略计时器）。若已有任务在跑则返回 false。
    bool triggerNow(QString *error = nullptr);

    /// 程序启动时的检查：回收异常退出的运行，并判断是否需要补一次。
    /// @return 是否需要补一次同步
    bool checkStartupState(QString *summary = nullptr);

    /// 上次成功同步时刻（来自 sync_state），无记录时返回无效值
    QDateTime lastSuccessfulSync() const;

signals:
    /// 定时触发了一次同步
    void triggered(SyncManager::Mode mode);

    /// 因为已有任务在运行而跳过一次触发
    void skippedBecauseBusy();

    /// 发现上次异常退出遗留的未完成运行
    void orphanedRunDetected(int count);

    /// 距上次成功同步已超过一个周期（程序可能长时间未运行）
    void overdueSince(const QDateTime &lastSuccess, qint64 overdueMinutes);

    /// 启用状态变化
    void enabledChanged(bool enabled);

    /// 下一次触发时刻变化（供 UI 显示倒计时）
    void nextRunAtChanged(const QDateTime &nextRunAt);

private:
    /// 按当前周期重排计时器，并更新下一次触发时刻
    void armTimer();

    /// 计算本次定时任务实际使用的模式（可能因全量对账到期而改为 Full）
    SyncManager::Mode nextRunMode() const;

    class Impl;
    std::unique_ptr<Impl> d;
};

} // namespace njulms
