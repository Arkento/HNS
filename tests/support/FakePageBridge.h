#pragma once

#include "adapters/IPageBridge.h"

#include <QHash>
#include <QString>
#include <QStringList>

namespace njulms {
namespace test {

/**
 * IPageBridge 的测试替身：按路径回放预先准备好的响应。
 *
 * 存在的意义：让 Adapter / Parser / Repository / SyncManager 可以在
 * **没有 Qt WebEngine、没有网络、没有真实账号**的条件下完成单元测试。
 * 这正是把网站专属逻辑与浏览器会话解耦的直接收益。
 */
class FakePageBridge : public IPageBridge
{
public:
    explicit FakePageBridge(QObject *parent = nullptr) : IPageBridge(parent) {}

    /// 一次预置的响应。transportOk=false 时 body 字段承载错误描述。
    struct Entry
    {
        QString body;
        int httpStatus = 200;
        bool transportOk = true;
    };

    /// 注册一个路径（不含查询串）对应的响应体
    void setResponse(const QString &path, const QString &body, int httpStatus = 200)
    {
        m_responses.insert(path, Entry{body, httpStatus, true});
    }

    /// 注册一个「传输层失败」（模拟网络中断）
    void setTransportFailure(const QString &path, const QString &error)
    {
        m_responses.insert(path, Entry{error, 0, false});
    }

    /// 让某个路径重复返回不同响应（第 n 次调用取第 n 个，超出则重复最后一个）
    void setSequentialResponses(const QString &path, const QList<Entry> &entries)
    {
        m_sequences.insert(path, entries);
        m_callCounts.insert(path, 0);
    }

    void setReady(bool ready) { m_ready = ready; }
    void setUrl(const QString &url) { m_url = url; }

    /// 记录收到的请求，便于断言适配器发出的路径与方法
    const QList<Request> &receivedRequests() const { return m_received; }
    int requestCount() const { return m_received.size(); }
    void clearReceived() { m_received.clear(); m_callCounts.clear(); }

    // ---- IPageBridge ----
    void request(const Request &req, Callback callback) override
    {
        m_received.append(req);

        // 去掉查询串后匹配，便于断言路径本身
        const QString key = req.path.section(QLatin1Char('?'), 0, 0);

        if (m_sequences.contains(key)) {
            const int index = m_callCounts.value(key, 0);
            const QList<Entry> &seq = m_sequences.value(key);
            m_callCounts.insert(key, index + 1);
            const Entry entry = seq.isEmpty() ? Entry{} : seq.at(qMin(index, seq.size() - 1));
            emitResponse(entry, callback);
            return;
        }

        if (m_responses.contains(key)) {
            emitResponse(m_responses.value(key), callback);
            return;
        }

        // ---- v4 兼容：考试接入后，SyncManager 在作业之后总会请求 /exam-list ----
        // 绝大多数用例不关心考试；这里默认回放"空考试列表"，除非显式注册了该路径
        // （setResponse / setSequentialResponses 优先，先命中前面的分支）。
        // 这样每个既有用例不必逐一声明这个它不关心的接口。
        if (key.endsWith(QLatin1String("/exam-list"))) {
            Entry empty;
            empty.body = QStringLiteral(
                R"({"exams":[],"total":0,"page":1,"pages":1,"page_size":50})");
            emitResponse(empty, callback);
            return;
        }

        Response res;
        res.transportOk = false;
        res.error = QStringLiteral("FakePageBridge: no response registered for '%1'").arg(key);
        callback(res);
    }

    bool isReady() const override { return m_ready; }
    QString currentUrl() const override { return m_url; }

private:
    void emitResponse(const Entry &entry, const Callback &callback)
    {
        Response res;
        res.transportOk = entry.transportOk;
        res.httpStatus = entry.httpStatus;
        res.body = entry.body;
        if (!entry.transportOk) {
            res.error = entry.body;  // 传输失败时 body 字段承载错误描述
        }
        callback(res);
    }

    QHash<QString, Entry> m_responses;
    QHash<QString, QList<Entry>> m_sequences;
    QHash<QString, int> m_callCounts;
    QList<Request> m_received;
    bool m_ready = true;
    QString m_url = QStringLiteral("https://lms.nju.edu.cn/user/index#/");
};

} // namespace test
} // namespace njulms
