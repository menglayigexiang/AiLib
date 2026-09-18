#include "DemoWindow.h"
#include <QApplication>
#include <QCommandLineParser>

int main(int argc, char* argv[])  // 使用命令行参数创建 GUI 环境，SDK 不依赖 Widgets
{
    QApplication application(argc, argv);  // 应用事件循环
    QCommandLineParser parser;             // GUI 启动配置，不接受命令行凭据
    parser.addHelpOption();
    parser.addOption({"provider", "offline / deepseek / kimi", "provider", "offline"});
    parser.process(application);
    const QString provider = parser.value("provider");  // 初始服务配置
    if (provider != "offline" && provider != "deepseek" && provider != "kimi")
        parser.showHelp(2);
    DemoWindow window(provider);  // 应用拥有的工具、确认策略和工作线程
    window.show();
    return application.exec();
}
