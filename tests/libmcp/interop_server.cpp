#include <LibMcp/McpServer.h>
#include <LibMcp/StreamableHttpTransport.h>

#include <QCoreApplication>
#include <QFutureWatcher>
#include <QHostAddress>
#include <QJsonObject>
#include <QTextStream>

using namespace LibMcp;

int main(int argc, char* argv[])  // 启动供官方 SDK 互操测试使用的最小 HTTP Server
{
    QCoreApplication application(argc, argv);  // 驱动 Qt Network 事件循环
    const quint16 port = application.arguments().value(1).toUShort();  // 读取外部分配的测试端口
    auto transport = std::make_unique<StreamableHttpServerTransport>(  // 创建无状态 HTTP 传输
        QHostAddress::LocalHost,
        port,
        QStringLiteral("/mcp"));
    McpServer server(  // 创建只公布回显工具的互操服务
        std::move(transport),
        {QStringLiteral("libmcp-interop"),
         QStringLiteral("1.0.0"),
         QStringLiteral("LibMcp Interop")});
    McpTool tool;  // 描述官方 SDK 可列出并调用的回显工具
    tool.name = QStringLiteral("echo");
    tool.inputSchema = {{QStringLiteral("type"), QStringLiteral("object")}};
    server.addTool(
        tool,
        [](const McpToolCallRequest& request,
           const McpRequestContext&) {  // 原样返回官方 SDK 提交的结构化参数
            McpToolCallResult result;  // 保存符合现代协议的完整结果
            result.content = QJsonArray{QJsonObject{
                {QStringLiteral("type"), QStringLiteral("text")},
                {QStringLiteral("text"), QStringLiteral("echo")}}};
            result.structuredContent = request.arguments;
            return result;
        });

    QFutureWatcher<McpResult<void>> watcher;  // 监视服务端监听启动结果
    QObject::connect(
        &watcher,
        &QFutureWatcher<McpResult<void>>::finished,
        &application,
        [&application, &watcher] {  // 输出可机器识别的就绪或失败标记
            const McpResult<void> result = watcher.result();  // 读取最终监听结果
            QTextStream stream(result.isSuccess() ? stdout : stderr);  // 选择就绪或错误输出通道
            stream << (result.isSuccess() ? QStringLiteral("READY\n")
                                          : result.error().message + QLatin1Char('\n'));
            stream.flush();
            if (result.isError()) {
                application.exit(1);
            }
        });
    watcher.setFuture(server.start());
    return application.exec();
}
