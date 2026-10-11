#include "sync/YuketangSyncRunner.h"

#include "repository/SqliteRepository.h"
#include "utils/TimeUtils.h"

#include <QLoggingCategory>

namespace njulms {
namespace {

Q_LOGGING_CATEGORY(lcYuketang, "njulms.yuketang")

} // namespace

class YuketangSyncRunner::Impl
{
public:
    Impl(YuketangSyncRunner *owner, SqliteRepository *repository, YuketangAdapter *adapter)
        : q(owner), repo(repository), adapter(adapter)
    {
    }

    bool start(const QString &modeStr, QString *error)
    {
        if (running) {
            if (error != nullptr) {
                *error = QStringLiteral("已有雨课堂采集在运行");
            }
            return false;
        }
        QString runError;
        const qint64 id = repo->startRun(QStringLiteral("yuketang-sync"), modeStr, &runError);
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
        courses.clear();

        emit q->runStarted(runId, mode);
        emit q->statusMessage(QStringLiteral("雨课堂：正在获取课程列表…"));
        adapter->discoverCourses(
            [this](const YuketangAdapter::CourseListResult &result) { onCourses(result); });
        return true;
    }

    void cancel()
    {
        if (!running) {
            return;
        }
        finish(Outcome::Cancelled, QStringLiteral("雨课堂采集已取消"));
    }

    void onCourses(const YuketangAdapter::CourseListResult &result)
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
        if (!repo->applyCourseSnapshot(source_keys::kYuketang.toString(), result.courses,
                                       &repoError)) {
            finish(Outcome::Failed,
                   QStringLiteral("写入雨课堂课程列表失败：%1").arg(repoError));
            return;
        }

        courses = result.courses;
        index = 0;
        emit q->progressChanged(0);
        emit q->statusMessage(
            QStringLiteral("雨课堂：共 %1 门课程，开始采集作业与考试…").arg(courses.size()));
        fetchNext();
    }

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
        emit q->statusMessage(QStringLiteral("雨课堂：正在采集「%1」（%2/%3）…")
                                  .arg(course.courseName)
                                  .arg(index + 1)
                                  .arg(courses.size()));
        adapter->fetchCourseWork(
            course, [this, course](const YuketangAdapter::WorkListResult &result) {
                onWork(course, result);
            });
    }

    void onWork(const Course &course, const YuketangAdapter::WorkListResult &result)
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
            qCInfo(lcYuketang) << "course failed;" << "course:" << course.courseName
                               << "reason:" << result.error.message;
            emit q->courseFinished(course.courseName, false, result.error.message);
            advance();
            return;
        }

        recordIssues(course.courseId, result.issues);

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

    void conclude()
    {
        Outcome outcome = Outcome::Success;
        if (succeeded == 0 && failed > 0) {
            outcome = Outcome::Failed;
        } else if (failed > 0) {
            outcome = Outcome::Partial;
        }
        const QString message = QStringLiteral("雨课堂：成功 %1 门，失败 %2 门")
                                    .arg(succeeded)
                                    .arg(failed);
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
            if (issue.state != FieldState::Invalid) {
                continue;   // 与南大同步一致：只把"解析失败"写进 parse_errors
            }
            ParseErrorRecord rec;
            rec.runId = runId;
            rec.courseId = courseId;
            rec.field = issue.field;
            rec.reason = issue.reason;
            rec.observedAt = now;
            records.append(rec);
        }
        if (!records.isEmpty()) {
            repo->recordParseErrors(records);
        }
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
        case Outcome::Success:         status = CrawlRun::Status::Success; break;
        case Outcome::Partial:         status = CrawlRun::Status::Partial; break;
        case Outcome::Failed:          status = CrawlRun::Status::Failed; break;
        case Outcome::WaitingForLogin: status = CrawlRun::Status::NeedLogin; break;
        case Outcome::Cancelled:       status = CrawlRun::Status::Cancelled; break;
        }
        if (id > 0) {
            repo->finishRun(id, status, message);
        }
        emit q->runFinished(outcome, message, succeeded, failed);
    }

    YuketangSyncRunner *q = nullptr;
    SqliteRepository *repo = nullptr;
    YuketangAdapter *adapter = nullptr;

    bool running = false;
    QString mode;
    qint64 runId = 0;
    QList<Course> courses;
    int index = 0;
    int succeeded = 0;
    int failed = 0;
};

// ---------------------------------------------------------------------------

YuketangSyncRunner::YuketangSyncRunner(SqliteRepository *repository, YuketangAdapter *adapter,
                                       QObject *parent)
    : QObject(parent), d(std::make_unique<Impl>(this, repository, adapter))
{
}

YuketangSyncRunner::~YuketangSyncRunner() = default;

bool YuketangSyncRunner::isRunning() const
{
    return d->running;
}

qint64 YuketangSyncRunner::currentRunId() const
{
    return d->runId;
}

bool YuketangSyncRunner::start(const QString &mode, QString *error)
{
    return d->start(mode, error);
}

void YuketangSyncRunner::cancel()
{
    d->cancel();
}

} // namespace njulms
