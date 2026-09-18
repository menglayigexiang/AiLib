#include <AiLib/agent/Agent.h>
#include <AiLib/protocol/openai/OpenAIChatCompatibleAdapter.h>
#include <AiLib/protocol/anthropic/AnthropicMessagesAdapter.h>
#include <AiLib/tools/ToolErrorCodes.h>
#include "../support/FakeTransport.h"
#include "../support/TestOptions.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QtTest>
#include <stdexcept>

using namespace AiLib;
namespace {
std::unique_ptr<LLMClient>
client(FakeTransport*& fake)  // 创建真实 Adapter 加离线 Transport 的 Client，并输出观察指针
{
    ProviderConfig provider;  // 无认证的离线服务配置
    provider.baseUrl = QUrl("https://example.com/v1/");
    auto transport = std::make_unique<FakeTransport>();  // 转移给 Client 的预设传输
    fake = transport.get();
    return std::make_unique<LLMClient>(provider, std::make_unique<OpenAIChatCompatibleAdapter>(),
                                       std::move(transport));
}
AgentRequest request()  // 创建最小用户请求
{
    AgentRequest value;                          // 应用传入的完整运行配置
    value.limits.llmRetryPolicy.maxRetries = 0;  // 本组验证单次请求和工具行为，不验证自动重试
    value.chat.model = "test-model";
    value.chat.messages.append(Message::system("系统指令"));
    value.chat.messages.append(Message::user("用户问题"));
    return value;
}
FunctionToolDefinition definition(const QString& name = "lookup")  // 创建对象参数的本地函数工具声明
{
    FunctionToolDefinition value;  // 不包含 Handler 的纯描述
    value.name = name;
    value.inputSchema =
        QJsonObject{{"type", "object"},
                    {"properties", QJsonObject{{"id", QJsonObject{{"type", "integer"}}}}},
                    {"required", QJsonArray{"id"}}};
    return value;
}
QJsonObject nativeCall(const QString& id = "call-1",             // 当前响应内的调用 ID
                       const QString& name = "lookup",           // 待调用的工具名称
                       const QString& arguments = "{\"id\":1}")  // 生成 Provider 侧函数调用测试数据
{
    return QJsonObject{{"id", id},
                       {"type", "function"},
                       {"function", QJsonObject{{"name", name}, {"arguments", arguments}}}};
}
TransportResponse response(const QJsonArray& calls = {},    // 原生工具调用集合
                           const QString& finish = "stop",  // 模型停止原因
                           const QJsonObject& usage = {})   // 构造单候选普通响应和显式用量
{
    QJsonObject message{  // 单个助手消息的原生数据
                        {"role", "assistant"},
                        {"content", calls.isEmpty() ? "最终回答" : "执行工具"}};
    if (!calls.isEmpty())
        message.insert("tool_calls", calls);
    const QJsonObject root{
        // 原生单候选响应

        {"id", "response-1"},
        {"choices", QJsonArray{QJsonObject{{"message", message}, {"finish_reason", finish}}}},
        {"usage", usage}};
    return TransportResponse{200, {}, QJsonDocument(root).toJson(QJsonDocument::Compact)};
}
ToolHandler handler(int& count)  // 返回记录执行次数的同步业务函数
{
    return
        [&count](const ToolCall&, const ToolExecutionContext&,  // 当前调用、停止上下文及业务结果参数
                 ToolResult& result,                            // 当前调用、停止上下文及业务结果输出
                 SdkError&) {                                   // 输出不额外包装的业务结果
            ++count;
            result.data = 42;
            return true;
        };
}
// 模拟应用同步确认策略，记录来源并返回预设决定。
class Approval : public IToolApprovalProvider {
public:
    ToolApprovalDecision decision = ToolApprovalDecision::Deny;  // 本次用户决定
    QString agentId;                                             // 确认来源身份
    QString callId;                                              // 工具调用关联
    bool requestApproval(const ToolCall& call,                   // 待确认调用
                         const FunctionToolDefinition&,          // 工具描述，本测试无需读取
                         const ToolExecutionContext& context,    // Agent 身份及停止上下文
                         ToolApprovalResult& result,             // 决定输出
                         SdkError&) override                     // 同步记录来源并返回用户决定
    {
        agentId = context.agentId;
        callId = call.id;
        result.decision = decision;
        result.reason = "用户决定";
        return true;
    }
};
}
// 验证使用真实协议转换的最小 Agent 主链和必要执行边界。
class AgentTest : public QObject {
    Q_OBJECT
private slots:
    void anthropic();            // 验证相同 Agent 主链适配 Anthropic Messages
    void plain();                // 无工具普通回答和稳定身份
    void tools_data();           // 单工具、多工具和多轮调用
    void tools();                // 工具闭环与历史顺序
    void failure_data();         // 工具业务失败、异常、校验和白名单边界
    void failure();              // 工具失败返回模型继续决策
    void approval_data();        // 用户拒绝、取消及策略缺失
    void approval();             // 确认决定与 Agent 正常停止
    void configuration_data();   // 启动前配置错误
    void configuration();        // 配置非法时不发送请求
    void limits();               // 轮数及 Length 均不执行当前批工具
    void providerFailure();      // 第二轮故障保留已产生的消息
    void handlerCancellation();  // Handler 完成时取消仍保留实际工具结果
    void underlyingTimeout();    // 底层请求超时属于异常失败
    void cancellation();         // 主动取消不升级为运行失败
    void reentry();              // 同实例重入拒绝并释放运行标记
    void streaming();            // 完整 Streaming 后才执行工具
    void partialStreaming();     // 异常流保留有效文本并禁止工具执行
    void usage();                // 完整已知值和未知值分别累计
};
void AgentTest::anthropic()  // Agent 不依赖工具调用的原生厂商格式
{
    auto transport = std::make_unique<FakeTransport>();  // 即将交给 Client 的离线传输
    auto* fake = transport.get();                        // Client 生命周期内有效的观察指针
    ProviderConfig provider;                             // Anthropic 协议的服务配置
    provider.baseUrl = QUrl("https://example.com/");
    provider.protocol = ProtocolType::AnthropicMessages;
    auto llm = std::make_unique<LLMClient>(
        provider, std::make_unique<AnthropicMessagesAdapter>(),  // 独占协议和传输组件
        std::move(transport));
    ToolRegistry registry;  // 应用工具集合
    SdkError error;         // 注册及运行输出
    int count = 0;          // 实际 Handler 次数
    QVERIFY(registry.registerTool(definition(), handler(count), error));
    Agent agent(std::move(llm), registry);  // 相同的协议无关 Agent
    fake->enqueue(TransportResponse{
        200,
        {},
        R"({"id":"msg-1","type":"message","role":"assistant","content":[{"type":"tool_use","id":"call-1","name":"lookup","input":{"id":1}}],"stop_reason":"tool_use","usage":{"input_tokens":2,"output_tokens":3}})"});
    fake->enqueue(TransportResponse{
        200,
        {},
        R"({"id":"msg-2","type":"message","role":"assistant","content":[{"type":"text","text":"最终回答"}],"stop_reason":"end_turn","usage":{"input_tokens":7,"output_tokens":4}})"});
    auto input = request();  // 工具来自白名单和 Registry
    input.chat.maxOutputTokens = 64;
    input.enabledTools.append("lookup");
    AgentResult result;  // 协议规范化后的运行结果
    QVERIFY(agent.run(input, result, error));
    QCOMPARE(result.finishReason, AgentFinishReason::Completed);
    QCOMPARE(count, 1);
    QCOMPARE(result.newMessages.size(), 3);
    const auto body =  // 第二轮真实协议编码
        QJsonDocument::fromJson(fake->requests().last().body).object();
    const auto content = body.value("messages")  // 原生 tool_result 内容
                             .toArray()
                             .last()
                             .toObject()
                             .value("content")
                             .toArray()
                             .first()
                             .toObject();
    QCOMPARE(content.value("type").toString(), QStringLiteral("tool_result"));
    QCOMPARE(content.value("tool_use_id").toString(), QStringLiteral("call-1"));
}
void AgentTest::plain()  // Agent 不维护长期历史，空白名单覆盖外部工具定义
{
    FakeTransport* fake = nullptr;        // Client 存活期间有效的观察指针
    ToolRegistry registry;                // 应用拥有的工具集合
    Agent agent(client(fake), registry);  // 自动生成稳定身份的 Agent
    fake->enqueue(response());
    auto input = request();  // 原始输入，应保持不变
    input.chat.tools.append(definition("ignored"));
    AgentResult result;  // 运行结果
    SdkError error;      // 异常错误输出
    QVERIFY(!agent.id().isEmpty());
    QVERIFY(agent.run(input, result, error));
    QCOMPARE(result.finishReason, AgentFinishReason::Completed);
    QVERIFY(result.finalMessage.has_value());
    QCOMPARE(result.finalMessage->text(), QStringLiteral("最终回答"));
    QCOMPARE(result.newMessages.size(), 1);
    QCOMPARE(input.chat.messages.size(), 2);
    const auto body =  // 实际发给模型的参数
        QJsonDocument::fromJson(fake->requests().first().body).object();
    QVERIFY(!body.contains("tools"));
    QCOMPARE(body.value("messages").toArray().size(), 2);
}
void AgentTest::tools_data()  // 主链中的调用数量与轮数
{
    QTest::addColumn<int>("callsPerTurn");
    QTest::addColumn<int>("toolTurns");
    QTest::newRow("single") << 1 << 1;
    QTest::newRow("multiple") << 2 << 1;
    QTest::newRow("multi-turn") << 2 << 2;
}
void AgentTest::tools()  // Assistant 与 Tool 消息完整反馈至下一轮模型
{
    QFETCH(int, callsPerTurn);      // 每轮工具调用数
    QFETCH(int, toolTurns);         // 包含工具调用的轮数
    FakeTransport* fake = nullptr;  // 请求观察指针
    ToolRegistry registry;          // 工具集合
    SdkError error;                 // 注册和运行错误
    int count = 0;                  // 实际 Handler 次数
    QVERIFY(registry.registerTool(definition(), handler(count), error));
    QVERIFY(registry.registerTool(definition("other"), handler(count), error));
    Agent agent(client(fake), registry, nullptr, "agent-A");  // 指定应用身份
    for (int turn = 0; turn < toolTurns; ++turn) {            // 模拟多轮调用
        QJsonArray calls;                                     // 当前响应的完整调用批
        for (int index = 0; index < callsPerTurn; ++index) {  // 当前响应范围内唯一的调用 ID
            calls.append(
                nativeCall("call-" + QString::number(index), index == 0 ? "lookup" : "other"));
        }
        fake->enqueue(response(calls, "tool_calls"));
    }
    fake->enqueue(response());
    auto input = request();  // 白名单来源只有 Registry
    input.enabledTools = QStringList{"lookup", "other", "lookup"};
    AgentResult result;  // 增量消息输出
    QVERIFY(agent.run(input, result, error));
    QCOMPARE(result.finishReason, AgentFinishReason::Completed);
    QCOMPARE(count, callsPerTurn * toolTurns);
    QCOMPARE(result.newMessages.size(), toolTurns * (callsPerTurn + 1) + 1);
    QCOMPARE(fake->requests().size(), toolTurns + 1);
    const auto body =
        QJsonDocument::fromJson(fake->requests().last().body)  // 最后一轮完整历史的协议编码
            .object();
    const auto messages = body.value("messages").toArray();  // 原始历史及本轮新产生的调用和结果
    QCOMPARE(messages.size(), 2 + toolTurns * (callsPerTurn + 1));
    QCOMPARE(body.value("tools").toArray().size(), 2);
    QCOMPARE(messages.at(0).toObject().value("content").toString(), QStringLiteral("系统指令"));
    QCOMPARE(messages.at(2).toObject().value("role").toString(), QStringLiteral("assistant"));
    QCOMPARE(messages.at(3).toObject().value("tool_call_id").toString(), QStringLiteral("call-0"));
    QCOMPARE(messages.at(3).toObject().value("content").toString(), QStringLiteral("42"));
    QCOMPARE(input.chat.messages.size(), 2);
}
void AgentTest::failure_data()  // 不同 ToolResult failure 应继续调用模型
{
    QTest::addColumn<int>("mode");
    QTest::newRow("handler-error") << 0;
    QTest::newRow("exception") << 1;
    QTest::newRow("invalid-arguments") << 2;
    QTest::newRow("not-enabled") << 3;
}
void AgentTest::failure()  // 模型实际收到稳定失败语义
{
    QFETCH(int, mode);              // 当前业务失败情形
    FakeTransport* fake = nullptr;  // 观察模型请求
    ToolRegistry registry;          // 工具集合
    SdkError error;                 // 注册和运行错误
    int count = 0;                  // Handler 次数
    QVERIFY(registry.registerTool(
        definition(),
        [&count, mode](const ToolCall&,              // 本次调用、上下文或业务结果参数
                       const ToolExecutionContext&,  // 当前调用、停止上下文及业务结果参数
                       ToolResult&,                  // 当前调用、停止上下文及业务结果输出
                       SdkError& failure) {          // 模拟业务失败或异常
            ++count;
            if (mode == 1)
                throw std::runtime_error("设备异常");
            failure.code = "DeviceOffline";
            failure.message = "设备离线";
            return false;
        },
        error));
    Agent agent(client(fake), registry);  // 同步 Agent
    fake->enqueue(response(
        QJsonArray{nativeCall("call-1", "lookup", mode == 2 ? "{}" : "{\"id\":1}")}, "tool_calls"));
    fake->enqueue(response());
    auto input = request();  // 当前运行允许的工具
    if (mode != 3)
        input.enabledTools.append("lookup");
    AgentResult result;  // 模型继续后得到的最终结果
    QVERIFY(agent.run(input, result, error));
    QCOMPARE(result.finishReason, AgentFinishReason::Completed);
    QCOMPARE(error.category, ErrorCategory::None);
    QCOMPARE(count, mode < 2 ? 1 : 0);
    const auto& toolResult =
        std::get<ToolResultContent>(result.newMessages.at(1).contents.first())  // 工具失败消息
            .result;
    QVERIFY(!toolResult.success);
    const QString expected = mode == 0   ? "DeviceOffline"  // 当前失败的稳定标识
                             : mode == 1 ? "ExecutionFailed"
                             : mode == 2 ? "InvalidArguments"
                                         : "ToolNotEnabled";
    QCOMPARE(toolResult.errorCode, expected);
    const auto body =  // 第二轮 Provider 请求
        QJsonDocument::fromJson(fake->requests().last().body).object();
    const auto wire =  // 失败结果的 JSON fallback
        QJsonDocument::fromJson(
            body.value("messages").toArray().last().toObject().value("content").toString().toUtf8())
            .object();
    QCOMPARE(wire.value("success").toBool(), false);
    QCOMPARE(wire.value("errorCode").toString(), expected);
}
void AgentTest::approval_data()  // 拒绝继续、取消停止、缺失策略继续
{
    QTest::addColumn<int>("mode");
    QTest::newRow("deny") << 0;
    QTest::newRow("cancel") << 1;
    QTest::newRow("missing-provider") << 2;
}
void AgentTest::approval()  // 身份来源仅由执行上下文传递
{
    QFETCH(int, mode);              // 当前确认情形
    FakeTransport* fake = nullptr;  // 请求观察指针
    ToolRegistry registry;          // 工具集合
    auto tool = definition();       // 每次确认的工具
    tool.approvalPolicy = ToolApprovalPolicy::Always;
    int count = 0;   // Handler 执行次数
    SdkError error;  // 流程输出
    QVERIFY(registry.registerTool(tool, handler(count), error));
    Approval provider;  // 应用拥有的策略
    if (mode == 1)
        provider.decision = ToolApprovalDecision::Cancel;
    Agent agent(client(fake), registry,
                mode == 2 ? nullptr : &provider,  // 使用应用确认策略和指定身份
                "agent-A");
    fake->enqueue(response(QJsonArray{nativeCall()}, "tool_calls"));
    fake->enqueue(response());
    auto input = request();  // 启用已注册工具
    input.enabledTools.append("lookup");
    AgentResult result;  // 增量和正常停止原因
    QVERIFY(agent.run(input, result, error));
    QCOMPARE(count, 0);
    QCOMPARE(error.category, ErrorCategory::None);
    QCOMPARE(result.finishReason,
             mode == 1 ? AgentFinishReason::Cancelled : AgentFinishReason::Completed);
    QCOMPARE(fake->requests().size(), mode == 1 ? 1 : 2);
    if (mode != 2) {
        QCOMPARE(provider.agentId, agent.id());
        QCOMPARE(provider.callId, QStringLiteral("call-1"));
    }
}
void AgentTest::configuration_data()  // 启动前检查避免发送不符合约定的请求
{
    QTest::addColumn<int>("mode");
    for (int mode = 0; mode < 5; ++mode) {  // 当前配置错误编号
        QTest::newRow(qPrintable(QString::number(mode))) << mode;
    }
}
void AgentTest::configuration()  // 非法白名单及限制明确失败
{
    QFETCH(int, mode);                    // 当前配置错误
    FakeTransport* fake = nullptr;        // 观察是否发送请求
    ToolRegistry registry;                // 空注册表
    Agent agent(client(fake), registry);  // 同步 Agent
    auto input = request();               // 待校验请求
    if (mode == 0)
        input.enabledTools.append("unknown");
    if (mode == 1)
        input.limits.maxTurns = 0;
    if (mode == 2)
        input.limits.totalTimeoutSeconds = 0;
    if (mode == 3)
        input.limits.llmTimeoutSeconds = -2;
    if (mode == 4)
        input.limits.maxToolCalls = -2;
    AgentResult result;  // 失败结果
    SdkError error;      // 明确配置错误
    QVERIFY(!agent.run(input, result, error));
    QCOMPARE(result.finishReason, AgentFinishReason::Failed);
    QVERIFY(result.newMessages.isEmpty());
    QVERIFY(fake->requests().isEmpty());
    QVERIFY(!error.code.isEmpty());
}
void AgentTest::limits()  // 不执行最后一轮或 Length 响应中的工具
{
    for (int mode = 0; mode < 2; ++mode) {  // 两种正常停止情形
        FakeTransport* fake = nullptr;      // 请求观察指针
        ToolRegistry registry;              // 工具集合
        SdkError error;                     // 注册和运行输出
        int count = 0;                      // Handler 次数
        QVERIFY(registry.registerTool(definition(), handler(count), error));
        Agent agent(client(fake), registry);  // 同步 Agent
        fake->enqueue(response(QJsonArray{nativeCall()}, mode == 0 ? "tool_calls" : "length"));
        auto input = request();  // 设置一轮上限或 Length
        input.enabledTools.append("lookup");
        if (mode == 0)
            input.limits.maxTurns = 1;
        AgentResult result;  // 保留助手已实际生成的消息
        QVERIFY(agent.run(input, result, error));
        QCOMPARE(result.finishReason,
                 mode == 0 ? AgentFinishReason::MaxTurns : AgentFinishReason::Length);
        QCOMPARE(count, 0);
        QCOMPARE(result.newMessages.size(), 1);
        QVERIFY(!result.finalMessage);
    }
}
void AgentTest::providerFailure()  // 第二轮网络故障不丢弃此前工具闭环消息
{
    FakeTransport* fake = nullptr;  // 请求观察指针
    ToolRegistry registry;          // 工具集合
    SdkError error;                 // 注册及运行输出
    int count = 0;                  // Handler 次数
    QVERIFY(registry.registerTool(definition(), handler(count), error));
    Agent agent(client(fake), registry);  // 同步 Agent
    fake->enqueue(response(QJsonArray{nativeCall()}, "tool_calls"));
    SdkError network;  // 第二轮底层故障
    network.category = ErrorCategory::Network;
    network.code = "ConnectionLost";
    fake->enqueueError(network);
    auto input = request();  // 启用工具请求
    input.enabledTools.append("lookup");
    AgentResult result;  // 已产生的增量
    QVERIFY(!agent.run(input, result, error));
    QCOMPARE(result.finishReason, AgentFinishReason::Failed);
    QCOMPARE(result.newMessages.size(), 2);
    QCOMPARE(error.code, network.code);
    QCOMPARE(count, 1);
}
void AgentTest::handlerCancellation()  // 实际业务结果不因执行结束后的取消而丢失
{
    FakeTransport* fake = nullptr;  // 请求观察指针
    ToolRegistry registry;          // 应用工具集合
    CancellationSource source;      // 单次运行取消来源
    SdkError error;                 // 注册和运行输出
    QVERIFY(registry.registerTool(
        definition(),
        [&source](const ToolCall&,              // 本次调用、上下文或业务结果参数
                  const ToolExecutionContext&,  // 当前调用、停止上下文及业务结果参数
                  ToolResult& result,           // 当前调用、停止上下文及业务结果输出
                  SdkError&) {                  // 完成业务后协作取消
            result.data = 42;
            source.cancel();
            return true;
        },
        error));
    Agent agent(client(fake), registry);  // 同步 Agent
    fake->enqueue(response(QJsonArray{nativeCall()}, "tool_calls"));
    auto input = request();  // 唯一取消状态沿执行链传播
    input.enabledTools.append("lookup");
    input.requestOptions.cancellation = source.token();
    AgentResult result;  // 保存已经完成的业务结果
    QVERIFY(agent.run(input, result, error));
    QCOMPARE(result.finishReason, AgentFinishReason::Cancelled);
    QCOMPARE(result.newMessages.size(), 2);
    QCOMPARE(
        std::get<ToolResultContent>(result.newMessages.last().contents.first()).result.data.toInt(),
        42);
    QCOMPARE(fake->requests().size(), 1);
    QVERIFY(!result.finalMessage);
}
void AgentTest::underlyingTimeout()  // HTTP 请求超时与总运行超时语义不同
{
    FakeTransport* fake = nullptr;        // 请求观察指针
    ToolRegistry registry;                // 空工具集合
    Agent agent(client(fake), registry);  // 同步 Agent
    SdkError timeout;                     // 底层请求故障
    timeout.category = ErrorCategory::Timeout;
    timeout.code = "RequestTimeout";
    fake->enqueueError(timeout);
    AgentResult result;  // 应为 Failed 的结果
    SdkError error;      // 保留底层错误
    QVERIFY(!agent.run(request(), result, error));
    QCOMPARE(result.finishReason, AgentFinishReason::Failed);
    QCOMPARE(error.code, timeout.code);
    QVERIFY(result.newMessages.isEmpty());
}
void AgentTest::cancellation()  // 已取消的 Token 不发送请求
{
    FakeTransport* fake = nullptr;        // 请求观察指针
    ToolRegistry registry;                // 应用集合
    Agent agent(client(fake), registry);  // 同步 Agent
    CancellationSource source;            // 应用唯一取消来源
    source.cancel();
    auto input = request();  // 共享取消状态
    input.requestOptions.cancellation = source.token();
    AgentResult result;  // 正常停止结果
    SdkError error;      // 应清空的异常输出
    QVERIFY(agent.run(input, result, error));
    QCOMPARE(result.finishReason, AgentFinishReason::Cancelled);
    QVERIFY(fake->requests().isEmpty());
    QVERIFY(result.newMessages.isEmpty());
    QCOMPARE(error.category, ErrorCategory::None);
}
void AgentTest::reentry()  // Handler 重入同一实例被拒绝，外层可正常完成并再次运行
{
    FakeTransport* fake = nullptr;        // 请求观察指针
    ToolRegistry registry;                // 工具集合
    Agent agent(client(fake), registry);  // 同实例重入目标
    SdkError error;                       // 注册及运行输出
    QString nestedCode;                   // 嵌套调用的错误码
    QVERIFY(registry.registerTool(
        definition(),
        [&agent, &nestedCode](const ToolCall&,              // 本次调用、上下文或业务结果参数
                              const ToolExecutionContext&,  // 当前调用、停止上下文及业务结果参数
                              ToolResult&,                  // 当前调用、停止上下文及业务结果输出
                              SdkError&) {                  // 模拟工具回调中重入
            AgentResult nested;                             // 嵌套输出，与外层输出独立
            SdkError failure;                               // 嵌套错误
            if (agent.run(request(), nested, failure))
                return false;
            nestedCode = failure.code;
            return true;
        },
        error));
    fake->enqueue(response(QJsonArray{nativeCall()}, "tool_calls"));
    fake->enqueue(response());
    fake->enqueue(response());
    auto input = request();  // 启用重入测试工具
    input.enabledTools.append("lookup");
    AgentResult result;  // 外层运行结果
    QVERIFY(agent.run(input, result, error));
    QCOMPARE(nestedCode, QStringLiteral("AgentBusy"));
    QVERIFY(agent.run(request(), result, error));
    QCOMPARE(result.newMessages.size(), 1);
}
void AgentTest::streaming()  // 流事件通知期间绝不执行 Handler
{
    FakeTransport* fake = nullptr;  // 请求观察指针
    ToolRegistry registry;          // 工具集合
    SdkError error;                 // 注册及运行输出
    int count = 0;                  // 实际 Handler 次数
    QVERIFY(registry.registerTool(definition(), handler(count), error));
    Agent agent(client(fake), registry);  // Streaming Agent
    fake->enqueueStream(
        {"data: {\"choices\":[{\"index\":0,\"delta\":{\"role\":\"assistant\",\"tool_calls\":[{\"index\":0,\"id\":\"call-1\",\"type\":\"function\",\"function\":{\"name\":\"lookup\",\"arguments\":\"{\\\"id\\\":1}\"}}]},\"finish_reason\":null}]}\n\n",
         "data: {\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"tool_calls\"}]}\n\ndata: [DONE]\n\n"});
    fake->enqueueStream(
        {"data: {\"choices\":[{\"index\":0,\"delta\":{\"role\":\"assistant\",\"content\":\"最终回答\"},\"finish_reason\":\"stop\"}]}\n\ndata: [DONE]\n\n"});
    auto input = request();  // 流式请求和唯一工具来源
    input.chat.stream = true;
    input.enabledTools.append("lookup");
    bool earlyExecution = false;  // 第一轮收到事件时是否过早执行
    input.requestOptions.streamCallback = [&count, &earlyExecution,
                                           fake](const StreamEvent&) {  // 回调只展示模型生成事件
        if (fake->requests().size() == 1 && count != 0)
            earlyExecution = true;
    };
    AgentResult result;  // 完整主链输出
    QVERIFY(agent.run(input, result, error));
    QVERIFY(!earlyExecution);
    QCOMPARE(count, 1);
    QCOMPARE(result.finishReason, AgentFinishReason::Completed);
    QCOMPARE(result.newMessages.size(), 3);
}
void AgentTest::partialStreaming()  // 缺少完整结束标识时保留文本而不执行工具
{
    FakeTransport* fake = nullptr;  // 请求观察指针
    ToolRegistry registry;          // 工具集合
    SdkError error;                 // 注册及运行输出
    int count = 0;                  // Handler 次数
    QVERIFY(registry.registerTool(definition(), handler(count), error));
    Agent agent(client(fake), registry);  // Streaming Agent
    fake->enqueueStream(
        {"data: {\"choices\":[{\"index\":0,\"delta\":{\"role\":\"assistant\",\"content\":\"部分文本\"},\"finish_reason\":null}]}\n\n"});
    auto input = request();  // 开启 Streaming 并提供工具
    input.chat.stream = true;
    input.enabledTools.append("lookup");
    AgentResult result;  // 部分有效输出
    QVERIFY(!agent.run(input, result, error));
    QCOMPARE(result.finishReason, AgentFinishReason::Failed);
    QCOMPARE(result.newMessages.size(), 1);
    QCOMPARE(result.newMessages.first().status, MessageStatus::Incomplete);
    QCOMPARE(result.newMessages.first().text(), QStringLiteral("部分文本"));
    QCOMPARE(count, 0);
}
void AgentTest::usage()  // 各字段独立处理未知值，不推算总量
{
    FakeTransport* fake = nullptr;  // 请求观察指针
    ToolRegistry registry;          // 工具集合
    SdkError error;                 // 注册及运行输出
    int count = 0;                  // Handler 次数
    QVERIFY(registry.registerTool(definition(), handler(count), error));
    Agent agent(client(fake), registry);  // 多轮 Agent
    fake->enqueue(
        response(QJsonArray{nativeCall()}, "tool_calls",
                 QJsonObject{{"prompt_tokens", 0}, {"completion_tokens", 3}, {"total_tokens", 3}}));
    fake->enqueue(response({}, "stop", QJsonObject{{"prompt_tokens", 5}, {"total_tokens", 9}}));
    auto input = request();  // 启用工具形成两轮调用
    input.enabledTools.append("lookup");
    input.requestOptions.timeoutSeconds = 9;
    input.limits.llmTimeoutSeconds = 7;
    AgentResult result;  // 汇总用量
    QVERIFY(agent.run(input, result, error));
    QCOMPARE(result.turnUsages.size(), 2);
    QCOMPARE(*result.totalUsage.inputTokens, qint64(5));
    QVERIFY(!result.totalUsage.outputTokens);
    QCOMPARE(*result.totalUsage.totalTokens, qint64(12));
    QCOMPARE(fake->timeouts(), QList<int>({7, 7}));
    QCOMPARE(input.requestOptions.timeoutSeconds, 9);
}
QTEST_GUILESS_MAIN(AgentTest)
#include "tst_Agent.moc"
