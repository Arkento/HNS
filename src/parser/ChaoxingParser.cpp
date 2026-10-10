#include "parser/ChaoxingParser.h"

#include <QHash>
#include <QRegularExpression>
#include <QStringList>
#include <QUrl>
#include <QUrlQuery>

namespace njulms {
namespace {

/// HTML 实体：解析时只需要把最常见的几个还原（标题里会出现 &amp; &#39; 等）
QString unescapeHtml(QString text)
{
    text.replace(QLatin1String("&amp;"), QLatin1String("&"));
    text.replace(QLatin1String("&lt;"), QLatin1String("<"));
    text.replace(QLatin1String("&gt;"), QLatin1String(">"));
    text.replace(QLatin1String("&quot;"), QLatin1String("\""));
    text.replace(QLatin1String("&#39;"), QLatin1String("'"));
    text.replace(QLatin1String("&nbsp;"), QLatin1String(" "));
    return text;
}

/// 去掉标签、压掉多余空白（用于从一小段 HTML 里取纯文本）
QString stripTags(const QString &html)
{
    QString text = html;
    text.remove(QRegularExpression(QStringLiteral("<[^>]*>")));
    return unescapeHtml(text).simplified();
}

/**
 * 「剩余 101 小时 4 分钟」这类相对时间 → 绝对时间。
 *
 * 站点在作业列表里**只给相对剩余**（原始 HTML：`剩余101小时4分钟`），
 * 所以这里按采集时刻折算。折算不出来（格式变了）就返回无效 QDateTime，
 * 由调用方记为缺失，绝不编造。
 */
QDateTime deadlineFromRemaining(const QString &timeText, const QDateTime &observedAt)
{
    if (!observedAt.isValid()) {
        return QDateTime();
    }
    const QRegularExpression re(QStringLiteral("剩余\\s*(\\d+)\\s*天\\s*(\\d+)\\s*小时"
                                               "|剩余\\s*(\\d+)\\s*小时\\s*(\\d+)\\s*分钟"
                                               "|剩余\\s*(\\d+)\\s*分钟"));
    const QRegularExpressionMatch match = re.match(timeText);
    if (!match.hasMatch()) {
        return QDateTime();
    }

    qint64 seconds = 0;
    if (!match.captured(1).isEmpty()) {
        seconds = match.captured(1).toLongLong() * 86400 + match.captured(2).toLongLong() * 3600;
    } else if (!match.captured(3).isEmpty()) {
        seconds = match.captured(3).toLongLong() * 3600 + match.captured(4).toLongLong() * 60;
    } else {
        seconds = match.captured(5).toLongLong() * 60;
    }
    return observedAt.addSecs(seconds);
}

/// 状态文案 → 我们的状态枚举（站点文案见 docs/06）
AssignmentStatus statusFromText(const QString &text, bool pastDue)
{
    if (text.contains(QStringLiteral("已完成")) || text.contains(QStringLiteral("已交"))
        || text.contains(QStringLiteral("待批阅"))) {
        return AssignmentStatus::Submitted;
    }
    if (text.contains(QStringLiteral("未完成")) || text.contains(QStringLiteral("未交"))
        || text.contains(QStringLiteral("待完成")) || text.contains(QStringLiteral("未开始"))
        || text.contains(QStringLiteral("进行中"))) {
        return pastDue ? AssignmentStatus::Overdue : AssignmentStatus::Pending;
    }
    return AssignmentStatus::Unknown;
}

} // namespace

QString ChaoxingParser::sourceKey()
{
    return source_keys::kChaoxing.toString();
}

QList<Course> ChaoxingParser::parseCourseList(const QString &html, const QDateTime &observedAt,
                                              ParseIssues *issues)
{
    QList<Course> courses;

    // 课程卡片：链接文本即课程名，URL 里带 courseid / clazzid / cpi
    static const QRegularExpression linkRe(
        QStringLiteral("<a[^>]+href=\"([^\"]*stucoursemiddle\\?[^\"]+)\"[^>]*>(.*?)</a>"),
        QRegularExpression::DotMatchesEverythingOption);

    // courseid-clazzid → 已收录课程在 courses 中的下标（同一门课可能有多个链接）
    QHash<QString, int> seen;
    auto it = linkRe.globalMatch(html);
    while (it.hasNext()) {
        const QRegularExpressionMatch match = it.next();
        const QString href = unescapeHtml(match.captured(1));
        const QString name = stripTags(match.captured(2));

        const QRegularExpression idRe(
            QStringLiteral("courseid=(\\d+)[^\"']*?clazzid=(\\d+)"));
        const QRegularExpressionMatch idMatch = idRe.match(href);
        if (!idMatch.hasMatch()) {
            continue;
        }

        const QString localId = idMatch.captured(1) + QLatin1Char('-') + idMatch.captured(2);
        if (const auto existing = seen.constFind(localId); existing != seen.constEnd()) {
            // 同一张卡片通常有两个链接（封面 + 标题）。封面链接常常没有文本，
            // 此时课程名先用 localId 兜底；若之后出现带文本的链接，替换掉兜底值。
            Course &course = courses[existing.value()];
            if (course.courseName == localId && !name.isEmpty()) {
                course.courseName = name;
            }
            continue;
        }
        seen.insert(localId, courses.size());

        Course course;
        course.courseId = localId;
        course.courseName = name.isEmpty() ? localId : name;
        course.detailUrl = href;
        course.source = source_keys::kChaoxing.toString();
        course.lastSeenAt = observedAt;
        courses.append(course);
    }

    if (courses.isEmpty() && issues != nullptr) {
        issues->append(ParseIssue(QStringLiteral("chaoxing.courselist"),
                                  QStringLiteral("no course links found (template changed or not logged in)"),
                                  FieldState::Missing));
    }
    return courses;
}

QList<Assignment> ChaoxingParser::parseWorkList(const QString &html, const QString &courseId,
                                                const QDateTime &observedAt, ParseIssues *issues)
{
    QList<Assignment> assignments;

    // 每条作业一个 <li …data="详情URL"…aria-label="标题 ; 状态"…>
    static const QRegularExpression itemRe(
        QStringLiteral("<li[^>]*data=\"([^\"]*work/task[^\"]*)\"[^>]*aria-label=\"([^\"]*)\"[^>]*>(.*?)</li>"),
        QRegularExpression::DotMatchesEverythingOption);
    static const QRegularExpression statusRe(
        QStringLiteral("<p[^>]*class=\"[^\"]*status[^\"]*\"[^>]*>(.*?)</p>"),
        QRegularExpression::DotMatchesEverythingOption);
    static const QRegularExpression titleRe(
        QStringLiteral("<p[^>]*class=\"[^\"]*overHidden2[^\"]*\"[^>]*>(.*?)</p>"),
        QRegularExpression::DotMatchesEverythingOption);
    static const QRegularExpression timeRe(
        QStringLiteral("<div[^>]*class=\"([^\"]*time[^\"]*)\"[^>]*>(.*?)</div>"),
        QRegularExpression::DotMatchesEverythingOption);
    static const QRegularExpression workIdRe(QStringLiteral("workId=(\\d+)"));

    auto it = itemRe.globalMatch(html);
    while (it.hasNext()) {
        const QRegularExpressionMatch match = it.next();
        const QString detailUrl = unescapeHtml(match.captured(1));
        const QString ariaLabel = unescapeHtml(match.captured(2));
        const QString body = match.captured(3);

        const QRegularExpressionMatch idMatch = workIdRe.match(detailUrl);
        if (!idMatch.hasMatch()) {
            if (issues != nullptr) {
                issues->append(ParseIssue(QStringLiteral("chaoxing.work"),
                                          QStringLiteral("work item without workId skipped"),
                                          FieldState::Invalid));
            }
            continue;
        }
        const QString workId = idMatch.captured(1);

        // 标题：优先 aria-label（"标题 ; 状态"），退回 p.overHidden2
        QString title = ariaLabel;
        QString statusText;
        const int separator = ariaLabel.lastIndexOf(QStringLiteral(" ; "));
        if (separator > 0) {
            title = ariaLabel.left(separator).trimmed();
            statusText = ariaLabel.mid(separator + 3).trimmed();
        }
        if (title.isEmpty()) {
            title = stripTags(titleRe.match(body).captured(1));
        }
        if (statusText.isEmpty()) {
            statusText = stripTags(statusRe.match(body).captured(1));
        }

        // 时间：站点只给相对剩余；class 里带 over 的视为已过期
        const QRegularExpressionMatch timeMatch = timeRe.match(body);
        const QString timeClass = timeMatch.hasMatch() ? timeMatch.captured(1) : QString();
        const QString timeText = timeMatch.hasMatch() ? stripTags(timeMatch.captured(2)) : QString();
        const bool pastDue = timeClass.contains(QStringLiteral("over"))
                             || timeText.contains(QStringLiteral("已过期"))
                             || timeText.contains(QStringLiteral("已结束"));
        const QDateTime dueAt = deadlineFromRemaining(timeText, observedAt);

        Assignment a;
        a.courseId = courseId;
        a.sourceId = QStringLiteral("cx-work-%1").arg(workId);
        a.activityId = workId;
        a.title = Observed<QString>::present(title.isEmpty() ? QStringLiteral("（标题未获取）") : title);
        a.dueAt = dueAt.isValid() ? Observed<QDateTime>::present(dueAt)
                                  : Observed<QDateTime>::missing();
        a.detailUrl = Observed<QString>::present(detailUrl);
        a.status = statusText.isEmpty() ? Observed<AssignmentStatus>::missing()
                                        : Observed<AssignmentStatus>::present(
                                              statusFromText(statusText, pastDue));
        if (a.status.hasValue()) {
            const AssignmentStatus status = a.status.require();
            a.submitted = Observed<bool>::present(status == AssignmentStatus::Submitted);
        } else {
            a.submitted = Observed<bool>::missing();
        }
        a.score = Observed<double>::missing();        // 列表页不含成绩
        a.description = Observed<QString>::missing(); // 说明在作业详情页，阶段 2 再取
        a.observedAt = observedAt;
        assignments.append(a);
    }

    if (assignments.isEmpty() && issues != nullptr) {
        // 显式空列表（页面写明"暂无作业"）是权威结论：不再记 Missing，
        // 让上层把它当作"确实没有作业"处理（可以安全地软删本课程的历史条目）。
        static const QRegularExpression emptyRe(
            QStringLiteral("暂无作业|null-data"));
        if (!emptyRe.match(html).hasMatch()) {
            issues->append(ParseIssue(
                QStringLiteral("chaoxing.worklist"),
                QStringLiteral("no work items found (template changed, empty list, or not logged in)"),
                FieldState::Missing));
        }
    }
    return assignments;
}

QList<Assignment> ChaoxingParser::parseExamList(const QString &html, const QString &courseId,
                                                const QString &examListUrl,
                                                const QDateTime &observedAt,
                                                ParseIssues *issues)
{
    QList<Assignment> exams;

    static const QRegularExpression itemRe(
        QStringLiteral("<li[^>]*>(.*?)</li>"), QRegularExpression::DotMatchesEverythingOption);
    static const QRegularExpression onclickRe(
        QStringLiteral("onclick=\"([A-Za-z0-9_]+)\\('(\\d+)','(\\d+)'\\)\""));
    static const QRegularExpression titleRe(
        QStringLiteral("<p[^>]*class=\"[^\"]*overHidden2[^\"]*\"[^>]*>(.*?)</p>"),
        QRegularExpression::DotMatchesEverythingOption);
    static const QRegularExpression statusRe(
        QStringLiteral("<p[^>]*class=\"[^\"]*status[^\"]*\"[^>]*>(.*?)</p>"),
        QRegularExpression::DotMatchesEverythingOption);
    static const QRegularExpression timeRe(
        QStringLiteral("<div[^>]*class=\"([^\"]*time[^\"]*)\"[^>]*>(.*?)</div>"),
        QRegularExpression::DotMatchesEverythingOption);

    // 列表 URL 里的课程参数：用于构造"查看答卷"链接（与站点自己的 viewExamAnswer 一致）
    const QUrl listUrl(examListUrl);
    const QUrlQuery query(listUrl);
    const QString courseid = query.queryItemValue(QStringLiteral("courseid"));
    const QString clazzid = query.queryItemValue(QStringLiteral("clazzid"));
    const QString cpi = query.queryItemValue(QStringLiteral("cpi"));

    auto it = itemRe.globalMatch(html);
    while (it.hasNext()) {
        const QRegularExpressionMatch match = it.next();
        const QString body = match.captured(1);

        // 只认"带两个数字参数的 onclick"的条目：对话框等无关 <li> 会被自然跳过
        const QRegularExpressionMatch onclickMatch = onclickRe.match(body);
        if (!onclickMatch.hasMatch()) {
            continue;
        }
        const QString fn = onclickMatch.captured(1);
        const QString examId = onclickMatch.captured(2);
        const QString answerId = onclickMatch.captured(3);

        QString title = stripTags(titleRe.match(body).captured(1));
        if (title.isEmpty()) {
            title = QStringLiteral("（标题未获取）");
        }
        const QString statusText = stripTags(statusRe.match(body).captured(1));

        // 时间块（未完成/进行中的考试可能出现；已完成条目不显示时间）
        const QRegularExpressionMatch timeMatch = timeRe.match(body);
        const QString timeClass = timeMatch.hasMatch() ? timeMatch.captured(1) : QString();
        const QString timeText = timeMatch.hasMatch() ? stripTags(timeMatch.captured(2)) : QString();
        const bool pastDue = timeClass.contains(QStringLiteral("over"))
                             || timeText.contains(QStringLiteral("已过期"))
                             || timeText.contains(QStringLiteral("已结束"));
        const QDateTime dueAt = deadlineFromRemaining(timeText, observedAt);

        Assignment a;
        a.kind = QStringLiteral("exam");
        a.courseId = courseId;
        a.sourceId = QStringLiteral("cx-exam-%1").arg(examId);
        a.activityId = examId;
        a.title = Observed<QString>::present(title);
        a.dueAt = dueAt.isValid() ? Observed<QDateTime>::present(dueAt)
                                  : Observed<QDateTime>::missing();
        a.status = statusText.isEmpty() ? Observed<AssignmentStatus>::missing()
                                        : Observed<AssignmentStatus>::present(
                                              statusFromText(statusText, pastDue));
        if (a.status.hasValue()) {
            a.submitted = Observed<bool>::present(a.status.require() == AssignmentStatus::Submitted);
        } else {
            a.submitted = Observed<bool>::missing();
        }
        a.score = Observed<double>::missing();
        a.description = Observed<QString>::missing();
        a.publishedAt = Observed<QDateTime>::missing();
        a.observedAt = observedAt;

        // 详情链接（见头文件说明）：已完成 → 查看答卷页；其它 → 列表页兜底
        QString detailUrl;
        if (fn == QLatin1String("viewExamAnswer") && !courseid.isEmpty() && !clazzid.isEmpty()
            && !cpi.isEmpty()) {
            detailUrl = QStringLiteral("https://mooc1.chaoxing.com/exam-ans/exam/test/look"
                                       "?courseId=%1&classId=%2&examId=%3&examAnswerId=%4&cpi=%5")
                            .arg(courseid, clazzid, examId, answerId, cpi);
        } else if (!examListUrl.isEmpty()) {
            detailUrl = examListUrl;
        }
        a.detailUrl = detailUrl.isEmpty() ? Observed<QString>::missing()
                                          : Observed<QString>::present(detailUrl);

        exams.append(a);
    }

    if (exams.isEmpty() && issues != nullptr) {
        // 显式空列表（页面写明"暂无考试"）是权威结论；读不出内容才记 Missing
        static const QRegularExpression emptyRe(QStringLiteral("暂无考试"));
        if (!emptyRe.match(html).hasMatch()) {
            issues->append(ParseIssue(
                QStringLiteral("chaoxing.examlist"),
                QStringLiteral("no exam items found (template changed, empty list, or not logged in)"),
                FieldState::Missing));
        }
    }
    return exams;
}

} // namespace njulms
