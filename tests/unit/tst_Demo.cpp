#include "../../TestApp/LibAiCore/LibAiCorePage.h"
#include "../../TestApp/LogWidget.h"
#include "../../TestApp/MainWindow.h"
#include <QtTest>
#include <QTabWidget>
#if defined(AILIB_TESTAPP_HAS_MCP)
#include "../../TestApp/LibMcp/LibMcpPage.h"
#include "../../TestApp/LibMcp/McpClientConfigDialog.h"
#include <QLineEdit>
#include <QStackedWidget>
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
        QVERIFY(addButton);
        QVERIFY(clientTable);
        QVERIFY(serverStatus);
        QVERIFY(recentRequests);
        QCOMPARE(clientTable->columnCount(), 5);
        QCOMPARE(recentRequests->columnCount(), 5);
        QVERIFY(!page.findChild<QWidget*>(QStringLiteral("connectedClients")));
    }
#endif
};

QTEST_MAIN(DemoTest)
#include "tst_Demo.moc"
