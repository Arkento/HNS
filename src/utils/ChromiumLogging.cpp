#include "utils/ChromiumLogging.h"

#include <QByteArray>
#include <QtGlobal>

namespace njulms {

void quietChromiumInfoLogs()
{
    const QByteArray logFlag = QByteArrayLiteral("--log-level=2");

    if (!qEnvironmentVariableIsSet("QTWEBENGINE_CHROMIUM_FLAGS")) {
        qputenv("QTWEBENGINE_CHROMIUM_FLAGS", logFlag);
        return;
    }

    QByteArray flags = qgetenv("QTWEBENGINE_CHROMIUM_FLAGS");
    if (flags.contains("--log-level")) {
        return; // 调用方显式指定了级别，尊重它
    }

    flags += ' ';
    flags += logFlag;
    qputenv("QTWEBENGINE_CHROMIUM_FLAGS", flags);
}

} // namespace njulms
