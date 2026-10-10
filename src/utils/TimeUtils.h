#pragma once

#include <QDateTime>
#include <QString>

#include <optional>

namespace njulms {
namespace timeutil {

/// 站点返回的时间统一带 'Z'（UTC）后缀。本系统内部一律以 UTC 表示时刻。

/**
 * 解析站点返回的 ISO 8601 时间字符串。
 *
 * 实测样例： "2026-09-17T02:14:44Z"、"2026-09-24T15:59:00Z"
 * 兼容处理： 带/不带小数秒、带/不带 'Z'、带 "+08:00" 形式偏移、首尾空白。
 *
 * ⚠️ 对不带时区标识的字符串按 **UTC** 解释，而不是本地时间——站点所有时间都是 UTC，
 * 若按本地时间解释会整体偏移 8 小时，进而导致「截止时间」显示错误。
 *
 * @return 解析成功则返回 UTC 时刻；失败返回 nullopt，调用方应记为 FieldState::Invalid，
 *         **不得**用当前时间兜底。
 */
std::optional<QDateTime> parseSiteTime(const QString &raw);

/**
 * 把时刻序列化为存储用的稳定字符串：UTC、毫秒精度、以 'Z' 结尾。
 *
 * 固定宽度与固定时区保证字符串可以直接用 SQL 比较排序。
 */
QString toStorage(const QDateTime &dt);

/// 序列化 std::optional 版本；空值返回空字符串
QString toStorage(const std::optional<QDateTime> &dt);

/// 从存储字符串还原（测试与 Repository 用）
std::optional<QDateTime> fromStorage(const QString &stored);

/// 当前 UTC 时刻，统一入口便于测试替换
QDateTime nowUtc();

/**
 * 北京时间的展示格式（UTC+8）。
 *
 * 注意：这是**展示层**转换，数据库里始终存 UTC。需求要求「时间应使用明确的时区语义」，
 * 因此存储与展示严格分离，展示时显式标注时区。
 */
QString toBeijingDisplay(const QDateTime &utc, const QString &format = QStringLiteral("yyyy-MM-dd HH:mm"));

/// 相对当前时间的可读描述，例如「3 小时后截止」「已截止 2 天」
QString describeRelativeToNow(const QDateTime &utc);

} // namespace timeutil
} // namespace njulms
