#include <AiLib/client/LLMClient.h>
#include <utility>
#include <AiLib/stream/StreamSession.h>
#include <exception>
#include <thread>
#include <algorithm>

namespace AiLib {
namespace {
bool checkRequestStop(const RequestOptions& options,  // 本次执行配置
                      SdkError& error)                // 沿网络与流处理边界检查唯一取消来源及精确截止时间
{
    const bool cancelled = options.cancellation.isCancellationRequested();  // 主动取消优先
    if (!cancelled && !options.isDeadlineExpired())
        return false;
    error = {};
    error.category = cancelled ? ErrorCategory::Cancelled : ErrorCategory::Timeout;
    error.code = cancelled ? QStringLiteral("Cancelled") : QStringLiteral("Timeout");
    error.message = cancelled ? QStringLiteral("Request cancelled")
                              : QStringLiteral("Request deadline expired");
    return true;
}
bool canRetry(const RetryPolicy& policy,  // 本次自动重试策略
              const SdkError& error)      // 只重试临时连接、限流和服务端失败，禁止配置/协议/取消等重试
{
    if (error.category != ErrorCategory::Network && error.category != ErrorCategory::RateLimited &&
        error.category != ErrorCategory::Provider)
        return false;
    if (error.httpStatus) {
        if (*error.httpStatus == 429)
            return policy.retryOnRateLimit;
        if (*error.httpStatus >= 500 && *error.httpStatus <= 599)
            return policy.retryOnServerError;
        if (*error.httpStatus >= 400)
            return false;
    }
    return error.category == ErrorCategory::Network && policy.retryOnNetworkError &&
           error.details.value(QStringLiteral("retryable"), true).toBool();
}
bool waitForRetry(qint64 delayMs,                 // 本次等待的总毫秒数
                  const RequestOptions& options,  // 本次执行配置
                  SdkError& error)                // 单调时钟同步等待，取消和截止时间可中途停止
{
    const auto started =  // 整个等待区间的起点，避免分段取整累计误差
        std::chrono::steady_clock::now();
    for (;;) {
        if (checkRequestStop(options, error))
            return false;
        const auto elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(  // 实际累计等待毫秒数
                std::chrono::steady_clock::now() - started)
                .count();
        if (elapsed >= delayMs)
            return true;
        const qint64 remaining = delayMs - elapsed;         // 服务端建议或固定间隔的剩余预算
        qint64 interval = std::min<qint64>(remaining, 20);  // 每段最多等待 20ms，不创建线程
        if (options.deadline) {
            const auto budget =
                std::chrono::duration_cast<std::chrono::milliseconds>(  // 调用链剩余整毫秒
                    *options.deadline - std::chrono::steady_clock::now())
                    .count();
            interval = std::min(interval, std::max<qint64>(1, budget));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(interval));
    }
}
bool hasEffectiveContent(const StreamEvent& event)  // 明确有效内容边界，元数据和空文本不阻止重试
{
    if (event.type == StreamEventType::TextDelta || event.type == StreamEventType::ReasoningDelta)
        return !event.delta.isEmpty();
    if (event.type == StreamEventType::ToolCallDelta)
        return !event.delta.isEmpty() || !event.toolNameDelta.isEmpty() ||
               (event.toolCallId && !event.toolCallId->isEmpty());
    if (event.type == StreamEventType::PartStarted)
        return event.partType == StreamPartType::ToolCall;
    if (event.type == StreamEventType::UsageUpdated)
        return event.usage.inputTokens || event.usage.outputTokens || event.usage.totalTokens;
    return event.type == StreamEventType::FinishReasonReceived ||
           event.type == StreamEventType::ResponseCompleted;
}
void reportFinalStreamError(const StreamCallback& callback,  // 应用同步事件接收器
                            const SdkError& error)           // 只通知最终失败，临时尝试错误不混入最终响应流
{
    if (!callback)
        return;
    StreamEvent event;  // 最终网络/协议/取消/等待故障通知
    event.type = StreamEventType::Error;
    event.error = error;
    try {
        callback(event);
    } catch (...) { /* 原始故障优先，不覆盖二次错误通知异常。 */
    }
}

}
LLMClient::LLMClient(ProviderConfig provider,                    // 本 Client 的服务配置副本
                     std::unique_ptr<IProtocolAdapter> adapter,  // 接管协议实现
                     std::unique_ptr<ITransport> transport,      // 接管传输实现
                     RequestOptions defaults)                    // 保存默认请求配置，不进行隐式合并
    : m_provider(std::move(provider)), m_adapter(std::move(adapter)),
      m_transport(std::move(transport)), m_defaults(std::move(defaults))
{
}

bool LLMClient::chat(const ChatRequest& request,  // 当前请求
                     ChatResponse& response,      // 请求结果输出
                     SdkError& error) const       // 使用默认选项同步调用模型，并按配置重试
{
    return chat(request, response, error, m_defaults);
}

bool LLMClient::chat(const ChatRequest& request,           // 待编码的规范模型请求
                     ChatResponse& response,               // 本次结果，调用开始清除旧值
                     SdkError& error,                      // 故障输出，成功时为空
                     const RequestOptions& options) const  // 本次完整请求选项，覆盖默认值
{
    response = {};
    response.message.role = Role::Assistant;
    response.message.status = MessageStatus::Incomplete;
    error = {};
    if (!m_adapter || !m_transport) {
        error.category = ErrorCategory::Configuration;
        error.code = QStringLiteral("MissingDependency");
        error.message = QStringLiteral("Client requires an Adapter and a Transport");
        return false;
    }
    if ((options.timeoutSeconds != -1 && options.timeoutSeconds <= 0) ||
        options.retryPolicy.maxRetries < 0 || options.retryPolicy.retryIntervalMs < 0) {
        error.category = ErrorCategory::Configuration;
        error.code = QStringLiteral("InvalidRequestOptions");
        error.message = QStringLiteral("Invalid timeout or retry policy");
        return false;
    }
    if (checkRequestStop(options, error))
        return false;

    TransportRequest encoded;  // Adapter 生成的请求级 HTTP 数据
    if (!m_adapter->encodeChatRequest(m_provider, request, encoded, error))
        return false;
    if (checkRequestStop(options, error))
        return false;
    qint64 retries = 0;  // 已使用的重试次数，不包含首次尝试
    for (;;) {
        if (checkRequestStop(options, error)) {
            if (request.stream)
                reportFinalStreamError(options.streamCallback, error);
            return false;
        }
        response = {};
        response.message.role = Role::Assistant;
        response.message.status = MessageStatus::Incomplete;
        error = {};
        bool effectiveContent = false;  // 本次尝试是否已经输出不可重复的有效内容
        bool success = false;           // 当前单次模型尝试是否完整成功
        if (request.stream) {
            RequestOptions attempt = options;  // 请求级 Decoder/Session 每次重新创建，原配置不变
            attempt.streamCallback =
                [&](const StreamEvent& event) {  // 跟踪重试边界，同时转发标准化非错误事件
                    if (event.type == StreamEventType::Error)
                        return;
                    if (hasEffectiveContent(event))
                        effectiveContent = true;
                    if (options.streamCallback)
                        options.streamCallback(event);
                };
            success = chatStream(encoded, response, error, attempt);
        } else {
            TransportResponse received;  // 本次尝试独立的网络响应
            success = m_transport->send(encoded, options, received, error);
            if (!success && !error.httpStatus)
                error.httpStatus = received.statusCode;
            if (success && checkRequestStop(options, error))
                success = false;
            if (success)
                success = m_adapter->decodeChatResponse(received, response, error);
        }
        if (success && checkRequestStop(options, error)) {
            response.completionState = CompletionState::Incomplete;
            response.message.status = MessageStatus::Incomplete;
            success = false;
        }
        if (success)
            return true;
        if (error.category == ErrorCategory::None) {
            error.category = ErrorCategory::Internal;
            error.code = QStringLiteral("MissingChatError");
            error.message = QStringLiteral("Chat failed without an error");
        }
        if (retries >= options.retryPolicy.maxRetries || effectiveContent ||
            !canRetry(options.retryPolicy, error)) {
            if (request.stream)
                reportFinalStreamError(options.streamCallback, error);
            return false;
        }
        const qint64 delay =
            error.retryAfterMs && *error.retryAfterMs >= 0  // 优先使用有效的服务端建议
                ? *error.retryAfterMs
                : options.retryPolicy.retryIntervalMs;
        if (!waitForRetry(delay, options, error)) {
            if (request.stream)
                reportFinalStreamError(options.streamCallback, error);
            return false;
        }
        ++retries;
    }
}

bool LLMClient::chatStream(const TransportRequest& encoded,      // 已编码的 HTTP 请求
                           ChatResponse& response,               // 输出 Session 的最终值快照
                           SdkError& error,                      // 失败的具体流程原因
                           const RequestOptions& options) const  // 当前请求的超时、取消和实时通知
{
    auto decoder = m_adapter->createStreamDecoder(error);  // 每次调用独立的厂商协议解析器
    if (!decoder)
        return false;
    StreamSession session;                             // 当前请求唯一的内容聚合中心
    TransportResponse received;                        // HTTP 元数据及非成功状态的原始正文
    bool reportedError = false;                        // 是否已经通知流内错误，避免重复错误事件
    const auto notify = [&](const StreamEvent& event,  // 本次调用、上下文或业务结果参数
                            SdkError& failure) {       // 先聚合再通知，回调故障不能抹掉有效内容
        if (checkRequestStop(options, failure))
            return false;
        const bool applied =  // 聚合是否接受当前事件，Error 事件也要通知调用方
            session.apply(event, failure);
        if (!applied && event.type != StreamEventType::Error)
            return false;
        if (event.type == StreamEventType::Error)
            reportedError = true;
        if (!options.streamCallback)
            return applied;
        try {
            options.streamCallback(event);
        } catch (...) {
            failure = {};
            failure.category = ErrorCategory::Internal;
            failure.code = QStringLiteral("StreamCallbackFailed");
            failure.message = QStringLiteral("Stream callback threw an exception");
            return false;
        }
        return applied;
    };
    const auto consume = [&](const TransportResponse&, const QByteArray& bytes,  // 原始网络片段
                             SdkError& failure) {                                // 只将成功 HTTP 响应的原始数据交给 Decoder
        if (checkRequestStop(options, failure))
            return false;
        return decoder->feed(bytes, notify, failure);
    };
    bool success =
        m_transport->sendStream(encoded, options, received, consume,  // 同步增量传输，SDK 不创建线程
                                error);
    if (success && received.statusCode &&
        (*received.statusCode < 200 || *received.statusCode >= 300))
        success = m_adapter->decodeChatResponse(received, response, error);
    if (success && checkRequestStop(options, error))
        success = false;
    if (success)
        success = decoder->finish(notify, error);
    if (!success) {
        if (error.category == ErrorCategory::None) {
            error.category = ErrorCategory::Internal;
            error.code = QStringLiteral("MissingStreamError");
            error.message = QStringLiteral("Streaming failed without an error");
        }
        error.httpStatus = received.statusCode;
        session.fail(error);
        if (!reportedError && options.streamCallback) {
            StreamEvent event;  // 网络、取消、解析或回调故障的统一实时通知
            event.type = StreamEventType::Error;
            event.error = error;
            try {
                options.streamCallback(event);
            } catch (...) {
                // 原始故障优先，二次通知失败不覆盖已有诊断。
            }
        }
    }
    response = session.response();
    return success;
}

}  // AiLib 命名空间结束
