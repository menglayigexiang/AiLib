#include "../../TestApp/LibAiCore/LibAiCorePage.h"
#include "../../TestApp/LogWidget.h"
#include "../../TestApp/MainWindow.h"
#include <QtTest>
#include <QTabWidget>
#if defined(AILIB_TESTAPP_HAS_MCP)
#include "../../TestApp/LibMcp/LibMcpPage.h"
#include "../../TestApp/LibMcp/McpClientConfigDialog.h"
#include "../../TestApp/LibMcp/McpClientWorkbench.h"
#include <LibMcp/McpServer.h>
#include <LibMcp/StreamableHttpTransport.h>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QSplitter>
#include <QStackedWidget>
#include <QTabWidget>
#include <QTableWidget>
#endif

// 验证 TestApp 的 LibAiCore 页面展示 Provider、模型并在缺少凭据时安全失败。
class DemoTest final : public QObject {
    Q_OBJECT
private slots:
    void mergedTabs()  // 统一 TestApp 使用顶层页签承载各模块页面
    {
        auto* logWidget = new LogWidget;  // 由主窗口接管所有权的底部日志控件
        MainWindow window(QStringLiteral("deepseek"), logWidget);  // 当前统一测试应用窗口
        auto* tabs = window.findChild<QTabWidget*>();  // 嵌套在分隔器中的顶层模块页签容器

        QVERIFY(tabs);
        QCOMPARE(tabs->tabText(0), QStringLiteral("LibAiCore"));
#if defined(AILIB_TESTAPP_HAS_MCP)
        QCOMPARE(tabs->count(), 2);
        QCOMPARE(tabs->tabText(1), QStringLiteral("LibMcp"));
#else
        QCOMPARE(tabs->count(), 1);
#endif
    }

    void catalogPopulatesSelectors()  // 默认展示 DeepSeek 及其目录模型
    {
        LibAiCorePage page;  // 当前测试的 LibAiCore 页面
        auto* provider = page.findChild<QComboBox*>(QStringLiteral("provider"));  // Provider 选择框
        auto* model = page.findChild<QComboBox*>(QStringLiteral("model"));        // 模型选择框
        QVERIFY(provider);
        QVERIFY(model);
        QCOMPARE(provider->count(), 3);
        QCOMPARE(provider->currentData().toString(), QStringLiteral("deepseek"));
        QCOMPARE(model->currentText(), QStringLiteral("deepseek-flash"));
    }

    void openaiModels()  // 切换 OpenAI 后展示当前 Responses 模型集合
    {
        LibAiCorePage page;  // 当前测试的 LibAiCore 页面
        auto* provider = page.findChild<QComboBox*>(QStringLiteral("provider"));  // Provider 选择框
        auto* model = page.findChild<QComboBox*>(QStringLiteral("model"));        // 模型选择框
        provider->setCurrentIndex(provider->findData(QStringLiteral("openai")));
        QCOMPARE(model->count(), 3);
        QCOMPARE(model->itemData(0).toString(), QStringLiteral("gpt-5.6-sol"));
        QCOMPARE(model->itemData(1).toString(), QStringLiteral("gpt-5.6-terra"));
        QCOMPARE(model->itemData(2).toString(), QStringLiteral("gpt-5.6-luna"));
    }

    void missingKeyDoesNotSend()  // 未提供凭据时工作线程返回明确错误并恢复 UI
    {
        const QByteArray originalKey = qgetenv("DEEPSEEK_API_KEY");  // 调用方原有环境值
        qunsetenv("DEEPSEEK_API_KEY");
        LibAiCorePage page;  // 当前测试的 LibAiCore 页面
        auto* send = page.findChild<QPushButton*>(QStringLiteral("send"));  // 启动按钮
        auto* status = page.findChild<QLabel*>(QStringLiteral("status"));   // 运行状态
        send->click();
        QTRY_COMPARE_WITH_TIMEOUT(status->text(), QStringLiteral("Failed · MissingApiKey"), 3000);
        QVERIFY(send->isEnabled());
        if (!originalKey.isNull())
            qputenv("DEEPSEEK_API_KEY", originalKey);
    }

#if defined(AILIB_TESTAPP_HAS_MCP)
    void mcpClientDialogSwitchesTransportForms()  // 配置对话框按类型展示 STDIO 或 HTTP 参数
    {
        McpClientConfigDialog dialog;  // 当前测试的新增与编辑共用配置表单
        auto* type = dialog.findChild<QComboBox*>(QStringLiteral("mcpClientTypeCombo"));  // Transport 类型选择框
        auto* pages = dialog.findChild<QStackedWidget*>(QStringLiteral("mcpClientTransportPages"));  // Transport 参数页
        auto* command = dialog.findChild<QLineEdit*>(QStringLiteral("mcpStdioCommandEdit"));  // STDIO 命令输入框
        auto* url = dialog.findChild<QLineEdit*>(QStringLiteral("mcpHttpUrlEdit"));  // HTTP URL 输入框
        QVERIFY(type);
        QVERIFY(pages);
        QVERIFY(command);
        QVERIFY(url);
        QCOMPARE(type->currentData().toString(), QStringLiteral("stdio"));
        QCOMPARE(pages->currentIndex(), 0);
        type->setCurrentIndex(type->findData(QStringLiteral("streamable-http")));
        QCOMPARE(pages->currentIndex(), 1);
    }

    void mcpPageExposesClientAndStatelessServerControls()  // 验证 Client 列表与无状态 Server 观测控件存在
    {
        LibMcpPage page;  // 当前通过控件树验证的 LibMcp 测试页
        auto* addButton =  // 打开 Client 新增对话框的入口
            page.findChild<QPushButton*>(QStringLiteral("addMcpClientButton"));
        auto* clientTable =  // 展示 Client 配置、状态和行内操作的表格
            page.findChild<QTableWidget*>(QStringLiteral("mcpClientTable"));
        auto* serverStatus =  // 展示 Server 最终运行状态的文本
            page.findChild<QLabel*>(QStringLiteral("mcpServerStatusLabel"));
        auto* recentRequests =  // 展示最近请求 ClientInfo 和结果的表格
            page.findChild<QTableWidget*>(QStringLiteral("mcpRecentRequestsTable"));
        auto* tabs = page.findChild<QTabWidget*>(QStringLiteral("mcpTabs"));  // 承载配置、调试和 Server 页面
        auto* discover =  // 发起无状态 Server 能力发现
            page.findChild<QPushButton*>(QStringLiteral("mcpDiscoverButton"));
        auto* loadTools =  // 加载完整工具列表
            page.findChild<QPushButton*>(QStringLiteral("mcpLoadToolsButton"));
        auto* callTool =  // 使用 JSON 参数调用当前工具
            page.findChild<QPushButton*>(QStringLiteral("mcpCallToolButton"));
        auto* arguments =  // 编辑工具参数 JSON 对象
            page.findChild<QPlainTextEdit*>(QStringLiteral("mcpToolArguments"));
        auto* result =  // 展示结构化结果、业务错误或协议错误
            page.findChild<QPlainTextEdit*>(QStringLiteral("mcpToolResultOutput"));
        auto* progress =  // 展示工具调用确定或不确定进度
            page.findChild<QProgressBar*>(QStringLiteral("mcpToolProgress"));
        auto* pageSplitter =  // 允许用户调整发现信息与工具区域高度
            page.findChild<QSplitter*>(QStringLiteral("mcpWorkbenchPageSplitter"));
        auto* contentSplitter =  // 允许用户调整工具列表与详情宽度
            page.findChild<QSplitter*>(QStringLiteral("mcpWorkbenchContentSplitter"));
        auto* detailTabs =  // 分离 Schema 与调用结果以扩大阅读区域
            page.findChild<QTabWidget*>(QStringLiteral("mcpToolDetailTabs"));
        QVERIFY(addButton);
        QVERIFY(clientTable);
        QVERIFY(serverStatus);
        QVERIFY(recentRequests);
        QVERIFY(tabs);
        QVERIFY(discover);
        QVERIFY(loadTools);
        QVERIFY(callTool);
        QVERIFY(arguments);
        QVERIFY(result);
        QVERIFY(progress);
        QVERIFY(pageSplitter);
        QVERIFY(contentSplitter);
        QVERIFY(detailTabs);
        QCOMPARE(pageSplitter->orientation(), Qt::Vertical);
        QCOMPARE(contentSplitter->orientation(), Qt::Horizontal);
        QCOMPARE(detailTabs->count(), 2);
        QCOMPARE(detailTabs->currentIndex(), 1);
        QCOMPARE(tabs->count(), 3);
        QCOMPARE(tabs->tabText(1), QStringLiteral("Client 调试"));
        QVERIFY(!discover->isEnabled());
        QVERIFY(!loadTools->isEnabled());
        QVERIFY(!callTool->isEnabled());
        QCOMPARE(clientTable->columnCount(), 7);
        QCOMPARE(recentRequests->columnCount(), 5);
        QVERIFY(!page.findChild<QWidget*>(QStringLiteral("connectedClients")));
    }

    void mcpWorkbenchCompletesRealHttpFlow()  // 通过真实本地 HTTP 完成发现、工具列表和工具调用
    {
        auto transport = std::make_unique<LibMcp::StreamableHttpServerTransport>(
            QHostAddress::LocalHost,
            0,
            QStringLiteral("/mcp"));  // 使用系统分配端口创建受控测试 Server
        auto* transportView = transport.get();  // 在移交所有权后读取实际监听端口
        LibMcp::McpServer server(
            std::move(transport),
            {QStringLiteral("ui-test-server"),
             QStringLiteral("1.0.0"),
             QStringLiteral("UI Test Server")});  // 提供工作台端到端测试 Server
        LibMcp::McpTool echoTool;  // 声明可使用对象参数调用的回显工具
        echoTool.name = QStringLiteral("echo");
        echoTool.description = QStringLiteral("回显参数");
        echoTool.inputSchema = {{QStringLiteral("type"), QStringLiteral("object")}};
        echoTool.outputSchema = {{QStringLiteral("type"), QStringLiteral("object")}};
        QVERIFY(server.addTool(
            echoTool,
            [](const LibMcp::McpToolCallRequest& request,
               const LibMcp::McpRequestContext&) {  // 返回结构化参数供界面断言
                LibMcp::McpToolCallResult result;  // 保存工具调用的结构化成功结果
                result.structuredContent = request.arguments;
                result.content = QJsonArray{QJsonObject{
                    {QStringLiteral("type"), QStringLiteral("text")},
                    {QStringLiteral("text"), QStringLiteral("ok")}}};
                return result;
            }));
        LibMcp::McpResource resource;  // 声明由 Manager 自动加载的固定资源
        resource.name = QStringLiteral("status");
        resource.uri = QStringLiteral("test://status");
        QVERIFY(server.addResource(
            resource,
            [](const QString& uri) {  // 返回固定文本资源供能力缓存断言
                return QList<LibMcp::McpResourceContent>{LibMcp::McpTextResourceContent{
                    uri, QStringLiteral("text/plain"), QStringLiteral("running"), {}}};
            }));
        LibMcp::McpResourceTemplate resourceTemplate;  // 声明由 Manager 自动加载的资源模板
        resourceTemplate.name = QStringLiteral("user");
        resourceTemplate.uriTemplate = QStringLiteral("test://users/{id}");
        QVERIFY(server.addResourceTemplate(
            resourceTemplate,
            [](const QString& uri) {  // 返回模板资源供协议方法保持可调用
                return QList<LibMcp::McpResourceContent>{LibMcp::McpTextResourceContent{
                    uri, QStringLiteral("text/plain"), QStringLiteral("user"), {}}};
            }));
        LibMcp::McpPrompt prompt;  // 声明由 Manager 自动加载的 Prompt
        prompt.name = QStringLiteral("greet");
        QVERIFY(server.addPrompt(
            prompt,
            [](const QJsonObject&) {  // 返回固定 Prompt 消息供协议方法保持可调用
                return QList<LibMcp::McpPromptMessage>{LibMcp::McpPromptMessage{
                    LibMcp::McpRole::User,
                    QJsonObject{{QStringLiteral("type"), QStringLiteral("text")},
                                {QStringLiteral("text"), QStringLiteral("hello")}}}};
            }));
        const QFuture<LibMcp::McpResult<void>> serverStart = server.start();  // 启动本地 HTTP 监听
        QTRY_VERIFY_WITH_TIMEOUT(serverStart.isFinished(), 3000);
        QVERIFY(serverStart.result().isSuccess());

        LibMcp::McpClientManager manager;  // 管理工作台使用的本地 Client
        LibMcp::McpClientConfig config;  // 描述连接到受控测试 Server 的 HTTP 配置
        config.id = QStringLiteral("ui-http-test");
        config.name = QStringLiteral("UI HTTP Test");
        config.transportType = QStringLiteral("streamable-http");
        config.transportConfig = {
            {QStringLiteral("url"),
             QStringLiteral("http://127.0.0.1:%1/mcp")
                 .arg(transportView->port())}};
        QVERIFY(manager.addConfig(config));
        const auto clientStart = manager.startClient(config.id);  // 启动工作台使用的 Client Transport
        QTRY_VERIFY_WITH_TIMEOUT(clientStart->isFinished(), 3000);
        QCOMPARE(clientStart->status(), LibMcp::McpOperationBase::Status::Succeeded);
        QCOMPARE(manager.clientState(config.id),
                 LibMcp::McpClientManager::ClientState::Ready);
        QCOMPARE(manager.clientTransportState(config.id),
                 LibMcp::McpClientManager::TransportState::Reachable);
        QCOMPARE(manager.clientProtocolState(config.id),
                 LibMcp::McpClientManager::ProtocolState::Compatible);
        QVERIFY(manager.clientEnabled(config.id));
        QCOMPARE(manager.clientTools(config.id).size(), 1);
        QCOMPARE(manager.clientResources(config.id).size(), 1);
        QCOMPARE(manager.clientResourceTemplates(config.id).size(), 1);
        QCOMPARE(manager.clientPrompts(config.id).size(), 1);
        QVERIFY(manager.clientDiscovery(config.id).capabilities.contains(
            QStringLiteral("resources")));
        QVERIFY(manager.clientDiscovery(config.id).capabilities.contains(
            QStringLiteral("prompts")));

        McpClientWorkbench workbench(&manager);  // 创建需要实际操作的 Client 调试工作台
        auto* discover = workbench.findChild<QPushButton*>(QStringLiteral("mcpDiscoverButton"));  // 能力发现按钮
        auto* discoveryOutput = workbench.findChild<QPlainTextEdit*>(QStringLiteral("mcpDiscoveryOutput"));  // 能力发现输出
        auto* loadTools = workbench.findChild<QPushButton*>(QStringLiteral("mcpLoadToolsButton"));  // 工具列表按钮
        auto* connectionState = workbench.findChild<QLabel*>(QStringLiteral("mcpWorkbenchConnectionState"));  // 当前协议操作状态
        auto* toolTable = workbench.findChild<QTableWidget*>(QStringLiteral("mcpToolTable"));  // 工具列表表格
        auto* arguments = workbench.findChild<QPlainTextEdit*>(QStringLiteral("mcpToolArguments"));  // 工具参数编辑器
        auto* callTool = workbench.findChild<QPushButton*>(QStringLiteral("mcpCallToolButton"));  // 工具调用按钮
        auto* resultOutput = workbench.findChild<QPlainTextEdit*>(QStringLiteral("mcpToolResultOutput"));  // 工具结果输出
        QVERIFY(discover);
        QVERIFY(discoveryOutput);
        QVERIFY(loadTools);
        QVERIFY(connectionState);
        QVERIFY(toolTable);
        QVERIFY(arguments);
        QVERIFY(callTool);
        QVERIFY(resultOutput);

        QVERIFY(discoveryOutput->toPlainText().contains(QStringLiteral("2026-07-28")));
        QCOMPARE(toolTable->rowCount(), 1);
        QCOMPARE(toolTable->item(0, 0)->text(), QStringLiteral("echo"));
        discover->click();
        QTRY_VERIFY_WITH_TIMEOUT(
            connectionState->text() == QStringLiteral("协议发现成功"),
            5000);
        toolTable->selectRow(0);
        QTest::mouseClick(toolTable->viewport(),
                          Qt::LeftButton,
                          Qt::NoModifier,
                          toolTable->visualItemRect(toolTable->item(0, 0)).center());
        arguments->setPlainText(QStringLiteral("{\"city\":\"北京\"}"));
        callTool->click();
        QTRY_VERIFY_WITH_TIMEOUT(
            resultOutput->toPlainText().contains(QStringLiteral("北京")),
            5000);

        const auto clientStop = manager.stopClient(config.id);  // 正常停止测试 Client
        QTRY_VERIFY_WITH_TIMEOUT(clientStop->isFinished(), 3000);
        const QFuture<LibMcp::McpResult<void>> serverStop = server.stop();  // 正常停止测试 Server
        QTRY_VERIFY_WITH_TIMEOUT(serverStop.isFinished(), 3000);
    }
#endif
};

QTEST_MAIN(DemoTest)
#include "tst_Demo.moc"
