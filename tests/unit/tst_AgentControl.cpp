#include <AiLib/agent/Agent.h>
#include <AiLib/protocol/openai/OpenAIChatCompatibleAdapter.h>
#include <AiLib/network/QtHttpTransport.h>
#include "../support/FakeTransport.h"
#include "../support/TestOptions.h"
#include "../support/LocalHttpServer.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QElapsedTimer>
#include <QtTest>
#include <atomic>
#include <future>
#include <thread>

using namespace AiLib;
namespace {
AgentRequest input()  // 创建允许一个工具的标准运行输入
{
    AgentRequest request;                          // 当前运行配置
    request.limits.llmRetryPolicy.maxRetries = 0;  // 本组验证单次请求和工具行为，不验证自动重试
    request.chat.model = "test-model";
    request.chat.messages.append(Message::user("执行任务"));
    request.enabledTools.append("work");
    return request;
}
TransportResponse reply(int count = 0, bool invalidArguments = false)  // 构造单候选工具批或最终回答
{
    QJsonArray calls;                              // 当前响应的完整调用集合
    for (int index = 0; index < count; ++index) {  // 生成当前响应内唯一 ID
        calls.append(QJsonObject{
            {"id", "call-" + QString::number(index)},
            {"type", "function"},
            {"function", QJsonObject{{"name", "work"},
                                     {"arguments", invalidArguments ? "{}" : "{\"id\":1}"}}}});
    }
    QJsonObject message{{"role", "assistant"}, {"content", "回答"}};  // 规范协议中的助手消息
    if (count)
        message.insert("tool_calls", calls);
    QJsonObject body{
        // 单候选响应
        {"choices", QJsonArray{QJsonObject{{"message", message},
                                           {"finish_reason", count ? "tool_calls" : "stop"}}}}};
    return TransportResponse{200, {}, QJsonDocument(body).toJson(QJsonDocument::Compact)};
}
FunctionToolDefinition tool()  // 声明用于次数和错误验证的整数参数工具
{
    FunctionToolDefinition definition;  // 工具纯描述
    definition.name = "work";
    definition.inputSchema =
        QJsonObject{{"type", "object"},
                    {"properties", QJsonObject{{"id", QJsonObject{{"type", "integer"}}}}},
                    {"required", QJsonArray{"id"}}};
    return definition;
}
std::unique_ptr<LLMClient> fakeClient(FakeTransport*& fake)  // 构造可观察请求的真实 Adapter 客户端
{
    auto transport = std::make_unique<FakeTransport>();  // 即将移交的测试传输
    fake = transport.get();
    ProviderConfig provider;  // 离线服务配置
    provider.baseUrl = QUrl("https://example.com/v1/");
    return std::make_unique<LLMClient>(provider, std::make_unique<OpenAIChatCompatibleAdapter>(),
                                       std::move(transport));
}
// 测试应用等待确认时的协作取消和总截止时间。
class WaitingApproval : public IToolApprovalProvider {
public:
    std::function<void()> started;         // 通知测试确认等待已进入
    bool requestApproval(const ToolCall&,  // 当前调用、停止上下文及业务结果参数
                         const FunctionToolDefinition&,
                         const ToolExecutionContext& context,  // 来源和停止上下文
                         ToolApprovalResult& result,           // 本次运行的增量和结束原因
                         SdkError&) override                   // 应用同步等待并检查共享停止上下文
    {
        if (started)
            started();
        while (!context.cancellation.isCancellationRequested() && !context.isTimedOut())
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        result.decision = ToolApprovalDecision::Cancel;
        return true;
    }
};
}
// 验证 Agent 限制、事件、精确网络预算和实例之间的并发独立性。
class AgentControlTest : public QObject {
    Q_OBJECT
private slots:
    void quota_data();                // 构造调用总额度和批量预检情形
    void quota();                     // 额度超限时本批全部不执行
    void differentToolsShareQuota();  // 所有工具共享一个实际执行总额度
    void deniedDoesNotCount();        // 确认拒绝不消耗 Handler 次数
    void rejectedDoesNotCount();      // 参数校验失败不消耗实际 Handler 额度
    void handlerFailureCounts();      // Handler false 和异常仍消耗调用次数
    void events();                    // 事件与增量消息顺序及身份关联
    void callbackFailure_data();      // 开始和结束回调异常
    void callbackFailure();           // 回调异常保留已产生消息并阻止后续副作用
    void approvalStop_data();         // 确认等待期间取消和总超时
    void approvalStop();              // 应用协作等待停止后不进入 Handler
    void instanceConcurrency();       // 同实例并发拒绝，不同实例共享 Registry 可运行
    void exactNetworkDeadline();      // 亚秒总预算由真实 Qt 网络栈中止
    void timeoutAfterTool();          // 不可中断的同步业务返回后正常结束 Timeout
    void streamStop_data();           // 流式文本产生后取消或总超时
    void streamStop();                // 停止保留有效文本且不执行模型工具
    void expiredDeadline();           // 到期请求不发送网络或消费 Fake 队列
};
void AgentControlTest::quota_data()  // 额度检查独立于模型轮数
{
    QTest::addColumn<int>("limit");
    QTest::addColumn<int>("batch");
    QTest::addColumn<bool>("secondBatch");
    QTest::newRow("zero") << 0 << 1 << false;
    QTest::newRow("whole-batch") << 1 << 2 << false;
    QTest::newRow("exact") << 2 << 2 << false;
    QTest::newRow("across-turns") << 1 << 1 << true;
}
void AgentControlTest::quota()  // 实际执行次数跨工具轮次累计
{
    FakeTransport* fake = nullptr;  // 请求观察指针
    ToolRegistry registry;          // 应用工具集合
    int count = 0;                  // 实际 Handler 次数
    SdkError error;                 // 注册与运行输出
    QVERIFY(registry.registerTool(
        tool(),
        [&count](const ToolCall&, const ToolExecutionContext&, ToolResult& result,  // 当前调用、上下文或业务结果参数
                 SdkError&) {  // 同步处理本次业务或事件，不创建线程
            ++count;
            result.data = 42;
            return true;
        },
        error));                              // 记录实际业务执行
    Agent agent(fakeClient(fake), registry);  // 最小 Agent
    QFETCH(int, limit);                       // 本次执行总上限
    QFETCH(int, batch);                       // 首轮调用批大小
    QFETCH(bool, secondBatch);                // 是否继续请求第二批工具
    fake->enqueue(reply(batch));
    fake->enqueue(reply(secondBatch ? 1 : 0));
    auto request = input();  // 限额运行配置
    request.limits.maxToolCalls = limit;
    int events = 0;                                                 // 超额批不发送执行事件
    request.callback = [&events](const AgentEvent&) { ++events; };  // 记录客户端执行流程事件
    AgentResult result;                                             // 增量和停止原因
    QVERIFY(agent.run(request, result, error));
    const bool completes = limit >= batch && !secondBatch;  // 只有额度满足且没有后续超额批才最终完成
    QCOMPARE(result.finishReason,
             completes ? AgentFinishReason::Completed : AgentFinishReason::MaxToolCalls);
    QCOMPARE(count, limit >= batch ? batch : 0);
    QCOMPARE(events, 2 * count);
    QCOMPARE(error.category, ErrorCategory::None);
}
void AgentControlTest::differentToolsShareQuota()  // 额度不是每个工具独立统计
{
    FakeTransport* fake = nullptr;  // 请求观察指针
    ToolRegistry registry;          // 应用工具集合
    SdkError error;                 // 注册及运行输出
    int count = 0;                  // 两个工具的实际执行总次数
    ToolHandler handler = [&count](const ToolCall&,  // 当前调用、上下文或业务结果参数
                                   const ToolExecutionContext&,  // 本次调用、上下文或业务结果参数
                                   ToolResult&,                  // 当前调用、停止上下文及业务结果参数
                                   SdkError&) {                  // 同步处理本次业务或事件，不创建线程
        ++count;
        return true;
    };  // 共用执行计数
    QVERIFY(registry.registerTool(tool(), handler, error));
    auto other = tool();  // 第二个工具纯描述
    other.name = "other";
    QVERIFY(registry.registerTool(other, handler, error));
    Agent agent(fakeClient(fake), registry);  // 单次运行管理两个工具
    fake->enqueue(reply(1));
    auto second = reply(1);  // 第二轮请求不同工具
    second.body.replace("\"name\":\"work\"", "\"name\":\"other\"");
    fake->enqueue(second);
    fake->enqueue(reply(1));
    auto request = input();  // 两次实际执行的总额度
    request.enabledTools.append("other");
    request.limits.maxToolCalls = 2;
    AgentResult result;  // 第三轮工具批不执行
    QVERIFY(agent.run(request, result, error));
    QCOMPARE(result.finishReason, AgentFinishReason::MaxToolCalls);
    QCOMPARE(count, 2);
    QCOMPARE(std::get<ToolResultContent>(result.newMessages.at(3).contents.first()).result.toolName,
             QStringLiteral("other"));
}
void AgentControlTest::deniedDoesNotCount()  // 拒绝后仍有一次实际 Handler 执行额度
{
    // 应用策略先拒绝后允许，不依赖 SDK 线程或 GUI。
    class Approval : public IToolApprovalProvider {
    public:
        int count = 0;                         // 确认请求总次数
        bool requestApproval(const ToolCall&,  // 当前调用、停止上下文及业务结果参数
                             const FunctionToolDefinition&,
                             const ToolExecutionContext&,
                             ToolApprovalResult& result,  // 本次运行的增量和结束原因
                             SdkError&) override          // 输出先拒绝后允许的用户决定
        {
            result.decision =
                ++count == 1 ? ToolApprovalDecision::Deny : ToolApprovalDecision::Allow;
            result.reason = "测试决定";
            return true;
        }
    };
    Approval approval;              // 应用确认策略
    FakeTransport* fake = nullptr;  // 请求观察指针
    ToolRegistry registry;          // 工具集合
    auto definition = tool();       // 每次要求确认的工具
    definition.approvalPolicy = ToolApprovalPolicy::Always;
    SdkError error;  // 注册及运行输出
    int count = 0;   // 实际 Handler 次数
    QVERIFY(registry.registerTool(
        definition,
        [&count](const ToolCall&, const ToolExecutionContext&, ToolResult&,  // 当前调用、上下文或业务结果参数
                 SdkError&) {  // 同步处理本次业务或事件，不创建线程
            ++count;
            return true;
        },
        error));                                         // 确认通过后才执行
    Agent agent(fakeClient(fake), registry, &approval);  // 使用应用策略
    fake->enqueue(reply(1));
    fake->enqueue(reply(1));
    fake->enqueue(reply());
    auto request = input();  // 仅一次实际执行额度
    request.limits.maxToolCalls = 1;
    AgentResult result;  // 应正常完成
    QVERIFY(agent.run(request, result, error));
    QCOMPARE(result.finishReason, AgentFinishReason::Completed);
    QCOMPARE(count, 1);
    QCOMPARE(approval.count, 2);
}
void AgentControlTest::rejectedDoesNotCount()  // 非业务执行的失败不减少剩余额度
{
    FakeTransport* fake = nullptr;  // 请求观察指针
    ToolRegistry registry;          // 工具集合
    SdkError error;                 // 注册及运行错误
    int count = 0;                  // Handler 实际次数
    QVERIFY(registry.registerTool(
        tool(),
        [&count](const ToolCall&, const ToolExecutionContext&, ToolResult&,  // 当前调用、上下文或业务结果参数
                 SdkError&) {  // 同步处理本次业务或事件，不创建线程
            ++count;
            return true;
        },
        error));                              // 记录实际执行
    Agent agent(fakeClient(fake), registry);  // 同步 Agent
    fake->enqueue(reply(1, true));
    fake->enqueue(reply(1));
    fake->enqueue(reply());
    auto request = input();  // 一次实际 Handler 执行额度
    request.limits.maxToolCalls = 1;
    QList<AgentEvent> events;                                // 所有工具流程事件
    request.callback = [&events](const AgentEvent& event) {  // 同步处理本次业务或事件，不创建线程
        events.append(event);
    };                   // 保存同步事件副本
    AgentResult result;  // 应完成两轮工具流程
    QVERIFY(agent.run(request, result, error));
    QCOMPARE(result.finishReason, AgentFinishReason::Completed);
    QCOMPARE(count, 1);
    QCOMPARE(events.size(), 4);
    QVERIFY(!events.at(1).handlerExecuted);
    QVERIFY(events.at(3).handlerExecuted);
}
void AgentControlTest::handlerFailureCounts()  // 两次 Handler 故障仍用尽两次额度
{
    FakeTransport* fake = nullptr;  // 请求观察指针
    ToolRegistry registry;          // 工具集合
    int count = 0;                  // Handler 次数
    SdkError error;                 // 注册及运行输出
    QVERIFY(registry.registerTool(
        tool(),
        [&count](const ToolCall&, const ToolExecutionContext&,  // 本次调用、上下文或业务结果参数
                 ToolResult&,                                   // 当前调用、停止上下文及业务结果参数
                 SdkError&) {                                   // 首次 false、第二次异常
            if (++count == 2)
                throw 1;
            return false;
        },
        error));
    Agent agent(fakeClient(fake), registry);  // 同步 Agent
    fake->enqueue(reply(1));
    fake->enqueue(reply(1));
    fake->enqueue(reply(1));
    auto request = input();  // 两次实际执行额度
    request.limits.maxToolCalls = 2;
    AgentResult result;  // 保留失败工具结果
    QVERIFY(agent.run(request, result, error));
    QCOMPARE(count, 2);
    QCOMPARE(result.finishReason, AgentFinishReason::MaxToolCalls);
    QCOMPARE(result.newMessages.size(), 5);
}
void AgentControlTest::events()  // 开始事件先于业务执行，结束事件携带完整结果
{
    FakeTransport* fake = nullptr;  // 请求观察指针
    ToolRegistry registry;          // 工具集合
    int count = 0;                  // Handler 次数
    SdkError error;                 // 注册及运行输出
    QVERIFY(registry.registerTool(
        tool(),
        [&count](const ToolCall&,  // 当前调用、上下文或业务结果参数
                 const ToolExecutionContext& context,  // 本次调用、上下文或业务结果参数
                 ToolResult& result,                   // 当前调用、停止上下文及业务结果参数
                 SdkError&) {                          // 检查身份沿 Context 传播
            ++count;
            result.data = context.agentId;
            return true;
        },
        error));
    Agent agent(fakeClient(fake), registry, nullptr, "agent-1");  // 稳定来源身份
    fake->enqueue(reply(1));
    fake->enqueue(reply());
    auto request = input();    // 当前运行
    QList<AgentEvent> events;  // 事件副本
    bool orderValid = true;    // 事件与 Handler 的相对顺序
    request.callback = [&events, &count,
                        &orderValid](const AgentEvent& event) {  // 实时通知不改变调度职责
        if (event.type == AgentEventType::ToolExecutionStarted && count != 0)
            orderValid = false;
        if (event.type == AgentEventType::ToolExecutionFinished && count != 1)
            orderValid = false;
        events.append(event);
    };
    AgentResult result;  // 增量及最终回答
    QVERIFY(agent.run(request, result, error));
    QVERIFY(orderValid);
    QCOMPARE(events.size(), 2);
    QCOMPARE(events.first().agentId, agent.id());
    QCOMPARE(events.last().call.id, QStringLiteral("call-0"));
    QVERIFY(!events.first().result);
    QVERIFY(events.last().result.has_value());
    QCOMPARE(events.last().result->data.toString(), agent.id());
}
void AgentControlTest::callbackFailure_data()  // 两个事件回调边界
{
    QTest::addColumn<bool>("finished");
    QTest::newRow("before-handler") << false;
    QTest::newRow("after-handler") << true;
}
void AgentControlTest::callbackFailure()  // 已产生消息先保存，接收器异常不能抹掉结果
{
    QFETCH(bool, finished);         // 在开始或结束回调抛出异常
    FakeTransport* fake = nullptr;  // 请求观察指针
    ToolRegistry registry;          // 工具集合
    SdkError error;                 // 注册与运行输出
    int count = 0;                  // Handler 次数
    QVERIFY(registry.registerTool(
        tool(),
        [&count](const ToolCall&, const ToolExecutionContext&, ToolResult&,  // 当前调用、上下文或业务结果参数
                 SdkError&) {  // 同步处理本次业务或事件，不创建线程
            ++count;
            return true;
        },
        error));                              // 记录实际执行
    Agent agent(fakeClient(fake), registry);  // 同步 Agent
    fake->enqueue(reply(1));
    auto request = input();                                   // 当前运行配置
    request.callback = [finished](const AgentEvent& event) {  // 模拟应用接收器异常
        if ((event.type == AgentEventType::ToolExecutionFinished) == finished)
            throw 1;
    };
    AgentResult result;  // 保留已产生消息
    QVERIFY(!agent.run(request, result, error));
    QCOMPARE(error.code, QStringLiteral("AgentCallbackFailed"));
    QCOMPARE(count, finished ? 1 : 0);
    QCOMPARE(result.newMessages.size(), finished ? 2 : 1);
}
void AgentControlTest::approvalStop_data()  // 协作等待的两个正常停止原因
{
    QTest::addColumn<bool>("cancelled");
    QTest::newRow("cancel") << true;
    QTest::newRow("timeout") << false;
}
void AgentControlTest::approvalStop()  // SDK 不创建 UI 线程，由应用策略同步等待
{
    QFETCH(bool, cancelled);        // 主动取消或总超时
    FakeTransport* fake = nullptr;  // 请求观察指针
    ToolRegistry registry;          // 工具集合
    auto definition = tool();       // 要求确认的声明
    definition.approvalPolicy = ToolApprovalPolicy::Always;
    int count = 0;   // Handler 次数
    SdkError error;  // 注册及运行输出
    QVERIFY(registry.registerTool(
        definition,
        [&count](const ToolCall&, const ToolExecutionContext&, ToolResult&,  // 当前调用、上下文或业务结果参数
                 SdkError&) {  // 同步处理本次业务或事件，不创建线程
            ++count;
            return true;
        },
        error));                // 不应被调用的业务函数
    WaitingApproval approval;   // 应用协作等待策略
    CancellationSource source;  // 唯一取消来源
    approval.started = [&source, cancelled] {
        if (cancelled)
            source.cancel();
    };                                                   // 等待开始后请求主动取消
    Agent agent(fakeClient(fake), registry, &approval);  // 借用应用确认策略
    fake->enqueue(reply(1));
    auto request = input();  // 统一取消和总截止时间配置
    request.requestOptions.cancellation = source.token();
    request.limits.totalTimeoutSeconds = 1;
    AgentResult result;  // 正常结束状态
    QVERIFY(agent.run(request, result, error));
    QCOMPARE(result.finishReason,
             cancelled ? AgentFinishReason::Cancelled : AgentFinishReason::Timeout);
    QCOMPARE(count, 0);
    QCOMPARE(result.newMessages.size(), 1);
    QCOMPARE(error.category, ErrorCategory::None);
}
void AgentControlTest::instanceConcurrency()  // 不同 Agent 共享 Registry，同实例同时只允许一个运行
{
    ToolRegistry registry;     // 多 Agent 共享工具集合
    auto definition = tool();  // 允许应用并行业务执行
    definition.concurrency = ToolConcurrency::Concurrent;
    std::promise<void> entered;                // 首个 Handler 已进入
    std::promise<void> release;                // 应用解除首个 Handler 等待
    auto gate = release.get_future().share();  // 首个 Agent 的等待门
    SdkError error;                            // 注册输出
    QVERIFY(registry.registerTool(
        definition,
        [&entered, gate](const ToolCall&,  // 当前调用、上下文或业务结果参数
                         const ToolExecutionContext& context,  // 本次调用、上下文或业务结果参数
                         ToolResult&,                          // 当前调用、停止上下文及业务结果参数
                         SdkError&) {                          // 仅阻塞首个 Agent
            if (context.agentId == "first") {
                entered.set_value();
                return gate.wait_for(std::chrono::seconds(3)) == std::future_status::ready;
            }
            return true;
        },
        error));
    FakeTransport* firstFake = nullptr;                                 // 首个请求队列观察指针
    FakeTransport* secondFake = nullptr;                                // 第二个请求队列观察指针
    Agent first(fakeClient(firstFake), registry, nullptr, "first");     // 首个独立实例
    Agent second(fakeClient(secondFake), registry, nullptr, "second");  // 第二个独立实例
    firstFake->enqueue(reply(1));
    firstFake->enqueue(reply());
    secondFake->enqueue(reply(1));
    secondFake->enqueue(reply());
    auto running = std::async(std::launch::async, [&first] {  // 应用启动工作线程
        AgentResult result;                                   // 首个运行结果
        SdkError error;                                       // 首个运行输出
        return first.run(input(), result, error);
    });
    entered.get_future().wait();
    AgentResult busy;                                          // 同实例并发调用结果
    const bool admitted = first.run(input(), busy, error);     // 应拒绝并发 run
    const QString code = error.code;                           // 保留 busy 错误码
    AgentResult other;                                         // 第二个 Agent 的独立结果
    const bool completed = second.run(input(), other, error);  // 在首个实例等待期间完成
    release.set_value();
    QVERIFY(running.get());
    QVERIFY(!admitted);
    QCOMPARE(code, QStringLiteral("AgentBusy"));
    QVERIFY(completed);
    QCOMPARE(other.finishReason, AgentFinishReason::Completed);
}
void AgentControlTest::exactNetworkDeadline()  // 先执行耗时工具，再以剩余亚秒预算等待真实 HTTP
{
    int requests = 0;  // 本地服务收到的逻辑请求数
    LocalHttpServer server(
        [&requests](QTcpSocket* socket, const QByteArray&) {  // 第二轮保持连接，验证总预算中止
            if (++requests == 1) {
                socket->write(LocalHttpServer::httpResponse(200, reply(1).body));
                socket->disconnectFromHost();
            }
        });
    QVERIFY(server.listen());
    ProviderConfig provider;  // 真实 Qt 本地网络配置
    provider.baseUrl = server.url("/v1/");
    auto client = std::make_unique<LLMClient>(  // 真实 Adapter 和 Qt 网络栈
        provider, std::make_unique<OpenAIChatCompatibleAdapter>(),
        std::make_unique<QtHttpTransport>());
    ToolRegistry registry;  // 工具集合
    SdkError error;         // 注册及运行输出
    QVERIFY(registry.registerTool(
        tool(),
        [](const ToolCall&, const ToolExecutionContext&,  // 本次调用、上下文或业务结果参数
           ToolResult&,                                   // 当前调用、停止上下文及业务结果参数
           SdkError&) {                                   // 同步工具消耗部分总预算
            std::this_thread::sleep_for(std::chrono::milliseconds(450));
            return true;
        },
        error));
    Agent agent(std::move(client), registry);  // 完整的本地 Agent 网络闭环
    auto request = input();                    // 总预算短于第二轮默认请求超时
    request.limits.totalTimeoutSeconds = 1;
    request.limits.llmTimeoutSeconds = -1;
    QElapsedTimer timer;  // 验证没有额外整秒取整等待
    timer.start();
    AgentResult result;  // 保留已产生的工具消息
    QVERIFY(agent.run(request, result, error));
    QCOMPARE(result.finishReason, AgentFinishReason::Timeout);
    QVERIFY(timer.elapsed() >= 900);
    QVERIFY(timer.elapsed() < 1400);
    QCOMPARE(requests, 2);
    QCOMPARE(result.newMessages.size(), 2);
    QCOMPARE(error.category, ErrorCategory::None);
}
void AgentControlTest::timeoutAfterTool()  // 无法强行中断的同步工具完成后保留业务结果
{
    FakeTransport* fake = nullptr;  // 请求观察指针
    ToolRegistry registry;          // 工具集合
    SdkError error;                 // 注册及运行输出
    QVERIFY(registry.registerTool(
        tool(),
        [](const ToolCall&, const ToolExecutionContext&,  // 本次调用、上下文或业务结果参数
           ToolResult& result,                            // 当前调用、停止上下文及业务结果参数
           SdkError&) {                                   // 不可中断的同步业务示例
            std::this_thread::sleep_for(std::chrono::milliseconds(1050));
            result.data = 42;
            return true;
        },
        error));
    Agent agent(fakeClient(fake), registry);  // 同步 Agent
    fake->enqueue(reply(1));
    auto request = input();  // 一秒总预算
    request.limits.totalTimeoutSeconds = 1;
    AgentResult result;  // 实际消息保留
    QVERIFY(agent.run(request, result, error));
    QCOMPARE(result.finishReason, AgentFinishReason::Timeout);
    QCOMPARE(result.newMessages.size(), 2);
    QCOMPARE(
        std::get<ToolResultContent>(result.newMessages.last().contents.first()).result.data.toInt(),
        42);
}
void AgentControlTest::streamStop_data()  // 共享 Token 和总截止时间的停止语义
{
    QTest::addColumn<bool>("cancelled");
    QTest::newRow("cancelled") << true;
    QTest::newRow("timeout") << false;
}
void AgentControlTest::streamStop()  // 同步回调耗时仍受总预算约束，不强行终止线程
{
    QFETCH(bool, cancelled);        // 本次主动取消或总超时
    FakeTransport* fake = nullptr;  // 请求观察指针
    ToolRegistry registry;          // 工具集合
    SdkError error;                 // 注册及运行输出
    int count = 0;                  // 实际 Handler 次数
    QVERIFY(registry.registerTool(
        tool(),
        [&count](const ToolCall&, const ToolExecutionContext&, ToolResult&,  // 当前调用、上下文或业务结果参数
                 SdkError&) {  // 同步处理本次业务或事件，不创建线程
            ++count;
            return true;
        },
        error));                              // 流未完整时不应执行
    Agent agent(fakeClient(fake), registry);  // Streaming Agent
    fake->enqueueStream(
        {"data: {\"choices\":[{\"index\":0,\"delta\":{\"role\":\"assistant\",\"content\":\"部分文本\"},\"finish_reason\":null}]}\n\n"});
    auto request = input();  // 一秒总预算和唯一取消来源
    request.chat.stream = true;
    request.limits.totalTimeoutSeconds = 1;
    CancellationSource source;  // 应用取消来源
    request.requestOptions.cancellation = source.token();
    request.requestOptions.streamCallback =
        [&source, cancelled](const StreamEvent& event) {  // 模拟 UI 展示后取消或耗时处理
            if (event.type == StreamEventType::TextDelta) {
                if (cancelled)
                    source.cancel();
                else
                    std::this_thread::sleep_for(std::chrono::milliseconds(1050));
            }
        };
    AgentResult result;  // 有效部分文本和正常停止原因
    QVERIFY(agent.run(request, result, error));
    QCOMPARE(result.finishReason,
             cancelled ? AgentFinishReason::Cancelled : AgentFinishReason::Timeout);
    QCOMPARE(result.newMessages.size(), 1);
    QCOMPARE(result.newMessages.first().text(), QStringLiteral("部分文本"));
    QCOMPARE(result.newMessages.first().status, MessageStatus::Incomplete);
    QCOMPARE(count, 0);
    QVERIFY(!result.finalMessage);
    QCOMPARE(error.category, ErrorCategory::None);
}
void AgentControlTest::expiredDeadline()  // Client 在请求开始前就响应精确截止时间
{
    FakeTransport* fake = nullptr;   // 请求观察指针
    auto client = fakeClient(fake);  // 普通 Client 可使用相同预算机制
    RequestOptions options;          // 已到期的请求上下文
    options.deadline = std::chrono::steady_clock::now();
    ChatResponse response;  // 不产生有效输出
    SdkError error;         // 请求超时输出
    QVERIFY(!client->chat(input().chat, response, error, options));
    QCOMPARE(error.category, ErrorCategory::Timeout);
    QVERIFY(fake->requests().isEmpty());
}
QTEST_GUILESS_MAIN(AgentControlTest)
#include "tst_AgentControl.moc"
