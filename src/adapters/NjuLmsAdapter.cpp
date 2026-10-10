#include "NjuLmsAdapter.h"

#include "parser/AssignmentParser.h"
#include "utils/Logger.h"
#include "utils/Redact.h"
#include "utils/TimeUtils.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>
#include <QUrl>

#include <algorithm>
#include <utility>

namespace njulms {
namespace {

/// [实测] 本人课程列表接口
constexpr auto kMyCoursesPath = "/api/my-courses";
/// [实测] 作业列表接口。page_size 实测可调，这里一次取全量。
constexpr auto kHomeworkActivitiesPath = "/api/courses/%1/homework-activities";
/// [实测] 课程活动合集接口。⚠️ 实测响应不含任何分页元数据。
constexpr auto kActivitiesPath = "/api/courses/%1/activities";
/// [实测] 响应中作业数组的键名
constexpr auto kHomeworkActivitiesKey = "homework_activities";
/// [实测] 响应中活动数组的键名
constexpr auto kActivitiesKey = "activities";
/// [实测] 分页元数据键名
constexpr auto kPageSizeKey = "page_size";
constexpr auto kTotalKey = "total";
constexpr auto kPagesKey = "pages";

/// 翻页安全上限：服务端分页元数据异常时避免无限请求。
/// 按每页 200 条计算，100 页足以覆盖任何真实课程。
constexpr int kMaxPages = 100;

/// 一次请求取全量的 page_size。实测 200 被接受。若服务端夹取该值，
/// total 校验会失败并把 snapshotComplete 置为 false，断点不会推进。
constexpr int kFullSnapshotPageSize = 200;

/// 同一站点请求之间的最小间隔，延续既有原型的克制频率
constexpr int kDefaultRequestIntervalMs = 400;
constexpr int kDefaultMaxRetries = 3;

} // namespace

// ---------------------------------------------------------------------------
// Impl
// ---------------------------------------------------------------------------

class NjuLmsAdapter::Impl
{
public:
    IPageBridge *bridge = nullptr;
    int maxRetries = kDefaultMaxRetries;
    int requestIntervalMs = kDefaultRequestIntervalMs;
};

// ---------------------------------------------------------------------------
// 构造
// ---------------------------------------------------------------------------

NjuLmsAdapter::NjuLmsAdapter(IPageBridge *bridge, QObject *parent)
    : QObject(parent), d(std::make_unique<Impl>())
{
    d->bridge = bridge;
    if (d->bridge != nullptr) {
        connect(d->bridge, &IPageBridge::becameUnavailable, this, [this](const QString &reason) {
            qCWarning(lcAdapter) << "page bridge became unavailable:" << reason;
        });
    }
}

NjuLmsAdapter::~NjuLmsAdapter() = default;

QString NjuLmsAdapter::myCoursesPath()
{
    return QString::fromLatin1(kMyCoursesPath);
}

QString NjuLmsAdapter::todosPath()
{
    // [实测 2026-10-09] GET /api/todos → {"todo_list":[...]}
    return QStringLiteral("/api/todos");
}

void NjuLmsAdapter::setMaxRetries(int retries)
{
    d->maxRetries = qBound(0, retries, 10);
}

void NjuLmsAdapter::setRequestIntervalMs(int ms)
{
    d->requestIntervalMs = qBound(0, ms, 60000);
}

// ---------------------------------------------------------------------------
// 内部：带重试的请求执行
// ---------------------------------------------------------------------------

namespace {

/// 判断响应是否表示会话失效
bool looksLikeLoginPage(const IPageBridge::Response &res)
{
    // 站点会话失效的典型表现：返回登录相关主机或 HTML。此处只做形状判断，
    // 具体分类交给 AssignmentParser::inspectEnvelope 统一处理。
    const QString body = res.body.trimmed();
    if (body.isEmpty()) {
        return false;
    }
    if (body.startsWith(QLatin1Char('<'))) {
        return true;
    }
    return body.contains(QLatin1String("authserver"), Qt::CaseInsensitive)
           || body.contains(QLatin1String("keycloak"), Qt::CaseInsensitive)
           || body.contains(QLatin1String("id=\"pwdFromId\""), Qt::CaseInsensitive);
}

} // namespace

/// 带有限次重试与递增等待的请求（需求场景 D：有限次重试 + 逐渐增加的等待时间）。
///
/// 实现说明：重试需要「等一会儿再发一次」，因此必须把状态放在堆上并用
/// shared_ptr 保活——若把重试函数放在栈上，QTimer 回调触发时它已经析构。
static void requestWithRetry(NjuLmsAdapter *self, IPageBridge *bridge, int maxRetries,
                             int backoffBase, IPageBridge::Request req,
                             std::function<void(const IPageBridge::Response &)> done)
{
    struct RetryState
    {
        int attempt = 0;
        std::function<void()> run;  ///< 由 shared_ptr 持有，保证在 QTimer 回调期间存活
    };

    auto state = std::make_shared<RetryState>();

    state->run = [self, bridge, maxRetries, backoffBase, req, done, state]() {
        bridge->request(req, [self, bridge, maxRetries, backoffBase, req, done, state](
                                  const IPageBridge::Response &res) {
            const bool transportFailure = !res.transportOk;
            const bool serverError = res.hasHttpResponse() && res.httpStatus >= 500;
            const bool retryable = transportFailure || serverError;

            if (!retryable || state->attempt >= maxRetries) {
                done(res);
                return;
            }

            ++state->attempt;
            const int delay = qMin(backoffBase * (1 << qBound(0, state->attempt - 1, 5)), 8000);
            qCWarning(lcAdapter) << "request failed, retry" << state->attempt << "of" << maxRetries
                                 << "in" << delay << "ms;"
                                 << "path:" << redact::url(IPageBridge::Request(req).path)
                                 << "status:" << res.httpStatus;
            emit self->retrying(state->attempt, maxRetries,
                                transportFailure ? QStringLiteral("transport failure")
                                                 : QStringLiteral("HTTP %1").arg(res.httpStatus));

            // state 被捕获，保证重试函数在定时器触发时仍然有效
            QTimer::singleShot(delay, self, [state]() { state->run(); });
        });
    };

    state->run();
}

// ---------------------------------------------------------------------------
// checkSession
// ---------------------------------------------------------------------------

void NjuLmsAdapter::checkSession(std::function<void(const SessionCheck &)> callback)
{
    SessionCheck result;

    if (d->bridge == nullptr || !d->bridge->isReady()) {
        result.error = Error::make(ErrorCode::Transport, QStringLiteral("page bridge is not ready"));
        callback(result);
        return;
    }

    // 复用课程列表请求作为会话探针：它是最轻量的、且必然需要认证的接口。
    auto req = IPageBridge::Request::post(myCoursesPath(), QStringLiteral("{}"));

    requestWithRetry(this, d->bridge, d->maxRetries, d->requestIntervalMs, req,
                     [this, callback, result](const IPageBridge::Response &res) mutable {
        if (!res.transportOk) {
            result.error = Error::make(ErrorCode::Transport, res.error);
            callback(result);
            return;
        }
        if (res.httpStatus == 401 || res.httpStatus == 403) {
            result.error = Error::make(ErrorCode::SessionExpired,
                                       QStringLiteral("server rejected the session"),
                                       res.httpStatus);
            emit sessionExpired();
            callback(result);
            return;
        }
        if (!res.hasHttpResponse() || res.httpStatus / 100 != 2) {
            result.error = Error::make(ErrorCode::HttpStatus,
                                       QStringLiteral("unexpected HTTP status"),
                                       res.httpStatus);
            callback(result);
            return;
        }

        // 关键判定：必须是可解析的 JSON，而不是登录页 HTML。
        // 会话失效时站点可能返回 200 + HTML，若按状态码判定就会把登录页
        // 当成「空的作业列表」，进而用空值覆盖库中有效数据（需求场景 C）。
        const ParseOutcome envelope = AssignmentParser::inspectEnvelope(res.body);
        if (!envelope.ok()) {
            result.error = Error::make(ErrorCode::SessionExpired, envelope.detail);
            if (looksLikeLoginPage(res)) {
                qCWarning(lcAuth) << "session appears expired: response is not JSON";
            } else {
                qCWarning(lcAuth) << "response is not JSON (possible session issue):"
                                  << envelope.detail;
            }
            emit sessionExpired();
            callback(result);
            return;
        }

        const QJsonObject root = QJsonDocument::fromJson(res.body.toUtf8()).object();
        if (!root.contains(QStringLiteral("courses"))) {
            // 是 JSON 但没有 courses —— 结构不符，不得当作「0 门课程」
            result.error = Error::make(ErrorCode::UnexpectedShape,
                                       QStringLiteral("JSON response has no 'courses' key"));
            callback(result);
            return;
        }

        result.authenticated = true;
        result.courseCount = root.value(QStringLiteral("courses")).toArray().size();
        qCInfo(lcAuth) << "session check ok, courses:" << result.courseCount;
        callback(result);
    });
}

// ---------------------------------------------------------------------------
// discoverCourses
// ---------------------------------------------------------------------------

void NjuLmsAdapter::discoverCourses(std::function<void(const CourseListResult &)> callback)
{
    CourseListResult result;

    if (d->bridge == nullptr || !d->bridge->isReady()) {
        result.error = Error::make(ErrorCode::Transport, QStringLiteral("page bridge is not ready"));
        callback(result);
        return;
    }

    // [实测] body 为 {} 即可返回本人全部课程，无需复现前端的 fields/conditions 过滤器。
    auto req = IPageBridge::Request::post(myCoursesPath(), QStringLiteral("{}"));

    requestWithRetry(this, d->bridge, d->maxRetries, d->requestIntervalMs, req,
                     [this, callback, result](const IPageBridge::Response &res) mutable {
        if (!res.transportOk) {
            result.error = Error::make(ErrorCode::Transport, res.error);
            callback(result);
            return;
        }
        if (!res.hasHttpResponse() || res.httpStatus / 100 != 2) {
            result.error = Error::make(ErrorCode::HttpStatus,
                                       QStringLiteral("unexpected HTTP status"), res.httpStatus);
            callback(result);
            return;
        }

        const ParseOutcome envelope = AssignmentParser::inspectEnvelope(res.body);
        if (!envelope.ok()) {
            result.error = Error::make(ErrorCode::SessionExpired, envelope.detail);
            emit sessionExpired();
            callback(result);
            return;
        }

        const QJsonObject root = QJsonDocument::fromJson(res.body.toUtf8()).object();
        ParseIssues issues;
        const QDateTime observedAt = timeutil::nowUtc();
        result.courses = AssignmentParser::parseCourseList(root, observedAt, &issues);

        if (result.courses.isEmpty() && root.value(QStringLiteral("courses")).toArray().isEmpty()) {
            // 确实一门课程都没有：这是合法状态（例如新生尚未选课），不是错误，
            // 但必须与「会话失效」严格区分——此处已确认是可解析 JSON 且含空数组。
            qCInfo(lcAdapter) << "course list is genuinely empty";
        }

        qCInfo(lcAdapter) << "discovered" << result.courses.size() << "course(s)";
        callback(result);
    });
}

// ---------------------------------------------------------------------------
// fetchTodos（v2：增量同步的「变更信号」）
// ---------------------------------------------------------------------------

void NjuLmsAdapter::fetchTodos(std::function<void(const TodoListResult &)> callback)
{
    TodoListResult result;

    if (d->bridge == nullptr || !d->bridge->isReady()) {
        result.error = Error::make(ErrorCode::Transport,
                                   QStringLiteral("page bridge is not ready"));
        result.supported = false;
        callback(result);
        return;
    }

    auto req = IPageBridge::Request::get(todosPath());

    requestWithRetry(this, d->bridge, d->maxRetries, d->requestIntervalMs, req,
                     [this, callback, result](const IPageBridge::Response &res) mutable {
        if (!res.transportOk) {
            result.error = Error::make(ErrorCode::Transport, res.error);
            result.supported = false;
            callback(result);
            return;
        }
        if (res.httpStatus == 401 || res.httpStatus == 403) {
            result.error = Error::make(ErrorCode::SessionExpired,
                                       QStringLiteral("server rejected the session"),
                                       res.httpStatus);
            result.supported = false;
            emit sessionExpired();
            callback(result);
            return;
        }
        if (!res.hasHttpResponse() || res.httpStatus / 100 != 2) {
            // 接口不存在/被下线：这不是"没有待办"，而是"这个优化不可用"，
            // 调用方必须回退到完整扫描。
            result.error = Error::make(ErrorCode::Unsupported,
                                       QStringLiteral("todo endpoint unavailable"),
                                       res.httpStatus);
            result.supported = false;
            qCWarning(lcAdapter) << "todo endpoint unavailable; status:" << res.httpStatus;
            callback(result);
            return;
        }

        const ParseOutcome envelope = AssignmentParser::inspectEnvelope(res.body);
        if (!envelope.ok()) {
            result.error = Error::make(ErrorCode::SessionExpired, envelope.detail);
            result.supported = false;
            emit sessionExpired();
            callback(result);
            return;
        }

        const QJsonObject root = QJsonDocument::fromJson(res.body.toUtf8()).object();
        if (!root.contains(QStringLiteral("todo_list"))) {
            result.error = Error::make(ErrorCode::UnexpectedShape,
                                       QStringLiteral("JSON response has no 'todo_list' key"));
            result.supported = false;
            callback(result);
            return;
        }

        const QJsonArray items = root.value(QStringLiteral("todo_list")).toArray();
        result.items.reserve(items.size());
        for (const QJsonValue &value : items) {
            if (!value.isObject()) {
                continue;
            }
            const QJsonObject object = value.toObject();
            TodoItem item;
            item.courseId = object.value(QStringLiteral("course_id")).toVariant().toString();
            item.activityId = object.value(QStringLiteral("id")).toVariant().toString();
            item.type = object.value(QStringLiteral("type")).toString();
            if (const auto parsed =
                    timeutil::parseSiteTime(object.value(QStringLiteral("end_time")).toString());
                parsed.has_value()) {
                item.endTime = *parsed;
            }
            if (item.courseId.isEmpty() || item.activityId.isEmpty()) {
                continue;
            }
            result.items.append(item);
        }

        qCInfo(lcAdapter) << "todo list fetched; items:" << result.items.size();
        callback(result);
    });
}

// ---------------------------------------------------------------------------
// fetchAssignments
// ---------------------------------------------------------------------------

void NjuLmsAdapter::fetchAssignments(const QString &courseId,
                                     std::function<void(const AssignmentListResult &)> callback)
{
    AssignmentListResult result;

    if (courseId.isEmpty()) {
        result.error = Error::make(ErrorCode::UnexpectedShape, QStringLiteral("courseId is empty"));
        callback(result);
        return;
    }
    if (d->bridge == nullptr || !d->bridge->isReady()) {
        result.error = Error::make(ErrorCode::Transport, QStringLiteral("page bridge is not ready"));
        callback(result);
        return;
    }

    // [实测] GET /api/courses/{id}/homework-activities?page=1&page_size=N
    // 优先一次取全量（把分页降级为单次快照）；若服务端夹取了 page_size，
    // 则自动继续翻页，最后按 total 做完整性校验（需求第四节场景 H）。
    fetchAssignmentsPage(courseId, /*page=*/1, std::make_shared<AssignmentListResult>(),
                         std::move(callback));
}

void NjuLmsAdapter::fetchAssignmentsPage(const QString &courseId, int page,
                                         std::shared_ptr<AssignmentListResult> accumulated,
                                         std::function<void(const AssignmentListResult &)> callback)
{
    if (d->bridge == nullptr || !d->bridge->isReady()) {
        accumulated->error = Error::make(ErrorCode::Transport,
                                         QStringLiteral("page bridge is not ready"));
        callback(*accumulated);
        return;
    }

    const QString path = QString::fromLatin1(kHomeworkActivitiesPath).arg(courseId)
                         + QStringLiteral("?page=%1&page_size=%2")
                               .arg(page)
                               .arg(kFullSnapshotPageSize);
    auto req = IPageBridge::Request::get(path);

    requestWithRetry(
        this, d->bridge, d->maxRetries, d->requestIntervalMs, req,
        [this, callback, accumulated, courseId, page](const IPageBridge::Response &res) mutable {
            if (!res.transportOk) {
                accumulated->error = Error::make(ErrorCode::Transport, res.error);
                callback(*accumulated);
                return;
            }
            if (!res.hasHttpResponse() || res.httpStatus / 100 != 2) {
                accumulated->error = Error::make(ErrorCode::HttpStatus,
                                                 QStringLiteral("unexpected HTTP status"),
                                                 res.httpStatus);
                callback(*accumulated);
                return;
            }

            const ParseOutcome envelope = AssignmentParser::inspectEnvelope(res.body);
            if (!envelope.ok()) {
                accumulated->error = Error::make(ErrorCode::SessionExpired, envelope.detail);
                emit sessionExpired();
                callback(*accumulated);
                return;
            }

            const QJsonObject root = QJsonDocument::fromJson(res.body.toUtf8()).object();
            const QString itemsKey = QString::fromLatin1(kHomeworkActivitiesKey);
            if (!root.contains(itemsKey)) {
                accumulated->error = Error::make(
                    ErrorCode::UnexpectedShape,
                    QStringLiteral("JSON response has no '%1' key").arg(itemsKey));
                callback(*accumulated);
                return;
            }

            // 分页元数据（实测字段：page / page_size / pages / total）
            const int reportedTotal = root.value(QString::fromLatin1(kTotalKey)).toInt(-1);
            const int reportedPages = root.value(QString::fromLatin1(kPagesKey)).toInt(-1);

            if (reportedTotal >= 0) {
                accumulated->reportedTotal = reportedTotal;
            }

            const QJsonArray items = root.value(itemsKey).toArray();
            const QDateTime observedAt = timeutil::nowUtc();

            // ⚠️ 每次都重新解析本页并**按稳定标识合并**，而不是简单累加。
            // 原因：翻页期间列表若发生插入/删除，同一条目可能出现在两页里
            // （重复），也可能前移到已经取过的页（漏项）。按 sourceId 去重可以
            // 化解重复；漏项则由最后的 total 校验兜住 —— 数量对不上就不算完整。
            const QList<Assignment> pageItems =
                AssignmentParser::parseAssignments(courseId, items, observedAt,
                                                   &accumulated->issues);
            for (const Assignment &a : pageItems) {
                if (a.sourceId.isEmpty()) {
                    continue;
                }
                const auto existing = std::find_if(
                    accumulated->assignments.begin(), accumulated->assignments.end(),
                    [&a](const Assignment &other) { return other.sourceId == a.sourceId; });
                if (existing == accumulated->assignments.end()) {
                    accumulated->assignments.append(a);
                } else {
                    *existing = a;  // 以最后一次观察到的为准
                }
            }

            // 是否还有下一页？
            const bool morePagesByMetadata = reportedPages > 0 && page < reportedPages;
            const bool morePagesByCount =
                reportedTotal > 0 && accumulated->assignments.size() < reportedTotal
                && items.size() == kFullSnapshotPageSize;
            const bool needNextPage = morePagesByMetadata || morePagesByCount;

            if (needNextPage) {
                if (page >= kMaxPages) {
                    // 安全上限：避免服务端元数据异常时无限翻页
                    accumulated->paginated = true;
                    accumulated->snapshotComplete = false;
                    qCWarning(lcAdapter) << "stopping pagination at page" << page
                                         << "for course" << redact::courseId(courseId)
                                         << "(safety limit reached)";
                    callback(*accumulated);
                    return;
                }
                accumulated->paginated = true;
                fetchAssignmentsPage(courseId, page + 1, accumulated, std::move(callback));
                return;
            }

            // ---- 完整性校验：只有能证明「本次拿到了全部条目」才允许断点推进 ----
            const int mergedCount = accumulated->assignments.size();
            if (accumulated->reportedTotal < 0) {
                // 服务端没给 total，无法证明完整
                accumulated->snapshotComplete = false;
            } else if (mergedCount == accumulated->reportedTotal) {
                accumulated->snapshotComplete = true;
            } else {
                // 去重后仍与 total 不符：可能列表在翻页期间发生了变化，或服务端语义不同。
                // 保守处理：不声称完整，断点不推进，下次同步会重试整门课。
                accumulated->snapshotComplete = false;
                qCWarning(lcAdapter) << "snapshot incomplete for course"
                                     << redact::courseId(courseId) << "- merged" << mergedCount
                                     << "of reported total" << accumulated->reportedTotal
                                     << "across" << page << "page(s)";
            }

            qCInfo(lcAdapter) << "course" << redact::courseId(courseId) << "assignments parsed:"
                              << mergedCount << "reported total:" << accumulated->reportedTotal
                              << "pages fetched:" << page
                              << "issues:" << accumulated->issues.size()
                              << "complete:" << accumulated->snapshotComplete;

            // 注意：这里**不再**额外请求 /activities 做交叉校验。
            // 权衡结论：那会为每门课多打一次请求（需求第七节明确要求频率克制），
            // 而换来的只是「用站点原生 unique_key 替代推导值」——
            // 推导规则 {type}-{id} 已实测 6/6 完全吻合，风险极低。
            // 完整性仍由本接口自己的 total 权威判定。
            callback(*accumulated);
        });
}

// ---------------------------------------------------------------------------
// fetchCourseTasks / fetchAssignmentDetail — 未验证，明确不实现
// ---------------------------------------------------------------------------

void NjuLmsAdapter::fetchCourseTasks(const QString &courseId,
                                     std::function<void(const CourseTaskResult &)> callback)
{
    CourseTaskResult result;

    if (courseId.isEmpty()) {
        result.error = Error::make(ErrorCode::UnexpectedShape, QStringLiteral("courseId is empty"));
        callback(result);
        return;
    }
    if (d->bridge == nullptr || !d->bridge->isReady()) {
        result.error = Error::make(ErrorCode::Transport, QStringLiteral("page bridge is not ready"));
        callback(result);
        return;
    }

    // [实测] GET /api/courses/{id}/activities → {"activities":[...]}，无分页元数据。
    auto req = IPageBridge::Request::get(
        QString::fromLatin1(kActivitiesPath).arg(courseId));

    requestWithRetry(this, d->bridge, d->maxRetries, d->requestIntervalMs, req,
                     [this, callback, result, courseId](const IPageBridge::Response &res) mutable {
        if (!res.transportOk) {
            result.error = Error::make(ErrorCode::Transport, res.error);
            callback(result);
            return;
        }
        if (!res.hasHttpResponse() || res.httpStatus / 100 != 2) {
            result.error = Error::make(ErrorCode::HttpStatus,
                                       QStringLiteral("unexpected HTTP status"), res.httpStatus);
            callback(result);
            return;
        }

        const ParseOutcome envelope = AssignmentParser::inspectEnvelope(res.body);
        if (!envelope.ok()) {
            result.error = Error::make(ErrorCode::SessionExpired, envelope.detail);
            emit sessionExpired();
            callback(result);
            return;
        }

        const QJsonObject root = QJsonDocument::fromJson(res.body.toUtf8()).object();
        const QString itemsKey = QString::fromLatin1(kActivitiesKey);
        if (!root.contains(itemsKey)) {
            result.error = Error::make(
                ErrorCode::UnexpectedShape,
                QStringLiteral("JSON response has no '%1' key").arg(itemsKey));
            callback(result);
            return;
        }

        // 只提取 homework 类型条目；其余类型（exam / courseware / forum …）尚未纳入范围。
        QJsonArray homeworkItems;
        const QJsonArray all = root.value(itemsKey).toArray();
        for (const QJsonValue &v : all) {
            if (!v.isObject()) {
                continue;
            }
            const QString type = v.toObject().value(QStringLiteral("type")).toString();
            if (type == QLatin1String("homework")) {
                homeworkItems.append(v);
            } else {
                ++result.skippedOtherTypes;
            }
        }

        const QDateTime observedAt = timeutil::nowUtc();
        result.tasks = AssignmentParser::parseAssignments(courseId, homeworkItems, observedAt,
                                                          &result.issues);
        result.homeworkCount = result.tasks.size();

        // 该接口实测没有任何分页字段，因此无法证明完整性（需求断点规则：
        // 不确定是否完整的批次不能标记成功）。恒为 false，调用方不得据此推进断点。
        result.snapshotComplete = false;

        qCInfo(lcAdapter) << "course" << redact::courseId(courseId) << "activities: homework"
                          << result.homeworkCount << "skipped" << result.skippedOtherTypes
                          << "other type(s); completeness not provable (no paging metadata)";
        callback(result);
    });
}

void NjuLmsAdapter::fetchAssignmentDetail(const QString &courseId, const QString &assignmentId,
                                          std::function<void(const AssignmentListResult &)> callback)
{
    Q_UNUSED(courseId)
    Q_UNUSED(assignmentId)
    AssignmentListResult result;
    result.error = Error::make(
        ErrorCode::Unsupported,
        QStringLiteral("fetchAssignmentDetail is not implemented: no verified per-assignment "
                       "detail endpoint exists. All required fields are present in the list "
                       "response, so this call is currently unnecessary."));
    qCWarning(lcAdapter) << "fetchAssignmentDetail called but not implemented (no verified endpoint)";
    callback(result);
}

} // namespace njulms
