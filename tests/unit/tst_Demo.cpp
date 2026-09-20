#include "../../TestApp/src/DemoWindow.h"
#include <QtTest>

// 验证 TestApp 从统一目录展示 Provider、模型并在缺少凭据时安全失败。
class DemoTest final : public QObject {
    Q_OBJECT
private slots:
    void catalogPopulatesSelectors()  // 默认展示 DeepSeek 及其目录模型
    {
        DemoWindow window;  // 当前测试窗口
        auto* provider = window.findChild<QComboBox*>(QStringLiteral("provider"));  // Provider 选择框
        auto* model = window.findChild<QComboBox*>(QStringLiteral("model"));        // 模型选择框
        QVERIFY(provider);
        QVERIFY(model);
        QCOMPARE(provider->count(), 3);
        QCOMPARE(provider->currentData().toString(), QStringLiteral("deepseek"));
        QCOMPARE(model->currentText(), QStringLiteral("deepseek-flash"));
    }

    void openaiModels()  // 切换 OpenAI 后展示当前 Responses 模型集合
    {
        DemoWindow window;  // 当前测试窗口
        auto* provider = window.findChild<QComboBox*>(QStringLiteral("provider"));  // Provider 选择框
        auto* model = window.findChild<QComboBox*>(QStringLiteral("model"));        // 模型选择框
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
        DemoWindow window;  // 当前测试窗口
        auto* send = window.findChild<QPushButton*>(QStringLiteral("send"));  // 启动按钮
        auto* status = window.findChild<QLabel*>(QStringLiteral("status"));   // 运行状态
        send->click();
        QTRY_COMPARE_WITH_TIMEOUT(status->text(), QStringLiteral("Failed · MissingApiKey"), 3000);
        QVERIFY(send->isEnabled());
        if (!originalKey.isNull())
            qputenv("DEEPSEEK_API_KEY", originalKey);
    }
};

QTEST_MAIN(DemoTest)
#include "tst_Demo.moc"
