#include <LibMcp/StdioTransport.h>

#include "../core/FutureUtils_p.h"

#include <QFile>
#include <QJsonDocument>
#include <QSocketNotifier>

#include <cstdio>

namespace LibMcp {

// 保存 STDIO Server 的标准流、事件通知器和逐行 framing 缓冲。
class StdioServerTransportPrivate
{
public:
    StdioServerTransport* owner = nullptr;          // 接收解析消息的 Transport
    QFile input;                                    // 当前进程 stdin
    QFile output;                                   // 当前进程 stdout
    std::unique_ptr<QSocketNotifier> notifier;      // stdin 可读事件通知器
    QByteArray inputBuffer;                         // 尚未完成逐行解析的输入字节
    quint64 nextRequestId = 1;                      // 生成 Transport 内部请求路由标识

    void consumeInput()  // 读取 stdin 并解析全部完整 JSON 行
    {
        inputBuffer += input.readAll();
        while (true) {
            const qsizetype lineEnd = inputBuffer.indexOf('\n');  // 当前完整消息的行结束位置
            if (lineEnd < 0) {
                return;
            }
            const QByteArray line = inputBuffer.left(lineEnd).trimmed();  // 当前完整协议行
            inputBuffer.remove(0, lineEnd + 1);
            if (line.isEmpty()) {
                continue;
            }
            const QJsonDocument document = QJsonDocument::fromJson(line);  // 解析一条 JSON-RPC 消息
            if (!document.isObject()) {
                continue;
            }
            const McpTransportRequestId requestId =  // 生成仅用于本次响应路由的标识
                QString::number(nextRequestId++);
            emit owner->messageReceived(requestId, document.object());
        }
    }
};

StdioServerTransport::StdioServerTransport(QObject* parent)  // 绑定当前进程标准流
    : McpServerTransport(parent)
    , d(std::make_unique<StdioServerTransportPrivate>())
{
    d->owner = this;
}

StdioServerTransport::~StdioServerTransport() = default;  // 停止监听但不关闭系统标准流

QFuture<McpResult<void>> StdioServerTransport::start()  // 开始监听 stdin
{
    if (d->notifier) {
        return Internal::readyFuture(McpResult<void>::success());
    }
    if (!d->input.open(stdin, QIODevice::ReadOnly)
        || !d->output.open(stdout, QIODevice::WriteOnly)) {
        return Internal::readyFuture(McpResult<void>::failure(
            {McpErrorCode::TransportError,
             QStringLiteral("无法打开进程标准输入或标准输出")}));
    }
    d->notifier = std::make_unique<QSocketNotifier>(
        fileno(stdin),
        QSocketNotifier::Read,
        this);
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    connect(d->notifier.get(),
            &QSocketNotifier::activated,
            this,
            [this](QSocketDescriptor, QSocketNotifier::Type) {  // Qt 6 下 stdin 可读时解析协议消息
                d->consumeInput();
            });
#else
    connect(d->notifier.get(),
            QOverload<int>::of(&QSocketNotifier::activated),
            this,
            [this](int) {  // Qt 5 下 stdin 可读时解析协议消息
                d->consumeInput();
            });
#endif
    return Internal::readyFuture(McpResult<void>::success());
}

QFuture<McpResult<void>> StdioServerTransport::stop()  // 停止监听 stdin
{
    d->notifier.reset();
    d->inputBuffer.clear();
    d->input.close();
    d->output.close();
    return Internal::readyFuture(McpResult<void>::success());
}

QFuture<McpResult<void>> StdioServerTransport::sendMessage(
    const McpTransportRequestId&,  // STDIO 顺序写入 stdout，不需要显式路由标识
    const QJsonObject& message)    // 向 stdout 写入单行 JSON
{
    if (!d->output.isOpen()) {
        return Internal::readyFuture(McpResult<void>::failure(
            {McpErrorCode::ConnectionClosed,
             QStringLiteral("STDIO Server 尚未启动")}));
    }
    const QByteArray bytes =  // 编码不带内部换行的单行 JSON-RPC 消息
        QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n';
    if (d->output.write(bytes) != bytes.size() || !d->output.flush()) {
        return Internal::readyFuture(McpResult<void>::failure(
            {McpErrorCode::TransportError,
             QStringLiteral("写入 STDIO stdout 失败")}));
    }
    return Internal::readyFuture(McpResult<void>::success());
}

} // namespace LibMcp
