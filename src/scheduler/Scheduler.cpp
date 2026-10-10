#include "Scheduler.h"

#include "utils/Logger.h"
#include "utils/TimeUtils.h"

#include <QDateTime>
#include <QTimer>

#include <limits>
#include <utility>

namespace njulms {

class Scheduler::Impl
{
public:
    SqliteRepository *repository = nullptr;
    SyncManager *sync = nullptr;

    QTimer *timer = nullptr;
    bool enabled = false;
    int intervalMinutes = Scheduler::kDefaultIntervalMinutes;
    SyncManager::Mode mode = SyncManager::Mode::Incremental;
    bool catchUpOnStartup = true;
    /// v2：距上次成功全量扫描超过这个小时数时，定时任务自动改跑全量对账（0 = 关闭）
    int fullReconcileHours = 24;

    /// 已排程的下一次触发时刻（UTC）。QTimer 本身不暴露这个信息，
    /// 因此自己记录，用于 UI 倒计时。
    QDateTime nextRunAt;
};

Scheduler::Scheduler(SqliteRepository *repository, SyncManager *syncManager, QObject *parent)
    : QObject(parent), d(std::make_unique<Impl>())
{
    Q_ASSERT(repository != nullptr);
    Q_ASSERT(syncManager != nullptr);

    d->repository = repository;
    d->sync = syncManager;

    d->timer = new QTimer(this);
    d->timer->setSingleShot(true);  // 每次触发后重新计算下一次，避免漂移累积
    connect(d->timer, &QTimer::timeout, this, [this]() {
        d->nextRunAt = QDateTime();
        if (!d->enabled) {
            return;
        }
        // 不并发执行：SyncManager 自己也会拒绝，这里提前跳过并告知 UI
        if (d->sync->isRunning()) {
            qCInfo(lcScheduler) << "scheduled run skipped: a sync task is already running";
            emit skippedBecauseBusy();
            armTimer();
            return;
        }
        // v2：到点前先判断要不要做「完整对账」（距上次全量成功超过阈值）
        const SyncManager::Mode mode = nextRunMode();
        qCInfo(lcScheduler) << "scheduled run triggered; mode:" << syncModeKey(mode);
        emit triggered(mode);

        QString error;
        if (!d->sync->start(mode, &error)) {
            qCWarning(lcScheduler) << "scheduled start rejected:" << error;
        }
        armTimer();
    });
}

Scheduler::~Scheduler() = default;

// ---------------------------------------------------------------------------
// 配置
// ---------------------------------------------------------------------------

void Scheduler::setIntervalMinutes(int minutes)
{
    d->intervalMinutes = qBound(kMinIntervalMinutes, minutes, kMaxIntervalMinutes);
    if (d->enabled) {
        armTimer();  // 周期变化后立即按新周期重排
    }
}

int Scheduler::intervalMinutes() const
{
    return d->intervalMinutes;
}

void Scheduler::setMode(SyncManager::Mode mode)
{
    d->mode = mode;
}

SyncManager::Mode Scheduler::mode() const
{
    return d->mode;
}

void Scheduler::setFullReconcileHours(int hours)
{
    d->fullReconcileHours = qMax(0, hours);
}

int Scheduler::fullReconcileHours() const
{
    return d->fullReconcileHours;
}

SyncManager::Mode Scheduler::nextRunMode() const
{
    if (d->fullReconcileHours <= 0 || d->mode == SyncManager::Mode::Full) {
        return d->mode;
    }

    const auto state = d->repository->findSyncState(SqliteRepository::fullReconcileTaskKey());
    const QDateTime last =
        state.has_value() ? state->lastSuccessAt.value_or(QDateTime()) : QDateTime();
    if (!last.isValid()) {
        qCInfo(lcScheduler) << "no successful full scan yet; this run will be a full scan";
        return SyncManager::Mode::Full;
    }
    if (last.secsTo(timeutil::nowUtc()) >= qint64(d->fullReconcileHours) * 3600) {
        qCInfo(lcScheduler) << "full reconcile is due (last full scan:" << last.toString(Qt::ISODate)
                            << "); this run will be a full scan";
        return SyncManager::Mode::Full;
    }
    return SyncManager::Mode::Incremental;
}

void Scheduler::setCatchUpOnStartup(bool enabled)
{
    d->catchUpOnStartup = enabled;
}

// ---------------------------------------------------------------------------
// 控制
// ---------------------------------------------------------------------------

void Scheduler::start()
{
    if (d->enabled) {
        return;
    }
    d->enabled = true;
    armTimer();
    emit enabledChanged(true);
    qCInfo(lcScheduler) << "scheduler enabled; interval (minutes):" << d->intervalMinutes;
}

void Scheduler::stop()
{
    if (!d->enabled) {
        return;
    }
    d->enabled = false;
    d->timer->stop();
    d->nextRunAt = QDateTime();
    emit enabledChanged(false);
    emit nextRunAtChanged(d->nextRunAt);
    qCInfo(lcScheduler) << "scheduler disabled";
}

bool Scheduler::isEnabled() const
{
    return d->enabled;
}

qint64 Scheduler::msUntilNextRun() const
{
    if (!d->enabled || !d->nextRunAt.isValid()) {
        return -1;
    }
    return qMax<qint64>(0, timeutil::nowUtc().msecsTo(d->nextRunAt));
}

QDateTime Scheduler::nextRunAt() const
{
    return d->nextRunAt;
}

void Scheduler::armTimer()
{
    if (!d->enabled) {
        return;
    }
    const qint64 intervalMs = qint64(d->intervalMinutes) * 60 * 1000;
    d->timer->start(int(qMin<qint64>(intervalMs, std::numeric_limits<int>::max())));
    d->nextRunAt = timeutil::nowUtc().addMSecs(intervalMs);
    emit nextRunAtChanged(d->nextRunAt);
}

bool Scheduler::triggerNow(QString *error)
{
    if (d->sync->isRunning()) {
        if (error != nullptr) {
            *error = QStringLiteral("a sync task is already running");
        }
        emit skippedBecauseBusy();
        return false;
    }

    // 手动触发同样会走「全量对账到期则改跑全量」的判断
    const SyncManager::Mode mode = nextRunMode();
    qCInfo(lcScheduler) << "manual trigger; mode:" << syncModeKey(mode);
    emit triggered(mode);
    return d->sync->start(mode, error);
}

// ---------------------------------------------------------------------------
// 启动状态检查
// ---------------------------------------------------------------------------

QDateTime Scheduler::lastSuccessfulSync() const
{
    const auto state = d->repository->findSyncState(SqliteRepository::courseDiscoveryTaskKey());
    if (!state.has_value()) {
        return QDateTime();
    }
    return state->lastSuccessAt.value_or(QDateTime());
}

bool Scheduler::checkStartupState(QString *summary)
{
    QStringList notes;

    // 1) 回收上次异常退出遗留的 running 运行。
    //    这是需求场景 A 在调度层的入口：程序重启后必须识别未完成的任务。
    QString error;
    const int reaped = d->repository->reapOrphanedRuns(&error);
    if (reaped < 0) {
        qCWarning(lcScheduler) << "cannot reap orphaned runs:" << error;
        notes << QStringLiteral("无法检查上次运行状态：%1").arg(error);
    } else if (reaped > 0) {
        notes << QStringLiteral("发现 %1 个上次未完成的采集任务，已标记为中断").arg(reaped);
        emit orphanedRunDetected(reaped);
    }

    // 2) 判断距上次成功同步是否已超过一个周期。
    //    由于 QTimer 在程序未运行时不会触发，这里必须主动补一次提示。
    bool needsCatchUp = false;
    const QDateTime last = lastSuccessfulSync();
    if (last.isValid()) {
        const qint64 elapsedMinutes = last.secsTo(timeutil::nowUtc()) / 60;
        if (elapsedMinutes > d->intervalMinutes) {
            needsCatchUp = true;
            notes << QStringLiteral("距上次成功同步已 %1 分钟（周期 %2 分钟），建议立即同步一次")
                         .arg(elapsedMinutes)
                         .arg(d->intervalMinutes);
            emit overdueSince(last, elapsedMinutes);
        }
    } else {
        // 从未成功同步过
        needsCatchUp = true;
        notes << QStringLiteral("尚未完成过任何同步，建议先执行一次全量采集");
    }

    if (summary != nullptr) {
        *summary = notes.join(QStringLiteral("；"));
    }
    if (!notes.isEmpty()) {
        qCInfo(lcScheduler) << "startup check:" << notes.join(QStringLiteral("; "));
    }

    return needsCatchUp && d->catchUpOnStartup;
}

} // namespace njulms
