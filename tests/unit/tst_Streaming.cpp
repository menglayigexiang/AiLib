#include <AiLib/client/LLMClientFactory.h>
#include <AiLib/protocol/openai/OpenAIChatCompatibleAdapter.h>
#include <AiLib/protocol/anthropic/AnthropicMessagesAdapter.h>
#include <AiLib/stream/StreamSession.h>
#include "../support/FakeTransport.h"
#include "../support/TestOptions.h"
#include "../support/LocalHttpServer.h"
#include <QtTest>
#include <QJsonArray>
#include <QJsonDocument>
#include <stdexcept>
#include <thread>
#include <atomic>
#include <vector>

using namespace AiLib;
namespace {
QByteArray frame(const QJsonObject& root,
                 const QByteArray& event = {})  // 将原生对象封装为完整 SSE 事件
{
    return (event.isEmpty() ? QByteArray() : "event: " + event + '\n') +
           "data: " + QJsonDocument(root).toJson(QJsonDocument::Compact) + "\n\n";
}
QByteArray chunk(const QJsonObject& delta,
                 const QJsonValue& finish = QJsonValue())  // 构造唯一候选的 Chat Completions 分片
{
    return frame({{"id", "chat1"},
                  {"model", "test"},
                  {"choices", QJsonArray{QJsonObject{
                                  {"index", 0}, {"delta", delta}, {"finish_reason", finish}}}}});
}
QByteArray openAIText(bool done = true)  // 构造带结束后 Usage 的完整或中断文字流
{
    QByteArray bytes = chunk({{"content", QStringLiteral("你好")}}) +  // 文本及模型停止通知
                       chunk({{"content", "!"}}, "stop");
    bytes +=
        frame({{"id", "chat1"},
               {"choices", QJsonArray{}},
               {"usage",
                QJsonObject{{"prompt_tokens", 0}, {"completion_tokens", 2}, {"total_tokens", 2}}}});
    if (done)
        bytes += "data: [DONE]\n\n";
    return bytes;
}
QByteArray anthropicStart()  // 构造原生 Messages 响应开始及初始用量
{
    return frame({{"type", "message_start"},
                  {"message",
                   QJsonObject{{"type", "message"},
                               {"role", "assistant"},
                               {"id", "msg1"},
                               {"model", "test"},
                               {"content", QJsonArray{}},
                               {"usage", QJsonObject{{"input_tokens", 0}, {"output_tokens", 1}}}}}},
                 "message_start");
}
QByteArray anthropicBlock(const QString& type,
                          int index,
                          const QJsonObject& value = {})  // 构造指定原生索引的块事件
{
    QJsonObject root{{"type", type}, {"index", index}};  // 当前块事件的原生数据
    if (type == "content_block_start")
        root.insert("content_block", value);
    if (type == "content_block_delta")
        root.insert("delta", value);
    return frame(root, type.toUtf8());
}
QByteArray
anthropicEnd(const QString& reason = QStringLiteral("end_turn"))  // 构造累计用量及独立协议结束事件
{
    return frame({{"type", "message_delta"},
                  {"delta", QJsonObject{{"stop_reason", reason}}},
                  {"usage", QJsonObject{{"output_tokens", 5}}}},
                 "message_delta") +
           frame({{"type", "message_stop"}}, "message_stop");
}
QList<QByteArray>
byteChunks(const QByteArray& bytes)  // 在每个字节边界拆包，覆盖 UTF-8 和 CRLF 被拆开的情况
{
    QList<QByteArray> output;  // 按原顺序交付的原始片段
    for (const char byte : bytes)
        output.append(QByteArray(1, byte));  // 当前单字节网络片段
    return output;
}
ProviderConfig
provider(ProtocolType protocol = ProtocolType::OpenAIChatCompletions)  // 构造离线服务配置
{
    ProviderConfig config;  // 显式指定协议的假地址
    config.protocol = protocol;
    config.baseUrl = QUrl(QStringLiteral("https://example.com"));
    return config;
}
ChatRequest request()  // 构造单候选流式请求，保持与普通 Chat 相同 API
{
    ChatRequest chat;  // 一个用户问题的流式输入
    chat.model = QStringLiteral("test");
    chat.stream = true;
    chat.messages.append(Message::user(QStringLiteral("测试")));
    return chat;
}
std::unique_ptr<LLMClient>
client(FakeTransport* fake,
       ProtocolType protocol =
           ProtocolType::OpenAIChatCompletions)  // 转移 Fake 所有权并创建对应协议 Client
{
    std::unique_ptr<IProtocolAdapter> adapter;  // 与当前服务匹配的协议实现
    if (protocol == ProtocolType::AnthropicMessages)
        adapter = std::make_unique<AnthropicMessagesAdapter>();
    else
        adapter = std::make_unique<OpenAIChatCompatibleAdapter>();
    return std::make_unique<LLMClient>(provider(protocol), std::move(adapter),
                                       std::unique_ptr<ITransport>(fake), singleAttemptOptions());
}
}  // 内部命名空间结束

// 覆盖协议解析、统一聚合、取消及真实网络拆包，不执行任何工具。
class TestStreaming final : public QObject {
    Q_OBJECT
private slots:
    void openAITextAndUsage()  // 验证 UTF-8 拆包、同步通知及 FinishReason 后的 Usage
    {
        auto* fake = new FakeTransport;                     // 即将转移给 Client 的测试传输
        QByteArray bytes = ": comment\n\n" + openAIText();  // 含心跳注释的原始 SSE
        bytes.replace("\n", "\r\n");
        fake->enqueueStream(byteChunks(bytes));
        auto sdk = client(fake);                                  // 独占 Fake 的同步 Client
        ChatResponse output;                                      // 最终聚合结果
        SdkError error;                                           // 流程故障输出
        QList<StreamEventType> events;                            // 实时通知的次序记录
        const auto caller = std::this_thread::get_id();           // 验证回调与调用方线程一致
        RequestOptions options = singleAttemptOptions();          // 本次实时通知及执行选项
        options.streamCallback = [&](const StreamEvent& event) {  // 只观察事件，不自行聚合响应
            QCOMPARE(std::this_thread::get_id(), caller);
            QVERIFY(!fake->requests().isEmpty());
            events.append(event.type);
        };
        QVERIFY(sdk->chat(request(), output, error, options));
        QCOMPARE(output.message.text(), QStringLiteral("你好!"));
        QCOMPARE(output.completionState, CompletionState::Complete);
        QCOMPARE(*output.usage.inputTokens, qint64(0));
        QCOMPARE(*output.usage.totalTokens, qint64(2));
        QVERIFY(events.indexOf(StreamEventType::FinishReasonReceived) <
                events.indexOf(StreamEventType::UsageUpdated));
        QCOMPARE(events.last(), StreamEventType::ResponseCompleted);
        const auto body =  // 实际流式请求参数
            QJsonDocument::fromJson(fake->requests().first().body).object();
        QVERIFY(body.value("stream").toBool());
        QVERIFY(body.value("stream_options").toObject().value("include_usage").toBool());
    }
    void sseMultilineAndCrOnly()  // 验证 BOM、多行 data 和单独 CR 换行，包括末尾 CR 的 EOF 处理
    {
        auto* fake = new FakeTransport;    // 当前 SSE 边界测试 Fake
        QByteArray bytes("\xEF\xBB\xBF");  // 标准 UTF-8 BOM
        const QJsonObject object{
            // 多行 JSON 的完整候选
            {"id", "chat1"},
            {"model", "test"},
            {"choices", QJsonArray{QJsonObject{{"index", 0},
                                               {"delta", QJsonObject{{"content", "keep"}}},
                                               {"finish_reason", "stop"}}}}};
        for (const auto& line : QJsonDocument(object)
                                    .toJson(QJsonDocument::Indented)
                                    .trimmed()
                                    .split('\n')) {  // 当前待包装的 JSON 行
            bytes += "data: " + line + '\r';
        }
        bytes += "\rdata: [DONE]\r\r";
        fake->enqueueStream(byteChunks(bytes));
        auto sdk = client(fake);  // 当前同步 Client
        ChatResponse output;      // 完整响应
        SdkError error;           // SSE 或协议故障
        QVERIFY(sdk->chat(request(), output, error));
        QCOMPARE(output.message.text(), QStringLiteral("keep"));
    }
    void lateIdsAndParallelCalls()  // 验证原生工具索引映射、晚到 ID、交错参数及有序推理
    {
        auto* fake = new FakeTransport;  // 即将移交的离线传输
        QByteArray bytes =
            chunk({{"reasoning_content", QStringLiteral("分析")}}) +  // 推理及文字先后出现
            chunk({{"content", "first"}});
        bytes +=
            chunk({{"tool_calls",
                    QJsonArray{QJsonObject{
                        {"index", 8},
                        {"function", QJsonObject{{"name", "add"}, {"arguments", "{\"a\":"}}}}}}});
        bytes += chunk({{"tool_calls",
                         QJsonArray{QJsonObject{
                             {"index", 3},
                             {"id", "b"},
                             {"type", "function"},
                             {"function", QJsonObject{{"name", "other"}, {"arguments", "{}"}}}}}}});
        bytes += chunk(
            {{"tool_calls",
              QJsonArray{QJsonObject{
                  {"index", 8}, {"id", "a"}, {"function", QJsonObject{{"arguments", "42}"}}}}}}},
            "tool_calls");
        bytes += "data: [DONE]\n\n";
        fake->enqueueStream(byteChunks(bytes));
        auto sdk = client(fake);  // 本次模型 Client
        ChatResponse output;      // 有序聚合结果
        SdkError error;           // 流程故障
        QVERIFY(sdk->chat(request(), output, error));
        QCOMPARE(output.message.contents.size(), 4);
        QVERIFY(std::holds_alternative<ReasoningContent>(output.message.contents[0]));
        QCOMPARE(output.message.toolCalls()[0].id, QStringLiteral("a"));
        QCOMPARE(output.message.toolCalls()[0].arguments.value("a").toInt(), 42);
        QCOMPARE(output.message.toolCalls()[1].id, QStringLiteral("b"));
    }
    void incompleteKeepsData()  // 验证网络正常关闭仍需协议结束标记，未完成工具不会进入消息
    {
        auto* fake = new FakeTransport;  // 当前离线传输
        const QByteArray bytes =         // 文本有效而工具尚未完成
            chunk({{"content", "keep"}}) +
            chunk({{"tool_calls",
                    QJsonArray{QJsonObject{
                        {"index", 0},
                        {"id", "a"},
                        {"function", QJsonObject{{"name", "add"}, {"arguments", "{\"a\":"}}}}}}});
        fake->enqueueStream({bytes});
        auto sdk = client(fake);  // 当前 Client
        ChatResponse output;      // 必须保留已产生文本的部分响应
        SdkError error;           // 缺少协议结束的故障
        QVERIFY(!sdk->chat(request(), output, error));
        QCOMPARE(output.message.text(), QStringLiteral("keep"));
        QVERIFY(output.message.toolCalls().isEmpty());
        QCOMPARE(output.message.status, MessageStatus::Incomplete);
        QCOMPARE(output.completionState, CompletionState::Incomplete);
        QCOMPARE(error.category, ErrorCategory::Protocol);
    }
    void lengthSkipsTruncatedCall()  // Length 正常成功，但截断工具参数不进入正式消息
    {
        auto* fake = new FakeTransport;  // 离线传输
        const QByteArray bytes =         // 模型截断参数后协议正常结束
            chunk({{"content", "keep"},
                   {"tool_calls",
                    QJsonArray{QJsonObject{
                        {"index", 0},
                        {"id", "a"},
                        {"function", QJsonObject{{"name", "add"}, {"arguments", "{"}}}}}}},
                  "length") +
            "data: [DONE]\n\n";
        fake->enqueueStream({bytes});
        auto sdk = client(fake);  // 当前 Client
        ChatResponse output;      // Length 的成功响应
        SdkError error;           // 正常停止时应为空
        QVERIFY(sdk->chat(request(), output, error));
        QCOMPARE(output.finishReason, FinishReason::Length);
        QCOMPARE(output.completionState, CompletionState::Complete);
        QVERIFY(output.message.toolCalls().isEmpty());
        QCOMPARE(output.message.status, MessageStatus::Incomplete);
    }
    void cancellationAndCallbackFailure()  // 验证在通知中取消、异常回调及已有文本保留
    {
        for (const bool cancel : {true, false}) {  // 当前测试是主动取消还是回调抛异常
            auto* fake = new FakeTransport;        // 当前迭代的请求级 Fake
            fake->enqueueStream({openAIText()});
            auto sdk = client(fake);                          // 当前同步 Client
            CancellationSource source;                        // 跨流程共享的取消源
            RequestOptions options = singleAttemptOptions();  // 当前取消上下文及观察回调
            options.cancellation = source.token();
            options.streamCallback =
                [&](const StreamEvent& event) {  // 收到有效文本后请求取消或模拟 UI 故障
                    if (event.type != StreamEventType::TextDelta)
                        return;
                    if (cancel)
                        source.cancel();
                    else
                        throw std::runtime_error("test callback failure");
                };
            ChatResponse output;  // 保留有效文本的部分响应
            SdkError error;       // 取消或回调异常故障
            QVERIFY(!sdk->chat(request(), output, error, options));
            QCOMPARE(output.message.text(), QStringLiteral("你好"));
            QCOMPARE(error.category, cancel ? ErrorCategory::Cancelled : ErrorCategory::Internal);
            QCOMPARE(output.completionState, CompletionState::Incomplete);
        }
    }
    void anthropicOrderAndCumulativeUsage()  // 验证原生块索引映射、工具 JSON 分片及累计统计覆盖
    {
        auto* fake = new FakeTransport;       // 当前 Messages 离线传输
        QByteArray bytes = anthropicStart();  // 唯一助手响应开始
        bytes += anthropicBlock("content_block_start", 9, {{"type", "thinking"}, {"thinking", ""}});
        bytes += anthropicBlock("content_block_delta", 9,
                                {{"type", "thinking_delta"}, {"thinking", QStringLiteral("推理")}});
        bytes += anthropicBlock("content_block_delta", 9,
                                {{"type", "signature_delta"}, {"signature", "native"}});
        bytes += anthropicBlock("content_block_stop", 9);
        bytes += anthropicBlock(
            "content_block_start", 1,
            {{"type", "tool_use"}, {"id", "call1"}, {"name", "add"}, {"input", QJsonObject{}}});
        bytes += anthropicBlock("content_block_delta", 1,
                                {{"type", "input_json_delta"}, {"partial_json", "{\"a\":"}});
        bytes += anthropicBlock("content_block_delta", 1,
                                {{"type", "input_json_delta"}, {"partial_json", "42}"}});
        bytes += anthropicBlock("content_block_stop", 1);
        bytes += anthropicBlock("content_block_start", 4, {{"type", "text"}, {"text", "after"}});
        bytes += anthropicBlock("content_block_stop", 4);
        bytes += anthropicEnd(QStringLiteral("tool_use"));
        fake->enqueueStream(byteChunks(bytes));
        auto sdk = client(fake, ProtocolType::AnthropicMessages);  // 当前 Messages Client
        ChatResponse output;                                       // 标准化聚合输出
        SdkError error;                                            // 流程故障
        QVERIFY(sdk->chat(request(), output, error));
        QCOMPARE(output.message.contents.size(), 3);
        QVERIFY(std::holds_alternative<ReasoningContent>(output.message.contents[0]));
        QVERIFY(std::holds_alternative<ToolCallContent>(output.message.contents[1]));
        QCOMPARE(output.message.toolCalls().first().arguments.value("a").toInt(), 42);
        QCOMPARE(*output.usage.outputTokens, qint64(5));
        QVERIFY(!output.usage.totalTokens);
    }
    void anthropicLengthAndMissingStop()  // 验证 Messages 的截断参数及缺少 message_stop
    {
        for (const bool length : {true, false}) {  // 当前分支是否协议正常结束且模型截断
            auto* fake = new FakeTransport;        // 当前 Fake
            QByteArray bytes = anthropicStart();   // 当前原生响应
            bytes += anthropicBlock(
                "content_block_start", 0,
                {{"type", "tool_use"}, {"id", "a"}, {"name", "add"}, {"input", QJsonObject{}}});
            bytes += anthropicBlock("content_block_delta", 0,
                                    {{"type", "input_json_delta"}, {"partial_json", "{"}});
            if (length)
                bytes += anthropicBlock("content_block_stop", 0) +
                         anthropicEnd(QStringLiteral("max_tokens"));
            fake->enqueueStream({bytes});
            auto sdk = client(fake, ProtocolType::AnthropicMessages);  // 当前 Messages Client
            ChatResponse output;                                       // 截断或异常的部分结果
            SdkError error;                                            // 未正常完成的故障
            QCOMPARE(sdk->chat(request(), output, error), length);
            QVERIFY(output.message.toolCalls().isEmpty());
            QCOMPARE(output.completionState,
                     length ? CompletionState::Complete : CompletionState::Incomplete);
        }
    }
    void invalidStreams_data()  // 提供多候选、非法调用、异常 JSON 及乱序事件样本
    {
        QTest::addColumn<QByteArray>("bytes");
        QTest::addColumn<bool>("anthropic");
        QTest::newRow("zero-choice") << frame({{"choices", QJsonArray{}}}) << false;
        QTest::newRow("multiple-choice")
            << frame({{"choices", QJsonArray{QJsonObject{}, QJsonObject{}}}}) << false;
        QTest::newRow("early-done") << QByteArray("data: [DONE]\n\n") << false;
        QTest::newRow("bad-json") << (chunk({{"content", "keep"}}) + "data: {bad}\n\n") << false;
        QTest::newRow("invalid-arguments")
            << (chunk({{"tool_calls",
                        QJsonArray{QJsonObject{
                            {"index", 0},
                            {"id", "a"},
                            {"function", QJsonObject{{"name", "add"}, {"arguments", "{"}}}}}}},
                      "tool_calls") +
                "data: [DONE]\n\n")
            << false;
        const QJsonObject call{  // 用于制造重复 ID 的完整工具片段
                               {"id", "same"},
                               {"function", QJsonObject{{"name", "add"}, {"arguments", "{}"}}}};
        auto first = call;  // 第一个厂商工具索引
        first.insert("index", 0);
        auto second = call;  // 第二个厂商工具索引，故意使用相同调用 ID
        second.insert("index", 1);
        QTest::newRow("duplicate-id")
            << (chunk({{"tool_calls", QJsonArray{first, second}}}, "tool_calls") +
                "data: [DONE]\n\n")
            << false;
        QTest::newRow("delta-before-start")
            << anthropicBlock("content_block_delta", 0, {{"type", "text_delta"}, {"text", "bad"}})
            << true;
        QTest::newRow("missing-block")
            << (anthropicStart() + anthropicBlock("content_block_stop", 0)) << true;
        QTest::newRow("open-block-at-stop")
            << (anthropicStart() +
                anthropicBlock("content_block_start", 0, {{"type", "text"}, {"text", "keep"}}) +
                anthropicEnd())
            << true;
        QTest::newRow("unknown-content")
            << (anthropicStart() +
                anthropicBlock("content_block_start", 0, {{"type", "server_tool_use"}}))
            << true;
    }
    void invalidStreams()  // 非法流必须失败并保留未完成状态
    {
        QFETCH(QByteArray, bytes);       // 当前原始 SSE 样本
        QFETCH(bool, anthropic);         // 当前样本所属协议
        auto* fake = new FakeTransport;  // 当前 Fake
        fake->enqueueStream(byteChunks(bytes));
        auto sdk = client(fake, anthropic ? ProtocolType::AnthropicMessages  // 当前协议 Client
                                          : ProtocolType::OpenAIChatCompletions);
        ChatResponse output;  // 部分结果
        SdkError error;       // 协议故障
        QVERIFY(!sdk->chat(request(), output, error));
        QCOMPARE(output.completionState, CompletionState::Incomplete);
        QVERIFY(error.category != ErrorCategory::None);
    }
    void networkAndProviderErrors()  // 保留部分内容及 Usage，同时通知网络和流内故障
    {
        for (const bool network : {true, false}) {  // 当前分支是 Transport 故障还是原生流内错误
            auto* fake = new FakeTransport;         // 当前 Fake
            SdkError terminal;                      // 预设的网络故障
            terminal.category = network ? ErrorCategory::Network : ErrorCategory::None;
            terminal.code = network ? QStringLiteral("NetworkError") : QString();
            QByteArray bytes = chunk({{"content", "keep"}});  // 故障前已生成的有效文本
            bytes +=
                frame({{"choices", QJsonArray{}}, {"usage", QJsonObject{{"prompt_tokens", 7}}}});
            if (!network)
                bytes += frame({{"error", QJsonObject{{"type", "overloaded_error"},
                                                      {"message", "test failure"}}}});
            fake->enqueueStream({bytes}, terminal);
            auto sdk = client(fake);                                  // 当前 Client
            int errors = 0;                                           // 已收到的 Error 通知数量
            RequestOptions options = singleAttemptOptions();          // 当前实时通知
            options.streamCallback = [&](const StreamEvent& event) {  // 只记录统一错误事件
                if (event.type == StreamEventType::Error)
                    ++errors;
            };
            ChatResponse output;  // 保留有效文本和用量的部分响应
            SdkError error;       // 网络或 Provider 原因
            QVERIFY(!sdk->chat(request(), output, error, options));
            QCOMPARE(output.message.text(), QStringLiteral("keep"));
            QCOMPARE(*output.usage.inputTokens, qint64(7));
            QCOMPARE(error.category, network ? ErrorCategory::Network : ErrorCategory::Provider);
            QCOMPARE(errors, 1);
        }
        auto* fake = new FakeTransport;  // 非流式 HTTP 错误正文的 Fake
        fake->enqueue({429, {{"Retry-After", "2"}}, "{\"error\":{\"message\":\"rate limited\"}}"});
        auto sdk = client(fake);  // HTTP 错误仍交给原 Adapter 解码
        ChatResponse output;      // 不完整的故障响应
        SdkError error;           // 规范化限流故障
        QVERIFY(!sdk->chat(request(), output, error));
        QCOMPARE(error.category, ErrorCategory::RateLimited);
        QCOMPARE(*error.retryAfterMs, qint64(2000));
    }
    void localHttpStreaming()  // 使用真实 Qt 网络栈验证在连接结束前回调与逐片 SSE 聚合
    {
        const QByteArray first = chunk({{"content", "keep"}});  // 第一个立即交付的有效片段
        const QByteArray tail =                                 // 延迟交付的协议结束片段
            chunk({{"content", "after"}}, "stop") + "data: [DONE]\n\n";
        bool tailSent = false;                           // 验证实时回调发生时连接尚未结束
        LocalHttpServer server([&](QTcpSocket* socket,   // 本次调用、上下文或业务结果参数
                                   const QByteArray&) {  // 测试应用模拟两次网络写入
            socket->write("HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nContent-Length: " +
                          QByteArray::number(first.size() + tail.size()) +
                          "\r\nConnection: close\r\n\r\n" + first);
            socket->flush();
            QTimer::singleShot(50, socket, [&, socket] {  // 在同一测试线程延迟尾部响应
                tailSent = true;
                socket->write(tail);
                socket->disconnectFromHost();
            });
        });
        QVERIFY(server.listen());
        auto config = provider();  // 环回地址的真实协议服务
        config.baseUrl = server.url();
        std::unique_ptr<LLMClient> sdk;  // Factory 组装真实 Transport
        SdkError error;                  // 流程故障
        QVERIFY(LLMClientFactory::create(config, sdk, error));
        bool observedBeforeClose = false;                         // 是否在尾部写入前观察到首个文本增量
        RequestOptions options = singleAttemptOptions();          // 当前实时回调
        options.streamCallback = [&](const StreamEvent& event) {  // 回调只观察网络时序
            if (event.type == StreamEventType::TextDelta && event.delta == QStringLiteral("keep"))
                observedBeforeClose = !tailSent;
        };
        ChatResponse output;  // 最终完整响应
        QVERIFY(sdk->chat(request(), output, error, options));
        QVERIFY(observedBeforeClose);
        QCOMPARE(output.message.text(), QStringLiteral("keepafter"));
    }
    void localHttpCancellationAndDisconnect()  // 验证真实流式取消与异常断连均保留已经收到的文本
    {
        for (const bool cancel : {true, false}) {                   // 当前测试模拟主动取消还是异常关闭
            const QByteArray first = chunk({{"content", "keep"}});  // 故障前成功解析的文字
            LocalHttpServer server([&](QTcpSocket* socket,          // 本次调用、上下文或业务结果参数
                                       const QByteArray&) {         // 测试服务故意不完成声明的响应长度
                socket->write(
                    "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nContent-Length: 999999\r\nConnection: close\r\n\r\n" +
                    first);
                socket->flush();
                if (!cancel)
                    QTimer::singleShot(40, socket, [socket] {
                        socket->disconnectFromHost();
                    });  // 模拟协议结束前断连
            });
            QVERIFY(server.listen());
            auto config = provider();  // 当前本地服务地址
            config.baseUrl = server.url();
            std::unique_ptr<LLMClient> sdk;  // 当前真实网络 Client
            SdkError error;                  // 故障输出
            QVERIFY(LLMClientFactory::create(config, sdk, error));
            CancellationSource source;                        // 本次统一取消源
            RequestOptions options = singleAttemptOptions();  // 当前 Token 及回调
            options.cancellation = source.token();
            options.streamCallback = [&](const StreamEvent& event) {  // 在已收到有效文本后协作取消
                if (cancel && event.type == StreamEventType::TextDelta)
                    source.cancel();
            };
            ChatResponse output;  // 故障时保留的部分结果
            QVERIFY(!sdk->chat(request(), output, error, options));
            QCOMPARE(output.message.text(), QStringLiteral("keep"));
            QCOMPARE(error.category, cancel ? ErrorCategory::Cancelled : ErrorCategory::Network);
            QCOMPARE(output.message.status, MessageStatus::Incomplete);
        }
    }
    void localHttpTimeout()  // 真实网络超时保留有效文本，并返回底层请求 Timeout
    {
        const QByteArray first = chunk({{"content", "keep"}});  // 超时前已经收到的有效片段
        LocalHttpServer server([&](QTcpSocket* socket,          // 本次调用、上下文或业务结果参数
                                   const QByteArray&) {         // 本地服务交付文本后保持连接不结束
            socket->write(
                "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nContent-Length: 999999\r\n\r\n" +
                first);
            socket->flush();
        });
        QVERIFY(server.listen());
        auto config = provider();  // 当前本地服务配置
        config.baseUrl = server.url();
        std::unique_ptr<LLMClient> sdk;  // 当前真实网络 Client
        SdkError error;                  // 超时故障输出
        QVERIFY(LLMClientFactory::create(config, sdk, error));
        RequestOptions options = singleAttemptOptions();  // 短时测试请求配置
        options.timeoutSeconds = 1;
        ChatResponse output;  // 超时后的部分响应
        QVERIFY(!sdk->chat(request(), output, error, options));
        QCOMPARE(error.category, ErrorCategory::Timeout);
        QCOMPARE(output.message.text(), QStringLiteral("keep"));
        QCOMPARE(output.completionState, CompletionState::Incomplete);
    }
    void sessionRejectsInvalidScope()  // 自定义 Decoder 也必须满足逻辑索引契约，故障时保留先前文本
    {
        StreamSession session;  // 不含厂商解析逻辑的独立聚合中心
        StreamEvent event;      // 人工构造的标准化事件
        SdkError error;         // 契约故障输出
        event.type = StreamEventType::PartStarted;
        event.partIndex = 7;
        QVERIFY(session.apply(event, error));
        event.type = StreamEventType::TextDelta;
        event.delta = QStringLiteral("keep");
        QVERIFY(session.apply(event, error));
        event.type = StreamEventType::UsageUpdated;
        event.partIndex = -2;
        QVERIFY(!session.apply(event, error));
        QCOMPARE(error.category, ErrorCategory::Protocol);
        QCOMPARE(session.response().message.text(), QStringLiteral("keep"));
        QCOMPARE(session.response().completionState, CompletionState::Incomplete);
    }
    void concurrentRequests()  // 验证共享 Client 的 Decoder 与 Session 不共享可变请求状态
    {
        auto* fake = new FakeTransport;  // 支持同步队列消费的共享传输
        for (int i = 0; i < 4; ++i)
            fake->enqueueStream(byteChunks(openAIText()));  // 当前待预设的独立请求序号
        auto sdk = client(fake);                            // 多线程共享但配置不可变的 Client
        std::atomic<int> completed{0};                      // 正确完成的独立请求数
        std::vector<std::thread> threads;                   // 仅由测试调用方创建的线程
        for (int i = 0; i < 4; ++i) {                       // 当前调用线程序号
            threads.emplace_back([&] {                      // 每个线程调用同步 API，各自拥有结果和故障
                ChatResponse output;                        // 当前线程独立结果
                SdkError error;                             // 当前线程独立错误
                if (sdk->chat(request(), output, error) &&
                    output.message.text() == QStringLiteral("你好!"))
                    ++completed;
            });
        }
        for (auto& thread : threads)
            thread.join();  // 当前需要等待的应用测试线程
        QCOMPARE(completed.load(), 4);
    }
};
QTEST_GUILESS_MAIN(TestStreaming)
#include "tst_Streaming.moc"
