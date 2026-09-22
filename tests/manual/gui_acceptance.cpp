#include "../../TestApp/LibAiCore/LibAiCorePage.h"
#include "../../examples/support/DemoSupport.h"
#include <QtTest>
#include <QMessageBox>

// 可选真实 GUI 代码验收，直接断言控件和同步流程，不进行截图或外部 UI 自动操作。
class LiveGuiTest final : public QObject {
    Q_OBJECT
private slots:
    void approval_data()  // 两种真实 Provider 的允许和拒绝闭环
    {
        QTest::addColumn<QString>("provider");
        QTest::addColumn<bool>("allow");
        QTest::newRow("deepseek-allow") << QStringLiteral("deepseek") << true;
        QTest::newRow("deepseek-deny") << QStringLiteral("deepseek") << false;
        QTest::newRow("kimi-allow") << QStringLiteral("kimi") << true;
        QTest::newRow("kimi-deny") << QStringLiteral("kimi") << false;
    }
    void approval()  // 在应用线程操作真实确认接口，并断言模型后续回答
    {
        QFETCH(QString, provider);                                   // 本次真实服务
        QFETCH(bool, allow);                                         // 本次同步确认决定
        LibAiCorePage page(provider);                               // 应用拥有的 UI 和运行依赖
        page.findChild<QLineEdit*>("apiKey")->setText(Demo::demoApiKeyFromEnv(provider));
        auto* output = page.findChild<QPlainTextEdit*>("output");  // 模型与工具输出
        auto* status = page.findChild<QLabel*>("status");          // 最终停止状态
        page.findChild<QPushButton*>("send")->click();
        QTRY_VERIFY_WITH_TIMEOUT(!page.findChildren<QMessageBox*>().isEmpty() ||
                                 page.findChild<QLabel*>("status")->text() != "Running", 20000);
        QVERIFY2(!page.findChildren<QMessageBox*>().isEmpty(),
                 qPrintable(page.findChild<QLabel*>("status")->text()));
        auto* box = page.findChildren<QMessageBox*>().first();  // 已实际触发的工具确认
        QVERIFY(box->text().contains("widgets-agent"));
        QVERIFY(box->text().contains("add"));
        QVERIFY(!box->isModal());
        box->button(allow ? QMessageBox::Yes : QMessageBox::No)->click();
        QTRY_COMPARE_WITH_TIMEOUT(status->text(), QStringLiteral("Completed"), 30000);
        QVERIFY(output->toPlainText().contains(allow ? QStringLiteral("Handler=已执行")
                                                     : QStringLiteral("Handler=未执行")));
        QVERIFY(output->toPlainText().contains(allow ? QStringLiteral("42")
                                                     : QStringLiteral("ApprovalDenied")));
    }
    void approvalStop_data()  // 两种服务在实际确认等待期间取消
    {
        QTest::addColumn<QString>("provider");
        QTest::newRow("deepseek") << QStringLiteral("deepseek");
        QTest::newRow("kimi") << QStringLiteral("kimi");
    }
    void approvalStop()  // 实际确认等待中点击 Stop，验证 UI 桥接使用同一取消令牌
    {
        QFETCH(QString, provider);  // 当前真实服务
        LibAiCorePage page(provider);  // 当前真实应用运行依赖
        page.findChild<QLineEdit*>("apiKey")->setText(Demo::demoApiKeyFromEnv(provider));
        page.findChild<QPushButton*>("send")->click();
        QTRY_VERIFY_WITH_TIMEOUT(!page.findChildren<QMessageBox*>().isEmpty() ||
                                 page.findChild<QLabel*>("status")->text() != "Running", 20000);
        QVERIFY2(!page.findChildren<QMessageBox*>().isEmpty(),
                 qPrintable(page.findChild<QLabel*>("status")->text()));
        page.findChild<QPushButton*>("stop")->click();
        QTRY_COMPARE_WITH_TIMEOUT(page.findChild<QLabel*>("status")->text(), QStringLiteral("Cancelled"), 5000);
        QVERIFY(page.findChild<QPlainTextEdit*>("output")->toPlainText().contains(QStringLiteral("Handler=未执行")));
        QTRY_VERIFY_WITH_TIMEOUT(page.findChildren<QMessageBox*>().isEmpty(), 1000);
    }
    void streamStop_data()  // 两种服务在有效文字输出后取消
    {
        QTest::addColumn<QString>("provider");
        QTest::newRow("deepseek") << QStringLiteral("deepseek");
        QTest::newRow("kimi") << QStringLiteral("kimi");
    }
    void streamStop()  // UI 线程请求 Stop，不通过截图判断部分文字与取消状态
    {
        QFETCH(QString, provider);    // 本次真实服务
        LibAiCorePage page(provider);  // 当前应用运行依赖
        page.findChild<QLineEdit*>("apiKey")->setText(Demo::demoApiKeyFromEnv(provider));
        page.findChild<QCheckBox*>("tools")->setChecked(false);
        page.findChild<QLineEdit*>("input")->setText(
            QStringLiteral("请详细介绍 C++17 的十个特性，每个特性至少五句话。"));
        auto* output = page.findChild<QPlainTextEdit*>("output");  // 当前已显示的有效文字
        page.findChild<QPushButton*>("send")->click();
        const auto baseline = output->toPlainText().size();  // 仅用户输入和展示标签的长度
        QTRY_VERIFY_WITH_TIMEOUT(output->toPlainText().size() > baseline + 5, 20000);
        page.findChild<QPushButton*>("stop")->click();
        QTRY_COMPARE_WITH_TIMEOUT(page.findChild<QLabel*>("status")->text(),
                                  QStringLiteral("Cancelled"), 5000);
        QVERIFY(output->toPlainText().size() > baseline + 5);
    }
};
QTEST_MAIN(LiveGuiTest)
#include "gui_acceptance.moc"
