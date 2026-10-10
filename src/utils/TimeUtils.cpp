#include "TimeUtils.h"

#include <QRegularExpression>
#include <QTimeZone>

namespace njulms {
namespace timeutil {
namespace {

/// 北京时间的固定偏移（UTC+8）。中国大陆不实行夏令时，用固定偏移即可。
constexpr int kBeijingOffsetSeconds = 8 * 3600;

QTimeZone beijingZone()
{
    // 优先使用 IANA 时区数据库；不可用时退回固定偏移。
    const QTimeZone iana("Asia/Shanghai");
    return iana.isValid() ? iana : QTimeZone(kBeijingOffsetSeconds);
}

} // namespace

std::optional<QDateTime> parseSiteTime(const QString &raw)
{
    const QString s = raw.trimmed();
    if (s.isEmpty()) {
        return std::nullopt;
    }

    // 主路径：Qt 自带的 ISO 解析。Qt 6 的 Qt::ISODate 已能处理 'Z' 与小数秒。
    {
        QDateTime dt = QDateTime::fromString(s, Qt::ISODate);
        if (dt.isValid()) {
            // 站点全部时间为 UTC；若无时区标识，Qt 会按本地时区解释，这里强制纠正为 UTC。
            if (dt.timeSpec() == Qt::LocalTime) {
                dt.setTimeZone(QTimeZone::UTC);
            }
            return dt.toUTC();
        }
    }

    // 兼容路径：ISO 但带 "2026-09-17T02:14:44.000+0000" 这类无冒号偏移，
    // 以及 Qt::ISODateWithMs 能处理但 ISODate 不能处理的情形。
    {
        QDateTime dt = QDateTime::fromString(s, Qt::ISODateWithMs);
        if (dt.isValid()) {
            if (dt.timeSpec() == Qt::LocalTime) {
                dt.setTimeZone(QTimeZone::UTC);
            }
            return dt.toUTC();
        }
    }

    // 兜底路径：手工解析 "yyyy-MM-ddTHH:mm:ss[.fff][Z|±HH:MM|±HHMM]"。
    static const QRegularExpression re(
        QStringLiteral(R"(^(\d{4})-(\d{2})-(\d{2})[T ](\d{2}):(\d{2}):(\d{2})(?:\.(\d{1,9}))?)"
                       R"(\s*(Z|z|[+-]\d{2}:?\d{2})?$)"));
    const QRegularExpressionMatch m = re.match(s);
    if (!m.hasMatch()) {
        return std::nullopt;
    }

    QDate date(int(m.captured(1).toInt()), int(m.captured(2).toInt()), int(m.captured(3).toInt()));
    QTime time(int(m.captured(4).toInt()), int(m.captured(5).toInt()), int(m.captured(6).toInt()));
    const QString frac = m.captured(7);
    if (!frac.isEmpty()) {
        // 补齐到毫秒
        QString ms = frac.left(3);
        while (ms.size() < 3) {
            ms.append(QLatin1Char('0'));
        }
        time = time.addMSecs(ms.toInt());
    }
    if (!date.isValid() || !time.isValid()) {
        return std::nullopt;
    }

    QDateTime dt(date, time, QTimeZone::UTC);

    const QString offset = m.captured(8);
    if (!offset.isEmpty() && offset.compare(QLatin1String("Z"), Qt::CaseInsensitive) != 0) {
        QString o = offset;
        o.remove(QLatin1Char(':'));
        const int sign = o.startsWith(QLatin1Char('-')) ? -1 : 1;
        const int hours = o.mid(1, 2).toInt();
        const int minutes = o.mid(3, 2).toInt();
        dt = dt.addSecs(-sign * (hours * 3600 + minutes * 60));
    }

    return dt.toUTC();
}

QString toStorage(const QDateTime &dt)
{
    if (!dt.isValid()) {
        return QString();
    }
    return dt.toUTC().toString(Qt::ISODateWithMs);
}

QString toStorage(const std::optional<QDateTime> &dt)
{
    return dt.has_value() ? toStorage(*dt) : QString();
}

std::optional<QDateTime> fromStorage(const QString &stored)
{
    if (stored.isEmpty()) {
        return std::nullopt;
    }
    return parseSiteTime(stored);
}

QDateTime nowUtc()
{
    return QDateTime::currentDateTimeUtc();
}

QString toBeijingDisplay(const QDateTime &utc, const QString &format)
{
    if (!utc.isValid()) {
        return QString();
    }
    return utc.toTimeZone(beijingZone()).toString(format);
}

QString describeRelativeToNow(const QDateTime &utc)
{
    if (!utc.isValid()) {
        return QString();
    }

    const QDateTime now = nowUtc();
    const qint64 secs = now.secsTo(utc);
    const bool future = secs >= 0;
    const qint64 absSecs = future ? secs : -secs;

    const qint64 days = absSecs / 86400;
    const qint64 hours = (absSecs % 86400) / 3600;
    const qint64 minutes = (absSecs % 3600) / 60;

    QString magnitude;
    if (days > 0) {
        magnitude = QStringLiteral("%1 天 %2 小时").arg(days).arg(hours);
    } else if (hours > 0) {
        magnitude = QStringLiteral("%1 小时 %2 分钟").arg(hours).arg(minutes);
    } else {
        magnitude = QStringLiteral("%1 分钟").arg(minutes);
    }

    return future ? QStringLiteral("%1后截止").arg(magnitude)
                  : QStringLiteral("已截止 %1").arg(magnitude);
}

} // namespace timeutil
} // namespace njulms
