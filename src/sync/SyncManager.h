#pragma once

#include "adapters/NjuLmsAdapter.h"
#include "models/Assignment.h"
#include "models/Course.h"
#include "repository/SqliteRepository.h"

#include <QMetaType>
#include <QObject>
#include <QQueue>
#include <QString>
#include <QStringList>

#include <memory>

namespace njulms {

/**
 * 同步与断点续传管理模块。
 *
 * 职责边界（需求文档第三节第 5 点）：
 *   - 编排「会话检查 → 课程发现 → 逐课程采集 → 分批入库 → 断点推进」
 *   - 管理暂停 / 继续 / 取消
 *   - 区分 成功 / 部分完成 / 失败 / 等待登录 四种终态
 *   - **不**直接发网络请求（交给 NjuLmsAdapter），**不**直接写 SQL（交给 SqliteRepository）
 *
 * ── 断点设计（需求第四节断点规则）─────────────────────────────────────────
 * 1. 断点粒度是「**一门课程的完整作业快照**」，不是时间戳。
 *    不使用截止时间、也不使用最后一条记录的发布时间来推断进度。
 * 2. 每个课程快照的「作业写入 + 批次记录 + 断点更新」处于**同一个 SQLite 事务**
 *    （由 SqliteRepository::applyAssignmentSnapshot 保证），因此不存在
 *    「数据没保存但断点已前进」的中间状态。
 * 3. 只有能**证明完整**的快照才允许推进断点（snapshotComplete）。
 *    接口没有分页元数据时无法证明完整，此时照常采集入库（保证不漏），
 *    但把该课程的断点标记为「未证实」，供上层提示。
 * 4. 恢复时**重新扫描全部课程**而不是只从断点之后继续。理由：整份快照
 *    在 Repository 层是幂等的（重复采集只更新、不产生重复记录），
 *    因此重扫的正确性有保证，同时天然避免「漏掉中断点之前的变更」。
 *    断点用于审计与展示进度，不作为「跳过前面的课程」的依据。
 */
class SyncManager : public QObject
{
    Q_OBJECT

public:
    /// 同步模式
    enum class Mode {
        Full,        ///< 首次全量采集
        Incremental  ///< 后续增量同步
    };

    /// 同步任务的终态。WaitingForLogin 与 Failed 必须区分：
    /// 前者需要人工重新登录，后者需要排查错误。
    enum class Outcome {
        Success,          ///< 全部课程都成功且完整
        Partial,          ///< 有课程成功、有课程失败
        Failed,           ///< 无任何课程成功
        WaitingForLogin,  ///< 会话失效，已暂停并等待人工登录
        Cancelled         ///< 用户主动取消
    };

    /// 单门课程的同步结果
    struct CourseOutcome
    {
        QString courseId;
        QString courseName;
        bool ok = false;
        /// 是否可证明快照完整（决定该课程断点能否标记为「已证实」）
        bool snapshotComplete = false;
        int inserted = 0;
        int updated = 0;
        int unchanged = 0;
        int markedMissing = 0;
        QStringList preservedFields;
        QString error;
    };

    /// 一次同步运行的汇总，用于 UI 展示与 crawl_runs 记录
    struct RunSummary
    {
        qint64 runId = 0;
        Mode mode = Mode::Full;
        Outcome outcome = Outcome::Failed;
        QDateTime startedAt;
        QDateTime finishedAt;
        int coursesTotal = 0;
        int coursesSucceeded = 0;
        int coursesFailed = 0;
        int coursesIncomplete = 0;   ///< 成功入库但无法证明完整
        int coursesSkipped = 0;      ///< 快速检查判定「无变化」而跳过的课程（v2）
        /// 本轮是否走了「待办变更信号」快速路径（v2）
        bool quickPath = false;
        int assignmentsInserted = 0;
        int assignmentsUpdated = 0;
        int assignmentsMarkedMissing = 0;
        int parseIssueCount = 0;
        QString message;
        QList<CourseOutcome> perCourse;

        int durationMs() const;
    };

    /// 同步配置
    struct Config
    {
        /// 每门课程之间额外插入的等待（毫秒）。适配器已对每次请求限速，
        /// 这里再留一点余量，避免对校方服务造成压力。
        int pauseBetweenCoursesMs = 300;
        /// 连续失败多少门课程后中止整轮同步（0 = 不因失败数中止）
        int abortAfterConsecutiveFailures = 5;
        /// 是否在会话失效时停止（必须为 true；保留开关便于测试）
        bool stopOnSessionExpired = true;
        /// v2：增量同步是否先读 /api/todos 作为「变更信号」，只深扫可能变化的课程。
        /// 接口不可用或不满足前提（尚无全量基线）时自动回退到完整扫描。
        bool useTodoFastPath = true;
        /// v2：是否用 /api/courses/{id}/activities 补全作业说明。
        ///
        /// 为什么需要：作业列表接口（/homework-activities）**经常不带** data.description
        /// （实测微积分I 的 4 条全都没有），而活动接口稳定带它——但活动接口没有分页
        /// 元数据、无法证明集合完整，因此**只补正文，绝不用于增删记录**。
        /// 代价：每门课多 1 个请求，且仅在该课程确实有"缺正文"的作业时才发。
        bool enrichDescriptions = true;
        /// v2：距上次成功全量扫描超过这个小时数时，定时任务自动改跑全量对账（0 = 关闭）
        int fullReconcileHours = 24;
    };

    SyncManager(SqliteRepository *repository, NjuLmsAdapter *adapter, QObject *parent = nullptr);
    ~SyncManager() override;

    void setConfig(const Config &config);
    Config config() const;

    // ---- 控制 ---------------------------------------------------------------

    /// 开始一次同步。若已有任务在运行则返回 false 并给出原因。
    bool start(Mode mode, QString *error = nullptr);

    /// 暂停：当前课程的快照完成后停止，后续课程不再开始。
    void pause();

    /// 继续被暂停的任务
    void resume();

    /// 取消：当前课程的快照完成后停止，终态为 Cancelled。
    /// 已提交的批次与断点保留，便于下次安全恢复。
    void cancel();

    bool isRunning() const;
    bool isPaused() const;

    int currentCourseIndex() const;   ///< 从 1 开始；无任务时为 0
    int totalCourses() const;
    QString currentCourseName() const;

signals:
    /// 同步开始
    void runStarted(njulms::SyncManager::Mode mode, qint64 runId, int totalCourses);

    /// 开始处理某门课程
    void courseStarted(int index, int total, const QString &courseId, const QString &courseName);

    /// 某门课程处理完成
    void courseFinished(const njulms::SyncManager::CourseOutcome &outcome);

    /// 解析过程中发现的字段级问题（供「错误日志」面板显示）
    void parseIssuesReported(const QString &courseId, const QList<njulms::ParseIssue> &issues);

    /// 进度变化（0..100）
    void progressChanged(int percent);

    /// 会话失效：UI 应暂停并提示用户重新登录
    void loginRequired(const QString &reason);

    /// 同步结束
    void runFinished(const njulms::SyncManager::RunSummary &summary);

    /// 供 UI 显示的实时状态文字
    void statusMessage(const QString &message);

private:
    /// 阶段 2：发现课程并写入课程快照
    void discoverCourses();

    /// v2：把待办列表与上一次快照做差异比对，算出「哪些课程需要深扫」
    void prepareQuickDiff(const QList<TodoItem> &items);

    /// 阶段 3：取出下一门课程并发起采集；队列空则收敛为终态
    void processNextCourse();

    /// 处理单门课程的采集结果：入库、记录解析异常、推进进度
    void handleCourseResult(const QString &courseId,
                            const NjuLmsAdapter::AssignmentListResult &result);

    /// 用活动接口补全缺失的作业说明（只改正文，不动记录集合）
    void enrichDescriptions(const QString &courseId,
                            const NjuLmsAdapter::AssignmentListResult &result,
                            std::function<void(const NjuLmsAdapter::AssignmentListResult &)> callback);

    /// 考试合并完成后的收尾：按需补全作业说明，然后写库（考试接入）
    void continueCourseAfterExams(const QString &courseId,
                                  const NjuLmsAdapter::AssignmentListResult &merged);

    /// 把考试列表合并进作业列表（考试接入；失败时降级为快照不完整，不做软删除）
    void mergeExamResult(const QString &courseId,
                         NjuLmsAdapter::AssignmentListResult &merged,
                         const NjuLmsAdapter::ExamListResult &exams);

    /// 课程之间的限速间隔结束后继续下一门
    void scheduleNextCourse();

    /// 汇报进度百分比
    void emitProgress();

    /// 依据 perCourse 重算成功/失败课程数（收尾前统一调用，保证各终态都有正确统计）
    void recomputeCourseCounts();

    /// 收敛为终态：写 crawl_runs、更新同步状态、发信号
    void finishRun(Outcome outcome, const QString &message);

    class Impl;
    std::unique_ptr<Impl> d;
};

/// 便于日志与 UI 使用
const char *syncModeKey(SyncManager::Mode mode) noexcept;
QString syncModeDisplay(SyncManager::Mode mode);

} // namespace njulms

// 这两个类型会经由信号传递，因此需要在 Qt 元类型系统中注册，
// 否则 QSignalSpy / 跨线程信号无法携带它们。
Q_DECLARE_METATYPE(njulms::SyncManager::Mode)
Q_DECLARE_METATYPE(njulms::SyncManager::Outcome)
Q_DECLARE_METATYPE(njulms::SyncManager::CourseOutcome)
Q_DECLARE_METATYPE(njulms::SyncManager::RunSummary)
