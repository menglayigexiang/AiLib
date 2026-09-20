#include <AiLib/protocol/openai/OpenAIResponsesAdapter.h>
#include <AiLib/stream/StreamSession.h>
#include <QJsonDocument>
#include <QtTest>

using namespace AiLib;
namespace {
ProviderConfig provider()  // 构造 Responses 测试服务配置
{
    ProviderConfig value;  // 待返回的连接配置
    value.baseUrl = QUrl(QStringLiteral("https://api.openai.com/v1/"));
    value.protocol = ProtocolType::OpenAIResponses;
    value.apiKey = QStringLiteral("test-key");
    return value;
}

ChatRequest request()  // 构造包含 Function Tool 的最小请求
{
    ChatRequest value;  // 待编码的 canonical 请求
    value.model = QStringLiteral("gpt-5.6-sol");
    value.messages = {Message::system(QStringLiteral("Be concise")), Message::user(QStringLiteral("add"))};
    FunctionToolDefinition tool;  // 加法工具定义
    tool.name = QStringLiteral("add");
    tool.description = QStringLiteral("Add numbers");
    tool.inputSchema = QJsonObject{{QStringLiteral("type"), QStringLiteral("object")}};
    value.tools.append(tool);
    value.stream = true;
    return value;
}
}  // 测试数据辅助函数命名空间结束

// 验证 OpenAI Responses 普通、工具及流式 canonical 映射。
class OpenAIResponsesTest final : public QObject {
    Q_OBJECT
private slots:
    void encodeRequest()  // 编码 Responses 路径、输入与工具定义
    {
        OpenAIResponsesAdapter adapter;  // 被测无状态 Adapter
        TransportRequest output;         // 编码后的 HTTP 请求
        SdkError error;                  // 编码错误
        QVERIFY(adapter.encodeChatRequest(provider(), request(), output, error));
        QCOMPARE(output.url.path(), QStringLiteral("/v1/responses"));
        const QJsonObject body = QJsonDocument::fromJson(output.body).object();  // 实际请求体
        QCOMPARE(body.value(QStringLiteral("model")).toString(), QStringLiteral("gpt-5.6-sol"));
        QCOMPARE(body.value(QStringLiteral("input")).toArray().size(), 2);
        QCOMPARE(body.value(QStringLiteral("tools")).toArray().first().toObject().value(QStringLiteral("name")).toString(), QStringLiteral("add"));
        QVERIFY(body.value(QStringLiteral("stream")).toBool());
    }

    void decodeToolResponse()  // 解码完整文本和 Function Tool Call
    {
        const QJsonObject root{  // OpenAI Responses 完整响应样本
            {QStringLiteral("id"), QStringLiteral("resp_1")},
            {QStringLiteral("model"), QStringLiteral("gpt-5.6-sol")},
            {QStringLiteral("status"), QStringLiteral("completed")},
            {QStringLiteral("output"), QJsonArray{
                QJsonObject{{QStringLiteral("type"), QStringLiteral("message")},
                            {QStringLiteral("content"), QJsonArray{QJsonObject{{QStringLiteral("type"), QStringLiteral("output_text")}, {QStringLiteral("text"), QStringLiteral("calling")}}}}},
                QJsonObject{{QStringLiteral("type"), QStringLiteral("function_call")},
                            {QStringLiteral("call_id"), QStringLiteral("call_1")},
                            {QStringLiteral("name"), QStringLiteral("add")},
                            {QStringLiteral("arguments"), QStringLiteral("{\"a\":1,\"b\":2}")}}}},
            {QStringLiteral("usage"), QJsonObject{{QStringLiteral("input_tokens"), 3}, {QStringLiteral("output_tokens"), 4}, {QStringLiteral("total_tokens"), 7}}}};
        OpenAIResponsesAdapter adapter;  // 被测无状态 Adapter
        TransportResponse input{200, {}, QJsonDocument(root).toJson(QJsonDocument::Compact)};  // HTTP 响应样本
        ChatResponse output;  // canonical 响应输出
        SdkError error;       // 解码错误
        QVERIFY(adapter.decodeChatResponse(input, output, error));
        QCOMPARE(output.message.text(), QStringLiteral("calling"));
        QCOMPARE(output.message.toolCalls().size(), 1);
        QCOMPARE(output.finishReason, FinishReason::ToolCalls);
        QCOMPARE(*output.usage.totalTokens, qint64(7));
    }

    void streamText()  // 聚合任意分片的 Responses 文本流
    {
        const QByteArray stream =  // Responses SSE 协议样本
            "event: response.created\ndata: {\"type\":\"response.created\",\"response\":{\"id\":\"resp_1\",\"model\":\"gpt-5.6-sol\"}}\n\n"
            "event: response.output_text.delta\ndata: {\"type\":\"response.output_text.delta\",\"output_index\":0,\"delta\":\"hello\"}\n\n"
            "event: response.output_item.done\ndata: {\"type\":\"response.output_item.done\",\"output_index\":0}\n\n"
            "event: response.completed\ndata: {\"type\":\"response.completed\",\"response\":{\"usage\":{\"input_tokens\":1,\"output_tokens\":1,\"total_tokens\":2}}}\n\n";
        OpenAIResponsesAdapter adapter;  // 创建流 Decoder 的 Adapter
        SdkError error;                  // 流解析或聚合错误
        auto decoder = adapter.createStreamDecoder(error);  // 请求级 Decoder
        StreamSession session;  // canonical 流聚合器
        const StreamEventSink sink = [&](const StreamEvent& event, SdkError& failure) {  // 将事件交给 Session
            return session.apply(event, failure);
        };
        for (int offset = 0; offset < stream.size(); offset += 7)  // 使用不规则网络分片输入
            QVERIFY(decoder->feed(stream.mid(offset, 7), sink, error));
        QVERIFY(decoder->finish(sink, error));
        QCOMPARE(session.response().message.text(), QStringLiteral("hello"));
        QCOMPARE(session.response().completionState, CompletionState::Complete);
    }

    void streamToolCall()  // 聚合 Responses 工具身份和参数增量
    {
        const QByteArray stream =  // Function Tool SSE 样本
            "event: response.created\ndata: {\"type\":\"response.created\",\"response\":{\"id\":\"resp_2\",\"model\":\"gpt-5.6-sol\"}}\n\n"
            "event: response.output_item.added\ndata: {\"type\":\"response.output_item.added\",\"output_index\":0,\"item\":{\"type\":\"function_call\",\"call_id\":\"call_1\",\"name\":\"add\"}}\n\n"
            "event: response.function_call_arguments.delta\ndata: {\"type\":\"response.function_call_arguments.delta\",\"output_index\":0,\"delta\":\"{\\\"a\\\":1,\\\"b\\\":2}\"}\n\n"
            "event: response.output_item.done\ndata: {\"type\":\"response.output_item.done\",\"output_index\":0}\n\n"
            "event: response.completed\ndata: {\"type\":\"response.completed\",\"response\":{\"usage\":{\"input_tokens\":2,\"output_tokens\":3,\"total_tokens\":5}}}\n\n";
        OpenAIResponsesAdapter adapter;  // 创建工具流 Decoder 的 Adapter
        SdkError error;                  // 流解析或聚合错误
        auto decoder = adapter.createStreamDecoder(error);  // 请求级 Decoder
        StreamSession session;  // canonical 流聚合器
        const StreamEventSink sink = [&](const StreamEvent& event, SdkError& failure) {  // 将工具事件交给 Session
            return session.apply(event, failure);
        };
        QVERIFY(decoder->feed(stream, sink, error));
        QVERIFY(decoder->finish(sink, error));
        const ChatResponse response = session.response();  // 完整工具响应
        QCOMPARE(response.message.toolCalls().size(), 1);
        QCOMPARE(response.message.toolCalls().first().name, QStringLiteral("add"));
        QCOMPARE(response.finishReason, FinishReason::ToolCalls);
    }
};

QTEST_APPLESS_MAIN(OpenAIResponsesTest)
#include "tst_OpenAIResponses.moc"
