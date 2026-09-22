#include "MainWindow.h"
#include "LogWidget.h"
#include <QApplication>
#include <QCommandLineParser>

int main(int argc, char* argv[])  // 使用命令行参数创建 GUI 环境，SDK 不依赖 Widgets
{
    QApplication application(argc, argv);  // 应用事件循环
    auto* logWidget = new LogWidget;       // 尽早截获启动日志并交由主窗口管理生命周期
    QCommandLineParser parser;             // GUI 启动配置，不接受命令行凭据
    parser.addHelpOption();
    parser.addOption({"provider", "deepseek / kimi / openai", "provider", "deepseek"});
    parser.process(application);
    const QString provider = parser.value("provider");  // 初始服务配置
    if (provider != "deepseek" && provider != "kimi" && provider != "openai")
        parser.showHelp(2);
    QCoreApplication::setApplicationName(QStringLiteral("TestApp"));
    QCoreApplication::setApplicationVersion(QStringLiteral("0.1.0"));

    MainWindow window(provider, logWidget);  // 统一承载测试页面与进程日志区域
    window.show();
    return application.exec();
}
