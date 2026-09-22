#include "../../TestApp/LibAiCore/LibAiCorePage.h"
#include "../../TestApp/LogWidget.h"
#include "../../TestApp/MainWindow.h"
#include <QtTest>
#include <QTabWidget>

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
};

QTEST_MAIN(DemoTest)
#include "tst_Demo.moc"
