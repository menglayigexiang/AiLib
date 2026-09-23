#include <LibMcp/McpClient.h>
#include <LibMcp/StreamableHttpTransport.h>

#include <QCoreApplication>
#include <QEventLoop>
#include <QJsonObject>
#include <QTimer>
#include <QUrl>

using namespace LibMcp;

template<typename T>
bool waitForOperation(
    const QSharedPointer<McpOperation<T>>& operation,  // 需要等待的官方 SDK 互操操作
    int timeoutMilliseconds = 10000)                  // 最长等待时间，单位毫秒
{                                                     // 在受控事件循环中等待操作结束
    if (!operation || operation->isFinished()) {
        return operation
               && operation->status() == McpOperationBase::Status::Succeeded;
    }
    QEventLoop loop;  // 处理网络回调直到操作完成或超时
    QTimer timer;     // 防止互操错误导致测试永久等待
    timer.setSingleShot(true);
    QObject::connect(operation.data(), &McpOperationBase::finished,
                     &loop, &QEventLoop::quit);
    QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    timer.start(timeoutMilliseconds);
    loop.exec();
    return operation->status() == McpOperationBase::Status::Succeeded;
}

int main(int argc, char* argv[])  // 使用 LibMcp Client 验证官方 SDK HTTP Server 互操
{
    QCoreApplication application(argc, argv);  // 驱动 Qt Network 事件循环
    const QUrl endpoint(application.arguments().value(1));  // 读取外部启动的官方 SDK 端点
    McpClient client(  // 创建只使用 2026-07-28 无状态请求的 Client
        std::make_unique<StreamableHttpClientTransport>(endpoint),
        {QStringLiteral("libmcp-interop-client"), QStringLiteral("1.0.0")});
    const auto start = client.start();  // 启动 HTTP Transport，不发送旧版握手
    if (!waitForOperation(start)) {
        return 2;
    }
    const auto discover = client.discover();  // 验证官方 Server 的现代协议发现
    if (!waitForOperation(discover)
        || !discover->result()->supportedVersions.contains(
            QStringLiteral(LIBMCP_PROTOCOL_VERSION))) {
        return 3;
    }
    const auto tools = client.listTools();  // 验证官方 Server 的工具列表
    if (!waitForOperation(tools) || tools->result()->isEmpty()) {
        return 4;
    }
    const QString toolName = tools->result()->front().name;  // 读取官方测试 Server 公布的工具名
    const auto call = client.callTool(  // 验证结构化工具调用往返
        toolName,
        QJsonObject{{QStringLiteral("source"), QStringLiteral("libmcp")}});
    if (!waitForOperation(call) || call->result()->content.isEmpty()) {
        return 5;
    }
    const auto stop = client.stop();  // 正常关闭 HTTP Transport
    return waitForOperation(stop) ? 0 : 6;
}
