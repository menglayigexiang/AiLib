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
    using QObject::QObject;                            // 复用 QObject 构造方式
    ~McpClientTransport() override = default;          // 释放具体 Client 传输

    /// 启动传输，Future 成功表示可以发送消息。
    virtual QFuture<McpResult<void>> start() = 0;      // 启动传输并准备发送
    /// 停止传输并清理连接资源。
    virtual QFuture<McpResult<void>> stop() = 0;       // 停止传输并终止活动请求
    /// 发送一个完整 JSON-RPC 对象。
    virtual QFuture<McpResult<void>> sendMessage(const QJsonObject &message) = 0;  // 发送完整 JSON-RPC 消息

signals:
    void messageReceived(const QJsonObject &message);       // 通知 Client 收到完整 JSON-RPC 消息
    void disconnected(const LibMcp::McpError &error);       // 通知 Client 传输已异常断开
};

} // namespace LibMcp
