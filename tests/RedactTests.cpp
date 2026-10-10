#include "utils/Redact.h"

#include <QTest>
#include <QUrl>

using namespace njulms;

/**
 * 日志脱敏测试。
 *
 * 对应需求文档第五节「日志不得记录：密码、Cookie、会话令牌、验证码、
 * 完整的敏感认证跳转 URL」。这些用例是**回归防线**：任何一处脱敏失效都会让
 * 凭据进入日志文件。
 */
class RedactTests : public QObject
{
    Q_OBJECT

private slots:

    // ---- URL 查询参数 -------------------------------------------------------

    void redactsCasTicket()
    {
        // 典型的 CAS 回跳 URL：ticket 是一次性凭据
        const QString out = redact::url(
            QStringLiteral("https://lms.nju.edu.cn/cas?ticket=ST-123456-abcdef&next=/user/index"));
        QVERIFY(!out.contains(QStringLiteral("ST-123456-abcdef")));
        QVERIFY(out.contains(QStringLiteral("***")));
        // 非敏感参数应保留，便于排查
        QVERIFY(out.contains(QStringLiteral("next")));
    }

    void redactsServiceParameter()
    {
        // service 参数里可能带编码后的完整认证跳转地址
        const QString out = redact::url(
            QStringLiteral("https://authserver.nju.edu.cn/authserver/login"
                           "?service=https%3A%2F%2Flms-identity.nju.edu.cn%2Fauth%2Frealms%2Fnju"));
        QVERIFY(!out.contains(QStringLiteral("lms-identity.nju.edu.cn")));
        QVERIFY(out.contains(QStringLiteral("***")));
    }

    void redactsSessionAndTokenParameters()
    {
        const QString out = redact::url(
            QStringLiteral("https://lms.nju.edu.cn/x?session=abc123&access_token=tok456"
                           "&code=xyz789&execution=e1s1&lt=LT-999"));
        for (const QString &secret :
             {QStringLiteral("abc123"), QStringLiteral("tok456"), QStringLiteral("xyz789"),
              QStringLiteral("e1s1"), QStringLiteral("LT-999")}) {
            QVERIFY2(!out.contains(secret), qPrintable(QStringLiteral("leaked: ") + secret));
        }
    }

    void redactsUsernameParameter()
    {
        const QString out = redact::url(QStringLiteral("https://example/x?username=2026000001"));
        QVERIFY(!out.contains(QStringLiteral("2026000001")));
    }

    void stripsFragment()
    {
        // SPA 路由片段可能含一次性参数，直接丢弃
        const QString out =
            redact::url(QStringLiteral("https://lms.nju.edu.cn/user/index#/token=secret"));
        QVERIFY(!out.contains(QStringLiteral("secret")));
        QVERIFY(!out.contains(QLatin1Char('#')));
    }

    void removesUserInfo()
    {
        // URL 里内嵌的 user:password 必须被去掉
        const QUrl u(QStringLiteral("https://user:secretpw@lms.nju.edu.cn/api/x"));
        const QString out = redact::url(u);
        QVERIFY(!out.contains(QStringLiteral("secretpw")));
        QVERIFY(!out.contains(QStringLiteral("user:")));
    }

    void keepsHarmlessUrlIntact()
    {
        const QString out = redact::url(QStringLiteral("https://lms.nju.edu.cn/api/courses/11311"));
        QCOMPARE(out, QStringLiteral("https://lms.nju.edu.cn/api/courses/11311"));
    }

    void keepsUrlShapeWhenMasking()
    {
        // 两个曾经出过问题的点：
        //   1. 结构化脱敏后又被整体跑了一遍关键字规则，把
        //      "service=***" 里已经脱敏的值再吃掉，得到 "...login?service"；
        //   2. 编码后的 service%3D... 因为 "=" 被编码而漏过关键字规则。
        const QString plain = redact::url(
            QStringLiteral("https://authserver.nju.edu.cn/authserver/login"
                           "?service=https%3A%2F%2Flms.nju.edu.cn%2Fuser%2Findex&locale=zh_CN"));
        QVERIFY2(!plain.contains(QStringLiteral("lms.nju.edu.cn")), "service value must be masked");
        QVERIFY2(plain.contains(QStringLiteral("service=***")),
                 qPrintable(QStringLiteral("masked value must survive as ***; actual=[%1]").arg(plain)));
        QVERIFY2(plain.contains(QStringLiteral("locale=zh_CN")),
                 "non-sensitive parameters must be preserved for diagnostics");

        const QString encoded = redact::url(
            QStringLiteral("https://authserver.nju.edu.cn/authserver/login"
                           "?service%3Dhttps%253A%252F%252Flms-identity.nju.edu.cn%252Fauth"));
        QVERIFY2(!encoded.contains(QStringLiteral("lms-identity.nju.edu.cn")),
                 "URL-encoded service value must also be masked");
    }

    void handlesInvalidUrlAsFreeText()
    {
        // 非 URL 字符串走文本兜底，不应崩溃
        const QString out = redact::url(QStringLiteral("password=hunter2"));
        QVERIFY(!out.contains(QStringLiteral("hunter2")));
    }

    // ---- 请求头 -------------------------------------------------------------

    void redactsCookieHeaders()
    {
        const QString cookie = redact::headerLine(
            QStringLiteral("Cookie: session=abc123; KEYCLOAK_IDENTITY=eyJhbGciOi...; CASTGC=TGT-1"));
        QVERIFY(!cookie.contains(QStringLiteral("abc123")));
        QVERIFY(!cookie.contains(QStringLiteral("TGT-1")));
        QVERIFY(cookie.contains(QStringLiteral("***")));
    }

    void redactsSetCookieHeaders()
    {
        const QString line = redact::headerLine(
            QStringLiteral("Set-Cookie: session=deadbeef; Path=/; HttpOnly"));
        QVERIFY(!line.contains(QStringLiteral("deadbeef")));
    }

    void redactsAuthorizationHeader()
    {
        const QString line =
            redact::headerLine(QStringLiteral("Authorization: Bearer eyJhbGciOiJIUzI1NiJ9.payload"));
        QVERIFY(!line.contains(QStringLiteral("eyJhbGciOiJIUzI1NiJ9")));
    }

    void keepsOrdinaryHeaders()
    {
        const QString line = redact::headerLine(QStringLiteral("Content-Type: application/json"));
        QCOMPARE(line, QStringLiteral("Content-Type: application/json"));
    }

    // ---- 自由文本兜底 -------------------------------------------------------

    void redactsPasswordAssignments()
    {
        const QString out = redact::text(
            QStringLiteral("login attempt with password=hunter2 and pwd: swordfish"));
        QVERIFY(!out.contains(QStringLiteral("hunter2")));
        QVERIFY(!out.contains(QStringLiteral("swordfish")));
    }

    void redactsSaltAndTokenLikeKeys()
    {
        const QString out = redact::text(
            QStringLiteral("pwdEncryptSalt=AbC123 token=xyzzy captcha=9182"));
        QVERIFY(!out.contains(QStringLiteral("AbC123")));
        QVERIFY(!out.contains(QStringLiteral("xyzzy")));
        QVERIFY(!out.contains(QStringLiteral("9182")));
    }

    void redactsUrlEncodedSeparators()
    {
        // 实测踩到的真实泄漏路径：Chromium 打印的 JS 控制台消息里，
        // 认证跳转 URL 的参数被 URL 编码成 service%3D...，
        // 只认 "=" 的规则完全匹配不上，整条认证 URL 会原样落盘。
        const QString raw = QStringLiteral(
            "https://authserver.nju.edu.cn/authserver/login"
            "?service%3Dhttps%253A%252F%252Flms-identity.nju.edu.cn%252Fauth"
            "&locale%3Dzh_CN");
        const QString out = redact::text(raw);
        QVERIFY2(!out.contains(QStringLiteral("lms-identity.nju.edu.cn")),
                 "URL-encoded service parameter must not survive redaction");
    }

    void redactsOrdinarySeparatorsToo()
    {
        const QString out = redact::text(QStringLiteral("service=https://idp.example/cas&x=1"));
        QVERIFY(!out.contains(QStringLiteral("idp.example")));
    }

    void leavesOrdinaryTextAlone()
    {
        const QString original = QStringLiteral("parsed 3 assignments for course 90001 in 412 ms");
        QCOMPARE(redact::text(original), original);
    }

    // ---- 课程 ID 脱敏开关 ---------------------------------------------------

    void courseIdMaskingIsOptional()
    {
        redact::setMaskCourseIds(false);
        QCOMPARE(redact::courseId(QStringLiteral("11311")), QStringLiteral("11311"));

        redact::setMaskCourseIds(true);
        const QString masked = redact::courseId(QStringLiteral("11311"));
        QVERIFY(masked != QStringLiteral("11311"));
        QVERIFY(masked.startsWith(QStringLiteral("11")));  // 保留前两位便于对照
        QVERIFY(masked.contains(QStringLiteral("…")));

        // 同一输入必须稳定映射到同一输出（否则日志无法关联同一次运行）
        QCOMPARE(redact::courseId(QStringLiteral("11311")), masked);
        // 不同输入必须可区分
        QVERIFY(redact::courseId(QStringLiteral("11312")) != masked);

        redact::setMaskCourseIds(false);
        QCOMPARE(redact::courseId(QStringLiteral("11311")), QStringLiteral("11311"));
    }
};

QTEST_MAIN(RedactTests)
#include "RedactTests.moc"
