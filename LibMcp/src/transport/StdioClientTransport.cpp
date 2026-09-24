#include <LibMcp/StdioTransport.h>

#include "../core/FutureUtils_p.h"

#include <QJsonDocument>
#include <QProcess>

namespace LibMcp {
namespace {

constexpr qsizetype maximumStdioFrameBytes = 16 * 1024 * 1024;  // STDIO 单行协议帧的最大字节数

} // namespace

// 保存 STDIO Client 的子进程、配置和 stdout framing 缓冲。
class StdioClientTransportPrivate
{
public:
    StdioClientTransport* owner = nullptr;  // 接收解析消息和诊断事件的 Transport
    StdioClientConfig config;               // 子进程启动配置
    QProcess process;                       // MCP Server 子进程
    QByteArray outputBuffer;                // 尚未完成逐行解析的 stdout 字节
    bool stopping = false;                  // 当前进程退出是否由 stop 或析构主动发起

    void consumeOutput()  // 解析 stdout 中所有完整 JSON 行
    {
        outputBuffer += process.readAllStandardOutput();
        if (outputBuffer.size() > maximumStdioFrameBytes
            && !outputBuffer.contains('\n')) {
            outputBuffer.clear();
            emit owner->disconnected(
                {McpErrorCode::InvalidMessage,
                 QStringLiteral("STDIO stdout 协议帧超过 16 MiB 安全上限")});
            process.kill();
            return;
        }
        while (true) {
            const qsizetype lineEnd = outputBuffer.indexOf('\n');  // 当前完整消息的行结束位置
            if (lineEnd < 0) {
                return;
            }
            const QByteArray line = outputBuffer.left(lineEnd).trimmed();  // 当前完整协议行
            outputBuffer.remove(0, lineEnd + 1);
            if (line.size() > maximumStdioFrameBytes) {
                emit owner->disconnected(
                    {McpErrorCode::InvalidMessage,
                     QStringLiteral("STDIO stdout 协议帧超过 16 MiB 安全上限")});
                process.kill();
                return;
            }
            if (line.isEmpty()) {
                continue;
            }
            const QJsonDocument document = QJsonDocument::fromJson(line);  // 解析一条 JSON-RPC 消息
            if (!document.isObject()) {
                emit owner->disconnected(
                    {McpErrorCode::InvalidMessage,
                     QStringLiteral("STDIO stdout 包含非 JSON 对象行")});
                process.kill();
                return;
            }
            emit owner->messageReceived(document.object());
        }
    }
};

StdioClientTransport::StdioClientTransport(
    StdioClientConfig config,  // 子进程启动与环境配置
    QObject* parent)           // 可选 QObject 所有者
    : McpClientTransport(parent)
    , d(std::make_unique<StdioClientTransportPrivate>())
{  // 创建尚未启动的 STDIO Client Transport
    d->owner = this;
    d->config = std::move(config);
    connect(&d->process,
            &QProcess::readyReadStandardOutput,
            this,
            [this] { d->consumeOutput(); });
    connect(&d->process,
            &QProcess::readyReadStandardError,
            this,
            [this] {  // 转发 stderr，但不把它当作协议消息
                emit standardErrorReceived(
                    QString::fromUtf8(d->process.readAllStandardError()));
            });
    connect(&d->process,
            qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
            this,
            [this](int exitCode, QProcess::ExitStatus status) {  // 报告非主动停止的子进程退出
                const bool expectedExit = d->stopping;  // 区分用户主动停止与 Server 自行退出
                d->stopping = false;
                if (!expectedExit) {
                    const QString reason =  // 保留退出码与崩溃状态供诊断
                        status == QProcess::CrashExit
                            ? QStringLiteral("STDIO Server 已崩溃，状态码 %1").arg(exitCode)
                            : QStringLiteral("STDIO Server 已退出，状态码 %1").arg(exitCode);
                    emit disconnected({McpErrorCode::ConnectionClosed, reason});
                }
            });
}

StdioClientTransport::~StdioClientTransport()  // 停止子进程并释放管道资源
{
    if (d->process.state() != QProcess::NotRunning) {
        d->stopping = true;
        d->process.kill();
        d->process.waitForFinished(1000);
    }
}

QFuture<McpResult<void>> StdioClientTransport::start()  // 启动 Server 子进程
{
    if (d->process.state() != QProcess::NotRunning) {
        return Internal::readyFuture(McpResult<void>::success());
    }
    d->stopping = false;
    if (d->config.command.trimmed().isEmpty()) {
        return Internal::readyFuture(McpResult<void>::failure(
            {McpErrorCode::TransportError,
             QStringLiteral("STDIO 启动命令为空")}));
    }

    QProcessEnvironment environment = d->config.environment;  // 汇总显式和按名继承的环境变量
    const QProcessEnvironment systemEnvironment =  // 读取当前进程环境用于白名单传递
        QProcessEnvironment::systemEnvironment();
    for (const QString& name : d->config.inheritedEnvironmentNames) {
        if (systemEnvironment.contains(name)) {
            environment.insert(name, systemEnvironment.value(name));
        }
    }
    d->process.setProcessEnvironment(environment);
    if (!d->config.workingDirectory.isEmpty()) {
        d->process.setWorkingDirectory(d->config.workingDirectory);
    }
    d->process.start(d->config.command, d->config.arguments);
    if (!d->process.waitForStarted(5000)) {
        return Internal::readyFuture(McpResult<void>::failure(
            {McpErrorCode::TransportError, d->process.errorString()}));
    }
    return Internal::readyFuture(McpResult<void>::success());
}

QFuture<McpResult<void>> StdioClientTransport::stop()  // 终止 Server 子进程
{
    if (d->process.state() == QProcess::NotRunning) {
        return Internal::readyFuture(McpResult<void>::success());
    }
    d->stopping = true;
    d->process.terminate();
    if (!d->process.waitForFinished(2000)) {
        d->process.kill();
        d->process.waitForFinished(1000);
    }
    d->outputBuffer.clear();
    return Internal::readyFuture(McpResult<void>::success());
}

QFuture<McpResult<void>> StdioClientTransport::sendMessage(
    const QJsonObject& message)  // 向子进程 stdin 写入单行 JSON
{
    if (d->process.state() != QProcess::Running) {
        return Internal::readyFuture(McpResult<void>::failure(
            {McpErrorCode::ConnectionClosed,
             QStringLiteral("STDIO Server 未运行")}));
    }
    const QByteArray bytes =  // 编码不带内部换行的单行 JSON-RPC 消息
        QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n';
    if (d->process.write(bytes) != bytes.size()) {
        return Internal::readyFuture(McpResult<void>::failure(
            {McpErrorCode::TransportError,
             QStringLiteral("写入 STDIO Server 失败")}));
    }
    return Internal::readyFuture(McpResult<void>::success());
}

} // namespace LibMcp
