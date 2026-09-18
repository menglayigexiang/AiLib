#pragma once
#include <AiLib/agent/Agent.h>
#include <AiLib/client/LLMClientFactory.h>
#include <AiLib/protocol/openai/OpenAIChatCompatibleAdapter.h>
#include <QJsonArray>
#include <QJsonDocument>
#include <QThread>

namespace Demo {
// 示例专用离线服务，走真实 Adapter；不属于 SDK 运行时。
class OfflineTransport final : public AiLib::ITransport {
public:
    bool send(const AiLib::TransportRequest& request,  // Adapter 编码的请求
              const AiLib::RequestOptions& options,    // 共享取消和截止时间
              AiLib::TransportResponse& response,      // 输出模拟 HTTP 响应
              AiLib::SdkError& error) override         // 同步返回完整单候选响应
    {
        if (!pause(options, error))
            return false;
        response.statusCode = 200;
        response.body = QJsonDocument(reply(request)).toJson(QJsonDocument::Compact);
        return true;
    }
    bool sendStream(const AiLib::TransportRequest& request,    // 已编码的流式请求
                    const AiLib::RequestOptions& options,      // 共享停止条件
                    AiLib::TransportResponse& response,        // 模拟 HTTP 元数据
                    const AiLib::TransportDataCallback& sink,  // 接收原始 SSE 字节
                    AiLib::SdkError& error) override           // 延迟交付分片，便于观察 Stop
    {
        response.statusCode = 200;
        const auto full = reply(request);                                        // 与普通响应一致的离线结果
        const auto choice = full.value("choices").toArray().first().toObject();  // 单候选数据
        const auto message = choice.value("message").toObject();                 // 模型生成内容
        QList<QJsonObject> deltas;                                               // 按顺序发送的原始协议片段
        if (message.contains("tool_calls")) {
            auto call = message.value("tool_calls").toArray().first().toObject();  // 离线调用
            call.insert("index", 0);
            deltas.append(QJsonObject{{"tool_calls", QJsonArray{call}}});
        } else {
            const QString text = message.value("content").toString();  // 待拆分文字
            for (int offset = 0; offset < text.size(); offset += 3) {  // 当前片段起点
                deltas.append(QJsonObject{{"content", text.mid(offset, 3)}});
            }
        }
        for (const auto& delta : deltas) {  // 当前交付的模型增量
            if (!pause(options, error))
                return false;
            const QJsonObject event{  // 协议增量事件
                {"id", "offline"},
                {"model", "demo"},
                {"choices",
                 QJsonArray{QJsonObject{{"index", 0},
                                        {"delta", delta},
                                        {"finish_reason", QJsonValue::Null}}}}};
            if (!sink(response,
                      "data: " + QJsonDocument(event).toJson(QJsonDocument::Compact) + "\n\n",
                      error))
                return false;
        }
        const QJsonObject end{  // 模型结束事件
            {"choices", QJsonArray{QJsonObject{
                            {"index", 0},
                            {"delta", QJsonObject{}},
                            {"finish_reason", choice.value("finish_reason")}}}}};
        return sink(response,
                    "data: " + QJsonDocument(end).toJson(QJsonDocument::Compact) +
                        "\n\ndata: [DONE]\n\n",
                    error);
    }

private:
    static bool pause(const AiLib::RequestOptions& options,  // 本次执行配置
                      AiLib::SdkError& error)                // 模拟延迟并协作响应停止
    {
        for (int step = 0; step < 5; ++step) {  // 每二十毫秒检查一次停止条件
            if (options.cancellation.isCancellationRequested() || options.isDeadlineExpired()) {
                error.category = options.cancellation.isCancellationRequested()
                                     ? AiLib::ErrorCategory::Cancelled
                                     : AiLib::ErrorCategory::Timeout;
                error.code = QStringLiteral("OfflineStopped");
                return false;
            }
            QThread::msleep(20);
        }
        return true;
    }
    static QJsonObject reply(const AiLib::TransportRequest& request)  // 根据实际工具回传生成后续回答
    {
        const auto input = QJsonDocument::fromJson(request.body).object();  // Adapter 的实际请求
        const auto messages = input.value("messages").toArray();            // 已发送的外部历史和本轮增量
        QJsonObject message{{"role", "assistant"}};                         // 模拟单个 Assistant 响应
        QString finish = QStringLiteral("stop");                            // 本轮模型结束原因
        if (!messages.isEmpty() && messages.last().toObject().value("role") == "tool") {
            const auto data = QJsonDocument::fromJson(  // 工具真实回传
                                  messages.last().toObject().value("content").toString().toUtf8())
                                  .object();
            message.insert(
                "content",
                data.contains("success") && !data.value("success").toBool()
                    ? QStringLiteral("工具未执行：") + data.value("errorCode").toString()
                    : QStringLiteral("工具计算结果：%1").arg(data.value("sum").toDouble()));
        } else if (!input.value("tools").toArray().isEmpty()) {
            message.insert("tool_calls",
                           QJsonArray{QJsonObject{
                               {"id", "call_demo"},
                               {"type", "function"},
                               {"function", QJsonObject{{"name", "add"},
                                                        {"arguments", "{\"a\":19,\"b\":23}"}}}}});
            finish = QStringLiteral("tool_calls");
        } else {
            message.insert("content", QStringLiteral("你好，这是 AiLib 的离线回答。"));
        }
        return QJsonObject{
            {"id", "offline"},
            {"model", "demo"},
            {"choices", QJsonArray{QJsonObject{
                            {"index", 0}, {"message", message}, {"finish_reason", finish}}}},
            {"usage",
             QJsonObject{{"prompt_tokens", 10}, {"completion_tokens", 8}, {"total_tokens", 18}}}};
    }
};

inline bool createClient(const QString& provider,                    // 离线或真实服务名称
                         std::unique_ptr<AiLib::LLMClient>& client,  // 输出独占 Client
                         QString& model,                             // 输出选定模型名称
                         AiLib::SdkError& error)                     // 组装离线或环境变量配置的真实 Client
{
    AiLib::ProviderConfig config;  // 示例应用拥有的服务配置
    config.id = provider;
    if (provider == "offline") {
        config.baseUrl = QUrl("https://offline.invalid/v1");
        model = QStringLiteral("demo");
        client = std::make_unique<AiLib::LLMClient>(
            config, std::make_unique<AiLib::OpenAIChatCompatibleAdapter>(),
            std::make_unique<OfflineTransport>());
        return true;
    }
    if (provider == "deepseek") {
        config.baseUrl = QUrl("https://api.deepseek.com");
        config.apiKey = QString::fromUtf8(qgetenv("DEEPSEEK_API_KEY"));
        model = QStringLiteral("deepseek-flash");
    } else if (provider == "kimi") {
        config.protocol = AiLib::ProtocolType::AnthropicMessages;
        config.baseUrl = QUrl("https://api.kimi.com/coding/");
        config.apiKey = QString::fromUtf8(qgetenv("KIMI_CODE_API_KEY"));
        config.customHeaders.insert("User-Agent", "AiLib/0.1.0 (SDK example)");
        model = QStringLiteral("kimi-for-coding");
    } else {
        error.category = AiLib::ErrorCategory::InvalidArgument;
        error.code = QStringLiteral("UnknownDemoProvider");
        return false;
    }
    if (config.apiKey.isEmpty()) {
        error.category = AiLib::ErrorCategory::InvalidArgument;
        error.code = QStringLiteral("MissingEnvironmentKey");
        return false;
    }
    return AiLib::LLMClientFactory::create(config, client, error);
}

inline bool registerTools(AiLib::ToolRegistry& registry,  // 非拥有的应用注册表
                          AiLib::SdkError& error)         // 注册需要确认的无副作用加法工具
{
    AiLib::FunctionToolDefinition definition;  // 纯工具描述
    definition.name = QStringLiteral("add");
    definition.description =
        QStringLiteral("Add two integers. Use a=19 and b=23 to verify the SDK example.");
    definition.approvalPolicy = AiLib::ToolApprovalPolicy::Always;
    definition.inputSchema = {{"type", "object"},
                              {"properties", QJsonObject{{"a", QJsonObject{{"type", "integer"}}},
                                                         {"b", QJsonObject{{"type", "integer"}}}}},
                              {"required", QJsonArray{"a", "b"}}};
    return registry.registerTool(
        definition,
        [](const AiLib::ToolCall& call,                 // 完整的业务参数
           const AiLib::ToolExecutionContext& context,  // 执行来源和停止上下文
           AiLib::ToolResult& result,                   // 输出业务计算结果
           AiLib::SdkError& failure) {                  // 执行业务计算，关联字段由 Executor 填写
            Q_UNUSED(context)
            Q_UNUSED(failure)
            result.data = QJsonObject{{"sum", call.arguments.value("a").toDouble() +
                                                  call.arguments.value("b").toDouble()}};
            return true;
        },
        error);
}
inline QString finishName(AiLib::AgentFinishReason reason)  // 将正常停止原因转换为示例展示名称
{
    switch (reason) {
    case AiLib::AgentFinishReason::Completed:
        return QStringLiteral("Completed");
    case AiLib::AgentFinishReason::Length:
        return QStringLiteral("Length");
    case AiLib::AgentFinishReason::Cancelled:
        return QStringLiteral("Cancelled");
    case AiLib::AgentFinishReason::MaxTurns:
        return QStringLiteral("MaxTurns");
    case AiLib::AgentFinishReason::MaxToolCalls:
        return QStringLiteral("MaxToolCalls");
    case AiLib::AgentFinishReason::Timeout:
        return QStringLiteral("Timeout");
    case AiLib::AgentFinishReason::Failed:
        return QStringLiteral("Failed");
    }
    return QStringLiteral("Failed");
}
inline AiLib::ChatRequest chatRequest(const QString& model,                  // 当前选定模型
                                      const QList<AiLib::Message>& history,  // 应用历史值对象
                                      bool stream)                           // 生成两种协议均可编码的示例请求
{
    AiLib::ChatRequest request;  // 本次请求的值对象
    request.model = model;
    request.messages = history;
    request.stream = stream;
    request.maxOutputTokens = 1024;
    if (model == "kimi-for-coding" || model == "deepseek-flash")
        request.extraParameters = {{"thinking", QJsonObject{{"type", "disabled"}}}};
    return request;
}
}  // 示例辅助命名空间结束
