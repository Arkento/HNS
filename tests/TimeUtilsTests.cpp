#include "utils/TimeUtils.h"

#include <QTest>

using namespace njulms;
using namespace njulms::timeutil;

/**
 * 时间处理测试。
 *
 * 需求文档明确要求「时间应使用明确的时区语义」「不要将采集时间误认为发布时间或截止时间」，
 * 因此这里重点验证 UTC 语义与时区展示的分离。
 */
class TimeUtilsTests : public QObject
{
    Q_OBJECT

private slots:

    // ---- 站点时间格式（实测样例）------------------------------------------

    void parsesMeasuredSiteFormat()
    {
        // 实测样例："2026-09-17T02:14:44Z"
        const auto dt = parseSiteTime(QStringLiteral("2026-09-17T02:14:44Z"));
        QVERIFY(dt.has_value());
        QCOMPARE(dt->timeSpec(), Qt::UTC);
        QCOMPARE(dt->date(), QDate(2026, 9, 17));
        QCOMPARE(dt->time(), QTime(2, 14, 44));
    }

    void parsesDeadlineFormat()
    {
        const auto dt = parseSiteTime(QStringLiteral("2026-09-24T15:59:00Z"));
        QVERIFY(dt.has_value());
        QCOMPARE(*dt, QDateTime(QDate(2026, 9, 24), QTime(15, 59, 0), QTimeZone::UTC));
    }

    void parsesFractionalSeconds()
    {
        const auto dt = parseSiteTime(QStringLiteral("2026-09-17T02:14:44.123Z"));
        QVERIFY(dt.has_value());
        QCOMPARE(dt->time().msec(), 123);
    }

    void parsesOffsetForm()
    {
        // "+08:00" 形式的偏移必须被正确换算成 UTC
        const auto dt = parseSiteTime(QStringLiteral("2026-09-24T23:59:00+08:00"));
        QVERIFY(dt.has_value());
        QCOMPARE(*dt, QDateTime(QDate(2026, 9, 24), QTime(15, 59, 0), QTimeZone::UTC));
    }

    void parsesOffsetWithoutColon()
    {
        const auto dt = parseSiteTime(QStringLiteral("2026-09-24T23:59:00+0800"));
        QVERIFY(dt.has_value());
        QCOMPARE(*dt, QDateTime(QDate(2026, 9, 24), QTime(15, 59, 0), QTimeZone::UTC));
    }

    void timezoneLessStringIsTreatedAsUtc()
    {
        // 站点所有时间都是 UTC。若把无时区标识的串按**本地时间**解释，
        // 在东八区会整体偏移 8 小时，导致截止时间显示错误。
        const auto dt = parseSiteTime(QStringLiteral("2026-09-24T15:59:00"));
        QVERIFY(dt.has_value());
        QCOMPARE(*dt, QDateTime(QDate(2026, 9, 24), QTime(15, 59, 0), QTimeZone::UTC));
    }

    void trimsWhitespace()
    {
        const auto dt = parseSiteTime(QStringLiteral("  2026-09-24T15:59:00Z  "));
        QVERIFY(dt.has_value());
        QCOMPARE(*dt, QDateTime(QDate(2026, 9, 24), QTime(15, 59, 0), QTimeZone::UTC));
    }

    // ---- 失败情形：必须返回 nullopt，绝不用当前时间兜底 --------------------

    void rejectsEmptyAndGarbage()
    {
        QVERIFY(!parseSiteTime(QString()).has_value());
        QVERIFY(!parseSiteTime(QStringLiteral("   ")).has_value());
        QVERIFY(!parseSiteTime(QStringLiteral("not-a-timestamp")).has_value());
        QVERIFY(!parseSiteTime(QStringLiteral("2026-13-45T99:99:99Z")).has_value());
        QVERIFY(!parseSiteTime(QStringLiteral("null")).has_value());
    }

    void neverFallsBackToNow()
    {
        // 解析失败必须返回空，而不是返回「现在」——
        // 否则采集时间会被误当成发布时间/截止时间（需求明确禁止）
        const auto bad = parseSiteTime(QStringLiteral("garbage"));
        QVERIFY(!bad.has_value());
    }

    // ---- 存储格式 -----------------------------------------------------------

    void storageRoundTrips()
    {
        const QDateTime original(QDate(2026, 9, 24), QTime(15, 59, 0), QTimeZone::UTC);
        const QString stored = toStorage(original);
        QVERIFY(stored.endsWith(QLatin1Char('Z')));

        const auto back = fromStorage(stored);
        QVERIFY(back.has_value());
        QCOMPARE(*back, original);
    }

    void storageIsFixedWidthForSqlOrdering()
    {
        // 固定宽度 + 固定时区保证可直接用 SQL 字符串比较排序
        const QString a = toStorage(QDateTime(QDate(2026, 1, 2), QTime(3, 4, 5, 6), QTimeZone::UTC));
        const QString b = toStorage(QDateTime(QDate(2026, 11, 12), QTime(13, 14, 15, 16), QTimeZone::UTC));
        QCOMPARE(a.size(), b.size());
        QVERIFY(a < b);
    }

    void storageOfInvalidIsEmpty()
    {
        QCOMPARE(toStorage(QDateTime()), QString());
        QCOMPARE(toStorage(std::optional<QDateTime>()), QString());
    }

    void convertsLocalInputToUtcOnStorage()
    {
        // 无论传入什么时区的时刻，存储一律是 UTC
        const QDateTime plus8(QDate(2026, 9, 24), QTime(23, 59, 0), QTimeZone(8 * 3600));
        QCOMPARE(toStorage(plus8), QStringLiteral("2026-09-24T15:59:00.000Z"));
    }

    // ---- 展示时区（UTC+8）--------------------------------------------------

    void displaysInBeijingTime()
    {
        // 15:59 UTC == 23:59 北京时间（东八区）
        const QDateTime utc(QDate(2026, 9, 24), QTime(15, 59, 0), QTimeZone::UTC);
        QCOMPARE(toBeijingDisplay(utc), QStringLiteral("2026-09-24 23:59"));
    }

    void displayCrossesDateBoundary()
    {
        // 17:00 UTC == 次日 01:00 北京时间，日期必须进位
        const QDateTime utc(QDate(2026, 9, 24), QTime(17, 0, 0), QTimeZone::UTC);
        QCOMPARE(toBeijingDisplay(utc), QStringLiteral("2026-09-25 01:00"));
    }

    void displayOfInvalidIsEmpty()
    {
        QCOMPARE(toBeijingDisplay(QDateTime()), QString());
    }

    // ---- 相对时间描述 -------------------------------------------------------

    void describesFutureDeadline()
    {
        const QString text = describeRelativeToNow(nowUtc().addSecs(3 * 3600 + 30 * 60));
        QVERIFY(text.contains(QStringLiteral("后截止")));
        QVERIFY(text.contains(QStringLiteral("小时")));
    }

    void describesPastDeadline()
    {
        const QString text = describeRelativeToNow(nowUtc().addDays(-2));
        QVERIFY(text.contains(QStringLiteral("已截止")));
        QVERIFY(text.contains(QStringLiteral("天")));
    }

    void describeOfInvalidIsEmpty()
    {
        QCOMPARE(describeRelativeToNow(QDateTime()), QString());
    }

    void nowUtcIsUtc()
    {
        QCOMPARE(nowUtc().timeSpec(), Qt::UTC);
    }
};

QTEST_MAIN(TimeUtilsTests)
#include "TimeUtilsTests.moc"
