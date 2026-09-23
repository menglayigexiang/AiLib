#pragma once

#include <LibMcp/McpResult.h>

#include <QFuture>
#include <QJsonObject>
#include <QObject>

namespace LibMcp {

using McpTransportRequestId = QString;  // 仅在一次传输请求内路由响应，不表示协议 Session

// McpServer 使用的请求级消息传输抽象，不保存协议 Session。
class LIBMCP_EXPORT McpServerTransport : public QObject
{
    Q_OBJECT
public:
    using QObject::QObject;                              // 复用 QObject 构造方式
    ~McpServerTransport() override = default;           // 释放具体传输资源

    virtual QFuture<McpResult<void>> start() = 0;        // 启动传输并准备接收请求
    virtual QFuture<McpResult<void>> stop() = 0;         // 停止传输并释放连接资源
    virtual QFuture<McpResult<void>> sendMessage(
        const McpTransportRequestId& requestId,  // 需要接收响应的传输请求标识
        const QJsonObject& message) = 0;         // 向指定请求发送完整 JSON-RPC 消息

signals:
    void messageReceived(
        const LibMcp::McpTransportRequestId& requestId,  // 当前消息所属的传输请求标识
        const QJsonObject& message);                     // 收到完整 JSON-RPC 消息
    void requestClosed(
        const LibMcp::McpTransportRequestId& requestId);  // 通知上层当前传输请求已关闭
};

} // namespace LibMcp
