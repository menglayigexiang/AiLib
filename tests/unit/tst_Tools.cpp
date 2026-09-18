#include <AiLib/tools/ToolExecutor.h>
#include <AiLib/tools/ToolErrorCodes.h>
#include <QJsonArray>
#include <QtTest>
#include <atomic>
#include <future>
#include <stdexcept>
using namespace AiLib;
// 使用应用侧同步策略验证确认决定和上下文传播。
class Approval : public IToolApprovalProvider {
public:
    ToolApprovalDecision decision = ToolApprovalDecision::Allow;  // 测试所需的用户决定
    bool available = true;                                        // 模拟确认机制故障
    QString agentId;                                              // 收到的来源身份
    QString callId;                                               // 收到的调用关联
    std::function<void()> beforeDecision;                         // 应用等待或故障模拟，不由 SDK 创建线程
    int count = 0;                                                // 确认次数
    bool requestApproval(const ToolCall& call,                    // 待执行的规范化调用
                         const FunctionToolDefinition&,           // 当前工具声明（本测试不读取）
                         const ToolExecutionContext& context,     // 来源和停止上下文
                         ToolApprovalResult& result,              // 用户确认决定输出
                         SdkError& error) override                // 同步记录来源并返回预设决定
    {
        if (beforeDecision)
            beforeDecision();
        ++count;
        agentId = context.agentId;
        callId = call.id;
        result.decision = decision;
        result.reason = QStringLiteral("测试决定");
        error.message = QStringLiteral("测试确认故障");
        return available;
    }
};
// 验证独立工具闭环、Schema 边界及共享条目的并发行为。
class ToolsTest : public QObject {
    Q_OBJECT
private slots:
    void registration();    // 检查注册、查询和删除边界
    void execution();       // 验证关联字段和成功规范化
    void failures_data();   // 构造业务失败和异常情形
    void failures();        // 确认业务失败不升级为流程错误
    void approval_data();   // 构造各确认决定及不可用情形
    void approval();        // 检查同步确认及来源关联
    void arguments_data();  // 构造嵌套参数及类型约束
    void arguments();       // 参数非法时不确认或执行
    void cancellation();    // 检查执行前后取消及截止时间
    void nestedSchema();    // 检查嵌套数组、枚举及诊断路径
    void approvalWait();    // 检查等待取消、策略异常及等待时未占用执行锁
    void serialized();      // 两个执行器共用串行锁且等待可取消
    void concurrent();      // Concurrent 工具允许两个执行器同时进入 Handler
};
static FunctionToolDefinition definition()  // 创建整数参数工具描述
{
    FunctionToolDefinition value;  // 本地工具声明
    value.name = "add";
    value.inputSchema =
        QJsonObject{{"type", "object"},
                    {"properties", QJsonObject{{"n", QJsonObject{{"type", "integer"}}}}},
                    {"required", QJsonArray{"n"}},
                    {"additionalProperties", false}};
    return value;
}
static ToolCall call()  // 创建具有稳定调用关联的有效参数
{
    ToolCall value;  // 测试调用
    value.id = "call-1";
    value.name = "add";
    value.arguments = QJsonObject{{"n", 3}};
    return value;
}
void ToolsTest::registration()  // 非法注册不污染已有集合
{
    ToolRegistry registry;     // 应用持有的注册表
    SdkError error;            // 注册错误输出
    auto tool = definition();  // 待注册描述
    ToolHandler handler = [](const ToolCall&, const ToolExecutionContext&, ToolResult&, SdkError&) {
        return true;
    };  // 最小同步业务函数
    QVERIFY(registry.registerTool(tool, handler, error));
    QVERIFY(!registry.registerTool(tool, handler, error));
    QVERIFY(registry.contains("add"));
    FunctionToolDefinition output;  // 查询描述输出
    QVERIFY(registry.definition("add", output));
    QCOMPARE(output.name, tool.name);
    QVERIFY(!registry.definition("missing", output));
    QCOMPARE(output.name, tool.name);
    tool.name = "other";
    tool.inputSchema.insert("minimum", 0);
    QVERIFY(!registry.registerTool(tool, handler, error));
    QCOMPARE(error.code, QStringLiteral("InvalidToolSchema"));
    tool.inputSchema = QJsonObject{{"required", QJsonArray{1}}};
    QVERIFY(!registry.registerTool(tool, handler, error));
    tool.inputSchema = {};
    QVERIFY(!registry.registerTool(tool, {}, error));
    QVERIFY(registry.removeTool("add"));
    QVERIFY(!registry.removeTool("add"));
}
void ToolsTest::execution()  // Handler 无法破坏调用关联
{
    ToolRegistry registry;  // 工具集合
    SdkError error;         // 流程错误
    QVERIFY(registry.registerTool(
        definition(),
        [](const ToolCall&, const ToolExecutionContext&,
           ToolResult& result,  // 本次工具调用及业务结果参数
           SdkError&) {         // 模拟成功函数误填关联及错误字段
            result.callId = "wrong";
            result.toolName = "wrong";
            result.data = 42;
            result.errorCode = "stale";
            result.errorMessage = "stale";
            return true;
        },
        error));
    ToolExecutor executor(registry);  // 独立执行器
    ToolResult result;                // 最终业务结果
    QVERIFY(executor.execute(call(), {}, result, error));
    QVERIFY(result.success);
    QCOMPARE(result.callId, QStringLiteral("call-1"));
    QCOMPARE(result.toolName, QStringLiteral("add"));
    QCOMPARE(result.data.toInt(), 42);
    QVERIFY(result.errorCode.isEmpty());
    QVERIFY(result.errorMessage.isEmpty());
    QCOMPARE(error.category, ErrorCategory::None);
}
void ToolsTest::failures_data()  // 业务结果与 SDK 错误解耦
{
    QTest::addColumn<int>("mode");
    for (int mode = 0; mode < 5; ++mode) {  // 失败类型编号
        QTest::newRow(qPrintable(QString::number(mode))) << mode;
    }
}
void ToolsTest::failures()  // 不成功的业务调用仍正常返回工具结果
{
    QFETCH(int, mode);      // 当前失败类型
    ToolRegistry registry;  // 工具集合
    SdkError error;         // SDK 流程输出
    QVERIFY(registry.registerTool(
        definition(),
        [mode](const ToolCall&, const ToolExecutionContext&,
               ToolResult& result,   // 本次工具调用及业务结果参数
               SdkError& failure) {  // 模拟错误、业务拒绝和异常
            if (mode == 0) {
                failure.code = "DeviceOffline";
                failure.message = "设备离线";
                return false;
            }
            if (mode == 1)
                return false;
            if (mode == 2)
                throw std::runtime_error("handler failure");
            if (mode == 3)
                throw 7;
            result.success = false;
            result.errorCode = "CustomFailure";
            result.errorMessage = "业务失败";
            return true;
        },
        error));
    ToolExecutor executor(registry);  // 被测执行器
    ToolResult result;                // 工具输出
    QVERIFY(executor.execute(call(), {}, result, error));
    QVERIFY(!result.success);
    QCOMPARE(error.category, ErrorCategory::None);
    QCOMPARE(result.callId, QStringLiteral("call-1"));
    QCOMPARE(result.errorCode, mode == 0   ? QStringLiteral("DeviceOffline")
                               : mode == 4 ? QStringLiteral("CustomFailure")
                                           : ToolErrorCodes::ExecutionFailed);
}
void ToolsTest::approval_data()  // 各类决定与机制故障
{
    QTest::addColumn<int>("mode");
    for (int mode = 0; mode < 5; ++mode) {  // 确认情形编号
        QTest::newRow(qPrintable(QString::number(mode))) << mode;
    }
}
void ToolsTest::approval()  // 仅 Allow 可以执行 Handler
{
    QFETCH(int, mode);         // 当前确认情形
    ToolRegistry registry;     // 工具集合
    SdkError error;            // 流程输出
    int executed = 0;          // 业务执行次数
    auto tool = definition();  // 需要每次确认的描述
    tool.approvalPolicy = ToolApprovalPolicy::Always;
    QVERIFY(registry.registerTool(
        tool,
        [&executed](const ToolCall&, const ToolExecutionContext&, ToolResult&, SdkError&) {
            ++executed;
            return true;
        },
        error));        // 记录业务是否被执行
    Approval provider;  // 应用策略
    provider.decision = mode == 1   ? ToolApprovalDecision::Deny
                        : mode == 2 ? ToolApprovalDecision::Cancel
                                    : ToolApprovalDecision::Allow;
    provider.available = mode != 3;
    ToolExecutor executor(registry, mode == 4 ? nullptr : &provider);  // 模拟缺失确认策略
    ToolExecutionContext context;                                      // Agent 来源传递
    context.agentId = "agent-A";
    ToolResult result;  // 工具结果
    QCOMPARE(executor.execute(call(), context, result, error), mode != 2);
    QCOMPARE(executed, mode == 0 ? 1 : 0);
    if (mode != 4) {
        QCOMPARE(provider.agentId, context.agentId);
        QCOMPARE(provider.callId, call().id);
    }
    if (mode == 1)
        QCOMPARE(result.errorCode, ToolErrorCodes::ApprovalDenied);
    if (mode >= 3)
        QCOMPARE(result.errorCode, ToolErrorCodes::ApprovalUnavailable);
    if (mode == 2)
        QCOMPARE(error.category, ErrorCategory::Cancelled);
}
void ToolsTest::arguments_data()  // 整数、必填字段和额外字段
{
    QTest::addColumn<QJsonObject>("parameters");
    QTest::addColumn<bool>("valid");
    QTest::newRow("integer") << QJsonObject{{"n", 3}} << true;
    QTest::newRow("fraction") << QJsonObject{{"n", 3.5}} << false;
    QTest::newRow("missing") << QJsonObject{} << false;
    QTest::newRow("extra") << QJsonObject{{"n", 3}, {"extra", true}} << false;
    QTest::newRow("wrong-type") << QJsonObject{{"n", "3"}} << false;
}
void ToolsTest::arguments()  // 失败验证发生于 Approval 之前
{
    QFETCH(QJsonObject, parameters);  // 当前参数
    QFETCH(bool, valid);              // 预期验证结果
    ToolRegistry registry;            // 工具集合
    SdkError error;                   // 流程输出
    auto tool = definition();         // 待注册描述
    tool.approvalPolicy = ToolApprovalPolicy::Always;
    QVERIFY(registry.registerTool(
        tool,
        [](const ToolCall&, const ToolExecutionContext&, ToolResult&, SdkError&) {
            return true;
        },                                       // 本次工具调用及业务结果参数
        error));                                 // 无副作用测试函数
    Approval provider;                           // 记录是否进入确认
    ToolExecutor executor(registry, &provider);  // 独立执行器
    auto request = call();                       // 当前调用
    request.arguments = parameters;
    ToolResult result;  // 最终结果
    QVERIFY(executor.execute(request, {}, result, error));
    QCOMPARE(result.success, valid);
    QCOMPARE(provider.count, valid ? 1 : 0);
    if (!valid)
        QCOMPARE(result.errorCode, ToolErrorCodes::InvalidArguments);
}
void ToolsTest::cancellation()  // 取消可停止前置流程和后续继续执行
{
    ToolRegistry registry;         // 工具集合
    CancellationSource source;     // 应用取消来源
    ToolExecutionContext context;  // 统一取消上下文
    context.cancellation = source.token();
    SdkError error;  // 流程输出
    int count = 0;   // Handler 次数
    QVERIFY(registry.registerTool(
        definition(),
        [&source, &count](const ToolCall&, const ToolExecutionContext&,
                          ToolResult& result,  // 本次工具调用及业务结果参数
                          SdkError&) {         // 在业务完成时请求取消
            ++count;
            result.data = 17;
            source.cancel();
            return true;
        },
        error));
    ToolExecutor executor(registry);  // 被测执行器
    ToolResult result;                // 工具结果
    QVERIFY(!executor.execute(call(), context, result, error));
    QCOMPARE(error.category, ErrorCategory::Cancelled);
    QCOMPARE(result.data.toInt(), 17);
    QVERIFY(!executor.execute(call(), context, result, error));
    QCOMPARE(count, 1);
    context.cancellation = {};
    context.deadline = std::chrono::steady_clock::now();
    QVERIFY(!executor.execute(call(), context, result, error));
    QCOMPARE(error.category, ErrorCategory::Timeout);
    QCOMPARE(count, 1);
    context.deadline.reset();
    auto missing = call();  // 未注册工具调用
    missing.name = "missing";
    QVERIFY(executor.execute(missing, context, result, error));
    QCOMPARE(result.errorCode, ToolErrorCodes::ToolNotFound);
    missing.id.clear();
    QVERIFY(!executor.execute(missing, context, result, error));
}
void ToolsTest::nestedSchema()  // 递归 Schema 不静默漏掉数组及枚举约束
{
    ToolRegistry registry;     // 工具集合
    auto tool = definition();  // 嵌套参数声明
    tool.inputSchema = QJsonObject{
        {"type", "object"},
        {"properties",
         QJsonObject{{"n", QJsonObject{{"type", "array"},
                                       {"items", QJsonObject{{"type", "string"},
                                                             {"enum", QJsonArray{"A", "B"}}}}}}}},
        {"required", QJsonArray{"n"}}};
    SdkError error;  // 流程输出
    QVERIFY(registry.registerTool(
        tool,
        [](const ToolCall&, const ToolExecutionContext&, ToolResult&, SdkError&) {
            return true;
        },                            // 本次工具调用及业务结果参数
        error));                      // 无副作用业务函数
    ToolExecutor executor(registry);  // 独立执行器
    auto request = call();            // 当前调用参数
    request.arguments = QJsonObject{{"n", QJsonArray{"A", "B"}}};
    ToolResult result;  // 业务输出
    QVERIFY(executor.execute(request, {}, result, error));
    QVERIFY(result.success);
    request.arguments = QJsonObject{{"n", QJsonArray{"A", "C"}}};
    QVERIFY(executor.execute(request, {}, result, error));
    QVERIFY(!result.success);
    QVERIFY(result.errorMessage.contains("$.n[1]"));
}
void ToolsTest::approvalWait()  // 应用等待期间可取消，且不占用共享工具执行锁
{
    ToolRegistry registry;     // 共享工具集合
    auto tool = definition();  // 每次需要确认的串行工具
    tool.approvalPolicy = ToolApprovalPolicy::Always;
    SdkError error;                // 注册输出
    std::atomic<int> executed{0};  // 实际业务次数
    QVERIFY(registry.registerTool(
        tool,
        [&executed](const ToolCall&, const ToolExecutionContext&, ToolResult&, SdkError&) {
            ++executed;
            return true;
        },
        error));                   // 记录执行数量
    CancellationSource source;     // 应用取消来源
    ToolExecutionContext context;  // 共享取消上下文
    context.cancellation = source.token();
    Approval provider;                                         // 应用同步确认策略
    provider.beforeDecision = [&source] { source.cancel(); };  // 模拟等待用户时取消
    ToolExecutor executor(registry, &provider);                // 借用应用确认策略
    ToolResult result;                                         // 业务输出
    QVERIFY(!executor.execute(call(), context, result, error));
    QCOMPARE(error.category, ErrorCategory::Cancelled);
    QCOMPARE(executed.load(), 0);
    provider.beforeDecision = [] {
        throw std::runtime_error("approval failure");
    };  // 模拟确认策略异常
    QVERIFY(executor.execute(call(), {}, result, error));
    QCOMPARE(result.errorCode, ToolErrorCodes::ApprovalUnavailable);
    std::promise<void> entered;                // 首次确认已进入
    std::promise<void> release;                // 允许首次确认结束
    auto gate = release.get_future().share();  // 应用等待门
    provider.beforeDecision = [&entered, gate] {
        entered.set_value();
        gate.wait();
    };                                                         // 模拟等待 UI 决定
    auto first = std::async(std::launch::async, [&executor] {  // 应用侧工作线程执行确认流程
        ToolResult result;                                     // 首次调用结果
        SdkError error;                                        // 首次调用流程输出
        return executor.execute(call(), {}, result, error);
    });
    entered.get_future().wait();
    Approval otherProvider;                                  // 第二个 Agent 的独立确认策略
    ToolExecutor other(registry, &otherProvider);            // 共用 Registry 的另一个执行器
    auto second = std::async(std::launch::async, [&other] {  // 验证确认等待没有占用工具锁
        ToolResult result;                                   // 第二次调用结果
        SdkError error;                                      // 第二次调用流程输出
        return other.execute(call(), {}, result, error);
    });
    const auto status = second.wait_for(std::chrono::seconds(2));  // 有界检查另一调用能否执行
    release.set_value();
    QVERIFY(first.get());
    QVERIFY(second.get());
    QCOMPARE(status, std::future_status::ready);
    QCOMPARE(executed.load(), 2);
}
void ToolsTest::serialized()  // 跨执行器串行等待可协作取消
{
    ToolRegistry registry;                     // 两个执行器共享的工具集合
    SdkError error;                            // 注册错误输出
    std::promise<void> entered;                // 通知首个 Handler 已进入
    std::promise<void> release;                // 控制首个 Handler 返回
    auto gate = release.get_future().share();  // 首个 Handler 的等待门
    std::atomic<int> count{0};                 // 原子执行次数
    QVERIFY(registry.registerTool(
        definition(),
        [&entered, gate, &count](const ToolCall&, const ToolExecutionContext&,
                                 ToolResult&,  // 本次工具调用及业务结果参数
                                 SdkError&) {  // 首次执行占用共享工具锁
            if (++count == 1) {
                entered.set_value();
                gate.wait();
            }
            return true;
        },
        error));
    ToolExecutor first(registry);                             // 首个执行器
    ToolExecutor second(registry);                            // 第二个执行器
    auto running = std::async(std::launch::async, [&first] {  // 应用测试线程执行首个调用
        ToolResult result;                                    // 当前线程调用结果
        SdkError error;                                       // 当前线程流程输出
        return first.execute(call(), {}, result, error);
    });
    entered.get_future().wait();
    CancellationSource source;     // 第二个调用的取消来源
    ToolExecutionContext context;  // 第二个调用上下文
    context.cancellation = source.token();
    auto waiting = std::async(std::launch::async, [&second, context] {  // 测试等待锁时的取消
        ToolResult result;                                              // 当前线程调用结果
        SdkError error;                                                 // 当前线程流程输出
        return second.execute(call(), context, result, error) ||
               error.category != ErrorCategory::Cancelled;
    });
    const auto blocked =  // 取消前仍应被共享执行锁阻挡
        waiting.wait_for(std::chrono::milliseconds(50));
    source.cancel();
    const auto status = waiting.wait_for(std::chrono::seconds(2));  // 有界等待取消响应
    release.set_value();
    QVERIFY(running.get());
    QCOMPARE(blocked, std::future_status::timeout);
    QCOMPARE(status, std::future_status::ready);
    QVERIFY(!waiting.get());
    QCOMPARE(count.load(), 1);
}
void ToolsTest::concurrent()  // 显式允许并行时不持有串行锁
{
    ToolRegistry registry;     // 共享工具集合
    SdkError error;            // 注册输出
    auto tool = definition();  // 并发工具描述
    tool.concurrency = ToolConcurrency::Concurrent;
    std::promise<void> both;                   // 通知两个调用都进入
    auto barrier = both.get_future().share();  // 两个 Handler 共用门
    std::atomic<int> count{0};                 // 当前进入数量
    QVERIFY(registry.registerTool(
        tool,
        [&both, barrier, &count](const ToolCall&, const ToolExecutionContext&,
                                 ToolResult&,  // 本次工具调用及业务结果参数
                                 SdkError&) {  // 有界等待第二个 Handler
            if (++count == 2)
                both.set_value();
            return barrier.wait_for(std::chrono::seconds(2)) == std::future_status::ready;
        },
        error));
    ToolExecutor executor(registry);                           // 可并发使用的无请求状态执行器
    auto first = std::async(std::launch::async, [&executor] {  // 应用侧首个调用
        ToolResult result;                                     // 当前线程调用结果
        SdkError error;                                        // 当前线程流程输出
        return executor.execute(call(), {}, result, error) && result.success;
    });
    ToolResult result;  // 当前线程调用结果
    QVERIFY(executor.execute(call(), {}, result, error));
    QVERIFY(result.success);
    QVERIFY(first.get());
    QCOMPARE(count.load(), 2);
}
QTEST_GUILESS_MAIN(ToolsTest)
#include "tst_Tools.moc"
