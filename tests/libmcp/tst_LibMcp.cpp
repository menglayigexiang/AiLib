#include <LibMcp/InMemoryTransport.h>
#include <LibMcp/McpClient.h>
#include <LibMcp/McpClientManager.h>
#include <LibMcp/McpServer.h>

#include <QEventLoop>
#include <QFutureWatcher>
#include <QJsonArray>
#include <QJsonObject>
#include <QTest>
#include <QTimer>

using namespace LibMcp;

template<typename T>
bool waitForFuture(
    QFuture<T> future,  // 需要等待完成的异步结果
    T& result,         // 接收异步操作的最终结果
    int timeoutMs)     // 最长等待时间，单位为毫秒
{                      // 在有限时间内等待异步结果，返回是否按时完成
    QFutureWatcher<T> watcher;  // 监视异步任务的完成状态
    QEventLoop loop;            // 在当前测试线程中处理异步回调
    QTimer timeoutTimer;        // 防止测试因异步任务异常而永久挂起

    timeoutTimer.setSingleShot(true);
    QObject::connect(&watcher, &QFutureWatcher<T>::finished,
                     &loop, &QEventLoop::quit);
    QObject::connect(&timeoutTimer, &QTimer::timeout,
                     &loop, &QEventLoop::quit);

    watcher.setFuture(future);
    timeoutTimer.start(timeoutMs);
    if (!watcher.isFinished()) {
        loop.exec();
    }

    if (!watcher.isFinished()) {
        return false;
    }

    result = watcher.result();
    return true;
}

// 验证 LibMcp 的配置持久化与内存传输协议主流程。
class LibMcpTest final : public QObject
{
    Q_OBJECT

private slots:
    void managerSerialization();  // 验证客户端配置的序列化与反序列化
    void inMemoryProtocol();       // 验证工具、资源和提示词的内存协议交互
};

void LibMcpTest::managerSerialization()  // 验证客户端配置的序列化与反序列化
{
    McpClientManager manager;  // 保存待序列化的客户端配置
    McpClientConfig config;    // 描述一个远程 MCP 客户端连接

    config.id = QStringLiteral("remote");
    config.transportType = QStringLiteral("streamable-http");
    config.transportConfig = {
        {QStringLiteral("url"), QStringLiteral("http://127.0.0.1:8080/mcp")}};

    QVERIFY(manager.addConfig(config));

    McpClientManager restored;             // 接收反序列化后的客户端配置
    const McpResult<void> restoreResult =  // 保存反序列化操作的业务结果
        restored.deserialize(manager.serialize());

    QVERIFY2(restoreResult.isSuccess(),
             qPrintable(restoreResult.error().message));
    QCOMPARE(restored.configs().size(), 1);
    QCOMPARE(restored.configs().front().id, config.id);
}

void LibMcpTest::inMemoryProtocol()  // 验证工具、资源和提示词的内存协议交互
{
    auto transportPair = createInMemoryTransportPair();  // 创建互联的客户端与服务端内存传输
    McpServer server(                                    // 提供测试所需的 MCP 能力
        std::move(transportPair.second),
        {QStringLiteral("test-server"),
         QStringLiteral("1.0"),
         QStringLiteral("Test Server")});

    McpTool echoTool;  // 描述原样返回输入参数的工具
    echoTool.name = QStringLiteral("echo");
    echoTool.inputSchema = {{QStringLiteral("type"), QStringLiteral("object")}};
    QVERIFY(server.addTool(
        echoTool,
        [](const QJsonArray& input) { return input; }));  // 原样返回工具输入以验证调用链

    McpTool validatedTool;  // 描述要求 value 字符串字段的校验工具
    validatedTool.name = QStringLiteral("required-value");
    validatedTool.inputSchema = {
        {QStringLiteral("type"), QStringLiteral("object")},
        {QStringLiteral("required"), QJsonArray{QStringLiteral("value")}},
        {QStringLiteral("properties"),
         QJsonObject{{QStringLiteral("value"),
                      QJsonObject{{QStringLiteral("type"),
                                   QStringLiteral("string")}}}}}};
    QVERIFY(server.addTool(
        validatedTool,
        [](const QJsonArray& input) { return input; }));  // 返回通过 Schema 校验的工具输入

    McpResource resource;  // 描述固定状态文本资源
    resource.name = QStringLiteral("status");
    resource.uri = QStringLiteral("test://status");
    QVERIFY(server.addResource(
        resource,
        [](const QString& uri) {  // 根据请求 URI 生成固定状态资源
            return QList<McpResourceContent>{McpTextResourceContent{
                uri, QStringLiteral("text/plain"), QStringLiteral("running"), {}}};
        }));

    McpResourceTemplate resourceTemplate;  // 描述按用户编号展开的资源模板
    resourceTemplate.name = QStringLiteral("user");
    resourceTemplate.uriTemplate = QStringLiteral("test://users/{id}");
    QVERIFY(server.addResourceTemplate(
        resourceTemplate,
        [](const QString& uri) {  // 根据展开后的 URI 生成模板资源
            return QList<McpResourceContent>{McpTextResourceContent{
                uri, QStringLiteral("text/plain"), QStringLiteral("templated"), {}}};
        }));

    McpPrompt prompt;  // 描述需要 name 参数的问候提示词
    prompt.name = QStringLiteral("greet");
    prompt.arguments.append({QStringLiteral("name"), {}, true});
    QVERIFY(server.addPrompt(
        prompt,
        [](const QJsonObject& arguments) {  // 使用 name 参数生成问候消息
            return QList<McpPromptMessage>{McpPromptMessage{
                McpRole::User,
                QJsonObject{{QStringLiteral("type"), QStringLiteral("text")},
                            {QStringLiteral("text"),
                             QStringLiteral("Hello %1")
                                 .arg(arguments.value(QStringLiteral("name")).toString())}}}};
        }));

    McpClient client(  // 通过内存传输访问测试服务端
        std::move(transportPair.first),
        {QStringLiteral("test-client"), QStringLiteral("1.0")});

    constexpr int timeoutMs = 5000;   // 单个异步操作的最长等待时间
    McpResult<void> lifecycleResult =  // 保存启动和停止操作结果
        McpResult<void>::failure({});
    QVERIFY(waitForFuture(server.start(), lifecycleResult, timeoutMs));
    QVERIFY(lifecycleResult.isSuccess());
    QVERIFY(waitForFuture(client.start(), lifecycleResult, timeoutMs));
    QVERIFY(lifecycleResult.isSuccess());

    McpResult<QList<McpTool>> toolsResult =  // 保存工具列表查询结果
        McpResult<QList<McpTool>>::failure({});
    QVERIFY(waitForFuture(client.listTools(), toolsResult, timeoutMs));
    QVERIFY(toolsResult.isSuccess());
    QCOMPARE(toolsResult.value().size(), 2);

    McpResult<QJsonArray> toolCallResult =  // 保存工具调用结果
        McpResult<QJsonArray>::failure({});
    QVERIFY(waitForFuture(
        client.callTool(
            QStringLiteral("echo"),
            QJsonArray{QJsonObject{{QStringLiteral("value"), 42}}}),
        toolCallResult,
        timeoutMs));
    QVERIFY(toolCallResult.isSuccess());
    QCOMPARE(toolCallResult.value().size(), 1);

    McpResult<QJsonArray> invalidToolResult =  // 保存无效工具输入的校验结果
        McpResult<QJsonArray>::failure({});
    QVERIFY(waitForFuture(
        client.callTool(
            QStringLiteral("required-value"),
            QJsonArray{QJsonObject{}}),
        invalidToolResult,
        timeoutMs));
    QVERIFY(invalidToolResult.isError());

    McpResult<QList<McpResource>> resourcesResult =  // 保存资源列表查询结果
        McpResult<QList<McpResource>>::failure({});
    QVERIFY(waitForFuture(client.listResources(), resourcesResult, timeoutMs));
    QVERIFY(resourcesResult.isSuccess());
    QCOMPARE(resourcesResult.value().size(), 1);

    McpResult<QList<McpResourceContent>> contentsResult =  // 保存固定资源读取结果
        McpResult<QList<McpResourceContent>>::failure({});
    QVERIFY(waitForFuture(
        client.readResource(QStringLiteral("test://status")),
        contentsResult,
        timeoutMs));
    QVERIFY(contentsResult.isSuccess());
    QCOMPARE(contentsResult.value().size(), 1);

    McpResult<QList<McpResourceTemplate>> templatesResult =  // 保存资源模板列表查询结果
        McpResult<QList<McpResourceTemplate>>::failure({});
    QVERIFY(waitForFuture(
        client.listResourceTemplates(),
        templatesResult,
        timeoutMs));
    QVERIFY(templatesResult.isSuccess());
    QCOMPARE(templatesResult.value().size(), 1);

    McpResult<QList<McpResourceContent>> templatedContentsResult =  // 保存模板资源读取结果
        McpResult<QList<McpResourceContent>>::failure({});
    QVERIFY(waitForFuture(
        client.readResource(QStringLiteral("test://users/7")),
        templatedContentsResult,
        timeoutMs));
    QVERIFY(templatedContentsResult.isSuccess());
    QCOMPARE(templatedContentsResult.value().size(), 1);

    McpResult<QList<McpPrompt>> promptsResult =  // 保存提示词列表查询结果
        McpResult<QList<McpPrompt>>::failure({});
    QVERIFY(waitForFuture(client.listPrompts(), promptsResult, timeoutMs));
    QVERIFY(promptsResult.isSuccess());
    QCOMPARE(promptsResult.value().size(), 1);

    McpResult<QList<McpPromptMessage>> promptMessagesResult =  // 保存提示词生成的消息结果
        McpResult<QList<McpPromptMessage>>::failure({});
    QVERIFY(waitForFuture(
        client.getPrompt(
            QStringLiteral("greet"),
            QJsonObject{{QStringLiteral("name"), QStringLiteral("Qt")}}),
        promptMessagesResult,
        timeoutMs));
    QVERIFY(promptMessagesResult.isSuccess());
    QCOMPARE(promptMessagesResult.value().size(), 1);

    QVERIFY(waitForFuture(client.stop(), lifecycleResult, timeoutMs));
    QVERIFY(lifecycleResult.isSuccess());
    QVERIFY(waitForFuture(server.stop(), lifecycleResult, timeoutMs));
    QVERIFY(lifecycleResult.isSuccess());
}

QTEST_MAIN(LibMcpTest)

#include "tst_LibMcp.moc"
