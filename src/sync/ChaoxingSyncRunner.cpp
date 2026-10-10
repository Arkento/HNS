#include "sync/ChaoxingSyncRunner.h"

#include "repository/SqliteRepository.h"
#include "utils/TimeUtils.h"

#include <QLoggingCategory>

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
        emit q->statusMessage(QStringLiteral("学习通：正在采集「%1」（%2/%3）…")
                                  .arg(course.courseName)
                                  .arg(index + 1)
                                  .arg(courses.size()));
        adapter->fetchWorkList(course, [this, course](const ChaoxingAdapter::WorkListResult &r) {
            onWorkList(course, r);
        });
    }

    void onWorkList(const Course &course, const ChaoxingAdapter::WorkListResult &result)
    {
        if (!running) {
            return;
        }
        if (!result.error.ok()) {
            if (result.error.needsLogin()) {
                emit q->loginRequired(result.error.message);
                finish(Outcome::WaitingForLogin, result.error.message);
                return;
            }
            ++failed;
            qCInfo(lcChaoxingSync) << "course failed;" << "course:" << course.courseName
                                   << "reason:" << result.error.message;
            emit q->courseFinished(course.courseName, false, result.error.message);
            advance();
            return;
        }

        recordIssues(course.courseId, result.issues);

        // 页面读不出内容（模板改版、未登录形态等）→ 不写库、保留旧数据、计入"跳过"。
        // 明确写着"暂无作业"的空列表则继续走下面的快照写入（这会软删本课程的历史条目，
        // 是正确语义：列表是权威的，作业确实没有了）。
        bool worklistUnreadable = false;
        for (const ParseIssue &issue : result.issues) {
            if (issue.field == QLatin1String("chaoxing.worklist")
                && issue.state == FieldState::Missing) {
                worklistUnreadable = true;
            }
        }
        if (worklistUnreadable) {
            ++skipped;
            emit q->courseFinished(course.courseName, false,
                                   QStringLiteral("跳过：未解析到作业列表（保留旧数据）"));
            advance();
            return;
        }

        const BatchApplyResult applied = repo->applyAssignmentSnapshot(
            course.courseId, result.assignments, runId, 1, QString(), QString(),
            result.snapshotComplete, mode);
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
