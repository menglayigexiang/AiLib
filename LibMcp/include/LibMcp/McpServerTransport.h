#pragma once

#include <LibMcp/McpResult.h>

#include <QFuture>
#include <QJsonObject>
#include <QObject>

namespace LibMcp {

using McpSessionId = QString;

/// McpServer 使用的多 Session 消息传输抽象。
class LIBMCP_EXPORT McpServerTransport : public QObject
{
    Q_OBJECT
public:
    using QObject::QObject;
    ~McpServerTransport() override = default;

    virtual QFuture<McpResult<void>> start() = 0;
    virtual QFuture<McpResult<void>> stop() = 0;
    virtual QFuture<McpResult<void>> sendMessage(
        const McpSessionId &sessionId, const QJsonObject &message) = 0;

signals:
    void messageReceived(const LibMcp::McpSessionId &sessionId,
                         const QJsonObject &message);
    void sessionClosed(const LibMcp::McpSessionId &sessionId);
};

} // namespace LibMcp
