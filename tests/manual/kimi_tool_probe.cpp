#include <AiLib/client/LLMClient.h>
#include <AiLib/protocol/anthropic/AnthropicMessagesAdapter.h>
#include <AiLib/network/QtHttpTransport.h>
#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTextStream>

// 手动诊断专用传输包装，每次请求新建，只在内存捕获协议正文，不保存认证 Header。
class ProbeTransport final : public AiLib::ITransport {
public:
    bool send(const AiLib::TransportRequest& request,  // 已编码请求
              const AiLib::RequestOptions& options,    // 单次停止配置
              AiLib::TransportResponse& response,      // 原始响应
              AiLib::SdkError& error) override         // 普通响应与 SDK 解码作对照
    {
        sent = QJsonDocument::fromJson(request.body).object();
        const bool ok = m_transport.send(request, options, response, error);  // 单次传输结果
        received = response.body;
        return ok;
    }
    bool sendStream(const AiLib::TransportRequest& request,    // 已编码流式请求
                    const AiLib::RequestOptions& options,      // 停止配置
                    AiLib::TransportResponse& response,        // HTTP 元数据
                    const AiLib::TransportDataCallback& sink,  // 原始字节接收器
                    AiLib::SdkError& error) override           // 捕获原始 SSE，同时继续交付真实 Decoder
    {
        sent = QJsonDocument::fromJson(request.body).object();
        return m_transport.sendStream(
            request, options, response,
            [&](const AiLib::TransportResponse& metadata,  // 当前 HTTP 元数据
                const QByteArray& bytes,                   // 当前原始响应字节
                AiLib::SdkError& failure) {                // 捕获原始正文，保持接收器返回语义
                received.append(bytes);
                return sink(metadata, bytes, failure);
            },
            error);
    }
    QJsonObject sent;     // 不含认证 Header 的请求正文，仅用于字段摘要
    QByteArray received;  // 原始响应正文，仅存内存，不打印完整内容
private:
    AiLib::QtHttpTransport m_transport;  // 真正执行网络调用的同步 Transport
};

static QJsonObject rawSummary(const QByteArray& bytes,  // 当前完整或部分原始正文
                              bool stream)              // 按普通或流式响应汇总原始内容块与结束原因，不记录正文
{
    int toolCalls = 0;   // 原生 tool_use 块数量
    int textParts = 0;   // 原生 text 块数量
    QString stopReason;  // 服务端协议结束原因
    if (!stream) {
        const auto root = QJsonDocument::fromJson(bytes).object();  // 原始普通响应
        stopReason = root.value("stop_reason").toString();
        for (const auto& item : root.value("content").toArray()) {          // 原始内容块
            const QString type = item.toObject().value("type").toString();  // 原生块类别
            toolCalls += type == "tool_use";
            textParts += type == "text";
        }
    } else {
        for (const auto& line : bytes.split('\n')) {  // 原始 SSE 行，Kimi 本次事件数据为单行 JSON
            if (!line.startsWith("data:"))
                continue;
            const auto event = QJsonDocument::fromJson(line.mid(5).trimmed()).object();  // 原生事件
            if (event.value("type") == "content_block_start") {
                const QString type = event
                                         .value("content_block")  // 开始内容块的类型
                                         .toObject()
                                         .value("type")
                                         .toString();
                toolCalls += type == "tool_use";
                textParts += type == "text";
            }
            if (event.value("type") == "message_delta")
                stopReason = event.value("delta").toObject().value("stop_reason").toString();
        }
    }
    return {
        {"rawToolCalls", toolCalls}, {"rawTextParts", textParts}, {"rawStopReason", stopReason}};
}
int main(int argc, char* argv[])  // 对照原始协议与 SDK，默认八项，--demo-variants 只测原 Demo 四项
{
    QCoreApplication application(argc, argv);  // 调用方的 Qt 网络环境
    QTextStream log(stdout);                   // 只输出不含凭据或正文的 JSON 摘要
    AiLib::ProviderConfig config;              // 真实 Kimi 服务配置
    config.id = "kimi-probe";
    config.baseUrl = QUrl("https://api.kimi.com/coding/");
    config.protocol = AiLib::ProtocolType::AnthropicMessages;
    config.apiKey = QString::fromUtf8(qgetenv("KIMI_CODE_API_KEY"));
    config.customHeaders.insert("User-Agent", "AiLib/0.1.0 (SDK tool diagnostic)");
    if (config.apiKey.isEmpty())
        return 2;
    AiLib::FunctionToolDefinition tool;  // 与现有手动协议测试相同的工具定义
    tool.name = "add";
    tool.description = "Add two integers for a C++ unit test.";
    tool.inputSchema = {{"type", "object"},
                        {"properties", QJsonObject{{"a", QJsonObject{{"type", "integer"}}},
                                                   {"b", QJsonObject{{"type", "integer"}}}}},
                        {"required", QJsonArray{"a", "b"}}};
    const bool demoAuto =  // 只测原 Demo 定义的完整提示 Auto 选择
        application.arguments().contains("--demo-auto");
    const bool demoVariants =  // 使用原 Demo 说明
        demoAuto || application.arguments().contains("--demo-variants");
    const int scenarioBegin = demoAuto ? 3 : 0;                 // 首个需要诊断的场景
    const int scenarioEnd = demoVariants && !demoAuto ? 2 : 4;  // 不包含的场景上界
    if (demoVariants)
        tool.description = "Add two integers. Use a=19 and b=23 to verify the SDK example.";
    bool mismatch = false;               // 是否发现原生工具块与完整 SDK 响应数量不一致
    for (bool stream : {false, true}) {  // 普通与流式编码
        for (int scenario = scenarioBegin; scenario < scenarioEnd;  // 当前诊断的工具选择和提示场景
             ++scenario) {
            auto transport = std::make_unique<ProbeTransport>();  // 当前请求独立的捕获器
            auto* probe = transport.get();                        // Client 存活期间有效的非拥有观察指针
            AiLib::LLMClient client(  // 独占组件，走真实 SDK
                config, std::make_unique<AiLib::AnthropicMessagesAdapter>(), std::move(transport));
            AiLib::ChatRequest request;                     // 当前单候选诊断请求
            request.model = "kimi-for-coding";
            request.maxOutputTokens = 1024;
            request.stream = stream;
            request.extraParameters = {{"thinking", QJsonObject{{"type", "disabled"}}}};
            request.tools = {tool};
            request.messages = {AiLib::Message::user(
                scenario == 1
                    ? QStringLiteral("请使用 add 工具计算 19+23。")
                    : QStringLiteral(
                          "请调用 add 工具，参数 a=19、b=23，检查一个 C++ 加法函数测试，然后报告结果。"))};
            request.toolChoice.mode = scenario < 2    ? AiLib::ToolChoiceMode::Specific
                                      : scenario == 2 ? AiLib::ToolChoiceMode::Required
                                                      : AiLib::ToolChoiceMode::Auto;
            request.toolChoice.toolName = "add";
            AiLib::RequestOptions options;  // 诊断不自动重试，避免改变尝试次数
            options.timeoutSeconds = 45;
            options.retryPolicy.maxRetries = 0;
            AiLib::ChatResponse response;                                    // SDK 完整或部分响应
            AiLib::SdkError error;                                           // SDK 流程错误
            const bool ok = client.chat(request, response, error, options);  // 同步执行一次真实请求
            auto summary = rawSummary(probe->received, stream);              // 原生协议摘要
            summary.insert("scenario", scenario);
            summary.insert("demoDefinition", demoVariants);
            summary.insert("stream", stream);
            summary.insert("encodedTools", probe->sent.value("tools").toArray().size());
            summary.insert("encodedChoice", probe->sent.value("tool_choice"));
            summary.insert("ok", ok);
            summary.insert("sdkToolCalls", response.message.toolCalls().size());
            summary.insert("sdkTextChars", response.message.text().size());
            summary.insert("errorCode", error.code);
            summary.insert("httpError", error.httpStatus.value_or(0));
            log << QJsonDocument(summary).toJson(QJsonDocument::Compact) << '\n' << Qt::flush;
            mismatch = mismatch || (ok && summary.value("rawToolCalls").toInt() !=
                                              response.message.toolCalls().size());
        }
    }
    return mismatch ? 5 : 0;
}
