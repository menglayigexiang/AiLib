#pragma once

#include <LibMcp/McpResult.h>

#include <QFuture>
#include <QJsonObject>
#include <QObject>

namespace LibMcp {

/// McpClient 使用的消息传输抽象。
class LIBMCP_EXPORT McpClientTransport : public QObject
{
    Q_OBJECT
public:
    using QObject::QObject;
    ~McpClientTransport() override = default;

    /// 启动传输，Future 成功表示可以发送消息。
    virtual QFuture<McpResult<void>> start() = 0;
    /// 停止传输并清理连接资源。
    virtual QFuture<McpResult<void>> stop() = 0;
    /// 发送一个完整 JSON-RPC 对象。
    virtual QFuture<McpResult<void>> sendMessage(const QJsonObject &message) = 0;

signals:
    void messageReceived(const QJsonObject &message);
    void disconnected(const LibMcp::McpError &error);
};

} // namespace LibMcp
