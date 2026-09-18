#pragma once

#include <AiLib/network/ITransport.h>
#include <QMutex>
#include <QMutexLocker>
#include <QQueue>

// 使用真实 Transport 接口预设普通或流式响应，支持并发查询和发送。
class FakeTransport final : public AiLib::ITransport {
public:
    void enqueue(const AiLib::TransportResponse&
                     response)  // 预设下一次成功传输的 HTTP 响应，可包含服务端错误状态
    {
        QMutexLocker lock(&m_mutex);  // 保护测试响应队列的局部锁
        m_replies.enqueue(Reply{true, response, {}});
    }
    void
    enqueueError(const AiLib::SdkError& error,
                 const AiLib::TransportResponse& partial = {})  // 预设一次传输故障及已收到的部分数据
    {
        QMutexLocker lock(&m_mutex);  // 保护测试响应队列的局部锁
        m_replies.enqueue(Reply{false, partial, error});
    }
    QList<AiLib::TransportRequest>
    requests() const  // 返回已发送请求的只读值副本，便于断言请求次数和编码
    {
        QMutexLocker lock(&m_mutex);  // 保护请求记录读取的局部锁
        return m_requests;
    }
    QList<int> timeouts() const  // 返回每次发送实际收到的超时秒数，验证配置覆盖不修改默认值
    {
        QMutexLocker lock(&m_mutex);  // 保护超时记录读取的局部锁
        return m_timeouts;
    }
    bool send(const AiLib::TransportRequest& request,  // Client 编码的真实传输请求
              const AiLib::RequestOptions& options,    // 本次传输选项
              AiLib::TransportResponse& response,      // 输出预设响应或部分数据
              AiLib::SdkError& error) override         // 同步消费一项队列，不依赖外部网络
    {
        QMutexLocker lock(&m_mutex);  // 保护发送记录与响应队列的一次原子操作
        m_requests.append(request);
        m_timeouts.append(options.timeoutSeconds);
        if (m_replies.isEmpty()) {
            response = {};
            error = {};
            error.category = AiLib::ErrorCategory::Internal;
            error.code = QStringLiteral("FakeResponsesExhausted");
            error.message = QStringLiteral("No preset response remains");
            return false;
        }
        const Reply reply = m_replies.dequeue();  // 本次同步调用独立消费的预设结果
        response = reply.response;
        error = reply.error;
        return reply.success;
    }

    void enqueueStream(const QList<QByteArray>& chunks,            // 按顺序交付的原始网络分片
                       const AiLib::SdkError& terminalError = {})  // 分片结束后的可选网络故障
    {
        QMutexLocker lock(&m_mutex);  // 保护响应队列
        Reply reply{terminalError.category == AiLib::ErrorCategory::None,
                    {200, {}, {}},
                    terminalError};  // 流结束状态和 HTTP 元数据
        reply.chunks = chunks;
        m_replies.enqueue(reply);
    }
    bool sendStream(const AiLib::TransportRequest& request,      // 记录的真实 HTTP 请求
                    const AiLib::RequestOptions& options,        // 当前超时及取消
                    AiLib::TransportResponse& response,          // HTTP 元数据及非成功正文
                    const AiLib::TransportDataCallback& onData,  // 调用线程中的片段接收器
                    AiLib::SdkError& error) override             // 同步重放分片，回调时不持有队列锁
    {
        Reply reply;  // 锁内消费后在调用线程独立处理的结果
        {
            QMutexLocker lock(&m_mutex);  // 只保护队列及记录，允许回调查询 Fake
            m_requests.append(request);
            m_timeouts.append(options.timeoutSeconds);
            if (m_replies.isEmpty()) {
                error.category = AiLib::ErrorCategory::Internal;
                error.code = QStringLiteral("FakeResponsesExhausted");
                return false;
            }
            reply = m_replies.dequeue();
        }
        response = reply.response;
        if (response.statusCode && (*response.statusCode < 200 || *response.statusCode >= 300)) {
            error = reply.error;
            return reply.success;
        }
        for (const auto& chunk : reply.chunks) {  // 当前待交付的原始网络片段
            if (options.cancellation.isCancellationRequested()) {
                error = {};
                error.category = AiLib::ErrorCategory::Cancelled;
                error.code = QStringLiteral("Cancelled");
                return false;
            }
            if (!onData(response, chunk, error))
                return false;
        }
        error = reply.error;
        return reply.success;
    }

private:
    // 组合测试专用传输状态、HTTP 数据、分片和故障。
    struct Reply {
        bool success = false;               // 是否完成传输，不代表 HTTP 状态成功
        AiLib::TransportResponse response;  // 本次传输数据
        AiLib::SdkError error;              // 本次传输故障
        QList<QByteArray> chunks;           // 流式传输的原始分片序列
    };
    mutable QMutex m_mutex;                     // 保护 Fake 的响应队列和发送记录
    QQueue<Reply> m_replies;                    // 依次消费的多次响应
    QList<AiLib::TransportRequest> m_requests;  // 用于断言的实际请求记录
    QList<int> m_timeouts;                      // 用于断言的本次超时配置记录
};
