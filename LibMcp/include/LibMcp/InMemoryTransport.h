#pragma once

#include <LibMcp/McpClientTransport.h>
#include <LibMcp/McpServerTransport.h>

#include <QPointer>

#include <memory>
#include <utility>

namespace LibMcp {

// 前向声明与 Client 内存传输配对的 Server 端。
class InMemoryServerTransport;

/// 用于自动测试的 Client 端内存 Transport。
class LIBMCP_EXPORT InMemoryClientTransport final : public McpClientTransport
{
    Q_OBJECT
public:
    explicit InMemoryClientTransport(QObject *parent = nullptr);  // 创建尚未配对的 Client 内存传输
    QFuture<McpResult<void>> start() override;                     // 允许向配对端发送消息
    QFuture<McpResult<void>> stop() override;                      // 停止接受和发送消息
    QFuture<McpResult<void>> sendMessage(const QJsonObject &message) override;  // 异步投递一个 Client 消息
    void connectPeer(InMemoryServerTransport *peer);               // 绑定不拥有的 Server 配对端

private:
    QPointer<InMemoryServerTransport> m_peer;  // 自动感知销毁的非拥有配对端
    bool m_running = false;                    // 是否接受消息
};

/// 用于自动测试的 Server 端内存 Transport。
class LIBMCP_EXPORT InMemoryServerTransport final : public McpServerTransport
{
    Q_OBJECT
public:
    explicit InMemoryServerTransport(QObject *parent = nullptr);  // 创建尚未配对的 Server 内存传输
    QFuture<McpResult<void>> start() override;                     // 允许向配对端发送消息
    QFuture<McpResult<void>> stop() override;                      // 停止接受和发送消息
    QFuture<McpResult<void>> sendMessage(
        const McpTransportRequestId& requestId,  // 需要接收响应的内存请求标识
        const QJsonObject& message) override;    // 向配对 Client 发送完整消息
    void connectPeer(InMemoryClientTransport *peer);              // 绑定不拥有的 Client 配对端

private:
    QPointer<InMemoryClientTransport> m_peer;  // 自动感知销毁的非拥有配对端
    bool m_running = false;                    // 是否接受消息
};

LIBMCP_EXPORT std::pair<std::unique_ptr<InMemoryClientTransport>,
                       std::unique_ptr<InMemoryServerTransport>>
createInMemoryTransportPair();  // 创建已相互绑定的 Client/Server 内存传输

} // namespace LibMcp
