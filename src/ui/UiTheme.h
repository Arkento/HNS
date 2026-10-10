#pragma once

#include <QColor>
#include <QString>

QT_BEGIN_NAMESPACE
class QApplication;
QT_END_NAMESPACE

namespace njulms {
namespace uitheme {

/**
 * 界面设计令牌（单一事实来源）。
 *
 * 为什么需要它：此前界面里的颜色是散落在各处的硬编码十六进制（#555 / #fcf8e3 /
 * #a94442 等），在深色主题下会出现"灰字贴黑底"这种对比度不达标的情况，
 * 同一种语义颜色也有三个不同取值。
 *
 * 现在收敛为一套令牌：中性色分级 + 一个强调色 + 三个语义色（成功/警告/危险），
 * 深浅两套值一一对应，界面只引用语义名，不写死颜色。
 */
struct Tokens
{
    QColor window;        ///< 窗口底色
    QColor surface;       ///< 卡片/输入框底色
    QColor surfaceAlt;    ///< 斑马纹、表头等次一级底色
    QColor border;        ///< 分隔线/描边
    QColor text;          ///< 主文本
    QColor textMuted;     ///< 次要文本（仍满足 WCAG AA）
    QColor accent;        ///< 唯一强调色（主操作、选中态、焦点环）
    QColor accentHover;
    QColor accentText;    ///< 强调色上的文字
    QColor success;       ///< 语义：成功/已登录
    QColor warning;       ///< 语义：注意
    QColor warningBg;     ///< 注意条底色
    QColor danger;        ///< 语义：危险/失败
    QColor dangerBg;
    QColor disabledText;
};

/// 当前是否深色（跟随系统；无法判断时按浅色处理）
bool isDark();

/// 当前主题的令牌
const Tokens &tokens();

/// 等宽字体族（日志、来源 ID、时间列使用）
QString monospaceFamily();

/**
 * 安装主题：Fusion 风格 + 调色板 + 全局样式表。
 *
 * 会在系统深浅色切换时自动重新应用（QStyleHints::colorSchemeChanged）。
 * 必须在创建窗口之前调用。
 */
void apply(QApplication *app);

} // namespace uitheme
} // namespace njulms
