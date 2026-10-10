#include "parser/ChaoxingParser.h"
#include "support/ChaoxingFixtures.h"

#include <QDateTime>
#include <QTest>
#include <QTimeZone>

using namespace njulms;
using namespace njulms::test;

namespace {

/// 固定采集时刻：让「剩余 X 时间」折算出的绝对时间可以精确断言，且不随日历漂移。
QDateTime observedAt()
{
    return QDateTime(QDate(2026, 10, 10), QTime(12, 0, 0), QTimeZone::UTC);
}

} // namespace

/**
 * 学习通 HTML 解析测试。
 *
 * 样本结构见 tests/support/ChaoxingFixtures.h（结构与实测一致、值全部匿名化）。
 * 重点覆盖两类回归：
 *   1. 实测模板的正常提取（状态映射、剩余时间折算、链接去重）；
 *   2. 模板改版 / 条目残缺时**返回空 + ParseIssue**，而不是静默给出错误数据。
 */
class ChaoxingParserTests : public QObject
{
    Q_OBJECT

private slots:

    void sourceKey_isChaoxing()
    {
        QCOMPARE(ChaoxingParser::sourceKey(), QStringLiteral("chaoxing"));
    }

    // ---- 课程列表 -----------------------------------------------------------

    void courseList_extractsCoursesAndDeduplicatesLinks()
    {
        ParseIssues issues;
        const QList<Course> courses = ChaoxingParser::parseCourseList(
            chaoxing_fixtures::courseListTwoCourses(), observedAt(), &issues);

        QCOMPARE(courses.size(), 2);
        QVERIFY2(issues.isEmpty(), qPrintable(issues.value(0).reason));

        const Course &first = courses.at(0);
        QCOMPARE(first.source, QStringLiteral("chaoxing"));
        QCOMPARE(first.courseId, QStringLiteral("111111111-222222222"));
        // 封面链接没有文本 → 课程名必须来自同一卡片的标题链接（去重后补全）
        QCOMPARE(first.courseName, QStringLiteral("测试课程甲"));
        QVERIFY(first.detailUrl.startsWith(
            QStringLiteral("https://mooc1-1.chaoxing.com/visit/stucoursemiddle")));
        QVERIFY(first.detailUrl.contains(QStringLiteral("clazzid=222222222")));
        QCOMPARE(first.lastSeenAt, observedAt());
        QVERIFY(first.present);

        QCOMPARE(courses.at(1).courseId, QStringLiteral("444444444-555555555"));
        QCOMPARE(courses.at(1).courseName, QStringLiteral("测试课程乙 & 实践"));
    }

    void courseList_templateChangedYieldsEmptyWithIssue()
    {
        ParseIssues issues;
        const QList<Course> courses = ChaoxingParser::parseCourseList(
            QStringLiteral("<html><body>页面已改版</body></html>"), observedAt(), &issues);

        QVERIFY(courses.isEmpty());
        QCOMPARE(issues.size(), 1);
        QCOMPARE(issues.first().state, FieldState::Missing);
    }

    // ---- 作业列表 -----------------------------------------------------------

    void workList_mapsStatusesAndDeadlines()
    {
        ParseIssues issues;
        const QString courseId = QStringLiteral("111111111-222222222");
        const QList<Assignment> items = ChaoxingParser::parseWorkList(
            chaoxing_fixtures::workListMixedStatuses(), courseId, observedAt(), &issues);

        QCOMPARE(items.size(), 5);   // 无 workId 的坏条目被跳过，分页块被忽略
        QCOMPARE(issues.size(), 1);  // 坏条目记一条 Invalid，不静默
        QCOMPARE(issues.first().state, FieldState::Invalid);

        for (const Assignment &a : items) {
            QCOMPARE(a.courseId, courseId);
            QCOMPARE(a.observedAt, observedAt());
        }

        // ① 已完成：Submitted；截止 = 采集时刻 + 101小时4分钟（相对值折算）
        const Assignment &done = items.at(0);
        QCOMPARE(done.sourceId, QStringLiteral("cx-work-900001"));
        QCOMPARE(done.activityId, QStringLiteral("900001"));
        QCOMPARE(done.title.require(), QStringLiteral("单元一 阅读理解"));
        QCOMPARE(done.status.require(), AssignmentStatus::Submitted);
        QCOMPARE(done.submitted.require(), true);
        QCOMPARE(done.dueAt.require(),
                 QDateTime(QDate(2026, 10, 14), QTime(17, 4), QTimeZone::UTC));
        QVERIFY(done.detailUrl.require().startsWith(
            QStringLiteral("https://mooc1.chaoxing.com/mooc-ans/mooc2/work/task?")));

        // 列表页不含成绩与说明：必须是 Missing（不得编造，也不得覆盖库中旧值）
        QCOMPARE(done.score.state(), FieldState::Missing);
        QCOMPARE(done.description.state(), FieldState::Missing);

        // ② 待批阅：视为已提交；页面没有时间块 → 截止时间 Missing
        const Assignment &grading = items.at(1);
        QCOMPARE(grading.sourceId, QStringLiteral("cx-work-900002"));
        QCOMPARE(grading.status.require(), AssignmentStatus::Submitted);
        QCOMPARE(grading.dueAt.state(), FieldState::Missing);

        // ③ 未完成 + 剩余3天2小时 → Pending；标题里的 &amp; 已还原
        const Assignment &pending = items.at(2);
        QCOMPARE(pending.sourceId, QStringLiteral("cx-work-900003"));
        QCOMPARE(pending.title.require(), QStringLiteral("单元三 听力 & 口语"));
        QCOMPARE(pending.status.require(), AssignmentStatus::Pending);
        QCOMPARE(pending.submitted.require(), false);
        QCOMPARE(pending.dueAt.require(),
                 QDateTime(QDate(2026, 10, 13), QTime(14, 0), QTimeZone::UTC));

        // ④ 未完成 + 已过期（class 含 over）→ Overdue
        const Assignment &overdue = items.at(3);
        QCOMPARE(overdue.sourceId, QStringLiteral("cx-work-900004"));
        QCOMPARE(overdue.status.require(), AssignmentStatus::Overdue);
        QCOMPARE(overdue.submitted.require(), false);

        // ⑤ 未完成 + 剩余45分钟
        const Assignment &soon = items.at(4);
        QCOMPARE(soon.sourceId, QStringLiteral("cx-work-900005"));
        QCOMPARE(soon.status.require(), AssignmentStatus::Pending);
        QCOMPARE(soon.dueAt.require(),
                 QDateTime(QDate(2026, 10, 10), QTime(12, 45), QTimeZone::UTC));
    }

    void workList_templateChangedYieldsEmptyWithIssue()
    {
        ParseIssues issues;
        const QList<Assignment> items = ChaoxingParser::parseWorkList(
            chaoxing_fixtures::workListTemplateChanged(),
            QStringLiteral("111111111-222222222"), observedAt(), &issues);

        QVERIFY(items.isEmpty());
        QCOMPARE(issues.size(), 1);
        QCOMPARE(issues.first().state, FieldState::Missing);
    }

    void workList_explicitEmptyStateIsAuthoritative()
    {
        ParseIssues issues;
        const QList<Assignment> items =
            ChaoxingParser::parseWorkList(chaoxing_fixtures::workListEmptyState(),
                                          QStringLiteral("111111111-222222222"), observedAt(),
                                          &issues);

        QVERIFY(items.isEmpty());
        // 页面明确写着「暂无作业」= 权威的空列表，不记 Missing；
        // 否则上层会把它当成"读不出内容"，永远无法软删已经消失的旧作业。
        QVERIFY2(issues.isEmpty(), qPrintable(issues.value(0).reason));
    }
};

QTEST_MAIN(ChaoxingParserTests)
#include "ChaoxingParserTests.moc"
