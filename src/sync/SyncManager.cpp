#include "SyncManager.h"

#include "utils/Logger.h"
#include "utils/Redact.h"
#include "utils/TimeUtils.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QSet>
#include <QTimer>

#include <algorithm>
#include <utility>
namespace njulms {

const char *syncModeKey(SyncManager::Mode mode) noexcept
{
    switch (mode) {
    case SyncManager::Mode::Full:        return "full";
    case SyncManager::Mode::Incremental: return "incremental";
    }
    return "full";
}

QString syncModeDisplay(SyncManager::Mode mode)
{
    switch (mode) {
    case SyncManager::Mode::Full:        return QStringLiteral("全量采集");
    case SyncManager::Mode::Incremental: return QStringLiteral("增量同步");
    }
    return QStringLiteral("同步");
}

int SyncManager::RunSummary::durationMs() const
{
    if (!startedAt.isValid() || !finishedAt.isValid()) {
        return 0;
    }
    return int(startedAt.msecsTo(finishedAt));
}

// ---------------------------------------------------------------------------
// Impl
// ---------------------------------------------------------------------------

class SyncManager::Impl
{
public:
    Impl(SqliteRepository *repo, NjuLmsAdapter *ad) : repository(repo), adapter(ad) {}

    SqliteRepository *repository = nullptr;
    NjuLmsAdapter *adapter = nullptr;
    Config config;

    bool running = false;
    bool pauseRequested = false;
    bool cancelRequested = false;
    bool sessionExpired = false;

    // ---- v2 快速路径（待办变更信号）----
    /// 本轮是否启用了快速路径（决定是否跳过"无变化"的课程）
    bool quickDiffActive = false;
    /// 需要深度扫描的课程集合（由待办差异得出）
    QSet<QString> quickChangedCourses;
    /// 本轮待办集合的规范化行（成功后写入 sync_state，供下一轮比对）
    QStringList quickTodoLines;

    Mode mode = Mode::Full;
    qint64 runId = 0;

    QQueue<Course> pending;      ///< 待处理课程
    int totalCourses = 0;
    int processedCourses = 0;
    Course currentCourse;
    bool hasCurrentCourse = false;

    int consecutiveFailures = 0;
    int parseIssueCount = 0;

    RunSummary summary;
};

// ---------------------------------------------------------------------------
// 构造
// ---------------------------------------------------------------------------

SyncManager::SyncManager(SqliteRepository *repository, NjuLmsAdapter *adapter, QObject *parent)
    : QObject(parent), d(std::make_unique<Impl>(repository, adapter))
{
    Q_ASSERT(repository != nullptr);
    Q_ASSERT(adapter != nullptr);

    // 适配器报告会话失效时，立刻把本轮同步引向 WaitingForLogin 终态，
    // 而不是继续对剩余课程发请求（那些请求只会重复失败）。
    connect(d->adapter, &NjuLmsAdapter::sessionExpired, this, [this]() {
        if (!d->running) {
            return;
        }
        d->sessionExpired = true;
        d->pending.clear();
        if (d->config.stopOnSessionExpired) {
            d->pauseRequested = true;
            emit statusMessage(QStringLiteral("登录状态已失效，同步已暂停。请重新登录后再继续。"));
            emit loginRequired(QStringLiteral("会话已失效，响应不是有效的 JSON 数据"));
        }
    });
}

SyncManager::~SyncManager() = default;

void SyncManager::setConfig(const Config &config)
{
    d->config = config;
}

SyncManager::Config SyncManager::config() const
{
    return d->config;
}

bool SyncManager::isRunning() const
{
    return d->running;
}

bool SyncManager::isPaused() const
{
    return d->running && d->pauseRequested;
}

int SyncManager::currentCourseIndex() const
{
    if (!d->running) {
        return 0;
    }
    return qMin(d->processedCourses + 1, qMax(d->totalCourses, 1));
}

int SyncManager::totalCourses() const
{
    return d->totalCourses;
}

QString SyncManager::currentCourseName() const
{
    return d->hasCurrentCourse ? d->currentCourse.courseName : QString();
}

// ---------------------------------------------------------------------------
// start
// ---------------------------------------------------------------------------

bool SyncManager::start(Mode mode, QString *error)
{
    const auto reject = [error](const QString &message) {
        if (error != nullptr) {
            *error = message;
        }
        return false;
    };

    if (d->running) {
        return reject(QStringLiteral("a sync task is already running"));
    }
    if (d->repository == nullptr || !d->repository->isOpen()) {
        return reject(QStringLiteral("database is not open"));
    }
    if (d->adapter == nullptr) {
        return reject(QStringLiteral("adapter is not available"));
    }

    // 重置本轮状态
    d->mode = mode;
    d->running = true;
    d->pauseRequested = false;
    d->cancelRequested = false;
    d->sessionExpired = false;
    d->processedCourses = 0;
    d->totalCourses = 0;
    d->consecutiveFailures = 0;
    d->parseIssueCount = 0;
    d->hasCurrentCourse = false;
    d->pending.clear();

    // 快速路径的状态必须**每轮清空**。
    //
    // 踩过的坑（用户报的 bug）：这两个字段只在"增量 + 开了快速增量"的分支里被赋值，
    // 之前没有在启动时重置。于是跑过一次"待办无变化"的增量之后，再点全量采集，
    // quickDiffActive 仍是 true、quickChangedCourses 还留着上一轮的集合，
    // 结果全量把每一门课都当成"无变化"跳过 —— 一个请求都不发，
    // 摘要还显示"快速检查完成：N 门课程均无变化"。
    d->quickDiffActive = false;
    d->quickChangedCourses.clear();
    d->quickTodoLines.clear();

    d->summary = RunSummary{};
    d->summary.mode = mode;
    d->summary.startedAt = timeutil::nowUtc();

    QString repoError;
    d->runId = d->repository->startRun(QStringLiteral("sync"), QString::fromLatin1(syncModeKey(mode)),
                                      &repoError);
    if (d->runId <= 0) {
        d->running = false;
        return reject(QStringLiteral("cannot start run record: %1").arg(repoError));
    }
    d->summary.runId = d->runId;

    qCInfo(lcSync) << "sync started; mode:" << syncModeKey(mode) << "run:" << d->runId;

    // ---- v2 快速路径：先读「待办」作为变更信号 ----
    // 三个前提：增量模式、配置开启、并且已经有过一次成功的全量扫描（否则没有基线，
    // 必须完整扫一遍才能保证不漏）。
    const bool baselineReady =
        d->repository->findSyncState(SqliteRepository::fullReconcileTaskKey()).has_value();
    if (mode == Mode::Incremental && d->config.useTodoFastPath && baselineReady) {
        d->quickDiffActive = true;
        d->summary.quickPath = true;
        emit statusMessage(QStringLiteral("正在快速检查待办变化…"));
        d->adapter->fetchTodos([this](const NjuLmsAdapter::TodoListResult &todos) {
            if (!todos.error.ok() || !todos.supported) {
                // 接口不可用或形态变化：绝不能当成"没有待办"，回退到完整扫描
                qCWarning(lcSync) << "todo change signal unavailable; falling back to a full scan:"
                                  << todos.error.message;
                d->quickDiffActive = false;
                d->summary.quickPath = false;
                emit statusMessage(QStringLiteral("待办接口不可用，改为完整扫描…"));
            } else {
                prepareQuickDiff(todos.items);
            }
            // 课程列表依旧刷新（1 个请求）：保持课程表最新，同时为深扫提供课程对象
            discoverCourses();
        });
        return true;
    }
    if (mode == Mode::Incremental && d->config.useTodoFastPath && !baselineReady) {
        qCInfo(lcSync) << "no full baseline yet; running a complete scan first";
        emit statusMessage(QStringLiteral("尚无全量基线，先执行完整扫描…"));
    }

    // 第一步：确认登录状态。绝不因为首页能加载就认为已登录。
    emit statusMessage(QStringLiteral("正在检查登录状态…"));
    d->adapter->checkSession([this](const NjuLmsAdapter::SessionCheck &check) {
        if (!check.error.ok()) {
            if (check.error.needsLogin()) {
                d->sessionExpired = true;
                emit loginRequired(check.error.message);
            }
            finishRun(Outcome::WaitingForLogin, check.error.message);
            return;
        }
        // 第二步：发现课程。每次都重新发现，不依赖首次结果（需求场景 G）
        discoverCourses();
    });

    return true;
}

// ---------------------------------------------------------------------------
// 阶段 2：课程发现
// ---------------------------------------------------------------------------

void SyncManager::prepareQuickDiff(const QList<TodoItem> &items)
{
    struct Entry
    {
        QString courseId;
        QString sourceId;
        QDateTime endTime;
        QString line;   ///< "courseId|sourceId|endTime"，用于与上一轮快照比对
    };

    // 归一化当前待办集合：保留作业与考试（本项目的采集范围；考试接入后同为变更信号）
    QList<Entry> current;
    current.reserve(items.size());
    int ignoredOtherTypes = 0;
    for (const TodoItem &item : items) {
        const bool isHomework =
            item.type.compare(QLatin1String("homework"), Qt::CaseInsensitive) == 0;
        const bool isExam = item.type.compare(QLatin1String("exam"), Qt::CaseInsensitive) == 0;
        if (!isHomework && !isExam) {
            ++ignoredOtherTypes;   // 问卷/课件等不在采集范围内
            continue;
        }
        const QString sourceId = item.sourceId();
        if (sourceId.isEmpty()) {
            continue;
        }
        Entry entry;
        entry.courseId = item.courseId;
        entry.sourceId = sourceId;
        entry.endTime = item.endTime;
        entry.line = QStringLiteral("%1|%2|%3")
                         .arg(item.courseId, sourceId,
                              item.endTime.isValid()
                                  ? item.endTime.toUTC().toString(Qt::ISODate)
                                  : QString());
        current.append(entry);
    }
    std::sort(current.begin(), current.end(),
              [](const Entry &a, const Entry &b) { return a.line < b.line; });

    QStringList currentLines;
    currentLines.reserve(current.size());
    for (const Entry &entry : std::as_const(current)) {
        currentLines << entry.line;
    }
    d->quickTodoLines = currentLines;

    // 上一次成功同步时记下的待办集合（存在 sync_state.cursor 里）
    QStringList previous;
    if (const auto state = d->repository->findSyncState(SqliteRepository::todoSnapshotTaskKey());
        state.has_value() && !state->cursor.isEmpty()) {
        previous = state->cursor.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    }
    const QSet<QString> currentSet(currentLines.cbegin(), currentLines.cend());
    const QSet<QString> previousSet(previous.cbegin(), previous.cend());

    QSet<QString> changed;

    // ---- ① 与数据库对账：库里没有 / 曾软删除又回来 / 截止时间不一致 → 深扫 ----
    int newItems = 0;
    int deadlineMismatches = 0;
    for (const Entry &entry : std::as_const(current)) {
        const auto existing = d->repository->findAssignment(entry.courseId, entry.sourceId);
        if (!existing.has_value()) {
            ++newItems;
            changed.insert(entry.courseId);
            continue;
        }
        if (!existing->present) {
            changed.insert(entry.courseId);   // 曾经被判为消失，现在又出现在待办里
            continue;
        }
        if (entry.endTime.isValid()) {
            const bool storedValid = existing->dueAt.hasValue();
            if (!storedValid || existing->dueAt.require() != entry.endTime) {
                ++deadlineMismatches;
                changed.insert(entry.courseId);
            }
        }
    }

    // ---- ② 与上一轮待办快照对比：待办出现/消失 → 深扫（覆盖"提交后消失"这类状态变化）----
    // 注意：只有在**已经有上一轮快照**时才做"新增"判定——第一次快速检查
    // （通常在刚做完全量之后）没有快照，此时以 ① 的数据库对账为准，
    // 否则会把所有课程都误判成"新增"。
    if (!previous.isEmpty()) {
        for (const Entry &entry : std::as_const(current)) {
            if (!previousSet.contains(entry.line)) {
                changed.insert(entry.courseId);
            }
        }
    }
    for (const QString &entry : previous) {
        if (!currentSet.contains(entry)) {
            changed.insert(entry.section(QLatin1Char('|'), 0, 0));
        }
    }

    d->quickChangedCourses = changed;
    qCInfo(lcSync) << "quick check: todos:" << currentLines.size()
                   << "ignored(other types):" << ignoredOtherTypes
                   << "new items:" << newItems
                   << "deadline mismatches:" << deadlineMismatches
                   << "courses needing deep scan:" << changed.size();
}

void SyncManager::discoverCourses()
{
    emit statusMessage(QStringLiteral("正在获取课程列表…"));
    d->adapter->discoverCourses([this](const NjuLmsAdapter::CourseListResult &result) {
        if (!result.error.ok()) {
            if (result.error.needsLogin()) {
                d->sessionExpired = true;
                emit loginRequired(result.error.message);
                finishRun(Outcome::WaitingForLogin, result.error.message);
            } else {
                finishRun(Outcome::Failed, result.error.message);
            }
            return;
        }

        // 课程列表写入数据库：即使随后采集中断，课程也已被发现（场景 G）
        QString repoError;
        if (!d->repository->applyCourseSnapshot(source_keys::kNjuLms.toString(),
                                                result.courses, &repoError)) {
            finishRun(Outcome::Failed,
                      QStringLiteral("cannot persist course list: %1").arg(repoError));
            return;
        }

        if (result.courses.isEmpty()) {
            // 合法状态：确实没有任何课程。这是成功，不是失败。
            qCInfo(lcSync) << "no courses to sync";
            finishRun(Outcome::Success, QStringLiteral("当前账号没有可采集的课程"));
            return;
        }

        d->totalCourses = result.courses.size();
        for (const Course &c : std::as_const(result.courses)) {
            d->pending.enqueue(c);
        }

        d->summary.coursesTotal = d->totalCourses;
        emit runStarted(d->mode, d->runId, d->totalCourses);
        emit progressChanged(0);
        emit statusMessage(QStringLiteral("共发现 %1 门课程，开始采集…").arg(d->totalCourses));

        processNextCourse();
    });
}

// ---------------------------------------------------------------------------
// 阶段 3：逐课程采集
// ---------------------------------------------------------------------------

void SyncManager::processNextCourse()
{
    if (!d->running) {
        return;
    }

    // ---- 终态判定，顺序不能颠倒 ----
    if (d->cancelRequested) {
        finishRun(Outcome::Cancelled, QStringLiteral("用户取消了同步"));
        return;
    }
    if (d->sessionExpired && d->config.stopOnSessionExpired) {
        finishRun(Outcome::WaitingForLogin, QStringLiteral("会话已失效，等待重新登录"));
        return;
    }
    if (d->pauseRequested) {
        // 暂停不是终态：保留 running 状态与队列，等待 resume()
        emit statusMessage(QStringLiteral("已暂停。点击「继续」可从当前进度恢复。"));
        return;
    }
    if (d->pending.isEmpty()) {
        // 全部课程处理完毕，按成功/失败数量给出终态
        recomputeCourseCounts();

        Outcome outcome = Outcome::Failed;
        QString message;
        const int incomplete = d->summary.coursesIncomplete;
        if (d->summary.coursesFailed == 0 && incomplete == 0) {
            outcome = Outcome::Success;
            if (d->summary.coursesSkipped > 0) {
                // 快速路径：说明"几门有变化、几门被跳过"，避免让人误以为漏采
                message = d->summary.coursesSucceeded > 0
                              ? QStringLiteral("快速检查完成：%1 门课程中 %2 门有变化并已更新，%3 门无变化")
                                    .arg(d->summary.coursesTotal)
                                    .arg(d->summary.coursesSucceeded)
                                    .arg(d->summary.coursesSkipped)
                              : QStringLiteral("快速检查完成：%1 门课程均无变化")
                                    .arg(d->summary.coursesTotal);
            } else {
                message = QStringLiteral("全部 %1 门课程采集完成").arg(d->summary.coursesSucceeded);
            }
        } else if (d->summary.coursesSucceeded > 0) {
            // 有课程失败，或有意门课程快照不完整 —— 都不能算「全量成功」
            outcome = Outcome::Partial;
            message = incomplete > 0
                          ? QStringLiteral("%1 门成功，%2 门失败，%3 门快照不完整")
                                .arg(d->summary.coursesSucceeded)
                                .arg(d->summary.coursesFailed)
                                .arg(incomplete)
                          : QStringLiteral("%1 门成功，%2 门失败")
                                .arg(d->summary.coursesSucceeded)
                                .arg(d->summary.coursesFailed);
        } else {
            outcome = Outcome::Failed;
            message = QStringLiteral("全部 %1 门课程采集失败").arg(d->summary.coursesFailed);
        }
        finishRun(outcome, message);
        return;
    }

    d->currentCourse = d->pending.dequeue();
    d->hasCurrentCourse = true;

    // ---- v2 快速路径：待办没有变化的课程直接跳过，一个请求都不发 ----
    // 双重保险：即使标志位被上一轮污染，全量模式也绝不跳过任何课程。
    if (d->mode == Mode::Incremental && d->quickDiffActive
        && !d->quickChangedCourses.contains(d->currentCourse.courseId)) {
        qCDebug(lcSync) << "quick check: skipping unchanged course"
                        << redact::courseId(d->currentCourse.courseId);
        d->hasCurrentCourse = false;
        ++d->summary.coursesSkipped;
        ++d->processedCourses;
        emitProgress();
        scheduleNextCourse();
        return;
    }

    const int index = d->processedCourses + 1;
    emit courseStarted(index, d->totalCourses, d->currentCourse.courseId,
                       d->currentCourse.courseName);
    emit statusMessage(QStringLiteral("正在采集 %1（%2/%3）…")
                           .arg(d->currentCourse.courseName)
                           .arg(index)
                           .arg(d->totalCourses));

    // 记录断点：即将开始的课程
    SyncState state;
    state.taskKey = SqliteRepository::courseDiscoveryTaskKey();
    state.mode = QString::fromLatin1(syncModeKey(d->mode));
    state.cursor = QStringLiteral("in-progress:");
    state.lastSuccessAt = timeutil::nowUtc();
    state.fullScanCompleted = false;
    state.updatedAt = timeutil::nowUtc();
    QString ignored;
    d->repository->upsertSyncState(state, &ignored);

    const QString courseId = d->currentCourse.courseId;
    d->adapter->fetchAssignments(
        courseId, [this, courseId](const NjuLmsAdapter::AssignmentListResult &result) {
            if (!result.error.ok()) {
                // 作业列表失败：与以前一致，直接交结果（考试不再尝试，避免连续无效请求）
                handleCourseResult(courseId, result);
                return;
            }

            // 作业成功 → 再取考试列表，合并为一门课的统一快照（考试接入）
            d->adapter->fetchExams(
                courseId,
                [this, courseId, result](const NjuLmsAdapter::ExamListResult &exams) {
                    NjuLmsAdapter::AssignmentListResult merged = result;
                    mergeExamResult(courseId, merged, exams);
                    continueCourseAfterExams(courseId, merged);
                });
        });
}

/**
 * 考试合并完成后的收尾：按需补全作业说明，然后写库。
 *
 * 说明补全只针对"作业"条目：考试没有活动接口可补，且 exam-* 条目
 * 绝不参与 byActivity 匹配（避免万一 id 相撞被填错正文）。
 */
void SyncManager::continueCourseAfterExams(const QString &courseId,
                                           const NjuLmsAdapter::AssignmentListResult &merged)
{
    bool needsEnrichment = false;
    if (d->config.enrichDescriptions) {
        for (const Assignment &item : merged.assignments) {
            if (item.kind == QLatin1String("homework") && !item.description.hasValue()) {
                needsEnrichment = true;
                break;
            }
        }
    }
    if (!needsEnrichment) {
        handleCourseResult(courseId, merged);
        return;
    }

    enrichDescriptions(courseId, merged,
                       [this, courseId](const NjuLmsAdapter::AssignmentListResult &enriched) {
                           handleCourseResult(courseId, enriched);
                       });
}

/**
 * 把考试列表合并进作业列表（考试接入，2026-10-10）。
 *
 * 规则：
 *   - 考试条目直接追加（sourceId 前缀 exam-，与作业的 homework- 天然不冲突，仍做去重防御）；
 *   - 考试接口失败或信封不符：保留已取到的作业数据，但把整门课标记为**快照不完整**
 *     （snapshotComplete=false → 仓储不做软删除）——宁可保留旧数据，不可误删；
 *     并追加一条 Invalid 解析问题写入 parse_errors，便于排查；
 *   - 只有作业与考试两份列表都完整时，整门课才允许软删除对账。
 */
void SyncManager::mergeExamResult(const QString &courseId,
                                  NjuLmsAdapter::AssignmentListResult &merged,
                                  const NjuLmsAdapter::ExamListResult &exams)
{
    if (!exams.error.ok()) {
        merged.issues.append(ParseIssue(
            QStringLiteral("exam-list"),
            QStringLiteral("exam list unavailable: %1").arg(exams.error.message),
            FieldState::Invalid));
        merged.snapshotComplete = false;
        qCWarning(lcSync) << "exam list unavailable;"
                          << "course:" << redact::courseId(courseId)
                          << "needsLogin:" << exams.error.needsLogin()
                          << "reason:" << exams.error.message;
        return;
    }

    merged.issues.append(exams.issues);
    for (const Assignment &exam : exams.exams) {
        const auto duplicate = std::find_if(
            merged.assignments.cbegin(), merged.assignments.cend(),
            [&exam](const Assignment &existing) { return existing.sourceId == exam.sourceId; });
        if (duplicate == merged.assignments.cend()) {
            merged.assignments.append(exam);
        }
    }
    merged.snapshotComplete = merged.snapshotComplete && exams.snapshotComplete;

    qCInfo(lcSync) << "exams merged;"
                   << "course:" << redact::courseId(courseId)
                   << "exams:" << exams.exams.size()
                   << "complete:" << exams.snapshotComplete;
}

/**
 * 用活动接口补全作业说明。
 *
 * 只做一件事：把 /api/courses/{id}/activities 里的 data.description 按
 * activityId / unique_key 合并进已经拿到的作业列表。**不动记录集合**
 * （不新增、不删除、不改状态字段）——因为那个接口没有分页元数据，
 * 无法证明它返回的是完整集合。
 *
 * 失败一律忽略：补不到正文不影响本轮采集的结果与断点推进。
 */
void SyncManager::enrichDescriptions(
    const QString &courseId, const NjuLmsAdapter::AssignmentListResult &result,
    std::function<void(const NjuLmsAdapter::AssignmentListResult &)> callback)
{
    d->adapter->fetchCourseTasks(
        courseId,
        [this, courseId, result, callback](const NjuLmsAdapter::CourseTaskResult &tasks) {
            NjuLmsAdapter::AssignmentListResult merged = result;
            if (!tasks.error.ok()) {
                qCWarning(lcSync) << "description enrichment failed;"
                                  << "course:" << redact::courseId(courseId)
                                  << "reason:" << tasks.error.message;
                callback(merged);
                return;
            }

            QHash<QString, QString> byActivity;
            for (const Assignment &task : tasks.tasks) {
                if (!task.description.hasValue()) {
                    continue;
                }
                if (!task.activityId.isEmpty()) {
                    byActivity.insert(task.activityId, task.description.require());
                }
                if (!task.sourceId.isEmpty()) {
                    byActivity.insert(task.sourceId, task.description.require());
                }
            }

            int filled = 0;
            for (Assignment &item : merged.assignments) {
                if (item.kind != QLatin1String("homework")) {
                    continue;   // 考试不参与活动接口的正文补全
                }
                if (item.description.hasValue()) {
                    continue;
                }
                const QString text = byActivity.value(item.activityId,
                                                      byActivity.value(item.sourceId));
                if (!text.isEmpty()) {
                    item.description = Observed<QString>::present(text);
                    ++filled;
                }
            }

            qCInfo(lcSync) << "description enrichment;"
                           << "course:" << redact::courseId(courseId) << "filled:" << filled
                           << "candidates:" << tasks.homeworkCount;
            callback(merged);
        });
}

void SyncManager::handleCourseResult(const QString &courseId,
                                     const NjuLmsAdapter::AssignmentListResult &result)
{
    CourseOutcome outcome;
    outcome.courseId = courseId;
    outcome.courseName = d->currentCourse.courseName;

    if (!result.error.ok()) {
        outcome.ok = false;
        outcome.error = result.error.message;

        if (result.error.needsLogin()) {
            d->sessionExpired = true;
        }

        ++d->consecutiveFailures;
        qCWarning(lcSync) << "course fetch failed;"
                          << "course:" << redact::courseId(courseId)
                          << "retryable:" << result.error.retryable()
                          << "needsLogin:" << result.error.needsLogin();

        d->summary.perCourse.append(outcome);
        emit courseFinished(outcome);
        ++d->processedCourses;
        emitProgress();

        // 会话失效：停止继续请求，直接进入等待登录终态
        if (d->sessionExpired && d->config.stopOnSessionExpired) {
            emit loginRequired(QStringLiteral("采集过程中登录状态失效"));
            finishRun(Outcome::WaitingForLogin, QStringLiteral("会话已失效，等待重新登录"));
            return;
        }

        // 连续失败过多时中止，避免在站点故障时持续打请求（需求场景 D）
        const int limit = d->config.abortAfterConsecutiveFailures;
        if (limit > 0 && d->consecutiveFailures >= limit) {
            qCWarning(lcSync) << "aborting sync after" << d->consecutiveFailures
                              << "consecutive course failures";
            finishRun(Outcome::Failed,
                      QStringLiteral("连续 %1 门课程采集失败，已中止本轮同步")
                          .arg(d->consecutiveFailures));
            return;
        }

        scheduleNextCourse();
        return;
    }

    // ---- 成功取回：写入数据库（含批次与断点，同一事务）----
    d->consecutiveFailures = 0;

    if (!result.issues.isEmpty()) {
        d->parseIssueCount += result.issues.size();
        emit parseIssuesReported(courseId, result.issues);

        // 字段级解析异常写入 parse_errors 表，便于 UI 展示与之后重试
        QList<ParseErrorRecord> records;
        records.reserve(result.issues.size());
        for (const ParseIssue &issue : std::as_const(result.issues)) {
            // 只记录「解析失败」，Missing 属于正常情况（接口本来就不给该字段）
            if (issue.state != FieldState::Invalid) {
                continue;
            }
            ParseErrorRecord rec;
            rec.runId = d->runId;
            rec.courseId = courseId;
            rec.sourceId = QString();
            rec.field = issue.field;
            rec.reason = issue.reason;
            rec.observedAt = timeutil::nowUtc();
            records.append(rec);
        }
        if (!records.isEmpty()) {
            QString error;
            if (!d->repository->recordParseErrors(records, &error)) {
                qCWarning(lcSync) << "cannot persist parse errors:" << error;
            }
        }
    }

    const int batchNo = d->processedCourses + 1;
    const BatchApplyResult applied = d->repository->applyAssignmentSnapshot(
        courseId, result.assignments, d->runId, batchNo,
        QStringLiteral("course-index:%1").arg(batchNo),
        QStringLiteral("course:%1").arg(courseId),
        result.snapshotComplete,
        QString::fromLatin1(syncModeKey(d->mode)));

    if (!applied.ok) {
        outcome.ok = false;
        outcome.error = applied.error;
        qCWarning(lcSync) << "persist failed for course" << redact::courseId(courseId) << ":"
                          << applied.error;
    } else {
        outcome.ok = true;
        outcome.snapshotComplete = result.snapshotComplete;
        outcome.inserted = applied.inserted;
        outcome.updated = applied.updated;
        outcome.unchanged = applied.unchanged;
        outcome.markedMissing = applied.markedMissing;
        outcome.preservedFields = applied.preservedFields;

        d->summary.assignmentsInserted += applied.inserted;
        d->summary.assignmentsUpdated += applied.updated;
        d->summary.assignmentsMarkedMissing += applied.markedMissing;
        if (!result.snapshotComplete) {
            // 快照不完整：本轮不做缺失检测、不推进该课程断点，整轮结果降级为 partial
            ++d->summary.coursesIncomplete;
            qCWarning(lcSync) << "course" << redact::courseId(courseId)
                              << "snapshot incomplete; missing detection and breakpoint skipped";
        }
        if (!applied.preservedFields.isEmpty()) {
            // 场景 F 的可观测证据：本次没有用空值覆盖这些字段
            qCInfo(lcSync) << "course" << redact::courseId(courseId)
                           << "preserved" << applied.preservedFields.size()
                           << "field(s) that failed to parse this round";
        }
    }

    d->summary.perCourse.append(outcome);
    emit courseFinished(outcome);
    ++d->processedCourses;
    emitProgress();
    scheduleNextCourse();
}

void SyncManager::scheduleNextCourse()
{
    d->hasCurrentCourse = false;

    const int delay = qMax(0, d->config.pauseBetweenCoursesMs);
    if (delay == 0) {
        processNextCourse();
        return;
    }
    // 课程之间留出间隔，避免对校方服务造成压力
    QTimer::singleShot(delay, this, [this]() { processNextCourse(); });
}

void SyncManager::emitProgress()
{
    if (d->totalCourses <= 0) {
        emit progressChanged(0);
        return;
    }
    const int percent = qBound(0, (d->processedCourses * 100) / d->totalCourses, 100);
    emit progressChanged(percent);
}

// ---------------------------------------------------------------------------
// 终态处理
// ---------------------------------------------------------------------------

void SyncManager::recomputeCourseCounts()
{
    int succeeded = 0;
    int failed = 0;
    for (const CourseOutcome &o : std::as_const(d->summary.perCourse)) {
        if (o.ok) {
            ++succeeded;
        } else {
            ++failed;
        }
    }
    d->summary.coursesSucceeded = succeeded;
    d->summary.coursesFailed = failed;
}

void SyncManager::finishRun(Outcome outcome, const QString &message)
{
    if (!d->running) {
        return;
    }

    // 必须在收尾前统一重算成功/失败数：中止、取消、等待登录这几条路径
    // 都不会走到「队列排空」分支，若只在那里统计，crawl_runs 会记录成 0/0。
    recomputeCourseCounts();

    d->summary.outcome = outcome;
    d->summary.finishedAt = timeutil::nowUtc();
    d->summary.message = message;
    d->summary.parseIssueCount = d->parseIssueCount;
    d->running = false;

    // 把终态写入 crawl_runs。注意 WaitingForLogin 是独立状态，
    // 不能记为普通失败——两者的处理方式完全不同。
    CrawlRun::Status status = CrawlRun::Status::Failed;
    switch (outcome) {
    case Outcome::Success:         status = CrawlRun::Status::Success; break;
    case Outcome::Partial:         status = CrawlRun::Status::Partial; break;
    case Outcome::Failed:          status = CrawlRun::Status::Failed; break;
    case Outcome::WaitingForLogin: status = CrawlRun::Status::NeedLogin; break;
    case Outcome::Cancelled:       status = CrawlRun::Status::Cancelled; break;
    }

    QString error;
    if (d->runId > 0 && !d->repository->finishRun(d->runId, status, message, &error)) {
        qCWarning(lcSync) << "cannot finalize run record:" << error;
    }

    // 只有整轮成功时才把「全量扫描完成」标记为已证实。
    // 部分完成 / 失败 / 等登录都不得标记完成，否则下次会被误认为已有完整基线。
    if (outcome == Outcome::Success) {
        SyncState state;
        state.taskKey = SqliteRepository::courseDiscoveryTaskKey();
        state.mode = QString::fromLatin1(syncModeKey(d->mode));
        state.cursor = QStringLiteral("completed");
        state.lastSuccessAt = d->summary.finishedAt;
        state.fullScanCompleted = (d->mode == Mode::Full);
        state.updatedAt = d->summary.finishedAt;
        QString syncError;
        if (!d->repository->upsertSyncState(state, &syncError)) {
            qCWarning(lcSync) << "cannot update sync state:" << syncError;
        }
    }

    // v2：整轮成功时额外记录两件事 ——
    //   1) 完整全量扫描完成 → 写「全量对账」时间戳（定时任务据此决定何时再全量）；
    //   2) 走了快速路径 → 保存本轮待办快照，供下一轮做差异比对。
    // 只在 Success 时写：失败/部分完成时保留旧快照，下一轮还能重新发现这些变化。
    if (outcome == Outcome::Success) {
        if (d->mode == Mode::Full) {
            SyncState reconcile;
            reconcile.taskKey = SqliteRepository::fullReconcileTaskKey();
            reconcile.mode = QString::fromLatin1(syncModeKey(Mode::Full));
            reconcile.cursor = QStringLiteral("completed");
            reconcile.lastSuccessAt = d->summary.finishedAt;
            reconcile.fullScanCompleted = true;
            reconcile.updatedAt = d->summary.finishedAt;
            QString reconcileError;
            if (!d->repository->upsertSyncState(reconcile, &reconcileError)) {
                qCWarning(lcSync) << "cannot update full-reconcile marker:" << reconcileError;
            }
        }
        // 同上的双重保险：只有增量 + 快速路径这一轮才写待办基线
        if (d->mode == Mode::Incremental && d->quickDiffActive) {
            SyncState todos;
            todos.taskKey = SqliteRepository::todoSnapshotTaskKey();
            todos.mode = QString::fromLatin1(syncModeKey(Mode::Incremental));
            todos.cursor = d->quickTodoLines.join(QLatin1Char('\n'));
            todos.lastSuccessAt = d->summary.finishedAt;
            todos.fullScanCompleted = true;
            todos.updatedAt = d->summary.finishedAt;
            QString todoError;
            if (!d->repository->upsertSyncState(todos, &todoError)) {
                qCWarning(lcSync) << "cannot store todo snapshot:" << todoError;
            }
        }
    }

    if (outcome == Outcome::Success) {
        emit progressChanged(100);
    }

    // ---- 历史表保留策略（v2）----
    // 每轮结束后顺手清理过旧的运行/批次/字段级解析异常，避免长期运行后无限增长。
    {
        QString pruneError;
        const int pruned = d->repository->pruneCrawlHistory(200, 30, &pruneError);
        if (pruned < 0) {
            qCWarning(lcSync) << "cannot prune crawl history:" << pruneError;
        } else if (pruned > 0) {
            qCDebug(lcSync) << "pruned" << pruned << "old crawl history row(s)";
        }
    }

    qCInfo(lcSync) << "sync finished; outcome:" << static_cast<int>(outcome)
                   << "courses:" << d->summary.coursesSucceeded << "/" << d->summary.coursesTotal
                   << "inserted:" << d->summary.assignmentsInserted
                   << "updated:" << d->summary.assignmentsUpdated
                   << "incomplete:" << d->summary.coursesIncomplete
                   << "durationMs:" << d->summary.durationMs();

    emit statusMessage(message);
    emit runFinished(d->summary);

    d->hasCurrentCourse = false;
    d->pending.clear();
}

// ---------------------------------------------------------------------------
// 暂停 / 继续 / 取消
// ---------------------------------------------------------------------------

void SyncManager::pause()
{
    if (!d->running) {
        return;
    }
    d->pauseRequested = true;
    qCInfo(lcSync) << "pause requested; will stop after the current course snapshot";
    // 注意：当前正在进行的课程会被完整处理并提交，不会留下半个批次的中间状态。
    // 若此刻没有在途请求（例如正处于课程间隔），需要主动触发一次状态判定。
    if (!d->hasCurrentCourse) {
        QTimer::singleShot(0, this, [this]() { processNextCourse(); });
    }
}

void SyncManager::resume()
{
    if (!d->running || !d->pauseRequested) {
        return;
    }
    d->pauseRequested = false;

    if (d->sessionExpired && d->config.stopOnSessionExpired) {
        // 会话仍未恢复：不能继续，必须提示重新登录
        emit loginRequired(QStringLiteral("登录状态仍未恢复，无法继续同步"));
        finishRun(Outcome::WaitingForLogin, QStringLiteral("会话已失效，等待重新登录"));
        return;
    }

    qCInfo(lcSync) << "resuming sync";
    emit statusMessage(QStringLiteral("已继续同步…"));
    processNextCourse();
}

void SyncManager::cancel()
{
    if (!d->running) {
        return;
    }
    d->cancelRequested = true;
    qCInfo(lcSync) << "cancel requested; committed batches and checkpoints are kept";
    if (!d->hasCurrentCourse) {
        QTimer::singleShot(0, this, [this]() { processNextCourse(); });
    }
}

} // namespace njulms
