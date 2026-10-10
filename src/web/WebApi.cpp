#include "web/WebApi.h"

namespace njulms {

WebApi::WebApi(WebSources sources, QObject *parent)
    : QObject(parent), m_sources(std::move(sources))
{
}

QJsonObject WebApi::status()
{
    return run([this]() {
        return m_sources.status != nullptr ? m_sources.status() : QJsonObject();
    });
}

QJsonArray WebApi::courses()
{
    return run([this]() {
        return m_sources.courses != nullptr ? m_sources.courses() : QJsonArray();
    });
}

QJsonArray WebApi::assignments(const QString &courseId)
{
    return run([this, courseId]() {
        return m_sources.assignments != nullptr ? m_sources.assignments(courseId) : QJsonArray();
    });
}

QJsonArray WebApi::assignmentsByScope(const QString &scope)
{
    return run([this, scope]() {
        return m_sources.assignmentsByScope != nullptr ? m_sources.assignmentsByScope(scope)
                                                       : QJsonArray();
    });
}

QString WebApi::logTail(int lines)
{
    return run([this, lines]() {
        return m_sources.logTail != nullptr ? m_sources.logTail(lines) : QString();
    });
}

WebActionResult WebApi::patchSettings(const QJsonObject &patch)
{
    return run([this, patch]() {
        if (m_sources.patchSettings == nullptr) {
            return WebActionResult{false, 501, QStringLiteral("该实例没有实现设置写入"), {}};
        }
        return m_sources.patchSettings(patch);
    });
}

WebActionResult WebApi::startSync(const QString &mode)
{
    return run([this, mode]() {
        if (m_sources.startSync == nullptr) {
            return WebActionResult{false, 501, QStringLiteral("该实例没有实现同步触发"), {}};
        }
        return m_sources.startSync(mode);
    });
}

WebActionResult WebApi::pauseSync()
{
    return run([this]() {
        return m_sources.pauseSync != nullptr
                   ? m_sources.pauseSync()
                   : WebActionResult{false, 501, QStringLiteral("未实现"), {}};
    });
}

WebActionResult WebApi::resumeSync()
{
    return run([this]() {
        return m_sources.resumeSync != nullptr
                   ? m_sources.resumeSync()
                   : WebActionResult{false, 501, QStringLiteral("未实现"), {}};
    });
}

WebActionResult WebApi::cancelSync()
{
    return run([this]() {
        return m_sources.cancelSync != nullptr
                   ? m_sources.cancelSync()
                   : WebActionResult{false, 501, QStringLiteral("未实现"), {}};
    });
}

WebActionResult WebApi::purgeData(const QString &confirm)
{
    return run([this, confirm]() {
        return m_sources.purgeData != nullptr
                   ? m_sources.purgeData(confirm)
                   : WebActionResult{false, 501, QStringLiteral("未实现"), {}};
    });
}

} // namespace njulms
