#include "UiTheme.h"

#include <QApplication>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QPalette>
#include <QStyleFactory>
#include <QStyleHints>

namespace njulms {
namespace uitheme {
namespace {

// 浅色：中性偏冷（不是纯白/纯黑），单一强调色为蓝，三个语义色都满足 AA 对比度。
const Tokens kLight{
    QColor(0xf5, 0xf6, 0xf8),  // window
    QColor(0xff, 0xff, 0xff),  // surface
    QColor(0xf0, 0xf2, 0xf5),  // surfaceAlt
    QColor(0xdf, 0xe3, 0xe8),  // border
    QColor(0x1c, 0x1f, 0x23),  // text
    QColor(0x5c, 0x63, 0x6e),  // textMuted
    QColor(0x1f, 0x6f, 0xeb),  // accent
    QColor(0x17, 0x5c, 0xd3),  // accentHover
    QColor(0xff, 0xff, 0xff),  // accentText
    QColor(0x1a, 0x7f, 0x4b),  // success
    QColor(0x8a, 0x61, 0x00),  // warning
    QColor(0xff, 0xf6, 0xe0),  // warningBg
    QColor(0xb4, 0x23, 0x18),  // danger
    QColor(0xfd, 0xec, 0xea),  // dangerBg
    QColor(0x9a, 0xa2, 0xad),  // disabledText
};

// 深色：不是纯黑底，强调色改用浅蓝配深色文字（保证按钮文字对比度）。
const Tokens kDark{
    QColor(0x17, 0x19, 0x1d),  // window
    QColor(0x1e, 0x21, 0x26),  // surface
    QColor(0x23, 0x27, 0x2e),  // surfaceAlt
    QColor(0x30, 0x35, 0x40),  // border
    QColor(0xe8, 0xea, 0xee),  // text
    QColor(0xa8, 0xb0, 0xbd),  // textMuted
    QColor(0x6e, 0xa8, 0xfe),  // accent
    QColor(0x8e, 0xbc, 0xff),  // accentHover
    QColor(0x10, 0x14, 0x1a),  // accentText
    QColor(0x3f, 0xb2, 0x7f),  // success
    QColor(0xe3, 0xb3, 0x41),  // warning
    QColor(0x2a, 0x24, 0x13),  // warningBg
    QColor(0xff, 0x7b, 0x72),  // danger
    QColor(0x2d, 0x1a, 0x19),  // dangerBg
    QColor(0x6b, 0x72, 0x80),  // disabledText
};

bool resolveDark()
{
    if (QGuiApplication::styleHints() == nullptr) {
        return false;
    }
    switch (QGuiApplication::styleHints()->colorScheme()) {
    case Qt::ColorScheme::Dark:
        return true;
    case Qt::ColorScheme::Light:
        return false;
    default:
        return false;   // 未知时按浅色
    }
}

bool g_dark = false;

QString styleSheet(const Tokens &t, const QString &mono)
{
    QString qss = QStringLiteral(R"QSS(
QMainWindow, QDialog { background: @WINDOW@; }
QTabWidget::pane { border: 1px solid @BORDER@; border-radius: 10px; background: @SURFACE@; top: -1px; }
QTabWidget > QWidget { background: @SURFACE@; }
QTabBar::tab { background: transparent; color: @MUTED@; padding: 6px 16px; margin-right: 2px;
               border: 1px solid transparent; border-top-left-radius: 8px; border-top-right-radius: 8px; }
QTabBar::tab:selected { color: @TEXT@; background: @SURFACE@; border-color: @BORDER@; border-bottom-color: @SURFACE@; }
QTabBar::tab:hover:!selected { color: @TEXT@; }

QGroupBox { border: 1px solid @BORDER@; border-radius: 10px; margin-top: 14px; padding: 14px 12px 12px 12px; }
QGroupBox::title { subcontrol-origin: margin; left: 10px; top: 1px; padding: 0 5px;
                   color: @MUTED@; font-weight: 600; }

QLabel[role="muted"] { color: @MUTED@; }
QLabel[role="warning"] { color: @WARNING@; background: @WARNING_BG@; border: 1px solid @WARNING@;
                         border-left: 3px solid @WARNING@; border-radius: 8px; padding: 8px 10px; }
QLabel[role="empty"] { color: @MUTED@; padding: 18px 0; }
QLabel[state="good"] { color: @SUCCESS@; font-weight: 600; }
QLabel[state="bad"] { color: @DANGER@; font-weight: 600; }
QLabel[state="busy"] { color: @MUTED@; font-weight: 600; }
QLabel[state="warn"] { color: @WARNING@; font-weight: 600; }
QLabel[state="unknown"] { color: @TEXT@; font-weight: 600; }

QPushButton { background: @SURFACE@; color: @TEXT@; border: 1px solid @BORDER@; border-radius: 6px;
              padding: 5px 14px; min-height: 18px; }
QPushButton:hover { border-color: @ACCENT@; }
QPushButton:pressed { background: @SURFACE_ALT@; }
QPushButton:disabled { color: @DISABLED@; border-color: @BORDER@; background: transparent; }
QPushButton[variant="primary"] { background: @ACCENT@; border-color: @ACCENT@; color: @ACCENT_TEXT@; font-weight: 600; }
QPushButton[variant="primary"]:hover { background: @ACCENT_HOVER@; border-color: @ACCENT_HOVER@; }
QPushButton[variant="primary"]:disabled { background: @SURFACE_ALT@; border-color: @BORDER@; color: @DISABLED@; }
QPushButton[variant="ghost"] { background: transparent; border-color: transparent; color: @MUTED@; }
QPushButton[variant="ghost"]:hover { border-color: @BORDER@; color: @TEXT@; }
QPushButton[variant="danger"] { color: @DANGER@; border-color: @DANGER@; background: transparent; }
QPushButton[variant="danger"]:hover { background: @DANGER_BG@; }
/* 放在各变体之后：保证禁用态在所有变体上都能压住（否则"取消"这类按钮看起来仍可点） */
QPushButton:disabled, QPushButton[variant="primary"]:disabled, QPushButton[variant="danger"]:disabled,
QPushButton[variant="ghost"]:disabled { color: @DISABLED@; border-color: @BORDER@; background: transparent; }

QCheckBox { spacing: 8px; }
QSpinBox, QComboBox, QLineEdit { background: @SURFACE@; color: @TEXT@; border: 1px solid @BORDER@;
                                 border-radius: 6px; padding: 3px 6px; min-height: 18px; }
QSpinBox:focus, QComboBox:focus, QLineEdit:focus { border-color: @ACCENT@; }
QSpinBox:disabled, QComboBox:disabled, QLineEdit:disabled { color: @DISABLED@; }

QProgressBar { background: @SURFACE_ALT@; border: none; border-radius: 6px; min-height: 16px;
               text-align: center; color: @TEXT@; }
QProgressBar::chunk { background: @ACCENT@; border-radius: 6px; }

QTableWidget, QTableView { background: @SURFACE@; alternate-background-color: @SURFACE_ALT@;
                           border: 1px solid @BORDER@; border-radius: 10px; gridline-color: transparent;
                           selection-background-color: @ACCENT@; selection-color: @ACCENT_TEXT@; }
QHeaderView::section { background: @SURFACE_ALT@; color: @MUTED@; border: none;
                       border-bottom: 1px solid @BORDER@; padding: 6px 8px; font-weight: 600; }
QTableCornerButton::section { background: @SURFACE_ALT@; border: none; }

QPlainTextEdit { background: @SURFACE@; color: @TEXT@; border: 1px solid @BORDER@; border-radius: 10px;
                 padding: 6px; font-family: "@MONO@"; selection-background-color: @ACCENT@; }

QStatusBar { background: @WINDOW@; color: @MUTED@; border-top: 1px solid @BORDER@; }
QStatusBar::item { border: none; }

QScrollBar:vertical { background: transparent; width: 10px; margin: 2px; }
QScrollBar::handle:vertical { background: @BORDER@; border-radius: 5px; min-height: 24px; }
QScrollBar::handle:vertical:hover { background: @MUTED@; }
QScrollBar:horizontal { background: transparent; height: 10px; margin: 2px; }
QScrollBar::handle:horizontal { background: @BORDER@; border-radius: 5px; min-width: 24px; }
QScrollBar::add-line, QScrollBar::sub-line { width: 0; height: 0; }
QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }

QToolTip { background: @SURFACE@; color: @TEXT@; border: 1px solid @BORDER@; padding: 4px 6px; }
)QSS");

    const struct
    {
        const char *key;
        QColor value;
    } replacements[] = {
        {"@WINDOW@", t.window},       {"@SURFACE@", t.surface},
        {"@SURFACE_ALT@", t.surfaceAlt}, {"@BORDER@", t.border},
        {"@TEXT@", t.text},           {"@MUTED@", t.textMuted},
        {"@ACCENT@", t.accent},       {"@ACCENT_HOVER@", t.accentHover},
        {"@ACCENT_TEXT@", t.accentText}, {"@SUCCESS@", t.success},
        {"@WARNING@", t.warning},     {"@WARNING_BG@", t.warningBg},
        {"@DANGER@", t.danger},       {"@DANGER_BG@", t.dangerBg},
        {"@DISABLED@", t.disabledText},
    };
    for (const auto &item : replacements) {
        qss.replace(QString::fromLatin1(item.key), item.value.name());
    }
    qss.replace(QStringLiteral("@MONO@"), mono);
    return qss;
}

QPalette buildPalette(const Tokens &t)
{
    QPalette p;
    p.setColor(QPalette::Window, t.window);
    p.setColor(QPalette::WindowText, t.text);
    p.setColor(QPalette::Base, t.surface);
    p.setColor(QPalette::AlternateBase, t.surfaceAlt);
    p.setColor(QPalette::Text, t.text);
    p.setColor(QPalette::Button, t.surface);
    p.setColor(QPalette::ButtonText, t.text);
    p.setColor(QPalette::Highlight, t.accent);
    p.setColor(QPalette::HighlightedText, t.accentText);
    p.setColor(QPalette::ToolTipBase, t.surface);
    p.setColor(QPalette::ToolTipText, t.text);
    p.setColor(QPalette::PlaceholderText, t.textMuted);
    p.setColor(QPalette::Disabled, QPalette::Text, t.disabledText);
    p.setColor(QPalette::Disabled, QPalette::ButtonText, t.disabledText);
    p.setColor(QPalette::Disabled, QPalette::WindowText, t.disabledText);
    return p;
}

void applyOnce(QApplication *app)
{
    g_dark = resolveDark();
    const Tokens &t = tokens();
    app->setPalette(buildPalette(t));
    app->setStyleSheet(styleSheet(t, monospaceFamily()));
}

} // namespace

bool isDark()
{
    return g_dark;
}

const Tokens &tokens()
{
    return g_dark ? kDark : kLight;
}

QString monospaceFamily()
{
    const QStringList preferred = {QStringLiteral("Cascadia Mono"),
                                   QStringLiteral("Consolas"),
                                   QStringLiteral("DejaVu Sans Mono"),
                                   QStringLiteral("Courier New")};
    const QStringList available = QFontDatabase::families();
    for (const QString &family : preferred) {
        if (available.contains(family)) {
            return family;
        }
    }
    return QFontDatabase::systemFont(QFontDatabase::FixedFont).family();
}

void apply(QApplication *app)
{
    if (app == nullptr) {
        return;
    }
    if (QStyle *fusion = QStyleFactory::create(QStringLiteral("Fusion"))) {
        app->setStyle(fusion);
    }
    applyOnce(app);

    // 跟随系统深浅色切换（Windows 中切换主题后立即生效）
    QObject::connect(app->styleHints(), &QStyleHints::colorSchemeChanged, app,
                     [app](Qt::ColorScheme) { applyOnce(app); });
}

} // namespace uitheme
} // namespace njulms
