#pragma once

#include <QList>
#include <QMetaType>
#include <QString>
#include <QStringList>
#include <QStringView>

#include <utility>

namespace njulms {

/**
 * 单个字段的采集结果状态。
 *
 * 对应需求文档「Parser 职责」中必须区分的三种情况：
 *   - Absent  : 字段确实为空（源数据明确给出 null / 空串），这是**权威结论**，允许写入 NULL
 *   - Missing : 字段未加载（响应被截断、接口本次没返回该 key），**不得**覆盖库中已有有效值
 *   - Invalid : 字段存在但解析失败（类型不符、时间格式非法），**不得**覆盖库中已有有效值
 *
 * 之所以必须区分这三者：把 Missing / Invalid 当成 Absent 写入 NULL，就会用「解析失败」
 * 静默抹掉「上次解析成功」的有效数据（需求场景 F）。
 */
enum class FieldState {
    Absent,   ///< 确认空值，可安全写入 NULL
    Missing,  ///< 未加载，保留库中原值
    Invalid,  ///< 解析失败，保留库中原值并记入解析异常
    Present,  ///< 成功解析出值
};

/// 字段状态是否允许把结果写入数据库（true = 该字段本次有权威结论）
inline bool isAuthoritative(FieldState s) noexcept
{
    return s == FieldState::Present || s == FieldState::Absent;
}

/// 便于日志输出的稳定短名（不随翻译变化）
inline const char *fieldStateName(FieldState s) noexcept
{
    switch (s) {
    case FieldState::Absent:  return "absent";
    case FieldState::Missing: return "missing";
    case FieldState::Invalid: return "invalid";
    case FieldState::Present: return "present";
    }
    return "unknown";
}

/**
 * 一次解析过程中发现的字段级问题。
 *
 * 写入 parse_errors 表，供 UI 展示「哪些字段没解析到」并支持之后重试。
 * 只记录字段名与错误类型，**不记录字段正文**（避免把作业内容写进日志）。
 */
struct ParseIssue {
    QString field;    ///< 字段名，例如 "dueAt"
    QString reason;   ///< 简短原因，例如 "unrecognized time format"
    FieldState state = FieldState::Invalid;

    ParseIssue() = default;

    /// field 接受 QStringView：解析器里的字段名来自 QStringView 常量，
    /// 这样调用点不必先构造 QString 临时对象。
    ParseIssue(QStringView f, QString r, FieldState s)
        : field(f.toString()), reason(std::move(r)), state(s)
    {
    }

    ParseIssue(QString f, QString r, FieldState s)
        : field(std::move(f)), reason(std::move(r)), state(s)
    {
    }
};

using ParseIssues = QList<ParseIssue>;

} // namespace njulms

// ParseIssues 会经由 SyncManager 的信号传递，因此需要注册为元类型。
Q_DECLARE_METATYPE(njulms::ParseIssue)
Q_DECLARE_METATYPE(njulms::ParseIssues)
