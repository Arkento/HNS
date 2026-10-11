#include "parser/YuketangParser.h"
#include "support/YuketangFixtures.h"

#include <QTest>
#include <QTimeZone>

using namespace njulms;
using namespace njulms::test;

namespace {

/// 固定采集时刻：让"截止时间是否已过"的判定与真实日历无关
QDateTime observedAt()
{
    return QDateTime(QDate(2026, 10, 11), QTime(12, 0, 0), QTimeZone::UTC);
}

} // namespace

/**
 * 雨课堂 JSON 解析测试。
 *
 * 样本结构与 2026-10-11 登录态实测一致（见 docs/07 / tests/support/YuketangFixtures.cpp），
 * 值全部匿名化。重点：
 *   - 课程列表字段映射（classroom_id 作为主键、详情链接拼接）；
 *   - chapter + schedule 合并：leaf_type 5/6 → 考试/作业，视频等忽略；
 *   - 状态推导：已完成→已提交（带成绩）、未完成且已过→已逾期、未开始→未提交；
 *   - 结构缺失时如实记录（不猜测、不产生假条目）。
 */
class YuketangParserTests : public QObject
{
    Q_OBJECT

private slots:

    void sourceKey_isYuketang()
    {
        QCOMPARE(YuketangParser::sourceKey(), QStringLiteral("yuketang"));
    }

    void courseList_parsesTwoCourses()
    {
        ParseIssues issues;
        const QList<Course> courses = YuketangParser::parseCourseList(
            yuketang_fixtures::courseListTwoCourses(), observedAt(), &issues);

        QCOMPARE(courses.size(), 2);
        QVERIFY2(issues.isEmpty(), qPrintable(issues.value(0).reason));

        const Course &first = courses.at(0);
        QCOMPARE(first.source, QStringLiteral("yuketang"));
        QCOMPARE(first.courseId, QStringLiteral("33000001"));   // classroom_id
        QCOMPARE(first.courseName, QStringLiteral("2026秋-测试课程甲01班"));
        QCOMPARE(first.instructors, QStringList{QStringLiteral("教师甲")});
        QCOMPARE(first.semesterCode.value_or(QString()), QStringLiteral("202601"));
        QVERIFY(first.detailUrl.contains(QStringLiteral("/v2/web/studentLog/33000001")));
        QVERIFY(first.detailUrl.contains(QStringLiteral("university_id=4069")));
        QVERIFY(first.detailUrl.contains(QStringLiteral("platform_id=3")));

        QCOMPARE(courses.at(1).courseId, QStringLiteral("33000002"));
    }

    void courseList_missingEnvelopeYieldsEmptyWithIssue()
    {
        ParseIssues issues;
        const QList<Course> courses = YuketangParser::parseCourseList(
            QJsonObject{}, observedAt(), &issues);
        QVERIFY(courses.isEmpty());
        QCOMPARE(issues.size(), 1);
        QCOMPARE(issues.first().state, FieldState::Missing);
    }

    void courseWork_mergesChapterAndSchedule()
    {
        ParseIssues issues;
        const QList<Assignment> items = YuketangParser::parseCourseWork(
            yuketang_fixtures::courseChapter(), yuketang_fixtures::courseSchedule(),
            QStringLiteral("33000001"), 33000001, observedAt(), &issues);

        // 5 条 = 4 条（嵌套形态）+ 1 条（直接叶子形态）；两种形态的视频都被忽略
        QCOMPARE(items.size(), 5);
        QVERIFY2(issues.isEmpty(), qPrintable(issues.value(0).reason));

        // ① 考试（leaf_type 5）：未开始 → 未提交；截止/发布来自毫秒时间戳
        const Assignment &exam = items.at(0);
        QCOMPARE(exam.kind, QStringLiteral("exam"));
        QCOMPARE(exam.sourceId, QStringLiteral("ykt-91075025"));
        QCOMPARE(exam.title.require(), QStringLiteral("第一章 预习检测"));
        QCOMPARE(exam.status.require(), AssignmentStatus::Pending);
        QCOMPARE(exam.submitted.require(), false);
        QCOMPARE(exam.dueAt.require(),
                 QDateTime(QDate(2026, 10, 13), QTime(6, 0, 59), QTimeZone::UTC));
        QCOMPARE(exam.publishedAt.require(),
                 QDateTime(QDate(2026, 9, 29), QTime(13, 54, 49), QTimeZone::UTC));
        QVERIFY(exam.detailUrl.require().contains(
            QStringLiteral("/ai-workspace/lms-graph/33000001/quiz/91075025?node_id=8848673")));

        // ② 作业（leaf_type 6）未完成 → 未提交
        const Assignment &homework = items.at(1);
        QCOMPARE(homework.kind, QStringLiteral("homework"));
        QCOMPARE(homework.sourceId, QStringLiteral("ykt-90857633"));
        QCOMPARE(homework.status.require(), AssignmentStatus::Pending);
        QCOMPARE(homework.submitted.require(), false);
        QVERIFY(homework.detailUrl.require().contains(QStringLiteral("/exercise/90857633")));

        // ③ 已完成（schedule status=5）→ 已提交 + 成绩
        const Assignment &done = items.at(2);
        QCOMPARE(done.sourceId, QStringLiteral("ykt-90000001"));
        QCOMPARE(done.status.require(), AssignmentStatus::Submitted);
        QCOMPARE(done.submitted.require(), true);
        QCOMPARE(done.score.require(), 8.0);

        // ④ 未完成且早已超过截止 → 已逾期
        const Assignment &overdue = items.at(3);
        QCOMPARE(overdue.sourceId, QStringLiteral("ykt-90000002"));
        QCOMPARE(overdue.status.require(), AssignmentStatus::Overdue);
        QCOMPARE(overdue.submitted.require(), false);

        // ⑤ 直接叶子形态（section_leaf_list 的元素自带 leaf_type）：已完成 + 成绩
        const Assignment &direct = items.at(4);
        QCOMPARE(direct.kind, QStringLiteral("exam"));
        QCOMPARE(direct.sourceId, QStringLiteral("ykt-91076001"));
        QCOMPARE(direct.title.require(), QStringLiteral("第二章 预习检测（直接叶子）"));
        QCOMPARE(direct.status.require(), AssignmentStatus::Submitted);
        QCOMPARE(direct.score.require(), 5.0);
        QVERIFY(direct.detailUrl.require().contains(QStringLiteral("/quiz/91076001")));
    }

    void courseWork_missingScheduleLeavesStatusUnknown()
    {
        ParseIssues issues;
        const QList<Assignment> items = YuketangParser::parseCourseWork(
            yuketang_fixtures::courseChapter(), QJsonObject{}, QStringLiteral("33000001"),
            33000001, observedAt(), &issues);

        QCOMPARE(items.size(), 5);   // 条目仍然解析出来（标题/时间可用）
        QVERIFY(!issues.isEmpty());  // 但状态未知要如实记录
        for (const Assignment &item : items) {
            QCOMPARE(item.status.state(), FieldState::Missing);
        }
    }

    void courseWork_missingChapterYieldsEmptyWithIssue()
    {
        ParseIssues issues;
        const QList<Assignment> items = YuketangParser::parseCourseWork(
            QJsonObject{}, yuketang_fixtures::courseSchedule(), QStringLiteral("33000001"),
            33000001, observedAt(), &issues);
        QVERIFY(items.isEmpty());
        QCOMPARE(issues.size(), 1);
        QCOMPARE(issues.first().state, FieldState::Missing);
    }
};

QTEST_MAIN(YuketangParserTests)
#include "YuketangParserTests.moc"
