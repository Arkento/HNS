#include "Redact.h"

#include <QCryptographicHash>
#include <QRegularExpression>
#include <QStringList>
#include <QUrlQuery>

namespace njulms {
namespace redact {
namespace {

constexpr QLatin1String kMask("***");

/// 需要整值脱敏的查询参数（小写比较）
const QStringList &sensitiveQueryKeys()
{
    static const QStringList keys = {
        QStringLiteral("ticket"),
        QStringLiteral("service"),
        QStringLiteral("execution"),
        QStringLiteral("lt"),
        QStringLiteral("code"),
        QStringLiteral("token"),
        QStringLiteral("access_token"),
        QStringLiteral("refresh_token"),
        QStringLiteral("id_token"),
        QStringLiteral("session"),
        QStringLiteral("sessionid"),
        QStringLiteral("session_id"),
        QStringLiteral("jsessionid"),
        QStringLiteral("castgc"),
        QStringLiteral("auth_session_id"),
        QStringLiteral("password"),
        QStringLiteral("pwd"),
        QStringLiteral("captcha"),
        QStringLiteral("state"),
        QStringLiteral("nonce"),
        QStringLiteral("relaystate"),
        QStringLiteral("samlrequest"),
        QStringLiteral("samlresponse"),
    };
    return keys;
}

bool isSensitiveKey(const QString &key)
{
    return sensitiveQueryKeys().contains(key.toLower());
}

/// 关键字后跟 = 或 : 的值，替换为 ***
///
/// ⚠️ 四个实测踩过的坑，改动前请先读：
///  1. 刻意用普通字面量拼接而不是原始字符串 R"(...)"：
///     拼接原始字符串会把 R"( 与 )" 本身当成模式内容，QRegularExpression
///     编译失败（"pattern is invalid"），脱敏会**静默失效**。
///  2. 关键字**不能**加前导 \b。站点把盐值字段写作 `pwdEncryptSalt`，
///     其前一个字符是 `d`（单词字符），\b 不存在，`encryptsalt` 永远匹配不上。
///  3. 关键字**也不能**加「前一个字符不是字母数字」的否定环视（(?<![A-Za-z0-9])）。
///     实测该环视同样会挡掉 `pwdEncryptSalt` 里的 `encryptsalt`（前面是 `d`），
///     结果是 pwd 与 encryptsalt 两条备选**全都失败**，盐值原样进入日志。
///     代价是 `compass=123` 这类普通词会被误伤 —— 这是有意接受的：
///     日志多一处 *** 无害，凭据泄漏有害。
///  4. 值里排除 & ; , 引号 与右括号，避免把整个查询串或结构一次吞掉。
///  5. 分隔符必须单独捕获成 [2]。若只捕获关键字、把分隔符写成非捕获组，
///     替换时会把它一起丢掉，得到 "service***" 而不是 "service=***"。
const QRegularExpression &kvPattern()
{
    static const QRegularExpression re(QStringLiteral(
        "(?i)"
        "(password|passwd|pwd|pass|secret|token|cookie|session"
        "|sessionid|jsessionid|castgc|ticket|captcha|authorization"
        "|set-cookie|salt|encryptsalt|service|username|code|execution|lt)"
        "(?=\\s*[=:]|%3[dD])"
        "(\\s*(?:[=:]|%3[dD])\\s*)"
        "([^\\s;&,\"'}\\]]+)"));
    return re;
}

} // namespace

QString url(const QUrl &u)
{
    if (!u.isValid()) {
        return QString();
    }

    QUrl copy = u;
    copy.setFragment(QString());  // fragment 可能含 SPA 路由参数，一律丢弃

    QUrlQuery query(copy.query());
    QUrlQuery cleaned;
    // 用 FullyEncoded 读取，避免解码后再编码时把 %2F 之类的语义改变
    for (const auto &pair : query.queryItems(QUrl::FullyEncoded)) {
        if (isSensitiveKey(pair.first)) {
            cleaned.addQueryItem(pair.first, kMask);
        } else {
            cleaned.addQueryItem(pair.first, pair.second);
        }
    }
    copy.setQuery(cleaned);

    return copy.toString(QUrl::RemoveUserInfo);
}

QString url(const QString &u)
{
    if (u.isEmpty()) {
        return QString();
    }

    // 先无条件跑一遍关键字规则（同时覆盖 "service=" 与编码后的 "service%3D"），
    // 再做结构化清理。
    //
    // 为什么是这个顺序：编码后的参数（service%3Dhttps%253A%252F%252F...）里
    // "=" 被编码，QUrl 不会把它当查询分隔符，因此**结构化处理完全看不到它**，
    // 只能靠文本规则兜住。反过来若先结构化、把值替换成 ***，再跑文本规则，
    // 就会把刚写好的 *** 连同分隔符一起吃掉，得到 "...login?service" 这种畸形结果。
    //
    // 另一个实测坑：QUrl::toString() 会把 '*' 百分号编码成 %2A，
    // 于是日志里出现 "service=%2A%2A%2A" 而不是 "service=***"。
    // 因此文本规则先用一个只含字母的占位符，结构化处理之后再换回 ***。
    static const QString placeholder = QStringLiteral("NjUlMsMaSkEd");
    const QString masked = text(u, placeholder);

    const QUrl reparsed(masked, QUrl::StrictMode);
    QString result = masked;
    if (reparsed.isValid()) {
        QUrl cleaned = reparsed;
        cleaned.setFragment(QString());  // fragment 可能含 SPA 路由参数，一律丢弃
        result = cleaned.toString(QUrl::RemoveUserInfo);
    }

    result.replace(placeholder, kMask);
    return result;
}

QString headerLine(const QString &line)
{
    const QString trimmed = line.trimmed();
    const QString lower = trimmed.toLower();
    static const QStringList sensitiveHeaders = {
        QStringLiteral("cookie"),
        QStringLiteral("set-cookie"),
        QStringLiteral("authorization"),
        QStringLiteral("proxy-authorization"),
        QStringLiteral("x-csrf-token"),
        QStringLiteral("x-xsrf-token"),
    };

    const int colon = trimmed.indexOf(QLatin1Char(':'));
    if (colon > 0) {
        const QString name = trimmed.left(colon).trimmed().toLower();
        if (sensitiveHeaders.contains(name)) {
            return trimmed.left(colon + 1) + QLatin1Char(' ') + kMask;
        }
    }
    return trimmed;
}

QString text(const QString &s, const QString &mask)
{
    if (s.isEmpty()) {
        return s;
    }
    QString out = s;
    out.replace(kvPattern(), QStringLiteral("\\1\\2") + mask);
    return out;
}

namespace {
bool g_maskCourseIds = false;
} // namespace

void setMaskCourseIds(bool enabled)
{
    g_maskCourseIds = enabled;
}

bool maskCourseIds()
{
    return g_maskCourseIds;
}

QString courseId(const QString &id)
{
    if (!g_maskCourseIds || id.isEmpty()) {
        return id;
    }
    // 保留前 2 位便于人工对照，其余用哈希替代，既可区分又不可反查
    const QByteArray digest =
        QCryptographicHash::hash(id.toUtf8(), QCryptographicHash::Sha256).toHex().left(6);
    return id.left(2) + QStringLiteral("…") + QString::fromLatin1(digest);
}

} // namespace redact
} // namespace njulms
