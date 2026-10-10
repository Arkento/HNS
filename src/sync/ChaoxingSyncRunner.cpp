#include "sync/ChaoxingSyncRunner.h"

#include "repository/SqliteRepository.h"
#include "utils/TimeUtils.h"

#include <QLoggingCategory>

#include <algorithm>

namespace njulms {
namespace {

Q_LOGGING_CATEGORY(lcChaoxingSync, "njulms.chaoxing")

} // namespace

class ChaoxingSyncRunner::Impl
{
public:
    Impl(ChaoxingSyncRunner *owner, SqliteRepository *repository, ChaoxingAdapter *adapter)
        : q(owner), repo(repository), adapter(adapter)
    {
    }

    // ---- 运行入口 ----------------------------------------------------------

    bool start(const QString &modeStr, QString *error)
    {
        if (running) {
            if (error != nullptr) {
                *error = QStringLiteral("已有学习通采集在运行");
            }
            return false;
        }
        QString runError;
        const qint64 id = repo->startRun(QStringLiteral("chaoxing-sync"), modeStr, &runError);
        if (id <= 0) {
            if (error != nullptr) {
                *error = runError.isEmpty() ? QStringLiteral("无法创建采集运行记录") : runError;
            }
            return false;
        }

        runId = id;
        mode = modeStr;
        running = true;
        index = 0;
        succeeded = 0;
        failed = 0;
        skipped = 0;
        courses.clear();

        emit q->runStarted(runId, mode);
        emit q->statusMessage(QStringLiteral("学习通：正在获取课程列表…"));
        adapter->discoverCourses(
            [this](const ChaoxingAdapter::CourseListResult &result) { onCourses(result); });
        return true;
    }

    void cancel()
    {
        if (!running) {
            return;
        }
        adapter->cancel();
        finish(Outcome::Cancelled, QStringLiteral("学习通采集已取消"));
    }

    // ---- 课程列表 ----------------------------------------------------------

    void onCourses(const ChaoxingAdapter::CourseListResult &result)
    {
        if (!running) {
            return;
        }
        if (!result.error.ok()) {
            if (result.error.needsLogin()) {
                emit q->loginRequired(result.error.message);
                finish(Outcome::WaitingForLogin, result.error.message);
            } else {
                finish(Outcome::Failed, result.error.message);
            }
            return;
        }

        QString repoError;
        if (!repo->applyCourseSnapshot(source_keys::kChaoxing.toString(), result.courses,
                                       &repoError)) {
            finish(Outcome::Failed,
                   QStringLiteral("写入学习通课程列表失败：%1").arg(repoError));
            return;
        }

        courses = result.courses;
        index = 0;
        emit q->progressChanged(0);
        emit q->statusMessage(
            QStringLiteral("学习通：共 %1 门课程，开始采集作业…").arg(courses.size()));
        fetchNext();
    }

    // ---- 逐门课程 ----------------------------------------------------------

    void fetchNext()
    {
        if (!running) {
            return;
        }
        if (index >= courses.size()) {
            conclude();
            return;
        }
        const Course course = courses.at(index);
        emit q->statusMessage(QStringLiteral("学习通：正在采集「%1」（%2/%3）：作业…")
                                  .arg(course.courseName)
                                  .arg(index + 1)
                                  .arg(courses.size()));
        adapter->fetchWorkList(course, [this, course](const ChaoxingAdapter::ListResult &work) {
            if (!running) {
                return;
            }
            if (!work.error.ok()) {
                if (work.error.needsLogin()) {
                    emit q->loginRequired(work.error.message);
                    finish(Outcome::WaitingForLogin, work.error.message);
                    return;
                }
                ++failed;
                qCInfo(lcChaoxingSync) << "course failed;" << "course:" << course.courseName
                                       << "reason:" << work.error.message;
                emit q->courseFinished(course.courseName, false, work.error.message);
                advance();
                return;
            }

            // 作业成功（或作业入口缺失但这个结果本身有效）→ 再取考试列表
            emit q->statusMessage(QStringLiteral("学习通：正在采集「%1」（%2/%3）：考试…")
                                      .arg(course.courseName)
                                      .arg(index + 1)
                                      .arg(courses.size()));
            adapter->fetchExamList(
                course, [this, course, work](const ChaoxingAdapter::ListResult &exam) {
                    onLists(course, work, exam);
                });
        });
    }

    /// 判断一份列表是否是"读不出内容"（模板改版/未登录形态；权威空列表不算）
    static bool listUnreadable(const ChaoxingAdapter::ListResult &result, const char *field)
    {
        for (const ParseIssue &issue : result.issues) {
            if (issue.field == QLatin1String(field) && issue.state == FieldState::Missing) {
                return true;
            }
        }
        return false;
    }

    /**
     * 作业 + 考试都拿到之后的收尾：
     *   - 任一侧会话失效 → 等待登录；
     *   - 两份都读不出 → 跳过（保留旧数据）；
     *   - 否则合并写入；只有两份都完整时才允许软删除。
     */
    void onLists(const Course &course, const ChaoxingAdapter::ListResult &work,
                 const ChaoxingAdapter::ListResult &exam)
    {
        if (!running) {
            return;
        }
        if (exam.error.needsLogin()) {
            emit q->loginRequired(exam.error.message);
            finish(Outcome::WaitingForLogin, exam.error.message);
            return;
        }

        recordIssues(course.courseId, work.issues);
        recordIssues(course.courseId, exam.issues);

        // 硬错误（传输/超时等）落一条解析异常，便于排查
        if (!exam.error.ok()) {
            recordIssues(course.courseId,
                         ParseIssues{ParseIssue(
                             QStringLiteral("chaoxing.examlist"),
                             QStringLiteral("exam list error: %1").arg(exam.error.message),
                             FieldState::Invalid)});
        }

        const bool workReadable = !listUnreadable(work, "chaoxing.worklist");
        const bool examReadable = exam.error.ok() && !listUnreadable(exam, "chaoxing.examlist");

        if (!workReadable && !examReadable) {
            // 两份列表都读不出内容 → 跳过该课程（保留旧数据）
            ++skipped;
            emit q->courseFinished(course.courseName, false,
                                   QStringLiteral("跳过：作业与考试列表均未解析到（保留旧数据）"));
            advance();
            return;
        }

        // 合并：考试条目直接追加（sourceId 前缀 cx-exam-，与 cx-work- 天然不冲突，仍去重防御）
        QList<Assignment> merged = work.assignments;
        for (const Assignment &examItem : exam.assignments) {
            const auto duplicate = std::find_if(
                merged.cbegin(), merged.cend(), [&examItem](const Assignment &existing) {
                    return existing.sourceId == examItem.sourceId;
                });
            if (duplicate == merged.cend()) {
                merged.append(examItem);
            }
        }

        // 只有两份都读到且都完整时，才允许软删除对账
        const bool complete = workReadable && examReadable && work.snapshotComplete
                              && exam.snapshotComplete;

        const BatchApplyResult applied = repo->applyAssignmentSnapshot(
            course.courseId, merged, runId, 1, QString(), QString(), complete, mode);
        if (applied.ok) {
            ++succeeded;
            emit q->courseFinished(course.courseName, true, QString());
        } else {
            ++failed;
            emit q->courseFinished(course.courseName, false, applied.error);
        }
        advance();
    }

    void advance()
    {
        ++index;
        emit q->progressChanged(courses.isEmpty() ? 100
                                                  : static_cast<int>(index * 100 / courses.size()));
        fetchNext();
    }

    // ---- 收尾 --------------------------------------------------------------

    void conclude()
    {
        Outcome outcome = Outcome::Success;
        if (succeeded == 0 && (failed > 0 || skipped > 0)) {
            outcome = Outcome::Failed;
        } else if (failed > 0 || skipped > 0) {
            outcome = Outcome::Partial;
        }
        const QString message = QStringLiteral("学习通：成功 %1 门，失败 %2 门，跳过 %3 门")
                                    .arg(succeeded)
                                    .arg(failed)
                                    .arg(skipped);
        finish(outcome, message);
    }

    void recordIssues(const QString &courseId, const ParseIssues &issues)
    {
        if (issues.isEmpty()) {
            return;
        }
        const QDateTime now = timeutil::nowUtc();
        QList<ParseErrorRecord> records;
        records.reserve(issues.size());
        for (const ParseIssue &issue : issues) {
            ParseErrorRecord rec;
            rec.runId = runId;
            rec.courseId = courseId;
            rec.field = issue.field;
            rec.reason = issue.reason;
            rec.observedAt = now;
            records.append(rec);
        }
        repo->recordParseErrors(records);
    }

    void finish(Outcome outcome, const QString &message)
    {
        if (!running) {
            return;
        }
        running = false;
        const qint64 id = runId;
        runId = 0;

        CrawlRun::Status status = CrawlRun::Status::Failed;
        switch (outcome) {
        case Outcome::Success:          status = CrawlRun::Status::Success; break;
        case Outcome::Partial:          status = CrawlRun::Status::Partial; break;
        case Outcome::Failed:           status = CrawlRun::Status::Failed; break;
        case Outcome::WaitingForLogin:  status = CrawlRun::Status::NeedLogin; break;
        case Outcome::Cancelled:        status = CrawlRun::Status::Cancelled; break;
        }
        if (id > 0) {
            repo->finishRun(id, status, message);
        }
        emit q->runFinished(outcome, message, succeeded, failed, skipped);
    }

    ChaoxingSyncRunner *q = nullptr;
    SqliteRepository *repo = nullptr;
    ChaoxingAdapter *adapter = nullptr;

    bool running = false;
    QString mode;
    qint64 runId = 0;
    QList<Course> courses;
    int index = 0;
    int succeeded = 0;
    int failed = 0;
    int skipped = 0;
};

// ---------------------------------------------------------------------------

ChaoxingSyncRunner::ChaoxingSyncRunner(SqliteRepository *repository, ChaoxingAdapter *adapter,
                                       QObject *parent)
    : QObject(parent), d(std::make_unique<Impl>(this, repository, adapter))
{
}

ChaoxingSyncRunner::~ChaoxingSyncRunner() = default;

bool ChaoxingSyncRunner::isRunning() const
{
    return d->running;
}

qint64 ChaoxingSyncRunner::currentRunId() const
{
    return d->runId;
}

bool ChaoxingSyncRunner::start(const QString &mode, QString *error)
{
    return d->start(mode, error);
}

void ChaoxingSyncRunner::cancel()
{
    d->cancel();
}

} // namespace njulms
