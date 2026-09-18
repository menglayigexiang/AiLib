#include "../../TestApp/src/DemoWindow.h"
#include <QtTest>
#include <QMessageBox>
#include <QTimer>
#include <QProcess>

// 验证应用层确认桥接、工具闭环、Stop 和窗口生命周期，默认不连接外部服务。
class DemoTest final : public QObject {
    Q_OBJECT
private slots:
    void approval_data()  // 验证允许和拒绝均能继续由模型完成回答
    {
        QTest::addColumn<bool>("allow");
        QTest::newRow("allow") << true;
        QTest::newRow("deny") << false;
    }
    void approval()  // UI 线程操作真实非模态弹窗，工作线程同步等待
    {
        QFETCH(bool, allow);  // 当前确认决定
        DemoWindow window;    // 应用拥有的运行依赖
        window.show();
        auto* send = window.findChild<QPushButton*>("send");         // 启动按钮
        auto* status = window.findChild<QLabel*>("status");          // 最终停止原因
        auto* output = window.findChild<QPlainTextEdit*>("output");  // 模型输出及工具状态
        send->click();
        QTRY_VERIFY_WITH_TIMEOUT(!window.findChildren<QMessageBox*>().isEmpty(), 3000);
        auto* box = window.findChildren<QMessageBox*>().first();  // 当前待确认调用
        QVERIFY(!box->isModal());
        QVERIFY(box->text().contains("widgets-agent"));
        QVERIFY(box->text().contains("call_demo"));
        box->button(allow ? QMessageBox::Yes : QMessageBox::No)->click();
        QTRY_COMPARE_WITH_TIMEOUT(status->text(), QStringLiteral("Completed"), 5000);
        QVERIFY(output->toPlainText().contains(allow ? QStringLiteral("42")
                                                     : QStringLiteral("ApprovalDenied")));
        QVERIFY(output->toPlainText().contains(allow ? QStringLiteral("Handler=已执行")
                                                     : QStringLiteral("Handler=未执行")));
        QVERIFY(send->isEnabled());
    }
    void cancelDuringStream()  // 输出尚未完整时 Stop，UI 不阻塞且不进入工具执行
    {
        DemoWindow window;  // 当前测试窗口
        window.findChild<QPushButton*>("send")->click();
        window.findChild<QPushButton*>("stop")->click();
        QTRY_COMPARE_WITH_TIMEOUT(window.findChild<QLabel*>("status")->text(),
                                  QStringLiteral("Cancelled"), 3000);
        QVERIFY(!window.findChild<QPlainTextEdit*>("output")->toPlainText().contains(
            QStringLiteral("工具开始")));
    }
    void cancelAfterText()  // 已显示有效文字后取消，部分文字继续保留在界面
    {
        DemoWindow window;  // 当前测试窗口
        window.findChild<QCheckBox*>("tools")->setChecked(false);
        auto* output = window.findChild<QPlainTextEdit*>("output");  // 已显示的流式文字
        window.findChild<QPushButton*>("send")->click();
        QTRY_VERIFY_WITH_TIMEOUT(output->toPlainText().contains(QStringLiteral("你好")), 2000);
        window.findChild<QPushButton*>("stop")->click();
        QTRY_COMPARE_WITH_TIMEOUT(window.findChild<QLabel*>("status")->text(), QStringLiteral("Cancelled"), 3000);
        QVERIFY(output->toPlainText().contains(QStringLiteral("你好")));
    }
    void cliApproval_data()  // 覆盖普通与流式 Agent 的允许、拒绝、取消及 EOF
    {
        QTest::addColumn<bool>("stream");
        QTest::addColumn<QByteArray>("answer");
        QTest::addColumn<QByteArray>("expected");
        QTest::newRow("stream-allow") << true << QByteArray("1\n") << QByteArray("42");
        QTest::newRow("stream-deny") << true << QByteArray("2\n") << QByteArray("ApprovalDenied");
        QTest::newRow("stream-cancel") << true << QByteArray("3\n") << QByteArray("Cancelled");
        QTest::newRow("plain-allow") << false << QByteArray("1\n") << QByteArray("42");
        QTest::newRow("plain-deny") << false << QByteArray("2\n") << QByteArray("ApprovalDenied");
        QTest::newRow("plain-cancel") << false << QByteArray("3\n") << QByteArray("Cancelled");
        QTest::newRow("eof-denies") << true << QByteArray() << QByteArray("ApprovalDenied");
    }
    void cliApproval()  // 启动真实 CLI 程序，将测试输入写入同步确认接口
    {
        QFETCH(bool, stream);                      // 当前流式开关
        QFETCH(QByteArray, answer);                // 标准输入的用户决定
        QFETCH(QByteArray, expected);              // 预期结束或业务结果
        QProcess process;                          // 隔离运行的 CLI 示例
        QStringList arguments{"--mode", "agent"};  // 离线 Agent 模式
        if (!stream)
            arguments.append("--no-stream");
        process.start(QString::fromUtf8(AILIB_CLI_PATH), arguments);
        QVERIFY(process.waitForStarted(2000));
        process.write(answer);
        process.closeWriteChannel();
        QVERIFY(process.waitForFinished(5000));
        QCOMPARE(process.exitStatus(), QProcess::NormalExit);
        QCOMPARE(process.exitCode(), 0);
        QVERIFY(process.readAllStandardOutput().contains(expected));
    }
    void cancelDuringApproval()  // 等待确认时 Stop 同步唤醒运行，不强制终止线程
    {
        DemoWindow window;  // 当前测试窗口
        window.show();
        window.findChild<QPushButton*>("send")->click();
        QTRY_VERIFY_WITH_TIMEOUT(!window.findChildren<QMessageBox*>().isEmpty(), 3000);
        window.findChild<QPushButton*>("stop")->click();
        QTRY_COMPARE_WITH_TIMEOUT(window.findChild<QLabel*>("status")->text(),
                                  QStringLiteral("Cancelled"), 3000);
        QTRY_VERIFY_WITH_TIMEOUT(window.findChildren<QMessageBox*>().isEmpty(), 1000);
    }
    void closeDuringApproval()  // 关闭窗口先取消，运行依赖在工作线程结束之后销毁
    {
        DemoWindow window;  // 当前测试窗口
        window.show();
        window.findChild<QPushButton*>("send")->click();
        QTRY_VERIFY_WITH_TIMEOUT(!window.findChildren<QMessageBox*>().isEmpty(), 3000);
        window.close();
        QTRY_VERIFY_WITH_TIMEOUT(!window.isVisible(), 3000);
        QCOMPARE(window.findChild<QLabel*>("status")->text(), QStringLiteral("Cancelled"));
    }
    void deleteDuringRun()  // 不再处理 UI 队列也能协作退出，避免析构等待死锁
    {
        auto* window = new DemoWindow;  // 人工控制生命周期的应用窗口
        window->findChild<QPushButton*>("send")->click();
        delete window;
    }
};
QTEST_MAIN(DemoTest)
#include "tst_Demo.moc"
