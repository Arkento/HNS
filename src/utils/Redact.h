#pragma once

#include <QString>
#include <QUrl>

namespace njulms {
namespace redact {

/**
 * 日志脱敏。
 *
 * 需求文档第五节明确列出**不得记录**的内容：密码、Cookie、会话令牌、验证码、
 * 完整的敏感认证跳转 URL、不必要的个人信息与作业正文。
 *
 * 本模块提供集中式脱敏，所有日志写入路径都必须经过它，避免各处自行判断而遗漏。
 */

/// 把 URL 中敏感的查询参数值替换为 ***，并去掉 fragment。
/// 认证跳转 URL 的 service= / ticket= / execution= / lt= / code= 等参数一律脱敏。
QString url(const QUrl &u);
QString url(const QString &u);

/// 把 "Cookie: ..." / "Set-Cookie: ..." 这类整行替换为占位文本
QString headerLine(const QString &line);

/// 对自由文本做兜底脱敏：命中密码/令牌/验证码等关键字的值部分被替换。
/// @param mask 替换用的占位文本。默认是 "***"；URL 场景会传入一个
///             只含字母的占位符，以免 QUrl::toString() 把 '*' 编码成 %2A。
QString text(const QString &s, const QString &mask = QStringLiteral("***"));

/// 课程 ID 是否需要在日志中脱敏。默认关闭：课程 ID 本身不是敏感信息，
/// 但需求允许「如需，可进行脱敏」。保留开关便于将来按需启用。
void setMaskCourseIds(bool enabled);

/// 按当前设置格式化课程 ID
QString courseId(const QString &id);

} // namespace redact
} // namespace njulms
