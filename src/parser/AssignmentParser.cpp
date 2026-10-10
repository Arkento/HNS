#include "AssignmentParser.h"

#include "utils/TimeUtils.h"

#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QJsonValue>
#include <QStringView>

#include <optional>

namespace njulms {
namespace {

/**
 * 实测的站点字段名（2026-10-09 本机实测，见 v1/docs/00-可行性评估.md）。
 *
 * 集中定义便于站点改版时一处修改。**不允许**在此添加未经实测的字段名。
 *
 * 这里用 constexpr QStringView 而不是 const char*：
 * QJsonObject::value() 接受 QStringView，因此调用点无需再做
 * QString::fromLatin1 转换；同时避免每次解析都构造临时 QString。
 */
namespace field {

constexpr QStringView kCourses = u"courses";
constexpr QStringView kId = u"id";
constexpr QStringView kName = u"name";
constexpr QStringView kUrl = u"url";
constexpr QStringView kCourseCode = u"course_code";
constexpr QStringView kDepartment = u"department";
constexpr QStringView kInstructors = u"instructors";
constexpr QStringView kSemester = u"semester";
constexpr QStringView kAcademicYear = u"academic_year";
constexpr QStringView kCode = u"code";

constexpr QStringView kTitle = u"title";
constexpr QStringView kDeadline = u"deadline";
constexpr QStringView kEndTime = u"end_time";
constexpr QStringView kUniqueKey = u"unique_key";
constexpr QStringView kData = u"data";
constexpr QStringView kPublishTime = u"publish_time";
constexpr QStringView kDescription = u"description";
constexpr QStringView kSubmitted = u"submitted";
constexpr QStringView kScore = u"score";
constexpr QStringView kIsClosed = u"is_closed";
constexpr QStringView kIsStarted = u"is_started";
constexpr QStringView kHomeworkActivities = u"homework_activities";
constexpr QStringView kActivities = u"activities";

/// 作业数组键名，供 parseAssignments 的调用方选择数据源
constexpr QStringView kType = u"type";

} // namespace field

/**
 * 观测值 → 规范串中的一个 token。
 *
 * 值 token 用 "V<长度>:<内容>"（长度前缀，避免分隔符歧义）；
 * 其余状态统一用 "N" 表示「无值」。
 *
 * ⚠️ 关键设计：Absent 与 Missing **必须映射到同一个 token**。
 * 原因：数据库里两者都存成 NULL，读回来时无法区分「当时确认是空」
 * 还是「当时没解析到」。若给它们不同的 token，那么一条
 * publish_time 本来就缺失的作业（实测中很常见）会在每次同步时
 * 算出不同的摘要，从而被误判为「内容已变化」并反复 UPDATE。
 *
 * 这样做会失去「字段从有值变为 null」的差异吗？不会：
 * 有值 → V…，无值 → N，两者不同，摘要照样变化。
 * 「确认空」与「未解析」之间的差异属于解析质量问题，由 parse_errors
 * 与 FieldState 负责记录，不应影响内容摘要。
 */
QString valueToken(const Observed<QString> &o)
{
    return o.hasValue()
               ? QStringLiteral("V") + QString::number(o.require().size()) + QLatin1Char(':')
                     + o.require()
               : QStringLiteral("N");
}

QString valueToken(const Observed<QDateTime> &o)
{
    if (!o.hasValue()) {
        return QStringLiteral("N");
    }
    const QString s = timeutil::toStorage(o.require());
    return QStringLiteral("V") + QString::number(s.size()) + QLatin1Char(':') + s;
}

QString valueToken(const Observed<bool> &o)
{
    return o.hasValue() ? (o.require() ? QStringLiteral("V1") : QStringLiteral("V0"))
                        : QStringLiteral("N");
}

QString valueToken(const Observed<double> &o)
{
    if (!o.hasValue()) {
        return QStringLiteral("N");
    }
    const QString s = QString::number(o.require(), 'g', 10);
    return QStringLiteral("V") + QString::number(s.size()) + QLatin1Char(':') + s;
}

/// 某个 key 是否在对象中「存在」（区分 null 与缺 key）
bool hasKey(const QJsonObject &obj, QStringView key)
{
    return obj.contains(key);
}

/// 解析 data 嵌套对象；不存在或非对象时返回空对象
QJsonObject subObject(const QJsonObject &obj, QStringView key)
{
    const QJsonValue v = obj.value(key);
    return v.isObject() ? v.toObject() : QJsonObject();
}

} // namespace

// ---------------------------------------------------------------------------
// 会话与信封校验
// ---------------------------------------------------------------------------

ParseOutcome AssignmentParser::inspectEnvelope(const QString &body)
{
    ParseOutcome outcome;

    const QString trimmed = body.trimmed();
    if (trimmed.isEmpty()) {
        outcome.kind = ParseOutcome::Kind::NotJson;
        outcome.detail = QStringLiteral("empty response body");
        return outcome;
    }

    // 站点会话失效时极可能直接返回登录页 HTML，且 HTTP 状态码仍为 200。
    // 因此判定依据必须是「响应体是否为 JSON 对象」，而不是状态码。
    if (!trimmed.startsWith(QLatin1Char('{'))) {
        outcome.kind = ParseOutcome::Kind::NotJson;
        // 只记录形状特征，不记录正文（正文可能含登录票据）
        outcome.detail = QStringLiteral("response is not a JSON object (first char U+%1, %2 bytes)")
                             .arg(trimmed.at(0).unicode(), 4, 16, QLatin1Char('0'))
                             .arg(trimmed.size());
        return outcome;
    }

    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(trimmed.toUtf8(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        outcome.kind = ParseOutcome::Kind::NotJson;
        outcome.detail = QStringLiteral("JSON parse failed: %1 at offset %2")
                             .arg(err.errorString())
                             .arg(err.offset);
        return outcome;
    }

    outcome.kind = ParseOutcome::Kind::Ok;
    return outcome;
}

// ---------------------------------------------------------------------------
// 字段读取原语（三态区分）
// ---------------------------------------------------------------------------

Observed<QString> AssignmentParser::readString(const QJsonObject &obj, QStringView key,
                                               QStringView fieldLabel, ParseIssues *issues)
{
    if (!hasKey(obj, key)) {
        // 接口本次没给这个 key —— 未加载，不得覆盖库中已有值
        if (issues != nullptr) {
            issues->append(ParseIssue(fieldLabel, QStringLiteral("key not present in response"),
                                      FieldState::Missing));
        }
        return Observed<QString>::missing();
    }

    const QJsonValue v = obj.value(key);
    if (v.isNull() || v.isUndefined()) {
        // 源数据明确给出 null —— 确认空值
        return Observed<QString>::absent();
    }
    if (v.isString()) {
        const QString s = v.toString();
        return s.isEmpty() ? Observed<QString>::absent() : Observed<QString>::present(s);
    }
    if (v.isDouble()) {
        // 站点偶尔把数值型字段以数字给出，按字符串处理更宽容
        return Observed<QString>::present(QString::number(v.toDouble(), 'g', 15));
    }

    if (issues != nullptr) {
        issues->append(ParseIssue(fieldLabel, QStringLiteral("expected string, got another type"),
                                  FieldState::Invalid));
    }
    return Observed<QString>::invalid();
}

Observed<QDateTime> AssignmentParser::readTime(const QJsonObject &obj, QStringView key,
                                               QStringView fieldLabel, ParseIssues *issues)
{
    if (!hasKey(obj, key)) {
        if (issues != nullptr) {
            issues->append(ParseIssue(fieldLabel, QStringLiteral("key not present in response"),
                                      FieldState::Missing));
        }
        return Observed<QDateTime>::missing();
    }

    const QJsonValue v = obj.value(key);
    if (v.isNull() || v.isUndefined()) {
        return Observed<QDateTime>::absent();
    }
    if (v.isString()) {
        const QString raw = v.toString();
        if (raw.trimmed().isEmpty()) {
            return Observed<QDateTime>::absent();
        }
        if (const auto parsed = timeutil::parseSiteTime(raw); parsed.has_value()) {
            return Observed<QDateTime>::present(*parsed);
        }
        if (issues != nullptr) {
            issues->append(ParseIssue(fieldLabel, QStringLiteral("unrecognized time format"),
                                      FieldState::Invalid));
        }
        return Observed<QDateTime>::invalid();
    }

    if (issues != nullptr) {
        issues->append(ParseIssue(fieldLabel, QStringLiteral("expected ISO time string, got another type"),
                                  FieldState::Invalid));
    }
    return Observed<QDateTime>::invalid();
}

Observed<bool> AssignmentParser::readBool(const QJsonObject &obj, QStringView key,
                                          QStringView fieldLabel, ParseIssues *issues)
{
    if (!hasKey(obj, key)) {
        if (issues != nullptr) {
            issues->append(ParseIssue(fieldLabel, QStringLiteral("key not present in response"),
                                      FieldState::Missing));
        }
        return Observed<bool>::missing();
    }

    const QJsonValue v = obj.value(key);
    if (v.isNull() || v.isUndefined()) {
        return Observed<bool>::absent();
    }
    if (v.isBool()) {
        return Observed<bool>::present(v.toBool());
    }
    if (v.isString()) {
        // 站点部分布尔字段以 "true"/"false" 字符串给出
        const QString s = v.toString().trimmed().toLower();
        if (s == QLatin1String("true") || s == QLatin1String("1")) {
            return Observed<bool>::present(true);
        }
        if (s == QLatin1String("false") || s == QLatin1String("0")) {
            return Observed<bool>::present(false);
        }
    }
    if (v.isDouble()) {
        return Observed<bool>::present(v.toDouble() != 0.0);
    }

    if (issues != nullptr) {
        issues->append(ParseIssue(fieldLabel, QStringLiteral("expected boolean, got another type"),
                                  FieldState::Invalid));
    }
    return Observed<bool>::invalid();
}

Observed<double> AssignmentParser::readNumber(const QJsonObject &obj, QStringView key,
                                              QStringView fieldLabel, ParseIssues *issues)
{
    if (!hasKey(obj, key)) {
        if (issues != nullptr) {
            issues->append(ParseIssue(fieldLabel, QStringLiteral("key not present in response"),
                                      FieldState::Missing));
        }
        return Observed<double>::missing();
    }

    const QJsonValue v = obj.value(key);
    if (v.isNull() || v.isUndefined()) {
        return Observed<double>::absent();
    }
    if (v.isDouble()) {
        return Observed<double>::present(v.toDouble());
    }
    if (v.isString()) {
        const QString s = v.toString();
        if (s.isEmpty()) {
            return Observed<double>::absent();
        }
        bool ok = false;
        const double d = s.toDouble(&ok);
        if (ok) {
            return Observed<double>::present(d);
        }
    }

    if (issues != nullptr) {
        issues->append(ParseIssue(fieldLabel, QStringLiteral("expected number, got another type"),
                                  FieldState::Invalid));
    }
    return Observed<double>::invalid();
}

// ---------------------------------------------------------------------------
// 课程
// ---------------------------------------------------------------------------

Course AssignmentParser::parseCourse(const QJsonObject &obj, const QDateTime &observedAt,
                                     ParseIssues *issues)
{
    Course c;
    // 本解析器只服务智汇南雍：课程自带来源键，写入快照时据此隔离软删除范围。
    c.source = source_keys::kNjuLms.toString();
    c.lastSeenAt = observedAt;
    c.present = true;

    const auto id = readString(obj, field::kId, QStringLiteral("course.id"), issues);
    if (id.hasValue()) {
        c.courseId = id.require();
    }

    const auto name = readString(obj, field::kName,
                                 QStringLiteral("course.name"), issues);
    if (name.hasValue()) {
        c.courseName = name.require();
    }

    const auto url = readString(obj, field::kUrl,
                                QStringLiteral("course.url"), issues);
    if (url.hasValue()) {
        c.detailUrl = url.require();
    }

    const auto code = readString(obj, field::kCourseCode,
                                 QStringLiteral("course.course_code"), issues);
    if (code.hasValue()) {
        c.courseCode = code.require();
    }

    const QJsonObject dept = subObject(obj, field::kDepartment);
    if (!dept.isEmpty()) {
        const auto deptName = readString(dept, field::kName,
                                        QStringLiteral("course.department.name"), issues);
        if (deptName.hasValue()) {
            c.department = deptName.require();
        }
    }

    const QJsonValue instructors = obj.value(field::kInstructors);
    if (instructors.isArray()) {
        const QJsonArray arr = instructors.toArray();
        for (const QJsonValue &iv : arr) {
            if (!iv.isObject()) {
                continue;
            }
            const auto teacherName = readString(iv.toObject(), field::kName,
                                                QStringLiteral("course.instructors[].name"), issues);
            if (teacherName.hasValue()) {
                c.instructors.append(teacherName.require());
            }
        }
    } else if (instructors.isNull()) {
        // 确认没有教师信息，不是错误
    }

    const QJsonObject semester = subObject(obj, field::kSemester);
    if (!semester.isEmpty()) {
        const auto semCode = readString(semester, field::kCode,
                                        QStringLiteral("course.semester.code"), issues);
        if (semCode.hasValue()) {
            c.semesterCode = semCode.require();
        }
    }

    const QJsonObject year = subObject(obj, field::kAcademicYear);
    if (!year.isEmpty()) {
        const auto yearCode = readString(year, field::kCode,
                                         QStringLiteral("course.academic_year.code"), issues);
        if (yearCode.hasValue()) {
            c.academicYearCode = yearCode.require();
        }
    }

    return c;
}

QList<Course> AssignmentParser::parseCourseList(const QJsonObject &root,
                                                const QDateTime &observedAt,
                                                ParseIssues *issues)
{
    QList<Course> out;

    const QJsonValue coursesValue = root.value(field::kCourses);
    if (!coursesValue.isArray()) {
        if (issues != nullptr) {
            issues->append(ParseIssue(QStringLiteral("courses"),
                                      QStringLiteral("'courses' array missing from response"),
                                      FieldState::Missing));
        }
        return out;
    }

    const QJsonArray arr = coursesValue.toArray();
    out.reserve(arr.size());
    for (const QJsonValue &v : arr) {
        if (!v.isObject()) {
            if (issues != nullptr) {
                issues->append(ParseIssue(QStringLiteral("courses[]"),
                                          QStringLiteral("array element is not an object"),
                                          FieldState::Invalid));
            }
            continue;
        }
        Course c = parseCourse(v.toObject(), observedAt, issues);
        if (c.courseId.isEmpty()) {
            // 没有稳定标识的课程无法入库，跳过并记录
            if (issues != nullptr) {
                issues->append(ParseIssue(QStringLiteral("course.id"),
                                          QStringLiteral("course without id skipped"),
                                          FieldState::Invalid));
            }
            continue;
        }
        out.append(c);
    }

    return out;
}

// ---------------------------------------------------------------------------
// 作业
// ---------------------------------------------------------------------------

Assignment AssignmentParser::parseAssignment(const QString &courseId, const QJsonObject &obj,
                                             const QDateTime &observedAt, ParseIssues *issues)
{
    Assignment a;
    a.courseId = courseId;
    a.observedAt = observedAt;

    // ---- 身份字段 -------------------------------------------------------------
    const auto id = readString(obj, field::kId,
                               QStringLiteral("assignment.id"), issues);
    if (id.hasValue()) {
        a.activityId = id.require();
    }

    // 稳定标识来源（实测 2026-10-09，见 docs 2.4 节）：
    //   - /api/courses/{id}/activities      → 每条都有 "unique_key"，样例 "homework-43604"
    //   - /api/courses/{id}/homework-activities → **实测 6/6 条都没有 unique_key**
    // 因此不能用「有就用、没有才推导」的写法来描述主路径：对作业列表接口而言，
    // 推导才是**常态**。已实测确认推导规则与站点一致：unique_key == "{type}-{id}"。
    const auto uniqueKey = readString(obj, field::kUniqueKey,
                                      QStringLiteral("assignment.unique_key"), issues);
    if (uniqueKey.hasValue()) {
        a.sourceId = uniqueKey.require();
    } else if (!a.activityId.isEmpty()) {
        // [推导] 复现站点规则 "{type}-{id}"。type 固定为 homework：本函数只用于作业。
        // 若站点将来改变该规则，去重键会变化，因此记录一条解析提示便于排查。
        a.sourceId = QStringLiteral("homework-") + a.activityId;
        if (issues != nullptr) {
            issues->append(ParseIssue(QStringLiteral("assignment.sourceId"),
                                      QStringLiteral("derived from '{type}-{id}' because this "
                                                     "endpoint omits unique_key"),
                                      FieldState::Missing));
        }
    }

    // ---- 内容字段 -------------------------------------------------------------
    a.title = readString(obj, field::kTitle,
                         QStringLiteral("assignment.title"), issues);

    // 实测 description 位于顶层 "data" 对象内（data.description），
    // 而非作业对象顶层。此处按实测结构读取。
    const QJsonObject data = subObject(obj, field::kData);
    if (data.isEmpty()) {
        if (issues != nullptr) {
            issues->append(ParseIssue(QStringLiteral("assignment.data"),
                                      QStringLiteral("'data' object missing"),
                                      FieldState::Missing));
        }
        a.description = Observed<QString>::missing();
    } else {
        a.description = readString(data, field::kDescription,
                                   QStringLiteral("assignment.data.description"), issues);
    }

    // ---- 时间字段 -------------------------------------------------------------
    // 发布时间实测位于 data.publish_time。⚠️ 该字段**经常缺失**，
    // 例如课程 11311 的作业 44332 就没有它，因此绝不能用采集时间兜底。
    if (!data.isEmpty()) {
        a.publishedAt = readTime(data, field::kPublishTime,
                                 QStringLiteral("assignment.data.publish_time"), issues);
    } else {
        a.publishedAt = Observed<QDateTime>::missing();
    }

    // 截止时间：实测顶层 "deadline"；"end_time" 实测与之相同，作为备选。
    a.dueAt = readTime(obj, field::kDeadline,
                       QStringLiteral("assignment.deadline"), issues);
    if (!a.dueAt.isAuthoritative()) {
        const auto endTime = readTime(obj, field::kEndTime,
                                      QStringLiteral("assignment.end_time"), issues);
        if (endTime.isAuthoritative()) {
            a.dueAt = endTime;
        }
    }

    // ---- 状态字段 -------------------------------------------------------------
    a.submitted = readBool(obj, field::kSubmitted,
                           QStringLiteral("assignment.submitted"), issues);
    a.score = readNumber(obj, field::kScore,
                         QStringLiteral("assignment.score"), issues);

    // status 是**推导**字段，站点没有直接给出。推导规则见 AssignmentStatus 注释。
    //
    // 判定顺序很重要：只有「未提交」这一分支才需要看截止时间。
    // 若把截止时间判断前置，一条已提交且已截止的作业会被误判为 Overdue。
    const auto dueHasPassed = [&a, &observedAt]() -> std::optional<bool> {
        if (a.dueAt.state() != FieldState::Present) {
            return std::nullopt;  // 不知道截止时间
        }
        return a.dueAt.require() < observedAt;
    };

    if (a.submitted.state() == FieldState::Present) {
        if (a.submitted.require()) {
            // 已提交：无论是否过期都算 Submitted
            a.status = Observed<AssignmentStatus>::present(AssignmentStatus::Submitted);
        } else if (const auto passed = dueHasPassed(); passed.has_value()) {
            a.status = Observed<AssignmentStatus>::present(*passed ? AssignmentStatus::Overdue
                                                                   : AssignmentStatus::Pending);
        } else {
            // 未提交，但截止时间未知：不能断言是「未提交」还是「已逾期」，
            // 因此返回 Missing，等截止时间可解析时再判定（需求场景 F 的同类处理）。
            a.status = Observed<AssignmentStatus>::missing();
        }
    } else if (a.submitted.state() == FieldState::Absent) {
        // 该接口不提供提交状态（实测 /activities 就没有 submitted）：
        // 仅能根据 is_closed 与截止时间做**弱推断**，并在 UI 上标注为推导值。
        const auto closed = readBool(obj, field::kIsClosed,
                                     QStringLiteral("assignment.is_closed"), issues);
        if (closed.state() == FieldState::Present && !closed.require()) {
            // 明确未关闭 → 至少可以判定为未提交（是否逾期再看截止时间）
            if (const auto passed = dueHasPassed(); passed.has_value()) {
                a.status = Observed<AssignmentStatus>::present(*passed ? AssignmentStatus::Overdue
                                                                       : AssignmentStatus::Pending);
            } else {
                a.status = Observed<AssignmentStatus>::missing();
            }
        } else if (closed.state() == FieldState::Present) {
            if (const auto passed = dueHasPassed(); passed.has_value()) {
                a.status = Observed<AssignmentStatus>::present(*passed ? AssignmentStatus::Overdue
                                                                       : AssignmentStatus::Closed);
            } else {
                a.status = Observed<AssignmentStatus>::present(AssignmentStatus::Closed);
            }
        } else {
            a.status = Observed<AssignmentStatus>::missing();
        }
    } else {
        a.status = Observed<AssignmentStatus>::missing();
    }

    // 详情链接：站点**没有**提供现成的作业详情 URL 字段，只能按站点路由推导。
    //
    // [实测 2026-10-10] 在已登录的站点里点开一条作业，SPA 跳到的是
    //     https://lms.nju.edu.cn/course/11311/learning-activity#/45266
    // 页面标题为「第四次作业，十月十五号截止」且含「作业内容 / 我的作业」等详情区块。
    // 早先推导的 /course/{id}/activity/{activityId} 是错的（打开会显示"找不到资源"）。
    // 注意这里是 hash 路由：真正的活动 id 在 # 之后，去掉会变成"资源不存在"。
    if (!courseId.isEmpty() && !a.activityId.isEmpty()) {
        a.detailUrl = Observed<QString>::present(
            QStringLiteral("https://lms.nju.edu.cn/course/%1/learning-activity#/%2")
                .arg(courseId, a.activityId));
    } else {
        a.detailUrl = Observed<QString>::missing();
    }

    // ---- 内容摘要 -------------------------------------------------------------
    a.contentSignature = computeContentSignature(a);
    a.contentHash = QString::fromLatin1(
        QCryptographicHash::hash(a.contentSignature.toUtf8(), QCryptographicHash::Sha256).toHex());

    return a;
}

QList<Assignment> AssignmentParser::parseAssignments(const QString &courseId,
                                                     const QJsonArray &items,
                                                     const QDateTime &observedAt,
                                                     ParseIssues *issues)
{
    QList<Assignment> out;
    out.reserve(items.size());

    for (const QJsonValue &v : items) {
        if (!v.isObject()) {
            if (issues != nullptr) {
                issues->append(ParseIssue(QStringLiteral("assignments[]"),
                                          QStringLiteral("array element is not an object"),
                                          FieldState::Invalid));
            }
            continue;
        }
        Assignment a = parseAssignment(courseId, v.toObject(), observedAt, issues);
        if (a.sourceId.isEmpty()) {
            // 没有稳定标识就无法安全去重，宁可跳过并报错，也不要生成重复记录
            if (issues != nullptr) {
                issues->append(ParseIssue(QStringLiteral("assignment.unique_key"),
                                          QStringLiteral("assignment without stable id skipped"),
                                          FieldState::Invalid));
            }
            continue;
        }
        out.append(a);
    }

    return out;
}

// ---------------------------------------------------------------------------
// 内容摘要
// ---------------------------------------------------------------------------

QString AssignmentParser::computeContentSignature(const Assignment &a)
{
    // 只纳入对用户有意义的字段；采集时间 / 观察时间 / 摘要自身不参与，
    // 否则「什么都没变」也会被判为变化。
    //
    // 每个 token 见 valueToken()：有值 → V<len>:<value>，无值（Absent 或 Missing 或
    // Invalid）→ N。Invalid 同样归入 N，因为解析失败时数据库里保留的是旧值，
    // 摘要也会保持与旧记录一致，不应触发无意义的更新。
    const QStringList parts = {
        QStringLiteral("title=") + valueToken(a.title),
        QStringLiteral("desc=") + valueToken(a.description),
        QStringLiteral("due=") + valueToken(a.dueAt),
        QStringLiteral("pub=") + valueToken(a.publishedAt),
        QStringLiteral("submitted=") + valueToken(a.submitted),
        QStringLiteral("score=") + valueToken(a.score),
        // 详情链接（详见 parseAssignment 里的实测注释）。把它纳入摘要的原因：
        // 推导规则一旦修正，数据库里的旧值必须被重新采集覆盖；
        // 不纳入的话 contentHash 不变，仓储会认为"这条没变化"而跳过更新。
        QStringLiteral("url=") + valueToken(a.detailUrl),
    };
    return parts.join(QLatin1Char('\x1f'));
}

} // namespace njulms
