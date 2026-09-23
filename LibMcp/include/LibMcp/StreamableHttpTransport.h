#pragma once

#include <LibMcp/McpClientTransport.h>
#include <LibMcp/McpServerTransport.h>

#include <QHostAddress>
#include <QUrl>

#include <memory>

namespace LibMcp {

// 隐藏 Client Transport 的 HTTP facade 和请求状态。
class StreamableHttpClientTransportPrivate;
// 隐藏 Server Transport 的 HTTP facade 和 SSE 路由状态。
class StreamableHttpServerTransportPrivate;

/// 基于 HTTP POST 的 Streamable HTTP Client Transport。
class LIBMCP_EXPORT StreamableHttpClientTransport final : public McpClientTransport
{
    Q_OBJECT
public:
    explicit StreamableHttpClientTransport(
        QUrl endpoint,                 // MCP HTTP Endpoint
        QJsonObject headers = {},      // 需要附加的固定 HTTP Headers
        QObject *parent = nullptr);    // 可选 QObject 所有者
    ~StreamableHttpClientTransport() override;  // 终止活动 HTTP 请求并释放 facade
    QFuture<McpResult<void>> start() override;  // 校验 Endpoint 并允许发送
    QFuture<McpResult<void>> stop() override;   // 停止并中止活动 HTTP 请求
    QFuture<McpResult<void>> sendMessage(const QJsonObject &message) override;  // 使用独立 POST 发送 MCP 消息

private:
    std::unique_ptr<StreamableHttpClientTransportPrivate> d;  // Client Transport 私有实现
};

/// 可配置监听地址、端口和路径的 Streamable HTTP Server Transport。
class LIBMCP_EXPORT StreamableHttpServerTransport final : public McpServerTransport
{
    Q_OBJECT
public:
    explicit StreamableHttpServerTransport(
        QHostAddress address = QHostAddress::LocalHost,  // 默认仅绑定回环地址
        quint16 port = 8080,                            // 监听端口，零表示自动分配
        QString path = QStringLiteral("/mcp"),         // 唯一 MCP POST 路径
        QObject *parent = nullptr);                     // 可选 QObject 所有者
    ~StreamableHttpServerTransport() override;          // 关闭监听和全部请求流
    QFuture<McpResult<void>> start() override;          // 启动 HTTP 监听
    QFuture<McpResult<void>> stop() override;           // 停止 HTTP 监听并关闭连接
    QFuture<McpResult<void>> sendMessage(
        const McpTransportRequestId& requestId,  // 需要接收响应的 HTTP 请求标识
        const QJsonObject& message) override;    // 向指定 HTTP 请求写入 JSON 响应
    QHostAddress address() const;                       // 返回实际或配置的监听地址
    quint16 port() const;                               // 返回实际或配置的监听端口
    QString path() const;                               // 返回标准化 MCP Endpoint 路径

private:
    std::unique_ptr<StreamableHttpServerTransportPrivate> d;  // Server Transport 私有实现
};

} // namespace LibMcp
