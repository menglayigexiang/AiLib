#include "MainWindow.h"

#include "LibAiCore/LibAiCorePage.h"
#include "LogWidget.h"

#if defined(AILIB_TESTAPP_HAS_MCP)
#include "LibMcp/LibMcpPage.h"
#endif

#include <QSplitter>
#include <QTabWidget>

MainWindow::MainWindow(
    const QString& initialProvider,  // LibAiCore 页面的初始 Provider 标识
    LogWidget* logWidget,            // 已开始截获进程日志的底部日志控件
    QWidget* parent)                 // 创建统一测试窗口，parent 为可选父控件
    : QMainWindow(parent)
{
    setWindowTitle(QStringLiteral("AiLib TestApp"));
    resize(900, 640);

    auto* splitter = new QSplitter(Qt::Vertical, this);  // 分隔测试页面和统一日志区域
    auto* tabs = new QTabWidget(splitter);                // 承载各库独立维护的人工测试页面
    tabs->addTab(
        new LibAiCorePage(initialProvider, tabs),
        QStringLiteral("LibAiCore"));

#if defined(AILIB_TESTAPP_HAS_MCP)
    tabs->addTab(new LibMcpPage(tabs), QStringLiteral("LibMcp"));
#endif

    splitter->addWidget(tabs);
    splitter->addWidget(logWidget);
    splitter->setStretchFactor(0, 4);
    splitter->setStretchFactor(1, 1);
    splitter->setSizes({512, 128});
    setCentralWidget(splitter);
}
