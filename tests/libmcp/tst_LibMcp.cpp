#include <LibMcp/InMemoryTransport.h>
#include <LibMcp/McpClient.h>
#include <LibMcp/McpClientManager.h>
#include <LibMcp/McpServer.h>
#include <LibMcp/StreamableHttpTransport.h>
#include <LibMcp/StdioTransport.h>

#include <QEventLoop>
#include <QFutureWatcher>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTest>
#include <QTimer>

#include <memory>

using namespace LibMcp;

// 模拟 HTTP 可达但不支持现代 server/discover 的旧协议 Server。
class LegacyProtocolHttpServer final : public QObject
{
    Q_OBJECT
public:
    explicit LegacyProtocolHttpServer(QObject* parent = nullptr)  // 创建固定返回方法不存在错误的本地 Server
        : QObject(parent)
    {
        connect(&m_server,
                &QTcpServer::newConnection,
                this,
                [this] {  // 为每个诊断请求返回匹配 ID 的 JSON-RPC 错误
                    QTcpSocket* socket = m_server.nextPendingConnection();  // 当前需要处理的本地连接
                    auto buffer = std::make_shared<QByteArray>();  // 累积可能分段到达的 HTTP 请求
                    connect(socket,
                            &QTcpSocket::readyRead,
                            socket,
                            [socket, buffer] {  // 收齐 HTTP Body 后发送旧协议特征响应
                                buffer->append(socket->readAll());
                                const qsizetype headerEnd = buffer->indexOf("\r\n\r\n");  // 定位 HTTP Body 起点
                                if (headerEnd < 0) {
                                    return;
                                }
                                const QByteArray headers = buffer->left(headerEnd);  // 读取 Content-Length 所在的 Header
                                const QRegularExpression lengthExpression(
                                    QStringLiteral("Content-Length:\\s*(\\d+)"),
                                    QRegularExpression::CaseInsensitiveOption);  // 匹配请求体字节数
                                const QRegularExpressionMatch lengthMatch =
                                    lengthExpression.match(QString::fromLatin1(headers));  // 解析请求体长度
                                const int contentLength = lengthMatch.hasMatch()
                                                              ? lengthMatch.captured(1).toInt()
                                                              : 0;  // 请求体预期字节数
                                const qsizetype bodyStart = headerEnd + 4;  // HTTP Body 起始偏移
                                if (buffer->size() - bodyStart < contentLength) {
                                    return;
                                }
                                const QJsonDocument requestDocument = QJsonDocument::fromJson(
                                    buffer->mid(bodyStart, contentLength));  // 解析 JSON-RPC 请求以回显 ID
                                const QJsonValue requestId =
                                    requestDocument.object().value(QStringLiteral("id"));  // 当前请求 ID
                                const QJsonObject responseObject{  // 模拟旧 Server 不认识现代发现方法
                                    {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
                                    {QStringLiteral("id"), requestId},
                                    {QStringLiteral("error"),
                                     QJsonObject{{QStringLiteral("code"), -32601},
                                                 {QStringLiteral("message"),
                                                  QStringLiteral("Method not found: server/discover")}}}};
                                const QByteArray body = QJsonDocument(responseObject).toJson(
                                    QJsonDocument::Compact);  // 编码完整 JSON-RPC 错误体
                                const QByteArray response =  // 构造关闭连接的标准 HTTP JSON 响应
                                    QByteArrayLiteral("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: ")
                                    + QByteArray::number(body.size())
                                    + QByteArrayLiteral("\r\nConnection: close\r\n\r\n")
                                    + body;
                                socket->write(response);
                                socket->disconnectFromHost();
                            });
                });
    }

    bool start()  // 在回环地址的系统分配端口启动监听
    {
        return m_server.listen(QHostAddress::LocalHost, 0);
    }

    quint16 port() const  // 返回系统实际分配的监听端口
    {
        return m_server.serverPort();
    }

private:
    QTcpServer m_server;  // 承载旧协议诊断响应的本地 HTTP 监听器
};

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

template<typename T>
bool waitForOperation(
    const QSharedPointer<McpOperation<T>>& operation,  // 需要等待完成的公共异步操作
    McpResult<T>& result,                              // 接收操作的最终成功值或错误
    int timeoutMs)                                     // 最长等待时间，单位为毫秒
{                                                      // 在当前事件循环等待类型化 Operation
    QSignalSpy finishedSpy(operation.data(), &McpOperationBase::finished);  // 记录最终状态信号
    if (!operation->isFinished() && !finishedSpy.wait(timeoutMs)) {
        return false;
    }
    if (operation->status() == McpOperationBase::Status::Succeeded) {
        result = McpResult<T>::success(*operation->result());
    } else {
        result = McpResult<T>::failure(operation->error());
    }
    return true;
}

bool waitForOperation(
    const QSharedPointer<McpOperation<void>>& operation,  // 需要等待完成的公共无值操作
    McpResult<void>& result,                              // 接收操作的最终状态
    int timeoutMs)                                        // 最长等待时间，单位为毫秒
{                                                         // 在当前事件循环等待无值 Operation
    QSignalSpy finishedSpy(operation.data(), &McpOperationBase::finished);  // 记录最终状态信号
    if (!operation->isFinished() && !finishedSpy.wait(timeoutMs)) {
        return false;
    }
    result = operation->status() == McpOperationBase::Status::Succeeded
                 ? McpResult<void>::success()
                 : McpResult<void>::failure(operation->error());
    return true;
}

// 验证 LibMcp 的配置持久化与内存传输协议主流程。
class LibMcpTest final : public QObject
{
    Q_OBJECT

private slots:
    void managerSerialization();  // 验证客户端配置的序列化与反序列化
    void managerRejectsLegacyProtocol();  // 验证 HTTP 可达但旧协议的 Client 不会进入 Ready
    void rejectsInvalidToolSchema();  // 验证工具注册拒绝非法 JSON Schema 2020-12
    void wireMetadataAndDiscover();   // 验证请求级元数据校验与 Server 能力发现
    void statelessHttpProtocol();     // 验证无 Session 的独立 HTTP 请求往返
    void stdioProtocol();             // 验证子进程 STDIO 的逐行 JSON 请求往返
    void inMemoryProtocol();       // 验证工具、资源和提示词的内存协议交互
};

void LibMcpTest::managerRejectsLegacyProtocol()  // 验证 HTTP 可达但旧协议的 Client 不会进入 Ready
{
    LegacyProtocolHttpServer legacyServer;  // 模拟高德当前 server/discover 行为
    QVERIFY(legacyServer.start());
    McpClientManager manager;  // 执行完整启用与版本验证流程
    McpClientConfig config;  // 指向本地旧协议模拟 Server 的 Client 配置
    config.id = QStringLiteral("legacy-http");
    config.name = QStringLiteral("Legacy HTTP");
    config.transportType = QStringLiteral("streamable-http");
    config.transportConfig = {
        {QStringLiteral("url"),
         QStringLiteral("http://127.0.0.1:%1/mcp").arg(legacyServer.port())}};
    QVERIFY(manager.addConfig(config));

    McpResult<void> result = McpResult<void>::success();  // 接收完整启用流程的最终结果
    QVERIFY(waitForOperation(manager.startClient(config.id), result, 5000));
    QVERIFY(result.isError());
    QCOMPARE(result.error().code, McpErrorCode::UnsupportedProtocolVersion);
    QCOMPARE(manager.clientState(config.id), McpClientManager::ClientState::Error);
    QCOMPARE(manager.clientTransportState(config.id),
             McpClientManager::TransportState::Reachable);
    QCOMPARE(manager.clientProtocolState(config.id),
             McpClientManager::ProtocolState::Incompatible);
    QVERIFY(manager.clientEnabled(config.id));
    QVERIFY(manager.clientTools(config.id).isEmpty());
    QVERIFY(manager.clientResources(config.id).isEmpty());
    QVERIFY(manager.clientResourceTemplates(config.id).isEmpty());
    QVERIFY(manager.clientPrompts(config.id).isEmpty());
    QVERIFY(manager.clientDiscovery(config.id).supportedVersions.isEmpty());
    QTRY_VERIFY_WITH_TIMEOUT(!manager.client(config.id)->isRunning(), 3000);

    McpResult<void> stopResult = McpResult<void>::failure({});  // 接收用户关闭启用开关的结果
    QVERIFY(waitForOperation(manager.stopClient(config.id), stopResult, 3000));
    QVERIFY(stopResult.isSuccess());
    QCOMPARE(manager.clientState(config.id), McpClientManager::ClientState::Disabled);
    QCOMPARE(manager.clientTransportState(config.id),
             McpClientManager::TransportState::Stopped);
    QCOMPARE(manager.clientProtocolState(config.id),
             McpClientManager::ProtocolState::NotChecked);
    QVERIFY(!manager.clientEnabled(config.id));
}

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

void LibMcpTest::rejectsInvalidToolSchema()  // 验证工具注册拒绝非法 JSON Schema 2020-12
{
    auto transportPair = createInMemoryTransportPair();  // 提供构造 Server 所需的内存 Transport
    McpServer server(                                    // 接收待验证工具注册请求的 Server
        std::move(transportPair.second),
        {QStringLiteral("schema-test-server"),
         QStringLiteral("1.0"),
         QStringLiteral("Schema Test Server")});
    McpTool invalidTool;  // 保存 type 关键字类型错误的非法 Schema
    invalidTool.name = QStringLiteral("invalid-schema");
    invalidTool.inputSchema = {{QStringLiteral("type"), 42}};

    QVERIFY(!server.addTool(
        invalidTool,
        [](const McpToolCallRequest& request,
           const McpRequestContext&) {  // 提供不会因非法 Schema 而被调用的占位实现
            McpToolCallResult result;  // 保存占位工具的结构化结果
            result.structuredContent = request.arguments;
            return result;
        }));
}

void LibMcpTest::wireMetadataAndDiscover()  // 验证请求级元数据校验与 Server 能力发现
{
    auto transportPair = createInMemoryTransportPair();  // 创建可直接发送线协议消息的内存传输
    InMemoryClientTransport* clientTransport =  // 保留 Client 端传输以发送原始请求
        transportPair.first.get();
    McpServer server(  // 提供待发现和校验的无状态 Server
        std::move(transportPair.second),
        {QStringLiteral("discover-server"),
         QStringLiteral("1.0"),
         QStringLiteral("Discover Server")});
    QSignalSpy responseSpy(  // 记录 Server 返回的原始 JSON-RPC 消息
        clientTransport,
        &McpClientTransport::messageReceived);
    constexpr int timeoutMs = 5000;  // 单次异步操作的最长等待时间
    McpResult<void> lifecycleResult =  // 保存传输和 Server 生命周期操作结果
        McpResult<void>::failure({});

    QVERIFY(waitForFuture(server.start(), lifecycleResult, timeoutMs));
    QVERIFY(lifecycleResult.isSuccess());
    QVERIFY(waitForFuture(clientTransport->start(), lifecycleResult, timeoutMs));
    QVERIFY(lifecycleResult.isSuccess());

    const QJsonObject validMeta{  // 描述符合 2026-07-28 的完整请求级元数据
        {QStringLiteral("io.modelcontextprotocol/protocolVersion"),
         QStringLiteral(LIBMCP_PROTOCOL_VERSION)},
        {QStringLiteral("io.modelcontextprotocol/clientCapabilities"),
         QJsonObject{}},
        {QStringLiteral("io.modelcontextprotocol/clientInfo"),
         QJsonObject{{QStringLiteral("name"), QStringLiteral("wire-test")},
                     {QStringLiteral("version"), QStringLiteral("1.0")}}}};
    const QJsonObject discoverRequest{  // 构造无需初始化握手即可发送的能力发现请求
        {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
        {QStringLiteral("id"), QStringLiteral("discover")},
        {QStringLiteral("method"), QStringLiteral("server/discover")},
        {QStringLiteral("params"),
         QJsonObject{{QStringLiteral("_meta"), validMeta}}}};
    McpResult<void> sendResult =  // 保存原始发现请求的发送结果
        McpResult<void>::failure({});
    QVERIFY(waitForFuture(
        clientTransport->sendMessage(discoverRequest),
        sendResult,
        timeoutMs));
    QVERIFY(sendResult.isSuccess());
    QVERIFY(responseSpy.wait(timeoutMs));

    const QJsonObject discoverResponse =  // 提取并检查 Server 的能力发现响应
        qvariant_cast<QJsonObject>(responseSpy.takeFirst().at(0));
    const QJsonObject discoverResult =  // 提取能力发现的业务结果
        discoverResponse.value(QStringLiteral("result")).toObject();
    QCOMPARE(discoverResult.value(QStringLiteral("resultType")).toString(),
             QStringLiteral("complete"));
    QCOMPARE(discoverResult.value(QStringLiteral("supportedVersions"))
                 .toArray()
                 .at(0)
                 .toString(),
             QStringLiteral(LIBMCP_PROTOCOL_VERSION));
    QVERIFY(discoverResult.value(QStringLiteral("capabilities")).isObject());

    QJsonObject unsupportedMeta = validMeta;  // 构造携带不受支持版本的请求元数据
    unsupportedMeta.insert(
        QStringLiteral("io.modelcontextprotocol/protocolVersion"),
        QStringLiteral("2025-11-25"));
    const QJsonObject unsupportedRequest{  // 构造必须被 -32022 拒绝的发现请求
        {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
        {QStringLiteral("id"), QStringLiteral("unsupported")},
        {QStringLiteral("method"), QStringLiteral("server/discover")},
        {QStringLiteral("params"),
         QJsonObject{{QStringLiteral("_meta"), unsupportedMeta}}}};
    QVERIFY(waitForFuture(
        clientTransport->sendMessage(unsupportedRequest),
        sendResult,
        timeoutMs));
    QVERIFY(sendResult.isSuccess());
    QVERIFY(responseSpy.wait(timeoutMs));

    const QJsonObject unsupportedResponse =  // 提取版本拒绝响应
        qvariant_cast<QJsonObject>(responseSpy.takeFirst().at(0));
    const QJsonObject unsupportedError =  // 提取版本拒绝的 JSON-RPC 错误对象
        unsupportedResponse.value(QStringLiteral("error")).toObject();
    QCOMPARE(unsupportedError.value(QStringLiteral("code")).toInt(), -32022);
    QCOMPARE(unsupportedError.value(QStringLiteral("data"))
                 .toObject()
                 .value(QStringLiteral("requested"))
                 .toString(),
             QStringLiteral("2025-11-25"));

    QJsonObject missingCapabilitiesMeta = validMeta;  // 构造缺少必填客户端能力的请求元数据
    missingCapabilitiesMeta.remove(
        QStringLiteral("io.modelcontextprotocol/clientCapabilities"));
    const QJsonObject missingCapabilitiesRequest{  // 构造必须被参数校验拒绝的请求
        {QStringLiteral("jsonrpc"), QStringLiteral("2.0")},
        {QStringLiteral("id"), QStringLiteral("missing-capabilities")},
        {QStringLiteral("method"), QStringLiteral("server/discover")},
        {QStringLiteral("params"),
         QJsonObject{{QStringLiteral("_meta"), missingCapabilitiesMeta}}}};
    QVERIFY(waitForFuture(
        clientTransport->sendMessage(missingCapabilitiesRequest),
        sendResult,
        timeoutMs));
    QVERIFY(sendResult.isSuccess());
    QVERIFY(responseSpy.wait(timeoutMs));

    const QJsonObject missingCapabilitiesResponse =  // 提取必填能力缺失响应
        qvariant_cast<QJsonObject>(responseSpy.takeFirst().at(0));
    QCOMPARE(missingCapabilitiesResponse.value(QStringLiteral("error"))
                 .toObject()
                 .value(QStringLiteral("code"))
                 .toInt(),
             -32600);

    QVERIFY(waitForFuture(clientTransport->stop(), lifecycleResult, timeoutMs));
    QVERIFY(lifecycleResult.isSuccess());
    QVERIFY(waitForFuture(server.stop(), lifecycleResult, timeoutMs));
    QVERIFY(lifecycleResult.isSuccess());
}

void LibMcpTest::statelessHttpProtocol()  // 验证无 Session 的独立 HTTP 请求往返
{
    auto serverTransport =  // 创建由操作系统自动分配端口的 HTTP Server Transport
        std::make_unique<StreamableHttpServerTransport>(
            QHostAddress::LocalHost,
            0,
            QStringLiteral("/mcp"));
    StreamableHttpServerTransport* serverTransportView =  // 保留只读监听信息访问指针
        serverTransport.get();
    McpServer server(  // 提供两个独立 HTTP 请求需要访问的无状态 Server
        std::move(serverTransport),
        {QStringLiteral("http-test-server"),
         QStringLiteral("1.0"),
         QStringLiteral("HTTP Test Server")});
    McpTool progressTool;  // 描述通过请求级 SSE 报告进度的 HTTP 工具
    progressTool.name = QStringLiteral("progress");
    progressTool.inputSchema = {{QStringLiteral("type"), QStringLiteral("object")}};
    QVERIFY(server.addTool(
        progressTool,
        [](const McpToolCallRequest&,
           const McpRequestContext& context) {  // 在最终结果前发送一次进度事件
            context.reportProgress(1.0, 1.0, QStringLiteral("done"));
            McpToolCallResult result;  // 保存用于结束 SSE 响应的工具结果
            result.content = QJsonArray{QJsonObject{
                {QStringLiteral("type"), QStringLiteral("text")},
                {QStringLiteral("text"), QStringLiteral("done")}}};
            return result;
        }));
    constexpr int timeoutMs = 5000;  // 单个异步操作的最长等待时间
    McpResult<void> lifecycleResult =  // 保存 Client 与 Server 生命周期操作结果
        McpResult<void>::failure({});
    QVERIFY(waitForFuture(server.start(), lifecycleResult, timeoutMs));
    QVERIFY(lifecycleResult.isSuccess());
    QVERIFY(serverTransportView->port() != 0);

    const QUrl endpoint(  // 使用 Server 实际监听端口构造 Client Endpoint
        QStringLiteral("http://127.0.0.1:%1/mcp")
            .arg(serverTransportView->port()));
    McpClient client(  // 通过 Streamable HTTP 访问无状态 Server
        std::make_unique<StreamableHttpClientTransport>(endpoint),
        {QStringLiteral("http-test-client"), QStringLiteral("1.0")});
    QVERIFY(waitForOperation(client.start(), lifecycleResult, timeoutMs));
    QVERIFY(lifecycleResult.isSuccess());

    McpResult<QList<McpTool>> firstResult =  // 保存第一次独立 HTTP 工具列表结果
        McpResult<QList<McpTool>>::failure({});
    McpResult<QList<McpTool>> secondResult =  // 保存第二次独立 HTTP 工具列表结果
        McpResult<QList<McpTool>>::failure({});
    QVERIFY(waitForOperation(client.listTools(), firstResult, timeoutMs));
    QVERIFY(firstResult.isSuccess());
    QVERIFY(waitForOperation(client.listTools(), secondResult, timeoutMs));
    QVERIFY(secondResult.isSuccess());

    const auto progressOperation =  // 发起会切换为请求级 SSE 的工具调用
        client.callTool(QStringLiteral("progress"));
    QSignalSpy progressSpy(progressOperation.data(), &McpOperationBase::progressChanged);  // 记录 HTTP SSE 进度事件
    McpResult<McpToolCallResult> progressResult =  // 保存 SSE 流末尾的最终工具结果
        McpResult<McpToolCallResult>::failure({});
    QVERIFY(waitForOperation(progressOperation, progressResult, timeoutMs));
    QVERIFY(progressResult.isSuccess());
    QCOMPARE(progressSpy.count(), 1);

    QSignalSpy notificationSpy(&client, &McpClient::notificationReceived);  // 记录订阅确认和列表变更通知
    McpSubscriptionFilter filter;  // 只选择工具列表变更通知
    filter.toolsListChanged = true;
    const auto subscription = client.listen(filter);  // 打开使用长寿命 HTTP SSE 的订阅
    QTRY_VERIFY_WITH_TIMEOUT(notificationSpy.count() >= 1, timeoutMs);
    QCOMPARE(notificationSpy.at(0).at(0).toString(),
             QStringLiteral("notifications/subscriptions/acknowledged"));
    McpTool addedTool;  // 在 Server 运行中新增工具以触发订阅通知
    addedTool.name = QStringLiteral("added-after-listen");
    addedTool.inputSchema = {{QStringLiteral("type"), QStringLiteral("object")}};
    QVERIFY(server.addTool(
        addedTool,
        [](const McpToolCallRequest&,
           const McpRequestContext&) {  // 为列表变更测试提供最小工具实现
            McpToolCallResult result;  // 保存未实际调用的占位结果
            result.content = QJsonArray{};
            return result;
        }));
    QTRY_VERIFY_WITH_TIMEOUT(notificationSpy.count() >= 2, timeoutMs);
    QCOMPARE(notificationSpy.at(1).at(0).toString(),
             QStringLiteral("notifications/tools/list_changed"));
    subscription->cancel();
    QCOMPARE(subscription->status(), McpOperationBase::Status::Cancelled);

    QVERIFY(waitForOperation(client.stop(), lifecycleResult, timeoutMs));
    QVERIFY(lifecycleResult.isSuccess());
    QVERIFY(waitForFuture(server.stop(), lifecycleResult, timeoutMs));
    QVERIFY(lifecycleResult.isSuccess());
}

void LibMcpTest::stdioProtocol()  // 验证子进程 STDIO 的逐行 JSON 请求往返
{
    StdioClientConfig config;  // 配置测试专用的最小 STDIO Server 子进程
    config.command = QStringLiteral(LIBMCP_STDIO_TEST_SERVER);
    McpClient client(  // 通过 STDIO Transport 访问测试 Server
        std::make_unique<StdioClientTransport>(config),
        {QStringLiteral("stdio-test-client"), QStringLiteral("1.0")});
    constexpr int timeoutMs = 5000;  // 单个异步操作的最长等待时间
    McpResult<void> lifecycleResult =  // 保存 Client 生命周期操作结果
        McpResult<void>::failure({});
    QVERIFY(waitForOperation(client.start(), lifecycleResult, timeoutMs));
    QVERIFY(lifecycleResult.isSuccess());

    McpResult<QList<McpTool>> toolsResult =  // 保存 STDIO 工具列表查询结果
        McpResult<QList<McpTool>>::failure({});
    QVERIFY(waitForOperation(client.listTools(), toolsResult, timeoutMs));
    QVERIFY(toolsResult.isSuccess());
    QVERIFY(toolsResult.value().isEmpty());

    QVERIFY(waitForOperation(client.stop(), lifecycleResult, timeoutMs));
    QVERIFY(lifecycleResult.isSuccess());
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
        [](const McpToolCallRequest& request,
           const McpRequestContext& context) {  // 报告进度并原样返回结构化输入
            context.reportProgress(1.0, 2.0, QStringLiteral("half"));
            McpToolCallResult result;  // 保存回显工具的完整结果
            result.content =
                QJsonArray{QJsonObject{{QStringLiteral("type"), QStringLiteral("text")},
                                       {QStringLiteral("text"), QStringLiteral("echo")}}};
            result.structuredContent = request.arguments;
            return result;
        }));

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
        [](const McpToolCallRequest& request,
           const McpRequestContext&) {  // 返回通过 Schema 校验的工具输入
            McpToolCallResult result;  // 保存通过校验的结构化输入
            result.structuredContent = request.arguments;
            return result;
        }));

    McpTool approvalTool;  // 描述需要 Client 补充表单输入的多轮工具
    approvalTool.name = QStringLiteral("approval");
    approvalTool.inputSchema = {{QStringLiteral("type"), QStringLiteral("object")}};
    QVERIFY(server.addTool(
        approvalTool,
        [](const McpToolCallRequest& request,
           const McpRequestContext&) {  // 首轮请求用户确认，重试时返回最终结果
            McpToolCallResult result;  // 保存当前轮次的 MRTR 结果
            if (request.inputResponses.isEmpty()) {
                result.inputRequests = {
                    {QStringLiteral("approval-form"),
                     QJsonObject{
                         {QStringLiteral("method"), QStringLiteral("elicitation/create")},
                         {QStringLiteral("params"),
                          QJsonObject{
                              {QStringLiteral("mode"), QStringLiteral("form")},
                              {QStringLiteral("message"), QStringLiteral("请确认")},
                              {QStringLiteral("requestedSchema"),
                               QJsonObject{
                                   {QStringLiteral("type"), QStringLiteral("object")},
                                   {QStringLiteral("properties"), QJsonObject{}}}}}}}}};
                result.requestState = QStringLiteral("approval-state");
                return result;
            }
            result.content = QJsonArray{QJsonObject{
                {QStringLiteral("type"), QStringLiteral("text")},
                {QStringLiteral("text"), QStringLiteral("approved")}}};
            return result;
        }));

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

    server.setCompletionHandler(
        [](const McpCompletionRequest& request) {  // 根据当前输入前缀生成确定的测试候选值
            return McpCompletionResult{
                QStringList{request.argumentValue + QStringLiteral("-one"),
                            request.argumentValue + QStringLiteral("-two")},
                2,
                false};
        });

    McpClient client(  // 通过内存传输访问测试服务端
        std::move(transportPair.first),
        {QStringLiteral("test-client"), QStringLiteral("1.0")},
        QJsonObject{{QStringLiteral("elicitation"),
                     QJsonObject{{QStringLiteral("form"), QJsonObject{}}}}});
    QSignalSpy requestSpy(  // 记录 Server 实际分发的协议请求
        &server,
        &McpServer::requestHandled);

    constexpr int timeoutMs = 5000;   // 单个异步操作的最长等待时间
    McpResult<void> lifecycleResult =  // 保存启动和停止操作结果
        McpResult<void>::failure({});
    QVERIFY(waitForFuture(server.start(), lifecycleResult, timeoutMs));
    QVERIFY(lifecycleResult.isSuccess());
    QVERIFY(waitForOperation(client.start(), lifecycleResult, timeoutMs));
    QVERIFY(lifecycleResult.isSuccess());
    QCOMPARE(requestSpy.count(), 0);  // 启动 Client 不应产生旧 initialize 握手

    McpResult<McpDiscoveryResult> discoveryResult =  // 保存显式 Server 能力发现结果
        McpResult<McpDiscoveryResult>::failure({});
    QVERIFY(waitForOperation(client.discover(), discoveryResult, timeoutMs));
    QVERIFY(discoveryResult.isSuccess());
    QCOMPARE(discoveryResult.value().supportedVersions,
             QStringList{QStringLiteral(LIBMCP_PROTOCOL_VERSION)});
    QVERIFY(discoveryResult.value().capabilities.contains(QStringLiteral("tools")));
    QCOMPARE(client.serverInfo().name, QStringLiteral("test-server"));

    McpResult<QList<McpTool>> toolsResult =  // 保存工具列表查询结果
        McpResult<QList<McpTool>>::failure({});
    QVERIFY(waitForOperation(client.listTools(), toolsResult, timeoutMs));
    QVERIFY(toolsResult.isSuccess());
    QCOMPARE(toolsResult.value().size(), 3);

    McpResult<McpToolCallResult> toolCallResult =  // 保存完整工具调用结果
        McpResult<McpToolCallResult>::failure({});
    const auto echoOperation =  // 保存同时产生进度事件的工具调用
        client.callTool(
            QStringLiteral("echo"),
            QJsonObject{{QStringLiteral("value"), 42}});
    QSignalSpy progressSpy(echoOperation.data(), &McpOperationBase::progressChanged);  // 记录按 Token 路由的请求内进度
    QVERIFY(waitForOperation(echoOperation, toolCallResult, timeoutMs));
    QVERIFY(toolCallResult.isSuccess());
    QCOMPARE(toolCallResult.value().content.size(), 1);
    QCOMPARE(toolCallResult.value().structuredContent.toObject()
                 .value(QStringLiteral("value"))
                 .toInt(),
             42);
    QCOMPARE(progressSpy.count(), 1);
    QCOMPARE(progressSpy.first().at(0).toDouble(), 1.0);

    McpResult<McpToolCallResult> invalidToolResult =  // 保存无效工具输入的校验结果
        McpResult<McpToolCallResult>::failure({});
    QVERIFY(waitForOperation(
        client.callTool(
            QStringLiteral("required-value"),
            QJsonObject{}),
        invalidToolResult,
        timeoutMs));
    QVERIFY(invalidToolResult.isError());

    const auto inputOperation =  // 保存首轮需要补充输入的工具操作
        client.callTool(QStringLiteral("approval"));
    QSignalSpy inputSpy(inputOperation.data(), &McpOperationBase::inputRequired);  // 记录向调用方暴露的输入需求
    McpResult<McpToolCallResult> inputResult =  // 保存首轮 input_required 结果
        McpResult<McpToolCallResult>::failure({});
    QVERIFY(waitForOperation(inputOperation, inputResult, timeoutMs));
    QVERIFY(inputResult.isSuccess());
    QCOMPARE(inputResult.value().requestState, QStringLiteral("approval-state"));
    QCOMPARE(inputResult.value().inputRequests.size(), 1);
    QCOMPARE(inputSpy.count(), 1);

    McpResult<McpToolCallResult> retryResult =  // 保存回传用户输入后的最终工具结果
        McpResult<McpToolCallResult>::failure({});
    QVERIFY(waitForOperation(
        client.callTool(
            QStringLiteral("approval"),
            {},
            inputResult.value().requestState,
            QJsonObject{{QStringLiteral("approval-form"),
                         QJsonObject{{QStringLiteral("action"),
                                      QStringLiteral("accept")},
                                     {QStringLiteral("content"), QJsonObject{}}}}}),
        retryResult,
        timeoutMs));
    QVERIFY(retryResult.isSuccess());
    QCOMPARE(retryResult.value().content.size(), 1);

    McpResult<QList<McpResource>> resourcesResult =  // 保存资源列表查询结果
        McpResult<QList<McpResource>>::failure({});
    QVERIFY(waitForOperation(client.listResources(), resourcesResult, timeoutMs));
    QVERIFY(resourcesResult.isSuccess());
    QCOMPARE(resourcesResult.value().size(), 1);

    McpResult<QList<McpResourceContent>> contentsResult =  // 保存固定资源读取结果
        McpResult<QList<McpResourceContent>>::failure({});
    QVERIFY(waitForOperation(
        client.readResource(QStringLiteral("test://status")),
        contentsResult,
        timeoutMs));
    QVERIFY(contentsResult.isSuccess());
    QCOMPARE(contentsResult.value().size(), 1);

    McpResult<QList<McpResourceTemplate>> templatesResult =  // 保存资源模板列表查询结果
        McpResult<QList<McpResourceTemplate>>::failure({});
    QVERIFY(waitForOperation(
        client.listResourceTemplates(),
        templatesResult,
        timeoutMs));
    QVERIFY(templatesResult.isSuccess());
    QCOMPARE(templatesResult.value().size(), 1);

    McpResult<QList<McpResourceContent>> templatedContentsResult =  // 保存模板资源读取结果
        McpResult<QList<McpResourceContent>>::failure({});
    QVERIFY(waitForOperation(
        client.readResource(QStringLiteral("test://users/7")),
        templatedContentsResult,
        timeoutMs));
    QVERIFY(templatedContentsResult.isSuccess());
    QCOMPARE(templatedContentsResult.value().size(), 1);

    McpResult<QList<McpPrompt>> promptsResult =  // 保存提示词列表查询结果
        McpResult<QList<McpPrompt>>::failure({});
    QVERIFY(waitForOperation(client.listPrompts(), promptsResult, timeoutMs));
    QVERIFY(promptsResult.isSuccess());
    QCOMPARE(promptsResult.value().size(), 1);

    McpResult<QList<McpPromptMessage>> promptMessagesResult =  // 保存提示词生成的消息结果
        McpResult<QList<McpPromptMessage>>::failure({});
    QVERIFY(waitForOperation(
        client.getPrompt(
            QStringLiteral("greet"),
            QJsonObject{{QStringLiteral("name"), QStringLiteral("Qt")}}),
        promptMessagesResult,
        timeoutMs));
    QVERIFY(promptMessagesResult.isSuccess());
    QCOMPARE(promptMessagesResult.value().size(), 1);

    McpResult<McpCompletionResult> completionResult =  // 保存参数补全查询结果
        McpResult<McpCompletionResult>::failure({});
    McpCompletionRequest completionRequest;  // 描述 Prompt 参数补全测试请求
    completionRequest.referenceType = McpCompletionReferenceType::Prompt;
    completionRequest.reference = QStringLiteral("greet");
    completionRequest.argumentName = QStringLiteral("name");
    completionRequest.argumentValue = QStringLiteral("q");
    QVERIFY(waitForOperation(
        client.complete(completionRequest),
        completionResult,
        timeoutMs));
    QVERIFY(completionResult.isSuccess());
    QCOMPARE(completionResult.value().values,
             QStringList({QStringLiteral("q-one"), QStringLiteral("q-two")}));

    QVERIFY(waitForOperation(client.stop(), lifecycleResult, timeoutMs));
    QVERIFY(lifecycleResult.isSuccess());
    QVERIFY(waitForFuture(server.stop(), lifecycleResult, timeoutMs));
    QVERIFY(lifecycleResult.isSuccess());
}

QTEST_MAIN(LibMcpTest)

#include "tst_LibMcp.moc"
