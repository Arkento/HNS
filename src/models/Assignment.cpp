#include "Assignment.h"

#include <QCoreApplication>

namespace njulms {

const char *assignmentStatusKey(AssignmentStatus s) noexcept
{
    // 稳定英文键：用于数据库存储与日志，不随界面语言变化
    switch (s) {
    case AssignmentStatus::Unknown:   return "unknown";
    case AssignmentStatus::Pending:   return "pending";
    case AssignmentStatus::Submitted: return "submitted";
    case AssignmentStatus::Overdue:   return "overdue";
    case AssignmentStatus::Closed:    return "closed";
    }
    return "unknown";
}

QString assignmentStatusDisplay(AssignmentStatus s)
{
    switch (s) {
    case AssignmentStatus::Unknown:   return QStringLiteral("未知");
    case AssignmentStatus::Pending:   return QStringLiteral("未提交");
    case AssignmentStatus::Submitted: return QStringLiteral("已提交");
    case AssignmentStatus::Overdue:   return QStringLiteral("未提交（已截止）");
    case AssignmentStatus::Closed:    return QStringLiteral("已关闭");
    }
    return QStringLiteral("未知");
}

std::optional<AssignmentStatus> assignmentStatusFromKey(const QString &key)
{
    if (key == QLatin1String("unknown"))   return AssignmentStatus::Unknown;
    if (key == QLatin1String("pending"))   return AssignmentStatus::Pending;
    if (key == QLatin1String("submitted")) return AssignmentStatus::Submitted;
    if (key == QLatin1String("overdue"))   return AssignmentStatus::Overdue;
    if (key == QLatin1String("closed"))    return AssignmentStatus::Closed;
    return std::nullopt;
}

} // namespace njulms
