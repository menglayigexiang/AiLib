#include <LibMcp/InMemoryTransport.h>

#include "../core/FutureUtils_p.h"

#include <QMetaObject>

namespace LibMcp {

using Internal::readyFuture;

InMemoryClientTransport::InMemoryClientTransport(QObject *parent)
    : McpClientTransport(parent)
{
}

void InMemoryClientTransport::connectPeer(InMemoryServerTransport *peer)
{
    m_peer = peer;
}

QFuture<McpResult<void>> InMemoryClientTransport::start()
{
    m_running = true;
    return readyFuture(McpResult<void>::success());
}

QFuture<McpResult<void>> InMemoryClientTransport::stop()
{
    m_running = false;
    return readyFuture(McpResult<void>::success());
}

QFuture<McpResult<void>> InMemoryClientTransport::sendMessage(
    const QJsonObject &message)
{
    if (!m_running || !m_peer) {
        return readyFuture(McpResult<void>::failure(
            {McpErrorCode::ConnectionClosed,
             QStringLiteral("In-memory transport is not connected")}));
    }

    const QPointer<InMemoryServerTransport> peer = m_peer;  // 防止排队投递期间访问已销毁的 Server
    QMetaObject::invokeMethod(
        peer.data(),
        [peer, message] {
            if (peer) {
                emit peer->messageReceived(QStringLiteral("in-memory"), message);
            }
        },
        Qt::QueuedConnection);
    return readyFuture(McpResult<void>::success());
}

InMemoryServerTransport::InMemoryServerTransport(QObject *parent)
    : McpServerTransport(parent)
{
}

void InMemoryServerTransport::connectPeer(InMemoryClientTransport *peer)
{
    m_peer = peer;
}

QFuture<McpResult<void>> InMemoryServerTransport::start()
{
    m_running = true;
    return readyFuture(McpResult<void>::success());
}

QFuture<McpResult<void>> InMemoryServerTransport::stop()
{
    m_running = false;
    return readyFuture(McpResult<void>::success());
}

QFuture<McpResult<void>> InMemoryServerTransport::sendMessage(
    const McpTransportRequestId&,  // 内存传输只有一个配对端，无需使用请求标识
    const QJsonObject& message)    // 需要发送给配对 Client 的完整消息
{
    if (!m_running || !m_peer) {
        return readyFuture(McpResult<void>::failure(
            {McpErrorCode::ConnectionClosed,
             QStringLiteral("In-memory transport is not connected")}));
    }

    const QPointer<InMemoryClientTransport> peer = m_peer;  // 防止排队投递期间访问已销毁的 Client
    QMetaObject::invokeMethod(
        peer.data(),
        [peer, message] {  // 仅在配对端仍存活时投递消息
            if (peer) {
                emit peer->messageReceived(message);
            }
        },
        Qt::QueuedConnection);
    return readyFuture(McpResult<void>::success());
}

std::pair<std::unique_ptr<InMemoryClientTransport>,
          std::unique_ptr<InMemoryServerTransport>>
createInMemoryTransportPair()
{
    auto client = std::make_unique<InMemoryClientTransport>();
    auto server = std::make_unique<InMemoryServerTransport>();
    client->connectPeer(server.get());
    server->connectPeer(client.get());
    return {std::move(client), std::move(server)};
}

} // namespace LibMcp
