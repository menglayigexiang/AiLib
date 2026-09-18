#include <AiLib/client/LLMClient.h>
#include <AiLib/protocol/openai/OpenAIChatCompatibleAdapter.h>
#include <AiLib/protocol/anthropic/AnthropicMessagesAdapter.h>
#include <AiLib/agent/Agent.h>
#include <AiLib/network/QtHttpTransport.h>
#include "../support/FakeTransport.h"
#include "../support/LocalHttpServer.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QElapsedTimer>
#include <QtTest>
#include <thread>
using namespace AiLib;
namespace {
ChatRequest input(bool stream = false)  // 创建普通或流式标准测试请求
{
    ChatRequest request;  // 当前模型输入
    request.model = "test-model";
    request.messages.append(Message::user("问题"));
    request.stream = stream;
    return request;
}
TransportResponse answer(int status = 200,
                         const QByteArray& retryAfter = {})  // 构造成功或服务端临时故障
{
    TransportResponse response;  // 当前单次 HTTP 结果
    response.statusCode = status;
    response.body =
        status == 200
            ? R"({"choices":[{"message":{"role":"assistant","content":"成功"},"finish_reason":"stop"}]})"
            : R"({"error":{"message":"服务端错误"}})";
    if (!retryAfter.isEmpty())
        response.headers.append(qMakePair(QByteArray("Retry-After"), retryAfter));
    return response;
}
std::unique_ptr<LLMClient> client(FakeTransport*& fake)  // 注入真实 Adapter 和预设网络结果
{
    auto transport = std::make_unique<FakeTransport>();  // Client 即将拥有的测试传输
    fake = transport.get();
    ProviderConfig provider;  // 无认证的离线服务
    provider.baseUrl = QUrl("https://example.com/v1/");
    return std::make_unique<LLMClient>(provider, std::make_unique<OpenAIChatCompatibleAdapter>(),
                                       std::move(transport));
}
RequestOptions options()  // 返回快速但真实使用自动重试的测试配置
{
    RequestOptions result;  // 本次重试策略
    result.retryPolicy.maxRetries = 2;
    result.retryPolicy.retryIntervalMs = 0;
    return result;
}
SdkError networkError()  // 自定义 Transport 未提供细分信息时的临时连接故障
{
    SdkError error;  // 连接故障描述
    error.category = ErrorCategory::Network;
    error.code = "ConnectionLost";
    return error;
}
QByteArray streamAnswer()  // 单候选协议完整流
{
    return "data: {\"choices\":[{\"index\":0,\"delta\":{\"role\":\"assistant\",\"content\":\"成功\"},\"finish_reason\":\"stop\"}]}\n\ndata: [DONE]\n\n";
}
}
// 验证可配置重试、流式不可重复边界和同步等待的取消/截止时间。
class RetryTest : public QObject {
    Q_OBJECT
private slots:
    void statuses_data();        // HTTP 状态和开关分类
    void statuses();             // 永久请求错误不能重试
    void attempts_data();        // 首次请求不计入重试次数
    void attempts();             // 耗尽时保留最后一次故障
    void network_data();         // 网络细分、策略关闭及非网络故障
    void network();              // 临时连接错误按策略重试
    void streamBoundary_data();  // 没有内容与已经产生有效增量
    void streamBoundary();       // 有效流内容后禁止重复输出
    void retryAfterDate_data();  // 三种 HTTP 日期建议
    void retryAfterDate();       // 服务端 HTTP 日期形式的等待建议
    void retryAfter();           // 优先采用服务端建议，耗尽保留建议
    void cancelWaiting();        // 重试等待能响应共享 Token
    void deadlineWaiting();      // Retry-After 等待受总截止时间约束
    void localHttp_data();       // 真实网络普通与流式重试
    void localHttp();            // 真实 Qt 网络栈连接多轮服务端响应
    void perAttemptDeadline();   // 重试后挂起的网络请求仍受总预算约束
    void anthropic_data();       // 两种 Messages 请求模式的限流重试
    void anthropic();            // 公共重试层不依赖 OpenAI 协议
    void agentBudget();          // Agent 总时间限制覆盖重试等待
    void timeoutResets();        // 每一次尝试独立计算请求超时
    void agent_data();           // 重试成功和耗尽后的增量保留
    void agent();                // 重试不增加 Agent 轮数或实际工具次数
};
void RetryTest::statuses_data()  // 400 系列永久故障和 429/5xx 临时故障
{
    QTest::addColumn<int>("status");
    QTest::addColumn<bool>("enabled");
    QTest::addColumn<bool>("retries");
    for (int status : {400, 401, 403, 404, 408, 422}) {  // 默认不重试的请求状态
        QTest::newRow(qPrintable(QString::number(status))) << status << true << false;
    }
    QTest::newRow("429") << 429 << true << true;
    QTest::newRow("429-disabled") << 429 << false << false;
    QTest::newRow("500") << 500 << true << true;
    QTest::newRow("503") << 503 << true << true;
    QTest::newRow("503-disabled") << 503 << false << false;
}
void RetryTest::statuses()  // 只有被策略启用的临时 HTTP 错误重试
{
    QFETCH(int, status);            // 当前 HTTP 状态
    QFETCH(bool, enabled);          // 当前类别是否允许重试
    QFETCH(bool, retries);          // 预期是否重试
    FakeTransport* fake = nullptr;  // 请求观察指针
    auto sdk = client(fake);        // 本次测试 Client
    fake->enqueue(answer(status));
    fake->enqueue(answer());
    auto config = options();  // 本次局部策略
    config.retryPolicy.retryOnRateLimit = enabled;
    config.retryPolicy.retryOnServerError = enabled;
    ChatResponse response;  // 最终尝试的输出
    SdkError error;         // 最终失败原因
    QCOMPARE(sdk->chat(input(), response, error, config), retries);
    QCOMPARE(fake->requests().size(), retries ? 2 : 1);
    if (retries) {
        QCOMPARE(response.message.text(), QStringLiteral("成功"));
        QCOMPARE(error.category, ErrorCategory::None);
    } else
        QCOMPARE(*error.httpStatus, status);
}
void RetryTest::attempts_data()  // 0、1、2 和默认 5 次重试
{
    QTest::addColumn<int>("maxRetries");
    for (int count : {0, 1, 2, 5}) {  // 最大重试次数
        QTest::newRow(qPrintable(QString::number(count))) << count;
    }
}
void RetryTest::attempts()  // 首次请求加 maxRetries 次才是总请求数
{
    QFETCH(int, maxRetries);        // 当前次数上限
    FakeTransport* fake = nullptr;  // 请求观察指针
    auto sdk = client(fake);        // 测试 Client
    for (int index = 0; index <= maxRetries; ++index)
        fake->enqueue(answer(503, "0"));  // 为所有尝试预设临时故障
    auto config = options();              // 不额外等待的重试策略
    config.retryPolicy.maxRetries = maxRetries;
    ChatResponse response;  // 最终输出
    SdkError error;         // 最后一次故障
    QVERIFY(!sdk->chat(input(), response, error, config));
    QCOMPARE(fake->requests().size(), maxRetries + 1);
    QCOMPARE(*error.httpStatus, 503);
    QCOMPARE(*error.retryAfterMs, qint64(0));
    QCOMPARE(RequestOptions{}.retryPolicy.maxRetries, 5);
}
void RetryTest::network_data()  // 临时连接故障与永久/非网络故障
{
    QTest::addColumn<int>("mode");
    for (int mode = 0; mode < 5; ++mode)
        QTest::newRow(qPrintable(QString::number(mode))) << mode;  // 当前故障编号
}
void RetryTest::network()  // 非临时连接、超时和协议错误不自动重放
{
    QFETCH(int, mode);              // 当前故障情形
    FakeTransport* fake = nullptr;  // 请求观察指针
    auto sdk = client(fake);        // 测试 Client
    auto failure = networkError();  // 自定义连接故障
    if (mode == 1)
        failure.details.insert("retryable", false);
    if (mode == 3)
        failure.category = ErrorCategory::Timeout;
    if (mode == 4)
        failure.category = ErrorCategory::Protocol;
    fake->enqueueError(failure);
    fake->enqueue(answer());
    auto config = options();  // 当前重试开关
    if (mode == 2)
        config.retryPolicy.retryOnNetworkError = false;
    ChatResponse response;  // 最终尝试的响应
    SdkError error;         // 最终故障
    QCOMPARE(sdk->chat(input(), response, error, config), mode == 0);
    QCOMPARE(fake->requests().size(), mode == 0 ? 2 : 1);
}
void RetryTest::streamBoundary_data()  // 回调存在与否都必须遵守不可重复边界
{
    QTest::addColumn<int>("mode");
    QTest::addColumn<bool>("callback");
    for (int mode = 0; mode < 5; ++mode) {     // 当前有效内容类别
        for (bool callback : {false, true}) {  // 调用方是否订阅事件
            QTest::newRow(
                qPrintable(QString::number(mode) + (callback ? "-callback" : "-no-callback")))
                << mode << callback;
        }
    }
}
void RetryTest::streamBoundary()  // 每次重试重新创建 Session 和 Decoder
{
    QFETCH(bool, callback);         // 是否设置外部事件接收器
    QFETCH(int, mode);              // 当前内容边界
    FakeTransport* fake = nullptr;  // 请求观察指针
    auto sdk = client(fake);        // 流式测试 Client
    QList<QByteArray> chunks;       // 首次尝试的部分流
    if (mode == 1)
        chunks.append(
            "data: {\"id\":\"failed-response\",\"choices\":[{\"index\":0,\"delta\":{\"role\":\"assistant\"},\"finish_reason\":null}]}\n\n");
    if (mode == 2)
        chunks.append(
            "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":\"部分\"},\"finish_reason\":null}]}\n\n");
    if (mode == 3)
        chunks.append(
            "data: {\"choices\":[{\"index\":0,\"delta\":{\"reasoning_content\":\"推理\"},\"finish_reason\":null}]}\n\n");
    if (mode == 4)
        chunks.append(
            "data: {\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"call-1\",\"type\":\"function\",\"function\":{\"name\":\"work\",\"arguments\":\"{\"}}]},\"finish_reason\":null}]}\n\n");
    fake->enqueueStream(chunks, networkError());
    fake->enqueueStream({streamAnswer()});
    auto config = options();  // 当前重试配置与标准化事件观察
    int errors = 0;           // 最终 Error 通知次数
    QString text;             // 调用方实际收到的文本，不允许重复重放
    if (callback)
        config.streamCallback = [&errors, &text](const StreamEvent& event) {  // 只观察标准化事件
            if (event.type == StreamEventType::Error)
                ++errors;
            if (event.type == StreamEventType::TextDelta)
                text += event.delta;
        };
    ChatResponse response;  // 最后尝试或有效部分响应
    SdkError error;         // 最终故障
    QCOMPARE(sdk->chat(input(true), response, error, config), mode < 2);
    QCOMPARE(fake->requests().size(), mode < 2 ? 2 : 1);
    QCOMPARE(errors, mode < 2 || !callback ? 0 : 1);
    if (mode < 2) {
        if (callback)
            QCOMPARE(text, QStringLiteral("成功"));
        QVERIFY(response.id != "failed-response");
    }
    if (mode == 2 && callback)
        QCOMPARE(text, QStringLiteral("部分"));
    if (mode >= 2)
        QCOMPARE(response.completionState, CompletionState::Incomplete);
}
void RetryTest::retryAfterDate_data()  // HTTP 收件方接受的三种日期形式
{
    QTest::addColumn<QByteArray>("date");
    QTest::newRow("IMF-fixdate") << QByteArray("Sat, 01 Jan 2000 00:00:00 GMT");
    QTest::newRow("RFC850") << QByteArray("Saturday, 01-Jan-00 00:00:00 GMT");
    QTest::newRow("asctime") << QByteArray("Sat Jan  1 00:00:00 2000");
}
void RetryTest::retryAfterDate()  // 过期 HTTP 日期保留为零等待，不误判为未知
{
    QFETCH(QByteArray, date);       // 当前服务端日期形式
    FakeTransport* fake = nullptr;  // 请求观察指针
    auto sdk = client(fake);        // 普通 Client
    fake->enqueue(answer(503, date));
    auto config = options();  // 单次错误用于检查原始建议保留
    config.retryPolicy.maxRetries = 0;
    ChatResponse response;  // 最终输出
    SdkError error;         // Provider 原始故障
    QVERIFY(!sdk->chat(input(), response, error, config));
    QVERIFY(error.retryAfterMs.has_value());
    QCOMPARE(*error.retryAfterMs, qint64(0));
}
void RetryTest::retryAfter()  // 服务端建议覆盖 30 秒固定间隔
{
    FakeTransport* fake = nullptr;  // 请求观察指针
    auto sdk = client(fake);        // 普通重试 Client
    fake->enqueue(answer(429, "1"));
    fake->enqueue(answer());
    auto config = options();  // 默认间隔很长但服务端建议较短
    config.retryPolicy.retryIntervalMs = 30000;
    QElapsedTimer timer;  // 等待时长证据
    timer.start();
    ChatResponse response;  // 成功重试后的响应
    SdkError error;         // 应清空的故障
    QVERIFY(sdk->chat(input(), response, error, config));
    QVERIFY(timer.elapsed() >= 950);
    QVERIFY(timer.elapsed() < 1600);
    QCOMPARE(fake->requests().size(), 2);
}
void RetryTest::cancelWaiting()  // 应用取消另一个线程中的同步重试等待
{
    FakeTransport* fake = nullptr;  // 请求观察指针
    auto sdk = client(fake);        // 普通 Client
    fake->enqueue(answer(503));
    auto config = options();  // 长等待便于验证取消响应
    config.retryPolicy.retryIntervalMs = 5000;
    CancellationSource source;  // 应用唯一取消来源
    config.cancellation = source.token();
    std::thread caller([source]() mutable {  // 同步处理本次业务或事件，不创建线程
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        source.cancel();
    });                                                                // 应用线程发起取消，SDK 不创建线程
    ChatResponse response;                                             // 最终有效输出
    SdkError error;                                                    // 取消原因
    const bool success = sdk->chat(input(), response, error, config);  // 同步等待中协作停止
    caller.join();
    QVERIFY(!success);
    QCOMPARE(error.category, ErrorCategory::Cancelled);
    QCOMPARE(fake->requests().size(), 1);
}
void RetryTest::deadlineWaiting()  // 服务端一小时建议等待受精确预算约束
{
    FakeTransport* fake = nullptr;  // 请求观察指针
    auto sdk = client(fake);        // 普通 Client
    fake->enqueue(answer(429, "3600"));
    auto config = options();  // 很短的总请求预算
    config.deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(80);
    ChatResponse response;  // 无有效内容的最终响应
    SdkError error;         // 总预算耗尽
    QVERIFY(!sdk->chat(input(), response, error, config));
    QCOMPARE(error.category, ErrorCategory::Timeout);
    QCOMPARE(fake->requests().size(), 1);
}
void RetryTest::localHttp_data()  // 普通与 Streaming 共用重试协调层
{
    QTest::addColumn<bool>("stream");
    QTest::newRow("normal") << false;
    QTest::newRow("stream") << true;
}
void RetryTest::localHttp()  // 本地服务先两次 503，再完整成功
{
    QFETCH(bool, stream);  // 当前普通或流式请求
    int count = 0;         // 本地服务实际接收次数
    LocalHttpServer server(
        [&count, stream](QTcpSocket* socket, const QByteArray&) {  // 模拟临时服务不可用后恢复
            ++count;
            socket->write(
                count <= 2
                    ? LocalHttpServer::httpResponse(503, "{\"error\":{\"message\":\"busy\"}}",
                                                    "Retry-After: 0\r\n")
                    : LocalHttpServer::httpResponse(200, stream ? streamAnswer() : answer().body));
            socket->disconnectFromHost();
        });
    QVERIFY(server.listen());
    ProviderConfig provider;  // 环回服务 API 根路径
    provider.baseUrl = server.url("/v1/");
    LLMClient sdk(provider, std::make_unique<OpenAIChatCompatibleAdapter>(),
                  std::make_unique<QtHttpTransport>());  // 真实 Qt 网络闭环
    ChatResponse response;                               // 最终完整模型响应
    SdkError error;                                      // 应清空的临时故障
    QVERIFY(sdk.chat(input(stream), response, error, options()));
    QCOMPARE(response.message.text(), QStringLiteral("成功"));
    QCOMPARE(count, 3);
}
void RetryTest::perAttemptDeadline()  // 每次重试网络请求继续遵守同一总截止时间
{
    int count = 0;  // 本地服务实际请求数
    LocalHttpServer server(
        [&count](QTcpSocket* socket, const QByteArray&) {  // 首次 503 后保持第二次连接不返回
            if (++count == 1) {
                socket->write(LocalHttpServer::httpResponse(503, "{}", "Retry-After: 0\r\n"));
                socket->disconnectFromHost();
            }
        });
    QVERIFY(server.listen());
    ProviderConfig provider;  // 环回服务配置
    provider.baseUrl = server.url("/v1/");
    LLMClient sdk(provider, std::make_unique<OpenAIChatCompatibleAdapter>(),
                  std::make_unique<QtHttpTransport>());  // 真实网络 Client
    auto config = options();                             // 总预算比单次 120 秒上限更早
    config.deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
    ChatResponse response;  // 未完整获得的响应
    SdkError error;         // 总截止时间到达
    QVERIFY(!sdk.chat(input(), response, error, config));
    QCOMPARE(error.category, ErrorCategory::Timeout);
    QCOMPARE(count, 2);
}
void RetryTest::anthropic_data()  // Messages 普通和流式限流后恢复
{
    QTest::addColumn<bool>("stream");
    QTest::newRow("normal") << false;
    QTest::newRow("stream") << true;
}
void RetryTest::anthropic()  // 重试只消费规范化错误，不理解厂商事件名称
{
    QFETCH(bool, stream);                                // 当前模式
    auto transport = std::make_unique<FakeTransport>();  // Client 即将拥有的测试传输
    auto* fake = transport.get();                        // 生命周期内的观察指针
    fake->enqueue(answer(429, "0"));
    if (stream) {
        fake->enqueueStream(
            {R"(data: {"type":"message_start","message":{"id":"msg","type":"message","role":"assistant","content":[],"usage":{"input_tokens":1,"output_tokens":0}}}

data: {"type":"content_block_start","index":0,"content_block":{"type":"text","text":""}}

data: {"type":"content_block_delta","index":0,"delta":{"type":"text_delta","text":"成功"}}

data: {"type":"content_block_stop","index":0}

data: {"type":"message_delta","delta":{"stop_reason":"end_turn"},"usage":{"output_tokens":1}}

data: {"type":"message_stop"}

)"});
    } else {
        fake->enqueue(TransportResponse{
            200,
            {},
            R"({"id":"msg","type":"message","role":"assistant","content":[{"type":"text","text":"成功"}],"stop_reason":"end_turn","usage":{"input_tokens":1,"output_tokens":1}})"});
    }
    ProviderConfig provider;  // 显式 Messages 协议的服务配置
    provider.baseUrl = QUrl("https://example.com/");
    provider.protocol = ProtocolType::AnthropicMessages;
    LLMClient sdk(provider, std::make_unique<AnthropicMessagesAdapter>(),
                  std::move(transport));  // 协议无关的重试协调
    auto request = input(stream);         // Messages 要求输出上限
    request.maxOutputTokens = 64;
    ChatResponse response;  // 重试后的完整响应
    SdkError error;         // 应清空的故障
    QVERIFY(sdk.chat(request, response, error, options()));
    QCOMPARE(response.message.text(), QStringLiteral("成功"));
    QCOMPARE(fake->requests().size(), 2);
}
void RetryTest::agentBudget()  // 总预算比 Retry-After 建议更短时正常结束 Timeout
{
    FakeTransport* fake = nullptr;  // 请求观察指针
    auto sdk = client(fake);        // 即将由 Agent 独占的 Client
    fake->enqueue(answer(429, "2"));
    ToolRegistry registry;                  // 无工具的应用集合
    Agent agent(std::move(sdk), registry);  // 同步 Agent
    AgentRequest request;                   // 一秒总运行预算
    request.chat = input();
    request.limits.totalTimeoutSeconds = 1;
    AgentResult result;  // 正常预算耗尽
    SdkError error;      // 不使用异常故障表达总超时
    QVERIFY(agent.run(request, result, error));
    QCOMPARE(result.finishReason, AgentFinishReason::Timeout);
    QCOMPARE(error.category, ErrorCategory::None);
    QCOMPARE(fake->requests().size(), 1);
    QVERIFY(result.newMessages.isEmpty());
}
void RetryTest::timeoutResets()  // 两次各 600ms 的尝试不能误用一个 1 秒请求超时
{
    int count = 0;                                                            // 本地请求次数
    LocalHttpServer server([&count](QTcpSocket* socket, const QByteArray&) {  // 每次尝试独立延迟响应
        const int status = ++count == 1 ? 503 : 200;                          // 当前尝试的服务端结果
        QTimer::singleShot(600, socket, [socket, status] {                    // 测试服务发送延迟响应
            socket->write(
                LocalHttpServer::httpResponse(status, answer(status).body, "Retry-After: 0\r\n"));
            socket->disconnectFromHost();
        });
    });
    QVERIFY(server.listen());
    ProviderConfig provider;  // 本地 HTTP 服务配置
    provider.baseUrl = server.url("/v1/");
    LLMClient sdk(provider, std::make_unique<OpenAIChatCompatibleAdapter>(),
                  std::make_unique<QtHttpTransport>());  // 真实 Qt 网络请求
    auto config = options();                             // 每次尝试允许一秒
    config.timeoutSeconds = 1;
    ChatResponse response;  // 第二次尝试完整成功
    SdkError error;         // 应清空的临时错误
    QVERIFY(sdk.chat(input(), response, error, config));
    QCOMPARE(count, 2);
}
void RetryTest::agent_data()  // 每轮重试成功或最后一轮耗尽
{
    QTest::addColumn<bool>("failed");
    QTest::newRow("success") << false;
    QTest::newRow("exhausted") << true;
}
void RetryTest::agent()  // 两次逻辑 LLM 调用各重试一次，工具仅执行一次
{
    QFETCH(bool, failed);           // 最后一次尝试是否仍然失败
    FakeTransport* fake = nullptr;  // 请求观察指针
    auto sdk = client(fake);        // Agent 即将独占的 Client
    fake->enqueue(answer(503, "0"));
    fake->enqueue(TransportResponse{
        200,
        {},
        R"({"choices":[{"message":{"role":"assistant","tool_calls":[{"id":"call-1","type":"function","function":{"name":"work","arguments":"{}"}}]},"finish_reason":"tool_calls"}],"usage":{"prompt_tokens":1}})"});
    fake->enqueueError(networkError());
    fake->enqueue(answer(failed ? 503 : 200));
    ToolRegistry registry;              // 应用工具集合
    FunctionToolDefinition definition;  // 最小工具声明
    definition.name = "work";
    int count = 0;   // 实际 Handler 次数
    SdkError error;  // 注册及运行输出
    QVERIFY(registry.registerTool(
        definition,
        [&count](const ToolCall&, const ToolExecutionContext&, ToolResult&,  // 当前调用、上下文或业务结果参数
                 SdkError&) {  // 同步处理本次业务或事件，不创建线程
            ++count;
            return true;
        },
        error));                            // 重试不重复执行业务工具
    Agent agent(std::move(sdk), registry);  // 完整 Agent 主链
    AgentRequest request;                   // 两轮 LLM、一轮工具执行
    request.chat = input();
    request.enabledTools.append("work");
    request.limits.maxTurns = 2;
    request.limits.maxToolCalls = 1;
    request.limits.llmRetryPolicy.maxRetries = 1;
    request.limits.llmRetryPolicy.retryIntervalMs = 0;
    request.requestOptions.retryPolicy.maxRetries = 0;
    AgentResult result;  // 重试不增加 turnUsages
    QCOMPARE(agent.run(request, result, error), !failed);
    QCOMPARE(result.finishReason,
             failed ? AgentFinishReason::Failed : AgentFinishReason::Completed);
    QCOMPARE(result.newMessages.size(), failed ? 2 : 3);
    QCOMPARE(fake->requests().size(), 4);
    QCOMPARE(result.turnUsages.size(), 2);
    QCOMPARE(count, 1);
    QCOMPARE(request.requestOptions.retryPolicy.maxRetries, 0);
}
QTEST_GUILESS_MAIN(RetryTest)
#include "tst_Retry.moc"
