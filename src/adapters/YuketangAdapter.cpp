#include "adapters/YuketangAdapter.h"

#include "parser/YuketangParser.h"
#include "utils/Logger.h"
#include "utils/Redact.h"
#include "utils/TimeUtils.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>

namespace njulms {
namespace {

/// [实测] 所有雨课堂请求都要带的自定义头；缺它返回 "XTBZ IS REQUIRED"
constexpr auto kXtbzValue = "ykt";

/// 未登录形态（三种，实测）：HTTP 401 / {"errcode":401002} / {"code":50000,"msg":"UNAUTHENTICATED"} / web_redirect
bool looksNotLoggedIn(const IPageBridge::Response &res)
{
    if (res.httpStatus == 401) {
        return true;
    }
    const QString &body = res.body;
    return body.contains(QLatin1String("401002"))
           || body.contains(QLatin1String("UNAUTHENTICATED"))
           || body.contains(QLatin1String("web_redirect"));
}

bool isParameterError(const QString &body)
{
    // {"msg":"参数错误","error_code":20002,...}
    return body.contains(QLatin1String("20002"));
}

} // namespace

class YuketangAdapter::Impl
{
public:
    explicit Impl(YuketangAdapter *owner, IPageBridge *bridge)
        : q(owner), bridge(bridge)
    {
    }

    YuketangAdapter *q = nullptr;
    IPageBridge *bridge = nullptr;
    int maxRetries = 2;
    int requestIntervalMs = 400;

    /// 大学标识（course_sign，如 nju-…）：同一所大学的课程通用，进程内缓存
    QString signCache;

    /// 带重试的 GET（自动附加 xtbz 头）
    void get(const QString &path, std::function<void(const IPageBridge::Response &)> callback,
             int attempt = 0)
    {
        IPageBridge::Request req = IPageBridge::Request::get(path);
        req.extraHeaders.append(qMakePair(QStringLiteral("xtbz"),
                                          QString::fromLatin1(kXtbzValue)));
        bridge->request(req, [this, path, callback = std::move(callback), attempt](
                                const IPageBridge::Response &res) {
            if (!res.transportOk && attempt < maxRetries) {
                QTimer::singleShot(requestIntervalMs, q, [this, path, callback, attempt]() {
                    get(path, callback, attempt + 1);
                });
                return;
            }
            callback(res);
        });
    }

    /// 通用的"响应 → JSON 对象 + 错误分类"
    std::optional<QJsonObject> readJson(const IPageBridge::Response &res, Error &error,
                                        QStringView what) const
    {
        if (!res.transportOk) {
            error = Error::make(ErrorCode::Transport, res.error);
            return std::nullopt;
        }
        if (looksNotLoggedIn(res)) {
            error = Error::make(ErrorCode::SessionExpired, QStringLiteral("需要登录雨课堂"));
            return std::nullopt;
        }
        if (!res.hasHttpResponse() || res.httpStatus / 100 != 2) {
            error = Error::make(ErrorCode::HttpStatus, QStringLiteral("unexpected HTTP status"),
                                res.httpStatus);
            return std::nullopt;
        }
        const QJsonDocument doc = QJsonDocument::fromJson(res.body.toUtf8());
        if (!doc.isObject()) {
            error = Error::make(ErrorCode::UnexpectedShape,
                                QStringLiteral("%1 response is not a JSON object").arg(what));
            return std::nullopt;
        }
        return doc.object();
    }

    // ---- 单门课程取数的分步状态机 -------------------------------------------

    struct WorkState
    {
        Course course;
        QJsonObject chapter;
        QJsonObject schedule;
        bool signRetried = false;
        std::function<void(const WorkListResult &)> callback;
    };

    void startWork(const Course &course, std::function<void(const WorkListResult &)> callback)
    {
        auto state = std::make_shared<WorkState>();
        state->course = course;
        state->callback = std::move(callback);
        ensureSign(state, [this, state](const QString &sign) { fetchChapter(state, sign); });
    }

    /// 步骤 1：取大学标识 sign（进程内缓存；缓存失效时由 schedule 的错误码触发重取）
    void ensureSign(const std::shared_ptr<WorkState> &state,
                    std::function<void(const QString &)> onReady)
    {
        if (!signCache.isEmpty()) {
            onReady(signCache);
            return;
        }
        get(basicInfoPath(state->course.courseId), [this, state, onReady](const IPageBridge::Response &res) {
            qCDebug(lcAdapter) << "yuketang basic_info;" << "course:"
                               << redact::courseId(state->course.courseId)
                               << "bytes:" << res.body.size();
            Error error;
            const auto root = readJson(res, error, QStringLiteral("basic_info"));
            if (!root.has_value()) {
                finishWork(state, error);
                return;
            }
            const QString sign =
                root->value(QStringLiteral("data")).toObject().value(QStringLiteral("course_sign")).toString();
            if (sign.isEmpty()) {
                finishWork(state, Error::make(ErrorCode::UnexpectedShape,
                                              QStringLiteral("course_sign missing from basic_info")));
                return;
            }
            signCache = sign;
            onReady(sign);
        });
    }

    /// 步骤 2：课程大纲
    void fetchChapter(const std::shared_ptr<WorkState> &state, const QString &sign)
    {
        get(chapterPath(state->course.courseId),
            [this, state, sign](const IPageBridge::Response &res) {
                Error error;
                const auto root = readJson(res, error, QStringLiteral("chapter"));
                if (!root.has_value()) {
                    if (error.needsLogin()) {
                        emit q->sessionExpired();
                    }
                    finishWork(state, error);
                    return;
                }
                qCDebug(lcAdapter) << "yuketang chapter;" << "course:"
                                   << redact::courseId(state->course.courseId)
                                   << "bytes:" << res.body.size()
                                   << "head:" << res.body.left(140);
                state->chapter = *root;
                fetchSchedule(state, sign);
            });
    }

    /// 步骤 3：学习进度（sign 失效时重取一次再试）
    void fetchSchedule(const std::shared_ptr<WorkState> &state, const QString &sign)
    {
        get(schedulePath(state->course.courseId, sign),
            [this, state](const IPageBridge::Response &res) {
                if (res.transportOk && !state->signRetried && isParameterError(res.body)) {
                    state->signRetried = true;
                    signCache.clear();
                    ensureSign(state, [this, state](const QString &freshSign) {
                        fetchSchedule(state, freshSign);
                    });
                    return;
                }
                Error error;
                const auto root = readJson(res, error, QStringLiteral("schedule"));
                if (!root.has_value()) {
                    if (error.needsLogin()) {
                        emit q->sessionExpired();
                    }
                    finishWork(state, error);
                    return;
                }
                qCDebug(lcAdapter) << "yuketang schedule;" << "course:"
                                   << redact::courseId(state->course.courseId)
                                   << "bytes:" << res.body.size();
                state->schedule = *root;
                finishWork(state, Error::none());
            });
    }

    /// 收尾：合并两份响应 → 作业/考试条目。两份都成功才算完整快照。
    void finishWork(const std::shared_ptr<WorkState> &state, const Error &error)
    {
        WorkListResult result;
        if (!error.ok()) {
            result.error = error;
            state->callback(result);
            return;
        }

        ParseIssues issues;
        result.assignments = YuketangParser::parseCourseWork(
            state->chapter, state->schedule, state->course.courseId,
            state->course.courseId.toLongLong(), timeutil::nowUtc(), &issues);
        result.issues = issues;
        result.snapshotComplete = true;
        for (const ParseIssue &issue : std::as_const(issues)) {
            qCWarning(lcAdapter) << "yuketang issue;" << "course:"
                                 << redact::courseId(state->course.courseId)
                                 << "field:" << issue.field << "reason:" << issue.reason;
        }
        qCInfo(lcAdapter) << "yuketang: course" << redact::courseId(state->course.courseId)
                          << "items:" << result.assignments.size() << "issues:" << issues.size();
        state->callback(result);
    }
};

// ---------------------------------------------------------------------------

YuketangAdapter::YuketangAdapter(IPageBridge *bridge, QObject *parent)
    : QObject(parent), d(std::make_unique<Impl>(this, bridge))
{
}

YuketangAdapter::~YuketangAdapter() = default;

QString YuketangAdapter::coursesPath()
{
    return QStringLiteral("/v2/api/web/courses/list?identity=2");
}

QString YuketangAdapter::chapterPath(const QString &classroomId)
{
    return QStringLiteral(
               "/mooc-api/v1/lms/learn/course/chapter?cid=%1&classroom_id=%1&term=latest")
        .arg(classroomId);
}

QString YuketangAdapter::schedulePath(const QString &classroomId, const QString &sign)
{
    return QStringLiteral(
               "/mooc-api/v1/lms/learn/course/schedule?cid=%1&classroom_id=%1&sign=%2")
        .arg(classroomId, sign);
}

QString YuketangAdapter::basicInfoPath(const QString &classroomId)
{
    return QStringLiteral(
               "/c27/online_courseware/course/classroom/%1/get_basic_info/?classroom_id=%1")
        .arg(classroomId);
}

void YuketangAdapter::setMaxRetries(int retries)
{
    d->maxRetries = qMax(0, retries);
}

void YuketangAdapter::setRequestIntervalMs(int ms)
{
    d->requestIntervalMs = qMax(0, ms);
}

void YuketangAdapter::discoverCourses(std::function<void(const CourseListResult &)> callback)
{
    d->get(coursesPath(), [this, callback = std::move(callback)](
                              const IPageBridge::Response &res) mutable {
        CourseListResult result;
        const auto root = d->readJson(res, result.error, QStringLiteral("courses/list"));
        if (!root.has_value()) {
            if (result.error.needsLogin()) {
                emit sessionExpired();
            }
            callback(result);
            return;
        }
        if (root->value(QStringLiteral("errcode")).toInt(-1) != 0) {
            result.error = Error::make(
                ErrorCode::UnexpectedShape,
                QStringLiteral("courses/list errcode=%1 errmsg=%2")
                    .arg(root->value(QStringLiteral("errcode")).toInt())
                    .arg(root->value(QStringLiteral("errmsg")).toString()));
            callback(result);
            return;
        }

        ParseIssues issues;
        result.courses = YuketangParser::parseCourseList(*root, timeutil::nowUtc(), &issues);
        qCInfo(lcAdapter) << "yuketang: discovered" << result.courses.size() << "course(s)"
                          << "issues:" << issues.size();
        callback(result);
    });
}

void YuketangAdapter::fetchCourseWork(const Course &course,
                                      std::function<void(const WorkListResult &)> callback)
{
    d->startWork(course, std::move(callback));
}

} // namespace njulms
