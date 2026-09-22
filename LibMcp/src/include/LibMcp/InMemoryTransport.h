#pragma once

#include <LibMcp/McpClientTransport.h>
#include <LibMcp/McpServerTransport.h>

#include <memory>
#include <utility>

namespace LibMcp {

class InMemoryServerTransport;

/// 用于自动测试的 Client 端内存 Transport。
class LIBMCP_EXPORT InMemoryClientTransport final : public McpClientTransport
{
    Q_OBJECT
public:
    explicit InMemoryClientTransport(QObject *parent = nullptr);
    QFuture<McpResult<void>> start() override;
    QFuture<McpResult<void>> stop() override;
    QFuture<McpResult<void>> sendMessage(const QJsonObject &message) override;
    void connectPeer(InMemoryServerTransport *peer);

private:
    InMemoryServerTransport *m_peer = nullptr; ///< 不拥有的配对端。
    bool m_running = false;                    ///< 是否接受消息。
};

/// 用于自动测试的 Server 端内存 Transport。
class LIBMCP_EXPORT InMemoryServerTransport final : public McpServerTransport
{
    Q_OBJECT
public:
    explicit InMemoryServerTransport(QObject *parent = nullptr);
    QFuture<McpResult<void>> start() override;
    QFuture<McpResult<void>> stop() override;
    QFuture<McpResult<void>> sendMessage(
        const McpSessionId &sessionId, const QJsonObject &message) override;
    void connectPeer(InMemoryClientTransport *peer);

private:
    InMemoryClientTransport *m_peer = nullptr; ///< 不拥有的配对端。
    bool m_running = false;                    ///< 是否接受消息。
};

LIBMCP_EXPORT std::pair<std::unique_ptr<InMemoryClientTransport>,
                       std::unique_ptr<InMemoryServerTransport>>
createInMemoryTransportPair();

} // namespace LibMcp
