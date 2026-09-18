#include <AiLib/protocol/anthropic/AnthropicMessagesAdapter.h>
#include <AiLib/client/LLMClientFactory.h>
#include "../support/FakeTransport.h"
#include <QJsonDocument>
#include <QJsonArray>
#include <QtTest>

using namespace AiLib;
namespace {
ProviderConfig provider()  // 构造带网关前缀的 Messages 测试配置
{
    ProviderConfig config;  // 免真实密钥的离线配置
    config.protocol = ProtocolType::AnthropicMessages;
    config.baseUrl = QUrl(QStringLiteral("https://example.com/coding/"));
    config.apiKey = QStringLiteral("test-only");
    return config;
}
ChatRequest request()  // 构造带前置系统指令的最小请求
{
    ChatRequest chat;  // 单个用户输入及系统文本
    chat.model = QStringLiteral("test-model");
    chat.messages = {Message::system(QStringLiteral("帮助编程")), Message::user(QStringLiteral("你好"))};
    return chat;
}
QJsonObject root()  // 构造完整非流式助手响应
{
    return {{"type", "message"}, {"id", "msg_1"}, {"role", "assistant"}, {"model", "test-model"}, {"stop_reason", "end_turn"},
        {"content", QJsonArray{QJsonObject{{"type", "text"}, {"text", "你好"}}}}, {"usage", QJsonObject{{"input_tokens", 0}, {"output_tokens", 2}}}};
}
TransportResponse response(const QJsonObject& object)  // 将给定 JSON 封装为成功 HTTP 响应
{
    return {200, {}, QJsonDocument(object).toJson(QJsonDocument::Compact)};
}
}  // 测试辅助函数命名空间结束

// 验证 Messages 编解码、工具闭环及发送前校验，不访问外部网络。
class TestAnthropicMessages final : public QObject {
    Q_OBJECT
private slots:
    void ordinaryChat()  // 验证 Factory 创建及注入 Fake 的普通 Chat
    {
        SdkError error;                            // 流程故障输出
        std::unique_ptr<LLMClient> factoryClient;  // Factory 创建的真实网络 Client
        QVERIFY(LLMClientFactory::create(provider(), factoryClient, error));
        auto fake = std::make_unique<FakeTransport>();  // 离线 Transport 所有者
        auto* observer = fake.get();                    // Client 所有权转移后的测试观察指针
        fake->enqueue(response(root()));
        LLMClient client(provider(), std::make_unique<AnthropicMessagesAdapter>(), std::move(fake));  // 独占注入组件的 Client
        ChatResponse output;                                                                          // 解析后的助手结果
        QVERIFY(client.chat(request(), output, error));
        QCOMPARE(output.message.text(), QStringLiteral("你好"));
        QCOMPARE(output.completionState, CompletionState::Complete);
        QCOMPARE(*output.usage.inputTokens, qint64(0));
        QVERIFY(!output.usage.totalTokens);
        const auto sent = observer->requests().first();                 // 实际发送的规范化 HTTP 请求
        const auto body = QJsonDocument::fromJson(sent.body).object();  // 用于断言的协议 JSON
        QCOMPARE(sent.url.path(), QStringLiteral("/coding/v1/messages"));
        QCOMPARE(body.value("max_tokens").toInt(), 1024);
        QCOMPARE(body.value("system").toArray().size(), 1);
        QCOMPARE(body.value("messages").toArray().first().toObject().value("role").toString(), QStringLiteral("user"));
        QVERIFY(sent.headers.contains(qMakePair(QByteArray("anthropic-version"), QByteArray("2023-06-01"))));
    }
    void toolsAndOrder()  // 验证原生工具调用及相邻工具结果合并与失败语义
    {
        AnthropicMessagesAdapter adapter;  // 无请求级共享状态的协议组件
        SdkError error;                    // 流程故障输出
        ChatResponse output;               // 解码结果
        QJsonObject raw = root();          // 模拟含推理、调用和文本的有序响应
        raw.insert("stop_reason", "tool_use");
        raw.insert("content", QJsonArray{QJsonObject{{"type", "thinking"}, {"thinking", "分析"}},
            QJsonObject{{"type", "tool_use"}, {"id", "a"}, {"name", "lookup"}, {"input", QJsonObject{{"id", 1}}}},
            QJsonObject{{"type", "text"}, {"text", "查询中"}},
            QJsonObject{{"type", "tool_use"}, {"id", "b"}, {"name", "lookup"}, {"input", QJsonObject{}}}});
        QVERIFY(adapter.decodeChatResponse(response(raw), output, error));
        QCOMPARE(output.message.contents.size(), 4);
        QVERIFY(std::holds_alternative<ReasoningContent>(output.message.contents[0]));
        QVERIFY(std::holds_alternative<ToolCallContent>(output.message.contents[1]));
        QCOMPARE(output.message.toolCalls().size(), 2);
        ChatRequest chat = request();  // 带工具及完整助手调用的后续请求
        chat.tools.append(FunctionToolDefinition{QStringLiteral("lookup"), QStringLiteral("查询"), QJsonObject{{"type", "object"}}});
        chat.toolChoice.mode = ToolChoiceMode::Required;
        output.message.contents.removeFirst();
        chat.messages.append(output.message);
        Message first;  // 第一条成功工具结果消息
        first.role = Role::Tool;
        first.contents.append(ToolResultContent{ToolResult{"a", "lookup", true, QJsonObject{{"online", true}}, {}, {}}});
        Message second;  // 第二条被拒绝的工具结果消息
        second.role = Role::Tool;
        second.contents.append(ToolResultContent{ToolResult{"b", "lookup", false, {}, "ApprovalDenied", "用户拒绝"}});
        chat.messages.append(first);
        chat.messages.append(second);
        TransportRequest encoded;  // 工具结果请求的编码输出
        QVERIFY(adapter.encodeChatRequest(provider(), chat, encoded, error));
        const auto body = QJsonDocument::fromJson(encoded.body).object();  // Messages 请求体
        const auto results = body.value("messages").toArray().last().toObject().value("content").toArray();  // 合并的同批工具结果
        QCOMPARE(results.size(), 2);
        QVERIFY(!results[0].toObject().value("is_error").toBool());
        QVERIFY(results[1].toObject().value("is_error").toBool());
        QCOMPARE(body.value("tool_choice").toObject().value("type").toString(), QStringLiteral("any"));
        const auto failure = QJsonDocument::fromJson(results[1].toObject().value("content").toString().toUtf8()).object();  // 给模型的失败核心语义
        QCOMPARE(failure.value("errorCode").toString(), QStringLiteral("ApprovalDenied"));
        QVERIFY(!failure.value("success").toBool());
    }
    void configurationChecks()  // 验证保留字段、Header 和无法编码的历史在发送前失败
    {
        AnthropicMessagesAdapter adapter;  // 待测试协议组件
        SdkError error;                    // 编码故障输出
        TransportRequest output;           // 成功后的请求输出
        auto config = provider();          // 可修改的服务配置
        auto chat = request();             // 可修改的模型请求
        for (const QString& field : QStringList{"model", "messages", "system", "max_tokens", "stream", "tools", "tool_choice", "n"}) {  // 当前保留字段
            chat.extraParameters = {{field, 1}};
            QVERIFY(!adapter.encodeChatRequest(config, chat, output, error));
            QCOMPARE(error.code, QStringLiteral("ReservedParameter"));
        }
        chat.extraParameters = {};
        config.customHeaders = {{"X-Api-Key", "custom"}};
        QVERIFY(adapter.encodeChatRequest(config, chat, output, error));
        QVERIFY(output.headers.contains(qMakePair(QByteArray("x-api-key"), QByteArray("custom"))));
        for (const QString& header : QStringList{"Content-Type", "ACCEPT", "Anthropic-Version", "Host"}) {  // 当前受保护 Header
            config.customHeaders = {{header, "custom"}};
            QVERIFY(!adapter.encodeChatRequest(config, chat, output, error));
            QCOMPARE(error.code, QStringLiteral("ProtectedHeader"));
        }
        config.customHeaders = {{"X-Api-Key", "one"}, {"x-api-key", "two"}};
        QVERIFY(!adapter.encodeChatRequest(config, chat, output, error));
        config.customHeaders = {{"x-custom", "bad\r\nvalue"}};
        QVERIFY(!adapter.encodeChatRequest(config, chat, output, error));
        config.customHeaders = {};
        chat.messages.append(Message::system(QStringLiteral("中途系统指令")));
        QVERIFY(!adapter.encodeChatRequest(config, chat, output, error));
        chat = request();
        chat.stream = true;
        QVERIFY(adapter.encodeChatRequest(config, chat, output, error));
        chat = request();
        chat.temperature = 1.5;
        QVERIFY(!adapter.encodeChatRequest(config, chat, output, error));
        chat = request();
        chat.maxOutputTokens = 0;
        QVERIFY(!adapter.encodeChatRequest(config, chat, output, error));
    }
    void imagesAndUnsupportedContent()  // 验证图片顺序、二进制图片和无法编码的内容
    {
        AnthropicMessagesAdapter adapter;  // 待测试协议组件
        auto chat = request();             // 追加图片内容的 canonical 请求
        SdkError error;                    // 编码故障输出
        TransportRequest output;           // 编码后的协议请求
        chat.messages.last().contents.append(ImageContent{MediaResource::fromUrl(QStringLiteral("https://example.com/a.png"))});
        chat.messages.last().contents.append(TextContent{QStringLiteral("图片之后")});
        QVERIFY(adapter.encodeChatRequest(provider(), chat, output, error));
        auto content = QJsonDocument::fromJson(output.body).object().value("messages").toArray().first().toObject().value("content").toArray();  // 图片与文本的协议顺序
        QCOMPARE(content.size(), 3);
        QCOMPARE(content[0].toObject().value("type").toString(), QStringLiteral("text"));
        QCOMPARE(content[1].toObject().value("type").toString(), QStringLiteral("image"));
        QCOMPARE(content[2].toObject().value("text").toString(), QStringLiteral("图片之后"));
        const QByteArray png = QByteArray::fromBase64("iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAusB9Wl2nS8AAAAASUVORK5CYII=");  // 离线一像素 PNG 数据
        chat.messages.last().contents[1] = ImageContent{MediaResource::fromBytes(png, QStringLiteral("image/png"))};
        QVERIFY(adapter.encodeChatRequest(provider(), chat, output, error));
        content = QJsonDocument::fromJson(output.body).object().value("messages").toArray().first().toObject().value("content").toArray();
        QCOMPARE(content[1].toObject().value("source").toObject().value("type").toString(), QStringLiteral("base64"));
        chat.messages.last().contents[1] = ImageContent{MediaResource::fromBytes(png, QStringLiteral("image/jpeg"))};
        QVERIFY(!adapter.encodeChatRequest(provider(), chat, output, error));
        chat.messages.last().contents[1] = ReasoningContent{QStringLiteral("没有原生签名的历史推理")};
        QVERIFY(!adapter.encodeChatRequest(provider(), chat, output, error));
        QCOMPARE(error.code, QStringLiteral("UnsupportedFeature"));
    }
    void invalidResponses_data()  // 为严格响应校验提供边界样本
    {
        QTest::addColumn<QJsonObject>("raw");
        QJsonObject raw = root();  // 每种错误的可修改响应样本
        raw.insert("content", "bad"); QTest::newRow("content-type") << raw;
        raw = root(); raw.insert("role", "user"); QTest::newRow("role") << raw;
        raw = root(); raw.insert("stop_reason", QJsonValue()); QTest::newRow("missing-stop") << raw;
        raw = root(); raw.insert("usage", QJsonObject{{"input_tokens", -1}}); QTest::newRow("negative-usage") << raw;
        raw = root(); raw.insert("content", QJsonArray{QJsonObject{{"type", "tool_use"}, {"id", ""}, {"name", "lookup"}, {"input", QJsonObject{}}}}); QTest::newRow("empty-id") << raw;
        raw = root(); raw.insert("content", QJsonArray{QJsonObject{{"type", "tool_use"}, {"id", "a"}, {"name", "lookup"}, {"input", "bad"}}}); QTest::newRow("bad-input") << raw;
        raw = root(); raw.insert("stop_reason", "tool_use"); QTest::newRow("no-calls") << raw;
        raw = root(); raw.insert("content", QJsonArray{QJsonObject{{"type", "server_tool_use"}}}); QTest::newRow("builtin") << raw;
    }
    void invalidResponses()  // 验证错误响应不能被标记完整成功
    {
        QFETCH(QJsonObject, raw);          // 当前参数化响应样本
        AnthropicMessagesAdapter adapter;  // 待测试协议组件
        ChatResponse output;               // 保留部分数据的解析输出
        SdkError error;                    // 具体响应故障
        QVERIFY(!adapter.decodeChatResponse(response(raw), output, error));
        QCOMPARE(output.completionState, CompletionState::Incomplete);
        QVERIFY(error.category != ErrorCategory::None);
    }
    void partialAndLength()  // 验证重复 ID 保留之前有效内容及 Length 的独立语义
    {
        AnthropicMessagesAdapter adapter;  // 待测试协议组件
        SdkError error;                    // 故障输出
        ChatResponse output;               // 部分或完整响应
        auto raw = root();                 // 重复调用 ID 的响应
        const QJsonObject call{{"type", "tool_use"}, {"id", "a"}, {"name", "lookup"}, {"input", QJsonObject{}}};  // 完整原生调用块
        raw.insert("content", QJsonArray{QJsonObject{{"type", "text"}, {"text", "保留"}}, call, call});
        QVERIFY(!adapter.decodeChatResponse(response(raw), output, error));
        QCOMPARE(output.message.text(), QStringLiteral("保留"));
        QCOMPARE(output.message.toolCalls().size(), 1);
        QCOMPARE(*output.usage.inputTokens, qint64(0));
        raw = root(); raw.insert("stop_reason", "max_tokens");
        QVERIFY(adapter.decodeChatResponse(response(raw), output, error));
        QCOMPARE(output.finishReason, FinishReason::Length);
        QCOMPARE(output.completionState, CompletionState::Complete);
        QCOMPARE(output.message.status, MessageStatus::Incomplete);
    }
    void providerErrors()  // 验证 HTTP 错误和 Retry-After 保留
    {
        AnthropicMessagesAdapter adapter;                // 待测试协议组件
        SdkError error;                                  // Provider 故障输出
        ChatResponse output;                             // 失败时的不完整响应
        for (const int status : {401, 403, 429, 500}) {  // 当前模拟 HTTP 故障状态
            auto input = response(QJsonObject{{"type", "error"}, {"error", QJsonObject{{"type", "rate_limit_error"}, {"message", "test error"}}}});  // 原生故障响应
            input.statusCode = status;
            input.headers = {{"Retry-After", "3"}};
            QVERIFY(!adapter.decodeChatResponse(input, output, error));
            QCOMPARE(*error.httpStatus, status);
            QCOMPARE(*error.retryAfterMs, qint64(3000));
            QCOMPARE(error.category, status == 401 || status == 403 ? ErrorCategory::Authentication : status == 429 ? ErrorCategory::RateLimited : ErrorCategory::Provider);
        }
    }
};
QTEST_GUILESS_MAIN(TestAnthropicMessages)
#include "tst_AnthropicMessages.moc"
